/**
 * @file    ext_dts.c
 * @brief   DTS extension footprint - on-die Digital Temperature Sensor.
 *
 * DTS is configured by CubeMX in dts.c (MX_DTS_Init, LSE reference clock,
 * 15-cycle sampling time, no hardware trigger).  Nothing else in this
 * firmware uses the sensor, so it is free for feature projects.
 *
 * NOTE: the .ioc clocks DTS from the LSE, and the Boot project only starts
 * the HSE.  Until the LSE is enabled (see ext_dts.h) the sensor produces no
 * conversions; every helper here detects that and returns immediately.
 */
#include "ext_dts.h"
#include "app_log.h"

/* ==========================================================================
 *  FEATURE REGISTRATION
 *  --------------------------------------------------------------------------
 *  Future DTS feature projects override these two weak hooks with their own
 *  strong definitions (no edits needed here or in main.c):
 *
 *    void EXT_DTS_FeatureInit(void)  - one-time setup, called from
 *                                      EXT_DTS_Init() after MX_DTS_Init().
 *    void EXT_DTS_FeaturePoll(void)  - periodic work, called from
 *                                      EXT_DTS_Poll() every super-loop pass.
 *
 *  Worked example (log the die temperature once a second, throttle at 100 C):
 *
 *    #include "ext_dts.h"
 *
 *    void EXT_DTS_FeaturePoll(void)
 *    {
 *      static uint32_t t;
 *      int32_t deg_c;
 *      uint32_t now = HAL_GetTick();
 *      if ((now - t) < 1000U) return;               // 1 Hz
 *      t = now;
 *      if (EXT_DTS_ReadTempC(&deg_c) != HAL_OK) return;
 *      APP_LOG_Printf("die temp: %ld C\r\n", (long)deg_c);
 *      if (deg_c > 100) { ...  drop the PD contract / spin a fan ...  }
 *    }
 * ========================================================================== */

__weak void EXT_DTS_FeatureInit(void)
{
  /* Add future DTS feature initialisation here (or override this hook). */
}

__weak void EXT_DTS_FeaturePoll(void)
{
  /* Add future DTS feature polling here (or override this hook). */
}

/* ==========================================================================
 *  FOOTPRINT API
 * ========================================================================== */

#define EXT_DTS_RETRY_MS  1000U   /* min. interval between (re)start attempts */

static HAL_StatusTypeDef s_last = HAL_OK;
static uint8_t           s_running;
static uint32_t          s_retry_at;

uint8_t EXT_DTS_IsReady(void)
{
  return (hdts.Instance == DTS) ? 1U : 0U;
}

uint8_t EXT_DTS_IsRunning(void)
{
  return s_running;
}

uint8_t EXT_DTS_LseRunning(void)
{
  return (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) != 0U) ? 1U : 0U;
}

HAL_StatusTypeDef EXT_DTS_LastStatus(void)
{
  return s_last;
}

/**
  * @brief  Start the 32.768 kHz LSE if it is not oscillating yet.
  *
  * The .ioc assigns PC14/PC15 as OSC32_IN/OSC32_OUT (Mcu.Pin4 / Mcu.Pin5, both
  * Mode=LSE-External-Oscillator), sets DTS.RefClock=DTS_REFCLKSEL_LSE and
  * RCC.RTCFreq_Value=32768 - so the DTS is meant to be clocked from the LSE.
  * Boot's generated SystemClock_Config() only starts the HSE, though, so the
  * reference clock is never switched on and HAL_DTS_Start() burns its 1 ms
  * TS1_RDY timeout on every attempt.
  *
  * Starting it here instead of in SystemClock_Config() is deliberate: this file
  * is never overwritten by CubeMX code generation, and the wait is folded into
  * the existing once-per-second retry below so nothing ever blocks.  A board
  * without a 32.768 kHz crystal therefore degrades to "no reading" rather than
  * stopping in Error_Handler().  If CubeMX is later regenerated with
  * RCC -> LSE enabled, the first test makes this a no-op.
  */
