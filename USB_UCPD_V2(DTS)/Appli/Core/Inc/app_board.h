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
#define APP_KEY_PRESSED()     (HAL_GPIO_ReadPin(APP_KEY_PORT, APP_KEY_PIN) == GPIO_PIN_RESET)

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
