/**
  ******************************************************************************
  * @file    apie_decode.c
  * @brief   Deterministic USB-PD message decoder.
  *
  * Field positions follow the normative USB Power Delivery 3.0/3.1
  * specification (USB-IF public technical material):
  *   - 16-bit header: bits[4:0] type, bit6 Port Data Role, bit7 SpecRev,
  *     bit9 Port Power Role, bits[11:10] MessageId, bits[14:12] NumObjects,
  *     bit15 Extended.
  *   - PDO: bits[1:0] role/type, then type-specific fields as below.
  * This decoder is deliberately independent of the ST USBPD library layout.
  ******************************************************************************
  */
#include "apie_decode.h"
#include <stdio.h>

/* --- Control message types (USB-PD spec) --------------------------------- */
#define PD_CTRL_GOODCRC         0x01
#define PD_CTRL_GOTOMIN         0x02
#define PD_CTRL_ACCEPT          0x03
#define PD_CTRL_REJECT          0x04
#define PD_CTRL_PING            0x05
#define PD_CTRL_PS_RDY          0x06
#define PD_CTRL_GET_SRC_CAP     0x07
#define PD_CTRL_GET_SNK_CAP     0x08
#define PD_CTRL_DR_SWAP         0x09
#define PD_CTRL_PR_SWAP         0x0A
#define PD_CTRL_VCONN_SWAP      0x0B
#define PD_CTRL_WAIT            0x0C
#define PD_CTRL_SOFT_RESET      0x0D
#define PD_CTRL_DATA_RESET      0x0E
#define PD_CTRL_DATA_RESET_COMP 0x0F
#define PD_CTRL_NOT_SUPPORTED   0x10
#define PD_CTRL_GET_SRC_CAP_EXT 0x11
#define PD_CTRL_GET_STATUS      0x12
#define PD_CTRL_FR_SWAP         0x13
#define PD_CTRL_GET_PPS_STATUS  0x14
#define PD_CTRL_GET_COUNTRY_CODES 0x15
#define PD_CTRL_GET_SNK_CAP_EXT 0x16
#define PD_CTRL_GET_SOURCE_INFO 0x17
#define PD_CTRL_GET_REVISION    0x18

/* --- Data message types -------------------------------------------------- */
#define PD_DATA_SRC_CAP         0x01
#define PD_DATA_REQUEST         0x02
#define PD_DATA_BIST            0x03
#define PD_DATA_SNK_CAP         0x04
#define PD_DATA_BATTERY_STATUS  0x05
#define PD_DATA_ALERT           0x06
#define PD_DATA_GET_COUNTRY_INFO 0x07
#define PD_DATA_ENTER_USB       0x08
#define PD_DATA_EPR_REQUEST     0x09
#define PD_DATA_EPR_MODE        0x0A
#define PD_DATA_SOURCE_INFO     0x0B
#define PD_DATA_REVISION        0x0C
#define PD_DATA_VENDOR_DEFINED  0x0F

/* --- PDO type (bits[31:30] of the data object, normative) ---------------- */
/* The USB PD specification puts the PDO "type" in the two most significant
   bits of the 32-bit object (USBPD_PDO_TYPE_Pos == 30 in the ST library and
   §6.4.2 of USB PD 3.0/3.1).  This is the reverse of the earlier LSB reading. */
#define PDO_TYPE_FIXED     0x0
#define PDO_TYPE_BATTERY   0x1
#define PDO_TYPE_VARIABLE  0x2
#define PDO_TYPE_APDO      0x3

/* APDO sub-type (bits[29:28]); PPS = 00, AVS (EPR Adjustable) = 01 */
#define APDO_TYPE_PPS      0x0
#define APDO_TYPE_AVS      0x1

/* EPR fixed PDO - the "type" bits[1:0] == 0 is reused; a fixed PDO is EPR
   when source fixed PDO can carry bit30 (EPR) and voltage > 28 V. */

/* SVDM commands (bits[15:8] of VDM header, structured only) */
#define SVDM_DISCOVER_IDENTITY  0x01
#define SVDM_DISCOVER_SVIDS     0x02
#define SVDM_DISCOVER_MODES     0x03
#define SVDM_ENTER_MODE         0x04
#define SVDM_EXIT_MODE          0x05
#define SVDM_ATTENTION          0x06
#define SVDM_SPECIFIC_BASE      0x10

