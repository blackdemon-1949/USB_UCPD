#include "app_pd.h"
#include "app_log.h"
#include "app_board.h"
#include "apie.h"
#include "apie_profile.h"
#include "usbpd_pdo_defs.h"
#include "usbpd_dpm_conf.h"
#include "usbpd_core.h"
#include "usbpd_dpm_core.h"
#include "usbpd_hw_if.h"
#if defined(_TRACE)
#include "usbpd_trace.h"
#endif
#include <string.h>
#include <stdio.h>

extern uint32_t HAL_GetTick(void);
static uint32_t s_vbus_restore_at;   /* when to re-assert the synthetic 5 V after a hard reset */
static uint32_t s_diag_next;         /* next PHY diagnostic trace timestamp */

/* automation state */
static uint32_t s_auto_mv;           /* 0 = no auto request */
static uint32_t s_auto_ma;
static uint8_t  s_remember;
static uint8_t  s_auto_applied;      /* 1 = auto request already sent for this attach */
static uint8_t  s_auto_pending;      /* 1 = SNK_READY seen, auto request to apply in task */
static uint8_t  s_snk_ready_seen;    /* 1 = "[PD] SNK ready" already printed this attach */
static uint8_t  s_caps_printed;      /* 1 = caps table already printed for current PDO set */
static uint32_t s_sweep_to, s_sweep_step, s_sweep_ma;
static uint32_t s_sweep_next_mv;
static uint32_t s_sweep_next_time;
static uint8_t  s_sweep_active;

static void APP_PD_AutoApply(uint8_t port);

APP_PD_Port_t APP_PD_Port[USBPD_PORT_COUNT];

void APP_PD_Init(void)
{
  memset(APP_PD_Port, 0, sizeof(APP_PD_Port));
  APP_PD_Port[0].SyntheticVbusMv = 0;
  s_vbus_restore_at = 0U;
  s_diag_next = HAL_GetTick() + 1000U;
  s_auto_mv = 0U;
  s_auto_ma = 0U;
  s_remember = 0U;
  s_auto_applied = 0U;
  s_auto_pending = 0U;
  s_snk_ready_seen = 0U;
  s_caps_printed = 0U;
  s_sweep_active = 0U;
}

uint8_t APP_PD_IsAttached(void)
{
  return APP_PD_Port[0].Attached;
}

uint8_t APP_PD_HasContract(void)
{
  return APP_PD_Port[0].Contract;
}

uint32_t APP_PD_GetVbusMv(void)
{
  return APP_PD_Port[0].SyntheticVbusMv;
}

void APP_PD_SetVbusMv(uint32_t mv)
{
  APP_PD_Port[0].SyntheticVbusMv = mv;
}

void APP_PD_OnCable(uint8_t port, USBPD_CAD_EVENT ev)
{
  if (port >= USBPD_PORT_COUNT)
  {
    return;
  }
  switch (ev)
  {
    case USBPD_CAD_EVENT_ATTACHED:
    case USBPD_CAD_EVENT_ATTEMC:
      APP_PD_Port[port].Attached = 1U;
      APP_PD_Port[port].Contract = 0U;
      APP_PD_Port[port].NumberOfRcvSRCPDO = 0U;
      APP_PD_Port[port].UserSelected = 0U;
      APP_PD_Port[port].PendingIndex = 0U;
      APP_PD_Port[port].CCx = (uint8_t)DPM_Params[port].ActiveCCIs;
      /* CC-only tester: PE waits for Vsafe5V after Accept. Synthesize 5 V. */
      APP_PD_Port[port].SyntheticVbusMv = 5000U;
      s_vbus_restore_at = 0U;
      s_auto_applied = 0U;
      s_snk_ready_seen = 0U;
      s_caps_printed = 0U;
      APP_LED_Set(APP_LED_PD_WAIT);
      APP_LOG_Printf("\r\n[PD] CC attached (event %u, CC%u)\r\n",
                     (unsigned)ev, (unsigned)APP_PD_Port[port].CCx);
      APIE_OnCableAttach(port, APP_PD_Port[port].CCx);
      break;
    case USBPD_CAD_EVENT_DETACHED:
    case USBPD_CAD_EVENT_EMC:
    default:
      APP_PD_Port[port].Attached = 0U;
      APP_PD_Port[port].Contract = 0U;
      APP_PD_Port[port].NumberOfRcvSRCPDO = 0U;
      APP_PD_Port[port].SyntheticVbusMv = 0U;
      s_vbus_restore_at = 0U;
      s_auto_applied = 0U;
      s_snk_ready_seen = 0U;
      s_caps_printed = 0U;
      s_sweep_active = 0U;
      APP_LED_Set(APP_LED_HEARTBEAT);
      APP_LOG_Printf("\r\n[PD] CC detached\r\n");
      APIE_OnCableDetach(port);
      break;
  }
}

void APP_PD_OnNotify(uint8_t port, USBPD_NotifyEventValue_TypeDef ev)
{
  if (port >= USBPD_PORT_COUNT)
  {
    return;
  }
  APP_PD_Port[port].LastNotify = (uint32_t)ev;
  APIE_OnNotify(port, (uint32_t)ev, APP_PD_Port[port].RequestedVoltage,
                APP_PD_Port[port].RequestedCurrent, APP_PD_Port[port].RDOPosition);
  switch (ev)
  {
    case USBPD_NOTIFY_POWER_EXPLICIT_CONTRACT:
      APP_PD_Port[port].Contract = 1U;
      APP_LED_Set(APP_LED_PD_CONTRACT);
      APP_LOG_Printf("[PD] explicit contract  %lu mV / %lu mA  (PDO %lu)\r\n",
                     (unsigned long)APP_PD_Port[port].RequestedVoltage,
                     (unsigned long)APP_PD_Port[port].RequestedCurrent,
                     (unsigned long)APP_PD_Port[port].RDOPosition);
      break;
    case USBPD_NOTIFY_REQUEST_ACCEPTED:
      APP_LOG_Write("[PD] REQUEST accepted\r\n");
      break;
    case USBPD_NOTIFY_REQUEST_REJECTED:
      APP_LOG_Write("[PD] REQUEST rejected\r\n");
      break;
    case USBPD_NOTIFY_REQUEST_WAIT:
      APP_LOG_Write("[PD] REQUEST wait\r\n");
      break;
    case USBPD_NOTIFY_HARDRESET_RX:
    case USBPD_NOTIFY_HARDRESET_TX:
      APP_PD_Port[port].Contract = 0U;
      /* The PD hard-reset protocol requires VBUS to fall to vSafe0V and then
         come back to 5 V.  There is no VBUS ADC on this rig, so synthesize it:
         drop to 0 V now, re-assert 5 V after the source's error-recovery. */
      APP_PD_Port[port].SyntheticVbusMv = 0U;
      s_vbus_restore_at = HAL_GetTick() + 50U;
      s_snk_ready_seen = 0U;   /* report once when the sink is ready again */
      APP_LED_Set(APP_PD_Port[port].Attached ? APP_LED_PD_WAIT : APP_LED_HEARTBEAT);
      APP_LOG_Write("[PD] hard reset\r\n");
      break;
    case USBPD_NOTIFY_STATE_SNK_READY:
      /* The PE (re-)enters SNK_READY repeatedly: on attach, after every
         request, after error recovery, and whenever the source re-advertises
         its capabilities.  Print the banner once per attach / hard reset so
         the console is not flooded; auto-apply still runs on every entry. */
      if (s_snk_ready_seen == 0U)
      {
        APP_LOG_Write("[PD] SNK ready\r\n");
        s_snk_ready_seen = 1U;
      }
      /* The PE is now ready to accept a request.  Queue the auto/remember
         application for the main loop (APP_PD_Task) so we do not re-enter the
         PE state machine from inside its own notification callback. */
      s_auto_pending = 1U;
      break;
    default:
      break;
  }
}

