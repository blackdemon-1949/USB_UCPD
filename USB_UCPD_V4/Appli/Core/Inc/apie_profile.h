/**
  ******************************************************************************
  * @file    apie_profile.h
  * @brief   Multi-dimensional source fingerprint (hard + protocol + behavior).
  *
  * A source is identified by the union of its credentials (VID/PID/FW/HW),
  * its protocol signature (PDO set / PPS / EPR / SVID / VDM / extended), and
  * its observed behavior (latency, advertisement interval, error profile).
  * VID/PID alone is NEVER used as the identity; it is one dimension.
  ******************************************************************************
  */
#ifndef APIE_PROFILE_H
#define APIE_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"

void APIE_Profile_Init(void);
void APIE_Profile_ResetSession(uint8_t conn_id);
void APIE_Profile_OnCaps(uint8_t port, const uint32_t *pdo, uint8_t n);
void APIE_Profile_OnIdentity(uint16_t vid, uint16_t pid, uint8_t fw, uint8_t hw);
void APIE_Profile_OnSvids(const uint16_t *svids, uint8_t n);
void APIE_Profile_OnBattery(uint8_t support);       /* 1 yes, 2 no, 0 unknown */
void APIE_Profile_OnIdentityResult(uint8_t ok);      /* 1 ACK, 0 NAK */
void APIE_Profile_OnAdvInterval(uint32_t interval_ms);
void APIE_Profile_OnGetStatusLatency(uint32_t latency_ms);
void APIE_Profile_OnHardReset(void);
void APIE_Profile_OnPps(uint16_t min_mv, uint16_t max_mv, uint16_t max_ma);
void APIE_Profile_OnExtendedSupport(uint8_t yes);

const APIE_Profile_t *APIE_Profile_Get(void);
/* Compute the rolling PDO signature for the current profile. */
uint32_t APIE_Profile_Signature(void);
int APIE_Profile_Match(const APIE_Profile_t *candidate, float *score_out);

void APIE_Profile_Print(void);

#ifdef __cplusplus
}
#endif

#endif /* APIE_PROFILE_H */
