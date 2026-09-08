/**
 * @file    ina226.h
 * @brief   INA226 voltage/current monitor on I2C2 (PB10 SCL / PB11 SDA).
 *
 * Hardware (this board):
 *   - one INA226 module, I2C address 0x40 (A0/A1/A2 grounded); 0x41-0x43
 *     are probed as a fallback.
 *   - 5 milli-ohm shunt resistor (R_SHUNT = 0.005 ohm).
 *   - no external I2C pull-ups: the MCU internal pull-ups enabled in
 *     CubeMX (i2c.c, GPIO_PULLUP) are used.
 *
 * The driver is fault tolerant by design: if the INA226 is not connected
 * (or disappears mid-run) the firmware keeps running and keeps reporting
 * "no ina226 connected" on the console.  Every I2C access uses a finite
 * timeout so the super loop can never stall on a missing or wedged chip.
 */
#ifndef INA226_H
#define INA226_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* --- presence / last measurement ---------------------------------------- */
uint8_t INA226_IsPresent(void);      /* 1 = chip answering on I2C2          */
uint8_t INA226_GetAddr(void);        /* 7-bit I2C address found (0 if none) */

/**
 * Last bus voltage measured between the INA226 VBUS pin and GND, in mV.
 * Valid only when INA226_IsPresent() returns 1 and INA226_DataFresh() does
 * too (reading is at most ~1 s old).
 */
int32_t INA226_GetBusMv(void);
/** Last shunt current in microamps (signed; negative = reverse flow). */
int32_t INA226_GetCurUa(void);
/** Last power in milliwatts (computed as V x I). */
int32_t INA226_GetPwrMw(void);
/** 1 = last measurement is younger than the sample period. */
uint8_t INA226_DataFresh(void);

/* --- console ------------------------------------------------------------- */
/** Print a one-shot measurement line (or the "not connected" message). */
void INA226_Print(void);
/** Short status lines for the 'status' command. */
void INA226_PrintStatus(void);
/**
 * 'ina' CLI command:                print one measurement
 *   ina auto on|off                 periodic reporting (default on, 1 s)
 *   ina period <ms>                 periodic reporting interval 250..60000
 *   ina addr <hex>                  force a specific 7-bit address and re-probe
 *   ina scan                        scan the whole I2C2 bus (0x08..0x77)
 *   ina vbus real|synth             feed the PD stack real INA226 VBUS
 *                                   instead of the synthetic value (synth is
 *                                   the default and the safe CC-only setup)
 */
void INA226_Cli(int argc, char *argv[]);

/** 1 when the PD stack should use the INA226 reading as VBUS. */
uint8_t INA226_VbusModeIsReal(void);

#ifdef __cplusplus
}
#endif

#endif /* INA226_H */