void APP_PD_Task(void)
{
  uint32_t now = HAL_GetTick();

  /* Re-assert the synthetic 5 V after a hard reset, so the PE can leave
     PE_SNK_HARD_RESET_WAIT_VSAFE_0V and go back to waiting for caps. */
  if ((s_vbus_restore_at != 0U) && ((int32_t)(now - s_vbus_restore_at) >= 0))
  {
    s_vbus_restore_at = 0U;
    if (APP_PD_Port[0].Attached != 0U)
    {
      APP_PD_Port[0].SyntheticVbusMv = 5000U;
    }
  }

  /* A sink must not actively request Source_Capabilities on attach: it waits
     for the source to advertise them.  The PE already does that in
     PE_SNK_WAIT_FOR_CAPABILITIES, so the old 150 ms/500 ms auto-request only
     produced a constant stream of "GET_SRC_CAPA not accepted by the stack"
     debug traces and served no purpose. */

  /* Apply the pending auto / remember request once the PE is ready. */
  if (s_auto_pending != 0U)
  {
    s_auto_pending = 0U;
    APP_PD_AutoApply(0U);
  }

  /* Voltage sweep (PPS): request the next step every 500 ms. */
  if (s_sweep_active != 0U)
  {
    if ((APP_PD_Port[0].Attached == 0U) || (APP_PD_Port[0].NumberOfRcvSRCPDO == 0U))
    {
      s_sweep_active = 0U;
    }
    else if ((int32_t)(now - s_sweep_next_time) >= 0)
    {
      if (s_sweep_next_mv > s_sweep_to)
      {
        s_sweep_active = 0U;
        APP_LOG_Write("Sweep finished.\r\n");
      }
      else
      {
        uint8_t idx = 0U;
        uint8_t is_pps = 0U;
        APP_PD_FindBestPdo(s_sweep_next_mv, &idx, &is_pps);
        if ((idx == 0U) || (is_pps == 0U))
        {
          s_sweep_active = 0U;
          APP_LOG_Printf("Sweep stopped: the source has no PPS range covering %lu mV.\r\n",
                         (unsigned long)s_sweep_next_mv);
        }
        else
        {
          USBPD_StatusTypeDef st = APP_PD_SendRequest(0U, idx, (uint16_t)s_sweep_next_mv, (uint16_t)s_sweep_ma);
          if (st == USBPD_OK)
          {
            APP_LOG_Printf("[sweep] asking for %lu mV\r\n", (unsigned long)s_sweep_next_mv);
            s_sweep_next_mv += s_sweep_step;
            s_sweep_next_time = now + 500U;
          }
          else
          {
            /* not ready to accept a request yet - retry the same step shortly */
            s_sweep_next_time = now + 150U;
          }
        }
      }
    }
  }

#if defined(_TRACE)
  /* Periodic PHY diagnostic: report what the UCPD actually sees on the CC
     line, so a CubeMonitor capture shows whether the source is transmitting
     and what message type arrived. */
  if ((int32_t)(now - s_diag_next) >= 0)
  {
    char _s[128];
    uint32_t _cr = UCPD1->CR;
    uint32_t _sr = UCPD1->SR;
    int _n = snprintf(_s, sizeof(_s),
      "PHY ord=%lu ok=%lu err=%lu ovr=%lu hrst=%lu rxhdr=0x%04X sop=%u cr=0x%08lX sr=0x%08lX rxbuf=0x%08lX",
      (unsigned long)g_usbpd_dbg.rxorddet,
      (unsigned long)g_usbpd_dbg.rxmsgend_ok,
      (unsigned long)g_usbpd_dbg.rxmsgend_err,
      (unsigned long)g_usbpd_dbg.rxovr,
      (unsigned long)g_usbpd_dbg.rxhrstdet,
      (unsigned)g_usbpd_dbg.last_rx_hdr,
      (unsigned)g_usbpd_dbg.last_rx_sop,
      (unsigned long)_cr,
      (unsigned long)_sr,
      (unsigned long)(uintptr_t)Ports[0].ptr_RxBuff);
    if (_n > 0)
    {
      USBPD_TRACE_Add(USBPD_TRACE_DEBUG, 0, 0, (uint8_t *)_s, (uint32_t)_n);
    }
    s_diag_next = now + 1000U;
  }
#endif
}

