/**
  ******************************************************************************
  * @file    app_wdt.c
  * @brief   Watchdog + reset-cause + safe mode - see app_wdt.h.
  ******************************************************************************
  */
#include "app_wdt.h"
#include "app_cmos.h"
#include "app_log.h"
#include "main.h"

/* IWDG keys (the CMSIS header only defines the field, not the values). */
#define WDT_KEY_UNLOCK   0x00005555UL
#define WDT_KEY_START    0x0000CCCCUL
#define WDT_KEY_RELOAD   0x0000AAAAUL

static uint8_t  s_running;
static uint8_t  s_safe_mode;
static uint32_t s_rsr;

const char *APP_WDT_ResetCauseName(void)
{
  if ((s_rsr & RCC_RSR_IWDGRSTF) != 0U) { return "watchdog"; }
  if ((s_rsr & RCC_RSR_SFTRSTF) != 0U)  { return "software"; }
  if ((s_rsr & RCC_RSR_PORRSTF) != 0U)  { return "power-on/brown-out"; }
  if ((s_rsr & RCC_RSR_PINRSTF) != 0U)  { return "reset pin"; }
  if ((s_rsr & RCC_RSR_BORRSTF) != 0U)  { return "brown-out"; }
  return "unknown";
}

void APP_WDT_ReportReset(void)
{
  app_settings_t *s = APP_CMOS();

  s_rsr = RCC->RSR;
  RCC->RSR |= RCC_RSR_RMVF;            /* clear the flags for the next boot  */

  if ((s_rsr & RCC_RSR_IWDGRSTF) != 0U)
  {
    APP_CMOS_LogFault(APP_FAULT_WATCHDOG, s_rsr);
    s->fault_count++;
  }
  else if ((s_rsr & (RCC_RSR_PORRSTF | RCC_RSR_PINRSTF | RCC_RSR_BORRSTF)) != 0U)
  {
    /* A real power cycle: the backup domain may have been powered from VBAT,
     * so the settings survive, and this is a normal boot - it clears the
     * abnormal counter so a single watchdog hit does not stick forever. */
    s->fault_count = 0U;
  }

  if (s->fault_count >= APP_WDT_SAFE_LIMIT)
  {
    s_safe_mode = 1U;
  }

  APP_LOG_Printf("[boot] reset cause: %s (RSR=0x%08lX), abnormal boots=%lu%s\r\n",
                 APP_WDT_ResetCauseName(), (unsigned long)s_rsr,
                 (unsigned long)s->fault_count,
                 s_safe_mode ? " - SAFE MODE" : "");

  if (s_safe_mode)
  {
    APP_LOG_Write("[boot] safe mode: console + PD only. 'safe off' clears this.\r\n");
  }
  APP_CMOS_Save();
}

void APP_WDT_Init(void)
{
  if (s_safe_mode)
  {
    /* No watchdog in safe mode: the whole point is to stay reachable even if
     * a peripheral misbehaves, and the user is at the console anyway. */
    s_running = 0U;
    return;
  }
  if (s_running)
  {
    return;
  }

  IWDG->KR = WDT_KEY_UNLOCK;
  IWDG->PR = 6U;                       /* LSI / 256 = 125 Hz                */
  IWDG->RLR = ((APP_WDT_TIMEOUT_MS * 125U) / 1000U);
  while (IWDG->SR != 0U)
  {
    /* Wait for the SRVU/PVU/WVU update flags (they clear by themselves). */
  }
  IWDG->KR = WDT_KEY_RELOAD;
  IWDG->KR = WDT_KEY_START;
  s_running = 1U;
}

void APP_WDT_Kick(void)
{
  if (s_running)
  {
    IWDG->KR = WDT_KEY_RELOAD;
  }
}

uint8_t APP_WDT_Running(void)     { return s_running; }
uint8_t APP_WDT_InSafeMode(void)  { return s_safe_mode; }

void APP_WDT_SetSafeMode(uint8_t on)
{
  s_safe_mode = (on != 0U) ? 1U : 0U;
  if (s_safe_mode)
  {
    s_running = 0U;                  /* cannot be stopped, only not started */
  }
}

void APP_WDT_Print(void)
{
  app_settings_t *s = APP_CMOS();

  APP_LOG_Printf("wdt: %s, timeout %ums, abnormal=%lu/%u, reset=%s, safe=%s\r\n",
                 s_running ? "running" : "stopped",
                 (unsigned)APP_WDT_TIMEOUT_MS,
                 (unsigned long)s->fault_count, (unsigned)APP_WDT_SAFE_LIMIT,
                 APP_WDT_ResetCauseName(),
                 s_safe_mode ? "yes" : "no");
}
