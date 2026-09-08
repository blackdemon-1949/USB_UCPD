/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32h7rsxx_hal_msp.c
  * @brief        This file provides code for the MSP Initialization
  *               and de-Initialization codes.
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
/**
  * Initializes the Global MSP.
  */
void HAL_MspInit(void)
{

  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  /* System interrupt init*/

  /* Enable USB Voltage detector.
   *
   * The return value is deliberately not turned into an Error_Handler() trap.
   * VDD33USB not coming up (PWR_CSR2_USB33RDY) means the USB HS PHY has no
   * supply, so the CDC console is lost - but the PD sink, the USART2 console
   * and the CLI are all unaffected, and Error_Handler() never returns, so it
   * would take those down as well over a USB problem.
   *
   * usbd_conf.c calls this again in HAL_PCD_MspInit(), checks the result, and
   * gates the D+ pull-up on it (s_usb_clock_ok -> USBD_LL_Init fails).  So a
   * dead VDD33USB now costs the USB console only, and the device simply never
   * appears to the host rather than appearing attached but unable to answer
   * GET_DESCRIPTOR ("device descriptor request failed" / Code 10). */
  (void)HAL_PWREx_EnableUSBVoltageDetector();

  HAL_PWREx_EnableUSBHSregulator();

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
