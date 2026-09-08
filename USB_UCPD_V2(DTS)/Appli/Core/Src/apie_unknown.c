/**
  ******************************************************************************
  * @file    apie_unknown.c
  * @brief   Unknown-protocol characterization (UNKNOWN_SIGNATURE).
  ******************************************************************************
  */
#include "apie_unknown.h"
#include "app_log.h"
#include <stdio.h>
#include <math.h>

extern uint32_t HAL_GetTick(void);

static APIE_UnknownKind_t s_kind[APIE_UNKNOWN_KINDS];
static uint16_t s_count;
static uint8_t s_last_type = 0xFFu;   /* for response-relationship tracking */

static uint32_t sig_for(uint8_t sop, uint8_t type, const uint8_t *payload, uint16_t len)
{
  uint32_t h = 2166136261u;
  uint8_t i;
  h ^= (uint32_t)sop; h *= 16777619u;
  h ^= (uint32_t)type; h *= 16777619u;
  for (i = 0U; i < 6U && i < len; i++)
  {
    h ^= (uint32_t)payload[i]; h *= 16777619u;
  }
  return h;
}

static uint8_t popcount8(uint8_t x)
{
  x = (uint8_t)(x - ((x >> 1) & 0x55u));
  x = (uint8_t)(((x & 0x33u) + ((x >> 2) & 0x33u)));
  x = (uint8_t)((x + (x >> 4)) & 0x0Fu);
  return x;
}

/* log2(1..16) * 4096, embedded (no libm).  Max nibble entropy is 4 bits. */
static const int32_t s_log2[17] = {
  0,
  0, 4096, 6492, 8192, 9510, 10592, 11497, 12288, 12992, 13622,
  14200, 14723, 15210, 15655, 16077, 16473
};

/* Shannon entropy of the first payload bytes, scaled to 0..255, integer-only. */
static uint8_t entropy_of(const uint8_t *p, uint16_t len)
{
  int32_t hist[16];
  int32_t i;
  int32_t tot;
  int32_t acc = 0;
  memset(hist, 0, sizeof(hist));
  if (len > 8U) { len = 8U; }
  if (len == 0U) { return 0U; }
  for (i = 0; i < (int32_t)len; i++)
  {
    hist[p[i] & 0x0Fu]++;
    hist[(p[i] >> 4) & 0x0Fu]++;
  }
  tot = (int32_t)len * 2;
  if (tot <= 0) { return 0U; }
  for (i = 0; i < 16; i++)
  {
    if (hist[i] > 0)
    {
      /* -p*log2(p) = (c/tot)*log2(tot/c); scaled by 4096. */
      int32_t bits = (s_log2[tot] - s_log2[hist[i]]);
      if (bits < 0) { bits = 0; }
      acc += (int32_t)hist[i] * bits;
    }
  }
  /* acc/tot^2 = average bits * 4096/tot?  -> bits*4096 = acc/tot (avg). */
  if (tot == 0) { return 0U; }
  {
    int32_t avg_bits4096 = acc / tot;   /* average -log2(p) * 4096 */
    int32_t bits = (avg_bits4096 + 2048) / 4096;  /* rounding to bits */
    if (bits > 4) { bits = 4; }
    return (uint8_t)((bits * 255) / 4);
  }
}

void APIE_Unknown_Init(void)
{
  memset(s_kind, 0, sizeof(s_kind));
  s_count = 0U;
  s_last_type = 0xFFu;
}

void APIE_Unknown_ResetSession(void)
{
  uint16_t i;
  for (i = 0U; i < APIE_UNKNOWN_KINDS; i++)
  {
    s_kind[i].session_count = 0U;
  }
  s_last_type = 0xFFu;
}

