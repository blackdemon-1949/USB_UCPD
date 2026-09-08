/**
  ******************************************************************************
  * @file    apie.h
  * @brief   Advanced PD Intelligence Engine (APIE) - shared types/API.
  *
  * The APIE subsystem sits ABOVE the ST USBPD application/DPM side.  It never
  * replaces the real-time ST PE/PRL/CAD path; it observes and extends it with
  * analysis, learning and adaptive policy.  All heavy work runs in the super
  * loop (APP_PD_Task / APIE_Task), never inside the UCPD/DMA/USB ISRs or the
  * time-critical PE callbacks.
  *
  * Copyright (c) 2026 blackdemon-1949 (DIY/research project).
  *                                                         *
  ******************************************************************************
  */
#ifndef APIE_H
#define APIE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

/* ===========================================================================
 *  Compile-time configuration
 * ========================================================================= */

/* Bounded memory budgets.  RAM is plentiful here (~440 KB free in AXI SRAM);
 * these are kept modest so the PD stack and USB buffers are never threatened. */
#ifndef APIE_PACKET_RING
#define APIE_PACKET_RING          64U   /* raw packet ring depth             */
#endif
#ifndef APIE_PACKET_MAX
#define APIE_PACKET_MAX           40U   /* max bytes kept per raw packet     */
#endif
#ifndef APIE_TXN_MAX
#define APIE_TXN_MAX              32U   /* outstanding transaction table     */
#endif
#ifndef APIE_TXN_HIST
#define APIE_TXN_HIST             32U   /* completed transaction history     */
#endif
#ifndef APIE_PROFILE_PDOS
#define APIE_PROFILE_PDOS         12U   /* max PDO slots in a fingerprint    */
#endif
#ifndef APIE_UNKNOWN_KINDS
#define APIE_UNKNOWN_KINDS        24U   /* distinct unknown-message classes  */
#endif
#ifndef APIE_SCHED_SLOTS
#define APIE_SCHED_SLOTS          16U   /* schedulable query slots           */
#endif
#ifndef APIE_DB_PROFILES
#define APIE_DB_PROFILES          12U   /* cached source profiles            */
#endif
#ifndef APIE_EPR
#define APIE_EPR                  1U    /* EPR/AVS protocol awareness        */
#endif

/* Hardware capability flags.  Physical EPR (>20 V) is NOT enabled here. */
#define APIE_HW_EPR_POWER_ENABLED 0U    /* 0 = never energise EPR on this board */
#define APIE_HW_HAS_VBUS_ADC      0U    /* 0 = CC-only rig, synthetic VBUS      */
#define APIE_HW_HAS_DPLUS_DMINUS  0U    /* 0 = D+/D- not wired to this connector */
#define APIE_HW_CABLE_EMARKER     1U    /* SOP'/SOP'' observable over UCPD      */

/* Experiment level defaults (see APIE_ExpLevel_t).
 *
 * R0 (observe) is the power-on default.  R1 and above let the engine
 * TRANSMIT on the PD link by itself - Get_Status / Get_Source_Cap_Ext /
 * VDM Discover Identity and friends - every ~500 ms whenever a contract
 * is up.  Plenty of chargers answer an extended or VDM message they do
 * not implement with a HARD RESET, which turns the engine into a reset
 * generator: query -> hard reset -> re-attach -> query -> ...
 *
 * Observation costs nothing and still profiles the source, so the board
 * now starts silent.  Turn transmitting on deliberately with
 * `experiment set 1|2` when you want the probing. */
#define APIE_EXP_LEVEL_DEFAULT    0U    /* R0: observe only, never transmit */
#define APIE_EXP_ALLOW_R3         0U    /* state-changing experiments OFF       */
#define APIE_EXP_ALLOW_R4         0U    /* unknown/vendor transmissions OFF     */

