/**
  ******************************************************************************
  * @file    apie_decode.h
  * @brief   Deterministic USB-PD message decoder (standards-backed).
  *
  * Pure/independent decode of every observable PD message field: 16-bit
  * header, PDO/APDO/EPR-Fixed/AVS data objects, control/data/vendor/extended
  * message types, SVDM/UVDM headers, cable (SOP'/SOP'') VDOs.  No dependence
  * on the ST USBPD header layout, so the decoder is also host-testable.
  *                                                         *
  ******************************************************************************
  */
#ifndef APIE_DECODE_H
#define APIE_DECODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "apie.h"

/* --- PDO type (bits[31:30], normative USB-PD layout) --------------------- */
#define APIE_PDO_TYPE_FIXED     0x00u
#define APIE_PDO_TYPE_BATTERY   0x01u
#define APIE_PDO_TYPE_VARIABLE  0x02u
#define APIE_PDO_TYPE_APDO      0x03u

/* --- APDO sub-type (bits[29:28]) ----------------------------------------- */
#define APIE_APDO_TYPE_PPS      0x00u
#define APIE_APDO_TYPE_AVS      0x01u

/* --- message classes (reuses APIE_MsgClass_t from apie.h) ----------------- */

typedef struct
{
  uint8_t  type;        /* message type (header bits 4:0)                  */
  uint8_t  port_data_role; /* 0=UFP(consumer), 1=DFP(provider)             */
  uint8_t  spec_rev;    /* 0=PD2.0, 1=PD3.0, 2=PD3.1                        */
  uint8_t  port_power_role; /* 0=SNK, 1=SRC                                */
  uint8_t  msgid;
  uint8_t  nobjects;
  uint8_t  extended;
  uint8_t  chunked;
} APIE_Header_t;

/* Decode the 16-bit PD header (little-endian wire form). */
void APIE_Decode_Header(uint16_t hdr, APIE_Header_t *out);

/* Classify a message type into a broad class. */
APIE_MsgClass_t APIE_Decode_Classify(uint8_t hdr_type, uint8_t extended);

/* Human-readable message-type name, e.g. "Get_Source_Cap", "Source_Capabilities". */
void APIE_Decode_TypeName(uint8_t hdr_type, uint8_t extended, char *out, uint32_t outsz);

/* Same, but disambiguates control vs data by the number of data objects.
   Only the data types are distinguishable this way (e.g. type 0x01 is
   GoodCRC when nobjects==0, Source_Capabilities when nobjects>0). */
void APIE_Decode_TypeNameN(uint8_t hdr_type, uint8_t extended, uint8_t nobjects,
                           char *out, uint32_t outsz);

/* Decode a single 32-bit PDO/APDO object into a short text caption.
   is_src selects the source (advertised) field layout vs sink layout. */
void APIE_Decode_PDO(uint32_t pdo, uint8_t is_src, char *out, uint32_t outsz);

/* PDO type in bits[31:30] (normative).  Returns one of
   APIE_PDO_TYPE_FIXED / _BATTERY / _VARIABLE / _APDO / 0xFF for invalid. */
uint8_t APIE_Decode_PdoType(uint32_t pdo);

/* APDO sub-type in bits[29:28].  Returns APIE_APDO_TYPE_PPS / _AVS for an
   APDO, 0xFF if pdo is not an APDO. */
uint8_t APIE_Decode_ApdoType(uint32_t pdo);

/* Parse a PDO into unit-scaled electrical values.  Pointers may be NULL.
   For Fixed  : min_mv & max_mv = voltage, *ma = max current.
   For Battery: min_mv/max_mv range, *mwp = max allowable power.
   For Variable: min_mv/max_mv range, *ma = max current.
   For PPS APDO: min_mv/max_mv range, *ma = max current.
   For AVS APDO: min_mv/max_mv range, *mwp = PDP (if signalled). */
void APIE_Decode_PdoCaps(uint32_t pdo, uint32_t *min_mv, uint32_t *max_mv,
                         uint32_t *ma, uint32_t *mwp);

/* Decode a VDM/VDO 32-bit object. */
void APIE_Decode_VDO(uint32_t vdo, char *out, uint32_t outsz);

/* Decode a VDM header (the first VDO of a Vendor_Defined message). */
void APIE_Decode_VDM_Header(uint32_t vdm_hdr, char *out, uint32_t outsz);

/* Extract the SVDM command from a VDM header (bits 15:8 for structured). */
uint8_t APIE_Decode_SvdmCommand(uint32_t vdm_hdr);

/* Determine whether a VDM header is structured (SVDM) or unstructured (UVDM). */
uint8_t APIE_Decode_VdmStructured(uint32_t vdm_hdr);

/* Decode a cable VDO from a SOP'/SOP'' Discover Identity response. */
void APIE_Decode_CableVdo(uint32_t vdo, char *out, uint32_t outsz);

/* Convert the ordered-set / SOP code to a short name. */
const char *APIE_Decode_SOPName(uint8_t sop);

/* Compute PDO signature hash (used by fingerprints). */
uint32_t APIE_Decode_PdoSignature(const uint32_t *pdo, uint8_t n);

#ifdef __cplusplus
}
#endif

#endif /* APIE_DECODE_H */
