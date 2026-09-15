/**
  ******************************************************************************
  * @file    app_oled.h
  * @brief   0.96" 128x64 I2C OLED page renderer for the PD bench.
  *
  * Shares I2C2 with the INA226 - the SSD1306 driver only ever issues short,
  * complete, chunked transfers, so both devices run together.
  *
  * Pages:  0 voltage, 1 current, 2 power, 3 requested V/A, 4 protocol,
 *         5 SoC die temperature (DTS).
  * Key:    PC13.  One press  = next page.
  *                Two presses = request the next SPR fixed PDO (1..7).
  *
  * Nothing here is required for the PD sink to work.  If the display is
  * missing, or the bus is busy, or I2C2 is not up, every entry point is a
  * no-op and the rest of the firmware never notices.
  ******************************************************************************
  */

#ifndef APP_OLED_H
#define APP_OLED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Number of pages. */
#define APP_OLED_PAGES        6U

void APP_OLED_Init(void);
void APP_OLED_Poll(void);

/** CLI: `oled ...`. */
void APP_OLED_Cli(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_OLED_H */
