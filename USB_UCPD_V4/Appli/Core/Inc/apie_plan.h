/**
  ******************************************************************************
  * @file    apie_plan.h
  * @brief   Adaptive query scheduler + information-gain selection + experiments.
  *
  * The scheduler serializes informational queries, applies learned cooldowns
  * and backoff, and never issues more than one at a time.  It bumps a query's
  * priority when evidence says it works and suppresses queries the source
  * repeatedly rejects, so a source never sees a query storm.
  ******************************************************************************
  */
#ifndef APIE_PLAN_H
#define APIE_PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"
#include "apie_stats.h"

void APIE_Plan_Init(void);
void APIE_Plan_ResetSession(uint8_t conn_id);
/* Mark a query outcome: success 1 / failure 0. */
void APIE_Plan_OnOutcome(uint8_t query, uint8_t success, uint32_t latency_ms);
/* Record that a query was physically issued (for cooldown). */
void APIE_Plan_OnIssue(uint8_t query, uint32_t now_ms);
/* Mark a query as known-unsupported (stop trying it). */
void APIE_Plan_OnNotSupported(uint8_t query);
/* Compute a score for each enabled query, then select the best next action.
   Returns the selected query id, or -1 if none is available. */
int APIE_Plan_Select(uint32_t now_ms, uint8_t attached, uint8_t has_contract);
const APIE_QueryState_t *APIE_Plan_Get(uint8_t query);
void APIE_Plan_Dump(void);
/* Refuse to over-query: true when a query should wait. */
uint8_t APIE_Plan_InCooldown(uint8_t query, uint32_t now_ms);

/* --- information gain ---------------------------------------------------- */
float APIE_Ig_Estimate(uint8_t query, const APIE_QueryState_t *qs, uint8_t attached);
void APIE_Ig_Dump(void);

/* --- experiments ----------------------------------------------------------
 * R0 observe, R1 informational query, R2 standard power request within limits,
 * R3 state-changing standard experiment, R4 unknown/vendor transmission.
 * Defaults: R0/R1 ON, R2 ON within validated source/hardware limits, R3/R4 OFF.
 */
#define APIE_EXP_R0 0U
#define APIE_EXP_R1 1U
#define APIE_EXP_R2 2U
#define APIE_EXP_R3 3U
#define APIE_EXP_R4 4U

uint8_t APIE_Exp_GetLevel(void);
void APIE_Exp_SetLevel(uint8_t level);
uint8_t APIE_Exp_Allows(uint8_t level);
const char *APIE_Exp_LevelName(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* APIE_PLAN_H */
