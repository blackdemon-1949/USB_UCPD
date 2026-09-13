/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           Target/usbd_conf.c
  * @author         MCD Application Team
  * @brief          This file implements the board support package for the USB device library
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
#include "stm32h7rsxx.h"
#include "usbd_def.h"
#include "stm32h7rsxx_hal.h"
#include "usbd_core.h"
#include "usbd_cdc.h"

/* USER CODE BEGIN Includes */
#include "stm32h7rsxx_ll_rcc.h"
#include "app_log.h"
#include "irq_priority.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* 1 = the USB PHYC clock mux and the VDD33USB voltage detector are confirmed
 *     programmed and read back correctly (see HAL_PCD_MspInit).
 * 0 = at least one of them failed or did not take effect.  In that state the
 *     PHY has no valid 48 MHz, so USBD_LL_Init() refuses to hand the core to
 *     the stack and the D+ pull-up is never asserted.  Windows seeing a device
 *     that is attached but answers GET_DESCRIPTOR with garbage is exactly how
 *     "device descriptor request failed" / Code 10 is produced, and it used to
 *     be produced by an *unconditional* connect here.
 *
 *     Reported by USBD_LL_UsbClockReady() and printed by the `info` command. */
static uint8_t s_usb_clock_ok = 1U;

/* USER CODE END PV */

PCD_HandleTypeDef hpcd_USB_OTG_HS;

/* External functions --------------------------------------------------------*/

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* Private function prototypes -----------------------------------------------*/
USBD_StatusTypeDef USBD_Get_USB_Status(HAL_StatusTypeDef hal_status);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private functions ---------------------------------------------------------*/

/* USER CODE BEGIN 1 */
/* USER CODE END 1 */

/*******************************************************************************
                       LL Driver Callbacks (PCD -> USB Device Library)
*******************************************************************************/
/* MSP Init */

