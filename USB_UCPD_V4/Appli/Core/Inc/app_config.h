/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   Build-time feature switches for the application.
  *
  * Everything that can be turned off here stays in the project (so an
  * STM32CubeIDE build keeps working unchanged) but compiles to nothing when
  * its switch is 0.  Defaults are chosen so the firmware is usable on a bare
  * board with no SD card, no I2C peripherals and no LSE crystal.
  ******************************************************************************
  */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ------------------------------------------------------------------ optional
 * SD card / FatFs.  OFF by default: the card socket is not used any more, the
 * external NOR flash is the persistent store.  The sources stay in the project
 * (app_sd.c, sdmmc.c, apie_sdlog.c and the FATFS folder) but compile to empty
 * objects, so setting this to 1 restores the whole SD stack without touching
 * anything else.  In the GNU Makefile build the FatFs, SD driver and SDMMC HAL
 * sources are additionally left out of the link entirely while this is 0.
 */
#ifndef APP_SD_ENABLED
#define APP_SD_ENABLED        0
#endif

/* External NOR (XSPI1) persistent store + owner command macros. */
#ifndef APP_STORE_ENABLED
#define APP_STORE_ENABLED     1
#endif

/* Backup-SRAM "CMOS" (settings + reset/fault log) and the watchdog. */
#ifndef APP_CMOS_ENABLED
#define APP_CMOS_ENABLED      1
#endif
#ifndef APP_WDT_ENABLED
#define APP_WDT_ENABLED       1
#endif

/* ------------------------------------------------------------------- safety
 * A peripheral that fails to initialise must never stop the console or the
 * USB-PD engine: the init functions log the failure, record it in the CMOS and
 * return.  Only a genuinely unrecoverable faulting CPU resets the board (and
 * that reset is explained by the CMOS and bounded by safe mode).
 */
#define APP_INIT_FAIL_IS_FATAL   0

#endif /* APP_CONFIG_H */
