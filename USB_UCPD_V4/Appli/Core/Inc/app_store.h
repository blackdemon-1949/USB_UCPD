/**
  ******************************************************************************
  * @file    app_store.h
  * @brief   Persistent store on the external NOR (replaces the removed SD card).
  *
  * Layout of the reserved 1 MB window (EXT_NOR_STORE_OFF .. +1 MB):
  *
  *   sector 0      superblock   { magic 'EXST', version, sectors, crc32 }
  *   sector 1..255 append journal, written round-robin with 4 KB sector erase
  *
  * A journal record is
  *
  *   { magic 'REC1', seq, type, len, crc32(payload), pad } + payload
  *
  * so any torn or corrupt write is rejected on its own; the newest valid
  * record of a type wins, which makes the store a crash-safe key/value map
  * plus a persistent event log.  Everything is optional: when the store is not
  * usable the firmware keeps running from the backup-SRAM settings only.
  ******************************************************************************
  */
#ifndef APP_STORE_H
#define APP_STORE_H

#include <stdint.h>

/* Record types ------------------------------------------------------------- */
#define APP_STORE_T_SETTINGS  1U    /* settings mirror (backup for the CMOS)   */
#define APP_STORE_T_LEARN     2U    /* learned signatures / fingerprints       */
#define APP_STORE_T_ML        3U    /* decision-model state                    */
#define APP_STORE_T_PROFILE   4U    /* owner profile list                      */
#define APP_STORE_T_EVENT     5U    /* event log entry (text)                  */
#define APP_STORE_T_PACKET    6U    /* PD packet log entry (binary)            */
#define APP_STORE_T_LEARN_CSV 7U    /* signature table as CSV (export)  */
#define APP_STORE_T_CMD_BASE  0x40U /* 0x40..0x7F: user command macros         */
#define APP_STORE_T_LEARN_CHUNK0 0x80U /* 0x80..0x8F: learned-signature chunks  */
#define APP_STORE_T_LEARN_CHUNKS 16U
#define APP_STORE_CMD_SLOTS   64U

#define APP_STORE_MAX_PAYLOAD 1024U

/* Init: read the superblock, honour the cached head, rescan when in doubt.
 * Returns 1 when the store is mounted (empty counts as mounted). */
uint8_t APP_STORE_Init(void);
uint8_t APP_STORE_Ready(void);
uint8_t APP_STORE_Formatted(void);

int APP_STORE_Write(uint8_t type, const void *data, uint32_t len);
int APP_STORE_Read(uint8_t type, void *buf, uint32_t max, uint32_t *out_len);
int APP_STORE_Delete(uint8_t type);
int APP_STORE_Format(void);

/* Event log: appends one text record, rate-limited, and always mirrors the
 * last entries into RAM so 'log show' works even while the flash is busy. */
void APP_STORE_Event(const char *fmt, ...);
void APP_STORE_LogShow(uint32_t count);

/* Statistics for the console. */
void APP_STORE_PrintStatus(void);

#endif /* APP_STORE_H */