void HAL_PCD_MspInit(PCD_HandleTypeDef* pcdHandle)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(pcdHandle->Instance==USB_OTG_HS)
  {
  /* USER CODE BEGIN USB_OTG_HS_MspInit 0 */
    /* USB PHY reference clock selection.
     *
     * NOTE (correction to an earlier note in this file): the old comment
     * blamed "SYSCFG_OTG_HS_PHY_CLK_SELECT_4".  There is no such thing on
     * STM32H7RS - this family has no SYSCFG USB-PHY clock mux at all (no
     * SYSCFG->OTG_HS_PHY_CTRL).  The USB HS PHY is fed from exactly two
     * fields of RCC->CCIPR1:
     *
     *   USBPHYCSEL  - which clock drives the PHY.  Here: HSE (0), 24 MHz.
     *   USBREFCKSEL - a 4-bit code telling the PHY what reference frequency
     *                 to expect, so it can generate 48 MHz from it.
     *                 24 MHz is LL_RCC_USBREF_CLKSOURCE_24M ==
     *                 (RCC_CCIPR1_USBREFCKSEL_3 | RCC_CCIPR1_USBREFCKSEL_1) ==
     *                 0xA.  (See Drivers/.../Inc/stm32h7rsxx_ll_rcc.h.)
     *
     * CubeMX does not emit USB_OTG_HS.RefClockSelection for STM32H7RS, so
     * USBREFCKSEL stayed at its reset value and the PHY produced no valid
     * 48 MHz; enumeration then failed or decoded as garbage.  Set it
     * explicitly here, and read both fields back below.
     *
     * Powering the PHY additionally needs PWR->CSR2.USBHSREGEN (VDD33USB
     * regulator, enabled in HAL_MspInit) and USB33DEN (voltage detector,
     * enabled both there and below). */
    LL_RCC_SetUSBREFClockSource(LL_RCC_USBREF_CLKSOURCE_24M);
  /* USER CODE END USB_OTG_HS_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USBPHYC;
    PeriphClkInit.UsbPhycClockSelection = RCC_USBPHYCCLKSOURCE_HSE;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      /* Do NOT call Error_Handler() here: it never returns and would brick
         the whole PD bench over a USB clock problem, and continuing into
         HAL_PCD_Init() would touch an unclocked USB_OTG_HS.  Record the
         failure; USBD_LL_Init() checks it before going any further. */
      s_usb_clock_ok = 0U;
      return;
    }

  /** Enable USB Voltage detector
  */
    if (HAL_PWREx_EnableUSBVoltageDetector() != HAL_OK)
    {
      /* Same reasoning: no Error_Handler(), and no connect without VDD33USB
         being reported ready (PWR_CSR2_USB33RDY). */
      s_usb_clock_ok = 0U;
      return;
    }

    /* Peripheral clock enable */
    __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
    __HAL_RCC_USBPHYC_CLK_ENABLE();

    /* Read both muxes back.  If either is not what we just programmed, the
       PHY has no valid 48 MHz and the device must not be allowed to assert
       the D+ pull-up.  (This is also the read-back a bench test can use:
       `info` prints the same two fields.) */
    if ((LL_RCC_GetUSBREFClockSource(LL_RCC_USBREF_CLKSOURCE) != LL_RCC_USBREF_CLKSOURCE_24M) ||
        (__HAL_RCC_GET_USBPHYC_SOURCE() != RCC_USBPHYCCLKSOURCE_HSE))
    {
      s_usb_clock_ok = 0U;
    }

    /* Peripheral interrupt init */
    /* CDC (USB OTG_HS) is second only to UCPD: enumeration and the control
       endpoint are timing critical, and a late GET_DESCRIPTOR response is
       exactly what Windows reports as "device descriptor request failed" /
       Code 10.  The level comes from irq_priority.h (IRQ_PRIO_CDC_USB). */
    HAL_NVIC_SetPriority(OTG_HS_IRQn, IRQ_PRIO_CDC_USB, 0);
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
  /* USER CODE BEGIN USB_OTG_HS_MspInit 1 */

  /* USER CODE END USB_OTG_HS_MspInit 1 */
  }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcdHandle)
{
  if(pcdHandle->Instance==USB_OTG_HS)
  {
  /* USER CODE BEGIN USB_OTG_HS_MspDeInit 0 */

  /* USER CODE END USB_OTG_HS_MspDeInit 0 */
    /* Disable Peripheral clock */
    __HAL_RCC_USB_OTG_HS_CLK_DISABLE();
    __HAL_RCC_USBPHYC_CLK_DISABLE();

    /* Peripheral interrupt Deinit*/
    HAL_NVIC_DisableIRQ(OTG_HS_IRQn);

  /* USER CODE BEGIN USB_OTG_HS_MspDeInit 1 */

  /* USER CODE END USB_OTG_HS_MspDeInit 1 */
  }
}

/**
  * @brief  Setup stage callback
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_SetupStageCallback_PreTreatment */

  /* USER CODE END HAL_PCD_SetupStageCallback_PreTreatment */
  USBD_LL_SetupStage((USBD_HandleTypeDef*)hpcd->pData, (uint8_t *)hpcd->Setup);
  /* USER CODE BEGIN HAL_PCD_SetupStageCallback_PostTreatment */

  /* USER CODE END HAL_PCD_SetupStageCallback_PostTreatment */

}

/**
  * @brief  Data Out stage callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_DataOutStageCallback_PreTreatment */

  /* USER CODE END HAL_PCD_DataOutStageCallback_PreTreatment */
  USBD_LL_DataOutStage((USBD_HandleTypeDef*)hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff);
  /* USER CODE BEGIN HAL_PCD_DataOutStageCallback_PostTreatment */

  /* USER CODE END HAL_PCD_DataOutStageCallback_PostTreatment */
}

/**
  * @brief  Data In stage callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_DataInStageCallback_PreTreatment */

  /* USER CODE END HAL_PCD_DataInStageCallback_PreTreatment */
  USBD_LL_DataInStage((USBD_HandleTypeDef*)hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff);
  /* USER CODE BEGIN HAL_PCD_DataInStageCallback_PostTreatment */

  /* USER CODE END HAL_PCD_DataInStageCallback_PostTreatment */
}

/**
  * @brief  SOF callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_SofCallback_PreTreatment */

  /* USER CODE END HAL_PCD_SofCallback_PreTreatment */
  USBD_LL_SOF((USBD_HandleTypeDef*)hpcd->pData);
  /* USER CODE BEGIN HAL_PCD_SofCallback_PostTreatment */

  /* USER CODE END HAL_PCD_SofCallback_PostTreatment */
}

