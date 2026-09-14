#ifndef APP_PD_H
#define APP_PD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "usbpd_def.h"
#include "usbpd_dpm_user.h"

typedef struct
{
  uint32_t ListOfRcvSRCPDO[USBPD_MAX_NB_PDO];
  uint8_t  NumberOfRcvSRCPDO;
  uint32_t RequestedVoltage;   /* mV */
  uint32_t RequestedCurrent;   /* mA */
  uint32_t RDOPosition;        /* 1-based */
  uint32_t RequestDOMsg;
  uint8_t  Attached;
  uint8_t  Contract;
  uint8_t  CCx;
  uint8_t  UserSelected;       /* 1 after the user picked a PDO / PPS */
  uint8_t  PendingIndex;       /* 1-based, 0 = auto 5 V */
  uint16_t PendingVoltage;     /* mV, used for PPS / volt */
  uint16_t PendingCurrent;     /* mA */
  uint32_t LastNotify;
  uint32_t SyntheticVbusMv;    /* CC-only tester: last requested / 5 V */
  uint8_t  HaveLast;           /* 1 after at least one user request (for 'remember') */
  uint8_t  LastIndex;          /* 1-based PDO of the last user request */
  uint32_t LastMv;             /* mV of the last user request */
  uint32_t LastMa;             /* mA of the last user request */
} APP_PD_Port_t;

extern APP_PD_Port_t APP_PD_Port[USBPD_PORT_COUNT];

void APP_PD_Init(void);
void APP_PD_OnCable(uint8_t port, USBPD_CAD_EVENT ev);
void APP_PD_OnNotify(uint8_t port, USBPD_NotifyEventValue_TypeDef ev);
void APP_PD_StoreSrcPDO(uint8_t port, const uint8_t *ptr, uint32_t size);
void APP_PD_Evaluate(uint8_t port, uint32_t *rdo, USBPD_CORE_PDO_Type_TypeDef *type);
void APP_PD_Task(void);
USBPD_StatusTypeDef APP_PD_SendRequest(uint8_t port, uint8_t index, uint16_t mv, uint16_t ma);
void APP_PD_PrintCaps(void);
void APP_PD_RequestCapsPrint(void);
void APP_PD_PrintStatus(void);
void APP_PD_FormatPdo(uint32_t pdo, char *out, uint32_t outsz);
uint8_t APP_PD_IsAttached(void);
uint8_t APP_PD_HasContract(void);
uint32_t APP_PD_GetVbusMv(void);
void APP_PD_SetVbusMv(uint32_t mv);

/* --- source-detail fetch & decoding -------------------------------------- */
void APP_PD_OnDataInfo(uint8_t port, USBPD_CORE_DataInfoType_TypeDef dataId, const uint8_t *ptr, uint32_t size);
void APP_PD_OnExtendedMessage(uint8_t port, USBPD_ExtendedMsg_TypeDef type, const uint8_t *data, uint16_t size);
void APP_PD_PrintIdentity(const USBPD_DiscoveryIdentity_TypeDef *id, uint8_t ok);
void APP_PD_PrintSvids(const USBPD_SVIDInfo_TypeDef *sv, uint8_t ok);
void APP_PD_PrintModes(const USBPD_ModeInfo_TypeDef *md, uint8_t ok);

/* --- helpers -------------------------------------------------------------- */
uint8_t APP_PD_FindBestPdo(uint32_t want_mv, uint8_t *out_index, uint8_t *is_pps);

/* --- automation: auto-request / remember / sweep -------------------------- */
void APP_PD_SetAuto(uint32_t mv, uint32_t ma);
void APP_PD_GetAuto(uint32_t *mv, uint32_t *ma);
void APP_PD_SetRemember(uint8_t on);
uint8_t APP_PD_GetRemember(void);
void APP_PD_StartSweep(uint32_t from_mv, uint32_t to_mv, uint32_t step_mv, uint32_t ma);
void APP_PD_StopSweep(void);
uint8_t APP_PD_SweepActive(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_PD_H */
