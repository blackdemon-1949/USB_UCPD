/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           usb_device.h
  * @author         MCD Application Team
  * @brief          Header for usb_device.c file.
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
#ifndef __USB_DEVICE_H
#define __USB_DEVICE_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx.h"
#include "usbd_def.h"
#include "stm32h7rsxx_hal.h"

/* USER CODE BEGIN INCLUDE */

/* USER CODE END INCLUDE */

/** @addtogroup USBD_OTG_DRIVER
  * @{
  */

/** @defgroup USBD_DEVICE USBD_DEVICE
  * @brief Device file for Usb otg low level driver.
  * @{
  */

/** @defgroup USBD_DEVICE_Exported_Variables USBD_DEVICE_Exported_Variables
  * @brief Public variables.
  * @{
  */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN VARIABLES */

/* USER CODE END VARIABLES */
/**
  * @}
  */

/** @defgroup USBD_DEVICE_Exported_FunctionsPrototype USBD_DEVICE_Exported_FunctionsPrototype
  * @brief Declaration of public functions for Usb device.
  * @{
  */

/** USB Device initialization function. */
void MX_USB_DEVICE_Init(void);

/* Deferred bring-up failsafe (see usb_device.c):
     MX_USB_DEVICE_Init() only tries once, and on this board the CDC core
     refuses to come up while the USB 3.3 V detector reports "not ready"
     (PWR_CSR2.USB33RDY).  If the rail is still rising when the firmware
     boots, a single attempt leaves the console dead for the whole power
     cycle.  USB_DEVICE_Task() is called from the main loop and retries the
     bring-up a bounded number of times, without ever blocking the PD stack. */
void    USB_DEVICE_Task(void);
void    USB_DEVICE_RetryNow(void);
uint8_t USB_DEVICE_Ready(void);
void    USB_DEVICE_Status(void);      /* prints one human-readable line */
uint8_t USBD_LL_ClockOk(void);        /* PHY clock / VDD33USB gate state */

/*
 * -- Insert functions declaration here --
 */
/* USER CODE BEGIN FD */

/* USER CODE END FD */
/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __USB_DEVICE_H */
