
/* --------------------------------------------------------------------------- */
/*  Compile-time switch (app_config.h).  With APP_SD_ENABLED == 0 the whole
 *  body of this translation unit disappears and the callers fall back to the
 *  body of this translation unit is replaced by empty implementations at the
 *  end of the file, so no SD / FatFs code can end up in the image.  The file
 *  itself stays where STM32CubeIDE expects it.
 */
#include "app_config.h"
#if APP_SD_ENABLED

#include "app_sd.h"

#include "sdmmc.h"
#include "fatfs.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>

/* Card detect on PA8 (active low when inserted, with internal pull-up) */
#define SD_CD_PORT        GPIOA
#define SD_CD_PIN         GPIO_PIN_8
#define SD_CD_CLK_ENABLE  __HAL_RCC_GPIOA_CLK_ENABLE

/* Debounce parameters */
#define SD_DEBOUNCE_MS    100U
#define SD_POLL_INTERVAL  500U

/* FatFS linked driver tag */
#define SD_DISK_TAG       "0:"

/* Internal state */
static APP_SD_State_t s_state = APP_SD_STATE_NONE;
static FATFS s_fs;
static uint8_t s_fs_mounted = 0U;
static uint32_t s_last_poll_ms = 0U;
static uint32_t s_cd_debounce_ms = 0U;
static uint8_t s_cd_last_raw = 1U;  /* 1 = no card (pull-up) */
static uint8_t s_cd_stable = 1U;
static uint8_t s_init_retry = 0U;
static uint8_t s_using_1bit = 0U;

static void gpio_cd_init(void)
{
  GPIO_InitTypeDef g = {0};
  SD_CD_CLK_ENABLE();
  g.Pin = SD_CD_PIN;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_PULLUP;  /* Internal pull-up since no external pullup on board */
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SD_CD_PORT, &g);
}

static uint8_t read_cd_raw(void)
{
  return (HAL_GPIO_ReadPin(SD_CD_PORT, SD_CD_PIN) == GPIO_PIN_RESET) ? 0U : 1U;
}

static void check_mount(void)
{
  FRESULT fr;
  if (s_fs_mounted)
  {
    return;
  }
  if (s_cd_stable == 0U)
  {
    if (!s_using_1bit)
    {
      MX_SDMMC1_SD_Init();
    }
    else
    {
      MX_SDMMC1_SD_Init_1Bit();
    }
    fr = f_mount(&s_fs, SD_DISK_TAG, 1U);
    if (fr == FR_OK)
    {
      s_fs_mounted = 1U;
      s_state = APP_SD_STATE_MOUNTED;
      s_init_retry = 0U;
      APP_LOG_Printf("sd: mounted (%s-bit)\r\n", s_using_1bit ? "1" : "4");
    }
    else
    {
      s_init_retry++;
      if (s_init_retry >= 3 && !s_using_1bit)
      {
        /* Fallback to 1-bit mode */
        s_using_1bit = 1U;
        s_init_retry = 0U;
        APP_LOG_Write("sd: 4-bit mount failed, retrying 1-bit...\r\n");
        check_mount();  /* Retry immediately with 1-bit */
        return;
      }
      s_state = APP_SD_STATE_ERROR;
      APP_LOG_Printf("sd: mount failed (%d)\r\n", (int)fr);
    }
  }
}

static void check_unmount(void)
{
  if (!s_fs_mounted)
  {
    return;
  }
  if (s_cd_stable == 1U)
  {
    f_mount(NULL, SD_DISK_TAG, 0U);
    s_fs_mounted = 0U;
    s_state = APP_SD_STATE_NONE;
    s_using_1bit = 0U;
    s_init_retry = 0U;
    APP_LOG_Write("sd: unmounted\r\n");
  }
}

void APP_SD_Init(void)
{
  gpio_cd_init();
  MX_FATFS_Init();
  s_last_poll_ms = HAL_GetTick();
  s_cd_last_raw = read_cd_raw();
  s_cd_stable = s_cd_last_raw;
  s_cd_debounce_ms = HAL_GetTick();
  s_state = (s_cd_stable == 0U) ? APP_SD_STATE_MOUNTED : APP_SD_STATE_NONE;
  if (s_cd_stable == 0U)
  {
    check_mount();
  }
}

