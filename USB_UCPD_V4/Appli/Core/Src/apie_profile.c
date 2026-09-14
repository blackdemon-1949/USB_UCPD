/**
  ******************************************************************************
  * @file    apie_profile.c
  * @brief   Source fingerprint maintenance.
  ******************************************************************************
  */
#include "apie_profile.h"
#include "apie_decode.h"
#include "app_log.h"
#include <stdio.h>

static APIE_Profile_t s_p;

void APIE_Profile_Init(void)
{
  memset(&s_p, 0, sizeof(s_p));
}

void APIE_Profile_ResetSession(uint8_t conn_id)
{
  memset(&s_p, 0, sizeof(s_p));
  s_p.conn_id = conn_id;
}

void APIE_Profile_OnCaps(uint8_t port, const uint32_t *pdo, uint8_t n)
{
  uint8_t i;
  (void)port;
  if (pdo == NULL || n == 0U)
  {
    return;
  }
  if (n > APIE_PROFILE_PDOS)
  {
    n = APIE_PROFILE_PDOS;
  }
  memcpy(s_p.pdo, pdo, (size_t)n * 4U);
  s_p.n_pdo = n;
  s_p.valid = 1U;

  for (i = 0U; i < n; i++)
  {
    uint32_t t = APIE_Decode_PdoType(pdo[i]); /* bits[31:30] */
    uint32_t min_mv = 0U, max_mv = 0U, ma = 0U, mwp = 0U;
    APIE_Decode_PdoCaps(pdo[i], &min_mv, &max_mv, &ma, &mwp);
    if (t == APIE_PDO_TYPE_APDO)
    {
      uint8_t at = APIE_Decode_ApdoType(pdo[i]); /* bits[29:28] */
      if (at == APIE_APDO_TYPE_PPS)
      {
        s_p.has_pps = 1U;
        s_p.pps_min_mv = (uint16_t)min_mv;
        s_p.pps_max_mv = (uint16_t)max_mv;
        s_p.pps_max_ma = (uint16_t)ma;
      }
      else if (at == APIE_APDO_TYPE_AVS)
      {
        s_p.has_epr = 1U; /* AVS is an EPR-capable APDO */
      }
    }
    else if (t == APIE_PDO_TYPE_VARIABLE)
    {
      s_p.has_variable = 1U;
    }
    else if (t == APIE_PDO_TYPE_BATTERY)
    {
      s_p.has_battery = 1U;
    }
  }
}

void APIE_Profile_OnIdentity(uint16_t vid, uint16_t pid, uint8_t fw, uint8_t hw)
{
  s_p.vid = vid;
  s_p.pid = pid;
  s_p.fw = fw;
  s_p.hw = hw;
  s_p.has_hard = 1U;
  s_p.valid = 1U;
}

void APIE_Profile_OnSvids(const uint16_t *svids, uint8_t n)
{
  uint8_t i;
  if (svids == NULL || n == 0U)
  {
    return;
  }
  if (n > 8U) { n = 8U; }
  for (i = 0U; i < n; i++)
  {
    s_p.svid[i] = svids[i];
  }
  s_p.n_svid = n;
  s_p.has_svid = 1U;
  s_p.valid = 1U;
}

void APIE_Profile_OnBattery(uint8_t support)   { s_p.battery_supported = support; }
void APIE_Profile_OnIdentityResult(uint8_t ok) { s_p.identity_supported = (ok != 0U) ? 1U : 2U; }
void APIE_Profile_OnAdvInterval(uint32_t interval_ms) { s_p.adv_interval_ms = interval_ms; }
void APIE_Profile_OnGetStatusLatency(uint32_t latency_ms) { s_p.getstatus_latency_ms = latency_ms; }
void APIE_Profile_OnHardReset(void) { s_p.reset_count++; }
void APIE_Profile_OnExtendedSupport(uint8_t yes) { s_p.extended_support = (yes != 0U) ? 1U : 2U; }

