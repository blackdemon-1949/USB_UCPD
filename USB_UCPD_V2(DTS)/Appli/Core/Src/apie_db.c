/**
  ******************************************************************************
  * @file    apie_db.c
  * @brief   Source-knowledge store (RAM), versioned + CRC-32.
  ******************************************************************************
  */
#include "apie_db.h"
#include "app_log.h"
#include <stdio.h>

static APIE_DbProfile_t s_db[APIE_DB_PROFILES];
static uint16_t s_db_count;

/* Logical flash-endurance / checkpoint accounting (NOR persist disabled). */
static APIE_DbCounters_t s_cnt;
#define APIE_NOR_PERSIST_ACTIVE 0U   /* RAM backend; see FLASH_ENDURANCE.md */

uint32_t APIE_Crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t i, j;
  for (i = 0U; i < len; i++)
  {
    crc ^= (uint32_t)data[i];
    for (j = 0U; j < 8U; j++)
    {
      uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

void APIE_Db_Init(void)
{
  memset(s_db, 0, sizeof(s_db));
  s_db_count = 0U;
  memset(&s_cnt, 0, sizeof(s_cnt));
  s_cnt.nor_persist = APIE_NOR_PERSIST_ACTIVE;
}

void APIE_Db_Reset(void)
{
  APIE_Db_Init();
}

int APIE_Db_Find(const APIE_Profile_t *p)
{
  uint16_t i;
  if (p == NULL) { return -1; }
  for (i = 0U; i < s_db_count; i++)
  {
    APIE_Profile_t const *q = &s_db[i].profile;
    if (p->has_hard != 0U && q->has_hard != 0U &&
        p->vid == q->vid && p->pid == q->pid)
    {
      return (int)i;
    }
    if (p->n_pdo == q->n_pdo && p->n_pdo > 0U &&
        memcmp(p->pdo, q->pdo, (size_t)p->n_pdo * 4U) == 0)
    {
      return (int)i;
    }
  }
  return -1;
}

int APIE_Db_StoreProfile(const APIE_Profile_t *p)
{
  int idx;
  APIE_DbProfile_t *rec;
  if (p == NULL || p->valid == 0U) { return -1; }
  idx = APIE_Db_Find(p);
  if (idx < 0)
  {
    if (s_db_count >= APIE_DB_PROFILES)
    {
      return -1;
    }
    idx = (int)s_db_count++;
  }
  rec = &s_db[idx];
  rec->magic = APIE_DB_MAGIC;
  rec->version = APIE_DB_VERSION;
  rec->profile = *p;
  rec->len = (uint16_t)sizeof(APIE_DbProfile_t) - 10u; /* excluding magic/ver/len/crc */
  rec->crc32 = APIE_Crc32((const uint8_t *)&rec->profile, sizeof(APIE_Profile_t));
  APIE_Db_CountStore();
  return idx;
}

/* --- flash-endurance / checkpoint counters ------------------------------- */
void APIE_Db_CountStore(void)
{
  s_cnt.writes++;
}

void APIE_Db_Checkpoint(void)
{
  s_cnt.checkpoints++;
  /* Physical NOR persistence is DISABLED on this board (XIP safety).  A
     logical checkpoint here is just accounting; no flash program/erase. */
}

uint16_t APIE_Db_Compact(void)
{
  uint16_t i, w = 0U;
  s_cnt.compacts++;
  /* Compact = compact the array, dropping any invalid records (dedupe). */
  for (i = 0U; i < s_db_count; i++)
  {
    if (APIE_Db_Validate(i))
    {
      if (w != i)
      {
        s_db[w] = s_db[i];
      }
      w++;
    }
  }
  s_db_count = w;
  return s_db_count;
}

uint8_t APIE_Db_SelfTest(void)
{
  APIE_Profile_t scratch;
  int idx;
  uint8_t ok;
  s_cnt.self_tests++;
  memset(&scratch, 0, sizeof(scratch));
  scratch.valid = 1U;
  scratch.has_hard = 1U;
  scratch.vid = 0xDCB0u;   /* unlikely-to-collide synthetic VID */
  scratch.pid = 0xB0DCu;
  scratch.n_pdo = 1U;
  scratch.pdo[0] = 0x2DC154u; /* 5 V / 3 A fixed PDO */
  idx = APIE_Db_StoreProfile(&scratch);
  if (idx < 0) { return 0U; }
  ok = APIE_Db_Validate((uint16_t)idx);
  /* Remove the scratch record cleanly so the store is unchanged afterwards. */
  {
    uint16_t i;
    for (i = (uint16_t)idx; (i + 1U) < s_db_count; i++)
    {
      s_db[i] = s_db[i + 1U];
    }
    if (s_db_count > 0U)
    {
      s_db_count--;
    }
  }
  return ok;
}

void APIE_Db_GetCounters(APIE_DbCounters_t *c)
{
  if (c != NULL)
  {
    *c = s_cnt;
  }
}

uint16_t APIE_Db_Count(void) { return s_db_count; }

const APIE_DbProfile_t *APIE_Db_Get(uint16_t idx)
{
  if (idx >= s_db_count) { return NULL; }
  return &s_db[idx];
}

uint8_t APIE_Db_Validate(uint16_t idx)
{
  const APIE_DbProfile_t *r;
  if (idx >= s_db_count) { return 0U; }
  r = &s_db[idx];
  if (r->magic != APIE_DB_MAGIC || r->version != APIE_DB_VERSION)
  {
    return 0U;
  }
  if (r->crc32 != APIE_Crc32((const uint8_t *)&r->profile, sizeof(APIE_Profile_t)))
  {
    return 0U;
  }
  return 1U;
}

uint8_t APIE_Db_ValidateAll(void)
{
  uint16_t i;
  for (i = 0U; i < s_db_count; i++)
  {
    if (APIE_Db_Validate(i) == 0U)
    {
      return 0U;
    }
  }
  return 1U;
}

void APIE_Db_Dump(void)
{
  uint16_t i;
  if (s_db_count == 0U)
  {
    APP_LOG_Write("db: no stored profiles\r\n");
    return;
  }
  APP_LOG_Printf("db: %u stored profile(s), crc=%s\r\n", (unsigned)s_db_count,
                 APIE_Db_ValidateAll() ? "all-valid" : "CORRUPT");
  for (i = 0U; i < s_db_count; i++)
  {
    const APIE_DbProfile_t *r = &s_db[i];
    APP_LOG_Printf("  [%02u] VID=0x%04X PID=0x%04X pdo=%u crc=0x%08lX %s\r\n",
                   (unsigned)i, (unsigned)r->profile.vid, (unsigned)r->profile.pid,
                   (unsigned)r->profile.n_pdo, (unsigned long)r->crc32,
                   APIE_Db_Validate(i) ? "ok" : "BAD");
  }
}

void APIE_Db_Status(char *out, uint32_t outsz)
{
  if (out == NULL || outsz == 0U) { return; }
  snprintf(out, outsz, "db: %u of %u profiles, crc=%s, version=%u",
           (unsigned)s_db_count, (unsigned)APIE_DB_PROFILES,
           APIE_Db_ValidateAll() ? "valid" : "corrupt", (unsigned)APIE_DB_VERSION);
}

uint16_t APIE_Db_Export(uint8_t *out, uint16_t outsz)
{
  uint32_t sz = (uint32_t)s_db_count * sizeof(APIE_DbProfile_t);
  if (out == NULL || outsz < sz) { return 0U; }
  memcpy(out, s_db, sz);
  return (uint16_t)sz;
}

uint8_t APIE_Db_Import(const uint8_t *in, uint16_t len)
{
  uint16_t n = (uint16_t)(len / sizeof(APIE_DbProfile_t));
  if (in == NULL || len == 0U || n > APIE_DB_PROFILES) { return 0U; }
  memcpy(s_db, in, (size_t)n * sizeof(APIE_DbProfile_t));
  s_db_count = n;
  return APIE_Db_ValidateAll();
}
