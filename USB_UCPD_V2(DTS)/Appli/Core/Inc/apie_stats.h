/**
  ******************************************************************************
  * @file    apie_stats.h
  * @brief   Numerically stable, bounded online statistical learning.
  *
  * Welford's online algorithm for mean/variance is used because it is
  * numerically stable and requires only O(1) state - ideal for an embedded
  * online learner that must never grow unbounded history.
  ******************************************************************************
  */
#ifndef APIE_STATS_H
#define APIE_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"

void APIE_Stats_Init(APIE_StatAccum_t *a);
void APIE_Stats_Update(APIE_StatAccum_t *a, float x);
float APIE_Stats_Mean(const APIE_StatAccum_t *a);
float APIE_Stats_Variance(const APIE_StatAccum_t *a);
float APIE_Stats_Stddev(const APIE_StatAccum_t *a);

/* Online success-rate tracker (bounded, exponential-ish decay). */
typedef struct
{
  float  rate;          /* smoothed success rate 0..1 */
  float  beta;          /* decay factor (0.9 = ~10 samples memory) */
  uint32_t n;           /* total updates */
  uint32_t success;
  uint32_t failure;
} APIE_RateTracker_t;

void APIE_Rate_Init(APIE_RateTracker_t *r, float beta);
void APIE_Rate_Update(APIE_RateTracker_t *r, uint8_t success);
float APIE_Rate_Value(const APIE_RateTracker_t *r);

/* Discrete outcome histogram over a small integer space. */
typedef struct
{
  uint32_t bin[8];
  uint32_t n;
} APIE_Hist_t;
void APIE_Hist_Init(APIE_Hist_t *h);
void APIE_Hist_Add(APIE_Hist_t *h, uint8_t bin);
uint32_t APIE_Hist_Count(const APIE_Hist_t *h, uint8_t bin);

#ifdef __cplusplus
}
#endif

#endif /* APIE_STATS_H */