/**
  * @brief  Reset callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ResetCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ResetCallback_PreTreatment */
  USBD_SpeedTypeDef speed = USBD_SPEED_FULL;

  if (hpcd->Init.speed == PCD_SPEED_HIGH)
  {
    speed = USBD_SPEED_HIGH;
  }
  else if (hpcd->Init.speed == PCD_SPEED_FULL)
  {
    speed = USBD_SPEED_FULL;
  }
  else
  {
    /* Garbage enum speed (USB_GetDevSpeed can return 0xF, e.g. when the
       PHY reference clock is marginal).  Running Error_Handler() from the
       USB IRQ used to brick the whole board (solid PB2, dead PD stack).
       Fall back to full-speed descriptors instead and keep running. */
    speed = USBD_SPEED_FULL;
  }
    /* Set Speed. */
  USBD_LL_SetSpeed((USBD_HandleTypeDef*)hpcd->pData, speed);

  /* Reset Device. */
  USBD_LL_Reset((USBD_HandleTypeDef*)hpcd->pData);
  /* USER CODE BEGIN HAL_PCD_ResetCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ResetCallback_PostTreatment */
}

/**
  * @brief  Suspend callback.
  * When Low power mode is enabled the debug cannot be used (IAR, Keil doesn't support it)
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_SuspendCallback_PreTreatment */

  /* USER CODE END HAL_PCD_SuspendCallback_PreTreatment */

  /* Inform USB library that core enters in suspend Mode. */
  USBD_LL_Suspend((USBD_HandleTypeDef*)hpcd->pData);
  /* Drop the console session; an IN transfer aborted by the suspend can
     never complete, which would otherwise leave the logger stuck busy. */
  APP_LOG_OnUsbSuspend();
  /* Enter in STOP mode. */
  /* USER CODE BEGIN 2 */
  if (hpcd->Init.low_power_enable)
  {
	HAL_SuspendTick();
    /* Set SLEEPDEEP bit and SleepOnExit of Cortex System Control Register. */
    SCB->SCR |= (uint32_t)((uint32_t)(SCB_SCR_SLEEPDEEP_Msk | SCB_SCR_SLEEPONEXIT_Msk));
  }
  /* USER CODE END 2 */
  /* USER CODE BEGIN HAL_PCD_SuspendCallback_PostTreatment */

  /* USER CODE END HAL_PCD_SuspendCallback_PostTreatment */
}

/**
  * @brief  Resume callback.
  * When Low power mode is enabled the debug cannot be used (IAR, Keil doesn't support it)
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ResumeCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ResumeCallback_PreTreatment */

  /* USER CODE BEGIN 3 */
  if (hpcd->Init.low_power_enable)
  {
    HAL_ResumeTick();
    /* Reset SLEEPDEEP bit of Cortex System Control Register. */
    SCB->SCR &= (uint32_t)~((uint32_t)(SCB_SCR_SLEEPDEEP_Msk | SCB_SCR_SLEEPONEXIT_Msk));

  }
  /* USER CODE END 3 */
  __HAL_PCD_UNGATE_PHYCLOCK(hpcd);
  USBD_LL_Resume((USBD_HandleTypeDef*)hpcd->pData);

  /* USER CODE BEGIN HAL_PCD_ResumeCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ResumeCallback_PostTreatment */
}

/**
  * @brief  ISOOUTIncomplete callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ISOOUTIncompleteCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ISOOUTIncompleteCallback_PreTreatment */
  USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef*)hpcd->pData, epnum);
  /* USER CODE BEGIN HAL_PCD_ISOOUTIncompleteCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ISOOUTIncompleteCallback_PostTreatment */

}

/**
  * @brief  ISOINIncomplete callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ISOINIncompleteCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ISOINIncompleteCallback_PreTreatment */
  USBD_LL_IsoINIncomplete((USBD_HandleTypeDef*)hpcd->pData, epnum);

  /* USER CODE BEGIN HAL_PCD_ISOINIncompleteCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ISOINIncompleteCallback_PostTreatment */
}