void APP_PD_StoreSrcPDO(uint8_t port, const uint8_t *ptr, uint32_t size)
{
  uint8_t n;
  uint8_t changed;
  if ((port >= USBPD_PORT_COUNT) || (ptr == NULL))
  {
    return;
  }
  n = (uint8_t)(size / 4U);
  if (n > USBPD_MAX_NB_PDO)
  {
    n = USBPD_MAX_NB_PDO;
  }

  /* Sources re-send Source_Capabilities all the time (negotiation retries,
     periodic re-advertisements, hard-reset recovery).  Only print the table
     when it has not been shown yet or the offered PDOs actually changed, so
     an unchanged offer does not flood the console. */
  changed = (uint8_t)((n != APP_PD_Port[port].NumberOfRcvSRCPDO) ||
                      (memcmp(APP_PD_Port[port].ListOfRcvSRCPDO, ptr, (size_t)n * 4U) != 0));

  memcpy(APP_PD_Port[port].ListOfRcvSRCPDO, ptr, (size_t)n * 4U);
  APP_PD_Port[port].NumberOfRcvSRCPDO = n;

  /* Feed the intelligence layer: profile, PDO signature, EPR awareness. */
  APIE_OnSourceCaps(port, APP_PD_Port[port].ListOfRcvSRCPDO, n);

  if ((changed != 0U) || (s_caps_printed == 0U))
  {
    APP_LOG_Printf("[PD] source capabilities (%u PDO)\r\n", (unsigned)n);
    APP_PD_PrintCaps();
    s_caps_printed = 1U;
  }

  /* Note: automation (auto <mv> / remember) is applied from the SNK_READY
     notification, once the PE has finished its default 5 V negotiation. */
}

void APP_PD_RequestCapsPrint(void)
{
  /* Force the next received Source_Capabilities to be printed even if the
     PDO set did not change (used by the 'getcaps' command). */
  s_caps_printed = 0U;
}

void APP_PD_FormatPdo(uint32_t pdo, char *out, uint32_t outsz)
{
  USBPD_PDO_TypeDef u;
  u.d32 = pdo;
  switch (u.GenericPDO.PowerObject)
  {
    case USBPD_CORE_PDO_TYPE_FIXED:
    {
      uint32_t mv = (uint32_t)u.SRCFixedPDO.VoltageIn50mVunits * 50U;
      uint32_t ma = (uint32_t)u.SRCFixedPDO.MaxCurrentIn10mAunits * 10U;
      snprintf(out, outsz, "FIXED  %lu mV  %lu mA  (%lu mW)",
               (unsigned long)mv, (unsigned long)ma,
               (unsigned long)((mv * ma) / 1000U));
      break;
    }
    case USBPD_CORE_PDO_TYPE_BATTERY:
      snprintf(out, outsz, "BATTERY  %u-%u mV",
               (unsigned)(u.SRCBatteryPDO.MinVoltageIn50mVunits * 50U),
               (unsigned)(u.SRCBatteryPDO.MaxVoltageIn50mVunits * 50U));
      break;
    case USBPD_CORE_PDO_TYPE_VARIABLE:
      snprintf(out, outsz, "VARIABLE  %u-%u mV  %u mA",
               (unsigned)(u.SRCVariablePDO.MinVoltageIn50mVunits * 50U),
               (unsigned)(u.SRCVariablePDO.MaxVoltageIn50mVunits * 50U),
               (unsigned)(u.SRCVariablePDO.MaxCurrentIn10mAunits * 10U));
      break;
#ifdef USBPDCORE_PPS
    case USBPD_CORE_PDO_TYPE_APDO:
      snprintf(out, outsz, "PPS APDO  %u-%u mV  max %u mA",
               (unsigned)(u.SRCSNKAPDO.MinVoltageIn100mV * 100U),
               (unsigned)(u.SRCSNKAPDO.MaxVoltageIn100mV * 100U),
               (unsigned)(u.SRCSNKAPDO.MaxCurrentIn50mAunits * 50U));
      break;
#endif
    default:
      snprintf(out, outsz, "UNKNOWN  0x%08lX", (unsigned long)pdo);
      break;
  }
}

void APP_PD_PrintCaps(void)
{
  char line[96];
  uint8_t i;
  uint8_t n = APP_PD_Port[0].NumberOfRcvSRCPDO;
  if ((APP_PD_Port[0].Attached == 0U) || (n == 0U))
  {
    APP_LOG_Write("no source capabilities (attach a PD source on one of PM0/PM1 plus GND)\r\n");
    return;
  }
  APP_LOG_Write("The source offers these power levels:\r\n");
  for (i = 0; i < n; i++)
  {
    APP_PD_FormatPdo(APP_PD_Port[0].ListOfRcvSRCPDO[i], line, sizeof(line));
    APP_LOG_Printf("  [%u] %s\r\n", (unsigned)(i + 1U), line);
  }
  APP_LOG_Write("Ask for one with:  req <n> [ma]   |   volt <mv> [ma]   |   pps <mv> [ma]\r\n");
}

void APP_PD_PrintStatus(void)
{
  APP_PD_Port_t *p = &APP_PD_Port[0];

  APP_LOG_Write("---- status ----\r\n");
  if (p->Attached == 0U)
  {
    APP_LOG_Write("No power source is connected. Connect one to PM0 (CC1) or PM1 (CC2) and GND.\r\n");
    return;
  }

  APP_LOG_Printf("A power source is connected on CC%u.\r\n", (unsigned)p->CCx);

  if (p->Contract != 0U)
  {
    APP_LOG_Printf("Negotiation is complete: an explicit power contract is active.\r\n");
  }
  else
  {
    APP_LOG_Write("No explicit contract yet (the source is still being negotiated).\r\n");
  }

  if (p->RDOPosition != 0U)
  {
    APP_LOG_Printf("We asked for: PDO %lu  ->  %lu mV at %lu mA.\r\n",
                   (unsigned long)p->RDOPosition,
                   (unsigned long)p->RequestedVoltage,
                   (unsigned long)p->RequestedCurrent);
    APP_LOG_Printf("The source's VBUS rail should now be at %lu mV (measure it on the source side).\r\n",
                   (unsigned long)p->SyntheticVbusMv);
  }
  else
  {
    APP_LOG_Write("No power request has been made yet.\r\n");
  }

  APP_LOG_Printf("The source offered %u power levels (type 'caps' to list them).\r\n",
                 (unsigned)p->NumberOfRcvSRCPDO);

  if (s_auto_mv != 0U)
  {
    APP_LOG_Printf("Auto request is ON: %lu mV%s.\r\n",
                   (unsigned long)s_auto_mv,
                   (s_auto_ma != 0U) ? " with a current limit" : "");
  }
  if (s_remember != 0U)
  {
    APP_LOG_Write("Remember mode is ON: your last request will be re-applied after re-connect.\r\n");
  }
  if (s_sweep_active != 0U)
  {
    APP_LOG_Write("A voltage sweep is running (type 'sweep stop' to stop it).\r\n");
  }
}

