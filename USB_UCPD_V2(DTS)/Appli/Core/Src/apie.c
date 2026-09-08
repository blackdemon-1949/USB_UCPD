/**
  ******************************************************************************
  * @file    apie.c
  * @brief   Advanced PD Intelligence Engine (APIE) - event feed + pipeline.
  *
  * The APIE layer observes the authoritative ST USBPD engine and adds
  * analysis/inference on top; it NEVER replaces or re-enters the PE/PRL/CAD
  * path.  All heavy work is done here in the super loop (APIE_Task), and the
  * only thing done in interrupt/callback context is a bounded copy.
  ******************************************************************************
  */
#include "apie.h"
#include "apie_decode.h"
#include "apie_analyzer.h"
#include "apie_stats.h"
#include "apie_ml.h"
#include "apie_profile.h"
#include "apie_unknown.h"
#include "apie_plan.h"
#include "apie_db.h"
#include "apie_cable.h"
#include "app_pd.h"
#include "app_log.h"
#include "ina226.h"
#include "dtsmon.h"
#include "usbpd_dpm_user.h"
#include "usbpd_core.h"
#include "usbpd_hw_if.h"
#include <stdio.h>
#include <stdlib.h>

extern uint32_t HAL_GetTick(void);

/* ---------------------------------------------------------------------------
 *  State
 * ------------------------------------------------------------------------- */
static APIE_State_t s_state;
static uint8_t      s_safe_mode;
static uint8_t      s_conn_id;
static uint32_t     s_conn_start;
static uint8_t      s_pending_query;   /* 0xFF = none                        */
static uint32_t     s_pending_since;
static uint32_t     s_adv_last_ms;
static uint32_t     s_last_caps_sig;
static uint32_t     s_count_tx;
static uint32_t     s_count_rx;
static uint32_t     s_count_txn;
static uint32_t     s_count_ml;
static float        s_ml_latency_ms_accum;
static uint32_t     s_ml_latency_n;
static APP_PD_Port_t *s_pd;

static float s_last_voltage_mv;
static float s_last_current_ma;
static float s_last_temp_c;

/* Main-loop / compute-budget instrumentation (see DIAGNOSTICS.md). */
static uint32_t s_task_calls;
static uint32_t s_task_period_max_ms;
static uint32_t s_task_period_acc_ms;
static uint32_t s_task_period_last_ms;
static uint32_t s_dwt_cycles_max;
static uint8_t  s_dwt_ready;

/* Unknown-protocol feed state (super-loop scan, never in an ISR). */
static uint32_t s_unknown_last_ts;   /* newest raw-packet ts already scanned */
static uint8_t  s_last_rx_type;      /* last decoded RX message type         */
static uint8_t  s_pend_reset;        /* hard reset observed since last scan */
static uint8_t  s_pend_voltchg;      /* voltage change observed             */
static uint8_t  s_pend_attach;       /* attach observed                     */
static float    s_prev_volt_mv;

static void record_outcome(uint8_t query, uint8_t success, uint32_t latency_ms);

void APIE_Init(void)
{
  APIE_Analyzer_Init();
  APIE_Txn_Init();
  APIE_Feature_Init();
  APIE_Ml_Init();
  APIE_Profile_Init();
  APIE_Unknown_Init();
  APIE_Plan_Init();
  APIE_Db_Init();
  APIE_Cable_Init();
  APIE_Db_StoreProfile(NULL); /* no-op guard */

  s_state = APIE_STATE_OBSERVING;
  s_safe_mode = 0U;
  s_conn_id = 0U;
  s_pending_query = 0xFFU;
  s_task_calls = 0U;
  s_task_period_max_ms = 0U;
  s_task_period_acc_ms = 0U;
  s_task_period_last_ms = 0U;
  s_dwt_cycles_max = 0U;
  /* Enable the DWT cycle counter for cheap compute-budget measurement.  This
     is a benign, one-time Cortex-M7 setup and never runs in an ISR. */
  s_dwt_ready = 0U;
#if defined(DWT) && defined(CoreDebug)
  {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_dwt_ready = 1U;
  }
#endif
  s_pending_since = 0U;
  s_adv_last_ms = 0U;
  s_last_caps_sig = 0U;
  s_count_tx = s_count_rx = s_count_txn = 0U;
  s_count_ml = 0U;
  s_ml_latency_ms_accum = 0.0f;
  s_ml_latency_n = 0U;
  s_last_voltage_mv = 0.0f;
  s_last_current_ma = 0.0f;
  s_last_temp_c = 0.0f;
  s_unknown_last_ts = 0U;
  s_last_rx_type = 0xFFu;
  s_pend_reset = 0U;
  s_pend_voltchg = 0U;
  s_pend_attach = 0U;
  s_prev_volt_mv = 0.0f;
  s_pd = &APP_PD_Port[0];
}

