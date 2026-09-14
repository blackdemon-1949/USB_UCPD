/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "dts.h"
#include "fatfs.h"
#include "gpdma.h"
#include "i2c.h"
#include "ucpd.h"
#include "usart.h"
#include "usbpd.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_config.h"
#include "usb_device.h"
#include "app_log.h"
#include "app_fault.h"
#include "app_cmos.h"
#include "app_wdt.h"
#include "app_store.h"
#include "app_cmd.h"
#include "app_cli.h"
#include "app_pd.h"
#include "app_board.h"
#include "apie.h"
#include "apie_learn.h"
#include "app_learn_store.h"
#include "apie_sdlog.h"
#include "ext_nor.h"
#include "ext_i2c.h"
#include "ext_uart.h"
#include "ext_dts.h"
#include "irq_priority.h"
#include "app_oled.h"
#include "app_profile.h"
#include "app_sd.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint8_t s_store_skipped;   /* NOR left alone this boot */
static uint8_t s_pd_up;
static uint32_t s_boot_ms;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Update SystemCoreClock variable according to RCC registers values. */
  SystemCoreClockUpdate();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* Restate the NVIC grouping instead of relying on the HAL_Init()
     default, so the levels in irq_priority.h really are the effective
     pre-emption priorities.  Must run before any peripheral enables an
     interrupt. */
  HAL_NVIC_SetPriorityGrouping(IRQ_PRIORITY_GROUP);

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* -------------------------------------------------------------------------
   * Boot order (deliberate): console first, USB-PD second, everything optional
   * last.
   *
   * The board must reach the console even when a peripheral is missing or
   * slow.  Nothing below the console initialisation may block, and no init is
   * allowed to call Error_Handler() any more (app_fault.c turns those paths
   * into a logged, recorded failure that the rest of the firmware ignores).
   * ---------------------------------------------------------------------- */

  /* ---- 1. board + consoles -------------------------------------------------*/
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  s_boot_ms = HAL_GetTick();
  APP_LOG_Init();
  APP_FAULT_Init();          /* from here on failures are logged, not fatal */
  APP_CLI_Init();
  APP_LED_Set(APP_LED_HEARTBEAT);

  /* ---- 2. settings + crash diagnostics from the VBAT-backed CMOS ---------- */
  APP_CMOS_Init();
  {
    uint32_t extra = 0U;
    uint32_t code = APP_CMOS_TakeFaultMark(&extra);
    if (code != APP_FAULT_NONE)
    {
      APP_LOG_Printf("[boot] previous boot ended in fault %lu (extra=0x%08lX)\r\n",
                     (unsigned long)code, (unsigned long)extra);
    }
  }
  APP_WDT_ReportReset();
  APP_WDT_Init();
  APP_CMOS_PrintFaults();

  /* ---- 3. USB CDC console: never behind a slow peripheral ----------------- */
  MX_USB_DEVICE_Init();

  /* ---- 4. USB-PD ----------------------------------------------------------- */
  if (APP_WDT_InSafeMode() == 0U)
  {
    MX_UCPD1_Init();
    MX_USBPD_Init();
    s_pd_up = 1U;
  }
  else
  {
    APP_LOG_Write("[boot] safe mode: USB-PD is not started ('safe off' clears it)\r\n");
  }

  /* ---- 5. optional peripherals (each one self-disabling, none fatal) ------ */
  MX_I2C2_Init();
  EXT_I2C_Init();
  EXT_UART_Init();
  /* The DTS runs from the LSE, which the bootloader does not start: bring the
     32.768 kHz reference up first (ext_dts.c retries every second), and treat a
     DTS failure as "no temperature readout", never as a fatal error. */
  EXT_DTS_Init();
  MX_DTS_Init();
  APP_OLED_Init();
  APP_PROFILE_Init();

#if APP_SD_ENABLED
  /* ---- 5b. SD card + FatFs (compile-time optional, see app_config.h) ------ */
  MX_FATFS_Init();
  APP_SD_Init();
  APIE_SdLog_Init();
#endif

  /* ---- 6. persistent store on the external NOR + learn/ML ---------------- */
  APIE_Init();
  APIE_Learn_Init();
