
/* --------------------------------------------------------------------------- */
/*  Compile-time switch (app_config.h).  With APP_SD_ENABLED == 0 the whole
 *  body of this translation unit disappears and the callers fall back to the
 *  body of this translation unit is replaced by empty implementations at the
 *  end of the file, so no SD / FatFs code can end up in the image.  The file
 *  itself stays where STM32CubeIDE expects it.
 */
#include "app_config.h"
#if APP_SD_ENABLED

#include "apie_sdlog.h"

#include "app_sd.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>

extern uint32_t HAL_GetTick(void);

/* File name patterns */
#define LOG_BASE_DIR      "0:/apie/logs"
#define LOG_PACKETS_FMT   "0:/apie/logs/pkt_%03lu.csv"
#define LOG_LEARN_FMT     "0:/apie/logs/learn_%03lu.bin"
#define LOG_MODEL_A       "0:/apie/model_A.bin"
#define LOG_MODEL_B       "0:/apie/model_B.bin"
#define LOG_PROFILES      "0:/apie/profiles.bin"
#define LOG_TRAIN_FMT     "0:/apie/logs/train_%03lu.csv"
#define LOG_EVENTS_FMT    "0:/apie/logs/evt_%03lu.csv"

#define MAX_LOG_INDEX     999U
#define STAGING_FLUSH_THRESHOLD  (APIE_SDLOG_STAGING_SIZE * 3 / 4)

static APIE_SdLogState_t s_sdlog;
static uint32_t s_log_indices[APIE_SDLOG_TYPE_COUNT];

static void ensure_log_dir(void)
{
  FATFS *fs = APP_SD_GetFs();
  if (fs)
  {
    f_mkdir("0:/apie");
    f_mkdir(LOG_BASE_DIR);
  }
}

static uint8_t open_log_file(APIE_SdLogType_t type, FIL *fil, const char *mode)
{
  if (!APP_SD_GetFs()) return 0U;
  
  ensure_log_dir();
  
  char fname[64];
  uint32_t idx = s_log_indices[type];
  
  switch (type)
  {
    case APIE_SDLOG_TYPE_PACKETS:
      snprintf(fname, sizeof(fname), LOG_PACKETS_FMT, (unsigned long)idx);
      break;
    case APIE_SDLOG_TYPE_LEARN:
      snprintf(fname, sizeof(fname), LOG_LEARN_FMT, (unsigned long)idx);
      break;
    case APIE_SDLOG_TYPE_TRAIN:
      snprintf(fname, sizeof(fname), LOG_TRAIN_FMT, (unsigned long)idx);
      break;
    case APIE_SDLOG_TYPE_EVENTS:
      snprintf(fname, sizeof(fname), LOG_EVENTS_FMT, (unsigned long)idx);
      break;
    default:
      return 0U;
  }
  
  FRESULT fr = f_open(fil, fname, FA_WRITE | FA_OPEN_APPEND);
  if (fr == FR_OK)
  {
    strncpy(s_sdlog.current_filename, fname, sizeof(s_sdlog.current_filename) - 1);
    return 1U;
  }
  return 0U;
}

static uint32_t get_file_size_kb(const char *fname)
{
  FILINFO fno;
  if (f_stat(fname, &fno) == FR_OK)
  {
    return (uint32_t)((fno.fsize + 1023) / 1024);
  }
  return 0U;
}