void APIE_SetSafeMode(uint8_t on)
{
  s_safe_mode = (on != 0U) ? 1U : 0U;
  if (s_safe_mode != 0U)
  {
    s_state = APIE_STATE_DISABLED;
  }
}

uint8_t APIE_IsSafeMode(void) { return s_safe_mode; }
APIE_State_t APIE_GetState(void) { return s_state; }

uint8_t APIE_Safety_LimitsSane(void)
{
  /* The PPS step must divide the safety headroom and limits must be > 0. */
  if (APIE_MAX_VOLTAGE_MV == 0U || APIE_MAX_CURRENT_MA == 0U ||
      APIE_PPS_STEP_MV == 0U)
  {
    return 0U;
  }
  if (APIE_MAX_VOLTAGE_MV < 5000U)
  {
    return 0U;
  }
  return 1U;
}

uint8_t APIE_GetExperimentLevel(void) { return APIE_Exp_GetLevel(); }
void APIE_SetExperimentLevel(uint8_t level) { APIE_Exp_SetLevel(level); }

/* ---------------------------------------------------------------------------
 *  Event feed
 * ------------------------------------------------------------------------- */
void APIE_OnCableAttach(uint8_t port, uint8_t cc)
{
  (void)port; (void)cc;
  s_conn_id++;
  s_pending_query = 0xFFU;
  s_pending_since = 0U;
  s_adv_last_ms = HAL_GetTick();
  s_last_caps_sig = 0U;
  s_conn_start = HAL_GetTick();
  APIE_Txn_ResetSession(s_conn_id);
  APIE_Profile_ResetSession(s_conn_id);
  APIE_Unknown_ResetSession();
  APIE_Cable_ResetSession(s_conn_id);
  APIE_Plan_ResetSession(s_conn_id);
  s_unknown_last_ts = 0U;
  s_pend_attach = 1U;
  s_prev_volt_mv = 0.0f;
  if (s_safe_mode == 0U)
  {
    s_state = APIE_STATE_OBSERVING;
  }
}

void APIE_OnCableDetach(uint8_t port)
{
  (void)port;
  const APIE_Profile_t *p = APIE_Profile_Get();
  if (s_safe_mode == 0U && p->valid != 0U)
  {
    (void)APIE_Db_StoreProfile(p);
  }
  s_pending_query = 0xFFU;
}

void APIE_OnSourceCaps(uint8_t port, const uint32_t *pdo, uint8_t n)
{
  uint32_t now = HAL_GetTick();
  uint32_t sig = APIE_Decode_PdoSignature(pdo, n);
  (void)port;

  /* advertisement interval: only when the PDO set actually changes or after a
     reasonable pause (re-advertisements are not counted as new sessions). */
  if (s_adv_last_ms != 0U && sig == s_last_caps_sig)
  {
    uint32_t iv = now - s_adv_last_ms;
    if (iv > 0U && iv < 60000U)
    {
      APIE_Profile_OnAdvInterval(iv);
    }
  }
  s_adv_last_ms = now;
  s_last_caps_sig = sig;

  APIE_Profile_OnCaps(port, pdo, n);
  APIE_EPR_OnSourceCaps(port, pdo, n);
  s_count_rx++;

  /* First seeing caps is the single most informative event: mark status the
     moment we observe the source. */
  if (s_safe_mode == 0U)
  {
    s_state = APIE_STATE_LEARNING;
  }
}