void APP_SD_Poll(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t cd_raw;

  if ((now - s_last_poll_ms) < SD_POLL_INTERVAL)
  {
    return;
  }
  s_last_poll_ms = now;

  cd_raw = read_cd_raw();
  if (cd_raw != s_cd_last_raw)
  {
    s_cd_last_raw = cd_raw;
    s_cd_debounce_ms = now;
  }
  else if ((now - s_cd_debounce_ms) >= SD_DEBOUNCE_MS)
  {
    if (cd_raw != s_cd_stable)
    {
      s_cd_stable = cd_raw;
      if (s_cd_stable == 0U)
      {
        APP_LOG_Write("sd: card inserted\r\n");
        check_mount();
      }
      else
      {
        check_unmount();
      }
    }
  }
}

APP_SD_State_t APP_SD_GetState(void)
{
  return s_state;
}

FATFS *APP_SD_GetFs(void)
{
  return (s_fs_mounted && s_state == APP_SD_STATE_MOUNTED) ? &s_fs : NULL;
}

uint8_t APP_SD_TryMount(void)
{
  if (s_cd_stable != 0U)
  {
    return 0U;
  }
  check_mount();
  return (s_state == APP_SD_STATE_MOUNTED) ? 1U : 0U;
}

uint8_t APP_SD_Unmount(void)
{
  if (!s_fs_mounted)
  {
    return 1U;
  }
  f_mount(NULL, SD_DISK_TAG, 0U);
  s_fs_mounted = 0U;
  s_state = APP_SD_STATE_NONE;
  s_using_1bit = 0U;
  s_init_retry = 0U;
  APP_LOG_Write("sd: unmounted (manual)\r\n");
  return 1U;
}

void APP_SD_Status(char *out, uint32_t outsz)
{
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  switch (s_state)
  {
    case APP_SD_STATE_NONE:
      snprintf(out, outsz, "sd: no card (PA8 high)");
      break;
    case APP_SD_STATE_MOUNTED:
      snprintf(out, outsz, "sd: mounted, ready (%s-bit)", s_using_1bit ? "1" : "4");
      break;
    case APP_SD_STATE_ERROR:
      snprintf(out, outsz, "sd: card present, mount failed (retries=%u)", (unsigned)s_init_retry);
      break;
    default:
      snprintf(out, outsz, "sd: unknown state");
      break;
  }
}

void APP_SD_CardInfo(char *out, uint32_t outsz)
{
  FATFS *fs;
  DWORD free_clust;
  uint64_t total, free;

  if (out == NULL || outsz == 0U)
  {
    return;
  }
  fs = APP_SD_GetFs();
  if (fs == NULL)
  {
    snprintf(out, outsz, "sd: not mounted");
    return;
  }
  if (f_getfree(SD_DISK_TAG, &free_clust, &fs) != FR_OK)
  {
    snprintf(out, outsz, "sd: getfree failed");
    return;
  }
  total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512ULL;
  free  = (uint64_t)free_clust * fs->csize * 512ULL;
  snprintf(out, outsz, "sd: %llu MB total, %llu MB free", total / (1024*1024), free / (1024*1024));
}

DWORD get_fattime(void)
{
  /* Returns current time in FAT format. Since we don't have RTC, use a
   * fixed base + HAL_GetTick() offset. For real timestamps, integrate RTC. */
  return ((DWORD)(2026 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

#else  /* APP_SD_ENABLED == 0: no card socket, no FatFs, no SDMMC in the image */

#include "app_sd.h"
#include <stdint.h>
#include <stdio.h>

/* Empty implementations keep every caller (APIE, CLI, learned-data loader)
   linking unchanged while the real driver, FatFs and the SDMMC HAL are all
   compiled out.  See app_config.h. */

void APP_SD_Init(void) { }

void APP_SD_Poll(void) { }

APP_SD_State_t APP_SD_GetState(void) { return APP_SD_STATE_NONE; }

FATFS *APP_SD_GetFs(void) { return NULL; }

uint8_t APP_SD_TryMount(void) { return 0U; }

uint8_t APP_SD_Unmount(void) { return 1U; }

void APP_SD_Status(char *out, uint32_t outsz)
{
  if ((out != NULL) && (outsz != 0U))
  {
    (void)snprintf(out, outsz, "sd: disabled at build time (APP_SD_ENABLED=0)");
  }
}

void APP_SD_CardInfo(char *out, uint32_t outsz)
{
  if ((out != NULL) && (outsz != 0U)) { out[0] = '\0'; }
}

uint32_t APP_SD_Dir(const char *path, char *out, uint32_t outsz)
{
  (void)path;
  if ((out != NULL) && (outsz != 0U))
  {
    (void)snprintf(out, outsz, "sd: disabled at build time\r\n");
  }
  return 0U;
}

const char *APP_SD_GetPath(void) { return ""; }

uint8_t APP_SD_IsCardDetectUsable(void) { return 0U; }

#endif /* APP_SD_ENABLED */
