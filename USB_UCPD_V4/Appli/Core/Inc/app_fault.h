/**
  ******************************************************************************
  * @file    app_fault.h
  * @brief   One place for "something in an initialisation failed" and
  *          "something in the CPU faulted".
  *
  * Rationale (the bug this file exists to kill):
  *   Every CubeMX-generated init used to end in Error_Handler(), and
  *   Error_Handler() was `__disable_irq(); while(1)` blinking the LED.  An
  *   optional peripheral that is simply absent or slow - the DTS has no LSE
  *   crystal to run from, an I2C device does not answer, the USB 3.3 V detector
  *   never becomes ready - therefore killed the whole board *before* USB CDC
  *   and USB-PD were up: no console, no PD, in any condition, until the user
  *   power-cycled it by hand.
  *
  *   APP_INIT_Fail()       - log it, drop a breadcrumb in the backup-SRAM CMOS,
  *                           carry on.  The subsystem disables itself.
  *   APP_FATAL()           - genuinely unrecoverable: breadcrumb + reset, never
  *                           an infinite loop; after APP_WDT_SAFE_LIMIT such
  *                           boots the firmware comes up in safe mode.
  ******************************************************************************
  */
#ifndef APP_FAULT_H
#define APP_FAULT_H

#include <stdint.h>
#include "app_cmos.h"   /* APP_FAULT_* breadcrumb codes */

/* Called as soon as the console exists (after APP_LOG_Init). */
void APP_FAULT_Init(void);

/** Record a non-fatal initialisation failure.  `tag` is a short ASCII label
 *  (up to 4 characters are kept), `status` the HAL/return status. */
void APP_INIT_Fail(const char *tag, int status);

/** Record a fatal error and reset the board (does not return). */
void APP_FATAL(uint8_t code) __attribute__((noreturn));

/** Fault handlers call this: breadcrumb only, no reset (the handler resets). */
void APP_FAULT_Mark(uint8_t code, uint32_t extra, uint32_t pc);

/** Pack up to four ASCII characters into the 32-bit breadcrumb payload. */
uint32_t APP_FAULT_Tag(const char *tag);

/** Number of non-fatal init failures seen this boot. */
uint16_t APP_FAULT_InitFailCount(void);

/* ---- external-flash ("RAM mode") safety ----------------------------------
   Programming the XSPI NOR means leaving memory-mapped mode, and while that
   window is open the CPU can only fetch instructions from ITCM - the whole
   application text lives in the flash that is being talked to.  The code path
   is kept tight and interrupt-free (see ext_nor.c), but if anything does go
   wrong in there it must be identifiable: APP_FAULT_FlashBegin()/End() bracket
   the window so the fault handlers can record APP_FAULT_RAMFUNC instead of a
   meaningless HardFault, and the next boot can then stay away from the flash
   until the operator asks for it. */
void    APP_FAULT_FlashBegin(void);
void    APP_FAULT_FlashEnd(void);
uint8_t APP_FAULT_FlashBusy(void);
uint8_t APP_FAULT_LastWasFlashRisk(void);   /* last recorded fault was RAMFUNC */

#endif /* APP_FAULT_H */
