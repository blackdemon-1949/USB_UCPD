/**
 * @file    ext_uart.c
 * @brief   UART extension footprint - USART2 (PD5 TX / PD6 RX, 115200 8N1).
 *
 * USART2 is configured by CubeMX in usart.c (MX_USART2_UART_Init) and its
 * interrupt is serviced by USART2_IRQHandler in stm32h7rsxx_it.c.  In this
 * firmware the port carries a second copy of the console (see app_log.c and
 * app_cli.c).
 *
 * Confirmed on hardware: both this port and the USB-HS CDC console stream
 * output and accept commands at the same time.
 */
#include "ext_uart.h"
#include "app_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ==========================================================================
 *  FEATURE REGISTRATION
 *  --------------------------------------------------------------------------
 *  Future USART2 feature projects override these weak hooks with their own
 *  strong definitions (no edits needed here or in main.c):
 *
 *    void EXT_UART_FeatureInit(void)  - one-time setup, called from
 *                                       EXT_UART_Init() after MX_USART2_UART_Init().
 *    void EXT_UART_FeaturePoll(void)  - periodic work, called from
 *                                       EXT_UART_Poll() every super-loop pass.
 *    void EXT_UART_RxByteReceived(uint8_t b)
 *                                     - one received byte, called from the
 *                                       USART2 interrupt (keep it short).
 *
 *  The console bridge in app_cli.c is itself such a feature: it overrides
 *  EXT_UART_FeatureInit (to arm the interrupt receiver) and
 *  EXT_UART_RxByteReceived (to feed the CLI line editor).
 * ========================================================================== */

__weak void EXT_UART_FeatureInit(void)
{
  /* Add future USART2 feature initialisation here (or override this hook). */
}

__weak void EXT_UART_FeaturePoll(void)
{
  /* Add future USART2 feature polling here (or override this hook). */
}

__weak void EXT_UART_RxByteReceived(uint8_t b)
{
  (void)b;
  /* Add future USART2 RX handling here (or override this hook).
   * Runs in interrupt context: keep it short and never block. */
}

/* ==========================================================================
 *  RX FIFO
 *  --------------------------------------------------------------------------
 *  Filled by the USART2 ISR, drained by the super loop.  One producer, one
 *  consumer, 16-bit aligned indices: no critical section needed on Cortex-M.
 * ========================================================================== */

#define EXT_UART_RX_FIFO  256U

static volatile uint8_t  s_fifo[EXT_UART_RX_FIFO];
static volatile uint16_t s_fifo_head;
static volatile uint16_t s_fifo_tail;
static volatile uint32_t s_rx_count;
static volatile uint32_t s_rx_dropped;

/* ==========================================================================
 *  FOOTPRINT API
 * ========================================================================== */

static HAL_StatusTypeDef s_last = HAL_OK;
static uint8_t           s_rx_byte;
static uint8_t           s_it_armed;

static uint16_t fifo_count(void)
{
  uint16_t h = s_fifo_head;
  uint16_t t = s_fifo_tail;
  return (h >= t) ? (uint16_t)(h - t)
                  : (uint16_t)(EXT_UART_RX_FIFO - t + h);
}

void EXT_UART_Init(void)
{
  /* The peripheral itself is brought up by MX_USART2_UART_Init() in main.c.
   * This hook is the application-level extension point that runs after it. */
  if (huart2.Instance != USART2)
  {
    APP_LOG_Printf("ext-uart: ERROR USART2 not initialised\r\n");
    s_last = HAL_ERROR;
    return;
  }

  APP_LOG_Printf("ext-uart: USART2 ready  TX=PD5 RX=PD6  %lu 8N1\r\n",
                 (unsigned long)huart2.Init.BaudRate);

  /* Feature hook: the console bridge arms the interrupt receiver here. */
  EXT_UART_FeatureInit();
}

void EXT_UART_Poll(void)
{
  /* Self-healing receiver.  HAL_UART_ErrorCallback re-arms after a line error,
   * but if that re-arm itself returns HAL_BUSY the HAL is left with no
   * reception in progress, RXNE interrupts stop arriving and the port stays
   * deaf until a reset.  Watching RxState from the super loop closes that
   * hole: READY means nothing is armed, so arm it again. */
  if ((s_it_armed != 0U) && EXT_UART_IsReady() &&
      (huart2.RxState == HAL_UART_STATE_READY))
  {
    if (HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U) == HAL_OK)
    {
      s_last = HAL_OK;
    }
  }

  /* Feature hook: future USART2 projects add their periodic work here.
   * Runs on every super-loop pass; keep it short (non-blocking or
   * time-gated) so the PD stack, USB CDC and CLI stay responsive. */
  EXT_UART_FeaturePoll();
}

uint8_t EXT_UART_IsReady(void)
{
  return (huart2.Instance == USART2) ? 1U : 0U;
}

