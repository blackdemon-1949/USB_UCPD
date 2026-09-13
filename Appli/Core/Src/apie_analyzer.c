/**
  ******************************************************************************
  * @file    apie_analyzer.c
  * @brief   Raw packet ring, transaction correlation, feature extraction.
  ******************************************************************************
  */
#include "apie_analyzer.h"
#include "apie_stats.h"
#include "app_log.h"
#include <stdio.h>

extern uint32_t HAL_GetTick(void);

/* ===========================================================================
 *  Raw packet ring
 * ========================================================================= */
static APIE_Packet_t s_ring[APIE_PACKET_RING];
static uint16_t s_head;
static uint16_t s_tail;
static uint16_t s_count;
static uint16_t s_dropped;
static uint32_t s_last_ts;
static uint32_t s_rx_total;
static uint32_t s_tx_total;
static uint32_t s_rx_by_sop[8];

void APIE_Analyzer_Init(void)
{
  memset(s_ring, 0, sizeof(s_ring));
  s_head = s_tail = s_count = s_dropped = 0;
  s_last_ts = 0;
  s_rx_total = s_tx_total = 0;
  memset(s_rx_by_sop, 0, sizeof(s_rx_by_sop));
}

/* Minimal capture: called from a short UCPD RX-complete hook.  `buf` points at
   the ST PRL RX buffer; we only read it (the DMA transfer is complete at
   RXMSGEND) and copy the bytes into our own private ring.  Ownership of the ST
   buffer and its DMA re-arm are completely untouched. */
void APIE_Analyzer_CaptureRaw(uint8_t port, uint8_t sop, const uint8_t *buf, uint32_t n)
{
  APIE_Packet_t *pk;
  uint16_t next;
  uint32_t now = HAL_GetTick();
  uint16_t hdr;
  uint32_t keep;
  (void)port;

  if (buf == NULL)
  {
    return;
  }

  /* Only a complete SOP-based message (no hard/cable reset) has a header. */
  if (sop == 5u || sop == 6u || sop == 7u)
  {
    return;
  }

  next = (uint16_t)((s_head + 1U) % APIE_PACKET_RING);
  if (next == s_tail)
  {
    /* Ring full: drop and advance paste a real (oldest) slot. */
    s_dropped++;
    return;
  }

  pk = &s_ring[s_head];
  memset(pk, 0, sizeof(*pk));
  hdr = (uint16_t)(buf[0] | (uint16_t)((uint16_t)buf[1] << 8U));

  pk->ts_ms = now;
  pk->dir   = APIE_DIR_RX;
  pk->sop   = sop;
  pk->hdr   = hdr;
  {
    APIE_Header_t h;
    APIE_Decode_Header(hdr, &h);
    pk->type     = h.type;
    pk->msgid    = h.msgid;
    pk->ext      = h.extended;
    pk->nobjects = h.nobjects;
  }

  keep = (n > (uint32_t)APIE_PACKET_MAX) ? (uint32_t)APIE_PACKET_MAX : n;
  pk->len = (uint16_t)keep;
  if (keep > 0U)
  {
    memcpy(pk->data, buf, keep);
  }

  s_head = next;
  s_count++;
  if (s_rx_total < 0xFFFFFFFFUL) { s_rx_total++; }
  if (sop < 8U) { s_rx_by_sop[sop]++; }
  s_last_ts = now;
}

void APIE_Analyzer_TxCapture(uint8_t sop, uint16_t hdr, const uint8_t *data, uint32_t n)
{
  APIE_Packet_t *pk;
  uint16_t next;
  uint32_t keep;
  APIE_Header_t h;

  next = (uint16_t)((s_head + 1U) % APIE_PACKET_RING);
  if (next == s_tail)
  {
    s_dropped++;
    return;
  }
  pk = &s_ring[s_head];
  memset(pk, 0, sizeof(*pk));
  pk->ts_ms = HAL_GetTick();
  pk->dir = APIE_DIR_TX;
  pk->sop = sop;
  pk->hdr = hdr;
  APIE_Decode_Header(hdr, &h);
  pk->type = h.type;
  pk->msgid = h.msgid;
  pk->ext = h.extended;
  pk->nobjects = h.nobjects;
  keep = (n > (uint32_t)APIE_PACKET_MAX) ? (uint32_t)APIE_PACKET_MAX : n;
  pk->len = (uint16_t)keep;
  if (keep > 0U) { memcpy(pk->data, data, keep); }
  s_head = next;
  s_count++;
  if (s_tx_total < 0xFFFFFFFFUL) { s_tx_total++; }
}