static uint8_t find_5v_index(uint8_t port)
{
  uint8_t i;
  for (i = 0; i < APP_PD_Port[port].NumberOfRcvSRCPDO; i++)
  {
    USBPD_PDO_TypeDef u;
    u.d32 = APP_PD_Port[port].ListOfRcvSRCPDO[i];
    if (u.GenericPDO.PowerObject == USBPD_CORE_PDO_TYPE_FIXED)
    {
      uint32_t mv = (uint32_t)u.SRCFixedPDO.VoltageIn50mVunits * 50U;
      if (mv == 5000U)
      {
        return (uint8_t)(i + 1U);
      }
    }
  }
  return (APP_PD_Port[port].NumberOfRcvSRCPDO > 0U) ? 1U : 0U;
}

static int build_rdo(uint8_t port, uint8_t index, uint16_t mv, uint16_t ma,
                     uint32_t *rdo, USBPD_CORE_PDO_Type_TypeDef *type)
{
  USBPD_PDO_TypeDef pdo;
  USBPD_SNKRDO_TypeDef req;
  uint8_t i;

  if ((index == 0U) || (index > APP_PD_Port[port].NumberOfRcvSRCPDO))
  {
    return -1;
  }
  i = (uint8_t)(index - 1U);
  pdo.d32 = APP_PD_Port[port].ListOfRcvSRCPDO[i];
  req.d32 = 0;

  switch (pdo.GenericPDO.PowerObject)
  {
    case USBPD_CORE_PDO_TYPE_FIXED:
    {
      uint32_t src_mv = (uint32_t)pdo.SRCFixedPDO.VoltageIn50mVunits * 50U;
      uint32_t src_ma = (uint32_t)pdo.SRCFixedPDO.MaxCurrentIn10mAunits * 10U;
      uint32_t op_ma = (ma != 0U) ? (uint32_t)ma : src_ma;
      if (op_ma > src_ma)
      {
        op_ma = src_ma;
      }
      req.FixedVariableRDO.ObjectPosition = index;
      req.FixedVariableRDO.OperatingCurrentIn10mAunits = (uint32_t)(op_ma / 10U);
      req.FixedVariableRDO.MaxOperatingCurrent10mAunits = (uint32_t)(src_ma / 10U);
      req.FixedVariableRDO.NoUSBSuspend = 1U;
      req.FixedVariableRDO.USBCommunicationsCapable = 0U;
      APP_PD_Port[port].RequestedVoltage = src_mv;
      APP_PD_Port[port].RequestedCurrent = op_ma;
      *type = USBPD_CORE_PDO_TYPE_FIXED;
      break;
    }
#ifdef USBPDCORE_PPS
    case USBPD_CORE_PDO_TYPE_APDO:
    {
      uint32_t min_mv = (uint32_t)pdo.SRCSNKAPDO.MinVoltageIn100mV * 100U;
      uint32_t max_mv = (uint32_t)pdo.SRCSNKAPDO.MaxVoltageIn100mV * 100U;
      uint32_t max_ma = (uint32_t)pdo.SRCSNKAPDO.MaxCurrentIn50mAunits * 50U;
      uint32_t req_mv = (mv != 0U) ? (uint32_t)mv : min_mv;
      uint32_t req_ma = (ma != 0U) ? (uint32_t)ma : max_ma;
      if (req_mv < min_mv) { req_mv = min_mv; }
      if (req_mv > max_mv) { req_mv = max_mv; }
      if (req_ma > max_ma) { req_ma = max_ma; }
      req.ProgRDO.ObjectPosition = index;
      req.ProgRDO.OutputVoltageIn20mV = (uint32_t)(req_mv / 20U);
      req.ProgRDO.OperatingCurrentIn50mAunits = (uint32_t)(req_ma / 50U);
      req.ProgRDO.NoUSBSuspend = 1U;
      APP_PD_Port[port].RequestedVoltage = req_mv;
      APP_PD_Port[port].RequestedCurrent = req_ma;
      *type = USBPD_CORE_PDO_TYPE_APDO;
      break;
    }
#endif
    case USBPD_CORE_PDO_TYPE_VARIABLE:
    {
      uint32_t min_mv = (uint32_t)pdo.SRCVariablePDO.MinVoltageIn50mVunits * 50U;
      uint32_t max_mv = (uint32_t)pdo.SRCVariablePDO.MaxVoltageIn50mVunits * 50U;
      uint32_t src_ma = (uint32_t)pdo.SRCVariablePDO.MaxCurrentIn10mAunits * 10U;
      uint32_t req_mv = (mv != 0U) ? (uint32_t)mv : min_mv;
      if (req_mv < min_mv) { req_mv = min_mv; }
      if (req_mv > max_mv) { req_mv = max_mv; }
      req.FixedVariableRDO.ObjectPosition = index;
      req.FixedVariableRDO.OperatingCurrentIn10mAunits = (uint32_t)(src_ma / 10U);
      req.FixedVariableRDO.MaxOperatingCurrent10mAunits = (uint32_t)(src_ma / 10U);
      req.FixedVariableRDO.NoUSBSuspend = 1U;
      APP_PD_Port[port].RequestedVoltage = req_mv;
      APP_PD_Port[port].RequestedCurrent = src_ma;
      *type = USBPD_CORE_PDO_TYPE_VARIABLE;
      break;
    }
    default:
      return -1;
  }

  APP_PD_Port[port].RDOPosition = index;
  APP_PD_Port[port].RequestDOMsg = req.d32;
  APP_PD_Port[port].SyntheticVbusMv = APP_PD_Port[port].RequestedVoltage;
  *rdo = req.d32;
  return 0;
}

void APP_PD_Evaluate(uint8_t port, uint32_t *rdo, USBPD_CORE_PDO_Type_TypeDef *type)
{
  uint8_t index;
  uint16_t mv;
  uint16_t ma;

  if ((rdo == NULL) || (type == NULL) || (port >= USBPD_PORT_COUNT))
  {
    return;
  }
  if (APP_PD_Port[port].NumberOfRcvSRCPDO == 0U)
  {
    *rdo = 0;
    *type = USBPD_CORE_PDO_TYPE_FIXED;
    return;
  }

  if (APP_PD_Port[port].PendingIndex != 0U)
  {
    index = APP_PD_Port[port].PendingIndex;
    mv = APP_PD_Port[port].PendingVoltage;
    ma = APP_PD_Port[port].PendingCurrent;
  }
  else
  {
    index = find_5v_index(port);
    mv = 5000U;
    ma = 0U;
  }

  if (build_rdo(port, index, mv, ma, rdo, type) != 0)
  {
    (void)build_rdo(port, 1U, 0U, 0U, rdo, type);
  }
}

