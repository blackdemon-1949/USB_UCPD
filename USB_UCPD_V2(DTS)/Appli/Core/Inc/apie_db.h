/**
  ******************************************************************************
  * @file    apie_db.h
  * @brief   Persistent source-knowledge store (versioned, CRC-protected).
  *
  * Profiles/capabilities/negative-capabilities/fingerprints live in RAM and
  * are batched.  External NOR persistence is deliberately DISABLED until
  * XIP-safe hardware validation proves it (the Boot maps the same part at
  * 0x90000000; writing while executing from it is unsafe).  The format is
  * versioned + CRC-32 so a stale/bad record is detected and dropped.
  ******************************************************************************
  */
#ifndef APIE_DB_H
#define APIE_DB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "apie.h"

#define APIE_DB_MAGIC    0x41445042u /* 'ADPB' */
#define APIE_DB_VERSION  1u

void APIE_Db_Init(void);
void APIE_Db_Reset(void);
/* Store the current source profile (deduplicated by signature). */
int APIE_Db_StoreProfile(const APIE_Profile_t *p);
/* Find a stored profile by signature/identity; returns index or -1. */
int APIE_Db_Find(const APIE_Profile_t *p);
uint16_t APIE_Db_Count(void);
const APIE_DbProfile_t *APIE_Db_Get(uint16_t idx);
uint8_t APIE_Db_Validate(uint16_t idx);
uint8_t APIE_Db_ValidateAll(void);
void APIE_Db_Dump(void);
void APIE_Db_Status(char *out, uint32_t outsz);

/* Compact serialized form of one DB record (used for host export). */
uint16_t APIE_Db_Export(uint8_t *out, uint16_t outsz);
uint8_t APIE_Db_Import(const uint8_t *in, uint16_t len);

/* ---------------------------------------------------------------------------
 * Flash-endurance / checkpoint counters.
 *
 * The APIE knowledge store is RAM-resident (external NOR persistence is
 * DISABLED for XIP safety, see FLASH_ENDURANCE.md).  These counters therefore
 * report the *logical* checkpoint/persist accounting and the number of physical
 * flash program/erase operations actually performed (always 0 while NOR
 * persistence is disabled).  They are real counters with a defined meaning, so
 * `db wear/writes/erases/checkpoint` are honest rather than fabricated.
 * ------------------------------------------------------------------------- */
typedef struct
{
  uint32_t checkpoints;   /* logical checkpoints taken since boot        */
  uint32_t writes;        /* DB store operations (RAM records)           */
  uint32_t erases;        /* physical flash erases performed (0 while off)*/
  uint32_t wear;          /* physical flash program/erase wear (0 while off)*/
  uint32_t compacts;      /* compaction runs                             */
  uint32_t self_tests;    /* DB self-test runs                           */
  uint8_t  nor_persist;   /* 1 if a NOR/XIP persistence backend is active */
} APIE_DbCounters_t;

/* Bump the store counter (call on every StoreProfile). */
void APIE_Db_CountStore(void);
/* Record a logical checkpoint (no physical write while NOR persist is off). */
void APIE_Db_Checkpoint(void);
/* Compact/re-index the store (dedupe by identity/signature). Returns new count. */
uint16_t APIE_Db_Compact(void);
/* Self-test: store a scratch profile, validate its CRC, drop it. 1 = OK. */
uint8_t APIE_Db_SelfTest(void);
/* Fill *c with the current counters. */
void APIE_Db_GetCounters(APIE_DbCounters_t *c);

#ifdef __cplusplus
}
#endif

#endif /* APIE_DB_H */