/**
  * @brief  Connect callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ConnectCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ConnectCallback_PreTreatment */
  USBD_LL_DevConnected((USBD_HandleTypeDef*)hpcd->pData);
  /* USER CODE BEGIN HAL_PCD_ConnectCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ConnectCallback_PostTreatment */
}

/**
  * @brief  Disconnect callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_DisconnectCallback_PreTreatment */

  /* USER CODE END HAL_PCD_DisconnectCallback_PreTreatment */
  USBD_LL_DevDisconnected((USBD_HandleTypeDef*)hpcd->pData);
  /* USER CODE BEGIN HAL_PCD_DisconnectCallback_PostTreatment */

  /* USER CODE END HAL_PCD_DisconnectCallback_PostTreatment */
}

/* USER CODE BEGIN LowLevelInterface */

/* USER CODE END LowLevelInterface */

/*******************************************************************************
                       LL Driver Interface (USB Device Library --> PCD)
*******************************************************************************/

/**
  * @brief  Initializes the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
  /* Init USB Ip. */
  if (pdev->id == DEVICE_HS) {
  /* Link the driver to the stack. */
  hpcd_USB_OTG_HS.pData = pdev;
  pdev->pData = &hpcd_USB_OTG_HS;

  hpcd_USB_OTG_HS.Instance = USB_OTG_HS;
  hpcd_USB_OTG_HS.Init.dev_endpoints = 9;
  hpcd_USB_OTG_HS.Init.speed = PCD_SPEED_HIGH;
  hpcd_USB_OTG_HS.Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY;
  hpcd_USB_OTG_HS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.Sof_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.use_dedicated_ep1 = DISABLE;

  /* VBUS sensing stays DISABLED on purpose - do not "fix" this to ENABLE.
   *
   * The board has no VBUS-sense pin wired to the OTG controller.  Enabling
   * vbus_sensing_enable would make the core gate its session state on
   * GCCFG.VBUSBSEN / VBUSASEN reading a pin that is not connected, so the
   * behaviour would be undefined (a floating or grounded input reads as
   * "no VBUS", and the device can then fail to connect at all).
   *
   * The consequence - the device asserts the D+ pull-up whether or not VBUS
   * is there - is acceptable here, because the board is either powered from
   * VBUS itself (in which case VBUS is de facto present whenever the firmware
   * is running) or powered externally with the USB cable attached.  Supply
   * readiness is checked through PWR_CSR2.USB33RDY instead: see the
   * s_usb_clock_ok gate in HAL_PCD_MspInit() / USBD_LL_Init() /
   * USBD_LL_Start(). */
  hpcd_USB_OTG_HS.Init.vbus_sensing_enable = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_HS) != HAL_OK)
  {
    /* Do NOT call Error_Handler() here: it never returns (LED code 7) and
       would brick the whole PD bench over a USB problem.  USBD_Init()
       propagates this to MX_USB_DEVICE_Init(), which logs the failure and
       keeps the sink running without the serial console. */
    return USBD_FAIL;
  }

  /* Clock / PHY readiness gate (set in HAL_PCD_MspInit).  HAL_PCD_MspInit
     runs inside HAL_PCD_Init(), so it can be evaluated now.  If the USBPHYC
     mux or the VDD33USB detector did not come up, do not program the FIFOs
     and do not let USBD_Start() assert the D+ pull-up: a device that is
     attached but cannot answer GET_DESCRIPTOR is precisely what Windows
     reports as "device descriptor request failed" / Code 10, and it is the
     failure mode that a half-initialised PHY produces. */
  if (s_usb_clock_ok == 0U)
  {
    return USBD_FAIL;
  }