#if APP_STORE_ENABLED
  /* If the previous boot died while the XiP window was switched off, the flash
     is the last thing we want to touch again before the operator has looked at
     it: the console is up by now, so skipping the store costs nothing and the
     board still boots.  'store on' (or 'store selftest') re-arms it. */
  if (APP_FAULT_LastWasFlashRisk() != 0U)
  {
    s_store_skipped = 1U;
    APP_LOG_Write("[boot] store skipped: the last boot faulted in flash mode\r\n"
                  "       (run 'store selftest' to re-try the NOR, 'store on' to arm writes)\r\n");
  }
  else if (EXT_NOR_Init() != 0U)
  {
    (void)APP_STORE_Init();
    APP_CMD_Init();
    (void)APIE_Learn_LoadStore();
  }
  else
  {
    APP_LOG_Printf("[boot] NOR store unavailable (%s) - settings come from the CMOS only\r\n",
                   EXT_NOR_Status());
  }
#endif
  /* USER CODE END 2 */

  APP_LOG_Printf("[boot] ready in %lums (pd=%u, store=%u, safe=%u, init-fails=%u,"
                 " store-skipped=%u)\r\n",
                 (unsigned long)(HAL_GetTick() - s_boot_ms),
                 (unsigned)s_pd_up,
                 (unsigned)APP_STORE_Ready(),
                 (unsigned)APP_WDT_InSafeMode(),
                 (unsigned)APP_FAULT_InitFailCount(),
                 (unsigned)s_store_skipped);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    APP_WDT_Kick();               /* the only place the watchdog is fed */

    /* Deferred CDC bring-up: bounded retries if the USB rail was still
       rising at boot (see usb_device.c).  Non-blocking, self-limiting. */
    USB_DEVICE_Task();

    if (s_pd_up != 0U)
    {
      USBPD_DPM_Run();
      APP_PD_Task();
    }
    APIE_Task();
    APIE_Learn_PeriodicTask();
#if APP_SD_ENABLED
    APIE_SdLog_PeriodicTask();
#endif

    /* Peripheral extension polling (see ext_i2c.c / ext_uart.c / ext_dts.c) */
    EXT_I2C_Poll();
    EXT_UART_Poll();
    EXT_DTS_Poll();
#if APP_SD_ENABLED
    APP_SD_Poll();
#endif

    /* OLED page: pushes at most one I2C chunk per pass and returns. */
    APP_OLED_Poll();

    /* USER CODE BEGIN 3 */
    APP_CLI_Poll();
    APP_LOG_Flush();
    APP_LED_Task();
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /* Disables all MPU regions */
  for(uint8_t i=0; i<__MPU_REGIONCOUNT; i++)
  {
    HAL_MPU_DisableRegion(i);
  }

  /** Region 0: 4 GB background, no access (subregions 0,1,2,7 disabled)
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 1: 8 MB XSPI1 NOR (PY25Q64HA) — XiP code + const
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 2: AXI SRAM (cacheable)
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 3: DTCM (always non-cacheable on M7, tightly coupled)
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x20000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_64KB;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 4: USB/CDC DMA buffers (8 KiB @ 0x2406E000, non-cacheable AXI SRAM)
   *
   * The linker places the CDC RX/TX and log TX buffers here.  MPU regions
   * have priority by number, so this region overrides the cacheable AXI
   * SRAM region above.  Without this override the USB DMA and CM7 cache can
   * observe different contents, causing enumeration and transfers to fail.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress = 0x2406E000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8KB;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Region 5: Backup SRAM (4 KiB @ 0x38800000, non-cacheable) - APIE store
   *  + the boot CMOS.
   */
  MPU_InitStruct.Number = MPU_REGION_NUMBER5;
  MPU_InitStruct.BaseAddress = 0x38800000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4KB;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* Never spin here: record the reason where it survives a reset and reboot.
     The watchdog + safe-mode logic in app_wdt.c makes sure a deterministic
     failure can not turn into an invisible boot loop. */
  APP_FATAL(APP_FAULT_ERROR_HANDLER);
  /* USER CODE END Error_Handler_Debug */
}

/** Public wrapper so middleware (usbpd.c) can report a fatal init error with a
 *  visible LED code + CMOS breadcrumb instead of hanging silently in while(1). */
void Appli_Fatal(uint8_t code)
{
  APP_FATAL(code);
}

#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  UNUSED(file);
  UNUSED(line);
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