void APIE_Decode_Header(uint16_t hdr, APIE_Header_t *out)
{
  if (out == NULL)
  {
    return;
  }
  out->type          = (uint8_t)(hdr & 0x1Fu);
  /* bit5 reserved */
  out->port_data_role = (uint8_t)((hdr >> 6) & 0x01u);
  out->spec_rev      = (uint8_t)((hdr >> 7) & 0x01u);
  /* bit8 reserved */
  out->port_power_role = (uint8_t)((hdr >> 9) & 0x01u);
  out->msgid         = (uint8_t)((hdr >> 10) & 0x03u);
  out->nobjects      = (uint8_t)((hdr >> 12) & 0x07u);
  out->extended      = (uint8_t)((hdr >> 15) & 0x01u);
  out->chunked       = 0;
}

APIE_MsgClass_t APIE_Decode_Classify(uint8_t hdr_type, uint8_t extended)
{
  if (extended != 0U)
  {
    switch (hdr_type)
    {
      case PD_DATA_VENDOR_DEFINED:
        return APIE_MSG_CLS_VDM_SVDM;
      default:
        return APIE_MSG_CLS_EXTENDED;
    }
  }
  if (hdr_type >= 0x01u && hdr_type <= 0x18u)
  {
    /* control message range (GoodCRC..Get_Revision) */
    if (hdr_type == PD_DATA_VENDOR_DEFINED)
    {
      return APIE_MSG_CLS_VDM_SVDM;
    }
    if (hdr_type >= 0x01u && hdr_type <= 0x0Cu)
    {
      /* The data-message range overlaps the control range.  A control message
         has NumObjects==0 and no payload; the real distinction is driven by
         number of objects and the "extended" bit.  We classify by name here:
         control values are 0x01..0x18 but the wire type field is shared.
         The caller resolves data vs control via nobjects==0. */
    }
    /* We cannot fully disambiguate without nobjects; return CONTROL default
       and let APIE_Decode_TypeName decide using the numeric value. */
    return APIE_MSG_CLS_CONTROL;
  }
  return APIE_MSG_CLS_UNKNOWN;
}

static const char *ctrl_name(uint8_t t)
{
  switch (t)
  {
    case PD_CTRL_GOODCRC:         return "GoodCRC";
    case PD_CTRL_GOTOMIN:         return "GotoMin";
    case PD_CTRL_ACCEPT:          return "Accept";
    case PD_CTRL_REJECT:          return "Reject";
    case PD_CTRL_PING:            return "Ping";
    case PD_CTRL_PS_RDY:          return "PS_RDY";
    case PD_CTRL_GET_SRC_CAP:     return "Get_Source_Cap";
    case PD_CTRL_GET_SNK_CAP:     return "Get_Sink_Cap";
    case PD_CTRL_DR_SWAP:         return "DR_Swap";
    case PD_CTRL_PR_SWAP:         return "PR_Swap";
    case PD_CTRL_VCONN_SWAP:      return "VCONN_Swap";
    case PD_CTRL_WAIT:            return "Wait";
    case PD_CTRL_SOFT_RESET:      return "Soft_Reset";
    case PD_CTRL_DATA_RESET:      return "Data_Reset";
    case PD_CTRL_DATA_RESET_COMP: return "Data_Reset_Complete";
    case PD_CTRL_NOT_SUPPORTED:   return "Not_Supported";
    case PD_CTRL_GET_SRC_CAP_EXT: return "Get_Source_Cap_Ext";
    case PD_CTRL_GET_STATUS:      return "Get_Status";
    case PD_CTRL_FR_SWAP:         return "FR_Swap";
    case PD_CTRL_GET_PPS_STATUS:  return "Get_PPS_Status";
    case PD_CTRL_GET_COUNTRY_CODES: return "Get_Country_Codes";
    case PD_CTRL_GET_SNK_CAP_EXT: return "Get_Sink_Cap_Ext";
    case PD_CTRL_GET_SOURCE_INFO: return "Get_Source_Info";
    case PD_CTRL_GET_REVISION:    return "Get_Revision";
    default:                      return NULL;
  }
}