void APIE_OnNotify(uint8_t port, uint32_t event, uint32_t voltage_mv, uint32_t current_ma, uint32_t pdo_pos)
{
  (void)voltage_mv; (void)current_ma; (void)pdo_pos;
  (void)port;
  switch (event)
  {
    case USBPD_NOTIFY_REQUEST_ACCEPTED:
      /* A power Request was accepted -> success txn, record evidence. */
      APIE_Txn_Record(0U, 0x02u /* Request */, APIE_TXN_SUCCESS, s_conn_id);
      s_count_txn++;
      if (s_safe_mode == 0U) { s_state = APIE_STATE_ADAPTIVE; }
      if (voltage_mv != 0U)
      {
        if (s_prev_volt_mv != 0.0f && s_prev_volt_mv != (float)voltage_mv)
        {
          s_pend_voltchg = 1U;
        }
        s_prev_volt_mv = (float)voltage_mv;
      }
      break;
    case USBPD_NOTIFY_REQUEST_REJECTED:
      APIE_Txn_Record(0U, 0x02u, APIE_TXN_REJECT, s_conn_id);
      s_count_txn++;
      break;
    case USBPD_NOTIFY_REQUEST_WAIT:
      APIE_Txn_Record(0U, 0x02u, APIE_TXN_WAIT, s_conn_id);
      s_count_txn++;
      break;
    case USBPD_NOTIFY_HARDRESET_RX:
    case USBPD_NOTIFY_HARDRESET_TX:
      APIE_OnHardReset(port);
      break;
    default:
      break;
  }
}

void APIE_OnRequestSent(uint8_t port, uint8_t index, uint16_t mv, uint16_t ma)
{
  uint8_t msgid = index & 0x03u;
  (void)port; (void)mv; (void)ma;
  APIE_Txn_Begin(0U, APIE_DIR_TX, 0U, 0x02u /* Request */, 0x02u, msgid, s_conn_id, 0U);
  s_count_tx++;
  s_count_txn++;
  s_last_voltage_mv = (float)mv;
}

/* Map a received info/extended reply to the pending query and mark outcome. */
static void inform_reply(uint8_t query, uint8_t success)
{
  if (s_pending_query == query)
  {
    uint32_t now = HAL_GetTick();
    uint32_t lat = (s_pending_since != 0U && now > s_pending_since)
                   ? (now - s_pending_since) : 0U;
    record_outcome(query, success, lat);
    s_pending_query = 0xFFU;
    s_pending_since = 0U;
  }
}

void APIE_OnDataInfo(uint8_t port, uint16_t data_id, const uint8_t *ptr, uint32_t size)
{
  (void)port;
  if (ptr == NULL) { return; }
  switch (data_id)
  {
    case 0x09u: /* USBPD_CORE_INFO_STATUS */
      inform_reply(APIE_QUERY_GET_STATUS, 1U);
      APIE_Profile_OnGetStatusLatency(HAL_GetTick());
      break;
    case 0x0Au: /* PPS_STATUS */
      inform_reply(APIE_QUERY_GET_PPS, 1U);
      break;
    case 0x08u: /* EXTENDED_CAPA */
      inform_reply(APIE_QUERY_SRC_EXT, 1U);
      APIE_Profile_OnExtendedSupport(1U);
      break;
    case 0x0Du: /* MANUFACTURER_INFO */
      inform_reply(APIE_QUERY_MANU_INFO, 1U);
      break;
    case 0x11u: /* USBPD_CORE_BATTERY_CAPABILITY */
    case 0x0Fu: /* USBPD_CORE_BATTERY_STATUS */
      inform_reply(APIE_QUERY_BATTERY, 1U);
      APIE_Profile_OnBattery(1U);
      break;
    default:
      break;
  }
}

void APIE_OnExtendedMessage(uint8_t port, uint16_t type, const uint8_t *data, uint16_t size)
{
  (void)port;
  if (data == NULL) { return; }
  switch (type)
  {
    case 0x02u: /* EXT_STATUS */
      inform_reply(APIE_QUERY_GET_STATUS, 1U);
      APIE_Profile_OnGetStatusLatency(HAL_GetTick());
      break;
    case 0x0Cu: /* EXT_PPS_STATUS */
      inform_reply(APIE_QUERY_GET_PPS, 1U);
      break;
    case 0x07u: /* EXT_MANUFACTURER_INFO */
      inform_reply(APIE_QUERY_MANU_INFO, 1U);
      break;
    case 0x05u: /* EXT_BATTERY_CAPABILITIES */
      inform_reply(APIE_QUERY_BATTERY, 1U);
      APIE_Profile_OnBattery(1U);
      break;
    case 0x0Eu: /* EXT_COUNTRY_CODES */
      inform_reply(APIE_QUERY_COUNTRY, 1U);
      break;
    case 0x0Du: /* EXT_COUNTRY_INFO */
      break;
    case 0x11u: /* EXT_EPR_SOURCE_CAPA */
      break;
    default:
      break;
  }
}