USBPD_StatusTypeDef APP_PD_SendRequest(uint8_t port, uint8_t index, uint16_t mv, uint16_t ma)
{
  uint32_t rdo;
  USBPD_CORE_PDO_Type_TypeDef type;
  USBPD_StatusTypeDef st;

  /* Same bound check the sibling APP_PD_Evaluate() uses: every caller today
     passes 0 (or the stack's PortNum, which is 0 with USBPD_PORT_COUNT == 1),
     but APP_PD_Port[] is indexed directly below and this keeps the two entry
     points consistent. */
  if (port >= USBPD_PORT_COUNT)
  {
    return USBPD_ERROR;
  }
  if (APP_PD_Port[port].Attached == 0U)
  {
    APP_LOG_Write("not attached\r\n");
    return USBPD_ERROR;
  }
  if (APP_PD_Port[port].NumberOfRcvSRCPDO == 0U)
  {
    APP_LOG_Write("no source caps yet — try getcaps\r\n");
    return USBPD_ERROR;
  }

  APP_PD_Port[port].PendingIndex = index;
  APP_PD_Port[port].PendingVoltage = mv;
  APP_PD_Port[port].PendingCurrent = ma;
  APP_PD_Port[port].UserSelected = 1U;

  if (build_rdo(port, index, mv, ma, &rdo, &type) != 0)
  {
    APP_LOG_Write("cannot build RDO for that PDO\r\n");
    return USBPD_ERROR;
  }

  st = USBPD_PE_Send_Request(port, rdo, type);
  if (st != USBPD_OK)
  {
    APP_LOG_Printf("REQUEST not accepted by stack (%d)\r\n", (int)st);
  }
  else
  {
    APP_LOG_Printf("REQUEST sent  PDO %u  %lu mV / %lu mA\r\n",
                   (unsigned)index,
                   (unsigned long)APP_PD_Port[port].RequestedVoltage,
                   (unsigned long)APP_PD_Port[port].RequestedCurrent);
    APIE_OnRequestSent(port, index, (uint16_t)APP_PD_Port[port].RequestedVoltage,
                       (uint16_t)APP_PD_Port[port].RequestedCurrent);
    /* remember for the 'remember' feature (re-apply after re-attach) */
    APP_PD_Port[port].HaveLast = 1U;
    APP_PD_Port[port].LastIndex = index;
    APP_PD_Port[port].LastMv = APP_PD_Port[port].RequestedVoltage;
    APP_PD_Port[port].LastMa = APP_PD_Port[port].RequestedCurrent;
  }
  return st;
}

/* ============================================================================
 *  Source-detail helpers: find best PDO, automation, and plain-English
 *  decoding of the replies (Status, PPS status, extended caps, manufacturer
 *  info, battery, country codes/info) plus VDM identity/SVID/mode results.
 * ========================================================================== */

uint8_t APP_PD_FindBestPdo(uint32_t want_mv, uint8_t *out_index, uint8_t *is_pps)
{
  uint8_t i;
  uint8_t n = APP_PD_Port[0].NumberOfRcvSRCPDO;
  uint8_t best_fixed = 0U;
  uint8_t pps = 0U;
  uint32_t best_diff = 0xFFFFFFFFUL;

  *out_index = 0U;
  *is_pps = 0U;

  for (i = 0U; i < n; i++)
  {
    USBPD_PDO_TypeDef u;
    u.d32 = APP_PD_Port[0].ListOfRcvSRCPDO[i];
    if (u.GenericPDO.PowerObject == USBPD_CORE_PDO_TYPE_FIXED)
    {
      uint32_t mv = (uint32_t)u.SRCFixedPDO.VoltageIn50mVunits * 50U;
      uint32_t d = (mv > want_mv) ? (mv - want_mv) : (want_mv - mv);
      if (d < best_diff)
      {
        best_diff = d;
        best_fixed = (uint8_t)(i + 1U);
      }
    }
#ifdef USBPDCORE_PPS
    else if (u.GenericPDO.PowerObject == USBPD_CORE_PDO_TYPE_APDO)
    {
      uint32_t min_mv = (uint32_t)u.SRCSNKAPDO.MinVoltageIn100mV * 100U;
      uint32_t max_mv = (uint32_t)u.SRCSNKAPDO.MaxVoltageIn100mV * 100U;
      if ((want_mv >= min_mv) && (want_mv <= max_mv))
      {
        pps = (uint8_t)(i + 1U);
      }
    }
#endif
  }

  if (pps != 0U)
  {
    *out_index = pps;
    *is_pps = 1U;
    return 1U;
  }
  if (best_fixed != 0U)
  {
    *out_index = best_fixed;
    return 1U;
  }
  return 0U;
}

static void APP_PD_AutoApply(uint8_t port)
{
  uint8_t index = 0U;
  uint8_t is_pps = 0U;
  uint32_t mv = 0U;
  uint32_t ma = 0U;

  if ((port != 0U) || (s_auto_applied != 0U))
  {
    return;
  }

  if ((s_remember != 0U) && (APP_PD_Port[0].HaveLast != 0U))
  {
    index = APP_PD_Port[0].LastIndex;
    mv = APP_PD_Port[0].LastMv;
    ma = APP_PD_Port[0].LastMa;
    if ((index == 0U) || (index > APP_PD_Port[0].NumberOfRcvSRCPDO))
    {
      if (APP_PD_FindBestPdo(mv, &index, &is_pps) == 0U)
      {
        return;
      }
    }
  }
  else if (s_auto_mv != 0U)
  {
    if (APP_PD_FindBestPdo(s_auto_mv, &index, &is_pps) == 0U)
    {
      return;
    }
    mv = s_auto_mv;
    ma = s_auto_ma;
  }
  else
  {
    return;
  }

  s_auto_applied = 1U;
  APP_LOG_Printf("[auto] asking the source for %lu mV\r\n", (unsigned long)mv);
  (void)APP_PD_SendRequest(0U, index, (uint16_t)mv, (uint16_t)ma);
}

void APP_PD_SetAuto(uint32_t mv, uint32_t ma)
{
  s_auto_mv = mv;
  s_auto_ma = ma;
  if (mv != 0U)
  {
    s_auto_applied = 0U;   /* apply on next capabilities */
  }
}

void APP_PD_GetAuto(uint32_t *mv, uint32_t *ma)
{
  if (mv != NULL) { *mv = s_auto_mv; }
  if (ma != NULL) { *ma = s_auto_ma; }
}

