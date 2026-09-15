/**
  ******************************************************************************
  * @file    app_cmos.c
  * @brief   Backup-SRAM settings store - see app_cmos.h.
  *
  * The window is the linker's .bkp_ram section: (NOLOAD), so the reset handler
  * never copies or zeroes it, and it stays non-cacheable through MPU region 5.
  * This module owns the first 1 KB; apie_bkp.c owns the rest (its window is
  * shifted by APP_CMOS_SIZE).
  ******************************************************************************
  */
#include "app_cmos.h"
#include "app_log.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

/* The window is addressed by its architectural base (BKPSRAM_BASE): the backup
 * SRAM is a fixed 4 KB block that startup never loads or clears, so no linker
 * section is needed - and adding one would risk the retained image.  apie_bkp.c
 * owns everything above APP_CMOS_FREE_OFF. */
#define APP_CMOS_END (APP_CMOS_BASE + APP_CMOS_SIZE)

#define CMOS_MEM   ((volatile uint8_t *)(uintptr_t)APP_CMOS_BASE)
/* apie_bkp.h uses the same window, starting at APP_CMOS_FREE_OFF. */

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t seq;
  uint32_t len;
  uint32_t crc;
} cmos_hdr_t;

typedef struct
{
  uint32_t code;
  uint32_t extra;
  uint32_t boot;
  uint32_t tick;
} cmos_fault_t;

static uint8_t  s_cmos_inited;   /* set once the CMOS was read at boot */
static app_settings_t s_settings;
static uint8_t  s_valid;
static uint32_t s_seq;
static uint8_t  s_active_slot;   /* 0 = A, 1 = B: the slot to write next */

/* -------------------------------------------------------------------------- */
/*  CRC-32 (same polynomial as apie_bkp: kept local so both areas are         */
/*  self-describing and a fault in one can never mask the other)              */
/* -------------------------------------------------------------------------- */
static uint32_t cmos_crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= data[i];
    for (uint32_t b = 0U; b < 8U; b++)
    {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1U)));
    }
  }
  return ~crc;
}

/* -------------------------------------------------------------------------- */
/*  Defaults                                                                   */
/* -------------------------------------------------------------------------- */
static void cmos_defaults(app_settings_t *s)
{
  memset(s, 0, sizeof(*s));
  s->schema      = APP_CMOS_VERSION;
  s->flags       = APP_SET_AUTO_REQUEST | APP_SET_REMEMBER | APP_SET_STORE_ENABLED |
                   APP_SET_MONITOR | APP_SET_PREFER_PPS | APP_SET_FALLBACK;
  s->target_mv   = 9000U;    /* a safe, universally available level         */
  s->target_ma   = 3000U;
  s->max_mv      = 21000U;   /* SPR ceiling: EPR needs the opt-in flag      */
  s->max_ma      = 5000U;
  s->min_mv      = 5000U;
  s->ovp_mv      = 0U;       /* monitor thresholds off until INA226 is live */
  s->ocp_ma      = 0U;
  s->uv_mv       = 4500U;
  s->cmd_mask    = 0xFFFFFFFFUL;
  s->retry       = 3U;
  memcpy(s->owner, "owner", 6U);
}

/* -------------------------------------------------------------------------- */
/*  Slot access                                                                */
/* -------------------------------------------------------------------------- */
static uint32_t cmos_slot_off(uint8_t slot)
{
  return (uint32_t)slot * APP_CMOS_SLOT_SIZE;
}

static uint8_t cmos_slot_load(uint8_t slot, app_settings_t *out, uint32_t *seq)
{
  const uint8_t *base = (const uint8_t *)CMOS_MEM + cmos_slot_off(slot);
  const cmos_hdr_t *h = (const cmos_hdr_t *)base;

  if ((h->magic != APP_CMOS_MAGIC) || (h->version != APP_CMOS_VERSION) ||
      (h->len != sizeof(app_settings_t)) ||
      ((sizeof(cmos_hdr_t) + h->len) > APP_CMOS_SLOT_SIZE))
  {
    return 0U;
  }
  memcpy(out, base + sizeof(cmos_hdr_t), sizeof(app_settings_t));
  if (cmos_crc32((const uint8_t *)out, sizeof(app_settings_t)) != h->crc)
  {
    return 0U;
  }
  *seq = h->seq;
  return 1U;
}