void APIE_OnVdmIdentity(uint8_t port, uint8_t ok)
{
  (void)port;
  inform_reply(APIE_QUERY_IDENTITY, (ok != 0U) ? 1U : 0U);
  APIE_Profile_OnIdentityResult(ok);
}

void APIE_OnVdmSvids(uint8_t port, const uint16_t *svids, uint8_t n, uint8_t ok)
{
  (void)port;
  if (ok != 0U)
  {
    APIE_Profile_OnSvids(svids, n);
  }
  inform_reply(APIE_QUERY_SVIDS, (ok != 0U) ? 1U : 0U);
}

void APIE_OnHardReset(uint8_t port)
{
  (void)port;
  APIE_Profile_OnHardReset();
  s_pend_reset = 1U;
}

/* ---------------------------------------------------------------------------
 *  Unknown-protocol feed.  Runs in the super loop (never an ISR).  Scans the
 *  bounded raw ring for messages the deterministic decoder could not name and
 *  characterizes them as UNKNOWN_SIGNATURE buckets.  The analyzer ring is
 *  owned by this task after capture; reading it here is safe.
 * ------------------------------------------------------------------------- */
static void feed_unknown(void)
{
  uint16_t i;
  uint16_t n = APIE_Analyzer_Count();
  uint32_t max_ts = s_unknown_last_ts;
  for (i = 0U; i < n; i++)
  {
    const APIE_Packet_t *pk = APIE_Analyzer_Get(i);
    if (pk == NULL) { continue; }
    if (pk->ts_ms > max_ts) { max_ts = pk->ts_ms; }
    if (pk->dir != APIE_DIR_RX || pk->ts_ms <= s_unknown_last_ts) { continue; }
    if (pk->sop >= 5u) { continue; } /* hard/cable reset, bist */
    /* Decode deterministically; if un-named, feed the unknown analyzer. */
    {
      APIE_MsgClass_t cls = APIE_Decode_Classify(pk->type, pk->ext);
      if (cls == APIE_MSG_CLS_UNKNOWN)
      {
        const uint8_t *payload = (pk->len >= 2U) ? &pk->data[2] : pk->data;
        uint16_t plen = (pk->len >= 2U) ? (uint16_t)(pk->len - 2U) : 0U;
        (void)APIE_Unknown_Observe(pk->sop, pk->type, pk->msgid, payload, plen,
                                   (uint32_t)s_last_voltage_mv, (uint32_t)s_last_current_ma,
                                   (uint32_t)s_last_temp_c, s_pend_reset, s_pend_voltchg,
                                   s_pend_attach, s_last_rx_type);
      }
      else
      {
        s_last_rx_type = pk->type;
      }
    }
  }
  s_unknown_last_ts = max_ts;
  s_pend_reset = 0U;
  s_pend_voltchg = 0U;
  s_pend_attach = 0U;
}

/* ---------------------------------------------------------------------------
 *  Outcome recording (single source of truth for plan + ML).
 * ------------------------------------------------------------------------- */
static void record_outcome(uint8_t query, uint8_t success, uint32_t latency_ms)
{
  uint8_t has_pps = 0U;
  uint8_t hard = 0U;
  const APIE_Profile_t *p = APIE_Profile_Get();
  if (p != NULL)
  {
    has_pps = p->has_pps;
    hard = p->has_hard;
  }
  APIE_Plan_OnOutcome(query, success, latency_ms);
  APIE_Ml_Observe(query, 0U, has_pps, hard, success);
  APIE_Ml_Anomaly_Observe((float)latency_ms);   /* online latency anomaly detector */
  s_count_ml++;

  /* Persistent negative capability: stop re-tuning queries the source NAKs. */
  if (success == 0U)
  {
    if (query == APIE_QUERY_BATTERY)
    {
      APIE_Profile_OnBattery(2U); /* known Not_Supported */
    }
    if (query == APIE_QUERY_IDENTITY)
    {
      APIE_Profile_OnIdentityResult(0U); /* known NAK */
    }
    if (query == APIE_QUERY_SRC_EXT || query == APIE_QUERY_MANU_INFO)
    {
      APIE_Profile_OnExtendedSupport(0U);
    }
  }
}

