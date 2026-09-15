#ifndef EXT_UART_H
#define EXT_UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "usart.h"

/**
 * @file    ext_uart.h
 * @brief   UART extension footprint (USART2, pins PD5 TX / PD6 RX).
 *
 * ============================================================================
 *  FOOTPRINT FOR FUTURE USART2 FEATURED PROJECTS
 * ============================================================================
 *  USART2 is configured by CubeMX (usart.c, 115200 8N1) and its interrupt is
 *  installed by CubeMX too (USART2_IRQHandler -> HAL_UART_IRQHandler).
 *
 *  In this firmware the port is used as a **second console**: everything the
 *  firmware prints on the USB-HS CDC port is mirrored to USART2, and bytes
 *  arriving on USART2 are fed to the same CLI that reads the CDC port.  Both
 *  consoles work at the same time (see app_log.c / app_cli.c and the
 *  `console` command).  Nothing stops a future feature project from taking
 *  the port over instead - see the hooks below.
 *
 *  To add a feature:
 *    1. Implement EXT_UART_FeatureInit()  - one-time setup after the USART2
 *       peripheral is up.  Called from EXT_UART_Init().
 *    2. Implement EXT_UART_FeaturePoll()  - periodic work from the super
 *       loop.  Called from EXT_UART_Poll().
 *    3. Optionally implement EXT_UART_RxByteReceived() - called from the
 *       USART2 RX interrupt for every byte when the IT receiver is armed.
 *    All three hooks are weak: a future project simply defines strong
 *    versions in its own source files without editing this module or main.c.
 *
 *  Every blocking helper takes a FINITE timeout, mirroring ext_i2c: a dead or
 *  absent peer must never stall the super loop (that would take USB CDC, the
 *  CLI and the PD stack down with it).
 * ============================================================================
 */

/* --- Extension hooks (override to add features) --------------------------- */
void EXT_UART_FeatureInit(void);         /* weak - feature one-time init   */
void EXT_UART_FeaturePoll(void);         /* weak - feature periodic poll   */
void EXT_UART_RxByteReceived(uint8_t b); /* weak - one byte, from the ISR  */

/* --- Footprint API -------------------------------------------------------- */
void    EXT_UART_Init(void);      /* call once after MX_USART2_UART_Init() */
void    EXT_UART_Poll(void);      /* call from the super loop              */
uint8_t EXT_UART_IsReady(void);   /* 1 if huart2 is initialised            */

/* Default timeout (ms) for the helpers below. */
#define EXT_UART_TIMEOUT_MS  25U

/**
 * @brief  Send `len` bytes on USART2 (blocking, finite timeout).
 * @return HAL status (HAL_OK on success).
 */
HAL_StatusTypeDef EXT_UART_Write(const uint8_t *data, uint16_t len);
HAL_StatusTypeDef EXT_UART_WriteTO(const uint8_t *data, uint16_t len,
                                   uint32_t timeout_ms);

/** @brief  printf-style convenience wrapper around EXT_UART_Write. */
int EXT_UART_Printf(const char *fmt, ...);

/**
 * @brief  Read received bytes (non-blocking unless `timeout_ms` > 0).
 *
 * When the interrupt receiver is armed (EXT_UART_ReceiveByteIT) the bytes are
 * taken from the module's RX FIFO, which the USART2 ISR fills.  Reading the
 * data register directly in that mode would steal bytes from the interrupt
 * path, so it is only done when the IT receiver is NOT armed.
 *
 * @param  data  Output buffer.
 * @param  max   Buffer size.
 * @param  timeout_ms  How long to wait for the FIRST byte (0 = pure poll).
 * @return Number of bytes copied into `data` (0 = nothing available).
 */
uint16_t EXT_UART_Read(uint8_t *data, uint16_t max, uint32_t timeout_ms);

/**
 * @brief  Arm 1-byte interrupt reception.  Every received byte is then pushed
 *         into the RX FIFO and delivered to EXT_UART_RxByteReceived() from the
 *         USART2 ISR; the reception re-arms itself automatically (including
 *         after an overrun/framing error).
 * @return HAL status (HAL_OK on success).
 */
HAL_StatusTypeDef EXT_UART_ReceiveByteIT(void);

/** @brief  Stop the interrupt receiver and empty the RX FIFO. */
void EXT_UART_AbortRx(void);

/** @brief  1 while the interrupt receiver is armed. */
uint8_t EXT_UART_IsRxArmed(void);

/** @brief  Bytes waiting in the RX FIFO. */
uint16_t EXT_UART_RxFifoCount(void);

/** @brief  Bytes dropped because the RX FIFO was full (diagnostics). */
uint32_t EXT_UART_RxDropped(void);

/** @brief  Total bytes received through the interrupt path. */
uint32_t EXT_UART_RxCount(void);

/** @brief  Last HAL status returned by the footprint (for `uart status`). */
HAL_StatusTypeDef EXT_UART_LastStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* EXT_UART_H */
