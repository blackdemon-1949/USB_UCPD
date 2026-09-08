#ifndef EXT_I2C_H
#define EXT_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "i2c.h"

/**
 * @file    ext_i2c.h
 * @brief   I2C extension footprint (I2C2, pins PB10 SCL / PB11 SDA).
 *
 * ============================================================================
 *  FOOTPRINT FOR FUTURE I2C FEATURED PROJECTS
 * ============================================================================
 *  This module is the single place where future I2C-based features plug in
 *  (e.g. an INA226 bus voltage/current monitor on I2C2).  The board already
 *  carries the wiring labels for it:  PB10 -> INA226_SCL, PB11 -> INA226_SDA.
 *
 *  To add a feature:
 *    1. Implement EXT_I2C_FeatureInit()  - one-time setup after the I2C2
 *       peripheral is up (device init, "hello" reads, ...).  Called from
 *       EXT_I2C_Init().
 *    2. Implement EXT_I2C_FeaturePoll()   - periodic work from the super
 *       loop (poll a sensor, service a state machine, ...).  Called from
 *       EXT_I2C_Poll().
 *    Both hooks are weak: a future project simply defines strong versions
 *    in its own source files without editing this module or main.c.
 *
 *  Generic 8-bit register helpers are provided below (EXT_I2C_ReadReg /
 *  EXT_I2C_WriteReg) - the common access pattern for the vast majority of
 *  I2C peripherals.  See ext_i2c.c for a worked INA226 example.
 * ============================================================================
 */

/* --- Extension hooks (override to add features) --------------------------- */
void EXT_I2C_FeatureInit(void);   /* weak - feature one-time init   */
void EXT_I2C_FeaturePoll(void);   /* weak - feature periodic poll   */

/* --- Footprint API -------------------------------------------------------- */
void    EXT_I2C_Init(void);       /* call once after MX_I2C2_Init() */
void    EXT_I2C_Poll(void);       /* call from the super loop       */
uint8_t EXT_I2C_IsReady(void);    /* 1 if hi2c2 is initialised      */

/*
 * Default timeout (ms) for EXT_I2C_ReadReg / EXT_I2C_WriteReg.  The old
 * HAL_MAX_DELAY could block the super loop forever when the bus got wedged
 * (SDA held low), killing USB CDC, the CLI and the PD stack with it.
 */
#define EXT_I2C_TIMEOUT_MS  25U

/**
 * @brief  Write up to `len` bytes to an 8-bit register of a device.
 * @param  dev_addr  7-bit I2C address of the device.
 * @param  reg       Register (command) byte.
 * @param  data      Payload.
 * @param  len       Payload length (0 = single register write).
 * @return HAL status (HAL_OK on success).
 */
HAL_StatusTypeDef EXT_I2C_WriteReg(uint16_t dev_addr, uint8_t reg,
                                   const uint8_t *data, uint16_t len);

/**
 * @brief  Read up to `len` bytes from an 8-bit register of a device.
 * @param  dev_addr  7-bit I2C address of the device.
 * @param  reg       Register (command) byte.
 * @param  data      Output buffer (>= len bytes).
 * @param  len       Number of bytes to read.
 * @return HAL status (HAL_OK on success).
 */
HAL_StatusTypeDef EXT_I2C_ReadReg(uint16_t dev_addr, uint8_t reg,
                                  uint8_t *data, uint16_t len);

/**
 * @brief  Timeout-aware variants of the two helpers above.  Use these from
 *         anything polled by the super loop (e.g. the INA226 monitor) so a
 *         missing or wedged device can never stall USB/PD/CLI.
 */
HAL_StatusTypeDef EXT_I2C_WriteRegTO(uint16_t dev_addr, uint8_t reg,
                                     const uint8_t *data, uint16_t len,
                                     uint32_t timeout_ms);
HAL_StatusTypeDef EXT_I2C_ReadRegTO(uint16_t dev_addr, uint8_t reg,
                                    uint8_t *data, uint16_t len,
                                    uint32_t timeout_ms);

/** Parse an unsigned decimal/0x-hex number (CLI helper). 0 = ok. */
int EXT_I2C_ParseU(const char *s, unsigned *out);

#ifdef __cplusplus
}
#endif

#endif /* EXT_I2C_H */