static void rotate_log(APIE_SdLogType_t type)
{
  if (s_log_indices[type] >= MAX_LOG_INDEX)
  {
    /* Wrap around - delete oldest */
    char old_name[64];
    switch (type)
    {
      case APIE_SDLOG_TYPE_PACKETS:
        snprintf(old_name, sizeof(old_name), LOG_PACKETS_FMT, 0UL);
        break;
      case APIE_SDLOG_TYPE_LEARN:
        snprintf(old_name, sizeof(old_name), LOG_LEARN_FMT, 0UL);
        break;
      case APIE_SDLOG_TYPE_TRAIN:
        snprintf(old_name, sizeof(old_name), LOG_TRAIN_FMT, 0UL);
        break;
      case APIE_SDLOG_TYPE_EVENTS:
        snprintf(old_name, sizeof(old_name), LOG_EVENTS_FMT, 0UL);
        break;
      default:
        return;
    }
    f_unlink(old_name);
    
    /* Shift all indices down */
    for (uint32_t i = 1; i <= MAX_LOG_INDEX; i++)
    {
      char src[64], dst[64];
      switch (type)
      {
        case APIE_SDLOG_TYPE_PACKETS:
          snprintf(src, sizeof(src), LOG_PACKETS_FMT, (unsigned long)i);
          snprintf(dst, sizeof(dst), LOG_PACKETS_FMT, (unsigned long)(i - 1));
          break;
        case APIE_SDLOG_TYPE_LEARN:
          snprintf(src, sizeof(src), LOG_LEARN_FMT, (unsigned long)i);
          snprintf(dst, sizeof(dst), LOG_LEARN_FMT, (unsigned long)(i - 1));
          break;
        case APIE_SDLOG_TYPE_TRAIN:
          snprintf(src, sizeof(src), LOG_TRAIN_FMT, (unsigned long)i);
          snprintf(dst, sizeof(dst), LOG_TRAIN_FMT, (unsigned long)(i - 1));
          break;
        case APIE_SDLOG_TYPE_EVENTS:
          snprintf(src, sizeof(src), LOG_EVENTS_FMT, (unsigned long)i);
          snprintf(dst, sizeof(dst), LOG_EVENTS_FMT, (unsigned long)(i - 1));
          break;
        default:
          continue;
      }
      f_rename(src, dst);
    }
    s_log_indices[type] = MAX_LOG_INDEX;
  }
  else
  {
    s_log_indices[type]++;
  }
}

static void check_rotate(APIE_SdLogType_t type)
{
  if (!s_sdlog.active_logs[type]) return;
  
  char fname[64];
  switch (type)
  {
    case APIE_SDLOG_TYPE_PACKETS:
      snprintf(fname, sizeof(fname), LOG_PACKETS_FMT, (unsigned long)s_log_indices[type]);
      break;
    case APIE_SDLOG_TYPE_LEARN:
      snprintf(fname, sizeof(fname), LOG_LEARN_FMT, (unsigned long)s_log_indices[type]);
      break;
    case APIE_SDLOG_TYPE_TRAIN:
      snprintf(fname, sizeof(fname), LOG_TRAIN_FMT, (unsigned long)s_log_indices[type]);
      break;
    case APIE_SDLOG_TYPE_EVENTS:
      snprintf(fname, sizeof(fname), LOG_EVENTS_FMT, (unsigned long)s_log_indices[type]);
      break;
    default:
      return;
  }
  
  if (get_file_size_kb(fname) >= APIE_SDLOG_MAX_SIZE_KB)
  {
    rotate_log(type);
  }
}

static void flush_staging(void)
{
  if (s_sdlog.staging_used == 0) return;
  
  /* For simplicity, we append staging to the packets log */
  FIL fil;
  if (open_log_file(APIE_SDLOG_TYPE_PACKETS, &fil, "a"))
  {
    UINT bw;
    FRESULT fr = f_write(&fil, s_sdlog.staging, s_sdlog.staging_used, &bw);
    if (fr == FR_OK)
    {
      s_sdlog.write_counts[APIE_SDLOG_TYPE_PACKETS]++;
      s_sdlog.log_sizes_kb[APIE_SDLOG_TYPE_PACKETS] = get_file_size_kb(s_sdlog.current_filename);
    }
    else
    {
      s_sdlog.error_counts[APIE_SDLOG_TYPE_PACKETS]++;
    }
    f_close(&fil);
  }
  s_sdlog.staging_used = 0;
}

