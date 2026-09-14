/**
  ******************************************************************************
  * @file    ext_nor.c
  * @brief   Raw XSPI1 NOR driver - see ext_nor.h for the safety rules.
  *
  * Two layers live here:
  *
  *   1. ext_nor_xspi_run() and every helper it calls are in the .ramfunc
  *      section, which the linker script places in ITCM (executable, never
  *      cached, and the only RAM the XiP switch-off cannot strand).  This is the
  *      only code allowed to run while the XSPI is out of memory-mapped mode,
  *      and it never calls out, never reads a constant table and never touches
  *      the D-cache, so nothing can fault on the missing XiP window.
  *
  *   2. The public API below stays in the NOR and only marshals data in and
  *      out of the non-cacheable scratch buffer for the RAM routine.
  *
  * The register sequences are the documented indirect-mode ones for this IP:
  * FMODE first (0 = indirect write, 1 = indirect read), then IR/CCR/TCR/DLR/AR,
  * then the data phase through DR using the FIFO level, then TCF.  Memory-mapped
  * mode is restored from a register snapshot on every exit path.
  ******************************************************************************
  */
#include "ext_nor.h"
#include "app_fault.h"
#include "main.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Timing                                                                     */
/* -------------------------------------------------------------------------- */
#define NOR_TMO_ABORT   (SystemCoreClock / 2U)     /* 500 ms to leave MM      */
#define NOR_TMO_STATUS  (SystemCoreClock * 2U)     /* 2 s  erase/program done */
#define NOR_TMO_XFER    (SystemCoreClock / 4U)     /* 250 ms data phase       */

/* -------------------------------------------------------------------------- */
/*  RAM-resident core                                                          */
/* -------------------------------------------------------------------------- */

/* Everything below runs with the XSPI out of memory-mapped mode. */
#define RAMF __attribute__((section(".ramfunc"), noinline, used))

/* Scratch window: non-cacheable, so the XSPI sees exactly what the CPU wrote
 * and the CPU sees exactly what the XSPI read - no cache maintenance at all. */
/* Scratch window: lives in the non-cacheable AXI window (RW_NONCACHEABLE,
 * NOLOAD - see the linker script), so the XSPI sees exactly what the CPU wrote
 * and the CPU sees exactly what the XSPI read, with no cache maintenance at
 * all.  It is deliberately NOT in .ramfunc: that section goes to ITCM for
 * execution, and the buffer must be reachable by the XSPI controller as a bus
 * master.  NOLOAD also keeps 2 KB out of the flash image. */
__attribute__((section(".nor_scratch"), used, aligned(32)))
volatile uint8_t EXT_NOR_SCRATCH[2048];

/* Descriptor filled in by the NOR-resident API before the RAM routine runs. */
typedef struct
{
  uint32_t op;        /* 0 = read, 1 = program page(s), 2 = erase sector,
                         3 = read JEDEC id, 4 = read status            */
  uint32_t off;       /* offset from EXT_NOR_BASE                      */
  uint32_t len;       /* byte count for op 0/1 (<= scratch size)       */
  uint32_t tmo;       /* cycle budget for the long phases              */
  uint32_t status;    /* out: 0 = ok                                   */
  uint32_t id;        /* out: JEDEC id for op 3                        */
} nor_job_t;

static nor_job_t s_job;   /* .bss: CPU-only data, never touched by the XSPI */

/* Register snapshot, kept in RAM while the XSPI is reconfigured. */
typedef struct
{
  uint32_t cr, dcr1, dcr2, dcr3, dcr4, tcr, ccr, ir, dlr, ar, abr, lptr;
} nor_xspi_regs_t;

RAMF static uint32_t nor_cycles(void)
{
  return DWT->CYCCNT;
}

RAMF static uint32_t nor_elapsed(uint32_t t0)
{
  return DWT->CYCCNT - t0;
}

RAMF static void nor_delay(uint32_t loops)
{
  while (loops != 0U)
  {
    loops--;
    __NOP();
  }
}

/* Save / restore the whole configuration the bootloader left behind: the
 * memory-mapped read command lives in CCR/TCR/IR, the device geometry in
 * DCR1..4, and CR.FMODE selects the functional mode. */
RAMF static void nor_save(nor_xspi_regs_t *r)
{
  r->cr   = XSPI1->CR;
  r->dcr1 = XSPI1->DCR1;
  r->dcr2 = XSPI1->DCR2;
  r->dcr3 = XSPI1->DCR3;
  r->dcr4 = XSPI1->DCR4;
  r->tcr  = XSPI1->TCR;
  r->ccr  = XSPI1->CCR;
  r->ir   = XSPI1->IR;
  r->dlr  = XSPI1->DLR;
  r->ar   = XSPI1->AR;
  r->abr  = XSPI1->ABR;
  r->lptr = XSPI1->LPTR;
}

