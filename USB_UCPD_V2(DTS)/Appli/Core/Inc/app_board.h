#ifndef APP_BOARD_H
#define APP_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define APP_LED_PORT          GPIOB
#define APP_LED_PIN           GPIO_PIN_2
#define APP_KEY_PORT          GPIOC
#define APP_KEY_PIN           GPIO_PIN_13
/* Active HIGH: the schematic is 3V3 - button - 330R - PC13 (see gpio.c).
   Nothing in the firmware uses this macro; the OLED key state machine in
   app_oled.c does its own sampling. */
#define APP_KEY_PRESSED()     (HAL_GPIO_ReadPin(APP_KEY_PORT, APP_KEY_PIN) == GPIO_PIN_SET)

typedef enum
{
  APP_LED_OFF = 0,
  APP_LED_ON,
  APP_LED_HEARTBEAT,   /* USB up, no PD */
  APP_LED_PD_WAIT,     /* CC attached, negotiating */
  APP_LED_PD_CONTRACT, /* explicit contract */
  APP_LED_FAULT
} APP_LED_Mode_t;

void APP_LED_Set(APP_LED_Mode_t mode);
void APP_LED_Task(void);
void APP_BOARD_PrintInfo(void);
void APP_BOARD_PrintUcpd(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BOARD_H */