static void append_to_staging(const char *data, uint16_t len)
{
  if (s_sdlog.staging_used + len >= APIE_SDLOG_STAGING_SIZE)
  {
    flush_staging();
  }
  if (len < APIE_SDLOG_STAGING_SIZE - s_sdlog.staging_used)
  {
    memcpy(&s_sdlog.staging[s_sdlog.staging_used], data, len);
    s_sdlog.staging_used += len;
  }
}

void APIE_SdLog_Init(void)
{
  memset(&s_sdlog, 0, sizeof(s_sdlog));
  s_sdlog.enabled = 1U;
  s_sdlog.active_logs[APIE_SDLOG_TYPE_PACKETS] = 1;
  s_sdlog.active_logs[APIE_SDLOG_TYPE_LEARN] = 1;
  s_sdlog.active_logs[APIE_SDLOG_TYPE_MODEL] = 1;
  s_sdlog.active_logs[APIE_SDLOG_TYPE_PROFILE] = 1;
  s_sdlog.active_logs[APIE_SDLOG_TYPE_TRAIN] = 1;
  s_sdlog.active_logs[APIE_SDLOG_TYPE_EVENTS] = 1;
  
  /* Initialize log indices by scanning existing files */
  ensure_log_dir();
  for (uint8_t t = 0; t < APIE_SDLOG_TYPE_COUNT; t++)
  {
    s_log_indices[t] = 0;
    /* Find highest existing index */
    for (uint32_t i = 0; i <= MAX_LOG_INDEX; i++)
    {
      char fname[64];
      switch (t)
      {
        case APIE_SDLOG_TYPE_PACKETS:
          snprintf(fname, sizeof(fname), LOG_PACKETS_FMT, (unsigned long)i);
          break;
        case APIE_SDLOG_TYPE_LEARN:
          snprintf(fname, sizeof(fname), LOG_LEARN_FMT, (unsigned long)i);
          break;
        case APIE_SDLOG_TYPE_TRAIN:
          snprintf(fname, sizeof(fname), LOG_TRAIN_FMT, (unsigned long)i);
          break;
        case APIE_SDLOG_TYPE_EVENTS:
          snprintf(fname, sizeof(fname), LOG_EVENTS_FMT, (unsigned long)i);
          break;
        default:
          continue;
      }
      FILINFO fno;
      if (f_stat(fname, &fno) == FR_OK)
      {
        s_log_indices[t] = i;
      }
    }
  }
}

void APIE_SdLog_EnableType(APIE_SdLogType_t type, uint8_t on)
{
  if (type < APIE_SDLOG_TYPE_COUNT)
  {
    s_sdlog.active_logs[type] = on ? 1U : 0U;
  }
}

void APIE_SdLog_Packet(uint32_t ts_ms, uint8_t dir, uint8_t sop, uint8_t msgid,
                       uint8_t type, uint8_t ext, uint8_t nobj, uint16_t hdr,
                       const uint8_t *payload, uint16_t len)
{
  if (!s_sdlog.enabled || !s_sdlog.active_logs[APIE_SDLOG_TYPE_PACKETS]) return;
  
  check_rotate(APIE_SDLOG_TYPE_PACKETS);
  
  char line[256];
  int n = snprintf(line, sizeof(line),
                   "%lu,%u,%u,%u,%u,%u,%u,0x%04X,",
                   (unsigned long)ts_ms, (unsigned)dir, (unsigned)sop,
                   (unsigned)msgid, (unsigned)type, (unsigned)ext,
                   (unsigned)nobj, (unsigned)hdr);
  
  if (n > 0 && (uint16_t)n < sizeof(line))
  {
    uint16_t pos = (uint16_t)n;
    for (uint16_t i = 0; i < len && pos < sizeof(line) - 3; i++)
    {
      pos += snprintf(&line[pos], sizeof(line) - pos, "%02X", payload[i]);
    }
    line[pos++] = '\r';
    line[pos++] = '\n';
    line[pos] = '\0';
    
    append_to_staging(line, pos);
  }
}

