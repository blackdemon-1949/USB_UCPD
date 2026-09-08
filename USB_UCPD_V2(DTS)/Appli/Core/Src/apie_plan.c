/**
  ******************************************************************************
  * @file    apie_plan.c
  * @brief   Adaptive scheduling, information gain, experiment gating.
  ******************************************************************************
  */
#include "apie_plan.h"
#include "app_log.h"
#include <stdio.h>

extern uint32_t HAL_GetTick(void);

static APIE_QueryState_t s_q[APIE_SCHED_SLOTS];
static APIE_RateTracker_t s_rate[APIE_SCHED_SLOTS];
static uint8_t s_exp_level;

void APIE_Plan_Init(void)
{
  uint8_t i;
  s_exp_level = APIE_EXP_LEVEL_DEFAULT;
  for (i = 0U; i < APIE_SCHED_SLOTS; i++)
  {
    memset(&s_q[i], 0, sizeof(s_q[i]));
    s_q[i].id = (APIE_QueryId_t)i;
    s_q[i].supported = 0U; /* unknown */
    s_q[i].cooldown_ms = APIE_QUERY_COOLDOWN_MS;
    APIE_Rate_Init(&s_rate[i], 0.9f);
    /* Enable the ones that are standard and safe on a sink. */
    switch ((APIE_QueryId_t)i)
    {
      case APIE_QUERY_GET_STATUS:
      case APIE_QUERY_GET_PPS:
      case APIE_QUERY_SRC_EXT:
      case APIE_QUERY_MANU_INFO:
        s_q[i].enabled = 1U;
        break;
      case APIE_QUERY_IDENTITY:
      case APIE_QUERY_SVIDS:
      case APIE_QUERY_MODES:
      case APIE_QUERY_BATTERY:
      case APIE_QUERY_COUNTRY:
      default:
        s_q[i].enabled = 1U;
        break;
    }
  }
}

void APIE_Plan_ResetSession(uint8_t conn_id)
{
  uint8_t i;
  (void)conn_id;
  for (i = 0U; i < APIE_SCHED_SLOTS; i++)
  {
    s_q[i].pending = 0U;
    s_q[i].attempts = 0U;
    if (s_q[i].supported == 3U)
    {
      /* permanently-banned query stays suppressed */;
    }
    else
    {
      s_q[i].supported = 0U;
    }
  }
}

void APIE_Plan_OnOutcome(uint8_t query, uint8_t success, uint32_t latency_ms)
{
  if (query >= APIE_SCHED_SLOTS) { return; }
  APIE_Rate_Update(&s_rate[query], success);
  if (success != 0U)
  {
    s_q[query].successes++;
    s_q[query].supported = 1U;
  }
  else
  {
    s_q[query].failures++;
    /* after enough confirmed failures, treat as known-unsupported */
    if (s_q[query].failures >= 3U && s_q[query].successes == 0U)
    {
      s_q[query].supported = 2U;
    }
  }
  /* adaptive cooldown: back off on repeated failure, relax on success */
  if (success != 0U)
  {
    uint32_t cd = APIE_QUERY_COOLDOWN_MS;
    if (s_q[query].cooldown_ms > cd)
    {
      s_q[query].cooldown_ms -= cd;
    }
    if (s_q[query].cooldown_ms < cd) { s_q[query].cooldown_ms = cd; }
  }
  else
  {
    s_q[query].cooldown_ms += APIE_QUERY_COOLDOWN_MS;
    if (s_q[query].cooldown_ms > 8000U) { s_q[query].cooldown_ms = 8000U; }
  }
  (void)latency_ms;
}

void APIE_Plan_OnIssue(uint8_t query, uint32_t now_ms)
{
  if (query >= APIE_SCHED_SLOTS) { return; }
  s_q[query].last_ms = now_ms;
  s_q[query].attempts++;
  s_q[query].pending = 1U;
}

void APIE_Plan_OnNotSupported(uint8_t query)
{
  if (query >= APIE_SCHED_SLOTS) { return; }
  s_q[query].supported = 2U;
}

uint8_t APIE_Plan_InCooldown(uint8_t query, uint32_t now_ms)
{
  if (query >= APIE_SCHED_SLOTS) { return 1U; }
  if (s_q[query].last_ms == 0U) { return 0U; }
  return (now_ms - s_q[query].last_ms) < s_q[query].cooldown_ms;
}

const APIE_QueryState_t *APIE_Plan_Get(uint8_t query)
{
  if (query >= APIE_SCHED_SLOTS) { return NULL; }
  return &s_q[query];
}

static float query_score(uint8_t query, uint32_t now_ms)
{
  APIE_QueryState_t *q = &s_q[query];
  float rate = APIE_Rate_Value(&s_rate[query]);
  float score;
  if (q->enabled == 0U) { return -1.0f; }
  if (q->supported == 2U || q->supported == 3U) { return -1.0f; }
  if (q->pending != 0U) { return -1.0f; }
  if (APIE_Plan_InCooldown(query, now_ms)) { return -1.0f; }
  /* Priority grows with success rate; unknown starts with a slight positive
     so it gets probed once; high information-gain queries are boosted. */
  score = rate;
  if (rate == 0.0f && q->attempts == 0U)
  {
    score = 0.5f; /* unknown -> try once */
  }
  score += APIE_Ig_Estimate(query, q, 1U) * 0.3f;
  return score;
}

