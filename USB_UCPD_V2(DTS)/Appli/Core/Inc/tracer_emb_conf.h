/**
  ******************************************************************************
  * @file    tracer_emb_conf.h
  * @author  MCD Application Team (H7RS template), adapted for WeAct H7R3Z8
  * @brief   Embedded USB-PD tracer (TRACER_EMB) hardware configuration.
  *
  * This file selects the UART + DMA channel used to stream the USB-PD trace
  * (TLV framed, TRACER_EMB protocol) to STM32CubeMonitor-UCPD.
  *
  * Wiring on the WeAct STM32H7R3Z8J6 board (both pins are on the pin header):
  *
  *     PA9  = USART1_TX  ->  USB-UART adapter RX
  *     PA10 = USART1_RX  ->  USB-UART adapter TX
  *     GND  = GND        ->  USB-UART adapter GND   (3.3 V logic!)
  *
  * (USART3's PD8/PD9 was NOT usable: this 144-pin part has no PD9 pin.)
  * Then in STM32CubeMonitor-UCPD add a board node, select the TRACER
  * interface and choose the adapter's COM port (921600 baud, 8N1).
  *
  * Alternative combos already routed on the header, if PA9/PA10 are needed
  * for something else:
  *   - USART2 : PA2 (TX) / PA3 (RX), AF7, APB1 clock
  *   - USART3 : PB10 (TX) / PB11 (RX), AF7, APB1 clock
  *   - USART3 : PD8 (TX) only - its RX (PD9) does not exist on H7R3Z8
  ******************************************************************************
  */
#ifndef __TRACER_EMB_CONF_H
#define __TRACER_EMB_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7rsxx_ll_bus.h"
#include "stm32h7rsxx_ll_dma.h"
#include "stm32h7rsxx_ll_gpio.h"
#include "stm32h7rsxx_ll_rcc.h"
#include "stm32h7rsxx_ll_usart.h"
/* Private typedef -----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* -----------------------------------------------------------------------------
      Definitions for TRACE feature
-------------------------------------------------------------------------------*/

#define TRACER_EMB_BAUDRATE                          921600UL

#define TRACER_EMB_DMA_MODE                          1UL
#define TRACER_EMB_IT_MODE                           0UL

#define TRACER_EMB_BUFFER_SIZE                       1024UL

/* -----------------------------------------------------------------------------
      Definitions for TRACE Hw information
-------------------------------------------------------------------------------*/

#define TRACER_EMB_IS_INSTANCE_LPUART_TYPE           0UL /* set to 0UL if USART is used */

#define TRACER_EMB_USART_INSTANCE                    USART1

#define TRACER_EMB_TX_GPIO                           GPIOA
#define TRACER_EMB_TX_PIN                            LL_GPIO_PIN_9
#define TRACER_EMB_TX_AF                             LL_GPIO_AF_7
#define TRACER_EMB_TX_GPIO_ENABLE_CLOCK()            LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOA)
#define TRACER_EMB_RX_GPIO                           GPIOA
#define TRACER_EMB_RX_PIN                            LL_GPIO_PIN_10
#define TRACER_EMB_RX_AF                             LL_GPIO_AF_7
#define TRACER_EMB_RX_GPIO_ENABLE_CLOCK()            LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOA)

#define TRACER_EMB_ENABLE_CLK_USART()                LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1)
#define TRACER_EMB_DISABLE_CLK_USART()               LL_APB2_GRP1_DisableClock(LL_APB2_GRP1_PERIPH_USART1)
#define TRACER_EMB_SET_CLK_SOURCE_USART()            /* USART1 kernel clock defaults to pclk2 (150 MHz) */
#define TRACER_EMB_USART_IRQ                         USART1_IRQn
#define TRACER_EMB_USART_IRQHANDLER                  USART1_IRQHandler
/* USART1 trace IRQ priority.  6 = just below USB OTG (4) and UCPD (5):
 * tracing must never delay USB enumeration or PD message handling. */
#define TRACER_EMB_TX_IRQ_PRIORITY                   6

#define TRACER_EMB_TX_AF_FUNCTION                    LL_GPIO_SetAFPin_8_15
#define TRACER_EMB_RX_AF_FUNCTION                    LL_GPIO_SetAFPin_8_15

#define TRACER_EMB_DMA_INSTANCE                      GPDMA1
#define TRACER_EMB_ENABLE_CLK_DMA()                  do {                                                       \
                                                       LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);      \
                                                       } while(0)
#define TRACER_EMB_TX_DMA_REQUEST                    LL_GPDMA1_REQUEST_USART1_TX
#define TRACER_EMB_TX_DMA_CHANNEL                    LL_DMA_CHANNEL_2
#define TRACER_EMB_ENABLECHANNEL                     LL_DMA_ResumeChannel
#define TRACER_EMB_DISABLECHANNEL                    LL_DMA_SuspendChannel
#define TRACER_EMB_TX_DMA_IRQ                        GPDMA1_Channel2_IRQn
#define TRACER_EMB_TX_DMA_IRQHANDLER                 GPDMA1_Channel2_IRQHandler
#define TRACER_EMB_TX_DMA_ACTIVE_FLAG(_DMA_)         LL_DMA_IsActiveFlag_TC(_DMA_, TRACER_EMB_TX_DMA_CHANNEL)
#define TRACER_EMB_TX_DMA_CLEAR_FLAG(_DMA_)          LL_DMA_ClearFlag_TC(_DMA_, TRACER_EMB_TX_DMA_CHANNEL)

#define TRACER_EMB_STRUCTURE_MEMORY_LOCATION         "noncacheable_buffer"

#ifdef __cplusplus
}
#endif

#endif /* __TRACER_EMB_CONF_H */