void APIE_SdLog_LearnSig(const void *sig, uint32_t sig_size)
{
  if (!s_sdlog.enabled || !s_sdlog.active_logs[APIE_SDLOG_TYPE_LEARN] || !sig) return;
  
  check_rotate(APIE_SDLOG_TYPE_LEARN);
  
  FIL fil;
  if (open_log_file(APIE_SDLOG_TYPE_LEARN, &fil, "a"))
  {
    UINT bw;
    FRESULT fr = f_write(&fil, sig, sig_size, &bw);
    if (fr == FR_OK)
    {
      s_sdlog.write_counts[APIE_SDLOG_TYPE_LEARN]++;
      s_sdlog.log_sizes_kb[APIE_SDLOG_TYPE_LEARN] = get_file_size_kb(s_sdlog.current_filename);
    }
    else
    {
      s_sdlog.error_counts[APIE_SDLOG_TYPE_LEARN]++;
    }
    f_close(&fil);
  }
}

uint8_t APIE_SdLog_SaveModel(const uint8_t *model_data, uint32_t model_size, uint8_t slot)
{
  if (!s_sdlog.enabled || !s_sdlog.active_logs[APIE_SDLOG_TYPE_MODEL] || !model_data)
  {
    return 0U;
  }
  
  ensure_log_dir();
  
  char tmp_name[64];
  char final_name[64];
  
  if (slot == 0)
  {
    snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", LOG_MODEL_A);
    snprintf(final_name, sizeof(final_name), "%s", LOG_MODEL_A);
  }
  else
  {
    snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", LOG_MODEL_B);
    snprintf(final_name, sizeof(final_name), "%s", LOG_MODEL_B);
  }
  
  FIL fil;
  FRESULT fr = f_open(&fil, tmp_name, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK)
  {
    return 0U;
  }
  
  UINT bw;
  fr = f_write(&fil, model_data, model_size, &bw);
  if (fr == FR_OK)
  {
    fr = f_sync(&fil);
  }
  f_close(&fil);
  
  if (fr == FR_OK)
  {
    f_unlink(final_name);
    fr = f_rename(tmp_name, final_name);
    if (fr == FR_OK)
    {
      APP_LOG_Printf("sdlog: model %c saved (%lu bytes)\r\n", 'A' + slot, (unsigned long)model_size);
      return 1U;
    }
  }
  
  f_unlink(tmp_name);
  APP_LOG_Printf("sdlog: model %c save failed (%d)\r\n", 'A' + slot, (int)fr);
  return 0U;
}

uint8_t APIE_SdLog_LoadModel(uint8_t *model_data, uint32_t max_size, uint8_t slot)
{
  if (!model_data || max_size == 0) return 0U;
  
  FATFS *fs = APP_SD_GetFs();
  if (!fs) return 0U;
  
  char fname[64];
  if (slot == 0)
  {
    snprintf(fname, sizeof(fname), "%s", LOG_MODEL_A);
  }
  else
  {
    snprintf(fname, sizeof(fname), "%s", LOG_MODEL_B);
  }
  
  FIL fil;
  FRESULT fr = f_open(&fil, fname, FA_READ);
  if (fr != FR_OK)
  {
    return 0U;
  }
  
  UINT br;
  fr = f_read(&fil, model_data, max_size, &br);
  f_close(&fil);
  
  if (fr == FR_OK && br > 0)
  {
    APP_LOG_Printf("sdlog: model %c loaded (%u bytes)\r\n", 'A' + slot, (unsigned)br);
    return 1U;
  }
  return 0U;
}