/* Safety thresholds (guard rails, never advisory-only). */
#define APIE_MAX_VOLTAGE_MV       21000U /* hard ceiling across all policies    */
#define APIE_MAX_CURRENT_MA       5000U
#define APIE_PPS_STEP_MV          100U   /* fine PPS step for safe ramp         */
#define APIE_QUERY_COOLDOWN_MS    500U
#define APIE_QUERY_TIMEOUT_MS     1200U
#define APIE_QUERY_MAX_PENDING    2U

/* ---------------------------------------------------------------------------
 *  Message-direction & transaction outcome
 * ------------------------------------------------------------------------- */
typedef enum
{
  APIE_DIR_RX = 0,
  APIE_DIR_TX = 1
} APIE_Dir_t;

typedef enum
{
  APIE_TXN_OPEN          = 0,  /* awaiting expected response                */
  APIE_TXN_SUCCESS       = 1,  /* expected ACK/response                     */
  APIE_TXN_REJECT        = 2,  /* Reject                                    */
  APIE_TXN_WAIT          = 3,  /* Wait                                      */
  APIE_TXN_NOT_SUPPORTED = 4,  /* Not_Supported                             */
  APIE_TXN_TIMEOUT       = 5,  /* no response within tTimeout               */
  APIE_TXN_ERROR         = 6,  /* protocol/CRC error                        */
  APIE_TXN_UNKNOWN       = 7   /* response that did not match expectation   */
} APIE_TxnResult_t;

/* ---------------------------------------------------------------------------
 *  Decoded message classification
 * ------------------------------------------------------------------------- */
typedef enum
{
  APIE_MSG_CLS_UNKNOWN   = 0,
  APIE_MSG_CLS_CONTROL,
  APIE_MSG_CLS_DATA,
  APIE_MSG_CLS_EXTENDED,
  APIE_MSG_CLS_VDM_SVDM,
  APIE_MSG_CLS_VDM_UVDM,
  APIE_MSG_CLS_EPR
} APIE_MsgClass_t;

/* ---------------------------------------------------------------------------
 *  Raw packet record used by the bounded analyzer.
 *  Stored LSB-first bytes (the ST middleware/Power-Delivery wire format).
 *  Ownership: the analyzer always COPIES out of the ST RX buffer; it never
 *  owns or re-arms the ST DMA (`Ports[0].ptr_RxBuff`) buffer.
 * ------------------------------------------------------------------------- */
typedef struct
{
  uint32_t ts_ms;            /* capture timestamp, ms from HAL_GetTick     */
  uint8_t  dir;              /* APIE_DIR_RX / APIE_DIR_TX                 */
  uint8_t  sop;              /* USBPD_SOPTYPE_* code (0=SOP,1=SOP',...)    */
  uint8_t  msgid;            /* header bits 12:9                          */
  uint8_t  type;             /* header bits 4:0                           */
  uint8_t  ext;              /* extended bit (header bit 15)               */
  uint8_t  chunks;           /* chunked bit (header bit 14)                */
  uint8_t  nobjects;         /* number of 32-bit data objects              */
  uint16_t hdr;              /* raw 16-bit header, little-endian           */
  uint16_t len;              /* number of payload bytes kept               */
  uint8_t  data[APIE_PACKET_MAX];
} APIE_Packet_t;

/* ---------------------------------------------------------------------------
 *  Deterministic transaction record
 * ------------------------------------------------------------------------- */
typedef struct
{
  uint8_t  active;           /* 1 = slot occupied                         */
  uint8_t  dir;
  uint8_t  sop;
  uint8_t  tx_type;          /* message type we sent                      */
  uint8_t  exp_type;         /* expected response message type             */
  uint8_t  msgid;
  uint32_t ts_tx;            /* tx timestamp (ms)                          */
  uint32_t ts_rx;            /* matching rx timestamp (ms)                 */
  uint32_t latency_ms;
  APIE_TxnResult_t result;
  uint8_t  conn_id;          /* per-attach session id                      */
  uint8_t  attempt;          /* retry count for this transaction           */
} APIE_Txn_t;

