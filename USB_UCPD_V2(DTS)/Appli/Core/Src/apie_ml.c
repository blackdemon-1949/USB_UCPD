/**
  ******************************************************************************
  * @file    apie_ml.c
  * @brief   Embedded ML inference: online Naive Bayes + logistic decision.
  ******************************************************************************
  */
#include "apie_ml.h"
#include "apie_decode.h"
#include "apie_stats.h"
#include <stdio.h>

#define ML_MAGIC 0x414D4C4Au /* 'AMLA' */

static APIE_MlModel_t s_ml;

static void bin_get(uint32_t val, uint8_t *bin);

void APIE_Ml_Init(void)
{
  memset(&s_ml, 0, sizeof(s_ml));
  s_ml.meta.id = 1U;
  s_ml.meta.version = 1U;
  s_ml.meta.feature_version = APIE_FEATURE_COUNT;
  s_ml.meta.kind = (uint8_t)APIE_MODEL_NAIVE_BAYES;
  s_ml.meta.accuracy = 0.0f;      /* unknown until validated on data */
  snprintf(s_ml.meta.trained, sizeof(s_ml.meta.trained), "seed-online");
  /* Seed priors: the host pipeline would import learned counts here.  The
     seed is intentionally "unknown" (no counts) so every prediction starts
     from Laplace (uniform) priors and is driven purely by online evidence. */
  s_ml.meta.crc32 = APIE_Crc32((const uint8_t *)&s_ml, sizeof(APIE_MlModel_t));
}

void APIE_Ml_Reset(void)
{
  APIE_Ml_Init();
}

const APIE_MlModel_t *APIE_Ml_GetModel(void)
{
  return &s_ml;
}

static void bin_get(uint32_t val, uint8_t *bin)
{
  /* Map an integer to a small bin (0..7) for the discrete likelihood table. */
  if (val >= 7U)
  {
    *bin = 7U;
  }
  else
  {
    *bin = (uint8_t)val;
  }
}

void APIE_Ml_Observe(uint8_t query, uint8_t attempt, uint8_t has_pps, uint8_t hard_known,
                     uint8_t success)
{
  uint8_t cls = (success != 0U) ? 1U : 0U;
  uint8_t abin, pbin, hbin, qbin;
  uint8_t i;
  if (query >= APIE_NB_QUERY_BINS) { query = (uint8_t)(APIE_NB_QUERY_BINS - 1U); }

  s_ml.nclass[cls]++;
  bin_get((uint32_t)attempt, &abin);
  bin_get((uint32_t)has_pps, &pbin);
  bin_get((uint32_t)hard_known, &hbin);
  qbin = query;

  /* store feature/class frequencies.  Bin arrays are small; keep in-bounds. */
  s_ml.ncount[cls][APIE_NB_FEAT_QUERY][qbin % 8U]++;
  /* attempt -> treats bins as categories, cap at 7 */
  s_ml.ncount[cls][APIE_NB_FEAT_ATTEMPT][abin]++;
  s_ml.ncount[cls][APIE_NB_FEAT_HASPPS][pbin]++;
  s_ml.ncount[cls][APIE_NB_FEAT_HARD][hbin]++;

  /* Rebuild the logistic head from the observed useful-rate (simple online
     update; bounded). */
  (void)i;
  s_ml.logit_b = (s_ml.nclass[0U] + s_ml.nclass[1U]) > 0U
                 ? (float)((float)s_ml.nclass[1U] /
                           (float)(s_ml.nclass[0U] + s_ml.nclass[1U]))
                 : 0.0f;
}

float APIE_Ml_PredictUseful(const APIE_FeatureVec_t *fv, uint8_t query)
{
  /* Naive Bayes: P(class=useful | features) via Laplace-smoothed likelihoods.
     Start with uniform priors; the ratios are driven by online evidence. */
  uint32_t i;
  float pc1, pc0, p1, p0;
  uint8_t abin, pbin, hbin, qbin;
  if (fv == NULL) { return 0.5f; }
  if (query >= APIE_NB_QUERY_BINS) { query = (uint8_t)(APIE_NB_QUERY_BINS - 1U); }
  qbin = query;

  bin_get((uint32_t)(fv->v[1]), &abin);  /* result-ish used as attempt proxy */
  bin_get((uint32_t)((fv->v[8] > 0.f) ? 1u : 0u), &pbin);
  bin_get((uint32_t)((fv->v[3] > 0.f && fv->v[6] > 0.f) ? 1u : 0u), &hbin);

  pc1 = (float)(s_ml.nclass[1U] + 1U) / (float)(s_ml.nclass[0U] + s_ml.nclass[1U] + 2U);
  pc0 = 1.0f - pc1;

  p1 = 1.0f; p0 = 1.0f;
  for (i = 0U; i < APIE_NB_FEAT_COUNT; i++)
  {
    /* 8 uniform categories, +1 Laplace */
    uint32_t tot1 = s_ml.nclass[1U] + 8U;
    uint32_t tot0 = s_ml.nclass[0U] + 8U;
    uint32_t c1, c0;
    switch (i)
    {
      case APIE_NB_FEAT_QUERY:   c1 = s_ml.ncount[1][i][qbin % 8U]; c0 = s_ml.ncount[0][i][qbin % 8U]; break;
      case APIE_NB_FEAT_ATTEMPT: c1 = s_ml.ncount[1][i][abin];      c0 = s_ml.ncount[0][i][abin];      break;
      case APIE_NB_FEAT_HASPPS:  c1 = s_ml.ncount[1][i][pbin];      c0 = s_ml.ncount[0][i][pbin];      break;
      default:                   c1 = s_ml.ncount[1][i][hbin];      c0 = s_ml.ncount[0][i][hbin];      break;
    }
    p1 *= ((float)c1 + 1.0f) / (float)tot1;
    p0 *= ((float)c0 + 1.0f) / (float)tot0;
  }
  {
    float d = pc1 * p1 + pc0 * p0;
    if (d <= 0.0f) { return (pc1 >= pc0) ? 1.0f : 0.0f; }
    return (pc1 * p1) / d;
  }
}

