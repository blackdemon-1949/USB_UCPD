/**
  ******************************************************************************
  * @file    apie_cable.h
  * @brief   Cable (SOP'/SOP'') intelligence + EPR/AVS protocol awareness.
  *
  * The source and the cable are treated as SEPARATE entities.  Cable facts
  * come only from SOP'/SOP'' messages (e-marker / cable identity), and are
  * never attributed to the source.  EPR/AVS structures are decoded so future
  * hardware can support extended power; on THIS board EPR power is gated by
  * APIE_HW_EPR_POWER_ENABLED (0) and is never energised.
  ******************************************************************************
  */
#ifndef APIE_CABLE_H
#define APIE_CABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"

typedef struct
{
  uint8_t present;         /* 1 = a cable identity has been observed        */
  uint8_t sop;             /* SOP' or SOP''                                */
  uint8_t current_cap;     /* 0 default, 1 = 3 A, 2 = 5 A                  */
  uint8_t ss_cap;          /* superspeed capability                        */
  uint8_t active;          /* 1 = active cable / e-marker                  */
  uint8_t vconn;           /* VCONN-powered                               */
  uint16_t vid;
  uint16_t pid;
  uint32_t hw_rev;
  uint32_t fw_rev;
} APIE_CableProfile_t;

typedef enum
{
  APIE_EPR_STATE_NOT_IN_EPR = 0,
  APIE_EPR_STATE_SRC_CAP,   /* source advertised EPR caps                  */
  APIE_EPR_STATE_MODE_ENTRY,/* EPR mode requested/entered                  */
  APIE_EPR_STATE_MODE_ACTIVE
} APIE_EPR_State_t;

typedef struct
{
  uint8_t  epr_capable;      /* source supports EPR                        */
  uint8_t  avs_present;
  uint16_t avs_min_mv;
  uint16_t avs_max_mv;
  uint8_t  state;            /* APIE_EPR_State_t                           */
  uint16_t epr_snk_pdp_w;    /* EPR sink operational PDP (watts)           */
  uint16_t current_voltage_mv;
} APIE_EPR_Info_t;

void APIE_Cable_Init(void);
void APIE_Cable_ResetSession(uint8_t conn_id);
void APIE_Cable_OnIdentity(uint8_t sop, const uint8_t *vdo, uint8_t nvdo);
void APIE_Cable_OnSopData(uint8_t sop, const uint8_t *payload, uint16_t len);
void APIE_EPR_OnSourceCaps(uint8_t port, const uint32_t *pdo, uint8_t n);
void APIE_EPR_OnModeChange(uint8_t new_state);
void APIE_EPR_OnAvs(const uint8_t *payload, uint16_t len);

const APIE_CableProfile_t *APIE_Cable_Get(void);
const APIE_EPR_Info_t *APIE_EPR_Get(void);
uint8_t APIE_EPR_PowerAllowed(void);       /* 1 only when HW flag set       */
void APIE_Cable_Dump(void);
void APIE_EPR_Dump(void);
const char *APIE_SopName(uint8_t sop);

#ifdef __cplusplus
}
#endif

#endif /* APIE_CABLE_H */
