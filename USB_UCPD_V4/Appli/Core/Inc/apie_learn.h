#ifndef APIE_LEARN_H
#define APIE_LEARN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"
#include "apie_unknown.h"
#include <stdint.h>

/* ============================================================================
 *  Packet Learning Engine (APIE_LEARN)
 * ============================================================================
 *  Captures ALL unknown/unstructured packets, decodes them (including vendor-
 *  specific commands), classifies their purpose, assigns CLI function names,
 *  and persists learned knowledge to SD card for reuse across sessions.
 *
 *  Fail-safe design:
 *  - All allocations are static/bounded
 *  - Every SD operation is non-blocking with timeout
 *  - No operation can hard-fault or hang
 *  - Graceful degradation when SD unavailable
 *  ========================================================================= */

/* Maximum number of learned command signatures we can track */
#ifndef APIE_LEARN_MAX_SIGNATURES
#define APIE_LEARN_MAX_SIGNATURES    64U
#endif

/* Maximum payload bytes to store per signature */
#ifndef APIE_LEARN_MAX_PAYLOAD
#define APIE_LEARN_MAX_PAYLOAD       32U
#endif

/* Maximum CLI function name length */
#define APIE_LEARN_NAME_MAX          24U

/* Maximum CLI command length (for the assigned function) */
#define APIE_LEARN_CMD_MAX           32U

/* Confidence thresholds */
#define APIE_LEARN_CONFIDENCE_LOW    30U
#define APIE_LEARN_CONFIDENCE_MED    60U
#define APIE_LEARN_CONFIDENCE_HIGH   85U

/* Vendor-specific SOP types we can observe (SOP'/SOP'') */
typedef enum
{
  APIE_LEARN_SRC_SOP        = 0,  /* Standard SOP messages */
  APIE_LEARN_SRC_SOP_PRIME  = 1,  /* SOP' - cable/e-marker */
  APIE_LEARN_SRC_SOP_DBL    = 2,  /* SOP'' - cable plug */
  APIE_LEARN_SRC_VENDOR     = 3,  /* Vendor-defined / unstructured */
  APIE_LEARN_SRC_COUNT
} APIE_LearnSource_t;

/* Learned packet signature - identifies a unique packet pattern */
typedef struct
{
  uint32_t signature;           /* FNV-1a hash of SOP+type+payload prefix */
  uint8_t  sop;                 /* SOP type (0=SOP, 1=SOP', 2=SOP'') */
  uint8_t  msg_type;            /* PD message type (header bits 4:0) */
  uint8_t  extended;            /* Extended message flag */
  uint8_t  n_objects;           /* Number of data objects */
  uint8_t  payload[APIE_LEARN_MAX_PAYLOAD];
  uint8_t  payload_len;
  
  /* Occurrence tracking */
  uint32_t count;               /* Total times observed */
  uint32_t first_seen_ms;       /* Timestamp of first observation */
  uint32_t last_seen_ms;        /* Timestamp of last observation */
  uint32_t interval_sum_ms;     /* Sum of intervals for frequency calc */
  uint32_t interval_count;      /* Number of intervals measured */
  
  /* Correlation with power/state events */
  uint16_t vbus_corr;           /* Correlation with VBUS changes */
  uint16_t current_corr;        /* Correlation with current changes */
  uint16_t temp_corr;           /* Correlation with temperature */
  uint16_t reset_corr;          /* Correlation with hard resets */
  uint16_t voltchg_corr;        /* Correlation with voltage changes */
  uint16_t attach_corr;         /* Correlation with attach events */
  
  /* Learned classification */
  uint8_t  category;            /* APIE_LearnCategory_t */
  uint8_t  confidence;          /* 0-100 */
  uint8_t  has_response;        /* 1 if we've seen a response to this */
  uint8_t  response_type;       /* Expected response message type */
  
  /* Assigned CLI identity */
  char     cli_name[APIE_LEARN_NAME_MAX];
  char     cli_cmd[APIE_LEARN_CMD_MAX];
  uint8_t  has_cli;             /* 1 if CLI function assigned */
  
  /* Source identity this was learned from */
  uint16_t source_vid;
  uint16_t source_pid;
  
  /* Persistence version */
  uint16_t version;
  
} APIE_LearnSig_t;