RAMF static void nor_restore(const nor_xspi_regs_t *r)
{
  XSPI1->CR   = r->cr & ~XSPI_CR_ABORT;   /* no abort while restoring      */
  XSPI1->DCR1 = r->dcr1;
  XSPI1->DCR2 = r->dcr2;
  XSPI1->DCR3 = r->dcr3;
  XSPI1->DCR4 = r->dcr4;
  XSPI1->TCR  = r->tcr;
  XSPI1->CCR  = r->ccr;
  XSPI1->IR   = r->ir;
  XSPI1->DLR  = r->dlr;
  XSPI1->AR   = r->ar;
  XSPI1->ABR  = r->abr;
  XSPI1->LPTR = r->lptr;
  XSPI1->CR   = r->cr;                    /* memory-mapped mode + EN       */
}

/* Leave memory-mapped mode (FMODE -> indirect write). */
RAMF static uint32_t nor_leave_mm(void)
{
  uint32_t t0 = nor_cycles();

  XSPI1->CR |= XSPI_CR_ABORT;
  while ((XSPI1->CR & XSPI_CR_ABORT) != 0U)
  {
    if (nor_elapsed(t0) > NOR_TMO_ABORT)
    {
      return 1U;
    }
  }
  XSPI1->CR = (XSPI1->CR & ~(XSPI_CR_ABORT | XSPI_CR_FMODE));

  while ((XSPI1->SR & XSPI_SR_BUSY) != 0U)
  {
    if (nor_elapsed(t0) > NOR_TMO_ABORT)
    {
      return 2U;
    }
  }
  XSPI1->SR = XSPI_SR_TCF;   /* write-1-to-clear */
  return 0U;
}

RAMF static void nor_mode(uint32_t read)
{
  if (read != 0U)
  {
    XSPI1->CR = (XSPI1->CR & ~XSPI_CR_FMODE) | (1UL << XSPI_CR_FMODE_Pos);
  }
  else
  {
    XSPI1->CR = (XSPI1->CR & ~XSPI_CR_FMODE);
  }
}

/* Program one command: 1-line 8-bit instruction, 1-line 24-bit address,
 * 1-line data.  FMODE must already be set by nor_mode(). */
RAMF static void nor_cmd(uint32_t inst, uint32_t addr, uint32_t dlr, uint32_t data_phase, uint32_t dummy)
{
  XSPI1->IR  = inst;
  XSPI1->CCR = XSPI_CCR_IMODE_0 | XSPI_CCR_ISIZE | XSPI_CCR_ADMODE_0 |
               XSPI_CCR_ADSIZE_1 | (data_phase ? XSPI_CCR_DMODE_0 : 0U);
  XSPI1->TCR = (dummy & 0x1FU);
  XSPI1->DLR = dlr;
  XSPI1->AR  = addr;
}

RAMF static uint32_t nor_wait_tcf(uint32_t tmo)
{
  uint32_t t0 = nor_cycles();

  while ((XSPI1->SR & XSPI_SR_TCF) == 0U)
  {
    if (nor_elapsed(t0) > tmo)
    {
      return 1U;
    }
  }
  XSPI1->SR = XSPI_SR_TCF;
  return 0U;
}

RAMF static uint32_t nor_wait_fifo(uint32_t want_data, uint32_t tmo)
{
  uint32_t t0 = nor_cycles();

  for (;;)
  {
    uint32_t lvl = (XSPI1->SR & XSPI_SR_FLEVEL) >> XSPI_SR_FLEVEL_Pos;

    if (want_data != 0U)
    {
      if (lvl != 0U)
      {
        return 0U;
      }
    }
    else
    {
      if (lvl < 8U)
      {
        return 0U;
      }
    }
    if (nor_elapsed(t0) > tmo)
    {
      return 1U;
    }
  }
}

/* 0x05 read status register, 1 byte, no address. */
RAMF static uint32_t nor_read_status(uint8_t *st, uint32_t tmo)
{
  nor_mode(1U);
  nor_cmd(0x05U, 0U, 0U, 1U, 0U);
  if (nor_wait_fifo(1U, tmo) != 0U)
  {
    return 1U;
  }
  *st = *(volatile uint8_t *)&XSPI1->DR;
  if (nor_wait_tcf(tmo) != 0U)
  {
    return 2U;
  }
  nor_mode(0U);
  return 0U;
}