static void dts_ensure_lse(void)
{
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) != 0U)
  {
    return;                       /* already running */
  }
  if ((RCC->BDCR & RCC_BDCR_LSEON) != 0U)
  {
    return;                       /* already requested, crystal still settling */
  }
  /* PWR is always clocked on the H7RS - there is no __HAL_RCC_PWR_CLK_ENABLE()
   * in this HAL, and HAL_PWR_EnableBkUpAccess() only sets PWR_CR1_DBP. */
  HAL_PWR_EnableBkUpAccess();     /* the LSE lives in the backup domain */
  __HAL_RCC_LSE_CONFIG(RCC_LSE_ON);
}

HAL_StatusTypeDef EXT_DTS_TryStart(void)
{
  if (!EXT_DTS_IsReady())
  {
    s_last = HAL_ERROR;
    return s_last;
  }

  /* This project clocks the DTS from the LSE.  Make sure that reference is
   * actually oscillating before asking the sensor to start, otherwise
   * HAL_DTS_Start() just burns its TS1_RDY timeout and reports nothing useful.
   * Gated on the configured reference clock so switching the .ioc to
   * DTS_REFCLKSEL_PCLK later keeps working. */
  if (hdts.Init.RefClock == DTS_REFCLKSEL_LSE)
  {
    dts_ensure_lse();
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == 0U)
    {
      s_last = HAL_BUSY;          /* EXT_DTS_Poll() retries once a second */
      s_running = 0U;
      return s_last;
    }
  }

  /* HAL_DTS_Start() leaves the handle in BUSY when it times out, which would
   * make every later attempt fail with HAL_BUSY.  Put it back to READY first
   * so a sensor that was merely unclocked can be brought up later. */
  if (hdts.State != HAL_DTS_STATE_READY)
  {
    hdts.State = HAL_DTS_STATE_READY;
  }

  s_last = HAL_DTS_Start(&hdts);
  s_running = (s_last == HAL_OK) ? 1U : 0U;
  return s_last;
}

void EXT_DTS_Init(void)
{
  /* The peripheral itself is brought up by MX_DTS_Init() in main.c.
   * This hook is the application-level extension point that runs after it. */
  if (hdts.Instance != DTS)
  {
    APP_LOG_Printf("ext-dts: ERROR DTS not initialised\r\n");
    s_last = HAL_ERROR;
    return;
  }

  if (EXT_DTS_TryStart() != HAL_OK)
  {
    /* Almost always "the LSE reference clock is not running" - see the
     * reference-clock note in ext_dts.h.  Reported, never fatal. */
    APP_LOG_Printf("ext-dts: not converting yet (LSE ready=%u, status=%d) - "
                   "starting the 32.768 kHz reference, retrying every 1 s\r\n",
                   (unsigned)EXT_DTS_LseRunning(), (int)s_last);
  }
  else
  {
    APP_LOG_Printf("ext-dts: DTS ready  refclk=%s  15-cycle sampling (footprint idle)\r\n",
                   (EXT_DTS_LseRunning() != 0U) ? "LSE" : "?");
  }

  /* Feature hook: future DTS projects add their one-time setup here. */
  EXT_DTS_FeatureInit();
}

void EXT_DTS_Poll(void)
{
  /* Keep the sensor running: if a start attempt failed earlier (unclocked
   * LSE at boot), retry once a second instead of hammering the HAL. */
  if ((s_running == 0U) && EXT_DTS_IsReady() &&
      ((int32_t)(HAL_GetTick() - s_retry_at) >= 0))
  {
    s_retry_at = HAL_GetTick() + EXT_DTS_RETRY_MS;
    (void)EXT_DTS_TryStart();
  }

  /* Feature hook: future DTS projects add their periodic work here.
   * Runs on every super-loop pass; keep it short (non-blocking or
   * time-gated) so the PD stack, USB CDC and CLI stay responsive. */
  EXT_DTS_FeaturePoll();
}

HAL_StatusTypeDef EXT_DTS_ReadTempC(int32_t *deg_c)
{
  if ((deg_c == NULL) || !EXT_DTS_IsReady())
  {
    return HAL_ERROR;
  }
  if (s_running == 0U)
  {
    if (EXT_DTS_TryStart() != HAL_OK)
    {
      return s_last;
    }
  }

  s_last = HAL_DTS_GetTemperature(&hdts, deg_c);
  if (s_last != HAL_OK)
  {
    /* Conversion lost (state machine left BUSY by the failed read): the next
     * poll re-arms it. */
    s_running = 0U;
  }
  return s_last;
}