void APP_PD_SetRemember(uint8_t on)
{
  s_remember = (on != 0U) ? 1U : 0U;
}

uint8_t APP_PD_GetRemember(void)
{
  return s_remember;
}

void APP_PD_StartSweep(uint32_t from_mv, uint32_t to_mv, uint32_t step_mv, uint32_t ma)
{
  s_sweep_to = to_mv;
  s_sweep_step = step_mv;
  s_sweep_ma = ma;
  s_sweep_next_mv = from_mv;
  s_sweep_next_time = HAL_GetTick();
  s_sweep_active = 1U;
}

void APP_PD_StopSweep(void)
{
  s_sweep_active = 0U;
}

uint8_t APP_PD_SweepActive(void)
{
  return s_sweep_active;
}

/* ------------------------- plain-English decoders ------------------------- */

static void print_hex(const uint8_t *d, uint16_t n)
{
  uint16_t i;
  for (i = 0U; i < n; i++)
  {
    APP_LOG_Printf("%02X", (unsigned)d[i]);
  }
}

/* Duplicate suppression: the closed library may report the same received
   message through both USBPD_DPM_SetDataInfo and
   USBPD_DPM_ExtendedMessageReceived, so print each kind at most once per
   ~10 ms. */
typedef enum
{
  DEC_STATUS = 1,
  DEC_PPS_STATUS,
  DEC_SRC_EXT,
  DEC_MANUFACTURER,
  DEC_BATTERY_CAP,
  DEC_BATTERY_STATUS,
  DEC_COUNTRY_CODES,
  DEC_COUNTRY_INFO,
} DecKind_t;

static uint32_t s_dec_ms;
static uint8_t  s_dec_kind;

static uint8_t dec_is_dup(uint8_t kind)
{
  uint32_t now = HAL_GetTick();
  if ((s_dec_kind == kind) && ((int32_t)(now - s_dec_ms) < 10))
  {
    return 1U;
  }
  s_dec_kind = kind;
  s_dec_ms = now;
  return 0U;
}

static void dec_status(const uint8_t *ptr, uint32_t size)
{
  if (size < 7U)
  {
    return;
  }
  {
    const USBPD_SDB_TypeDef *s = (const USBPD_SDB_TypeDef *)ptr;
    APP_LOG_Write("Source status report:\r\n");
    APP_LOG_Printf("  Internal temperature : %u degrees C\r\n", (unsigned)s->InternalTemp);
    switch (s->TemperatureStatus & USBPD_SDB_EVENT_TEMP_STATUS_MASK)
    {
      case USBPD_SDB_EVENT_TEMP_STATUS_NORMAL:
        APP_LOG_Write("  Temperature state    : normal\r\n");
        break;
      case USBPD_SDB_EVENT_TEMP_STATUS_WARNING:
        APP_LOG_Write("  Temperature state    : warning (getting hot)\r\n");
        break;
      case USBPD_SDB_EVENT_TEMP_STATUS_OVER_TEMP:
        APP_LOG_Write("  Temperature state    : OVER TEMPERATURE\r\n");
        break;
      default:
        APP_LOG_Write("  Temperature state    : not reported\r\n");
        break;
    }
    APP_LOG_Write("  Input power          : ");
    if (s->PresentInput & USBPD_SDB_PRESENT_INPUT_EXT_PWR)
    {
      APP_LOG_Write((s->PresentInput & USBPD_SDB_PRESENT_INPUT_EXT_PWR_ACDC) ?
                    "external AC mains\r\n" : "external DC\r\n");
    }
    else if (s->PresentInput & USBPD_SDB_PRESENT_INPUT_INT_PWR_FROM_BAT)
    {
      APP_LOG_Write("internal battery\r\n");
    }
    else if (s->PresentInput & USBPD_SDB_PRESENT_INPUT_INT_PWR_FROM_N0_BAT)
    {
      APP_LOG_Write("internal, non-battery source\r\n");
    }
    else
    {
      APP_LOG_Write("not reported\r\n");
    }
    if (s->EventFlags & USBPD_SDB_EVENT_FLAGS_OCP)
    {
      APP_LOG_Write("  Fault                : over-current (OCP)\r\n");
    }
    if (s->EventFlags & USBPD_SDB_EVENT_FLAGS_OTP)
    {
      APP_LOG_Write("  Fault                : over-temperature (OTP)\r\n");
    }
    if (s->EventFlags & USBPD_SDB_EVENT_FLAGS_OVP)
    {
      APP_LOG_Write("  Fault                : over-voltage (OVP)\r\n");
    }
    if ((s->EventFlags & (USBPD_SDB_EVENT_FLAGS_OCP |
                          USBPD_SDB_EVENT_FLAGS_OTP |
                          USBPD_SDB_EVENT_FLAGS_OVP)) == 0U)
    {
      APP_LOG_Write("  Faults               : none reported\r\n");
    }
  }
}

static void dec_pps(const uint8_t *ptr, uint32_t size)
{
#ifdef USBPDCORE_PPS
  if (size < 4U)
  {
    return;
  }
  {
    USBPD_PPSSDB_TypeDef p;
    memcpy(&p.d32, ptr, 4U);
    APP_LOG_Write("PPS output report:\r\n");
    if (p.fields.OutputVoltageIn20mVunits != 0xFFFFU)
    {
      APP_LOG_Printf("  The source is outputting %lu.%02lu V right now.\r\n",
                     (unsigned long)((uint32_t)p.fields.OutputVoltageIn20mVunits * 20U / 1000U),
                     (unsigned long)(((uint32_t)p.fields.OutputVoltageIn20mVunits * 20U) % 1000U / 10U));
    }
    else
    {
      APP_LOG_Write("  Output voltage      : not reported by the source.\r\n");
    }
    if (p.fields.OutputCurrentIn50mAunits != 0xFFU)
    {
      APP_LOG_Printf("  Output current      : %lu.%02lu A.\r\n",
                     (unsigned long)((uint32_t)p.fields.OutputCurrentIn50mAunits * 50U / 1000U),
                     (unsigned long)(((uint32_t)p.fields.OutputCurrentIn50mAunits * 50U) % 1000U / 10U));
    }
    else
    {
      APP_LOG_Write("  Output current      : not reported by the source.\r\n");
    }
    switch ((p.fields.RealTimeFlags >> 1U) & 3U)
    {
      case 1U: APP_LOG_Write("  Power condition     : normal.\r\n"); break;
      case 2U: APP_LOG_Write("  Power condition     : warning (limited).\r\n"); break;
      case 3U: APP_LOG_Write("  Power condition     : OVER TEMPERATURE.\r\n"); break;
      default: APP_LOG_Write("  Power condition     : not reported.\r\n"); break;
    }
  }
#else
  (void)ptr;
  (void)size;
#endif
}