/* ---------------------------------------------------------------------------
 *  Source fingerprint (hard + protocol + behavior)
 * ------------------------------------------------------------------------- */
typedef struct
{
  uint8_t  valid;            /* 1 when we have enough evidence             */
  uint8_t  conn_id;
  /* hard */
  uint16_t vid;
  uint16_t pid;
  uint8_t  fw;
  uint8_t  hw;
  uint8_t  has_hard;         /* 1 when VID/PID/FW/HW were seen             */
  /* protocol */
  uint8_t  pd_rev;           /* 1=PD2.0, 2=PD3.0, 3=PD3.1                  */
  uint8_t  n_pdo;
  uint32_t pdo[APIE_PROFILE_PDOS];
  uint8_t  has_pps;
  uint8_t  has_variable;
  uint8_t  has_battery;
  uint8_t  has_epr;
  uint16_t pps_min_mv;
  uint16_t pps_max_mv;
  uint16_t pps_max_ma;
  uint8_t  has_svid;
  uint16_t svid[8];
  uint8_t  n_svid;
  /* behavior */
  uint32_t adv_interval_ms;      /* mean interval between cap advertisements */
  uint32_t getstatus_latency_ms; /* mean response latency for Get_Status     */
  uint32_t reset_count;          /* hard resets observed                     */
  uint8_t  battery_supported;    /* 0 unknown, 1 yes, 2 no (Not_Supported)  */
  uint8_t  identity_supported;   /* 0 unknown, 1 yes, 2 no (NAK)            */
  uint8_t  extended_support;
} APIE_Profile_t;

/* ---------------------------------------------------------------------------
 *  Knowledge-database profile record (versioned, CRC-protected)
 * ------------------------------------------------------------------------- */
typedef struct
{
  uint32_t magic;            /* APIE_DB_MAGIC                              */
  uint16_t version;          /* APIE_DB_VERSION                            */
  uint16_t len;
  uint32_t crc32;            /* of the payload after this header           */
  APIE_Profile_t profile;
  uint8_t  reserved[32];
} APIE_DbProfile_t;

/* ---------------------------------------------------------------------------
 *  Scheduling / information-gain candidate
 * ------------------------------------------------------------------------- */
typedef enum
{
  APIE_QUERY_GET_STATUS   = 0,
  APIE_QUERY_GET_PPS      = 1,
  APIE_QUERY_IDENTITY     = 2,
  APIE_QUERY_SVIDS        = 3,
  APIE_QUERY_MODES        = 4,
  APIE_QUERY_SRC_EXT      = 5,
  APIE_QUERY_MANU_INFO    = 6,
  APIE_QUERY_BATTERY      = 7,
  APIE_QUERY_COUNTRY      = 8
} APIE_QueryId_t;

typedef struct
{
  APIE_QueryId_t id;
  uint8_t  enabled;
  uint8_t  pending;
  uint8_t  supported;      /* 0 unknown, 1 yes, 2 no, 3 never try          */
  uint8_t  attempts;
  uint16_t successes;
  uint16_t failures;
  float    score;          /* scheduler priority                           */
  uint32_t last_ms;        /* last time issued                             */
  uint32_t cooldown_ms;    /* learned cooldown                             */
} APIE_QueryState_t;

/* ---------------------------------------------------------------------------
 *  Learning metadata attached to every model / profile
 * ------------------------------------------------------------------------- */
typedef struct
{
  uint8_t  id;
  uint16_t version;
  uint16_t feature_version;
  uint32_t crc32;
  uint8_t  kind;           /* model kind tag                              */
  float    accuracy;       /* validation accuracy (0..1)                  */
  char     trained[24];
} APIE_ModelMeta_t;

/* ---------------------------------------------------------------------------
 *  Statistical accumulators (numerically stable, bounded)
 * ------------------------------------------------------------------------- */
typedef struct
{
  float  mean;
  float  m2;                /* Welford sum of squares                      */
  float  min;
  float  max;
  uint32_t n;
} APIE_StatAccum_t;