uint8_t APIE_SdLog_SaveProfiles(const uint8_t *data, uint32_t size)
{
  if (!s_sdlog.enabled || !s_sdlog.active_logs[APIE_SDLOG_TYPE_PROFILE] || !data)
  {
    return 0U;
  }
  
  ensure_log_dir();
  
  char tmp_name[64] = {0};
  snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", LOG_PROFILES);
  
  FIL fil;
  FRESULT fr = f_open(&fil, tmp_name, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) return 0U;
  
  UINT bw;
  fr = f_write(&fil, data, size, &bw);
  if (fr == FR_OK) fr = f_sync(&fil);
  f_close(&fil);
  
  if (fr == FR_OK)
  {
    f_unlink(LOG_PROFILES);
    fr = f_rename(tmp_name, LOG_PROFILES);
    if (fr == FR_OK)
    {
      APP_LOG_Printf("sdlog: profiles saved (%lu bytes)\r\n", (unsigned long)size);
      return 1U;
    }
  }
  f_unlink(tmp_name);
  return 0U;
}

uint8_t APIE_SdLog_LoadProfiles(uint8_t *data, uint32_t max_size)
{
  if (!data || max_size == 0) return 0U;
  
  FATFS *fs = APP_SD_GetFs();
  if (!fs) return 0U;
  
  FIL fil;
  FRESULT fr = f_open(&fil, LOG_PROFILES, FA_READ);
  if (fr != FR_OK) return 0U;
  
  UINT br;
  fr = f_read(&fil, data, max_size, &br);
  f_close(&fil);
  
  if (fr == FR_OK && br > 0)
  {
    APP_LOG_Printf("sdlog: profiles loaded (%u bytes)\r\n", (unsigned)br);
    return 1U;
  }
  return 0U;
}

void APIE_SdLog_TrainRow(const char *csv_row)
{
  if (!s_sdlog.enabled || !s_sdlog.active_logs[APIE_SDLOG_TYPE_TRAIN] || !csv_row) return;
  
  check_rotate(APIE_SDLOG_TYPE_TRAIN);
  
  FIL fil;
  if (open_log_file(APIE_SDLOG_TYPE_TRAIN, &fil, "a"))
  {
    UINT bw;
    FRESULT fr = f_write(&fil, csv_row, strlen(csv_row), &bw);
    if (fr == FR_OK)
    {
      s_sdlog.write_counts[APIE_SDLOG_TYPE_TRAIN]++;
      s_sdlog.log_sizes_kb[APIE_SDLOG_TYPE_TRAIN] = get_file_size_kb(s_sdlog.current_filename);
    }
    else
    {
      s_sdlog.error_counts[APIE_SDLOG_TYPE_TRAIN]++;
    }
    f_close(&fil);
  }
}

void APIE_SdLog_Event(const char *event, uint32_t value)
{
  if (!s_sdlog.enabled || !s_sdlog.active_logs[APIE_SDLOG_TYPE_EVENTS] || !event) return;
  
  check_rotate(APIE_SDLOG_TYPE_EVENTS);
  
  char line[128];
  int n = snprintf(line, sizeof(line), "%lu,%s,%lu\r\n", (unsigned long)HAL_GetTick(), event, (unsigned long)value);
  if (n > 0)
  {
    FIL fil;
    if (open_log_file(APIE_SDLOG_TYPE_EVENTS, &fil, "a"))
    {
      UINT bw;
      FRESULT fr = f_write(&fil, line, (UINT)n, &bw);
      if (fr == FR_OK)
      {
        s_sdlog.write_counts[APIE_SDLOG_TYPE_EVENTS]++;
        s_sdlog.log_sizes_kb[APIE_SDLOG_TYPE_EVENTS] = get_file_size_kb(s_sdlog.current_filename);
      }
      else
      {
        s_sdlog.error_counts[APIE_SDLOG_TYPE_EVENTS]++;
      }
      f_close(&fil);
    }
  }
}