static void dec_srcext(const uint8_t *ptr, uint32_t size)
{
  if (size < sizeof(USBPD_SCEDB_TypeDef))
  {
    return;
  }
  {
    const USBPD_SCEDB_TypeDef *c = (const USBPD_SCEDB_TypeDef *)ptr;
    APP_LOG_Write("Extended source information:\r\n");
    APP_LOG_Printf("  Vendor ID  : 0x%04X\r\n", (unsigned)c->VID);
    APP_LOG_Printf("  Product ID : 0x%04X\r\n", (unsigned)c->PID);
    APP_LOG_Printf("  XID        : 0x%08lX\r\n", (unsigned long)c->XID);
    APP_LOG_Printf("  Firmware revision : %u\r\n", (unsigned)c->FW_revision);
    APP_LOG_Printf("  Hardware revision : %u\r\n", (unsigned)c->HW_revision);
    APP_LOG_Printf("  Source PDP rating : %u (max continuous power)\r\n", (unsigned)c->SourcePDP);
    APP_LOG_Printf("  Peak currents     : %u / %u / %u (x10 mA)\r\n",
                   (unsigned)c->PeakCurrent1, (unsigned)c->PeakCurrent2, (unsigned)c->PeakCurrent3);
    APP_LOG_Printf("  Batteries inside  : %u\r\n", (unsigned)c->NbBatteries);
    /* Feed the intelligence layer with the hard source identity. */
    APIE_Profile_OnIdentity(c->VID, c->PID, (uint8_t)c->FW_revision, (uint8_t)c->HW_revision);
  }
}

static void dec_manu(const uint8_t *ptr, uint32_t size)
{
  if (size < sizeof(USBPD_MIDB_TypeDef))
  {
    return;
  }
  {
    const USBPD_MIDB_TypeDef *m = (const USBPD_MIDB_TypeDef *)ptr;
    char name[23];
    uint8_t i;
    APP_LOG_Write("Manufacturer information:\r\n");
    APP_LOG_Printf("  Vendor ID  : 0x%04X\r\n", (unsigned)m->VID);
    APP_LOG_Printf("  Product ID : 0x%04X\r\n", (unsigned)m->PID);
    for (i = 0U; i < 22U; i++)
    {
      name[i] = ((m->ManuString[i] >= 0x20U) && (m->ManuString[i] < 0x7FU)) ? (char)m->ManuString[i] : '.';
    }
    name[22] = '\0';
    APP_LOG_Printf("  Name       : %s\r\n", name);
  }
}

static void dec_battcap(const uint8_t *ptr, uint32_t size)
{
  if (size < sizeof(USBPD_BCDB_TypeDef))
  {
    return;
  }
  {
    const USBPD_BCDB_TypeDef *b = (const USBPD_BCDB_TypeDef *)ptr;
    APP_LOG_Write("Battery capability:\r\n");
    APP_LOG_Printf("  Vendor ID       : 0x%04X\r\n", (unsigned)b->VID);
    APP_LOG_Printf("  Product ID      : 0x%04X\r\n", (unsigned)b->PID);
    APP_LOG_Printf("  Design capacity : %u (x10 mWh)\r\n", (unsigned)b->BatteryDesignCapa);
    APP_LOG_Printf("  Last full charge: %u (x10 mWh)\r\n", (unsigned)b->BatteryLastFullChargeCapa);
    APP_LOG_Printf("  Battery type    : %u\r\n", (unsigned)b->BatteryType);
  }
}

static void dec_battstat(const uint8_t *ptr, uint32_t size)
{
  if (size < 4U)
  {
    return;
  }
  {
    USBPD_BSDO_TypeDef b;
    memcpy(&b.d32, ptr, 4U);
    APP_LOG_Write("Battery status:\r\n");
    APP_LOG_Printf("  Present capacity : %u (x10 mWh)\r\n", (unsigned)b.b.BatteryPC);
    APP_LOG_Printf("  Battery info     : 0x%02X\r\n", (unsigned)b.b.BatteryInfo);
  }
}

void APP_PD_OnDataInfo(uint8_t port, USBPD_CORE_DataInfoType_TypeDef dataId, const uint8_t *ptr, uint32_t size)
{
  (void)port;
  if (ptr == NULL)
  {
    return;
  }

  /* Feed the intelligence layer: correlate the reply to the pending query. */
  APIE_OnDataInfo(port, (uint16_t)dataId, ptr, size);

  switch (dataId)
  {
    case USBPD_CORE_INFO_STATUS:
      if (!dec_is_dup(DEC_STATUS))       { dec_status(ptr, size); }
      break;
    case USBPD_CORE_PPS_STATUS:
      if (!dec_is_dup(DEC_PPS_STATUS))   { dec_pps(ptr, size); }
      break;
    case USBPD_CORE_EXTENDED_CAPA:
      if (!dec_is_dup(DEC_SRC_EXT))      { dec_srcext(ptr, size); }
      break;
    case USBPD_CORE_MANUFACTURER_INFO:
      if (!dec_is_dup(DEC_MANUFACTURER)) { dec_manu(ptr, size); }
      break;
    case USBPD_CORE_BATTERY_CAPABILITY:
      if (!dec_is_dup(DEC_BATTERY_CAP))  { dec_battcap(ptr, size); }
      break;
    case USBPD_CORE_BATTERY_STATUS:
      if (!dec_is_dup(DEC_BATTERY_STATUS)) { dec_battstat(ptr, size); }
      break;
    default:
      break;
  }
}