/* Poll until WIP (status bit 0) clears. */
RAMF static uint32_t nor_wait_ready(uint32_t tmo)
{
  uint32_t t0 = nor_cycles();

  for (;;)
  {
    uint8_t st = 0xFFU;

    if (nor_read_status(&st, tmo) != 0U)
    {
      return 1U;
    }
    if ((st & 0x01U) == 0U)
    {
      return 0U;
    }
    if (nor_elapsed(t0) > tmo)
    {
      return 2U;
    }
    nor_delay(200U);
  }
}

/* 0x06 write enable + confirm WEL (status bit 1). */
RAMF static uint32_t nor_write_enable(uint32_t tmo)
{
  uint8_t st = 0;

  nor_mode(0U);
  nor_cmd(0x06U, 0U, 0U, 0U, 0U);
  if (nor_wait_tcf(tmo) != 0U)
  {
    return 1U;
  }
  if (nor_read_status(&st, tmo) != 0U)
  {
    return 2U;
  }
  return ((st & 0x02U) != 0U) ? 0U : 3U;
}

/* 0x0B fast read: 1-line instruction/address/data, 8 dummy cycles. */
RAMF static uint32_t nor_read_nor(uint32_t addr, uint8_t *dst, uint32_t len, uint32_t tmo)
{
  uint32_t i;

  if (len == 0U)
  {
    return 0U;
  }
  nor_mode(1U);
  nor_cmd(0x0BU, addr, len - 1U, 1U, 8U);
  for (i = 0U; i < len; i++)
  {
    if (nor_wait_fifo(1U, tmo) != 0U)
    {
      return 1U;
    }
    dst[i] = *(volatile uint8_t *)&XSPI1->DR;
  }
  if (nor_wait_tcf(tmo) != 0U)
  {
    return 2U;
  }
  nor_mode(0U);
  return 0U;
}

/* 0x02 page program.  'len' must not cross a 256-byte page. */
RAMF static uint32_t nor_program_page(uint32_t addr, const uint8_t *src, uint32_t len, uint32_t tmo)
{
  uint32_t i;

  if (nor_write_enable(tmo) != 0U)
  {
    return 1U;
  }
  nor_mode(0U);
  nor_cmd(0x02U, addr, len - 1U, 1U, 0U);
  for (i = 0U; i < len; i++)
  {
    if (nor_wait_fifo(0U, tmo) != 0U)
    {
      return 2U;
    }
    *(volatile uint8_t *)&XSPI1->DR = src[i];
  }
  if (nor_wait_tcf(tmo) != 0U)
  {
    return 3U;
  }
  if (nor_wait_ready(tmo) != 0U)
  {
    return 4U;
  }
  return 0U;
}

/* 0x20 sector erase (4 KB). */
RAMF static uint32_t nor_erase_sector(uint32_t addr, uint32_t tmo)
{
  if (nor_write_enable(tmo) != 0U)
  {
    return 1U;
  }
  nor_mode(0U);
  nor_cmd(0x20U, addr, 0U, 0U, 0U);
  if (nor_wait_tcf(tmo) != 0U)
  {
    return 2U;
  }
  if (nor_wait_ready(tmo) != 0U)
  {
    return 3U;
  }
  return 0U;
}

/**
  * @brief  The only function allowed to run while the XiP window is gone.
  *         Reads its descriptor from s_job, restores memory-mapped mode before
  *         returning on every path.
  */