static const char *data_name(uint8_t t)
{
  switch (t)
  {
    case PD_DATA_SRC_CAP:           return "Source_Capabilities";
    case 0x02u:                     return "Request";
    case PD_DATA_BIST:              return "BIST";
    case PD_DATA_SNK_CAP:           return "Sink_Capabilities";
    case PD_DATA_BATTERY_STATUS:    return "Battery_Status";
    case PD_DATA_ALERT:             return "Alert";
    case PD_DATA_GET_COUNTRY_INFO:  return "Get_Country_Info";
    case PD_DATA_ENTER_USB:         return "Enter_USB";
    case PD_DATA_EPR_REQUEST:       return "EPR_Request";
    case PD_DATA_EPR_MODE:          return "EPR_Mode";
    case PD_DATA_SOURCE_INFO:       return "Source_Info";
    case PD_DATA_REVISION:          return "Revision";
    case PD_DATA_VENDOR_DEFINED:    return "Vendor_Defined";
    default:                        return NULL;
  }
}

void APIE_Decode_TypeNameN(uint8_t hdr_type, uint8_t extended, uint8_t nobjects,
                           char *out, uint32_t outsz)
{
  const char *n = NULL;
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  if (extended != 0U && hdr_type == PD_DATA_VENDOR_DEFINED)
  {
    n = "Extended_Vendor_Defined";
  }
  else if (extended != 0U)
  {
    switch (hdr_type)
    {
      case 0x01: n = "Ext_Source_Capabilities"; break;
      case 0x02: n = "Ext_Status"; break;
      case 0x03: n = "Ext_Get_Battery_Cap"; break;
      case 0x04: n = "Ext_Get_Battery_Status"; break;
      case 0x05: n = "Ext_Battery_Capabilities"; break;
      case 0x06: n = "Ext_Get_Manufacturer_Info"; break;
      case 0x07: n = "Ext_Manufacturer_Info"; break;
      case 0x08: n = "Ext_Security_Request"; break;
      case 0x09: n = "Ext_Security_Response"; break;
      case 0x0A: n = "Ext_FW_Update_Request"; break;
      case 0x0B: n = "Ext_FW_Update_Response"; break;
      case 0x0C: n = "Ext_PPS_Status"; break;
      case 0x0D: n = "Ext_Country_Info"; break;
      case 0x0E: n = "Ext_Country_Codes"; break;
      case 0x0F: n = "Ext_Sink_Capabilities"; break;
      case 0x10: n = "Ext_Control"; break;
      case 0x11: n = "Ext_EPR_Source_Capa"; break;
      case 0x12: n = "Ext_EPR_Sink_Capa"; break;
      case 0x1E: n = "Ext_VDM"; break;
      default: break;
    }
  }
  /* Data messages carry >=1 data object; control carry 0.  A data type and a
     control type share the same numeric value (e.g. 0x01 is GoodCRC control
     and Source_Capabilities data), so disambiguate using nobjects. */
  if (n == NULL && extended == 0U)
  {
    if (nobjects > 0U)
    {
      n = data_name(hdr_type);
    }
    if (n == NULL)
    {
      n = ctrl_name(hdr_type);
    }
  }
  if (n == NULL)
  {
    snprintf(out, outsz, "%s_0x%02X", (extended != 0U) ? "Ext" : "Msg", hdr_type);
    return;
  }
  snprintf(out, outsz, "%s", n);
}

void APIE_Decode_TypeName(uint8_t hdr_type, uint8_t extended, char *out, uint32_t outsz)
{
  /* Legacy form: no nobjects known, resolve as a control message. */
  APIE_Decode_TypeNameN(hdr_type, extended, 0U, out, outsz);
}

static const char *vdm_command_name(uint8_t cmd)
{
  switch (cmd)
  {
    case SVDM_DISCOVER_IDENTITY: return "Discover_Identity";
    case SVDM_DISCOVER_SVIDS:    return "Discover_SVIDs";
    case SVDM_DISCOVER_MODES:    return "Discover_Modes";
    case SVDM_ENTER_MODE:        return "Enter_Mode";
    case SVDM_EXIT_MODE:         return "Exit_Mode";
    case SVDM_ATTENTION:         return "Attention";
    default:
      if (cmd >= SVDM_SPECIFIC_BASE && cmd <= 0x1F)
      {
        return "Specific";
      }
      return NULL;
  }
}

uint8_t APIE_Decode_VdmStructured(uint32_t vdm_hdr)
{
  /* Structured VDM: bit[15] (VDM Type) == 1.  This is the normative position
     (USB PD spec §6.4.3, VDM header <31:16> SVID, <15> VDM type). */
  return (uint8_t)((vdm_hdr >> 15) & 0x01u);
}