int APIE_Unknown_Observe(uint8_t sop, uint8_t type, uint8_t msgid, const uint8_t *payload,
                         uint16_t len, uint32_t vbus_mv, uint32_t current_ma, uint32_t temp_c,
                         uint8_t reset_occurred, uint8_t voltchg_occurred,
                         uint8_t attach_occurred, uint8_t prev_type)
{
  uint32_t sig = sig_for(sop, type, payload, len);
  uint16_t i;
  uint8_t j;
  uint32_t now = HAL_GetTick();
  uint8_t cap = (len > 8U) ? 8U : len;

  for (i = 0U; i < s_count; i++)
  {
    if (s_kind[i].sig == sig)
    {
      APIE_UnknownKind_t *k = &s_kind[i];
      /* timing / frequency */
      if (k->last_ms != 0U && k->session_count > 0U)
      {
        uint32_t iv = now - k->last_ms;
        if (iv > 0U)
        {
          k->sum_interval_ms += iv;
          k->n_interval++;
          if (k->n_interval > 0U)
          {
            uint32_t mean = k->sum_interval_ms / k->n_interval;
            k->freq_x1000 = (mean > 0U) ? (1000000UL / mean) : 0U;
          }
        }
      }
      k->session_count++;
      k->cross_session_count++;
      k->last_ms = now;

      /* payload structure vs previous sample */
      {
        uint16_t ns = (k->n_payload_samples > 0U) ? cap : 0U;
        uint8_t n_st = 0U, n_ch = 0U;
        for (j = 0U; j < ns && j < 8U; j++)
        {
          uint8_t a = k->prev_payload[j];
          uint8_t b = (j < cap) ? payload[j] : 0xFFu;
          if (a == b) { n_st++; }
          else { n_ch++; }
          k->bit_changes += (uint32_t)popcount8((uint8_t)(a ^ b));
        }
        k->stable_bytes = (k->n_payload_samples > 0U)
                            ? (uint16_t)(((uint32_t)k->stable_bytes + n_st) >> 1u)
                            : (uint16_t)n_st;
        k->changing_bytes = (k->n_payload_samples > 0U)
                            ? (uint16_t)(((uint32_t)k->changing_bytes + n_ch) >> 1u)
                            : (uint16_t)n_ch;
      }
      memcpy(k->prev_payload, payload, cap);
      k->n_payload_samples++;

      /* power / state correlations */
      for (j = 0U; j < cap; j++)
      {
        if ((payload[j] & 0xFFu) == (uint8_t)(vbus_mv & 0xFFu)) { k->vbus_corr = (k->vbus_corr < 255u) ? (uint8_t)(k->vbus_corr + 1U) : 255u; }
        if ((payload[j] & 0xFFu) == (uint8_t)(current_ma & 0xFFu)) { k->current_corr = (k->current_corr < 255u) ? (uint8_t)(k->current_corr + 1U) : 255u; }
        if ((payload[j] & 0xFFu) == (uint8_t)(temp_c & 0xFFu)) { k->temp_corr = (k->temp_corr < 255u) ? (uint8_t)(k->temp_corr + 1U) : 255u; }
      }
      if (reset_occurred) { k->reset_corr++; }
      if (voltchg_occurred) { k->voltchg_corr++; }
      if (attach_occurred) { k->attach_corr++; }
      if (prev_type != 0xFFu) { k->prev_type = prev_type; }

      /* derive category + confidence */
      {
        uint8_t cat = APIE_UNKNOWN_CAT_UNCLASSIFIED;
        if (k->reset_corr > 0U && k->session_count <= (uint32_t)(k->reset_corr + 2U))
        {
          cat = APIE_UNKNOWN_CAT_RESET_LINKED;
        }
        else if (k->n_interval >= 2U && k->session_count >= 3U && k->freq_x1000 >= 1000U &&
                 (k->vbus_corr > 0U || k->current_corr > 0U || k->temp_corr > 0U))
        {
          cat = APIE_UNKNOWN_CAT_TELEMETRY_CANDIDATE;
        }
        else if (k->n_interval >= 3U && k->freq_x1000 >= 1000U)
        {
          cat = APIE_UNKNOWN_CAT_PERIODIC;
        }
        else if (k->prev_type != 0xFFu && k->session_count >= 3U &&
                 k->prev_type == s_kind[i].prev_type)
        {
          cat = APIE_UNKNOWN_CAT_RESPONSE_LINKED;
        }
        else if (k->attach_corr > 0U && k->session_count <= (uint32_t)(k->attach_corr + 2U))
        {
          cat = APIE_UNKNOWN_CAT_STATE_DEPENDENT;
        }
        k->category = cat;
        {
          uint32_t conf = 40U;
          if (k->cross_session_count >= 2U) { conf += 20U; }
          if (k->n_interval >= 3U) { conf += 15U; }
          if (k->session_count >= 10U) { conf += 15U; }
          if (k->vbus_corr > 0U || k->current_corr > 0U || k->reset_corr > 0U) { conf += 10U; }
          k->confidence = (conf > 100U) ? 100U : (uint8_t)conf;
        }
      }
      s_last_type = type;
      return (int)i;
    }
  }

  /* new bucket */
  if (s_count >= APIE_UNKNOWN_KINDS)
  {
    s_last_type = type;
    return -1;
  }
  memset(&s_kind[s_count], 0, sizeof(s_kind[s_count]));
  s_kind[s_count].sig = sig;
  s_kind[s_count].sop = sop;
  s_kind[s_count].type = type;
  s_kind[s_count].msgid_seen = 1U;
  s_kind[s_count].len = cap;
  memcpy(s_kind[s_count].payload, payload, cap);
  memcpy(s_kind[s_count].prev_payload, payload, cap);
  s_kind[s_count].n_payload_samples = 1U;
  s_kind[s_count].entropy = entropy_of(payload, cap);
  s_kind[s_count].session_count = 1U;
  s_kind[s_count].cross_session_count = 1U;
  s_kind[s_count].first_ms = now;
  s_kind[s_count].last_ms = now;
  s_kind[s_count].category = APIE_UNKNOWN_CAT_UNCLASSIFIED;
  s_kind[s_count].confidence = 20U;
  if (prev_type != 0xFFu) { s_kind[s_count].prev_type = prev_type; }
  s_last_type = type;
  return (int)s_count++;
}

