/**
 * @file    app_log.c
 * @brief   Console output with two simultaneous sinks.
 *
 * One ring buffer, two independent readers:
 *
 *   - the USB-HS CDC port (USB_DEVICE / usbd_cdc_if.c) - the original console
 *   - USART2 (PD5 TX / PD6 RX)                          - the mirrored console
 *
 * Everything the firmware prints is queued once and drained by whichever
 * sinks are enabled, each at its own pace and with its own cursor, so a slow
 * or absent peer on one port can never hold the other one back (after three
 * failed attempts the UART cursor is advanced and the chunk is dropped).
 *
 * Input works the same way in reverse: CDC_Receive_HS and the USART2 RX
 * interrupt both feed the single CLI line editor (app_cli.c), so the MCU can
 * be driven from either port, or from both at the same time.
 */
#include "app_log.h"
#include "usbd_cdc_if.h"
#include "usb_device.h"
#include "ext_uart.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define LOG_Q_SIZE       2048U
/* Bytes handed to USART2 per super-loop pass.  At 115200 8N1 a 64-byte chunk
 * takes ~5.6 ms on the wire, which is the longest the loop is held. */
#define LOG_UART_CHUNK   64U
/* Consecutive UART transmit failures tolerated before the chunk is dropped
 * (otherwise a dead USART2 would fill the ring and starve the CDC sink). */
#define LOG_UART_MAXFAIL 3U

static uint8_t  s_q[LOG_Q_SIZE];
static uint16_t s_head;
static uint16_t s_tail_usb;
static uint16_t s_tail_uart;
static uint8_t  s_usb_ready;
static uint8_t  s_usb_mirror  = 1U;
static uint8_t  s_uart_mirror = 1U;
static uint8_t  s_uart_fails;
static uint32_t s_dropped;
static volatile uint8_t s_tx_busy;
/* CDC IN DMA reads this buffer; it must not be held in a stale D-cache line. */
static uint8_t  s_tx[256]
  __attribute__((section("noncacheable_buffer"), aligned(32)));
/* USART2 is CPU driven (no DMA), so this one may live in normal cacheable RAM. */
static uint8_t  s_tx_uart[LOG_UART_CHUNK];

extern USBD_HandleTypeDef hUsbDeviceHS;

uint8_t APP_LOG_UsbReady(void)
{
  return s_usb_ready;
}

void APP_LOG_SetUsbReady(uint8_t ready)
{
  s_usb_ready = ready;
}

void APP_LOG_OnTxComplete(void)
{
  s_tx_busy = 0U;
}

void APP_LOG_OnUsbConnect(void)
{
  /* Called from CDC_Init_HS on every SET_CONFIGURATION (i.e. on every
   * enumeration).  A bus reset / unplug can interrupt an IN transfer, in
   * which case TransmitCplt never fires and s_tx_busy would stay 1 for
   * ever: the console would go silent until the next replug.  Re-arm it
   * here so a re-enumerated port always prints again. */
  s_tx_busy = 0U;
}

void APP_LOG_OnUsbSuspend(void)
{
  /* Bus suspended: drop the session and re-arm the TX state. */
  s_usb_ready = 0U;
  s_tx_busy = 0U;
}

void APP_LOG_Init(void)
{
  s_head = 0;
  s_tail_usb = 0;
  s_tail_uart = 0;
  s_usb_ready = 0;
  s_tx_busy = 0;
  s_uart_fails = 0;
  s_dropped = 0;
}

void APP_LOG_SetUsbMirror(uint8_t on)
{
  s_usb_mirror = on ? 1U : 0U;
  if (s_usb_mirror == 0U)
  {
    s_tail_usb = s_head;   /* do not hold the ring back while muted */
  }
}

uint8_t APP_LOG_UsbMirror(void)
{
  return s_usb_mirror;
}

void APP_LOG_SetUartMirror(uint8_t on)
{
  s_uart_mirror = on ? 1U : 0U;
  if (s_uart_mirror == 0U)
  {
    s_tail_uart = s_head;
  }
}

uint8_t APP_LOG_UartMirror(void)
{
  return s_uart_mirror;
}

uint32_t APP_LOG_Dropped(void)
{
  return s_dropped;
}

/* Bytes queued for one reader. */
static uint16_t q_count(uint16_t tail)
{
  if (s_head >= tail)
  {
    return (uint16_t)(s_head - tail);
  }
  return (uint16_t)(LOG_Q_SIZE - tail + s_head);
}