RAMF void ext_nor_xspi_run(void)
{
  nor_xspi_regs_t regs;
  uint32_t st = 0U;

  nor_save(&regs);

  if (nor_leave_mm() != 0U)
  {
    s_job.status = 0xE1U;
    nor_restore(&regs);
    return;
  }

  switch (s_job.op)
  {
    case 0U:  /* read */
      st = nor_read_nor(s_job.off, (uint8_t *)EXT_NOR_SCRATCH, s_job.len, s_job.tmo);
      break;

    case 1U:  /* program, split on page boundaries */
    {
      uint32_t written = 0U;
      while ((written < s_job.len) && (st == 0U))
      {
        uint32_t addr = s_job.off + written;
        uint32_t room = EXT_NOR_PAGE_SIZE - (addr % EXT_NOR_PAGE_SIZE);
        uint32_t n = s_job.len - written;

        if (n > room)
        {
          n = room;
        }
        st = nor_program_page(addr, (const uint8_t *)EXT_NOR_SCRATCH + written, n, s_job.tmo);
        written += n;
      }
      break;
    }

    case 2U:  /* erase sector */
      st = nor_erase_sector(s_job.off, s_job.tmo);
      break;

    case 3U:  /* JEDEC / device ID (0x9F) */
    {
      uint8_t id[4];
      nor_mode(1U);
      nor_cmd(0x9FU, 0U, 3U, 1U, 0U);
      for (uint32_t i = 0U; i < 4U; i++)
      {
        if (nor_wait_fifo(1U, s_job.tmo) != 0U)
        {
          st = 1U;
          break;
        }
        id[i] = *(volatile uint8_t *)&XSPI1->DR;
      }
      if (st == 0U)
      {
        if (nor_wait_tcf(s_job.tmo) != 0U)
        {
          st = 2U;
        }
        nor_mode(0U);
        s_job.id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
      }
      break;
    }

    case 4U:  /* status register */
    {
      uint8_t s = 0U;
      st = nor_read_status(&s, s_job.tmo);
      if (st == 0U)
      {
        s_job.id = s;
      }
      break;
    }

    default:
      st = 0xE2U;
      break;
  }

  nor_restore(&regs);
  s_job.status = st;
}

/* -------------------------------------------------------------------------- */
/*  Public API (runs from the NOR)                                             */
/* -------------------------------------------------------------------------- */

static uint8_t s_present;    /* a device answered 0x9F                     */
static uint8_t s_ready;      /* writes allowed (self test passed)          */
static uint8_t s_enabled = 1U; /* master switch from the CLI               */
static uint32_t s_jedec;
static uint32_t s_errors;
static uint32_t s_writes;

extern uint32_t _etext;      /* end of the linked text (in the NOR)        */

static int nor_run(uint32_t op, uint32_t off, uint32_t len, uint32_t tmo)
{
  if (s_ready == 0U)
  {
    return EXT_NOR_ERR_NOTRDY;
  }
  if (off + len > EXT_NOR_SIZE)
  {
    return EXT_NOR_ERR_PARAM;
  }
  /* Only the reserved window may be written. */
  if ((op != 0U) && ((off < EXT_NOR_STORE_OFF) ||
                     ((off + len) > (EXT_NOR_STORE_OFF + EXT_NOR_STORE_SIZE))))
  {
    s_errors++;
    return EXT_NOR_ERR_OFF;
  }
  if (len > sizeof(EXT_NOR_SCRATCH))
  {
    return EXT_NOR_ERR_PARAM;
  }

  __disable_irq();
  s_job.op = op;
  s_job.off = off;
  s_job.len = len;
  s_job.tmo = tmo;
  s_job.status = 0xEEU;
  s_job.id = 0U;
  /* From here until FlashEnd() the XiP window is (or is about to be) off and
     the only executable memory is ITCM.  The flag lets the fault handlers
     record where the trouble came from, see app_fault.c. */
  APP_FAULT_FlashBegin();
  ext_nor_xspi_run();            /* RAM-resident: no NOR access from here   */
  APP_FAULT_FlashEnd();
  __enable_irq();

  if (s_job.status != 0U)
  {
    s_errors++;
    s_ready = 0U;                /* latch off: one failure disables writes  */
    return EXT_NOR_ERR_TIMEOUT;
  }
  return EXT_NOR_OK;
}

uint8_t EXT_NOR_Init(void)
{
  uint32_t id = 0U;

  /* DWT cycle counter: the only timebase available with interrupts masked. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* The application must be able to run entirely from the NOR below the
   * reserved window, otherwise the store would erase code. */
  if (((uint32_t)&_etext) >= (EXT_NOR_BASE + EXT_NOR_STORE_OFF))
  {
    return 0U;
  }

  /* Memory-mapped mode must be the current functional mode (the bootloader
   * leaves it that way); anything else means we must not touch the device. */
  if ((XSPI1->CR & XSPI_CR_FMODE) != (3UL << XSPI_CR_FMODE_Pos))
  {
    return 0U;
  }

  s_ready = 1U;                  /* temporarily allow the ID read            */
  if (nor_run(3U, 0U, 0U, NOR_TMO_XFER) == EXT_NOR_OK)
  {
    id = s_job.id;
  }

  if ((id == 0U) || (id == 0xFFFFFFU) || ((id & 0xFFU) == 0U))
  {
    s_ready = 0U;
    s_present = 0U;
    return 0U;
  }

  s_present = 1U;
  s_jedec = id;

  /* Self test: erase + program + read back the last sector of the window.
   * A failure keeps the rest of the firmware running, it only disables the
   * persistent store. */
  if (EXT_NOR_SelfTest() == 0U)
  {
    s_ready = 0U;
    return 0U;
  }
  return 1U;
}