/* ---------------------------------------------------------------------------
 *  Query dispatch (bridge to the ST DPM).
 * ------------------------------------------------------------------------- */
int APIE_IssueQuery(uint8_t port, APIE_QueryId_t id)
{
  USBPD_StatusTypeDef st = USBPD_ERROR;
  switch (id)
  {
    case APIE_QUERY_GET_STATUS:
      st = USBPD_DPM_RequestGetStatus(port);
      break;
    case APIE_QUERY_GET_PPS:
      st = USBPD_DPM_RequestGetPPS_Status(port);
      break;
    case APIE_QUERY_SRC_EXT:
      st = USBPD_DPM_RequestGetSourceCapabilityExt(port);
      break;
    case APIE_QUERY_MANU_INFO:
      {
        USBPD_GMIDB_TypeDef req;
        memset(&req, 0, sizeof(req));
        req.ManufacturerInfoTarget = USBPD_MANUFINFO_TARGET_PORT_CABLE_PLUG;
        st = USBPD_DPM_RequestGetManufacturerInfo(port, USBPD_SOPTYPE_SOP, (uint8_t *)&req);
      }
      break;
    case APIE_QUERY_BATTERY:
      {
        uint8_t ref = 0U;
        st = USBPD_DPM_RequestGetBatteryCapability(port, &ref);
      }
      break;
    case APIE_QUERY_COUNTRY:
      st = USBPD_DPM_RequestGetCountryCodes(port);
      break;
    case APIE_QUERY_IDENTITY:
      st = USBPD_DPM_RequestVDM_DiscoveryIdentify(port, USBPD_SOPTYPE_SOP);
      break;
    case APIE_QUERY_SVIDS:
      st = USBPD_DPM_RequestVDM_DiscoverySVID(port, USBPD_SOPTYPE_SOP);
      break;
    case APIE_QUERY_MODES:
      {
        const APIE_Profile_t *p = APIE_Profile_Get();
        if (p == NULL || p->n_svid == 0U)
        {
          return -1;
        }
        st = USBPD_DPM_RequestVDM_DiscoveryMode(port, USBPD_SOPTYPE_SOP, p->svid[0]);
      }
      break;
    default:
      return -1;
  }
  if (st == USBPD_OK)
  {
    APIE_Plan_OnIssue((uint8_t)id, HAL_GetTick());
    return 0;
  }
  return -1;
}

/* ---------------------------------------------------------------------------
 *  Super-loop pipeline
 * ------------------------------------------------------------------------- */
/* --- main-loop / compute-budget getters (DIAGNOSTICS.md) ------------------ */
uint32_t APIE_Diag_TaskCalls(void)          { return s_task_calls; }
uint32_t APIE_Diag_TaskPeriodMaxMs(void)    { return s_task_period_max_ms; }
uint32_t APIE_Diag_TaskPeriodAvgMs(void)
{
  return (s_task_calls > 0U) ? (s_task_period_acc_ms / s_task_calls) : 0U;
}
uint32_t APIE_Diag_DwtCyclesMax(void)       { return s_dwt_cycles_max; }
uint8_t  APIE_Diag_DwtReady(void)           { return s_dwt_ready; }

