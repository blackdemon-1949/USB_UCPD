/**
  ******************************************************************************
  * @file    apie_unknown.h
  * @brief   Unknown-protocol analyzer (UNKNOWN_SIGNATURE characterization).
  *
  * Repeated behavior that the deterministic decoder cannot name is NOT
  * discarded nor blindly called "proprietary".  It is bucketed by signature
  * (SOP + type + first payload bytes) and characterized with a structured
  * UNKNOWN_SIGNATURE:
  *
  *   frequency, sequence, timing/latency, payload length, stable vs changing
  *   bytes, bit changes, entropy, state dependence, and correlation with VBUS /
  *   current / temperature / attach / voltage-change / reset.
  *
  * The result is a *category + confidence* hypothesis (e.g.
  * TELEMETRY_CANDIDATE), never a fabricated protocol meaning.  The tool may say
  * "UNKNOWN" rather than invent an answer.
  ******************************************************************************
  */
#ifndef APIE_UNKNOWN_H
#define APIE_UNKNOWN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"

/* Structured hypothesis categories (evidence-derived, never asserted fact). */
typedef enum
{
  APIE_UNKNOWN_CAT_UNCLASSIFIED       = 0,
  APIE_UNKNOWN_CAT_PERIODIC           = 1, /* very regular interval            */
  APIE_UNKNOWN_CAT_TELEMETRY_CANDIDATE= 2, /* frequent + power-correlated      */
  APIE_UNKNOWN_CAT_RESPONSE_LINKED    = 3, /* consistently follows one message */
  APIE_UNKNOWN_CAT_RESET_LINKED       = 4, /* correlates with resets           */
  APIE_UNKNOWN_CAT_STATE_DEPENDENT    = 5  /* appears only in one PD state     */
} APIE_UnknownCat_t;

typedef struct
{
  uint32_t sig;             /* signature hash                              */
  uint8_t  sop;
  uint8_t  type;
  uint8_t  msgid_seen;      /* stable msg id repeated?                     */
  uint16_t len;             /* payload length of first occurrence          */
  uint8_t  payload[8];      /* first 8 payload bytes (first occurrence)    */

  /* occurrence / timing */
  uint32_t session_count;
  uint32_t cross_session_count;
  uint32_t first_ms;
  uint32_t last_ms;
  uint32_t sum_interval_ms;
  uint32_t n_interval;
  uint32_t freq_x1000;      /* packets/sec * 1000 (from mean interval)     */

  /* payload structure (computed incrementally against the previous sample) */
  uint8_t  prev_payload[8];
  uint16_t stable_bytes;    /* payload bytes that never changed            */
  uint16_t changing_bytes;  /* payload bytes that changed at least once    */
  uint32_t bit_changes;     /* accumulated popcount of XORs                */
  uint32_t n_payload_samples;
  uint8_t  entropy;         /* 0..255 Shannon entropy of first payload     */

  /* correlations (counts / evidence strength) */
  uint8_t  vbus_corr;       /* payload byte tracked VBUS                   */
  uint8_t  current_corr;
  uint8_t  temp_corr;
  uint8_t  reset_corr;
  uint8_t  voltchg_corr;    /* observed right after a voltage change       */
  uint8_t  attach_corr;     /* observed right after attach                 */

  /* state / sequence */
  uint8_t  prev_type;       /* message type seen immediately before        */
  uint8_t  category;        /* APIE_UnknownCat_t                           */
  uint8_t  confidence;      /* 0..100                                      */
} APIE_UnknownKind_t;

void APIE_Unknown_Init(void);
void APIE_Unknown_ResetSession(void);
/* Feed an unrecognized message.  Returns index of the bucket (-1 if full). */
int APIE_Unknown_Observe(uint8_t sop, uint8_t type, uint8_t msgid, const uint8_t *payload,
                         uint16_t len, uint32_t vbus_mv, uint32_t current_ma, uint32_t temp_c,
                         uint8_t reset_occurred, uint8_t voltchg_occurred,
                         uint8_t attach_occurred, uint8_t prev_type);
uint16_t APIE_Unknown_Count(void);
const APIE_UnknownKind_t *APIE_Unknown_Get(uint16_t idx);
void APIE_Unknown_Dump(void);
void APIE_Unknown_Stats(char *out, uint32_t outsz);
/* Human-readable category name for a bucket (host + CLI). */
const char *APIE_Unknown_CatName(uint8_t cat);

#ifdef __cplusplus
}
#endif

#endif /* APIE_UNKNOWN_H */