void APP_PD_OnExtendedMessage(uint8_t port, USBPD_ExtendedMsg_TypeDef type, const uint8_t *data, uint16_t size)
{
  (void)port;
  if (data == NULL)
  {
    return;
  }

  /* Feed the intelligence layer: correlate the extended reply. */
  APIE_OnExtendedMessage(port, (uint16_t)type, data, size);

  switch (type)
  {
    case USBPD_EXT_STATUS:
      if (!dec_is_dup(DEC_STATUS))       { dec_status(data, size); }
      break;
    case USBPD_EXT_PPS_STATUS:
      if (!dec_is_dup(DEC_PPS_STATUS))   { dec_pps(data, size); }
      break;
    case USBPD_EXT_MANUFACTURER_INFO:
      if (!dec_is_dup(DEC_MANUFACTURER)) { dec_manu(data, size); }
      break;
    case USBPD_EXT_BATTERY_CAPABILITIES:
      if (!dec_is_dup(DEC_BATTERY_CAP))  { dec_battcap(data, size); }
      break;
    case USBPD_EXT_COUNTRY_CODES:
      if (!dec_is_dup(DEC_COUNTRY_CODES) && (size >= 1U))
      {
        uint8_t n = data[0];
        uint8_t i;
        APP_LOG_Printf("The source supports %u country code(s): ", (unsigned)n);
        for (i = 0U; i < n; i++)
        {
          if ((size_t)(2U + (uint16_t)i * 2U) >= (size_t)size)
          {
            break;
          }
          /* Each country code is a 16-bit value, first character in the MSB,
             serialized little-endian: byte0 = second char, byte1 = first char. */
          APP_LOG_Printf("%c%c%s",
                         (char)data[2U + (uint16_t)i * 2U],
                         (char)data[1U + (uint16_t)i * 2U],
                         (i + 1U < n) ? ", " : "");
        }
        APP_LOG_Write("\r\n");
      }
      break;
    case USBPD_EXT_COUNTRY_INFO:
      if (!dec_is_dup(DEC_COUNTRY_INFO) && (size >= 6U))
      {
        APP_LOG_Printf("Country info for %c%c:\r\n", (char)data[1], (char)data[0]);
        APP_LOG_Write("  Country-specific data: ");
        print_hex(&data[6], (uint16_t)(size - 6U));
        APP_LOG_Write("\r\n");
      }
      break;
    case USBPD_EXT_SOURCE_CAPABILITIES:
      /* Chunked source capabilities are handled by USBPD_DPM_SetDataInfo. */
      break;
    default:
      APP_LOG_Printf("The source sent extended message type 0x%02X (%u bytes).\r\n",
                     (unsigned)type, (unsigned)size);
      break;
  }
}

static const char *dfp_product_type_name(uint32_t t)
{
  /* Product type of a DFP (a source). Values are per the USB PD ID Header VDO. */
  if (t == PRODUCT_TYPE_HOST)        { return "USB host"; }
  if (t == PRODUCT_TYPE_POWER_BRICK) { return "power brick (charger)"; }
  if (t == PRODUCT_TYPE_AMC)         { return "alternate-mode controller"; }
  return "unknown";
}

static const char *ufp_product_type_name(uint32_t t)
{
  /* Product type of a UFP / cable plug. */
  if (t == PRODUCT_TYPE_HUB)          { return "USB hub"; }
  if (t == PRODUCT_TYPE_PERIPHERAL)   { return "USB peripheral"; }
  if (t == PRODUCT_TYPE_PSD)          { return "power bank (PSD)"; }
  if (t == PRODUCT_TYPE_ACTIVE_CABLE) { return "active cable"; }
  if (t == PRODUCT_TYPE_AMA)          { return "alternate-mode adapter"; }
  return "unknown";
}

void APP_PD_PrintIdentity(const USBPD_DiscoveryIdentity_TypeDef *id, uint8_t ok)
{
  if ((ok == 0U) || (id == NULL))
  {
    APP_LOG_Write("The source did not provide identity information (it NAKed the request).\r\n");
    return;
  }
  APP_LOG_Write("Source identity (VDM):\r\n");
  APP_LOG_Printf("  Vendor ID   : 0x%04X\r\n", (unsigned)id->IDHeader.b20.VID);
  APP_LOG_Printf("  Product type: %s (UFP/CP side: %s)\r\n",
                 dfp_product_type_name((uint32_t)id->IDHeader.b30.ProductTypeDFP),
                 ufp_product_type_name((uint32_t)id->IDHeader.b20.ProductTypeUFPorCP));
  APP_LOG_Printf("  USB capable : %s / %s\r\n",
                 (id->IDHeader.b20.USBHostCapability == USB_CAPABLE) ? "host" : "not host",
                 (id->IDHeader.b20.USBDevCapability == USB_CAPABLE) ? "device" : "not device");
  APP_LOG_Printf("  Modal modes : %s\r\n",
                 (id->IDHeader.b20.ModalOperation == MODAL_OPERATION_SUPPORTED) ?
                 "supported" : "not supported");
  APP_LOG_Printf("  Product ID  : 0x%04X  (device version 0x%04X)\r\n",
                 (unsigned)id->ProductVDO.b.USBProductId,
                 (unsigned)id->ProductVDO.b.bcdDevice);
  APP_LOG_Printf("  Cert stat   : 0x%08lX\r\n", (unsigned long)id->CertStatVDO.d32);
}

void APP_PD_PrintSvids(const USBPD_SVIDInfo_TypeDef *sv, uint8_t ok)
{
  if ((ok == 0U) || (sv == NULL) || (sv->NumSVIDs == 0U))
  {
    APP_LOG_Write("The source did not list any alternate-mode SVIDs.\r\n");
    return;
  }
  APP_LOG_Printf("The source supports %u alternate-mode SVID(s): ", (unsigned)sv->NumSVIDs);
  uint8_t i;
  for (i = 0U; i < sv->NumSVIDs && i < 12U; i++)
  {
    APP_LOG_Printf("0x%04X%s", (unsigned)sv->SVIDs[i], (i + 1U < sv->NumSVIDs) ? ", " : "");
  }
  APP_LOG_Write("\r\n");
  APP_LOG_Write("(Use 'modes <svid>' to list the modes of one of them.)\r\n");
}

void APP_PD_PrintModes(const USBPD_ModeInfo_TypeDef *md, uint8_t ok)
{
  if ((ok == 0U) || (md == NULL))
  {
    APP_LOG_Write("The source did not report modes for that SVID.\r\n");
    return;
  }
  APP_LOG_Printf("Modes for SVID 0x%04X:\r\n", (unsigned)md->SVID);
  uint32_t i;
  /* USBPD_ModeInfo_TypeDef::Modes is Modes[MAX_MODES_PER_SVID] (6 entries).
     Clamp to the real array size; a bound larger than the array reads past it. */
  for (i = 0U; i < md->NumModes && i < (uint32_t)MAX_MODES_PER_SVID; i++)
  {
    APP_LOG_Printf("  mode %lu : 0x%08lX\r\n", (unsigned long)i, (unsigned long)md->Modes[i]);
  }
}
