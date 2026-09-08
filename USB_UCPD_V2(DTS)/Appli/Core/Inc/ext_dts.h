#ifndef EXT_DTS_H
#define EXT_DTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "dts.h"

/**
 * @file    ext_dts.h
 * @brief   DTS extension footprint (on-die Digital Temperature Sensor).
 *
 * ============================================================================
 *  FOOTPRINT FOR FUTURE DTS FEATURED PROJECTS
 * ============================================================================
 *  DTS is new in this firmware revision and is not consumed by the PD policy
 *  engine, the USB CDC console or the INA226 monitor.  This module is the
 *  single place where future temperature-based features plug in (thermal
 *  throttling of the negotiated contract, a fan/Peltier controller, over-
 *  temperature protection, telemetry on the console, ...).
 *
 *  CubeMX brings the peripheral up in dts.c (MX_DTS_Init: LSE reference
 *  clock, 15-cycle sampling time, no hardware trigger, no thresholds).
 *
 *  To add a feature:
 *    1. Implement EXT_DTS_FeatureInit()  - one-time setup after the sensor is
 *       up.  Called from EXT_DTS_Init().
 *    2. Implement EXT_DTS_FeaturePoll()  - periodic work from the super loop
 *       (sample the temperature, run a controller, ...).  Called from
 *       EXT_DTS_Poll().
 *    Both hooks are weak: a future project simply defines strong versions in
 *    its own source files without editing this module or main.c.
 *
 *  ---------------------------------------------------------------------------
 *  IMPORTANT - reference clock
 *  ---------------------------------------------------------------------------
 *  The .ioc selects DTS_REFCLKSEL_LSE and assigns PC14/PC15 as OSC32_IN /
 *  OSC32_OUT (Mcu.Pin4 / Mcu.Pin5, Mode=LSE-External-Oscillator), with
 *  RCC.RTCFreq_Value = 32768 - so the sensor is meant to be clocked from the
 *  32.768 kHz LSE.  Boot's generated SystemClock_Config() only starts the HSE,
 *  though, so nothing else switches the LSE on.
 *
 *  EXT_DTS_TryStart() therefore starts it on demand (dts_ensure_lse: backup
 *  access, then __HAL_RCC_LSE_CONFIG(RCC_LSE_ON)) and only calls
 *  HAL_DTS_Start() once RCC_FLAG_LSERDY is set.  That is folded into the
 *  once-per-second retry in EXT_DTS_Poll(), so nothing blocks and a board with
 *  no 32.768 kHz crystal simply keeps reporting "no reading".  If CubeMX is
 *  regenerated with RCC -> LSE enabled, the generated SystemClock_Config() will
 *  start it instead and dts_ensure_lse() becomes a no-op.
 *
 *  Verified on the WeAct STM32H7R3Z8 board: the LSE starts and the DTS returns
 *  a real die temperature, i.e. a 32.768 kHz crystal is fitted on PC14/PC15.
 *
 *  Why not the PCLK reference clock instead?  The DTS counter clock must stay
 *  below 1 MHz during calibration, the DTS sits on APB4 (RCC->APB4ENR.DTSEN)
 *  which runs at 150 MHz here, and HSREF_CLK_DIV is only 7 bits (max ratio
 *  128) - so the counter would run at 1.17 MHz, over the limit.  LSE is the
 *  correct reference for this clock tree.  Note also that HAL_DTS_GetTemperature()
 *  computes the PCLK case from HAL_RCC_GetPCLK1Freq() (APB1), not APB4, so a
 *  PCLK configuration would additionally require APB1 == APB4.
 *
 *  EXT_DTS_LseRunning() tells you which situation you are in.  All helpers
 *  here are non-blocking, so a sensor that is not clocked can never stall the
 *  super loop (USB CDC, CLI and PD stack stay alive).
 * ============================================================================
 */

/* --- Extension hooks (override to add features) --------------------------- */
void EXT_DTS_FeatureInit(void);   /* weak - feature one-time init   */
void EXT_DTS_FeaturePoll(void);   /* weak - feature periodic poll   */

/* --- Footprint API -------------------------------------------------------- */
void    EXT_DTS_Init(void);       /* call once after MX_DTS_Init()  */
void    EXT_DTS_Poll(void);       /* call from the super loop       */
uint8_t EXT_DTS_IsReady(void);    /* 1 if hdts is initialised       */

/** @brief  1 if the sensor is clocked and converting. */
uint8_t EXT_DTS_IsRunning(void);

/** @brief  1 if the LSE crystal is up (the DTS reference clock in the .ioc). */
uint8_t EXT_DTS_LseRunning(void);

/** @brief  (Re)start continuous conversion.  Non-blocking, HAL_OK on success. */
HAL_StatusTypeDef EXT_DTS_TryStart(void);

/**
 * @brief  Read the die temperature in degrees Celsius.
 * @param  deg_c  Output, degrees C (may be negative).
 * @return HAL_OK on success; the sensor is (re)started automatically if it
 *         stopped.  Never blocks the caller.
 */
HAL_StatusTypeDef EXT_DTS_ReadTempC(int32_t *deg_c);

/** @brief  Last HAL status returned by the footprint (for `dts status`). */
HAL_StatusTypeDef EXT_DTS_LastStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* EXT_DTS_H */