#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
  /* Register USB PCD CallBacks */
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_SOF_CB_ID, PCD_SOFCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_SETUPSTAGE_CB_ID, PCD_SetupStageCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_RESET_CB_ID, PCD_ResetCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_SUSPEND_CB_ID, PCD_SuspendCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_RESUME_CB_ID, PCD_ResumeCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_CONNECT_CB_ID, PCD_ConnectCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_HS, HAL_PCD_DISCONNECT_CB_ID, PCD_DisconnectCallback);

  HAL_PCD_RegisterDataOutStageCallback(&hpcd_USB_OTG_HS, PCD_DataOutStageCallback);
  HAL_PCD_RegisterDataInStageCallback(&hpcd_USB_OTG_HS, PCD_DataInStageCallback);
  HAL_PCD_RegisterIsoOutIncpltCallback(&hpcd_USB_OTG_HS, PCD_ISOOUTIncompleteCallback);
  HAL_PCD_RegisterIsoInIncpltCallback(&hpcd_USB_OTG_HS, PCD_ISOINIncompleteCallback);
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
  /* USER CODE BEGIN USB_HS_FIFO_Configuration */
  HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_HS, 0x200);
  HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 0, 0x40);
  HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 1, 0x80);

  /* TX FIFO 2 serves IN endpoint 2, which the CDC class opens as its
   * command / notification endpoint (CDC_CMD_EP == 0x82, opened in
   * usbd_cdc.c USBD_CDC_Init).  Without this call DIEPTXF[1] keeps its reset
   * value 0, i.e. depth 0 at FIFO offset 0 - which *aliases the RX FIFO*
   * (the RX FIFO occupies words 0..0x200).  Any activity on the notification
   * endpoint then reads and writes receive-buffer RAM, corrupting whatever
   * the host last sent.  The HAL states the rule explicitly in
   * HAL_PCDEx_SetTxFiFo(): an unused TX FIFO must still be given the 16-word
   * minimum so the following FIFO starts at the right address.
   *
   * Budget: 0x200 + 0x40 + 0x80 + 0x40 = 0x300 words = 3072 of the 4096
   * bytes of OTG_HS FIFO RAM. */
  HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 2, 0x40);
  /* USER CODE END USB_HS_FIFO_Configuration */
  }
  return USBD_OK;
}

/**
  * @brief  Report whether the USB clock / PHY readiness gate passed.
  * @param  None
  * @retval 1 = USBPHYC mux and VDD33USB detector verified, 0 = not.
  * @note   Read by the `info` command.  When this returns 0, USBD_LL_Init()
  *         refused to start the device, so no pull-up was ever asserted.
  */
uint8_t USBD_LL_UsbClockReady(void)
{
  return s_usb_clock_ok;
}