uint16_t APIE_Unknown_Count(void) { return s_count; }

const APIE_UnknownKind_t *APIE_Unknown_Get(uint16_t idx)
{
  if (idx >= s_count) { return NULL; }
  return &s_kind[idx];
}

const char *APIE_Unknown_CatName(uint8_t cat)
{
  switch (cat)
  {
    case APIE_UNKNOWN_CAT_PERIODIC:            return "PERIODIC";
    case APIE_UNKNOWN_CAT_TELEMETRY_CANDIDATE: return "TELEMETRY_CANDIDATE";
    case APIE_UNKNOWN_CAT_RESPONSE_LINKED:     return "RESPONSE_LINKED";
    case APIE_UNKNOWN_CAT_RESET_LINKED:        return "RESET_LINKED";
    case APIE_UNKNOWN_CAT_STATE_DEPENDENT:     return "STATE_DEPENDENT";
    default:                                   return "UNCLASSIFIED";
  }
}

void APIE_Unknown_Dump(void)
{
  uint16_t i;
  if (s_count == 0U)
  {
    APP_LOG_Write("unknown: no unrecognized messages recorded\r\n");
    return;
  }
  APP_LOG_Printf("unknown: %u distinct UNKNOWN_SIGNATURE(s)\r\n", (unsigned)s_count);
  for (i = 0U; i < s_count; i++)
  {
    APIE_UnknownKind_t *k = &s_kind[i];
    APP_LOG_Printf("  SIG#%u sop=%u type=0x%02X n=%lu c=%lu len=%u freq=%.1f/s\r\n",
                   (unsigned)i, (unsigned)k->sop, (unsigned)k->type,
                   (unsigned long)k->session_count, (unsigned long)k->cross_session_count,
                   (unsigned)k->len, (double)((float)k->freq_x1000 / 1000.0f));
    APP_LOG_Printf("      payload=");
    {
      uint8_t j;
      for (j = 0U; j < k->len; j++) { APP_LOG_Printf("%02X", (unsigned)k->payload[j]); }
    }
    APP_LOG_Printf("  stable=%u changing=%u bits=%lu entropy=%u\r\n",
                   (unsigned)k->stable_bytes, (unsigned)k->changing_bytes,
                   (unsigned long)k->bit_changes, (unsigned)k->entropy);
    APP_LOG_Printf("      corr: vbus=%u cur=%u temp=%u reset=%u voltchg=%u attach=%u prev_type=0x%02X\r\n",
                   (unsigned)k->vbus_corr, (unsigned)k->current_corr, (unsigned)k->temp_corr,
                   (unsigned)k->reset_corr, (unsigned)k->voltchg_corr, (unsigned)k->attach_corr,
                   (unsigned)k->prev_type);
    APP_LOG_Printf("      category=%s confidence=%u%%  (hypothesis, not proven meaning)\r\n",
                   APIE_Unknown_CatName(k->category), (unsigned)k->confidence);
  }
}

void APIE_Unknown_Stats(char *out, uint32_t outsz)
{
  if (out == NULL || outsz == 0U) { return; }
  snprintf(out, outsz, "unknown: %u UNKNOWN_SIGNATURE(s)", (unsigned)s_count);
}
