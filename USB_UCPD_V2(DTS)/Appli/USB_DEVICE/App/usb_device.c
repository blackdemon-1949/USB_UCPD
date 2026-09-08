/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           usb_device.c
  * @author         MCD Application Team
  * @brief          This file implements the USB Device
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

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

/* USER CODE BEGIN Includes */
#include "app_log.h"
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* 1 = the USB device stack came up cleanly (see MX_USB_DEVICE_Init). */
static uint8_t usb_device_ok;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceHS;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_DEVICE_Init(void)
{
  /* USER CODE BEGIN USB_DEVICE_Init_PreTreatment */

  /* USER CODE END USB_DEVICE_Init_PreTreatment */

  /* Init Device Library, add supported class and start the library.
     A failure here used to call Error_Handler() (solid PB2, dead board);
     the PD bench part of the firmware must survive a USB problem, so log
     it and keep running without the console instead. */
  if (USBD_Init(&hUsbDeviceHS, &CDC_Desc, DEVICE_HS) == USBD_OK)
  {
    if (USBD_RegisterClass(&hUsbDeviceHS, &USBD_CDC) == USBD_OK)
    {
      if (USBD_CDC_RegisterInterface(&hUsbDeviceHS, &USBD_Interface_fops_HS) == USBD_OK)
      {
        if (USBD_Start(&hUsbDeviceHS) == USBD_OK)
        {
          usb_device_ok = 1U;
        }
      }
    }
  }
  if (usb_device_ok == 0U)
  {
    APP_LOG_Write("usb: CDC init failed - running without the serial console\r\n");
  }

  /* USER CODE BEGIN USB_DEVICE_Init_PostTreatment */

  /* USER CODE END USB_DEVICE_Init_PostTreatment */
}

/**
  * @}
  */

/**
  * @}
  */