void APIE_SdLog_PeriodicTask(void)
{
  if (!s_sdlog.enabled) return;
  
  uint32_t now = HAL_GetTick();
  
  /* Throttled sync for active logs */
  for (uint8_t t = 0; t < APIE_SDLOG_TYPE_COUNT; t++)
  {
    if (!s_sdlog.active_logs[t]) continue;
    
    if ((now - s_sdlog.last_sync_ms[t]) >= APIE_SDLOG_SYNC_INTERVAL_MS)
    {
      if (t == APIE_SDLOG_TYPE_PACKETS && s_sdlog.staging_used > 0)
      {
        flush_staging();
      }
      /* Sync other open files would need file handles - skip for now */
      s_sdlog.last_sync_ms[t] = now;
    }
  }
  
  /* Rotate check every 10 seconds */
  static uint32_t last_rotate_check = 0;
  if ((now - last_rotate_check) >= 10000U)
  {
    last_rotate_check = now;
    for (uint8_t t = 0; t < APIE_SDLOG_TYPE_COUNT; t++)
    {
      check_rotate((APIE_SdLogType_t)t);
    }
  }
}

void APIE_SdLog_Flush(void)
{
  if (!s_sdlog.enabled) return;
  flush_staging();
  /* f_sync on all active files would need handles - skip for now */
}

void APIE_SdLog_Status(char *out, uint32_t outsz)
{
  if (!out || outsz == 0) return;
  
  char *p = out;
  uint32_t rem = outsz;
  int n;
  
  n = snprintf(p, rem, "sdlog: en=%s logs:",
               s_sdlog.enabled ? "on" : "off");
  if (n > 0 && (uint32_t)n < rem) { p += n; rem -= n; }
  
  for (uint8_t t = 0; t < APIE_SDLOG_TYPE_COUNT; t++)
  {
    if (s_sdlog.active_logs[t])
    {
      n = snprintf(p, rem, " %s(%luKB/%lu)",
                   t == 0 ? "pkt" : t == 1 ? "learn" : t == 2 ? "model" :
                   t == 3 ? "prof" : t == 4 ? "train" : "evt",
                   (unsigned long)s_sdlog.log_sizes_kb[t],
                   (unsigned long)s_sdlog.write_counts[t]);
      if (n > 0 && (uint32_t)n < rem) { p += n; rem -= n; }
    }
  }
  
  n = snprintf(p, rem, " err=%lu",
               (unsigned long)s_sdlog.error_counts[0]);
  (void)n;
}

void APIE_SdLog_RotateIfNeeded(void)
{
  for (uint8_t t = 0; t < APIE_SDLOG_TYPE_COUNT; t++)
  {
    check_rotate((APIE_SdLogType_t)t);
  }
}

void APIE_SdLog_Cli(int argc, char *argv[])
{
  if (argc < 2)
  {
    APP_LOG_Write("usage: sdlog on|off|status|flush|rotate|enable <type> on|off\r\n");
    return;
  }
  
  if (strcmp(argv[1], "on") == 0)
  {
    s_sdlog.enabled = 1U;
    APP_LOG_Write("sdlog: enabled\r\n");
  }
  else if (strcmp(argv[1], "off") == 0)
  {
    s_sdlog.enabled = 0U;
    APP_LOG_Write("sdlog: disabled\r\n");
  }
  else if (strcmp(argv[1], "status") == 0)
  {
    char buf[256];
    APIE_SdLog_Status(buf, sizeof(buf));
    APP_LOG_Printf("%s\r\n", buf);
  }
  else if (strcmp(argv[1], "flush") == 0)
  {
    APIE_SdLog_Flush();
    APP_LOG_Write("sdlog: flushed\r\n");
  }
  else if (strcmp(argv[1], "rotate") == 0)
  {
    APIE_SdLog_RotateIfNeeded();
    APP_LOG_Write("sdlog: rotation checked\r\n");
  }
  else if (strcmp(argv[1], "enable") == 0 && argc >= 4)
  {
    unsigned type = 0;
    if (strcmp(argv[2], "packets") == 0) type = 0;
    else if (strcmp(argv[2], "learn") == 0) type = 1;
    else if (strcmp(argv[2], "model") == 0) type = 2;
    else if (strcmp(argv[2], "profile") == 0) type = 3;
    else if (strcmp(argv[2], "train") == 0) type = 4;
    else if (strcmp(argv[2], "events") == 0) type = 5;
    else
    {
      APP_LOG_Write("sdlog: unknown type\r\n");
      return;
    }
    s_sdlog.active_logs[type] = (strcmp(argv[3], "on") == 0) ? 1U : 0U;
    APP_LOG_Printf("sdlog: type %u %s\r\n", type, s_sdlog.active_logs[type] ? "enabled" : "disabled");
  }
  else
  {
    APP_LOG_Write("usage: sdlog on|off|status|flush|rotate|enable <type> on|off\r\n");
  }
}

