/**
  ******************************************************************************
  * @file    app_wdt.h
  * @brief   IWDG watchdog, reset-cause analysis and the degraded "safe mode".
  *
  * Failsafe chain:
  *   - the independent watchdog is started once the console is alive and is
  *     kicked only from the main loop, so any hang (in the app, in the PD
  *     stack or in the NOR writer) ends in a clean reset instead of a dead
  *     board that has to be power-cycled by hand;
  *   - every reset cause and every fault is written into the backup-SRAM log
  *     (app_cmos.c) before the watchdog is started;
  *   - after APP_WDT_SAFE_LIMIT abnormal boots in a row the firmware comes up
  *     in SAFE MODE: console + PD only, no learn/ML/store writes, no automatic
  *     requests, no watchdog (so a broken peripheral cannot reset-loop the
  *     board).  'safe off' clears the counter and reboots the normal path.
  ******************************************************************************
  */
#ifndef APP_WDT_H
#define APP_WDT_H

#include <stdint.h>

#define APP_WDT_SAFE_LIMIT   3U      /* abnormal boots before safe mode        */
#define APP_WDT_TIMEOUT_MS   4000U   /* LSI/256, reload 500                    */

void    APP_WDT_ReportReset(void);   /* decode RCC->RSR, log, pick safe mode   */
void    APP_WDT_Init(void);          /* start the IWDG unless in safe mode     */
void    APP_WDT_Kick(void);          /* feed it (main loop only)               */
uint8_t APP_WDT_Running(void);
uint8_t APP_WDT_InSafeMode(void);
void    APP_WDT_SetSafeMode(uint8_t on);
void    APP_WDT_Print(void);
const char *APP_WDT_ResetCauseName(void);

#endif /* APP_WDT_H */