/* The reader furthest behind decides how much free space is left. */
static uint16_t q_count_slowest(void)
{
  uint16_t cu = q_count(s_tail_usb);
  uint16_t cs = q_count(s_tail_uart);
  return (cu >= cs) ? cu : cs;
}

void APP_LOG_WriteRaw(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++)
  {
    uint16_t next;
    if (q_count_slowest() >= (LOG_Q_SIZE - 1U))
    {
      s_dropped++;
      break;
    }
    next = (uint16_t)((s_head + 1U) % LOG_Q_SIZE);
    s_q[s_head] = data[i];
    s_head = next;
  }
}

void APP_LOG_Write(const char *s)
{
  if (s == NULL)
  {
    return;
  }
  APP_LOG_WriteRaw((const uint8_t *)s, (uint16_t)strlen(s));
}

int APP_LOG_Printf(const char *fmt, ...)
{
  char buf[256];
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
  APP_LOG_WriteRaw((const uint8_t *)buf, (uint16_t)n);
  return n;
}

/* --- sink 1: USB-HS CDC ---------------------------------------------------- */
static void log_flush_usb(void)
{
  uint16_t n;
  uint16_t i;

  if ((s_usb_mirror == 0U) || (s_usb_ready == 0U))
  {
    return;
  }
  if (hUsbDeviceHS.dev_state != USBD_STATE_CONFIGURED)
  {
    return;
  }
  if (s_tx_busy != 0U)
  {
    return;
  }
  n = q_count(s_tail_usb);
  if (n == 0U)
  {
    return;
  }
  if (n > sizeof(s_tx))
  {
    n = (uint16_t)sizeof(s_tx);
  }
  /* Copy without committing: the cursor only moves once the stack has
   * accepted the transfer, so a BUSY return does not silently lose the
   * chunk (it is retried on the next pass). */
  for (i = 0; i < n; i++)
  {
    s_tx[i] = s_q[(uint16_t)((s_tail_usb + i) % LOG_Q_SIZE)];
  }
  s_tx_busy = 1U;
  if (CDC_Transmit_HS(s_tx, n) == USBD_OK)
  {
    s_tail_usb = (uint16_t)((s_tail_usb + n) % LOG_Q_SIZE);
  }
  else
  {
    s_tx_busy = 0U;
  }
}

/* --- sink 2: USART2 -------------------------------------------------------- */
static void log_flush_uart(void)
{
  uint16_t n;
  uint16_t i;

  if ((s_uart_mirror == 0U) || (EXT_UART_IsReady() == 0U))
  {
    return;
  }
  n = q_count(s_tail_uart);
  if (n == 0U)
  {
    return;
  }
  if (n > LOG_UART_CHUNK)
  {
    n = (uint16_t)LOG_UART_CHUNK;
  }
  for (i = 0; i < n; i++)
  {
    s_tx_uart[i] = s_q[(uint16_t)((s_tail_uart + i) % LOG_Q_SIZE)];
  }
  if (EXT_UART_Write(s_tx_uart, n) == HAL_OK)
  {
    s_uart_fails = 0U;
    s_tail_uart = (uint16_t)((s_tail_uart + n) % LOG_Q_SIZE);
    return;
  }
  if (++s_uart_fails >= LOG_UART_MAXFAIL)
  {
    /* USART2 is not getting the data out.  Drop the chunk instead of letting
     * the stuck reader fill the ring and stall the CDC console too. */
    s_uart_fails = 0U;
    s_dropped += n;
    s_tail_uart = (uint16_t)((s_tail_uart + n) % LOG_Q_SIZE);
  }
}

void APP_LOG_Flush(void)
{
  /* A sink that cannot consume must never pin the ring buffer.  Its cursor
   * staying behind makes q_count_slowest() reach LOG_Q_SIZE-1, after which
   * APP_LOG_WriteRaw() refuses to queue anything at all - so one inactive
   * sink would silence BOTH consoles permanently.
   *
   * Muting is the obvious case, but a CDC port with no host attached (or not
   * yet configured) cannot drain either, and log_flush_usb() returns early
   * without moving s_tail_usb.  Discard the backlog for such a sink: for a
   * console, old output nobody received is not worth stalling the live one. */
  if ((s_usb_mirror == 0U) || (s_usb_ready == 0U) ||
      (hUsbDeviceHS.dev_state != USBD_STATE_CONFIGURED))
  {
    s_tail_usb = s_head;
  }
  if (s_uart_mirror == 0U)
  {
    s_tail_uart = s_head;
  }

  log_flush_usb();
  log_flush_uart();
}
