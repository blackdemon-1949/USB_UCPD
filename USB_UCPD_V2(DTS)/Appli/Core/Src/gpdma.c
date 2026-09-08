/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpdma.c
  * @brief   This file provides code for the configuration
  *          of the GPDMA instances.
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
#include "gpdma.h"
#include "irq_priority.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* GPDMA1 init function */
void MX_GPDMA1_Init(void)
{

  /* USER CODE BEGIN GPDMA1_Init 0 */

  /* USER CODE END GPDMA1_Init 0 */

  /* Peripheral clock enable */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* GPDMA1 interrupt Init
   * Channels 0/1 are the UCPD1 RX/TX DMA (serviced by the UCPD1 ISR, the
   * channel IRQs themselves stay masked).  They are part of the UCPD data
   * path, so they take the UCPD level: the highest in the system. */
    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, IRQ_PRIO_UCPD_DMA, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, IRQ_PRIO_UCPD_DMA, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);

  /* USER CODE BEGIN GPDMA1_Init 1 */
  /* USBPD trace TX (tracer_emb over USART1 DMA) uses GPDMA1 Channel 2.
     The .ioc does not model this channel, so a plain CubeMX regeneration
     would drop the NVIC setup below.  It lives in this USER CODE section
     on purpose: regeneration keeps it.
     HW_TRACER_EMB_Init() re-applies TRACER_EMB_TX_DMA_PRIORITY to this IRQ
     later; both use IRQ_PRIO_TRACE_DMA, so the value does not change. */
    HAL_NVIC_SetPriority(GPDMA1_Channel2_IRQn, IRQ_PRIO_TRACE_DMA, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
  /* USER CODE END GPDMA1_Init 1 */
  /* USER CODE BEGIN GPDMA1_Init 2 */

  /* USER CODE END GPDMA1_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