uint8_t APIE_Decode_SvdmCommand(uint32_t vdm_hdr)
{
  /* Command field is bits[4:0] of the VDM header. */
  return (uint8_t)(vdm_hdr & 0x1Fu);
}

void APIE_Decode_VDM_Header(uint32_t vdm_hdr, char *out, uint32_t outsz)
{
  uint8_t ver    = (uint8_t)((vdm_hdr >> 13) & 0x03u); /* StructVDM version  */
  uint8_t structd = APIE_Decode_VdmStructured(vdm_hdr);
  uint8_t cmd    = APIE_Decode_SvdmCommand(vdm_hdr);
  uint16_t svid  = (uint16_t)((vdm_hdr >> 16) & 0xFFFFu);
  const char *cn = vdm_command_name(cmd);
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  if (structd != 0U)
  {
    snprintf(out, outsz, "SVDM v%u %s%s svid=0x%04X", ver, (cn != NULL) ? cn : "cmd",
             (cn != NULL) ? "" : "", (unsigned)svid);
  }
  else
  {
    snprintf(out, outsz, "UVDM v%u cmd=0x%02X svid=0x%04X", ver, cmd, (unsigned)svid);
  }
}

void APIE_Decode_VDO(uint32_t vdo, char *out, uint32_t outsz)
{
  /* Best-effort generic VDO text (used when the exact VDO kind is unknown). */
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  snprintf(out, outsz, "VDO 0x%08lX", (unsigned long)vdo);
}

uint8_t APIE_Decode_PdoType(uint32_t pdo)
{
  return (uint8_t)((pdo >> 30) & 0x3u); /* bits[31:30] */
}

uint8_t APIE_Decode_ApdoType(uint32_t pdo)
{
  if (APIE_Decode_PdoType(pdo) != PDO_TYPE_APDO)
  {
    return 0xFFu;
  }
  return (uint8_t)((pdo >> 28) & 0x3u); /* bits[29:28] */
}

