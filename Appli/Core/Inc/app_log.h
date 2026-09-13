#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdarg.h>

/**
 * @file  app_log.h
 * @brief Queued console output with two simultaneous sinks: the USB-HS CDC
 *        port and USART2 (PD5/PD6).  See app_log.c.
 *
 * Both sinks drain the same queue independently, so either console - or both
 * at once - shows the full firmware output.  A sink can be muted with
 * APP_LOG_SetUsbMirror() / APP_LOG_SetUartMirror() (`console` command).
 */

void APP_LOG_Init(void);
/** Drain both enabled sinks; call once per super-loop pass. */
void APP_LOG_Flush(void);
int  APP_LOG_Printf(const char *fmt, ...);
void APP_LOG_Write(const char *s);
void APP_LOG_WriteRaw(const uint8_t *data, uint16_t len);

/* --- CDC session state ---------------------------------------------------- */
uint8_t APP_LOG_UsbReady(void);
void APP_LOG_SetUsbReady(uint8_t ready);
void APP_LOG_OnTxComplete(void);
/* USB event hooks (see app_log.c): re-arm the console TX path so a bus
 * reset / suspend / resume cycle can never leave the logger stuck busy. */
void APP_LOG_OnUsbConnect(void);
void APP_LOG_OnUsbSuspend(void);

/* --- sink selection -------------------------------------------------------- */
/** Enable/disable mirroring of the console output to the USB-HS CDC port. */
void APP_LOG_SetUsbMirror(uint8_t on);
uint8_t APP_LOG_UsbMirror(void);
/** Enable/disable mirroring of the console output to USART2 (PD5/PD6). */
void APP_LOG_SetUartMirror(uint8_t on);
uint8_t APP_LOG_UartMirror(void);

/** Bytes dropped because the queue was full or USART2 kept failing. */
uint32_t APP_LOG_Dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