HAL_StatusTypeDef EXT_UART_LastStatus(void)
{
  return s_last;
}

HAL_StatusTypeDef EXT_UART_WriteTO(const uint8_t *data, uint16_t len,
                                   uint32_t timeout_ms)
{
  if (!EXT_UART_IsReady() || (data == NULL) || (len == 0U))
  {
    return HAL_ERROR;
  }
  s_last = HAL_UART_Transmit(&huart2, (uint8_t *)data, len, timeout_ms);
  return s_last;
}

HAL_StatusTypeDef EXT_UART_Write(const uint8_t *data, uint16_t len)
{
  /* Timeout scales with the payload so a long line is never truncated just
   * because the default window was sized for a short one. */
  uint32_t kbps = huart2.Init.BaudRate / 1000U;
  uint32_t need;
  uint32_t to;

  if (kbps == 0U)
  {
    kbps = 1U;
  }
  need = ((uint32_t)len * 10U) / kbps + 5U;
  to = (need > EXT_UART_TIMEOUT_MS) ? need : EXT_UART_TIMEOUT_MS;
  return EXT_UART_WriteTO(data, len, to);
}

int EXT_UART_Printf(const char *fmt, ...)
{
  char buf[192];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
  {
    return n;
  }
  if (n >= (int)sizeof(buf))
  {
    n = (int)sizeof(buf) - 1;
  }
  (void)EXT_UART_Write((const uint8_t *)buf, (uint16_t)n);
  return n;
}

uint16_t EXT_UART_Read(uint8_t *data, uint16_t max, uint32_t timeout_ms)
{
  uint16_t got = 0U;
  uint32_t start = HAL_GetTick();

  if (!EXT_UART_IsReady() || (data == NULL) || (max == 0U))
  {
    return 0U;
  }

  if (s_it_armed != 0U)
  {
    /* Interrupt reception owns the data register: drain the FIFO instead. */
    while (got < max)
    {
      if (s_fifo_tail != s_fifo_head)
      {
        data[got++] = s_fifo[s_fifo_tail];
        s_fifo_tail = (uint16_t)((s_fifo_tail + 1U) % EXT_UART_RX_FIFO);
        continue;
      }
      if ((HAL_GetTick() - start) >= timeout_ms)
      {
        break;
      }
    }
    return got;
  }

  while (got < max)
  {
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) == 0U)
    {
      if ((HAL_GetTick() - start) >= timeout_ms)
      {
        break;
      }
      continue;
    }
    data[got++] = (uint8_t)(huart2.Instance->RDR & 0xFFU);
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) == 0U)
    {
      break;
    }
  }
  return got;
}

HAL_StatusTypeDef EXT_UART_ReceiveByteIT(void)
{
  if (!EXT_UART_IsReady())
  {
    return HAL_ERROR;
  }
  s_last = HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U);
  if (s_last == HAL_OK)
  {
    s_it_armed = 1U;
  }
  return s_last;
}

void EXT_UART_AbortRx(void)
{
  if (EXT_UART_IsReady())
  {
    (void)HAL_UART_AbortReceive(&huart2);
  }
  s_it_armed = 0U;
  s_fifo_head = 0U;
  s_fifo_tail = 0U;
}

uint8_t EXT_UART_IsRxArmed(void)
{
  return s_it_armed;
}

uint16_t EXT_UART_RxFifoCount(void)
{
  return fifo_count();
}

uint32_t EXT_UART_RxDropped(void)
{
  return s_rx_dropped;
}

uint32_t EXT_UART_RxCount(void)
{
  return s_rx_count;
}

/* --------------------------------------------------------------------------
 *  HAL callbacks for the interrupt reception.  Both are weak in the HAL;
 *  USART2 is the only UART instance in this firmware, but the guards keep
 *  this safe if another instance is added later.
 * ------------------------------------------------------------------------ */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    uint8_t  b    = s_rx_byte;
    uint16_t next = (uint16_t)((s_fifo_head + 1U) % EXT_UART_RX_FIFO);

    /* Re-arm first so back-to-back bytes are not dropped. */
    (void)HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U);

    if (next == s_fifo_tail)
    {
      s_rx_dropped++;          /* nobody drained the FIFO in time */
    }
    else
    {
      s_fifo[s_fifo_head] = b;
      s_fifo_head = next;
      s_rx_count++;
    }

    EXT_UART_RxByteReceived(b);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    s_last = HAL_ERROR;
    /* Overrun / framing / noise errors abort the reception: re-arm it so a
     * single glitch on the line cannot kill the port for good. */
    if (s_it_armed != 0U)
    {
      if (HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U) != HAL_OK)
      {
        (void)HAL_UART_AbortReceive(&huart2);
        s_last = HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U);
      }
    }
  }
}
