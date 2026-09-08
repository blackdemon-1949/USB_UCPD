/**
  ******************************************************************************
  * @file    apie_stats.c
  * @brief   Online statistical learning primitives.
  ******************************************************************************
  */
#include "apie_stats.h"

void APIE_Stats_Init(APIE_StatAccum_t *a)
{
  if (a == NULL) { return; }
  a->mean = 0.0f;
  a->m2 = 0.0f;
  a->min = 1e30f;
  a->max = -1e30f;
  a->n = 0U;
}

void APIE_Stats_Update(APIE_StatAccum_t *a, float x)
{
  float delta, delta2;
  if (a == NULL) { return; }
  a->n++;
  delta = x - a->mean;
  a->mean += delta / (float)a->n;
  delta2 = x - a->mean;
  a->m2 += delta * delta2;
  if (x < a->min) { a->min = x; }
  if (x > a->max) { a->max = x; }
}

float APIE_Stats_Mean(const APIE_StatAccum_t *a)
{
  if (a == NULL || a->n == 0U) { return 0.0f; }
  return a->mean;
}

float APIE_Stats_Variance(const APIE_StatAccum_t *a)
{
  if (a == NULL || a->n < 2U) { return 0.0f; }
  return a->m2 / (float)(a->n - 1U);
}

float APIE_Stats_Stddev(const APIE_StatAccum_t *a)
{
  float v = APIE_Stats_Variance(a);
  return (v > 0.0f) ? v : 0.0f;
}

void APIE_Rate_Init(APIE_RateTracker_t *r, float beta)
{
  if (r == NULL) { return; }
  r->rate = 0.0f;
  r->beta = beta;
  r->n = 0U;
  r->success = 0U;
  r->failure = 0U;
}

void APIE_Rate_Update(APIE_RateTracker_t *r, uint8_t success)
{
  if (r == NULL) { return; }
  r->n++;
  if (success != 0U) { r->success++; } else { r->failure++; }
  if (r->n == 1U)
  {
    r->rate = (success != 0U) ? 1.0f : 0.0f;
  }
  else
  {
    r->rate = r->beta * r->rate + (1.0f - r->beta) * ((success != 0U) ? 1.0f : 0.0f);
  }
}

float APIE_Rate_Value(const APIE_RateTracker_t *r)
{
  if (r == NULL) { return 0.0f; }
  return r->rate;
}

void APIE_Hist_Init(APIE_Hist_t *h)
{
  uint8_t i;
  if (h == NULL) { return; }
  for (i = 0U; i < 8U; i++) { h->bin[i] = 0U; }
  h->n = 0U;
}

void APIE_Hist_Add(APIE_Hist_t *h, uint8_t bin)
{
  if (h == NULL) { return; }
  if (bin < 8U) { h->bin[bin]++; }
  h->n++;
}

uint32_t APIE_Hist_Count(const APIE_Hist_t *h, uint8_t bin)
{
  if (h == NULL) { return 0U; }
  if (bin < 8U) { return h->bin[bin]; }
  return 0U;
}
