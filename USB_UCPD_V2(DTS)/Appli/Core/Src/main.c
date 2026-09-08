/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : PD Bench application — USB CDC CLI + UCPD sink (XiP)
  *
  * Runs from external NOR at 0x90000000 after the 64 KB internal bootloader
  * memory-maps Puya PY25Q64HA. Clocks are already up (Boot SystemClock_Config).
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dts.h"
#include "gpdma.h"
#include "i2c.h"
#include "ucpd.h"
#include "usart.h"
#include "usbpd.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usb_device.h"
#include "app_log.h"
#include "app_cli.h"
#include "app_pd.h"
#include "app_board.h"
#include "apie.h"
#include "ext_i2c.h"
#include "ext_uart.h"
#include "ext_dts.h"
#include "irq_priority.h"
#include "app_oled.h"
#include "app_profile.h"
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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
static void Appli_Fail(uint8_t code);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  Fatal appli error: blink PB2 `code` times, pause, repeat.
  *         6 = USB init, 7 = generic Error_Handler
  */
static void Appli_Fail(uint8_t code)
{
  __disable_irq();
  while (1)
  {
    for (uint8_t i = 0; i < code; i++)
    {
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_SET);
      for (volatile uint32_t d = 0; d < 400000UL; d++) { __NOP(); }
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
      for (volatile uint32_t d = 0; d < 400000UL; d++) { __NOP(); }
    }
    for (volatile uint32_t d = 0; d < 1600000UL; d++) { __NOP(); }
  }
}

/** Public wrapper so middleware (usbpd.c) can report a fatal init error
 *  with a visible LED code instead of hanging silently in while(1). */
void Appli_Fatal(uint8_t code)
{
  Appli_Fail(code);
}

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

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_UCPD1_Init();
  MX_I2C2_Init();
  MX_DTS_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  APP_LOG_Init();
  APP_CLI_Init();
  APP_PROFILE_Init();
  APP_LED_Set(APP_LED_HEARTBEAT);

  /* Peripheral extension footprints (see ext_i2c.c / ext_uart.c / ext_dts.c) */
  EXT_I2C_Init();
  EXT_UART_Init();
  EXT_DTS_Init();

  /* 0.96" 128x64 I2C OLED page on I2C2, shared with the INA226.
   * Additive and self-disabling: if nothing answers at 0x3C every entry
   * point becomes a no-op and the rest of the firmware never notices.
   * Must run after EXT_I2C_Init() so I2C2 is already configured. */
  APP_OLED_Init();

  MX_USB_DEVICE_Init();

  /* Advanced PD Intelligence Engine (observation + learning + policy). */
  APIE_Init();
  /* USER CODE END 2 */

  /* USBPD initialisation ---------------------------------*/
  MX_USBPD_Init();

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    USBPD_DPM_Run();
    APP_PD_Task();
    APIE_Task();

    /* Peripheral extension polling (see ext_i2c.c / ext_uart.c / ext_dts.c) */
    EXT_I2C_Poll();
    EXT_UART_Poll();
    EXT_DTS_Poll();

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

  /** Region 4: USB/CDC DMA buffers (8 KiB, non-cacheable AXI SRAM)
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
  Appli_Fail(7);
  /* USER CODE END Error_Handler_Debug */
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
