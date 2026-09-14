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

/* Deferred bring-up state (see USB_DEVICE_Task).  The CDC core is the
   human-facing half of this instrument: losing it because the USB 3.3 V
   rail was still rising at boot would leave a board that looks dead even
   though the PD stack is fine, so the attempt is repeated a few times
   instead of being written off for the whole power cycle. */
static uint8_t  usb_tries;              /* attempts made so far          */
static uint8_t  usb_fail_step;          /* 1..4, 0 = nothing failed      */
static uint8_t  usb_phy_ok_last;        /* USBD_LL_ClockOk() of last try */
static uint32_t usb_next_try_ms;

#define USB_DEVICE_MAX_TRIES        5U
#define USB_DEVICE_RETRY_PERIOD_MS  1000U

static const char *usb_fail_name(uint8_t step)
{
  switch (step)
  {
    case 1U: return "USBD_Init";
    case 2U: return "USBD_RegisterClass";
    case 3U: return "USBD_CDC_RegisterInterface";
    case 4U: return "USBD_Start";
    default: return "none";
  }
}

static void usb_attempt(uint8_t announce);
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
/* One bring-up attempt.  Never blocks, never calls Error_Handler(), never
   leaves the device half-attached: if the stack cannot be started it is torn
   down again so the D+ pull-up is not asserted by a core with no valid PHY
   clock (that state is what Windows reports as Code 10 / "descriptor request
   failed"). announce=1 logs the outcome once per state change. */
static void usb_attempt(uint8_t announce)
{
  uint8_t fail = 0U;

  if (usb_tries < USB_DEVICE_MAX_TRIES)
  {
    usb_tries++;
  }

  /* A previous attempt may have left the PCD half-configured; tear it down
     before re-initialising (USBD_DeInit -> USBD_LL_DeInit -> HAL_PCD_DeInit).
     The very first attempt has nothing to undo. */
  if (usb_tries > 1U)
  {
    (void)USBD_DeInit(&hUsbDeviceHS);
  }

  /* Build the serial-number string descriptor before USBD_Start() asserts the
     D+ pull-up.  It comes from the 96-bit device UID, so it is identical on
     every reset path and after every retry; Windows keys the COM port number
     (COM8 / COM10 / ...) off VID+PID+serial, so it must never change. */
  USBD_CDC_BuildSerialNum();

  usb_device_ok  = 0U;
  usb_fail_step  = 0U;

  if (USBD_Init(&hUsbDeviceHS, &CDC_Desc, DEVICE_HS) != USBD_OK)
  {
    fail = 1U;
  }
  else if (USBD_RegisterClass(&hUsbDeviceHS, &USBD_CDC) != USBD_OK)
  {
    fail = 2U;
  }
  else if (USBD_CDC_RegisterInterface(&hUsbDeviceHS, &USBD_Interface_fops_HS) != USBD_OK)
  {
    fail = 3U;
  }
  else if (USBD_Start(&hUsbDeviceHS) != USBD_OK)
  {
    fail = 4U;
  }
  else
  {
    usb_device_ok = 1U;
  }

  usb_phy_ok_last = USBD_LL_ClockOk();
  usb_fail_step   = fail;
  usb_next_try_ms = HAL_GetTick() + USB_DEVICE_RETRY_PERIOD_MS;

  if (usb_device_ok != 0U)
  {
    if (announce != 0U)
    {
      APP_LOG_Printf("usb: CDC console ready (vbus/phy=%u, try %u)\r\n",
                     (unsigned)usb_phy_ok_last, (unsigned)usb_tries);
    }
  }
  else
  {
    if (fail != 0U)
    {
      (void)USBD_DeInit(&hUsbDeviceHS);   /* never leave it half-attached */
    }
    if (announce != 0U)
    {
      if (usb_phy_ok_last == 0U)
      {
        APP_LOG_Printf("usb: no PHY clock / VDD33USB not ready yet (try %u/%u)\r\n",
                       (unsigned)usb_tries, (unsigned)USB_DEVICE_MAX_TRIES);
      }
      else
      {
        APP_LOG_Printf("usb: CDC init failed at %s (try %u/%u)\r\n",
                       usb_fail_name(fail), (unsigned)usb_tries,
                       (unsigned)USB_DEVICE_MAX_TRIES);
      }
    }
  }
}

/* CubeMX entry point: one attempt, then the main loop keeps trying. */
void MX_USB_DEVICE_Init(void)
{
  usb_attempt(1U);

  if ((usb_device_ok == 0U) && (usb_tries < USB_DEVICE_MAX_TRIES))
  {
    APP_LOG_Printf("usb: will keep trying in the background (%u more attempts)\r\n",
                   (unsigned)(USB_DEVICE_MAX_TRIES - usb_tries));
  }
  else if (usb_device_ok == 0U)
  {
    APP_LOG_Write("usb: giving up on CDC - the PD stack runs without it\r\n");
  }
}

/* Super-loop tick: bounded, non-blocking retry of a CDC that did not come up
   at boot (PWR_CSR2.USB33RDY can still be low while the USB 3.3 V rail is
   rising).  Once the console is up - or the attempts are used up - this is a
   single comparison. */
void USB_DEVICE_Task(void)
{
  if (usb_device_ok != 0U)
  {
    return;
  }
  if (usb_tries >= USB_DEVICE_MAX_TRIES)
  {
    return;
  }
  if ((int32_t)(HAL_GetTick() - usb_next_try_ms) < 0)
  {
    return;
  }
  usb_attempt(1U);
}

/* Console command: the operator has just reseated the cable / waited for the
   rail, so spend another attempt now instead of waiting for the timer.  The
   attempt budget is refreshed, which is what makes "usb retry" useful after
   the automatic attempts have been used up. */
void USB_DEVICE_RetryNow(void)
{
  if (usb_device_ok != 0U)
  {
    return;
  }
  usb_tries = 0U;
  usb_attempt(1U);
}

uint8_t USB_DEVICE_Ready(void)
{
  return usb_device_ok;
}

/* Human-readable one-liner for the console / CLI. */
void USB_DEVICE_Status(void)
{
  if (usb_device_ok != 0U)
  {
    APP_LOG_Printf("usb: CDC console up (try %u, phy=%u)\r\n",
                   (unsigned)usb_tries, (unsigned)usb_phy_ok_last);
  }
  else if (usb_tries >= USB_DEVICE_MAX_TRIES)
  {
    APP_LOG_Printf("usb: CDC down, out of attempts (%u/%u), last failure %s, phy=%u\r\n",
                   (unsigned)usb_tries, (unsigned)USB_DEVICE_MAX_TRIES,
                   usb_fail_name(usb_fail_step), (unsigned)usb_phy_ok_last);
  }
  else
  {
    APP_LOG_Printf("usb: CDC retrying (%u/%u), last failure %s, phy=%u\r\n",
                   (unsigned)usb_tries, (unsigned)USB_DEVICE_MAX_TRIES,
                   usb_fail_name(usb_fail_step), (unsigned)usb_phy_ok_last);
  }
}

/**
  * @}
  */

/**
  * @}
  */

