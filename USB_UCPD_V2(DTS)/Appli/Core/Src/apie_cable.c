/**
  ******************************************************************************
  * @file    apie_cable.c
  * @brief   Cable + EPR intelligence.
  ******************************************************************************
  */
#include "apie_cable.h"
#include "apie_decode.h"
#include "app_log.h"
#include <stdio.h>

static APIE_CableProfile_t s_cable;
static APIE_EPR_Info_t s_epr;

void APIE_Cable_Init(void)
{
  memset(&s_cable, 0, sizeof(s_cable));
  memset(&s_epr, 0, sizeof(s_epr));
}

void APIE_Cable_ResetSession(uint8_t conn_id)
{
  (void)conn_id;
  memset(&s_cable, 0, sizeof(s_cable));
  s_epr.state = APIE_EPR_STATE_NOT_IN_EPR;
}

void APIE_Cable_OnIdentity(uint8_t sop, const uint8_t *vdo, uint8_t nvdo)
{
  uint32_t idhdr, cert, prod;
  (void)nvdo;
  if (vdo == NULL) { return; }
  s_cable.present = 1U;
  s_cable.sop = sop;
  /* ID header VDO (cable plug): VID + product type + current cap. */
  idhdr = (uint32_t)(vdo[0] | ((uint32_t)vdo[1] << 8U) | ((uint32_t)vdo[2] << 16U) | ((uint32_t)vdo[3] << 24U));
  s_cable.vid = (uint16_t)(idhdr & 0xFFFFu);
  cert = (uint32_t)(vdo[4] | ((uint32_t)vdo[5] << 8U) | ((uint32_t)vdo[6] << 16U) | ((uint32_t)vdo[7] << 24U));
  prod = (uint32_t)(vdo[8] | ((uint32_t)vdo[9] << 8U) | ((uint32_t)vdo[10] << 16U) | ((uint32_t)vdo[11] << 24U));
  s_cable.pid = (uint16_t)((prod >> 16) & 0xFFFFu);
  s_cable.hw_rev = (prod & 0xFFFFu);
  s_cable.fw_rev = (cert & 0xFFFFu);
  if (sop == 1u || sop == 2u)
  {
    s_cable.current_cap = (uint8_t)((idhdr >> 7) & 0x03u);
    s_cable.ss_cap = (uint8_t)((idhdr >> 9) & 0x03u);
    s_cable.active = (uint8_t)((idhdr >> 18) & 0x01u);
    s_cable.vconn = (uint8_t)((idhdr >> 19) & 0x01u);
  }
}

void APIE_Cable_OnSopData(uint8_t sop, const uint8_t *payload, uint16_t len)
{
  /* Observed SOP'/SOP'' traffic: used by the unknown analyzer's cable path. */
  (void)sop; (void)payload; (void)len;
}

void APIE_EPR_OnSourceCaps(uint8_t port, const uint32_t *pdo, uint8_t n)
{
  uint8_t i;
  (void)port;
  if (pdo == NULL) { return; }
  for (i = 0U; i < n; i++)
  {
    uint32_t min_mv = 0U, max_mv = 0U, ma = 0U, mwp = 0U;
    APIE_Decode_PdoCaps(pdo[i], &min_mv, &max_mv, &ma, &mwp);
    if (APIE_Decode_PdoType(pdo[i]) == APIE_PDO_TYPE_APDO &&
        APIE_Decode_ApdoType(pdo[i]) == APIE_APDO_TYPE_AVS)
    {
      /* AVS APDO: an EPR-capable source profile. */
      s_epr.epr_capable = 1U;
      s_epr.avs_present = 1U;
      s_epr.avs_min_mv = (uint16_t)min_mv;
      s_epr.avs_max_mv = (uint16_t)max_mv;
      if (s_epr.state == APIE_EPR_STATE_NOT_IN_EPR)
      {
        s_epr.state = APIE_EPR_STATE_SRC_CAP;
      }
    }
  }
}

void APIE_EPR_OnModeChange(uint8_t new_state)
{
  s_epr.state = new_state;
}

void APIE_EPR_OnAvs(const uint8_t *payload, uint16_t len)
{
  (void)payload; (void)len;
  s_epr.state = APIE_EPR_STATE_MODE_ACTIVE;
}

const APIE_CableProfile_t *APIE_Cable_Get(void) { return &s_cable; }
const APIE_EPR_Info_t *APIE_EPR_Get(void) { return &s_epr; }

uint8_t APIE_EPR_PowerAllowed(void)
{
  return (uint8_t)(APIE_HW_EPR_POWER_ENABLED ? 1U : 0U);
}

void APIE_Cable_Dump(void)
{
  APP_LOG_Write("cable:\r\n");
  if (s_cable.present == 0U)
  {
    APP_LOG_Write("  no cable identity observed (no SOP'/SOP'' e-marker yet)\r\n");
    return;
  }
  APP_LOG_Printf("  present : yes  sop=%s\r\n", APIE_SopName(s_cable.sop));
  APP_LOG_Printf("  vid/pid : 0x%04X / 0x%04X\r\n", (unsigned)s_cable.vid, (unsigned)s_cable.pid);
  APP_LOG_Printf("  current : %s\r\n",
                 (s_cable.current_cap == 2u) ? "5 A" : (s_cable.current_cap == 1u) ? "3 A" : "default(0.5-1.5A)");
  APP_LOG_Printf("  active  : %s  vconn: %s\r\n",
                 s_cable.active ? "yes" : "no", s_cable.vconn ? "yes" : "no");
}

void APIE_EPR_Dump(void)
{
  APP_LOG_Write("epr:\r\n");
  APP_LOG_Printf("  capable   : %s\r\n", s_epr.epr_capable ? "yes" : "no");
  APP_LOG_Printf("  avs       : %s  (%u-%u mV)\r\n",
                 s_epr.avs_present ? "yes" : "no",
                 (unsigned)s_epr.avs_min_mv, (unsigned)s_epr.avs_max_mv);
  APP_LOG_Printf("  state     : %u\r\n", (unsigned)s_epr.state);
  APP_LOG_Printf("  power     : %s (hardware %s)\r\n",
                 APIE_EPR_PowerAllowed() ? "ALLOWED" : "DISABLED",
                 APIE_HW_EPR_POWER_ENABLED ? "enabled" : "flag off");
}

/* Small helper referenced in the dump. */
const char *APIE_SopName(uint8_t sop)
{
  switch (sop)
  {
    case 1u: return "SOP'";
    case 2u: return "SOP''";
    default: return "SOP";
  }
}