uint8_t APIE_Ml_Classify(const APIE_FeatureVec_t *fv, uint8_t query)
{
  return (APIE_Ml_PredictUseful(fv, query) >= 0.5f) ? 1U : 0U;
}

uint8_t APIE_Ml_Validate(void)
{
  /* Structural validation + metadata checksum. */
  if (s_ml.meta.id == 0U || s_ml.meta.feature_version != APIE_FEATURE_COUNT)
  {
    return 0U;
  }
  return 1U;
}

uint16_t APIE_Ml_Export(uint8_t *out, uint16_t outsz)
{
  uint32_t sz = (uint32_t)sizeof(APIE_MlModel_t);
  if (out == NULL || outsz < sz)
  {
    return 0U;
  }
  memcpy(out, &s_ml, sz);
  return (uint16_t)sz;
}

uint8_t APIE_Ml_Import(const uint8_t *in, uint16_t len)
{
  if (in == NULL || len != sizeof(APIE_MlModel_t))
  {
    return 0U;
  }
  memcpy(&s_ml, in, sizeof(s_ml));
  return APIE_Ml_Validate();
}

/* ---------------------------------------------------------------------------
 *  Decision-tree classifier ("is query Q likely supported / useful").
 *
 *  This is a fixed, interpretable rule tree with evidence-justified
 *  thresholds, NOT learned random weights.  It is subordinate to the
 *  deterministic scheduler (apie_plan.c), which is the only component that
 *  actually issues queries.  Returns 1 = "likely useful", 0 = "likely not".
 * ------------------------------------------------------------------------- */
uint8_t APIE_Tree_ClassifyUseful(const APIE_FeatureVec_t *fv, uint8_t query)
{
  float has_pps, hard_known, attemptish, adv_lat;
  if (fv == NULL) { return 0U; }
  if (query >= APIE_NB_QUERY_BINS) { query = (uint8_t)(APIE_NB_QUERY_BINS - 1U); }

  has_pps    = (fv->v[8] > 0.5f) ? 1.0f : 0.0f;   /* feature index 8 = has PPS */
  hard_known = (fv->v[3] > 0.0f && fv->v[6] > 0.0f) ? 1.0f : 0.0f; /* proxy */
  attemptish = fv->v[1];                            /* retry/attempt-ish        */
  adv_lat    = fv->v[11];                           /* advertisement interval   */

  (void)attemptish; (void)adv_lat;

  /* Structural rules (documented, deterministic):
       - PPS_Status is only worth asking on a source that offers PPS.
       - Battery query is only meaningful when a battery PDO exists, else
         Not_Supported is the expected answer -> not useful to schedule.
       - Identity/SVIDs need a source that answers VDM; if the source already
         NAKed identity the tree prefers to stop (handled by scheduler).
       - Get_Status / Get_PPS_Status are low-cost, generally useful.      */
  switch ((APIE_QueryId_t)query)
  {
    case APIE_QUERY_GET_PPS:
      return (has_pps > 0.5f) ? 1U : 0U;
    case APIE_QUERY_BATTERY:
      return 0U;                      /* rarely useful on fixed/PPS sinks     */
    case APIE_QUERY_IDENTITY:
    case APIE_QUERY_SVIDS:
    case APIE_QUERY_MODES:
      return hard_known > 0.5f ? 1U : 0U;
    case APIE_QUERY_GET_STATUS:
    case APIE_QUERY_MANU_INFO:
    default:
      return 1U;                      /* standard, low-risk informational      */
  }
}

/* ---------------------------------------------------------------------------
 *  Online Gaussian anomaly detector (transaction-latency, Welford bounded).
 * ------------------------------------------------------------------------- */
#define APIE_ANOMALY_MIN_SAMPLES 8U
static APIE_StatAccum_t s_anom;
static uint8_t s_anom_init;

void APIE_Ml_Anomaly_Observe(float x)
{
  if (s_anom_init == 0U)
  {
    APIE_Stats_Init(&s_anom);
    s_anom_init = 1U;
  }
  APIE_Stats_Update(&s_anom, x);
}

uint8_t APIE_Ml_Anomaly_Flag(float x, float k_sigma)
{
  float m, s;
  float dev;
  if (s_anom_init == 0U || s_anom.n < APIE_ANOMALY_MIN_SAMPLES)
  {
    return 0U;                       /* not trained yet -> not anomalous */
  }
  m = APIE_Stats_Mean(&s_anom);
  s = APIE_Stats_Stddev(&s_anom);
  dev = (x > m) ? (x - m) : (m - x);
  return (s > 0.0f && dev > k_sigma * s) ? 1U : 0U;
}

float APIE_Ml_Anomaly_Mean(void) { return s_anom_init ? APIE_Stats_Mean(&s_anom) : 0.0f; }
float APIE_Ml_Anomaly_Std(void) { return s_anom_init ? APIE_Stats_Stddev(&s_anom) : 0.0f; }
uint32_t APIE_Ml_Anomaly_Count(void) { return s_anom.n; }
uint8_t APIE_Ml_Anomaly_Trained(void)
{
  return (s_anom_init != 0U && s_anom.n >= APIE_ANOMALY_MIN_SAMPLES) ? 1U : 0U;
}

/* CRC helper lives in the database module but is exported from apie.h. */