uint16_t APIE_Analyzer_Count(void)      { return s_count; }
uint16_t APIE_Analyzer_Dropped(void)    { return s_dropped; }

const APIE_Packet_t *APIE_Analyzer_Get(uint16_t idx)
{
  if (idx >= s_count)
  {
    return NULL;
  }
  return &s_ring[(uint16_t)((s_tail + idx) % APIE_PACKET_RING)];
}

void APIE_Analyzer_Clear(void)
{
  s_head = s_tail = s_count = 0;
}

void APIE_Analyzer_Export(void)
{
  uint16_t i;
  /* Machine-readable capture export: one line per packet:
     "apie_cap ts=.. dir=.. sop=.. msgid=.. type=.. ext=.. n=.. hdr=0x.. hex=<payload>"
     The host replay tool (tools/apie_replay.py) parses this and re-runs the
     decoder + feature extraction over every packet. */
  for (i = 0U; i < s_count; i++)
  {
    const APIE_Packet_t *pk = APIE_Analyzer_Get(i);
    uint8_t j;
    if (pk == NULL) { continue; }
    APP_LOG_Printf("apie_cap ts=%lu dir=%s sop=%u msgid=%u type=%u ext=%u n=%u hdr=0x%04X hex=",
                   (unsigned long)pk->ts_ms,
                   (pk->dir == APIE_DIR_TX) ? "TX" : "RX",
                   (unsigned)pk->sop, (unsigned)pk->msgid, (unsigned)pk->type,
                   (unsigned)pk->ext, (unsigned)pk->nobjects, (unsigned)pk->hdr);
    for (j = 0U; j < pk->len; j++)
    {
      APP_LOG_Printf("%02X", (unsigned)pk->data[j]);
    }
    APP_LOG_Write("\r\n");
  }
}

void APIE_Analyzer_Stats(char *out, uint32_t outsz)
{
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  snprintf(out, outsz, "rx=%lu tx=%lu buf=%u ring=%u dropped=%u last_ms=%lu",
           (unsigned long)s_rx_total, (unsigned long)s_tx_total,
           (unsigned)s_count, (unsigned)APIE_PACKET_RING,
           (unsigned)s_dropped, (unsigned long)s_last_ts);
}

void APIE_Analyzer_Dump(uint8_t show_all)
{
  uint16_t i;
  uint16_t start;
  uint16_t n = s_count;
  uint8_t cols = 0;

  if (n == 0U)
  {
    APP_LOG_Write("raw: no packets captured\r\n");
    return;
  }
  if (show_all == 0U)
  {
    /* Show the last 16. */
    start = (n > 16U) ? (uint16_t)(n - 16U) : 0U;
  }
  else
  {
    start = 0U;
  }

  APP_LOG_Printf("raw: %u packet(s) in ring (%u dropped)\r\n", n, (unsigned)s_dropped);
  for (i = start; i < n && cols < 200U; i++)
  {
    const APIE_Packet_t *pk = &s_ring[(uint16_t)((s_tail + i) % APIE_PACKET_RING)];
    char tn[40];
    uint8_t j;
    APIE_Decode_TypeNameN(pk->type, pk->ext, pk->nobjects, tn, sizeof(tn));
    APP_LOG_Printf("  [%03u] %lu.%03lu %s %-18s id=%u n=%u hdr=0x%04X  ",
                   (unsigned)(i % 1000U),
                   (unsigned long)(pk->ts_ms / 1000U),
                   (unsigned long)(pk->ts_ms % 1000U),
                   (pk->dir == APIE_DIR_TX) ? "TX" : "RX",
                   tn, (unsigned)pk->msgid, (unsigned)pk->nobjects, (unsigned)pk->hdr);
    for (j = 0U; j < pk->len; j++)
    {
      APP_LOG_Printf("%02X", (unsigned)pk->data[j]);
    }
    APP_LOG_Write("\r\n");
    cols++;
  }
}

/* ===========================================================================
 *  Transaction engine
 * ========================================================================= */
static APIE_Txn_t s_txn[APIE_TXN_MAX];
static APIE_Txn_t s_hist[APIE_TXN_HIST];
static uint16_t s_hist_count;
static uint8_t s_conn;

void APIE_Txn_Init(void)
{
  memset(s_txn, 0, sizeof(s_txn));
  memset(s_hist, 0, sizeof(s_hist));
  s_hist_count = 0;
}

void APIE_Txn_ResetSession(uint8_t conn_id)
{
  uint8_t i;
  s_conn = conn_id;
  for (i = 0U; i < APIE_TXN_MAX; i++)
  {
    if (s_txn[i].active != 0U && s_txn[i].conn_id != conn_id)
    {
      s_txn[i].active = 0U;
    }
  }
}