#else  /* APP_SD_ENABLED == 0 */

#include "apie_sdlog.h"
#include "app_log.h"
#include <stdint.h>
#include <stdio.h>

/* The SD card was removed from the product: learned data, the ML model and the
   profiles are persisted in the backup SRAM (apie_bkp.c) and in the external
   NOR store (app_store.c) instead.  These empty implementations keep the API -
   and therefore the APIE engine and the console - unchanged. */

void APIE_SdLog_Init(void) { }

void APIE_SdLog_EnableType(APIE_SdLogType_t type, uint8_t on)
{
  (void)type; (void)on;
  APP_LOG_Write("sdlog: the SD card is disabled at build time (APP_SD_ENABLED=0); "
                "use 'store' / 'cmd' for the external flash, 'cmos' for settings\r\n");
}

void APIE_SdLog_Packet(uint32_t ts_ms, uint8_t dir, uint8_t sop, uint8_t msgid,
                       uint8_t type, uint8_t ext, uint8_t nobj,
                       uint16_t hdr, const uint8_t *payload, uint16_t len)
{
  (void)ts_ms; (void)dir; (void)sop; (void)msgid; (void)type; (void)ext;
  (void)nobj; (void)hdr; (void)payload; (void)len;
}

void APIE_SdLog_LearnSig(const void *sig, uint32_t sig_size) { (void)sig; (void)sig_size; }

uint8_t APIE_SdLog_SaveModel(const uint8_t *model_data, uint32_t model_size, uint8_t slot)
{ (void)model_data; (void)model_size; (void)slot; return 0U; }

uint8_t APIE_SdLog_LoadModel(uint8_t *model_data, uint32_t max_size, uint8_t slot)
{ (void)model_data; (void)max_size; (void)slot; return 0U; }

uint8_t APIE_SdLog_SaveProfiles(const uint8_t *data, uint32_t size)
{ (void)data; (void)size; return 0U; }

uint8_t APIE_SdLog_LoadProfiles(uint8_t *data, uint32_t max_size)
{ (void)data; (void)max_size; return 0U; }

void APIE_SdLog_TrainRow(const char *csv_row) { (void)csv_row; }

void APIE_SdLog_Event(const char *event, uint32_t value) { (void)event; (void)value; }

void APIE_SdLog_PeriodicTask(void) { }

void APIE_SdLog_Flush(void) { }

void APIE_SdLog_Status(char *out, uint32_t outsz)
{
  if ((out != NULL) && (outsz != 0U))
  {
    (void)snprintf(out, outsz, "sdlog: disabled at build time (APP_SD_ENABLED=0)");
  }
}

void APIE_SdLog_RotateIfNeeded(void) { }

void APIE_SdLog_Cli(int argc, char *argv[])
{
  (void)argc; (void)argv;
  APP_LOG_Write("sdlog: the SD card is disabled at build time (APP_SD_ENABLED=0).\r\n"
                "       Learned data and command macros live in the external NOR "
                "flash: 'store', 'cmd'.\r\n");
}

#endif /* APP_SD_ENABLED */