/* Category classification for learned signatures */
typedef enum
{
  APIE_LEARN_CAT_UNKNOWN         = 0,  /* Not yet classified */
  APIE_LEARN_CAT_GET_STATUS      = 1,  /* Get_Status / extended status */
  APIE_LEARN_CAT_GET_PPS         = 2,  /* Get_PPS_Status */
  APIE_LEARN_CAT_GET_SRC_CAP     = 3,  /* Get_Source_Cap / extended */
  APIE_LEARN_CAT_GET_SNK_CAP     = 4,  /* Get_Sink_Cap */
  APIE_LEARN_CAT_GET_SRC_CAP_EXT = 5,  /* Get_Source_Cap_Extended */
  APIE_LEARN_CAT_VDM_IDENTITY    = 6,  /* VDM Discover Identity */
  APIE_LEARN_CAT_VDM_SVIDS       = 7,  /* VDM Discover SVIDs */
  APIE_LEARN_CAT_VDM_MODES       = 8,  /* VDM Discover Modes */
  APIE_LEARN_CAT_VDM_ENTER       = 9,  /* VDM Enter Mode */
  APIE_LEARN_CAT_VDM_EXIT        = 10, /* VDM Exit Mode */
  APIE_LEARN_CAT_VDM_ATTENTION   = 11, /* VDM Attention */
  APIE_LEARN_CAT_MANU_INFO       = 12, /* Manufacturer Info */
  APIE_LEARN_CAT_BATTERY         = 13, /* Battery Cap/Status */
  APIE_LEARN_CAT_COUNTRY         = 14, /* Country Codes/Info */
  APIE_LEARN_CAT_ALERT           = 15, /* Alert message */
  APIE_LEARN_CAT_REQUEST         = 16, /* Power Request */
  APIE_LEARN_CAT_VENDOR_DEFINED  = 17, /* Vendor-specific (unstructured) */
  APIE_LEARN_CAT_CABLE_IDENTITY  = 18, /* Cable SOP'/SOP'' identity */
  APIE_LEARN_CAT_CABLE_VDM       = 19, /* Cable VDM */
  APIE_LEARN_CAT_EPR_AVS         = 20, /* EPR AVS messages */
  APIE_LEARN_CAT_EPR_MODE        = 21, /* EPR Mode Entry/Exit */
  APIE_LEARN_CAT_PERIODIC_TELEM  = 22, /* Periodic telemetry */
  APIE_LEARN_CAT_STATE_DEPENDENT = 23, /* State-dependent behavior */
  APIE_LEARN_CAT_RESET_RELATED   = 24, /* Correlates with resets */
} APIE_LearnCategory_t;

/* Learn engine state */
typedef struct
{
  APIE_LearnSig_t sigs[APIE_LEARN_MAX_SIGNATURES];
  uint16_t count;
  uint8_t  enabled;
  uint8_t  auto_name;           /* Auto-assign CLI names */
  uint32_t last_save_ms;        /* Last SD save timestamp */
  uint32_t save_interval_ms;    /* Save interval (default 30s) */
  uint8_t  dirty;               /* Has unsaved changes */
  
} APIE_LearnState_t;

/* Public API */

/* Initialize the learning engine */
void APIE_Learn_Init(void);

/* Reset session (new source attached) */
void APIE_Learn_ResetSession(uint16_t source_vid, uint16_t source_pid);

/* Feed a raw packet for learning. Called from super-loop (not ISR). */
void APIE_Learn_FeedPacket(uint8_t sop, uint8_t msg_type, uint8_t extended,
                           uint8_t n_objects, const uint8_t *payload, uint16_t len,
                           uint32_t vbus_mv, uint32_t current_ma, uint32_t temp_c,
                           uint8_t reset_occurred, uint8_t voltchg_occurred,
                           uint8_t attach_occurred);

/* Feed correlation event (VBUS change, reset, etc.) */
void APIE_Learn_FeedEvent(uint8_t event_type, uint32_t value);

/* Enable/disable auto CLI name assignment */
void APIE_Learn_SetAutoName(uint8_t on);

/* Manually assign a CLI name and command to a signature */
uint8_t APIE_Learn_AssignName(uint16_t sig_idx, const char *name, const char *cmd);

/* Get signature by index */
const APIE_LearnSig_t *APIE_Learn_Get(uint16_t idx);

/* Get signature count */
uint16_t APIE_Learn_Count(void);

/* Find signature by SOP+type+payload prefix */
int16_t APIE_Learn_Find(uint8_t sop, uint8_t msg_type, uint8_t extended,
                        const uint8_t *payload, uint16_t len);

/* Dump all learned signatures to console */
void APIE_Learn_Dump(void);

/* Execute a learned query and wait for its human reply (see .c) */
uint8_t APIE_Learn_Exec(uint16_t idx);
void APIE_Learn_OnResponse(uint8_t sop, const uint8_t *payload, uint16_t len);
const char *APIE_Learn_LastReply(void);

/* Save learned data to SD card */
uint8_t APIE_Learn_SaveToSD(void);

/* Load learned data from SD card */
uint8_t APIE_Learn_LoadFromSD(void);

/* Get human-readable category name */
const char *APIE_Learn_CatName(uint8_t cat);

/* Map a learned signature category to its canonical APIE query ID.
 * Returns 0xFF if no standard query maps to this category. */
uint8_t APIE_Learn_CategoryToQuery(uint8_t category);

/* Check if a signature should trigger auto-query (confidence≥30%, count≥3) */
uint8_t APIE_Learn_ShouldAutoQuery(const APIE_LearnSig_t *sig);

/* Execute auto-query for a signature index */
uint8_t APIE_Learn_ExecAutoQuery(uint16_t sig_idx);

/* CLI command handlers */
void APIE_Learn_CliLearn(int argc, char *argv[]);
void APIE_Learn_CliSignatures(int argc, char *argv[]);
void APIE_Learn_CliAssign(int argc, char *argv[]);
void APIE_Learn_CliExport(int argc, char *argv[]);

/* Internal helper - called from APIE_Task for periodic SD sync */
void APIE_Learn_PeriodicTask(void);

/* Get current engine state for diagnostics */
const APIE_LearnState_t *APIE_Learn_GetState(void);

/* BKPSRAM import/export for learn engine state */
uint16_t APIE_Learn_Export(uint8_t *out, uint16_t outsz);
uint8_t APIE_Learn_Import(const uint8_t *in, uint16_t len);

/* Update all signatures with the now-known source VID/PID */
void APIE_Learn_UpdateSourceID(uint16_t vid, uint16_t pid);

#ifdef __cplusplus
}
#endif

#endif /* APIE_LEARN_H */