void APIE_Txn_Begin(uint8_t port, uint8_t dir, uint8_t sop, uint8_t tx_type,
                    uint8_t exp_type, uint8_t msgid, uint8_t conn_id, uint8_t attempt)
{
  uint8_t i;
  uint8_t slot = 0xFFU;
  (void)port;
  for (i = 0U; i < APIE_TXN_MAX; i++)
  {
    if (s_txn[i].active == 0U)
    {
      slot = i;
      break;
    }
  }
  if (slot == 0xFFU)
  {
    return; /* table full, old entries are timed out by APIE_Txn_TimeoutAll */
  }
  s_txn[slot].active = 1U;
  s_txn[slot].dir = dir;
  s_txn[slot].sop = sop;
  s_txn[slot].tx_type = tx_type;
  s_txn[slot].exp_type = exp_type;
  s_txn[slot].msgid = msgid;
  s_txn[slot].ts_tx = HAL_GetTick();
  s_txn[slot].ts_rx = 0U;
  s_txn[slot].latency_ms = 0U;
  s_txn[slot].result = APIE_TXN_OPEN;
  s_txn[slot].conn_id = conn_id;
  s_txn[slot].attempt = attempt;
}

void APIE_Txn_Record(uint8_t port, uint8_t type, uint8_t result, uint8_t conn_id)
{
  uint8_t i;
  APIE_Txn_t *slot = NULL;
  (void)port;
  for (i = 0U; i < APIE_TXN_MAX; i++)
  {
    if ((s_txn[i].active != 0U) && (s_txn[i].conn_id == conn_id) &&
        (s_txn[i].exp_type == type))
    {
      slot = &s_txn[i];
      break;
    }
  }
  if (slot == NULL)
  {
    return;
  }
  slot->active = 0U;
  slot->result = (APIE_TxnResult_t)result;
  slot->ts_rx = HAL_GetTick();
  slot->latency_ms = ((slot->ts_rx >= slot->ts_tx) ? (slot->ts_rx - slot->ts_tx) : 0U);

  /* Push into history (bounded FIFO-style: overwrite oldest). */
  {
    uint16_t idx = s_hist_count;
    if (idx >= APIE_TXN_HIST)
    {
      /* shift down */
      for (i = 0U; i < APIE_TXN_HIST - 1U; i++)
      {
        s_hist[i] = s_hist[i + 1U];
      }
      idx = (uint16_t)(APIE_TXN_HIST - 1U);
    }
    else
    {
      s_hist_count++;
    }
    s_hist[idx] = *slot;
  }
}

void APIE_Txn_OnRx(uint8_t port, uint8_t sop, uint8_t msgid, uint8_t type, uint8_t conn_id)
{
  uint8_t i;
  (void)port;
  (void)sop;
  for (i = 0U; i < APIE_TXN_MAX; i++)
  {
    if ((s_txn[i].active != 0U) && (s_txn[i].conn_id == conn_id))
    {
      if (s_txn[i].exp_type == type)
      {
        APIE_Txn_Record(port, type, APIE_TXN_SUCCESS, conn_id);
        return;
      }
    }
  }
  /* Unmatched RX: could be a spontaneous advertisement (Source_Capabilities,
     e.g. re-advertise) - not a transaction response. */
}

void APIE_Txn_TimeoutAll(uint8_t conn_id, uint32_t now)
{
  uint8_t i;
  for (i = 0U; i < APIE_TXN_MAX; i++)
  {
    if ((s_txn[i].active != 0U) && (s_txn[i].conn_id == conn_id))
    {
      if ((now - s_txn[i].ts_tx) > APIE_QUERY_TIMEOUT_MS)
      {
        APIE_Txn_Record(0U, s_txn[i].exp_type, APIE_TXN_TIMEOUT, conn_id);
      }
    }
  }
}

uint16_t APIE_Txn_ActiveCount(void)
{
  uint16_t c = 0U;
  uint8_t i;
  for (i = 0U; i < APIE_TXN_MAX; i++)
  {
    if (s_txn[i].active != 0U) { c++; }
  }
  return c;
}

uint16_t APIE_Txn_HistoryCount(void)
{
  return s_hist_count;
}

int APIE_Txn_Get(const APIE_Txn_t **hist)
{
  if (hist != NULL)
  {
    *hist = s_hist;
  }
  return (int)s_hist_count;
}

