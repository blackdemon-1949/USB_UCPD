/**
  ******************************************************************************
  * @file    app_fault.c
  * @brief   See app_fault.h - non-fatal init failures and fatal-error policy.
  ******************************************************************************
  */
#include "app_fault.h"
#include "app_config.h"
#include "app_log.h"
#include "app_board.h"
#include "main.h"

#if APP_CMOS_ENABLED
#include "app_cmos.h"
#endif

/* Set by APP_FAULT_Init(): before that the console does not necessarily exist,
 * so a failure is only recorded in the CMOS (and even that is optional). */
static uint8_t  s_log_ok;
static uint16_t s_init_fails;

void APP_FAULT_Init(void)
{
  s_log_ok = 1U;
}

uint32_t APP_FAULT_Tag(const char *tag)
{
  uint32_t v = 0U;

  if (tag == NULL)
  {
    return 0U;
  }
  for (uint8_t i = 0U; (i < 4U) && (tag[i] != '\0'); i++)
  {
    v = (v << 8) | (uint32_t)(uint8_t)tag[i];
  }
  return v;
}

void APP_INIT_Fail(const char *tag, int status)
{
  s_init_fails++;
#if APP_CMOS_ENABLED
  APP_CMOS_LogFault(APP_FAULT_INIT, APP_FAULT_Tag(tag));
#endif
  if (s_log_ok != 0U)
  {
    APP_LOG_Printf("[init] %s failed (status=%d) - continuing without it\r\n",
                   (tag != NULL) ? tag : "?", status);
  }
}

/* Volatile: written in normal context, read from the fault handler. */
static volatile uint8_t s_flash_busy;

void APP_FAULT_FlashBegin(void) { s_flash_busy = 1U; }
void APP_FAULT_FlashEnd(void)   { s_flash_busy = 0U; }
uint8_t APP_FAULT_FlashBusy(void) { return s_flash_busy; }

uint8_t APP_FAULT_LastWasFlashRisk(void)
{
#if APP_CMOS_ENABLED
  return (uint8_t)((APP_CMOS()->last_fault == APP_FAULT_RAMFUNC) ? 1U : 0U);
#else
  return 0U;
#endif
}

void APP_FAULT_Mark(uint8_t code, uint32_t extra, uint32_t pc)
{
  /* A fault taken while the external flash was being programmed is not the
     same animal as a random HardFault: it means "the RAM-mode window is the
     suspect", and the next boot must not walk into it again. */
  if (s_flash_busy != 0U)
  {
    code = APP_FAULT_RAMFUNC;
  }
#if APP_CMOS_ENABLED
  APP_CMOS_FaultMark((uint32_t)code, extra, pc);
#else
  (void)code; (void)extra; (void)pc;
#endif
}

uint16_t APP_FAULT_InitFailCount(void)
{
  return s_init_fails;
}

/**
  * @brief  Fatal error policy: leave a breadcrumb that survives the reset, say
  *         what happened when there is a console, and reboot.  The watchdog and
  *         the safe-mode counter in app_wdt.c stop this from becoming an
  *         invisible boot loop: after APP_WDT_SAFE_LIMIT abnormal boots the
  *         firmware comes up with the optional subsystems switched off.
  */
void APP_FATAL(uint8_t code)
{
#if APP_CMOS_ENABLED
  APP_CMOS_FaultMark(code, HAL_GetTick(), (uint32_t)(uintptr_t)__builtin_return_address(0));
#endif
  if (s_log_ok != 0U)
  {
    APP_LOG_Printf("[fatal] code %u - resetting\r\n", (unsigned)code);
    APP_LOG_Flush();
  }

  /* Short visible blink so a board without a console is still diagnosable,
     then a controlled reset.  Never an infinite loop. */
  for (uint8_t i = 0U; i < code; i++)
  {
    HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_SET);
    for (volatile uint32_t d = 0U; d < 200000UL; d++) { __NOP(); }
    HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
    for (volatile uint32_t d = 0U; d < 200000UL; d++) { __NOP(); }
  }

  NVIC_SystemReset();
  for (;;) { }                      /* NVIC_SystemReset() does not return */
}