void APIE_Profile_OnPps(uint16_t min_mv, uint16_t max_mv, uint16_t max_ma)
{
  s_p.has_pps = 1U;
  s_p.pps_min_mv = min_mv;
  s_p.pps_max_mv = max_mv;
  s_p.pps_max_ma = max_ma;
  s_p.valid = 1U;
}

const APIE_Profile_t *APIE_Profile_Get(void)
{
  return &s_p;
}

uint32_t APIE_Profile_Signature(void)
{
  return APIE_Decode_PdoSignature(s_p.pdo, s_p.n_pdo);
}

int APIE_Profile_Match(const APIE_Profile_t *candidate, float *score_out)
{
  float score = 0.0f;
  float max = 0.0f;
  if (candidate == NULL)
  {
    return 0;
  }
  /* A true identity match is decisive. */
  if (s_p.has_hard != 0U && candidate->has_hard != 0U &&
      s_p.vid == candidate->vid && s_p.pid == candidate->pid)
  {
    if (score_out) { *score_out = 1.0f; }
    return 1;
  }
  /* Otherwise compare the PDO signature (protocol fingerprint). */
  if (s_p.n_pdo == candidate->n_pdo && s_p.n_pdo > 0U &&
      memcmp(s_p.pdo, candidate->pdo, (size_t)s_p.n_pdo * 4U) == 0)
  {
    score += 0.6f;
  }
  if (s_p.has_pps == candidate->has_pps) { score += 0.2f; max += 0.2f; }
  if (s_p.has_battery == candidate->has_battery) { score += 0.1f; max += 0.1f; }
  if (max <= 0.0f) { max = 1.0f; }
  score /= max;
  if (score_out) { *score_out = score; }
  return (score >= 0.7f) ? 1 : 0;
}

void APIE_Profile_Print(void)
{
  char line[64];
  uint8_t i;
  APP_LOG_Write("source profile:\r\n");
  if (s_p.has_hard != 0U)
  {
    APP_LOG_Printf("  hard   : VID=0x%04X PID=0x%04X FW=%u HW=%u\r\n",
                   (unsigned)s_p.vid, (unsigned)s_p.pid, (unsigned)s_p.fw, (unsigned)s_p.hw);
  }
  else
  {
    APP_LOG_Write("  hard   : unknown (no VID/PID observed yet)\r\n");
  }
  APP_LOG_Printf("  pdo    : %u slots, signature=0x%08lX\r\n",
                 (unsigned)s_p.n_pdo, (unsigned long)APIE_Profile_Signature());
  for (i = 0U; i < s_p.n_pdo; i++)
  {
    APIE_Decode_PDO(s_p.pdo[i], 1U, line, sizeof(line));
    APP_LOG_Printf("           [%u] %s\r\n", (unsigned)(i + 1U), line);
  }
  APP_LOG_Printf("  pps    : %s (%u-%u mV / %u mA)\r\n",
                 s_p.has_pps ? "yes" : "no",
                 (unsigned)s_p.pps_min_mv, (unsigned)s_p.pps_max_mv, (unsigned)s_p.pps_max_ma);
  APP_LOG_Printf("  epr/avs: %s\r\n", s_p.has_epr ? "yes" : "no");
  APP_LOG_Printf("  svid   : %u (%s)\r\n", (unsigned)s_p.n_svid,
                 s_p.has_svid ? "yes" : "none observed");
  APP_LOG_Printf("  battery: %s  identity: %s  extended: %s\r\n",
                 (s_p.battery_supported == 2u) ? "not-supported" :
                 (s_p.battery_supported == 1u) ? "supported" : "unknown",
                 (s_p.identity_supported == 2u) ? "naked" :
                 (s_p.identity_supported == 1u) ? "acked" : "unknown",
                 (s_p.extended_support == 2u) ? "no" :
                 (s_p.extended_support == 1u) ? "yes" : "unknown");
  APP_LOG_Printf("  behav  : adv=%lu ms  status_latency=%lu ms  hw_resets=%lu\r\n",
                 (unsigned long)s_p.adv_interval_ms,
                 (unsigned long)s_p.getstatus_latency_ms,
                 (unsigned long)s_p.reset_count);
}