void APIE_Decode_PdoCaps(uint32_t pdo, uint32_t *min_mv, uint32_t *max_mv,
                         uint32_t *ma, uint32_t *mwp)
{
  uint32_t t = APIE_Decode_PdoType(pdo);
  if (min_mv != NULL) { *min_mv = 0U; }
  if (max_mv != NULL) { *max_mv = 0U; }
  if (ma    != NULL) { *ma    = 0U; }
  if (mwp   != NULL) { *mwp   = 0U; }
  switch (t)
  {
    case PDO_TYPE_FIXED:
      {
        uint32_t mv = ((pdo >> 10) & 0x3FFu) * 50u; /* bits[19:10] */
        if (min_mv != NULL) { *min_mv = mv; }
        if (max_mv != NULL) { *max_mv = mv; }
        if (ma    != NULL) { *ma    = ((pdo >> 0) & 0x3FFu) * 10u; }   /* bits[9:0]  */
      }
      break;
    case PDO_TYPE_BATTERY:
      if (min_mv != NULL) { *min_mv = ((pdo >> 10) & 0x3FFu) * 50u; } /* bits[19:10] */
      if (max_mv != NULL) { *max_mv = ((pdo >> 20) & 0x3FFu) * 50u; } /* bits[29:20] */
      if (mwp   != NULL) { *mwp   = ((pdo >> 0) & 0x3FFu) * 250u; }   /* bits[9:0]  */
      break;
    case PDO_TYPE_VARIABLE:
      if (min_mv != NULL) { *min_mv = ((pdo >> 10) & 0x3FFu) * 50u; } /* bits[19:10] */
      if (max_mv != NULL) { *max_mv = ((pdo >> 20) & 0x3FFu) * 50u; } /* bits[29:20] */
      if (ma    != NULL) { *ma    = ((pdo >> 0) & 0x3FFu) * 10u; }   /* bits[9:0]  */
      break;
    case PDO_TYPE_APDO:
      switch (APIE_Decode_ApdoType(pdo))
      {
        case APDO_TYPE_PPS:
          if (min_mv != NULL) { *min_mv = ((pdo >> 8) & 0xFFu) * 100u; }  /* bits[15:8] */
          if (max_mv != NULL) { *max_mv = ((pdo >> 17) & 0xFFu) * 100u; } /* bits[24:17] */
          if (ma    != NULL) { *ma    = ((pdo >> 0) & 0x7Fu) * 50u; }     /* bits[6:0]  */
          break;
        case APDO_TYPE_AVS:
          if (min_mv != NULL) { *min_mv = ((pdo >> 8) & 0xFFu) * 100u; }   /* bits[15:8] */
          if (max_mv != NULL) { *max_mv = ((pdo >> 17) & 0x1FFu) * 100u; } /* bits[25:17] */
          if (mwp   != NULL) { *mwp   = ((pdo >> 0) & 0xFFu); }            /* PDP in 1W */
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
}

void APIE_Decode_PDO(uint32_t pdo, uint8_t is_src, char *out, uint32_t outsz)
{
  uint32_t t;
  uint32_t min_mv = 0U, max_mv = 0U, ma = 0U, mwp = 0U;
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  t = APIE_Decode_PdoType(pdo); /* bits[31:30] */
  switch (t)
  {
    case PDO_TYPE_FIXED:
      APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
      if (is_src != 0U)
      {
        snprintf(out, outsz, "Fixed  %lu mV  %lu mA", (unsigned long)min_mv, (unsigned long)ma);
      }
      else
      {
        snprintf(out, outsz, "Fixed  %lu mV  %lu mA (sink)", (unsigned long)min_mv, (unsigned long)ma);
      }
      break;
    case PDO_TYPE_BATTERY:
      APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
      snprintf(out, outsz, "Battery  %lu-%lu mV  %lu mW",
               (unsigned long)min_mv, (unsigned long)max_mv, (unsigned long)mwp);
      break;
    case PDO_TYPE_VARIABLE:
      APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
      snprintf(out, outsz, "Variable  %lu-%lu mV  %lu mA",
               (unsigned long)min_mv, (unsigned long)max_mv, (unsigned long)ma);
      break;
    case PDO_TYPE_APDO:
      switch (APIE_Decode_ApdoType(pdo))
      {
        case APDO_TYPE_PPS:
          APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
          snprintf(out, outsz, "PPS APDO  %lu-%lu mV  max %lu mA",
                   (unsigned long)min_mv, (unsigned long)max_mv, (unsigned long)ma);
          break;
        case APDO_TYPE_AVS:
          APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
          snprintf(out, outsz, "AVS APDO  %lu-%lu mV  PDP %lu W",
                   (unsigned long)min_mv, (unsigned long)max_mv, (unsigned long)(mwp != 0U ? mwp : ((max_mv * 5U) / 1000U)));
          break;
        default:
          snprintf(out, outsz, "APDO 0x%08lX", (unsigned long)pdo);
          break;
      }
      break;
    default:
      snprintf(out, outsz, "PDO 0x%08lX", (unsigned long)pdo);
      break;
  }
}

void APIE_Decode_CableVdo(uint32_t vdo, char *out, uint32_t outsz)
{
  /* Passive/active-cable VDO (USB PD spec §6.4.4.2.2).  Best-effort decode of
     the cable current/SS fields from the LSB byte(s). */
  uint32_t cur = (vdo >> 7) & 0x03u;
  uint32_t ss  = (vdo >> 9) & 0x03u;
  if (out == NULL || outsz == 0U)
  {
    return;
  }
  snprintf(out, outsz, "Cable VDO  cur=%luA  ss=%lu",
           (unsigned long)(cur == 2u ? 5u : (cur == 1u ? 3u : 1u)),
           (unsigned long)ss);
}

const char *APIE_Decode_SOPName(uint8_t sop)
{
  switch (sop)
  {
    case 0u: return "SOP";
    case 1u: return "SOP'";
    case 2u: return "SOP''";
    case 3u: return "SOP' dbg";
    case 4u: return "SOP'' dbg";
    case 5u: return "HARD_RESET";
    case 6u: return "CABLE_RESET";
    case 7u: return "BIST_MODE2";
    default: return "INVALID";
  }
}

uint32_t APIE_Decode_PdoSignature(const uint32_t *pdo, uint8_t n)
{
  uint32_t h = 2166136261u; /* FNV-1a */
  uint8_t i;
  for (i = 0U; i < n; i++)
  {
    uint32_t v = pdo[i];
    h ^= (v & 0xFFu); h *= 16777619u;
    h ^= ((v >> 8) & 0xFFu); h *= 16777619u;
    h ^= ((v >> 16) & 0xFFu); h *= 16777619u;
    h ^= ((v >> 24) & 0xFFu); h *= 16777619u;
  }
  return h;
}