void APIE_Task(void)
{
  uint32_t now = HAL_GetTick();
  static uint32_t next_txn_check;
  uint32_t task_delta;
  uint32_t cyc = 0U;
  uint8_t attached;
  uint8_t contract;
  int q;

  /* Main-loop budget instrumentation (bounded, no persistent writes). */
  if (s_task_period_last_ms != 0U)
  {
    task_delta = now - s_task_period_last_ms;
    s_task_period_acc_ms += task_delta;
    if (task_delta > s_task_period_max_ms)
    {
      s_task_period_max_ms = task_delta;
    }
  }
  s_task_period_last_ms = now;
  s_task_calls++;
#if defined(DWT)
  if (s_dwt_ready != 0U)
  {
    cyc = DWT->CYCCNT;
  }
#endif

  if (s_safe_mode != 0U)
  {
    return; /* intelligence disabled; PD/CDC/HAL all keep running */
  }

  attached = s_pd->Attached;
  contract = s_pd->Contract;

  /* bounded TX/RX totals are maintained from events */
  (void)attached;

  /* Timeout a pending informational query. */
  if (s_pending_query != 0xFFU && s_pending_since != 0U &&
      (now - s_pending_since) > APIE_QUERY_TIMEOUT_MS)
  {
    record_outcome(s_pending_query, 0U, APIE_QUERY_TIMEOUT_MS);
    s_pending_query = 0xFFU;
    s_pending_since = 0U;
  }

  /* Timeout open transactions. */
  if ((int32_t)(now - next_txn_check) >= 0)
  {
    next_txn_check = now + 200U;
    APIE_Txn_TimeoutAll(s_conn_id, now);
  }

  /* Only run the intelligent query scheduler once we have an explicit contract
     (informational extended/VDM requests need a state where the source will
     answer) and only under experiment level R1 or above. */
  if ((s_pending_query == 0xFFU) && (contract != 0U) && APIE_Exp_Allows(APIE_EXP_R1))
  {
    q = APIE_Plan_Select(now, attached, contract);
    if (q >= 0)
    {
      if (APIE_IssueQuery(0U, (APIE_QueryId_t)q) == 0)
      {
        s_pending_query = (uint8_t)q;
        s_pending_since = now;
      }
    }
  }

  /* Update the cached power/temperature context used by features/ML. */
  s_last_voltage_mv = (float)APP_PD_GetVbusMv();
  s_last_current_ma = 0.0f;
  if (INA226_IsPresent() && INA226_DataFresh())
  {
    s_last_current_ma = (float)(INA226_GetCurUa() / 1000);
    s_last_voltage_mv = (float)INA226_GetBusMv();
  }
  s_last_temp_c = DTSMON_DataFresh() ? (float)DTSMON_GetTempC() : 0.0f;

  /* Characterize un-named RX messages as UNKNOWN_SIGNATURE buckets. */
  if (APIE_Exp_Allows(APIE_EXP_R0))
  {
    feed_unknown();
  }

  /* Track the APIE per-call compute budget in cycles (diagnostics only). */
#if defined(DWT)
  if (s_dwt_ready != 0U)
  {
    uint32_t used = DWT->CYCCNT - cyc;
    if (used > s_dwt_cycles_max)
    {
      s_dwt_cycles_max = used;
    }
  }
#endif
}

/* ---------------------------------------------------------------------------
 *  Build a feature vector over the current knowledge (for CLI / ML).
 * ------------------------------------------------------------------------- */
static void build_current_features(APIE_FeatureVec_t *fv)
{
  uint8_t has_pps = 0U;
  const APIE_Profile_t *p = APIE_Profile_Get();
  if (p != NULL && p->has_pps != 0U)
  {
    has_pps = 1U;
  }
  memset(fv, 0, sizeof(*fv));
  fv->v[3] = s_last_voltage_mv;
  fv->v[4] = s_last_current_ma;
  fv->v[5] = s_last_voltage_mv * s_last_current_ma;
  fv->v[6] = s_last_temp_c;
  fv->v[8] = (float)has_pps;
  if (s_count_txn > 0U) { fv->v[1] = 1.0f; }
}

/* ---------------------------------------------------------------------------
 *  CLI entry points
 * ------------------------------------------------------------------------- */
