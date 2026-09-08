/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOM_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(Built_IN_LED_GPIO_Port, Built_IN_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : User_Button_Pin
    PC13 user key, per the WeAct STM32H7R3Zx CoreBoard V1.0 schematic:

        3V3 --- button --- 330 R --- PC13

    There is NO resistor from PC13 to either rail.  Pressed, the pin is
    driven HIGH through 330 R.  Released, it is completely FLOATING - it
    does not fall to 0 V, it leaks down through the pin's own leakage
    current over an unpredictable time.  That is why the key "sort of
    worked but the timing was trash": the release edge arrived tens or
    hundreds of milliseconds late and at random, so the debounce either
    swallowed the press or reported two.

    GPIO_PULLDOWN is therefore REQUIRED, not cosmetic.  The internal
    ~40 k pull-down gives the pin a definite LOW when the button is open,
    and 330 R to 3V3 still wins easily against it when the button closes.
    (This is what the .ioc originally generated; do not "fix" it back to
    GPIO_NOPULL - with NOPULL the pin floats and the key misbehaves.) */
  GPIO_InitStruct.Pin = User_Button_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(User_Button_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : Built_IN_LED_Pin */
  GPIO_InitStruct.Pin = Built_IN_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Built_IN_LED_GPIO_Port, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
