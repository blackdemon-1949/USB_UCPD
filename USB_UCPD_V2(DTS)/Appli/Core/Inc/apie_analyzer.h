/**
  ******************************************************************************
  * @file    apie_analyzer.h
  * @brief   Bounded raw PD packet analyzer + transaction engine + features.
  *
  * The analyzer keeps a bounded ring of raw PD packets.  Capture is done by
  * COPYING bytes out of the ST-owned RX buffer (never re-arming or replacing
  * `Ports[0].ptr_RxBuff`), so the ST PRL DMA ownership model is intact.
  ******************************************************************************
  */
#ifndef APIE_ANALYZER_H
#define APIE_ANALYZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"
#include "apie_decode.h"

/* --- raw packet ring ----------------------------------------------------- */
void APIE_Analyzer_Init(void);
/* Minimal capture hook; buf points at the ST RX buffer just after RXMSGEND.
   The analyzer COPIES the bytes; it never owns or re-arms that buffer. */
void APIE_Analyzer_CaptureRaw(uint8_t port, uint8_t sop, const uint8_t *buf, uint32_t n);
uint16_t APIE_Analyzer_Count(void);
uint16_t APIE_Analyzer_Dropped(void);
const APIE_Packet_t *APIE_Analyzer_Get(uint16_t idx); /* oldest = 0 */
void APIE_Analyzer_Clear(void);
void APIE_Analyzer_Dump(uint8_t show_all);
/* Emit a machine-readable capture (one hex line per packet) for the host
   replay/analysis tool (tools/apie_replay.py).  Copy/format only. */
void APIE_Analyzer_Export(void);
void APIE_Analyzer_Stats(char *out, uint32_t outsz);

/* --- transaction engine -------------------------------------------------- */
void APIE_Txn_Init(void);
void APIE_Txn_ResetSession(uint8_t conn_id);
void APIE_Txn_Begin(uint8_t port, uint8_t dir, uint8_t sop, uint8_t tx_type,
                    uint8_t exp_type, uint8_t msgid, uint8_t conn_id, uint8_t attempt);
/* Correlate an incoming message type against an open expectation. */
void APIE_Txn_OnRx(uint8_t port, uint8_t sop, uint8_t msgid, uint8_t type, uint8_t conn_id);
void APIE_Txn_TimeoutAll(uint8_t conn_id, uint32_t now);
void APIE_Txn_Record(uint8_t port, uint8_t type, uint8_t result, uint8_t conn_id);
uint16_t APIE_Txn_ActiveCount(void);
uint16_t APIE_Txn_HistoryCount(void);
int APIE_Txn_Get(const APIE_Txn_t **hist); /* returns count, sets ptr */
void APIE_Txn_Dump(void);

/* --- feature extraction -------------------------------------------------- */
#define APIE_FEATURE_COUNT 12U
typedef struct
{
  float v[APIE_FEATURE_COUNT];
} APIE_FeatureVec_t;

void APIE_Feature_Init(void);
/* Build a feature vector from the last transaction + live power context. */
void APIE_Feature_Build(uint8_t conn_id, uint32_t voltage_mv, uint32_t current_ma,
                        uint32_t soc_temp_c, APIE_FeatureVec_t *out);
const char *APIE_Feature_Name(uint8_t i);

#ifdef __cplusplus
}
#endif

#endif /* APIE_ANALYZER_H */