int APIE_Plan_Select(uint32_t now_ms, uint8_t attached, uint8_t has_contract)
{
  uint8_t i;
  float best = -1.0f;
  int best_id = -1;
  uint8_t pending = 0U;
  (void)has_contract;

  for (i = 0U; i < APIE_SCHED_SLOTS; i++)
  {
    if (s_q[i].pending != 0U) { pending++; }
  }
  if (pending >= APIE_QUERY_MAX_PENDING)
  {
    return -1;
  }
  for (i = 0U; i < APIE_SCHED_SLOTS; i++)
  {
    float sc = query_score((uint8_t)i, now_ms);
    if (sc > best)
    {
      best = sc;
      best_id = (int)i;
    }
  }
  if (attached == 0U)
  {
    return -1;
  }
  return best_id;
}

void APIE_Plan_Dump(void)
{
  uint8_t i;
  APP_LOG_Printf("scheduler (%u slots, level %s):\r\n", (unsigned)APIE_SCHED_SLOTS,
                 APIE_Exp_LevelName(s_exp_level));
  for (i = 0U; i < APIE_SCHED_SLOTS; i++)
  {
    char nm[16];
    switch ((APIE_QueryId_t)i)
    {
      case APIE_QUERY_GET_STATUS: snprintf(nm, sizeof(nm), "getstatus"); break;
      case APIE_QUERY_GET_PPS:    snprintf(nm, sizeof(nm), "getpps"); break;
      case APIE_QUERY_IDENTITY:   snprintf(nm, sizeof(nm), "identify"); break;
      case APIE_QUERY_SVIDS:      snprintf(nm, sizeof(nm), "svids"); break;
      case APIE_QUERY_MODES:      snprintf(nm, sizeof(nm), "modes"); break;
      case APIE_QUERY_SRC_EXT:    snprintf(nm, sizeof(nm), "srcext"); break;
      case APIE_QUERY_MANU_INFO:  snprintf(nm, sizeof(nm), "manuinfo"); break;
      case APIE_QUERY_BATTERY:    snprintf(nm, sizeof(nm), "battery"); break;
      default:                    snprintf(nm, sizeof(nm), "country"); break;
    }
    APP_LOG_Printf("  %-10s en=%u sup=%u at=%u ok=%u bad=%u rate=%.2f cooldown=%lu score=%.2f\r\n",
                   nm, (unsigned)s_q[i].enabled, (unsigned)s_q[i].supported,
                   (unsigned)s_q[i].attempts, (unsigned)s_q[i].successes,
                   (unsigned)s_q[i].failures, (double)APIE_Rate_Value(&s_rate[i]),
                   (unsigned long)s_q[i].cooldown_ms,
                   (double)query_score(i, HAL_GetTick()));
  }
}

/* --- information gain ---------------------------------------------------- */
float APIE_Ig_Estimate(uint8_t query, const APIE_QueryState_t *qs, uint8_t attached)
{
  float p;    /* probability the query yields useful info   */
  float ent;  /* entropy of the current belief              */
  float r = APIE_Rate_Value(&s_rate[query]);
  (void)qs;
  if (attached == 0U) { return 0.0f; }
  if (s_q[query].supported == 2U || s_q[query].supported == 3U)
  {
    return 0.0f;
  }
  /* Information gain is the reduction in uncertainty, measured here by the
     Bernoulli variance of the current belief:  p*(1-p) peaks (max IG) when the
     outcome is unknown (p=0.5) and is 0 when the outcome is certain (p=0/1).
     This is monotonic with binary entropy and needs no libm log() on the MCU.
     An unknown query (never tried) is assigned the maximally-uncertain p=0.5
     so it gets probed once. */
  p = (r > 0.0f) ? r : 0.5f;
  ent = 4.0f * p * (1.0f - p); /* scaled to [0,1] */
  return ent;
}

/* --- experiments --------------------------------------------------------- */
uint8_t APIE_Exp_GetLevel(void) { return s_exp_level; }

void APIE_Exp_SetLevel(uint8_t level)
{
  if (level > APIE_EXP_R4) { level = APIE_EXP_R4; }
  s_exp_level = level;
}

uint8_t APIE_Exp_Allows(uint8_t level)
{
  /* R0 (observe) is always allowed.  R1 (informational query) and R2 (power
     request within validated limits) require the configured level.  R3/R4
     additionally require the compile-time gate, which is OFF on this board so
     state-changing / unknown-vendor experiments can never run regardless of a
     CLI request. */
  if (level == APIE_EXP_R0) { return 1U; }
  if (level == APIE_EXP_R1) { return (s_exp_level >= APIE_EXP_R1) ? 1U : 0U; }
  if (level == APIE_EXP_R2) { return (s_exp_level >= APIE_EXP_R2) ? 1U : 0U; }
  if (level == APIE_EXP_R3) { return (APIE_EXP_ALLOW_R3 && (s_exp_level >= APIE_EXP_R3)) ? 1U : 0U; }
  if (level == APIE_EXP_R4) { return (APIE_EXP_ALLOW_R4 && (s_exp_level >= APIE_EXP_R4)) ? 1U : 0U; }
  return 0U;
}

const char *APIE_Exp_LevelName(uint8_t level)
{
  switch (level)
  {
    case APIE_EXP_R0: return "R0 observe";
    case APIE_EXP_R1: return "R1 info-query";
    case APIE_EXP_R2: return "R2 power-req";
    case APIE_EXP_R3: return "R3 state-change";
    case APIE_EXP_R4: return "R4 vendor-tx";
    default: return "unknown";
  }
}