static void cmos_slot_store(uint8_t slot, const app_settings_t *s, uint32_t seq)
{
  uint8_t *base = (uint8_t *)CMOS_MEM + cmos_slot_off(slot);
  cmos_hdr_t h;

  h.magic   = APP_CMOS_MAGIC;
  h.version = APP_CMOS_VERSION;
  h.seq     = seq;
  h.len     = (uint32_t)sizeof(app_settings_t);
  h.crc     = cmos_crc32((const uint8_t *)s, sizeof(app_settings_t));

  /* Payload first, header last: a reset in the middle leaves the old header in
   * place with a CRC that no longer matches its payload, so the slot is simply
   * rejected instead of being half-valid. */
  memcpy(base + sizeof(cmos_hdr_t), s, sizeof(app_settings_t));
  memcpy(base, &h, sizeof(h));
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */
void APP_CMOS_Init(void)
{
  app_settings_t a, b;
  uint32_t sa = 0U, sb = 0U;
  uint8_t oka, okb;

  /* PWR clock + backup domain write access + BKPRAM clock.  Without DBP the
   * writes below are silently dropped, which is also why a board with no VBAT
   * or no DBP ends up with factory defaults - the intended behaviour. */
#ifdef __HAL_RCC_PWR_CLK_ENABLE
  __HAL_RCC_PWR_CLK_ENABLE();
#endif
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_BKPRAM_CLK_ENABLE();

  /* Make sure the linker window really is where we think it is. */
  if ((APP_CMOS_BASE + APP_CMOS_FREE_OFF) > APP_CMOS_END)
  {
    cmos_defaults(&s_settings);
    s_valid = 0U;
    s_seq = 0U;
    APP_LOG_Write("[cmos] backup SRAM window mismatch - using defaults\r\n");
    return;
  }

  oka = cmos_slot_load(0U, &a, &sa);
  okb = cmos_slot_load(1U, &b, &sb);

  if (oka && (!okb || (sa >= sb)))
  {
    s_settings = a;
    s_seq = sa;
    s_active_slot = 1U;
    s_valid = 1U;
  }
  else if (okb)
  {
    s_settings = b;
    s_seq = sb;
    s_active_slot = 0U;
    s_valid = 1U;
  }
  else
  {
    cmos_defaults(&s_settings);
    s_seq = 0U;
    s_active_slot = 0U;
    s_valid = 0U;

    /* Fresh battery / first boot: the fault ring is erased state (0xFF..),
       which is *not* a valid fault code.  Clear it before the first breadcrumb
       of this boot is written, otherwise the console would print a pile of
       bogus "boot 4294967295" rows on every battery change. */
    memset((void *)(CMOS_MEM + APP_CMOS_LOG_OFF), 0, APP_CMOS_LOG_SIZE);
    s_settings.abnormal_boots = 0U;
    s_settings.last_fault     = APP_FAULT_NONE;
    s_settings.fault_count    = 0U;
  }

  s_settings.schema = APP_CMOS_VERSION;
  s_settings.boot_count++;

  APP_LOG_Printf("[cmos] %s, seq=%lu, owner=%s, target=%lumV/%lumA\r\n",
                 s_valid ? "settings restored" : "FACTORY DEFAULTS (cleared battery or first boot)",
                 (unsigned long)s_seq, s_settings.owner,
                 (unsigned long)s_settings.target_mv,
                 (unsigned long)s_settings.target_ma);

  APP_CMOS_Save();
  s_cmos_inited = 1U;
}

app_settings_t *APP_CMOS(void)      { return &s_settings; }
uint8_t         APP_CMOS_Valid(void){ return s_valid; }
uint32_t        APP_CMOS_Seq(void)  { return s_seq; }

void APP_CMOS_Save(void)
{
  HAL_PWR_EnableBkUpAccess();
  s_seq++;
  cmos_slot_store(s_active_slot, &s_settings, s_seq);
  s_active_slot ^= 1U;     /* next write goes to the mirror */
  s_valid = 1U;
}

void APP_CMOS_FactoryReset(void)
{
  cmos_defaults(&s_settings);
  s_settings.boot_count = 0U;
  memset((void *)CMOS_MEM, 0, APP_CMOS_SIZE);   /* drop both slots + log too */
  s_seq = 0U;
  s_active_slot = 0U;
  s_valid = 0U;
  APP_CMOS_Save();
}

void APP_CMOS_Print(void)
{
  app_settings_t *s = &s_settings;

  APP_LOG_Printf("cmos: %s seq=%lu boots=%lu owner=%s\r\n",
                 s_valid ? "valid" : "defaults",
                 (unsigned long)s_seq, (unsigned long)s->boot_count, s->owner);
  APP_LOG_Printf("      target=%lumV/%lumA  caps=%lu-%lumV %lumA  retry=%u\r\n",
                 (unsigned long)s->target_mv, (unsigned long)s->target_ma,
                 (unsigned long)s->min_mv, (unsigned long)s->max_mv,
                 (unsigned long)s->max_ma, (unsigned)s->retry);
  APP_LOG_Printf("      monitor ovp=%lumV ocp=%lumA uv=%lumV %s\r\n",
                 (unsigned long)s->ovp_mv, (unsigned long)s->ocp_ma,
                 (unsigned long)s->uv_mv,
                 (s->flags & APP_SET_MONITOR) ? "on" : "off");
  APP_LOG_Printf("      flags=0x%04X cmds=%u cmd_mask=0x%08lX store_gen=%lu\r\n",
                 (unsigned)s->flags, (unsigned)s->cmd_count,
                 (unsigned long)s->cmd_mask, (unsigned long)s->store_gen);
}

/* -------------------------------------------------------------------------- */
/*  Fault / reset breadcrumbs                                                  */
/* -------------------------------------------------------------------------- */
void APP_CMOS_LogFault(uint32_t code, uint32_t extra)
{
  if (s_cmos_inited == 0U)
  {
    return;     /* called from an early init path, before the CMOS was read */
  }
  volatile uint8_t *log = CMOS_MEM + APP_CMOS_LOG_OFF;
  uint32_t idx = 0U;
  uint32_t oldest = 0xFFFFFFFFUL;
  cmos_fault_t f;

  /* Round-robin over 8 entries: overwrite the one with the smallest boot
   * number, so the newest faults always survive. */
  for (uint32_t i = 0U; i < (APP_CMOS_LOG_SIZE / sizeof(cmos_fault_t)); i++)
  {
    const cmos_fault_t *e = (const cmos_fault_t *)(log + (i * sizeof(cmos_fault_t)));
    if (e->boot < oldest)
    {
      oldest = e->boot;
      idx = i;
    }
  }

  f.code  = code;
  f.extra = extra;
  f.boot  = s_settings.boot_count;
  f.tick  = HAL_GetTick();
  memcpy((void *)(log + (idx * sizeof(cmos_fault_t))), &f, sizeof(f));

  s_settings.last_fault = code;
  s_settings.abnormal_boots++;
}

/* Written from a fault handler: only register writes and stores, no calls into
 * anything that could fault again. */
void APP_CMOS_FaultMark(uint32_t code, uint32_t extra, uint32_t pc)
{
  volatile uint32_t *m = (volatile uint32_t *)(CMOS_MEM + APP_CMOS_FAULT_OFF);

  PWR->CR1 |= PWR_CR1_DBP;          /* backup domain write access */
  m[0] = APP_CMOS_FAULT_MAGIC;
  m[1] = code;
  m[2] = extra;
  m[3] = pc;
  __DSB();
}

uint32_t APP_CMOS_TakeFaultMark(uint32_t *extra)
{
  volatile uint32_t *m = (volatile uint32_t *)(CMOS_MEM + APP_CMOS_FAULT_OFF);
  uint32_t code;

  if (m[0] != APP_CMOS_FAULT_MAGIC)
  {
    return APP_FAULT_NONE;
  }
  code = m[1];
  if (extra != NULL)
  {
    *extra = m[2];
  }
  APP_CMOS_LogFault(code, m[2]);
  s_settings.fault_count++;
  APP_CMOS_Save();
  m[0] = 0U;                        /* consume the marker */
  __DSB();
  return code;
}

uint32_t APP_CMOS_LastFault(void)
{
  const uint8_t *log = (const uint8_t *)CMOS_MEM + APP_CMOS_LOG_OFF;
  uint32_t newest_boot = 0U;
  uint32_t code = APP_FAULT_NONE;

  for (uint32_t i = 0U; i < (APP_CMOS_LOG_SIZE / sizeof(cmos_fault_t)); i++)
  {
    const cmos_fault_t *e = (const cmos_fault_t *)(log + (i * sizeof(cmos_fault_t)));
    if ((e->code != 0U) && (e->boot >= newest_boot))
    {
      newest_boot = e->boot;
      code = e->code;
    }
  }
  return code;
}

uint32_t APP_CMOS_FaultCount(void)
{
  return s_settings.abnormal_boots;
}

void APP_CMOS_ClearFaults(void)
{
  memset((void *)(CMOS_MEM + APP_CMOS_LOG_OFF), 0, APP_CMOS_LOG_SIZE);
  s_settings.last_fault = APP_FAULT_NONE;
  s_settings.abnormal_boots = 0U;
  s_settings.fault_count = 0U;
  APP_CMOS_Save();
}

void APP_CMOS_PrintFaults(void)
{
  const uint8_t *log = (const uint8_t *)CMOS_MEM + APP_CMOS_LOG_OFF;
  /* Index == APP_FAULT_* in app_cmos.h; keep the two in step. */
  static const char *names[] = { "none", "hard", "memmanage", "bus", "usage",
                                 "watchdog", "stack", "error-handler",
                                 "init-fail", "nor-ramfunc" };
  uint32_t shown = 0U;

  APP_LOG_Printf("cmos: abnormal boots=%lu last=%lu\r\n",
                 (unsigned long)s_settings.abnormal_boots,
                 (unsigned long)s_settings.last_fault);
  for (uint32_t i = 0U; i < (APP_CMOS_LOG_SIZE / sizeof(cmos_fault_t)); i++)
  {
    const cmos_fault_t *e = (const cmos_fault_t *)(log + (i * sizeof(cmos_fault_t)));
    /* Valid codes are 1..(names-1).  0 is "erased slot" and 0xFFFFFFFF is
       untouched backup SRAM, neither of which is a fault worth printing. */
    if ((e->code >= 1U) && (e->code < (sizeof(names) / sizeof(names[0]))))
    {
      APP_LOG_Printf("      boot %lu: %s extra=0x%08lX tick=%lu\r\n",
                     (unsigned long)e->boot, names[e->code],
                     (unsigned long)e->extra, (unsigned long)e->tick);
      shown++;
    }
  }
  if (shown == 0U)
  {
    APP_LOG_Write("      (fault ring empty - the counters above are all we have)\r\n");
  }
}