void APIE_Txn_Dump(void)
{
  uint16_t i;
  uint8_t j;
  if (s_hist_count == 0U)
  {
    APP_LOG_Write("txn: no completed transactions\r\n");
    return;
  }
  APP_LOG_Printf("txn: %u completed (%u active)\r\n",
                 (unsigned)s_hist_count, (unsigned)APIE_Txn_ActiveCount());
  for (i = 0U; i < s_hist_count; i++)
  {
    const char *r = "?";
    char tn[40];
    j = (uint8_t)s_hist[i].result;
    switch ((APIE_TxnResult_t)j)
    {
      case APIE_TXN_SUCCESS: r = "SUCCESS"; break;
      case APIE_TXN_REJECT: r = "REJECT"; break;
      case APIE_TXN_WAIT: r = "WAIT"; break;
      case APIE_TXN_NOT_SUPPORTED: r = "NOT_SUPPORTED"; break;
      case APIE_TXN_TIMEOUT: r = "TIMEOUT"; break;
      case APIE_TXN_ERROR: r = "ERROR"; break;
      case APIE_TXN_UNKNOWN: r = "UNKNOWN"; break;
      case APIE_TXN_OPEN: r = "OPEN"; break;
    }
    APIE_Decode_TypeName(s_hist[i].exp_type, 0U, tn, sizeof(tn));
    APP_LOG_Printf("  %02u  %-16s -> %-13s  %lu ms  conn=%u\r\n",
                   (unsigned)(i + 1U), tn, r,
                   (unsigned long)s_hist[i].latency_ms,
                   (unsigned)s_hist[i].conn_id);
  }
}

/* ===========================================================================
 *  Feature extraction
 * ========================================================================= */
static APIE_StatAccum_t s_latency;
static uint32_t s_last_pkt_ts;
static uint32_t s_pkt_rate_n;
static uint32_t s_pkt_rate_win_start;
static float s_pkt_rate;

static const char *s_feature_names[APIE_FEATURE_COUNT] =
{
  "latency_ms",
  "txn_result",
  "pdo_count",
  "voltage_mv",
  "current_ma",
  "power_mw",
  "soc_temp_c",
  "packet_rate",
  "has_pps",
  "msgid_delta",
  "nobjects",
  "adv_interval_ms"
};

void APIE_Feature_Init(void)
{
  APIE_Stats_Init(&s_latency);
  s_last_pkt_ts = 0U;
  s_pkt_rate_n = 0U;
  s_pkt_rate_win_start = 0U;
  s_pkt_rate = 0.0f;
}

const char *APIE_Feature_Name(uint8_t i)
{
  if (i >= APIE_FEATURE_COUNT)
  {
    return "?";
  }
  return s_feature_names[i];
}

void APIE_Feature_Build(uint8_t conn_id, uint32_t voltage_mv, uint32_t current_ma,
                        uint32_t soc_temp_c, APIE_FeatureVec_t *out)
{
  uint32_t now = HAL_GetTick();
  (void)conn_id;
  if (out == NULL)
  {
    return;
  }
  memset(out, 0, sizeof(*out));

  /* latency: last completed txn latency */
  if (s_hist_count > 0U)
  {
    out->v[0] = (float)s_hist[s_hist_count - 1U].latency_ms;
    APIE_Stats_Update(&s_latency, (float)out->v[0]);
  }
  out->v[1] = (s_hist_count > 0U) ? (float)s_hist[s_hist_count - 1U].result : 0.0f;
  out->v[3] = (float)voltage_mv;
  out->v[4] = (float)current_ma;
  out->v[5] = (float)((uint64_t)voltage_mv * current_ma / 1000UL);
  out->v[6] = (float)soc_temp_c;
  out->v[2] = 0.0f;

  /* Packet rate over a 1 s sliding window. */
  if ((now - s_pkt_rate_win_start) >= 1000UL)
  {
    s_pkt_rate = (float)s_pkt_rate_n * 1000.0f / (float)(now - s_pkt_rate_win_start);
    s_pkt_rate_n = 0U;
    s_pkt_rate_win_start = now;
  }
  else
  {
    s_pkt_rate_n++;
  }
  out->v[7] = s_pkt_rate;

  /* msgid delta + nobjects from the last raw packet. */
  if (s_count > 0U)
  {
    const APIE_Packet_t *pk = &s_ring[(uint16_t)((s_tail + (uint16_t)(s_count - 1U)) % APIE_PACKET_RING)];
    out->v[10] = (float)pk->nobjects;
    out->v[11] = (s_last_pkt_ts != 0U) ? (float)(pk->ts_ms - s_last_pkt_ts) : 0.0f;
    s_last_pkt_ts = pk->ts_ms;
  }
}


