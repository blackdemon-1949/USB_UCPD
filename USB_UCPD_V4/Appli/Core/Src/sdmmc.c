/* USER CODE BEGIN Header */
  /**
    ******************************************************************************
    * @file    sdmmc.c
    * @brief   This file provides code for the configuration
    *          of the SDMMC instances.
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
  
/* --------------------------------------------------------------------------- */
/*  Compile-time switch (app_config.h).  With APP_SD_ENABLED == 0 the whole
 *  body of this translation unit disappears and the callers fall back to the
 *  body of this translation unit is replaced by empty implementations at the
 *  end of the file, so no SD / FatFs code can end up in the image.  The file
 *  itself stays where STM32CubeIDE expects it.
 */
#include "app_config.h"
#if APP_SD_ENABLED

#include "sdmmc.h"


  /* USER CODE BEGIN 0 */

  /* USER CODE END 0 */

  SD_HandleTypeDef hsd1;

  /* SDMMC1 init function */

  void MX_SDMMC1_SD_Init(void)
  {

    /* USER CODE BEGIN SDMMC1_Init 0 */

    /* USER CODE END SDMMC1_Init 0 */

    /* USER CODE BEGIN SDMMC1_Init 1 */

    /* USER CODE END SDMMC1_Init 1 */
    hsd1.Instance = SDMMC1;
    hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    /* ClockDiv = 2 -> SDMMC_CK = PLL2S / 2.  Target PLL2S ~200MHz -> SDMMC_CK ~100MHz (SDR50).
     * If PLL2S is lower (e.g. 100MHz), SDMMC_CK = 50MHz (SDR25) - still functional.
     * Safe default; APP_SD will fallback to 1-bit if 4-bit fails. */
    hsd1.Init.ClockDiv = 2;
    if (HAL_SD_Init(&hsd1) != HAL_OK)
    {
      /* Non-fatal: card may be absent/corrupt.  Caller (APP_SD) checks f_mount
         result and retries later; Error_Handler() would blink-fault the whole
         PD bench over a missing SD card. */
      return;
    }
    /* USER CODE BEGIN SDMMC1_Init 2 */

    /* USER CODE END SDMMC1_Init 2 */

  }

  /* Try re-init with 1-bit bus width as fallback */
  uint8_t MX_SDMMC1_SD_Init_1Bit(void)
  {
    hsd1.Init.BusWide = SDMMC_BUS_WIDE_1B;
    if (HAL_SD_Init(&hsd1) == HAL_OK)
    {
      return 1U;
    }
    return 0U;
  }

  void HAL_SD_MspInit(SD_HandleTypeDef* sdHandle)
  {

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
    if(sdHandle->Instance==SDMMC1)
    {
    /* USER CODE BEGIN SDMMC1_MspInit 0 */

    /* USER CODE END SDMMC1_MspInit 0 */

    /** Initializes the peripherals clock
    */
      PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_SDMMC12;
      PeriphClkInit.Sdmmc12ClockSelection = RCC_SDMMC12CLKSOURCE_PLL2S;
      if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
      {
        /* Non-fatal: keep PD/USB running if SD clock fails */
        return;
      }

      /* SDMMC1 clock enable */
      __HAL_RCC_SDMMC1_CLK_ENABLE();

      __HAL_RCC_GPIOD_CLK_ENABLE();
      __HAL_RCC_GPIOC_CLK_ENABLE();
      /**SDMMC1 GPIO Configuration
      PD2     ------> SDMMC1_CMD
      PC12     ------> SDMMC1_CK
      PC10     ------> SDMMC1_D2
      PC11     ------> SDMMC1_D3
      PC9     ------> SDMMC1_D1
      PC8     ------> SDMMC1_D0
      */
      /* Use GPIO_PULLUP to drive lines high during init; external 47k pullups
       * on board form a voltage divider but MCU pullup (~40k) dominates.
       * After init, SD card drives DAT lines and CMD becomes bidirectional. */
      GPIO_InitStruct.Pin = GPIO_PIN_2;
      GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
      GPIO_InitStruct.Pull = GPIO_PULLUP;
      GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
      GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC1;
      HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

      GPIO_InitStruct.Pin = GPIO_PIN_12|GPIO_PIN_11|GPIO_PIN_9|GPIO_PIN_8;
      GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
      GPIO_InitStruct.Pull = GPIO_PULLUP;
      GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
      GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC1;
      HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

      GPIO_InitStruct.Pin = GPIO_PIN_10;
      GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
      GPIO_InitStruct.Pull = GPIO_PULLUP;
      GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
      GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
      HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

      /* SDMMC1 interrupt Init
       * SDMMC1 shares GPDMA-unreachable IDMA path (internal), not GPDMA.
       * Priority 5 so it never preempts UCPD(0)/CDC(1)/USART2(2). */
      HAL_NVIC_SetPriority(SDMMC1_IRQn, 5, 0);
      HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
    /* USER CODE BEGIN SDMMC1_MspInit 1 */

    /* USER CODE END SDMMC1_MspInit 1 */
    }
  }

void HAL_SD_MspDeInit(SD_HandleTypeDef* sdHandle)
{

  if(sdHandle->Instance==SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspDeInit 0 */

  /* USER CODE END SDMMC1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SDMMC1_CLK_DISABLE();

    /**SDMMC1 GPIO Configuration
    PD2     ------> SDMMC1_CMD
    PC12     ------> SDMMC1_CK
    PC10     ------> SDMMC1_D2
    PC11     ------> SDMMC1_D3
    PC9     ------> SDMMC1_D1
    PC8     ------> SDMMC1_D0
    */
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);

    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_12|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_9
                          |GPIO_PIN_8);

    /* SDMMC1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
  /* USER CODE BEGIN SDMMC1_MspDeInit 1 */

  /* USER CODE END SDMMC1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

#endif /* APP_SD_ENABLED */
