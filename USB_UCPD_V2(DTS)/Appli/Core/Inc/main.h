/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx_hal.h"

#include "stm32h7rsxx_ll_ucpd.h"
#include "stm32h7rsxx_ll_bus.h"
#include "stm32h7rsxx_ll_cortex.h"
#include "stm32h7rsxx_ll_rcc.h"
#include "stm32h7rsxx_ll_system.h"
#include "stm32h7rsxx_ll_utils.h"
#include "stm32h7rsxx_ll_pwr.h"
#include "stm32h7rsxx_ll_gpio.h"
#include "stm32h7rsxx_ll_dma.h"

#include "stm32h7rsxx_ll_exti.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
/* Visible fatal error: blink PB2 `code` times, pause, repeat (never returns).
 * Codes 1-5 boot, 6 USB, 7 generic, 8 USBPD init.  See README. */
void Appli_Fatal(uint8_t code);

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define User_Button_Pin GPIO_PIN_13
#define User_Button_GPIO_Port GPIOC
#define INA226_SCL_Pin GPIO_PIN_10
#define INA226_SCL_GPIO_Port GPIOB
#define INA226_SDA_Pin GPIO_PIN_11
#define INA226_SDA_GPIO_Port GPIOB
#define Built_IN_LED_Pin GPIO_PIN_2
#define Built_IN_LED_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* Board aliases used by the application layer.  LED / KEY follow the
 * CubeMX-generated labels above so a regeneration cannot desynchronise
 * them from the .ioc. */
#define LED_Pin                 Built_IN_LED_Pin
#define LED_GPIO_Port           Built_IN_LED_GPIO_Port
#define KEY_Pin                 User_Button_Pin
#define KEY_GPIO_Port           User_Button_GPIO_Port
/* UCPD1 CC lines live on the MCU header, not on the USB-HS Type-C socket. */
#define UCPD_CC1_Pin            GPIO_PIN_0
#define UCPD_CC1_GPIO_Port      GPIOM
#define UCPD_CC2_Pin            GPIO_PIN_1
#define UCPD_CC2_GPIO_Port      GPIOM
/* Appli image: XiP window that the Boot project maps on XSPI1. */
#define APP_XIP_BASE            (0x90000000UL)
#define APP_XIP_SIZE            (0x00800000UL)
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