/**
  * @brief  De-Initializes the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_DeInit(pdev->pData);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Starts the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  /* Final barrier before HAL_PCD_Start() clears DCTL.SFTDISCON and asserts
     the D+ pull-up, checked as late as it is possible to check it.
     *
     * The board can be powered from VBUS, so on a cold plug the whole 3.3 V
     * domain - including the USB HS supply - is still ramping while the CPU is
     * already executing: the MCU comes out of reset as soon as VDD crosses the
     * POR threshold, which is well before VDD33USB is stable.  Connecting
     * during that window is how an attached-but-incapable device (and hence
     * "device descriptor request failed" / Code 10, or a fresh broken COM
     * port) is produced.  PWR_CSR2.USB33RDY is the only supply-ready signal
     * available without a VBUS-sense pin: it is driven by the USB 3.3 V
     * detector and only asserts once that domain is up.  On a warm reset the
     * domain is already charged and USB33RDY is already set, which is why the
     * fault clears itself on the next reset.
     *
     * Refusing to start keeps the PD sink and the USART2 console alive; the
     * failure is logged by MX_USB_DEVICE_Init() and reported by `info`
     * (usb clock gate = 0). */
  if ((s_usb_clock_ok == 0U) || ((PWR->CSR2 & PWR_CSR2_USB33RDY) == 0U))
  {
    s_usb_clock_ok = 0U;
    return USBD_FAIL;
  }

  hal_status = HAL_PCD_Start(pdev->pData);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Stops the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_Stop(pdev->pData);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Opens an endpoint of the low level driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @param  ep_type: Endpoint type
  * @param  ep_mps: Endpoint max packet size
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t ep_type, uint16_t ep_mps)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Closes an endpoint of the low level driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Close(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Flushes an endpoint of the Low Level Driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Flush(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Sets a Stall condition on an endpoint of the Low Level Driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_SetStall(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Clears a Stall condition on an endpoint of the Low Level Driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_ClrStall(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Returns Stall condition.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval Stall (1: Yes, 0: No)
  */
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef*) pdev->pData;

  if((ep_addr & 0x80) == 0x80)
  {
    return hpcd->IN_ep[ep_addr & 0x7F].is_stall;
  }
  else
  {
    return hpcd->OUT_ep[ep_addr & 0x7F].is_stall;
  }
}

/**
  * @brief  Assigns a USB address to the device.
  * @param  pdev: Device handle
  * @param  dev_addr: Device address
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_SetAddress(pdev->pData, dev_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Transmits data over an endpoint.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @param  pbuf: Pointer to data to be sent
  * @param  size: Data size
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  /* USER CODE BEGIN USBD_LL_Transmit_ZlpErratum 0 */

  /* ES0596 (STM32H7Rxx/7Sxx device errata) erratum 2.21.3: "Potential
   * unexpected transfer on the USB bus instead of a zero-length packet".
   *
   * In BOTH buffer-DMA and slave mode, when a zero-length packet must be
   * transmitted, the controller can instead put a *data* packet on the bus.
   * Every control transfer whose status stage is an IN ZLP (SET_ADDRESS,
   * SET_CONFIGURATION, SET_LINE_CODING, ...) is exposed, and a host that
   * receives garbage where it expects a 0-length status packet fails the
   * whole control transfer - which is precisely the "device descriptor
   * request failed" / broken-COM-port failure Windows 11 reports
   * intermittently on this board.  See USB_ENUMERATION.md.
   *
   * The ST-documented workaround applies to "all IN transfers that involve
   * a zero-length packet transmission in device mode (DIEPTSIZx.XFRSIZ=0
   * and DIEPTSIZx.PKTCNT=1)": start the endpoint NAK-ed, hold it for a
   * fixed safe delay of 15 AHB clock cycles, then release the NAK.  The
   * ST HAL (stm32h7rsxx_ll_usb.c USB_EPStartXfer) does not implement it:
   * it writes DIEPCTL |= (CNAK | EPENA) directly.  This porting layer
   * therefore takes over ONLY the zero-length IN case, keeping the PCD
   * handle bookkeeping identical to HAL_PCD_EP_Transmit so the interrupt
   * path is unchanged:
   *   - xfer_buff/xfer_len/xfer_count/is_in/num are set exactly as the HAL
   *     sets them (for a ZLP nothing is pushed to the FIFO, so no
   *     DIEPEMPMSK is needed - matching the HAL).
   *   - DIEPTSIZ is programmed exactly as the HAL's ZLP branch does
   *     (XFRSIZ = 0, PKTCNT = 1), only the DIEPCTL release order differs.
   * If the erratum analysis is wrong the sequence still degenerates to
   * exactly what the HAL would have done, ~200 ns later - it cannot make
   * the transfer path worse than stock.
   *
   * Compile gate: set USBD_H7RS_ZLP_ERRATUM_WA to 0 to get the stock HAL
   * behaviour back for A/B testing on the bench. */
#if !defined(USBD_H7RS_ZLP_ERRATUM_WA)
#define USBD_H7RS_ZLP_ERRATUM_WA 1
#endif
#if USBD_H7RS_ZLP_ERRATUM_WA
  if ((pdev->pData != NULL) &&
      ((((PCD_HandleTypeDef *)pdev->pData)->Init.dma_enable == 0U)) && /* slave mode only */
      ((ep_addr & 0x80U) != 0U) && (size == 0U))                        /* IN ZLP */
  {
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;
    uint32_t USBx_BASE = (uint32_t)hpcd->Instance;
    USB_OTG_INEndpointTypeDef *inep = (USB_OTG_INEndpointTypeDef *)(
        USBx_BASE + USB_OTG_IN_ENDPOINT_BASE +
        (((uint32_t)(ep_addr & EP_ADDR_MSK)) * USB_OTG_EP_REG_SIZE));
    PCD_EPTypeDef *ep = &hpcd->IN_ep[ep_addr & EP_ADDR_MSK];

    /* Same handle bookkeeping HAL_PCD_EP_Transmit performs. */
    ep->xfer_buff = pbuf;
    ep->xfer_len = 0U;
    ep->xfer_count = 0U;
    ep->is_in = 1U;
    ep->num = (uint8_t)(ep_addr & EP_ADDR_MSK);

    /* Same DIEPTSIZ programming as USB_EPStartXfer()'s ZLP branch. */
    inep->DIEPTSIZ &= ~(USB_OTG_DIEPTSIZ_XFRSIZ | USB_OTG_DIEPTSIZ_PKTCNT);
    inep->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_PKTCNT & (1UL << 19));

    /* Erratum workaround ordering: enable the endpoint but hold it NAK-ed
       (CNAK = 0), wait, then release. */
    inep->DIEPCTL |= (USB_OTG_DIEPCTL_EPENA | USB_OTG_DIEPCTL_SNAK);
    {
      /* >= 15 AHB clock cycles.  The OTG AHB clock is HCLK (300 MHz here)
         while the CPU runs at 600 MHz, so 15 AHB cycles = 30 CPU cycles;
         this loop is deliberately far above that (each iteration is at
         least one cycle plus loop overhead, and a volatile access). */
      volatile uint32_t d;
      for (d = 0U; d < 64U; d++) { __NOP(); }
    }
    inep->DIEPCTL |= USB_OTG_DIEPCTL_CNAK;   /* releases the NAK; ZLP goes out */
    return USBD_OK;
  }
#endif /* USBD_H7RS_ZLP_ERRATUM_WA */

  /* USER CODE END USBD_LL_Transmit_ZlpErratum 0 */

  hal_status = HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Report whether the ES0596 2.21.3 ZLP erratum workaround is active.
  * @param  None
  * @retval 1 = workaround compiled in, 0 = stock HAL behaviour.
  * @note   Read by the `info` command.
  */
uint8_t USBD_LL_ZlpWaActive(void)
{
#if USBD_H7RS_ZLP_ERRATUM_WA
  return 1U;
#else
  return 0U;
#endif
}

/**
  * @brief  Prepares an endpoint for reception.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @param  pbuf: Pointer to data to be received
  * @param  size: Data size
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Returns the last transferred packet size.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval Received Data Size
  */
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef*) pdev->pData, ep_addr);
}