uint32_t EXT_NOR_SelfTest(void)
{
  const uint32_t off = EXT_NOR_STORE_OFF + EXT_NOR_STORE_SIZE - EXT_NOR_SECTOR_SIZE;
  uint8_t *scratch = (uint8_t *)EXT_NOR_SCRATCH;
  uint32_t i;

  if ((s_present == 0U) || (s_ready == 0U))
  {
    return 0U;
  }

  for (i = 0U; i < 32U; i++)
  {
    scratch[i] = (uint8_t)(0xA5U ^ (uint8_t)i);
  }

  if (nor_run(2U, off, 0U, NOR_TMO_STATUS) != EXT_NOR_OK)     /* erase     */
  {
    return 0U;
  }
  if (nor_run(1U, off, 32U, NOR_TMO_STATUS) != EXT_NOR_OK)    /* program   */
  {
    return 0U;
  }

  memset(scratch, 0, 32U);
  if (nor_run(0U, off, 32U, NOR_TMO_XFER) != EXT_NOR_OK)      /* read back */
  {
    return 0U;
  }
  for (i = 0U; i < 32U; i++)
  {
    if (scratch[i] != (uint8_t)(0xA5U ^ (uint8_t)i))
    {
      s_ready = 0U;
      return 0U;
    }
  }
  s_writes++;
  return 1U;
}

uint8_t  EXT_NOR_Ready(void)   { return (uint8_t)(s_ready && s_enabled); }
uint8_t  EXT_NOR_Present(void) { return s_present; }
uint32_t EXT_NOR_JedecId(void) { return s_jedec; }
uint32_t EXT_NOR_Errors(void)  { return s_errors; }
uint32_t EXT_NOR_Writes(void)  { return s_writes; }

void EXT_NOR_SetEnabled(uint8_t on)
{
  s_enabled = (on != 0U) ? 1U : 0U;
}

int EXT_NOR_Read(uint32_t off, void *buf, uint32_t len)
{
  if ((buf == NULL) || (len == 0U))
  {
    return EXT_NOR_ERR_PARAM;
  }
  if ((off < EXT_NOR_STORE_OFF) || ((off + len) > (EXT_NOR_STORE_OFF + EXT_NOR_STORE_SIZE)))
  {
    return EXT_NOR_ERR_OFF;         /* the store only reads its own window */
  }
  if (len > sizeof(EXT_NOR_SCRATCH))
  {
    return EXT_NOR_ERR_PARAM;
  }
  if (s_present == 0U)
  {
    return EXT_NOR_ERR_NOTRDY;
  }

  __disable_irq();
  s_job.op = 0U;
  s_job.off = off;
  s_job.len = len;
  s_job.tmo = NOR_TMO_XFER;
  s_job.status = 0xEEU;
  ext_nor_xspi_run();
  __enable_irq();

  if (s_job.status != 0U)
  {
    s_errors++;
    return EXT_NOR_ERR_TIMEOUT;
  }
  memcpy(buf, (const void *)EXT_NOR_SCRATCH, len);
  return EXT_NOR_OK;
}

int EXT_NOR_Program(uint32_t off, const void *buf, uint32_t len)
{
  if ((buf == NULL) || (len == 0U) || (len > sizeof(EXT_NOR_SCRATCH)))
  {
    return EXT_NOR_ERR_PARAM;
  }
  memcpy((void *)EXT_NOR_SCRATCH, buf, len);
  return nor_run(1U, off, len, NOR_TMO_STATUS);
}

int EXT_NOR_EraseSector(uint32_t off)
{
  if ((off % EXT_NOR_SECTOR_SIZE) != 0U)
  {
    return EXT_NOR_ERR_PARAM;
  }
  return nor_run(2U, off, 0U, NOR_TMO_STATUS);
}

const char *EXT_NOR_Status(void)
{
  if (s_present == 0U)
  {
    return "ext-nor: no device on XSPI1";
  }
  if (s_ready == 0U)
  {
    return "ext-nor: present but writes are latched off after an error";
  }
  if (s_enabled == 0U)
  {
    return "ext-nor: present, writes disabled by 'store off'";
  }
  return "ext-nor: ready";
}

/* -------------------------------------------------------------------------- */
/*  FIFO helper used by the counters above (kept here so the RAM section does  */
/*  not grow): nothing to do, the definitions live with the code.              */
/* -------------------------------------------------------------------------- */