void APIE_CliStatus(void)
{
  char b[128];
  APP_LOG_Printf("apie: state=%s safe=%s exp=%s\r\n",
                 (s_state == APIE_STATE_DISABLED) ? "disabled" :
                 (s_state == APIE_STATE_OBSERVING) ? "observing" :
                 (s_state == APIE_STATE_LEARNING) ? "learning" : "adaptive",
                 s_safe_mode ? "ON" : "off",
                 APIE_Exp_LevelName(APIE_Exp_GetLevel()));
  APP_LOG_Printf("  conn=%u  txn=%lu  ml=%lu  rx=%lu tx=%lu\r\n",
                 (unsigned)s_conn_id, (unsigned long)s_count_txn,
                 (unsigned long)s_count_ml, (unsigned long)s_count_rx,
                 (unsigned long)s_count_tx);
  APIE_Analyzer_Stats(b, sizeof(b));
  APP_LOG_Printf("  %s\r\n", b);
  APIE_Db_Status(b, sizeof(b));
  APP_LOG_Printf("  %s\r\n", b);
  APIE_Unknown_Stats(b, sizeof(b));
  APP_LOG_Printf("  %s\r\n", b);
  APP_LOG_Printf("  pps cap=%s  epr=%s  epr_pwr=%s\r\n",
                 (APIE_Profile_Get()->has_pps) ? "yes" : "no",
                 (APIE_EPR_Get()->epr_capable) ? "yes" : "no",
                 APIE_EPR_PowerAllowed() ? "allowed" : "blocked(hw)");
}

void APIE_CliPdStats(void)
{
  APP_LOG_Printf("pd stats: irq=%lu ok=%lu err=%lu ovr=%lu hrst=%lu typecevt=%lu\r\n",
                 (unsigned long)g_usbpd_dbg.ucpd_irq,
                 (unsigned long)g_usbpd_dbg.rxmsgend_ok,
                 (unsigned long)g_usbpd_dbg.rxmsgend_err,
                 (unsigned long)g_usbpd_dbg.rxovr,
                 (unsigned long)g_usbpd_dbg.rxhrstdet,
                 (unsigned long)g_usbpd_dbg.typecevt);
  APP_LOG_Printf("          tx=%lu disc=%lu abt=%lu und=%lu last_hdr=0x%04X sop=%u\r\n",
                 (unsigned long)g_usbpd_dbg.txmsgsent,
                 (unsigned long)g_usbpd_dbg.txmsgdisc,
                 (unsigned long)g_usbpd_dbg.txmsgabt,
                 (unsigned long)g_usbpd_dbg.txund,
                 (unsigned)g_usbpd_dbg.last_rx_hdr,
                 (unsigned)g_usbpd_dbg.last_rx_sop);
  APIE_Txn_Dump();
}

void APIE_CliPdPackets(uint8_t show_all)
{
  APIE_Analyzer_Dump(show_all);
}

void APIE_CliSource(void)
{
  APIE_Profile_Print();
  APIE_Cable_Dump();
  APIE_EPR_Dump();
}

void APIE_CliFingerprint(void)
{
  const APIE_Profile_t *p = APIE_Profile_Get();
  APP_LOG_Printf("fingerprint: sig=0x%08lX pdo=%u pps=%s svid=%u battery=%d ident=%d epr=%s\r\n",
                 (unsigned long)APIE_Profile_Signature(), (unsigned)p->n_pdo,
                 p->has_pps ? "y" : "n", (unsigned)p->n_svid,
                 (int)p->battery_supported, (int)p->identity_supported,
                 p->has_epr ? "y" : "n");
}

void APIE_CliTxnList(void)
{
  APIE_Txn_Dump();
}

void APIE_CliCounters(void)
{
  APIE_Analyzer_Stats("", 0);
  APP_LOG_Printf("txn_hist=%u txn_active=%u unknown=%u ml_calls=%lu\r\n",
                 (unsigned)APIE_Txn_HistoryCount(),
                 (unsigned)APIE_Txn_ActiveCount(),
                 (unsigned)APIE_Unknown_Count(),
                 (unsigned long)s_count_ml);
}

void APIE_CliSafety(void)
{
  APP_LOG_Printf("safety: vbus_max=%lu mV  cur_max=%lu mA  pps_step=%lu mV\r\n",
                 (unsigned long)APIE_MAX_VOLTAGE_MV, (unsigned long)APIE_MAX_CURRENT_MA,
                 (unsigned long)APIE_PPS_STEP_MV);
  APP_LOG_Printf("  hw epr_power=%s  vbus_adc=%s  dplus_dminus=%s  emarker=%s\r\n",
                 APIE_HW_EPR_POWER_ENABLED ? "on" : "off",
                 APIE_HW_HAS_VBUS_ADC ? "on" : "off",
                 APIE_HW_HAS_DPLUS_DMINUS ? "on" : "off",
                 APIE_HW_CABLE_EMARKER ? "on" : "off");
  APP_LOG_Printf("  exp level: %s\r\n", APIE_Exp_LevelName(APIE_Exp_GetLevel()));
}