typedef enum
{
  APIE_MODEL_NAIVE_BAYES = 1,
  APIE_MODEL_TREE        = 2,
  APIE_MODEL_LOGISTIC    = 3,
  APIE_MODEL_SMALL_MLP   = 4
} APIE_ModelKind_t;

/* ---------------------------------------------------------------------------
 *  APIE top-level status
 * ------------------------------------------------------------------------- */
typedef enum
{
  APIE_STATE_DISABLED = 0,   /* safe mode: intelligence fully off         */
  APIE_STATE_OBSERVING,
  APIE_STATE_LEARNING,
  APIE_STATE_ADAPTIVE
} APIE_State_t;

/* ===========================================================================
 *  Public API
 * ========================================================================= */
void APIE_Init(void);
void APIE_Task(void);                 /* super-loop drive                  */
void APIE_SetSafeMode(uint8_t on);
uint8_t APIE_IsSafeMode(void);

/** 1 once the hard-reset guard has forced the engine silent. */
uint8_t APIE_HardResetGuardTripped(void);
/** Forget the hard-reset history and re-arm the guard. */
void    APIE_HardResetGuardClear(void);

APIE_State_t APIE_GetState(void);
uint8_t APIE_GetExperimentLevel(void);
void APIE_SetExperimentLevel(uint8_t level);

/* External event hooks (called from application / DPM / a minimal ISR hook). */
void APIE_OnCableAttach(uint8_t port, uint8_t cc);
void APIE_OnCableDetach(uint8_t port);
void APIE_OnSourceCaps(uint8_t port, const uint32_t *pdo, uint8_t n);
void APIE_OnNotify(uint8_t port, uint32_t event, uint32_t voltage_mv, uint32_t current_ma, uint32_t pdo_pos);
void APIE_OnRequestSent(uint8_t port, uint8_t index, uint16_t mv, uint16_t ma);
void APIE_OnDataInfo(uint8_t port, uint16_t data_id, const uint8_t *ptr, uint32_t size);
void APIE_OnExtendedMessage(uint8_t port, uint16_t type, const uint8_t *data, uint16_t size);
void APIE_OnVdmIdentity(uint8_t port, uint8_t ok);
void APIE_OnVdmSvids(uint8_t port, const uint16_t *svids, uint8_t n, uint8_t ok);
void APIE_OnHardReset(uint8_t port);

/* Query issuance bridge (called by the scheduler; dispatches to DPM). */
int APIE_IssueQuery(uint8_t port, APIE_QueryId_t id);

/* Raw capture bridge (called from a minimal UCPD RX-complete hook). */
void APIE_Analyzer_CaptureRaw(uint8_t port, uint8_t sop, const uint8_t *buf, uint32_t n);

/* Validate the compile-time electrical guard rails are internally consistent. */
uint8_t APIE_Safety_LimitsSane(void);

/* Main-loop / compute-budget instrumentation (see DIAGNOSTICS.md). */
uint32_t APIE_Diag_TaskCalls(void);
uint32_t APIE_Diag_TaskPeriodMaxMs(void);
uint32_t APIE_Diag_TaskPeriodAvgMs(void);
uint32_t APIE_Diag_DwtCyclesMax(void);
uint8_t  APIE_Diag_DwtReady(void);

/* CLI entry points */
void APIE_CliStatus(void);
void APIE_CliPdStats(void);
void APIE_CliPdPackets(uint8_t show_all);
void APIE_CliSource(void);
void APIE_CliFingerprint(void);
void APIE_CliTxnList(void);
void APIE_CliCounters(void);
void APIE_CliSafety(void);
void APIE_CliFeature(void);
void APIE_CliMlStatus(void);
void APIE_CliPredict(char *hex);
void APIE_CliUnknown(void);
void APIE_CliScheduler(void);
void APIE_CliDbStatus(void);
void APIE_CliExperiment(void);

/* Host/build helpers */
uint32_t APIE_Crc32(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* APIE_H */
