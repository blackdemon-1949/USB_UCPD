#ifndef APP_SD_H
#define APP_SD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ff.h"

/* SD card state */
typedef enum
{
  APP_SD_STATE_NONE = 0,      /* no card detected */
  APP_SD_STATE_MOUNTED,       /* FATFS mounted, ready for I/O */
  APP_SD_STATE_ERROR          /* card present but mount/read failed */
} APP_SD_State_t;

/* Initialize SD card driver (hardware + FatFS link). Call once at boot. */
void APP_SD_Init(void);

/* Poll for card insert/remove and mount/unmount. Call once per super-loop. */
void APP_SD_Poll(void);

/* Get current SD state */
APP_SD_State_t APP_SD_GetState(void);

/* Get FatFS filesystem object (NULL if not mounted).  Do not cache across Poll(). */
FATFS *APP_SD_GetFs(void);

/* Try to mount now (e.g. after manual card insert). Returns 1 on success. */
uint8_t APP_SD_TryMount(void);

/* Unmount and prepare for safe removal. Returns 1 on success. */
uint8_t APP_SD_Unmount(void);

/* Get human-readable status string */
void APP_SD_Status(char *out, uint32_t outsz);

/* Get card info (size, free space) if mounted */
void APP_SD_CardInfo(char *out, uint32_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* APP_SD_H */