void APIE_CliFeature(void)
{
  APIE_FeatureVec_t fv;
  uint8_t i;
  build_current_features(&fv);
  for (i = 0U; i < APIE_FEATURE_COUNT; i++)
  {
    APP_LOG_Printf("  %-14s %8.3f\r\n", APIE_Feature_Name(i), (double)fv.v[i]);
  }
}

void APIE_CliMlStatus(void)
{
  const APIE_MlModel_t *m = APIE_Ml_GetModel();
  APP_LOG_Printf("ml model: id=%u ver=%u featv=%u kind=%u crc=0x%08lX validate=%s\r\n",
                 (unsigned)m->meta.id, (unsigned)m->meta.version,
                 (unsigned)m->meta.feature_version, (unsigned)m->meta.kind,
                 (unsigned long)m->meta.crc32, APIE_Ml_Validate() ? "ok" : "BAD");
  APP_LOG_Printf("  class0=%lu class1=%lu  useful-rate=%.2f\r\n",
                 (unsigned long)m->nclass[0U], (unsigned long)m->nclass[1U],
                 (double)(m->nclass[0U] + m->nclass[1U]) > 0.0 ?
                   (double)((float)m->nclass[1U]/(float)(m->nclass[0U]+m->nclass[1U])) : 0.0);
  APP_LOG_Printf("  anomaly: trained=%s n=%lu mean=%.1fms std=%.1fms\r\n",
                 APIE_Ml_Anomaly_Trained() ? "yes" : "no",
                 (unsigned long)APIE_Ml_Anomaly_Count(),
                 (double)APIE_Ml_Anomaly_Mean(), (double)APIE_Ml_Anomaly_Std());
}

void APIE_CliPredict(char *hex)
{
  /* hex is a query id (0..8) or a full 48-hex feature vector.  For simplicity
     accept a query id. */
  unsigned long v = 0UL;
  APIE_FeatureVec_t fv;
  float p;
  if (hex != NULL)
  {
    v = strtoul(hex, NULL, 16);
  }
  build_current_features(&fv);
  {
    uint8_t q = (uint8_t)(v & 0x0Fu);
    p = APIE_Ml_PredictUseful(&fv, q);
    APP_LOG_Printf("predict query=0x%02lX useful=%.3f nb_class=%u tree=%u\r\n",
                   v, (double)p, (unsigned)APIE_Ml_Classify(&fv, q),
                   (unsigned)APIE_Tree_ClassifyUseful(&fv, q));
  }
}

void APIE_CliUnknown(void)
{
  APIE_Unknown_Dump();
}

void APIE_CliScheduler(void)
{
  APIE_Plan_Dump();
}

void APIE_CliDbStatus(void)
{
  char b[96];
  APIE_Db_Dump();
  APIE_Db_Status(b, sizeof(b));
  APP_LOG_Printf("%s\r\n", b);
}

void APIE_CliExperiment(void)
{
  APP_LOG_Write("experiments:\r\n");
  APP_LOG_Printf("  level     : %s\r\n", APIE_Exp_LevelName(APIE_Exp_GetLevel()));
  APP_LOG_Printf("  R0 observe: %s\r\n", APIE_Exp_Allows(APIE_EXP_R0) ? "ON" : "off");
  APP_LOG_Printf("  R1 info   : %s\r\n", APIE_Exp_Allows(APIE_EXP_R1) ? "ON" : "off");
  APP_LOG_Printf("  R2 power  : %s\r\n", APIE_Exp_Allows(APIE_EXP_R2) ? "ON" : "off");
  APP_LOG_Printf("  R3 state  : %s\r\n", APIE_Exp_Allows(APIE_EXP_R3) ? "ON" : "off");
  APP_LOG_Printf("  R4 vendor : %s\r\n", APIE_Exp_Allows(APIE_EXP_R4) ? "ON" : "off");
}
