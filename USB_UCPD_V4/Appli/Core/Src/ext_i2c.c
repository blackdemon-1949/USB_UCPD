/**
 * @file    ext_i2c.c
 * @brief   I2C extension footprint - I2C2 (PB10 SCL / PB11 SDA, INA226 wiring).
 *
 * Wiring labels on the board:  PB10 -> INA226_SCL, PB11 -> INA226_SDA.
 * I2C2 is configured by CubeMX in i2c.c (MX_I2C2_Init, 400 kHz fast mode).
 */
#include "ext_i2c.h"
#include "app_log.h"
#include <string.h>
#include <stdlib.h>

/* ==========================================================================
 *  FEATURE REGISTRATION
 *  --------------------------------------------------------------------------
 *  Future I2C feature projects override these two weak hooks with their own
 *  strong definitions (no edits needed here or in main.c):
 *
 *    void EXT_I2C_FeatureInit(void)  - one-time setup, called from
 *                                      EXT_I2C_Init() after MX_I2C2_Init().
 *    void EXT_I2C_FeaturePoll(void)  - periodic work, called from
 *                                      EXT_I2C_Poll() every super-loop pass.
 *
 *  Worked example (INA226 power monitor on I2C2, addr 0x40 << 1):
 *
 *    #include "ext_i2c.h"
 *    #define INA226_ADDR       (0x40U)
 *    #define INA226_REG_MFR_ID (0xFEU)     // manufacturer ID register
 *    #define INA226_MFR_TI     (0x5449U)   // "TI"
 *
 *    void EXT_I2C_FeatureInit(void)
 *    {
 *      uint8_t id[2];
 *      if (EXT_I2C_ReadReg(INA226_ADDR, INA226_REG_MFR_ID, id, 2) == HAL_OK)
 *      {
 *        APP_LOG_Printf("ina226: mfr id 0x%02X%02X (expect TI)\r\n",
 *                       id[1], id[0]);             // big-endian reg value
 *      }
 *      // ... configure registers (calibration, shunt, averaging) ...
 *    }
 *
 *    void EXT_I2C_FeaturePoll(void)
 *    {
 *      static uint32_t t;
 *      uint8_t  bus[2], shunt[2];
 *      uint32_t now = HAL_GetTick();
 *      if ((now - t) < 250U) return;                // 4 Hz sample rate
 *      t = now;
 *      if (EXT_I2C_ReadReg(INA226_ADDR, 0x02U, bus, 2)   != HAL_OK) return;
 *      if (EXT_I2C_ReadReg(INA226_ADDR, 0x01U, shunt, 2) != HAL_OK) return;
 *      int32_t mv = ((bus[0]   << 8) | bus[1])   * 125 / 100;  // 1.25 mV/LSB
 *      int32_t ma = ((shunt[0] << 8) | shunt[1]) * 10;         // 10 uV/LSB
 *      APP_LOG_Printf("ina226: %ld mV, %ld mA\r\n", (long)mv, (long)ma);
 *    }
 * ========================================================================== */

__weak void EXT_I2C_FeatureInit(void)
{
  /* Add future I2C feature initialisation here (or override this hook). */
}

__weak void EXT_I2C_FeaturePoll(void)
{
  /* Add future I2C feature polling here (or override this hook). */
}

/* ==========================================================================
 *  FOOTPRINT API
 * ========================================================================== */

void EXT_I2C_Init(void)
{
  /* The peripheral itself is brought up by MX_I2C2_Init() in main.c.
   * This hook is the application-level extension point that runs after it. */
  if (hi2c2.Instance != I2C2)
  {
    APP_LOG_Printf("ext-i2c: ERROR I2C2 not initialised\r\n");
    return;
  }

  APP_LOG_Printf("ext-i2c: I2C2 ready  SCL=PB10(INA226_SCL) SDA=PB11(INA226_SDA) "
                 "400 kHz\r\n");

  /* Feature hook: future I2C projects add their one-time setup here. */
  EXT_I2C_FeatureInit();
}

void EXT_I2C_Poll(void)
{
  /* Feature hook: future I2C projects add their periodic work here.
   * Runs on every super-loop pass; keep it short (non-blocking or
   * time-gated) so the PD stack, USB CDC and CLI stay responsive. */
  EXT_I2C_FeaturePoll();
}

uint8_t EXT_I2C_IsReady(void)
{
  return (hi2c2.Instance == I2C2) ? 1U : 0U;
}

int EXT_I2C_ParseU(const char *s, unsigned *out)
{
  char *end = NULL;
  unsigned long v;
  if ((s == NULL) || (*s == '\0'))
  {
    return -1;
  }
  v = strtoul(s, &end, 0);
  if ((end == s) || (end == NULL) || (*end != '\0'))
  {
    return -1;
  }
  *out = (unsigned)v;
  return 0;
}

HAL_StatusTypeDef EXT_I2C_WriteRegTO(uint16_t dev_addr, uint8_t reg,
                                     const uint8_t *data, uint16_t len,
                                     uint32_t timeout_ms)
{
  if (!EXT_I2C_IsReady())
  {
    return HAL_ERROR;
  }
  if (len == 0U)
  {
    return HAL_I2C_Master_Transmit(&hi2c2, (uint16_t)(dev_addr << 1), &reg, 1U,
                                   timeout_ms);
  }
  uint8_t buf[1 + 32U];
  if (len > sizeof(buf) - 1U)
  {
    return HAL_ERROR;
  }
  buf[0] = reg;
  (void)memcpy(&buf[1], data, len);
  return HAL_I2C_Master_Transmit(&hi2c2, (uint16_t)(dev_addr << 1), buf,
                                 (uint16_t)(1U + len), timeout_ms);
}

HAL_StatusTypeDef EXT_I2C_ReadRegTO(uint16_t dev_addr, uint8_t reg,
                                    uint8_t *data, uint16_t len,
                                    uint32_t timeout_ms)
{
  if (!EXT_I2C_IsReady())
  {
    return HAL_ERROR;
  }
  if (HAL_I2C_Master_Transmit(&hi2c2, (uint16_t)(dev_addr << 1), &reg, 1U,
                              timeout_ms) != HAL_OK)
  {
    return HAL_ERROR;
  }
  return HAL_I2C_Master_Receive(&hi2c2, (uint16_t)(dev_addr << 1), data, len,
                                timeout_ms);
}

HAL_StatusTypeDef EXT_I2C_WriteReg(uint16_t dev_addr, uint8_t reg,
                                   const uint8_t *data, uint16_t len)
{
  return EXT_I2C_WriteRegTO(dev_addr, reg, data, len, EXT_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef EXT_I2C_ReadReg(uint16_t dev_addr, uint8_t reg,
                                  uint8_t *data, uint16_t len)
{
  return EXT_I2C_ReadRegTO(dev_addr, reg, data, len, EXT_I2C_TIMEOUT_MS);
}
