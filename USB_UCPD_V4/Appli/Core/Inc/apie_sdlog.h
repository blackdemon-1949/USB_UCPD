#ifndef APIE_SDLOG_H
#define APIE_SDLOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================================================================
 *  APIE SD Card Logging (APIE_SDLOG)
 * ============================================================================
 *  Always-on logging of:
 *  - Raw packet captures (log_N.csv)
 *  - Learned signatures database (learn.bin)
 *  - ML model checkpoints (model_A.bin, model_B.bin)
 *  - Source profile database (profiles.bin)
 *  - Experiment/training data (train_N.csv)
 *  
 *  Features:
 *  - Circular log rotation (configurable max files, max size)
 *  - Atomic writes with tmp+rename
 *  - Throttled f_sync (not every write)
 *  - Fail-safe: never blocks, graceful degradation
 *  ========================================================================= */

#ifndef APIE_SDLOG_MAX_LOGS
#define APIE_SDLOG_MAX_LOGS         32U   /* Max rotated log files */
#endif

#ifndef APIE_SDLOG_MAX_SIZE_KB
#define APIE_SDLOG_MAX_SIZE_KB      4096U /* 4 MB per log file */
#endif

#ifndef APIE_SDLOG_SYNC_INTERVAL_MS
#define APIE_SDLOG_SYNC_INTERVAL_MS 500U  /* f_sync throttle */
#endif

#ifndef APIE_SDLOG_STAGING_SIZE
#define APIE_SDLOG_STAGING_SIZE     4096U /* AXI SRAM staging buffer */
#endif

/* Log types */
typedef enum
{
  APIE_SDLOG_TYPE_PACKETS   = 0,  /* Raw packet CSV */
  APIE_SDLOG_TYPE_LEARN     = 1,  /* Learned signatures */
  APIE_SDLOG_TYPE_MODEL     = 2,  /* ML model checkpoints */
  APIE_SDLOG_TYPE_PROFILE   = 3,  /* Source profiles DB */
  APIE_SDLOG_TYPE_TRAIN     = 4,  /* Training data export */
  APIE_SDLOG_TYPE_EVENTS    = 5,  /* Event log (resets, attach, etc.) */
  APIE_SDLOG_TYPE_COUNT
} APIE_SdLogType_t;

/* SD log state */
typedef struct
{
  uint8_t  enabled;
  uint8_t  active_logs[APIE_SDLOG_TYPE_COUNT];  /* Which log types are active */
  uint32_t log_sizes_kb[APIE_SDLOG_TYPE_COUNT]; /* Current log sizes */
  uint32_t write_counts[APIE_SDLOG_TYPE_COUNT];
  uint32_t error_counts[APIE_SDLOG_TYPE_COUNT];
  uint32_t last_sync_ms[APIE_SDLOG_TYPE_COUNT];
  uint8_t  staging[APIE_SDLOG_STAGING_SIZE];
  uint16_t staging_used;
  char     current_filename[64];
  
} APIE_SdLogState_t;

/* Public API */

void APIE_SdLog_Init(void);
void APIE_SdLog_EnableType(APIE_SdLogType_t type, uint8_t on);

/* Log a raw packet (CSV format) */
void APIE_SdLog_Packet(uint32_t ts_ms, uint8_t dir, uint8_t sop, uint8_t msgid,
                       uint8_t type, uint8_t ext, uint8_t nobj, uint16_t hdr,
                       const uint8_t *payload, uint16_t len);

/* Log learned signature */
void APIE_SdLog_LearnSig(const void *sig, uint32_t sig_size);

/* Save ML model (with atomic rename) */
uint8_t APIE_SdLog_SaveModel(const uint8_t *model_data, uint32_t model_size, uint8_t slot);

/* Load ML model */
uint8_t APIE_SdLog_LoadModel(uint8_t *model_data, uint32_t max_size, uint8_t slot);

/* Save source profile database */
uint8_t APIE_SdLog_SaveProfiles(const uint8_t *data, uint32_t size);

/* Load source profile database */
uint8_t APIE_SdLog_LoadProfiles(uint8_t *data, uint32_t max_size);

/* Log training data row */
void APIE_SdLog_TrainRow(const char *csv_row);

/* Log system event */
void APIE_SdLog_Event(const char *event, uint32_t value);

/* Periodic task - call from super-loop for sync/rotation */
void APIE_SdLog_PeriodicTask(void);

/* Force sync all pending writes */
void APIE_SdLog_Flush(void);

/* Get status for CLI */
void APIE_SdLog_Status(char *out, uint32_t outsz);

/* Rotate logs if needed */
void APIE_SdLog_RotateIfNeeded(void);

/* CLI command handler */
void APIE_SdLog_Cli(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APIE_SDLOG_H */