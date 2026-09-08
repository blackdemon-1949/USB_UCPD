/**
  ******************************************************************************
  * @file    apie_decode_selftest.c
  * @brief   Host-side verification of the firmware PD decoder.
  *
  * This builds the actual firmware decoder (apie_decode.c) on the host with the
  * host gcc and checks known wire values against the normative USB PD layout
  * (PDO type in bits[31:30], APDO sub-type in bits[29:28]).  Run via
  * tools/apie_selftest.sh.
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include "apie_decode.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
  checks++; \
  if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
  APIE_Header_t h;
  char buf[64];

  /* Header: Get_Status control (type=0x12), SNK, PD3.0, msgid=2, 0 objects.
     byte0=0x12, byte1 = (msgid2<<10)|(specrev1<<7) = 0x880. */
  {
    uint16_t hdr = (uint16_t)(0x0012); /* type=0x12, specrev=0, msgid=0, nobj=0 */
    APIE_Decode_Header(hdr, &h);
    CHECK(h.type == 0x12, "Get_Status type");
    CHECK(h.spec_rev == 0, "spec_rev PD2.0 (default)");
    CHECK(h.msgid == 0, "msgid 0");
    CHECK(h.nobjects == 0, "nobjects 0");
    CHECK(h.extended == 0, "not extended");
  }
  {
    uint16_t hdr = (uint16_t)(0x0892); /* msgid=2 (0x800), specrev=1 (0x80) */
    APIE_Decode_Header(hdr, &h);
    CHECK(h.msgid == 2, "msgid 2");
    CHECK(h.spec_rev == 1, "spec_rev PD3.0");
  }
  /* Source_Capabilities: type=0x01, 5 objects, msgid=0, specrev=1 */
  {
    uint16_t hdr = (uint16_t)(0x5041); /* byte0=0x41(type1), byte1: nobj5(0x5000)|specrev1(0x80)=0x5080 */
    APIE_Decode_Header(hdr, &h);
    CHECK(h.type == 0x01, "Source_Cap type");
    CHECK(h.nobjects == 5, "nobjects 5");
  }

  /* Type names */
  APIE_Decode_TypeName(0x12, 0, buf, sizeof(buf));
  CHECK(strcmp(buf, "Get_Status") == 0, "Get_Status name");
  APIE_Decode_TypeName(0x03, 0, buf, sizeof(buf));
  CHECK(strcmp(buf, "Accept") == 0, "Accept name");
  APIE_Decode_TypeName(0x01, 1, buf, sizeof(buf));
  CHECK(strcmp(buf, "Ext_Source_Capabilities") == 0, "Ext_Source_Cap name");
  APIE_Decode_TypeName(0x02, 0, buf, sizeof(buf));
  CHECK(strcmp(buf, "GotoMin") == 0, "GotoMin name");
  /* Type 0x0F is Data_Reset_Complete (control, 0 objects) or Vendor_Defined
     (extended data message).  Non-extended resolves to the control name. */
  APIE_Decode_TypeName(0x0F, 0, buf, sizeof(buf));
  CHECK(strcmp(buf, "Data_Reset_Complete") == 0, "type 0x0F non-ext");
  APIE_Decode_TypeName(0x0F, 1, buf, sizeof(buf));
  CHECK(strcmp(buf, "Extended_Vendor_Defined") == 0, "type 0x0F ext");

  /* Control/data disambiguation: type 0x01 is GoodCRC (nobj=0) or
     Source_Capabilities (nobj>0). */
  APIE_Decode_TypeNameN(0x01, 0, 0, buf, sizeof(buf));
  CHECK(strcmp(buf, "GoodCRC") == 0, "type 0x01 n=0 goodcrc");
  APIE_Decode_TypeNameN(0x01, 0, 5, buf, sizeof(buf));
  CHECK(strcmp(buf, "Source_Capabilities") == 0, "type 0x01 n=5 src caps");
  APIE_Decode_TypeNameN(0x0F, 0, 1, buf, sizeof(buf));
  CHECK(strcmp(buf, "Vendor_Defined") == 0, "type 0x0F n=1 vdm");

  /* PDO type detection */
  CHECK(APIE_Decode_PdoType(0x0002D12Cu) == APIE_PDO_TYPE_FIXED, "fixed type");
  CHECK(APIE_Decode_PdoType(0x4002D12Cu) == APIE_PDO_TYPE_BATTERY, "battery type");
  CHECK(APIE_Decode_PdoType(0x8002D12Cu) == APIE_PDO_TYPE_VARIABLE, "variable type");
  CHECK(APIE_Decode_PdoType(0xC0000000u) == APIE_PDO_TYPE_APDO, "apdo type");

  /* Fixed PDO 9V/3A (source): 0x0002D12C.  voltage bits[19:10]=180 -> 9V,
     current bits[9:0]=300 -> 3A. */
  {
    uint32_t pdo = 0x0002D12Cu;
    uint32_t min_mv = 0, max_mv = 0, ma = 0, mwp = 0;
    APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
    CHECK(min_mv == 9000 && max_mv == 9000, "fixed 9V");
    CHECK(ma == 3000, "fixed 3A");
    APIE_Decode_PDO(pdo, 1, buf, sizeof(buf));
    CHECK(strstr(buf, "9000") != NULL, "fixed 9V caption");
    CHECK(strstr(buf, "3000") != NULL, "fixed 3A caption");
  }

  /* PPS APDO: 3.3-21V in 100mV, max current 3A in 50mA.
     bits[31:30]=11, bits[29:28]=00 (PPS), bits[24:17]=210 (max), bits[15:8]=33 (min),
     bits[6:0]=60 (3A/50mA). */
  {
    uint32_t pdo = 0xC0000000u | (0u << 28) | (210u << 17) | (33u << 8) | 60u;
    uint32_t min_mv = 0, max_mv = 0, ma = 0, mwp = 0;
    CHECK(APIE_Decode_ApdoType(pdo) == APIE_APDO_TYPE_PPS, "pps subtype");
    APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
    CHECK(min_mv == 3300, "pps min 3.3V");
    CHECK(max_mv == 21000, "pps max 21V");
    CHECK(ma == 3000, "pps 3A");
    APIE_Decode_PDO(pdo, 1, buf, sizeof(buf));
    CHECK(strstr(buf, "PPS") != NULL, "pps caption");
    CHECK(strstr(buf, "3300") != NULL, "pps min caption");
    CHECK(strstr(buf, "21000") != NULL, "pps max caption");
  }

  /* AVS APDO (source): 15-48V in 100mV, PDP 100W.
     bits[31:30]=11, bits[29:28]=01 (AVS), bits[25:17]=480 (max), bits[15:8]=150 (min),
     bits[7:0]=100 (PDP 1W). */
  {
    uint32_t pdo = 0xC0000000u | (1u << 28) | (480u << 17) | (150u << 8) | 100u;
    uint32_t min_mv = 0, max_mv = 0, ma = 0, mwp = 0;
    CHECK(APIE_Decode_ApdoType(pdo) == APIE_APDO_TYPE_AVS, "avs subtype");
    APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
    CHECK(min_mv == 15000, "avs min 15V");
    CHECK(max_mv == 48000, "avs max 48V");
    APIE_Decode_PDO(pdo, 1, buf, sizeof(buf));
    CHECK(strstr(buf, "AVS") != NULL, "avs caption");
    CHECK(strstr(buf, "15000") != NULL, "avs min caption");
    CHECK(strstr(buf, "48000") != NULL, "avs max caption");
  }

  /* EPR Fixed 48V source PDO (type bits[31:30]=00, but bit[23] EPR_Capable). */
  {
    uint32_t pdo = 0x00000000u | (960u << 10) | (50u << 0) | (1u << 23); /* 48V/0.5A EPR */
    uint32_t min_mv = 0, max_mv = 0, ma = 0, mwp = 0;
    CHECK(APIE_Decode_PdoType(pdo) == APIE_PDO_TYPE_FIXED, "epr fixed type");
    APIE_Decode_PdoCaps(pdo, &min_mv, &max_mv, &ma, &mwp);
    CHECK(max_mv == 48000, "epr fixed 48V");
  }

  /* SVDM command extraction. Discover_Identity (structured, PD SID):
     structVdm type bit15=1, version bits[14:13]=01, command bits[4:0]=0x01,
     SVID bits[31:16]=0xFF00 (PD SID). */
  {
    uint32_t vdm = (uint32_t)((0xFF00u << 16) | (1u << 15) | (1u << 13) | 0x01u);
    CHECK(APIE_Decode_VdmStructured(vdm) == 1, "vdm structured");
    CHECK(APIE_Decode_SvdmCommand(vdm) == 0x01, "vdm discover identity cmd");
    APIE_Decode_VDM_Header(vdm, buf, sizeof(buf));
    CHECK(strstr(buf, "Discover_Identity") != NULL, "vdm discover identity name");
  }
  /* UVDM: VDM type bit15 = 0. */
  {
    uint32_t vdm = (uint32_t)((0x1234u << 16) | (0u << 15) | 0x2Cu);
    CHECK(APIE_Decode_VdmStructured(vdm) == 0, "uvdm unstructured");
    APIE_Decode_VDM_Header(vdm, buf, sizeof(buf));
    CHECK(strcmp(buf, "UVDM v0 cmd=0x0C svid=0x1234") == 0, "uvdm header caption");
  }

  /* SOP names */
  CHECK(strcmp(APIE_Decode_SOPName(0), "SOP") == 0, "sop name");
  CHECK(strcmp(APIE_Decode_SOPName(1), "SOP'") == 0, "sop' name");
  CHECK(strcmp(APIE_Decode_SOPName(2), "SOP''") == 0, "sop'' name");
  CHECK(strcmp(APIE_Decode_SOPName(5), "HARD_RESET") == 0, "hard reset name");

  /* PDO signature (FNV) is deterministic. */
  {
    const uint32_t p[2] = { 0x0002D12Cu, 0x8002D12Cu };
    CHECK(APIE_Decode_PdoSignature(p, 2) == APIE_Decode_PdoSignature(p, 2), "pdo sig deterministic");
  }

  /* ---------------------------------------------------------------------
   * PB722 regression vectors (OBSERVED).  VID 0x2DC0 / PID 0x020B source
   * offering 5/9/12/15/20 V fixed + 3.3-21 V PPS.  Each raw header must decode
   * to the correct message type so the transaction engine classifies the flow
   * correctly.  These are real packet examples, not fabricated protocol rules.
   * --------------------------------------------------------------------- */
  {
    /* Source_Capabilities: 6 PDOs, msgid=0, specrev=1 -> 0x6081 */
    const uint32_t caps[6] = {
      0x0001912Cu, /* 5 V  / 3 A fixed */
      0x0002D12Cu, /* 9 V  / 3 A fixed */
      0x0003C12Cu, /* 12 V / 3 A fixed */
      0x0004B12Cu, /* 15 V / 3 A fixed */
      0x0006412Cu, /* 20 V / 3 A fixed */
      0xC1A4213Cu, /* PPS 3.3-21 V / 3 A */
    };
    uint32_t min_mv = 0, max_mv = 0, ma = 0, mwp = 0;
    APIE_Decode_Header(0x6081u, &h);
    CHECK(h.type == 0x01 && h.nobjects == 6, "pb722 src_cap header");
    APIE_Decode_TypeNameN(0x01, 0, 6, buf, sizeof(buf));
    CHECK(strcmp(buf, "Source_Capabilities") == 0, "pb722 src_cap name");
    APIE_Decode_PdoCaps(caps[0], &min_mv, &max_mv, &ma, &mwp);
    CHECK(min_mv == 5000 && ma == 3000, "pb722 5V/3A");
    APIE_Decode_PdoCaps(caps[4], &min_mv, &max_mv, &ma, &mwp);
    CHECK(max_mv == 20000, "pb722 20V");
    APIE_Decode_PdoCaps(caps[5], &min_mv, &max_mv, &ma, &mwp);
    CHECK(APIE_Decode_ApdoType(caps[5]) == APIE_APDO_TYPE_PPS &&
          min_mv == 3300 && max_mv == 21000 && ma == 3000, "pb722 pps 3.3-21V/3A");
    CHECK(APIE_Decode_PdoSignature(caps, 6) == APIE_Decode_PdoSignature(caps, 6),
          "pb722 caps signature deterministic");

    /* Request -> Accept -> PS_RDY (explicit contract) */
    APIE_Decode_Header(0x1082u, &h);
    CHECK(h.type == 0x02 && h.nobjects == 1, "pb722 request header");
    APIE_Decode_TypeNameN(0x02, 0, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "Request") == 0, "pb722 request name");
    APIE_Decode_TypeNameN(0x03, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Accept") == 0, "pb722 accept name");
    APIE_Decode_TypeNameN(0x06, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "PS_RDY") == 0, "pb722 ps_rdy name");

    /* Get_Status -> Not_Supported (as observed for unsupported queries) */
    APIE_Decode_TypeNameN(0x12, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Get_Status") == 0, "pb722 get_status name");
    APIE_Decode_TypeNameN(0x10, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Not_Supported") == 0, "pb722 not_supported name");

    /* Get_PPS_Status -> PPS_Status (extended 0x0C) */
    APIE_Decode_TypeNameN(0x14, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Get_PPS_Status") == 0, "pb722 get_pps_status name");
    APIE_Decode_TypeNameN(0x0C, 1, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Ext_PPS_Status") == 0, "pb722 pps_status name");

    /* SVDM Discover_Identity (NAKed on PB722), PD SID 0xFF00 */
    {
      uint32_t vdm = (uint32_t)((0xFF00u << 16) | (1u << 15) | (1u << 13) | 0x01u);
      CHECK(APIE_Decode_VdmStructured(vdm) == 1, "pb722 identity structured");
      CHECK(APIE_Decode_SvdmCommand(vdm) == 0x01, "pb722 identity cmd");
    }
  }

  printf("apie_decode_selftest: %d checks, %d failures\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