#ifdef USBD_HS_TESTMODE_ENABLE
/**
  * @brief  Set High speed Test mode.
  * @param  pdev: Device handle
  * @param  testmode: test mode
  * @retval USBD Status
  */
USBD_StatusTypeDef USBD_LL_SetTestMode(USBD_HandleTypeDef *pdev, uint8_t testmode)
{
   HAL_PCD_SetTestMode(pdev->pData, testmode);

  return USBD_OK;
}
#endif /* USBD_HS_TESTMODE_ENABLE */
/**
  * @brief  Static single allocation.
  * @param  size: Size of allocated memory
  * @retval None
  */
void *USBD_static_malloc(uint32_t size)
{
  UNUSED(size);
  static uint32_t mem[(sizeof(USBD_CDC_HandleTypeDef)/4)+1];/* On 32-bit boundary */
  return mem;
}

/**
  * @brief  Dummy memory free
  * @param  p: Pointer to allocated  memory address
  * @retval None
  */
void USBD_static_free(void *p)
{
  UNUSED(p);
}

/**
  * @brief  Delays routine for the USB device library.
  * @param  Delay: Delay in ms
  * @retval None
  */
void USBD_LL_Delay(uint32_t Delay)
{
  HAL_Delay(Delay);
}

/**
  * @brief  Returns the USB status depending on the HAL status:
  * @param  hal_status: HAL status
  * @retval USB status
  */
USBD_StatusTypeDef USBD_Get_USB_Status(HAL_StatusTypeDef hal_status)
{
  USBD_StatusTypeDef usb_status = USBD_OK;

  switch (hal_status)
  {
    case HAL_OK :
      usb_status = USBD_OK;
    break;
    case HAL_ERROR :
      usb_status = USBD_FAIL;
    break;
    case HAL_BUSY :
      usb_status = USBD_BUSY;
    break;
    case HAL_TIMEOUT :
      usb_status = USBD_FAIL;
    break;
    default :
      usb_status = USBD_FAIL;
    break;
  }
  return usb_status;
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */
