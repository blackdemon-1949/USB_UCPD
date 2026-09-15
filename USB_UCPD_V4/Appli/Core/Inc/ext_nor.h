/**
  ******************************************************************************
  * @file    ext_nor.h
  * @brief   Raw access to the XSPI1 NOR (PY25Q64HA / W25Q compatible, 8 MB).
  *
  * The application executes in place (XiP) from this same device, so EVERY
  * erase/program operation is run by a small routine that lives in the
  * executable, non-cacheable RAM window (RAMFUNC @ 0x24070000, see the linker
  * script and MPU region 6) with interrupts masked: while the XSPI is out of
  * memory-mapped mode no instruction fetch, constant read or stack access may
  * touch 0x90000000.  The public API in this file (which lives in the NOR)
  * prepares a descriptor in RAM, copies payloads into the non-cacheable
  * scratch buffer and then calls the RAM routine.
  *
  * Safety rules enforced here:
  *   - only the reserved storage window (top 1 MB of the device) is writable;
  *     everything below it is the running application image and is refused;
  *   - the device answers a JEDEC ID read before any write is attempted;
  *   - every operation has a DWT-cycle timeout and restores memory-mapped mode
  *     on every exit path, success or failure;
  *   - after the first failure the driver latches "not ready" and refuses
  *     further writes until the next reset (failsafe: a bad flash can never
  *     wedge the main loop twice).
  ******************************************************************************
  */
#ifndef EXT_NOR_H
#define EXT_NOR_H

#include <stdint.h>

/* Device geometry ---------------------------------------------------------- */
#define EXT_NOR_BASE        0x90000000UL          /* XSPI1 memory-mapped window */
#define EXT_NOR_SIZE        0x00800000UL          /* 8 MB                       */
#define EXT_NOR_SECTOR_SIZE 0x00001000UL          /* 4 KB sector erase          */
#define EXT_NOR_PAGE_SIZE   0x00000100UL          /* 256 B page program         */

/* Reserved storage window: the last 1 MB of the device.  The application
 * image is ~300 KB at offset 0, so this cannot collide with it (checked at
 * run time against the linker's _etext as well). */
#define EXT_NOR_STORE_OFF   0x00700000UL
#define EXT_NOR_STORE_SIZE  0x00100000UL
#define EXT_NOR_STORE_SECTORS (EXT_NOR_STORE_SIZE / EXT_NOR_SECTOR_SIZE)  /* 256 */

/* Status codes ------------------------------------------------------------- */
#define EXT_NOR_OK          0
#define EXT_NOR_ERR_PARAM   (-1)
#define EXT_NOR_ERR_OFF     (-2)   /* outside the reserved window            */
#define EXT_NOR_ERR_TIMEOUT (-3)
#define EXT_NOR_ERR_NOTRDY  (-4)
#define EXT_NOR_ERR_VERIFY  (-5)

/* One-time setup: latch DWT, read the JEDEC ID, check the reserved window is
 * clear of the application.  Returns 1 when the driver is usable. */
uint8_t  EXT_NOR_Init(void);

uint8_t  EXT_NOR_Ready(void);        /* 1 = writes are allowed              */
uint8_t  EXT_NOR_Present(void);      /* 1 = a device answered the ID read   */
uint32_t EXT_NOR_JedecId(void);      /* 3-byte ID, 0 if unknown             */
uint32_t EXT_NOR_Errors(void);       /* latched error count                 */
uint32_t EXT_NOR_Writes(void);       /* successful erase+program operations */

void     EXT_NOR_SetEnabled(uint8_t on);  /* master switch (CLI 'store off') */

/* Data access.  'off' is an absolute offset inside the 8 MB device. */
int EXT_NOR_Read(uint32_t off, void *buf, uint32_t len);
int EXT_NOR_Program(uint32_t off, const void *buf, uint32_t len);  /* page-safe */
int EXT_NOR_EraseSector(uint32_t off);

/* Reads back the reserved window's first sector and compares it with a known
 * pattern - "is this flash actually writable right now?" self test. */
uint32_t EXT_NOR_SelfTest(void);

const char *EXT_NOR_Status(void);    /* one-line human state for the console */

#endif /* EXT_NOR_H */
