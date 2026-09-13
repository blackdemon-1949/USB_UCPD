/**
  ******************************************************************************
  * @file    apie_bkp.c
  * @brief   APIE persistence backend on the STM32H7R3 Backup SRAM (4 KB).
  *
  * Hardware enable sequence (RM0477, STM32H7R3/S3 backup domain, verified
  * against the H7RS HAL sources and the ST community thread
  * "Writing to Backup SRAM on STM32H7S78-DK"):
  *
  *   1. HAL_PWR_EnableBkUpAccess()     - PWR_CR1.DBP = 1.  On this family
  *      DBP gates BKPSRAM *data* writes (writing with DBP clear hard-faults
  *      on H7S78).  It is set once here and left set for the whole session,
  *      exactly the way ext_dts.c leaves it for the LSE.
  *   2. __HAL_RCC_BKPRAM_CLK_ENABLE()  - RCC_AHB4ENR.BKPRAMEN (bit 28).
  *      Without it BKPSRAM reads return nothing useful.
  *   3. HAL_PWREx_EnableBkUpReg()      - PWR_CSR1.BREN, bounded wait for
  *      PWR_CSR1.BRRDY inside the HAL.  Required for the content to survive
  *      Standby / VDD-off-with-VBAT.  Failure here is NOT fatal: the image
  *      then survives resets only, and APIE_Bkp_RetentionOk() reports 0 so
  *      the CLI can say so instead of implying a guarantee that is not
  *      there.
  *
  * Cache coherency: main.c maps a dedicated non-cacheable MPU region over
  * 0x38800000 (4 KB), mirroring the pattern of the USB DMA window at
  * 0x2406E000.  Without it the ARMv7-M default memory map (this address
  * falls through region 0's disabled subregion 1 to the default map) makes
  * BKPSRAM Normal/WB/WA cacheable, and a checkpoint write can sit in dirty
  * D-cache lines that never reach the VBAT-retained SRAM before a power
  * loss - silently losing "permanent" data.  A __DSB() after every write is
  * kept as belt-and-suspenders anyway.
  *
  * Puya PY25Q64HA NOR is NOT touched here in any way.  NOR persistence
  * stays disabled for XIP safety (FLASH_ENDURANCE.md) - this backend is a
  * different memory on a different bus, which is the whole point.
  ******************************************************************************
  */
#include "apie_bkp.h"
#include "apie_db.h"
#include "apie_ml.h"
#include "app_profile.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>

/* Compile-time geometry guards: the blob layout is raw struct bytes, so any
 * change to these structs must bump APIE_BKP_SCHEMA (and this fires first). */
_Static_assert(sizeof(APIE_BkpHeader_t) == APIE_BKP_HDR_LEN,
               "APIE_BkpHeader_t must stay 32 bytes - bump APIE_BKP_SCHEMA");
#if APIE_BKP_HW_ENABLED
#include "main.h"   /* HAL / CMSIS register access (target build)          */
#define APIE_BKP_DSB()   __DSB()
#else
#define APIE_BKP_DSB()   ((void)0)   /* host build: no barrier to perform  */
/* Host builds do not pull in main.h/CMSIS; the host self-test provides the
   tick (same pattern as apie_selftest.c). */
extern uint32_t HAL_GetTick(void);
#endif
/* How often the aggregated model checkpoint may fire (ms) when the model
 * actually changed.  Never per-packet / per-loop (FLASH_ENDURANCE.md
 * checkpoint discipline; BKPSRAM has no wear but the discipline stays). */
#define APIE_BKP_MODEL_PERIOD_MS  60000U
/* Retry interval for a backup regulator that was not ready at boot. */
#define APIE_BKP_BREG_RETRY_MS    1000U

/* ------------------------------------------------------------------------ */
/*  Backing store                                                            */
/* ------------------------------------------------------------------------ */
#if APIE_BKP_HW_ENABLED

#define APIE_BKP_MEM ((volatile uint32_t *)0x38800000UL)  /* BKPSRAM_BASE   */
#define APIE_BKP_MEM_WORDS (4096u / 4u)                   /* BKPSRAM_SIZE   */

static uint8_t bkp_hw_enable(void)
{
  /* 1. backup-domain write access (PWR is always clocked on the H7RS - see
   *    the note in ext_dts.c; there is no __HAL_RCC_PWR_CLK_ENABLE here). */
  HAL_PWR_EnableBkUpAccess();

  /* 2. BKPSRAM interface clock. */
  __HAL_RCC_BKPRAM_CLK_ENABLE();

  /* The store is usable from here on (resets are survived without the
   * regulator).  Data writes below depend on DBP staying set. */

  /* 3. backup regulator for VBAT retention.  Bounded wait inside the HAL
   *    (PWR_FLAG_SETTING_DELAY); a failure is reported, not fatal. */
  return (HAL_PWREx_EnableBkUpReg() == HAL_OK) ? 1u : 0u;
}

#else  /* host test: plain RAM stands in for the backup SRAM */

static uint32_t s_host_mem[4096u / 4u];
#define APIE_BKP_MEM      ((volatile uint32_t *)s_host_mem)
#define APIE_BKP_MEM_WORDS (4096u / 4u)

static uint8_t bkp_hw_enable(void)
{
  return 1u;   /* host build: the "regulator" is always ready */
}

#endif /* APIE_BKP_HW_ENABLED */

/* Stage buffer: allocated for the FULL backup-SRAM window minus the header.
 * The payload holds everything the user or the engine customizes (owner
 * profile list, learned source profiles, ML model), and sizing the stage
 * for the whole window means a future section never needs re-sizing. */
#define APIE_BKP_STAGE_MAX  ((APIE_BKP_MEM_WORDS * 4u) - APIE_BKP_HDR_LEN)

/* ------------------------------------------------------------------------ */
/*  Module state                                                             */
/* ------------------------------------------------------------------------ */
static uint8_t  s_hw_ok;         /* clock + DBP up: the backend is usable   */
static uint8_t  s_retention_ok;  /* backup regulator ready: VBAT retention  */
static uint8_t  s_loaded_ok;     /* valid image restored at boot            */
static uint8_t  s_last_write_ok; /* last checkpoint verified                */
static uint8_t  s_inited;
static uint32_t s_writes;
static uint32_t s_write_fails;
static uint32_t s_loads;
static uint32_t s_seq;
static uint32_t s_breg_retry_at;
static uint32_t s_model_period_at;
static uint32_t s_model_obs_last;   /* nclass totals at last model save     */

/* Serialization staging.  Plain .bss (AXI SRAM): only ever touched by the
 * CPU, then copied word-wise into the non-cacheable backup SRAM window. */
static uint8_t s_stage[APIE_BKP_STAGE_MAX] __attribute__((aligned(4)));

/* ------------------------------------------------------------------------ */
/*  Word-wise copy to/from the (volatile) backup SRAM window                 */
/* ------------------------------------------------------------------------ */
static void bkp_mem_write(const uint8_t *src, uint32_t words)
{
  uint32_t i;
  const uint32_t *s = (const uint32_t *)(const void *)src;
  for (i = 0u; i < words; i++)
  {
    APIE_BKP_MEM[i] = s[i];
  }
  /* Belt and suspenders: the region is non-cacheable so the stores have
     already reached the SRAM, but make that explicit before anyone relies
     on the image (power can be lost any instant after this returns). */
  APIE_BKP_DSB();
}

static void bkp_mem_read(uint8_t *dst, uint32_t words)
{
  uint32_t i;
  uint32_t *d = (uint32_t *)(void *)dst;
  for (i = 0u; i < words; i++)
  {
    d[i] = APIE_BKP_MEM[i];
  }
}

/* Defined in the restore/save section below; needed by
   APIE_Bkp_LoadProfiles() to stage the stored image safely. */
static uint32_t bkp_stage_image(void);

/* ------------------------------------------------------------------------ */
/*  Pure blob build / validate+import (host-testable)                        */
/* ------------------------------------------------------------------------ */
/*  Profile section: u8 count, then count x 8 bytes (u16 kind, u16 index,
 *  u16 mv, u16 ma - explicit little-endian u16 fields, never raw struct
 *  bytes, so struct padding can never leak into the CRC).  Kinds match
 *  app_profile_kind_t. */
static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t prof_export(uint8_t *out, uint32_t outsz)
{
  uint8_t count = APP_PROFILE_Count();
  uint32_t need = 1u + (8u * (uint32_t)count);
  uint8_t i;

  if (outsz < need)
  {
    return 0u;
  }
  out[0] = count;
  for (i = 0u; i < count; i++)
  {
    const app_profile_entry_t *e = APP_PROFILE_Get(i);
    if (e == NULL)
    {
      return 0u;   /* count/list desync: refuse to write a torn image */
    }
    put16(&out[1u + (8u * (uint32_t)i) + 0u], (uint16_t)e->kind);
    put16(&out[1u + (8u * (uint32_t)i) + 2u], (uint16_t)e->index);
    put16(&out[1u + (8u * (uint32_t)i) + 4u], e->mv);
    put16(&out[1u + (8u * (uint32_t)i) + 6u], e->ma);
  }
  return need;
}

static uint8_t prof_import(const uint8_t *in, uint32_t len)
{
  uint8_t count;
  uint8_t i;

  if (len < 1u)
  {
    return 0u;
  }
  count = in[0];
  if ((len != (1u + (8u * (uint32_t)count))) || (count > APP_PROFILE_MAX))
  {
    return 0u;
  }
  /* Validate every entry's kind first so a corrupt entry cannot leave a
     half-replaced list behind. */
  for (i = 0u; i < count; i++)
  {
    uint16_t kind = get16(&in[1u + (8u * (uint32_t)i) + 0u]);
    if ((kind != (uint16_t)APP_PROFILE_FIXED) && (kind != (uint16_t)APP_PROFILE_PPS))
    {
      return 0u;
    }
  }
  APP_PROFILE_Clear();
  for (i = 0u; i < count; i++)
  {
    uint16_t kind  = get16(&in[1u + (8u * (uint32_t)i) + 0u]);
    uint16_t index = get16(&in[1u + (8u * (uint32_t)i) + 2u]);
    uint16_t mv    = get16(&in[1u + (8u * (uint32_t)i) + 4u]);
    uint16_t ma    = get16(&in[1u + (8u * (uint32_t)i) + 6u]);
    uint8_t ok = (kind == (uint16_t)APP_PROFILE_FIXED)
                   ? APP_PROFILE_AddFixed((uint8_t)index, ma)
                   : APP_PROFILE_AddPps(mv, ma);
    if (ok == 0u)
    {
      APP_PROFILE_Clear();
      return 0u;
    }
  }
  return 1u;
}

/* Validate a whole image (header + payload CRC).  Returns the payload
 * pointer, or NULL when the image must be discarded. */
static const uint8_t *bkp_validate(const uint8_t *buf, uint32_t size,
                                   APIE_BkpHeader_t *h)
{
  const uint8_t *payload;

  if ((buf == NULL) || (size < APIE_BKP_HDR_LEN) || (size > (APIE_BKP_MEM_WORDS * 4u)))
  {
    return NULL;
  }
  memcpy(h, buf, sizeof(*h));
  if ((h->magic != APIE_BKP_MAGIC) || (h->schema != APIE_BKP_SCHEMA) ||
      (h->hdr_len != APIE_BKP_HDR_LEN))
  {
    return NULL;
  }
  /* The image must fit inside the buffer (at restore time the whole 4 KiB
     window is passed; a smaller image than a previous one is fine - the CRC
     protects exactly payload_len bytes, stale trailing bytes from an older,
     larger image are simply ignored). */
  if ((h->payload_len < APIE_BKP_PAYLOAD_MIN) ||
      ((uint32_t)APIE_BKP_HDR_LEN + h->payload_len > size))
  {
    return NULL;
  }
  payload = &buf[APIE_BKP_HDR_LEN];
  if (h->payload_crc32 != APIE_Crc32(payload, h->payload_len))
  {
    return NULL;   /* blank / corrupted / auto-erased BKPSRAM lands here */
  }
  return payload;
}

/* Walk the section table of a validated payload.  Returns 1 and the three
 * (pointer, length) sections; 0 on any structural inconsistency. */
static uint8_t bkp_parse_sections(const uint8_t *payload, uint32_t plen,
                                  const uint8_t **prof, uint32_t *prof_len,
                                  const uint8_t **db, uint32_t *db_len,
                                  const uint8_t **ml, uint32_t *ml_len)
{
  uint32_t off = 0u;

  *prof = NULL; *prof_len = 0u;
  *db   = NULL; *db_len   = 0u;
  *ml   = NULL; *ml_len   = 0u;

  if ((payload == NULL) || (plen < 6u))
  {
    return 0u;
  }
  *prof_len = get16(&payload[off]); off += 2u;
  if ((off + *prof_len) > plen) { return 0u; }
  *prof = &payload[off]; off += *prof_len;

  *db_len = get16(&payload[off]); off += 2u;
  if ((off + *db_len) > plen) { return 0u; }
  *db = &payload[off]; off += *db_len;

  *ml_len = get16(&payload[off]); off += 2u;
  if ((off + *ml_len) != plen) { return 0u; }   /* no trailing garbage */
  *ml = &payload[off];

  return 1u;
}

uint32_t APIE_Bkp_BuildBlob(uint8_t *buf, uint32_t size)
{
  uint32_t total;
  uint32_t off;
  uint32_t prof_len;
  uint16_t db_len;
  uint16_t ml_len;
  APIE_BkpHeader_t h;
  uint8_t *payload;

  if ((buf == NULL) || (size < APIE_BKP_HDR_LEN) ||
      (size > (APIE_BKP_MEM_WORDS * 4u)))
  {
    return 0u;
  }
  payload = &buf[APIE_BKP_HDR_LEN];

  /* Section 1: the owner's PC13 profile list. */
  off = 0u;
  prof_len = prof_export(&payload[2u], (uint32_t)(size - APIE_BKP_HDR_LEN - 2u));
  if (prof_len == 0u && APP_PROFILE_Count() != 0u)
  {
    return 0u;   /* does not fit - caller must not truncate silently */
  }
  put16(&payload[off], (uint16_t)prof_len); off += 2u; off += prof_len;

  /* Section 2: the learned source profiles (APIE_Db_Export writes its own
     versioned, per-record CRC-32 envelope - apie_db.c owns that format). */
  db_len = APIE_Db_Export(&payload[off + 2u],
                          (uint16_t)(size - APIE_BKP_HDR_LEN - off - 2u));
  if ((db_len == 0u) && (APIE_Db_Count() != 0u))
  {
    return 0u;
  }
  put16(&payload[off], db_len); off += 2u; off += db_len;

  /* Section 3: the online ML model. */
  ml_len = APIE_Ml_Export(&payload[off + 2u],
                          (uint16_t)(size - APIE_BKP_HDR_LEN - off - 2u));
  if (ml_len == 0u)
  {
    return 0u;
  }
  put16(&payload[off], ml_len); off += 2u; off += ml_len;

  total = APIE_BKP_HDR_LEN + off;
  if (total > size)
  {
    return 0u;
  }

  memset(&h, 0, sizeof(h));
  h.magic = APIE_BKP_MAGIC;
  h.schema = (uint16_t)APIE_BKP_SCHEMA;
  h.hdr_len = (uint16_t)APIE_BKP_HDR_LEN;
  h.payload_len = off;
  h.payload_crc32 = APIE_Crc32(payload, h.payload_len);
  h.seq = s_seq + 1u;
  memcpy(buf, &h, sizeof(h));
  return total;
}

uint8_t APIE_Bkp_LoadBlob(const uint8_t *buf, uint32_t size)
{
  APIE_BkpHeader_t h;
  const uint8_t *payload;
  const uint8_t *prof, *db, *ml;
  uint32_t prof_len, db_len, ml_len;

  payload = bkp_validate(buf, size, &h);
  if (payload == NULL)
  {
    return 0u;
  }
  if (bkp_parse_sections(payload, h.payload_len, &prof, &prof_len,
                         &db, &db_len, &ml, &ml_len) == 0u)
  {
    return 0u;
  }

  /* Import profiles first: on failure nothing has been touched yet.  All
     three importers validate their own content (profile kinds, Db per-record
     CRCs, Ml metadata). */
  if (prof_import(prof, prof_len) == 0u)
  {
    return 0u;
  }
  if ((db_len != 0u) && (APIE_Db_Import(db, (uint16_t)db_len) == 0u))
  {
    return 0u;
  }
  if (APIE_Ml_Import(ml, (uint16_t)ml_len) == 0u)
  {
    return 0u;
  }
  s_seq = h.seq;
  return 1u;
}

uint8_t APIE_Bkp_LoadProfiles(void)
{
  APIE_BkpHeader_t h;
  const uint8_t *payload;
  const uint8_t *prof, *db, *ml;
  uint32_t prof_len, db_len, ml_len;
  uint32_t size = bkp_stage_image();

  /* Explicit 'profile load': re-read the stored image and re-apply only the
     profile section (the DB/model stay as they are). */
  if (size == 0u)
  {
    return 0u;
  }
  payload = bkp_validate(s_stage, size, &h);
  if (payload == NULL)
  {
    return 0u;
  }
  if (bkp_parse_sections(payload, h.payload_len, &prof, &prof_len,
                         &db, &db_len, &ml, &ml_len) == 0u)
  {
    return 0u;
  }
  return prof_import(prof, prof_len);
}

/* ------------------------------------------------------------------------ */
/*  Restore / save against the real (or host) backing store                  */
/* ------------------------------------------------------------------------ */
/* Stage-1 check: is there a plausible image header at the start of the
   backing store at all?  Bounded to the header, so a garbage payload_len
   can never drive an oversized read. */
static uint8_t bkp_header_plausible(const uint8_t *hdr)
{
  APIE_BkpHeader_t h;

  if (hdr == NULL)
  {
    return 0u;
  }
  memcpy(&h, hdr, sizeof(h));
  if ((h.magic != APIE_BKP_MAGIC) || (h.schema != APIE_BKP_SCHEMA) ||
      (h.hdr_len != APIE_BKP_HDR_LEN))
  {
    return 0u;
  }
  if ((h.payload_len < APIE_BKP_PAYLOAD_MIN) ||
      ((uint32_t)APIE_BKP_HDR_LEN + h.payload_len > (APIE_BKP_MEM_WORDS * 4u)))
  {
    return 0u;
  }
  return 1u;
}

/* Read a plausible image from the backing store into the staging buffer,
   header first and then only as many words as the header says the image
   occupies.  Returns the image size in bytes (header + payload), or 0 when
   no plausible image is present. */
static uint32_t bkp_stage_image(void)
{
  APIE_BkpHeader_t h;
  uint32_t words;

  bkp_mem_read(s_stage, APIE_BKP_HDR_LEN / 4u);
  if (bkp_header_plausible(s_stage) == 0u)
  {
    return 0u;
  }
  memcpy(&h, s_stage, sizeof(h));
  words = (APIE_BKP_HDR_LEN + h.payload_len + 3u) / 4u;
  if (words > APIE_BKP_MEM_WORDS)
  {
    return 0u;   /* cannot happen after the range check; kept for safety */
  }
  bkp_mem_read(s_stage, words);
  return APIE_BKP_HDR_LEN + h.payload_len;
}

static void bkp_restore(void)
{
  uint32_t size = bkp_stage_image();

  /* A blank, never-written, auto-erased (tamper / backup-domain reset) or
     corrupted BKPSRAM must never be parsed as data - reject it and continue
     with the clean empty store that APIE_Db_Init()/APIE_Ml_Init()/
     APP_PROFILE_Init() left. */
  if ((size != 0u) && (APIE_Bkp_LoadBlob(s_stage, size) != 0u))
  {
    s_loaded_ok = 1u;
    s_loads++;
    s_last_write_ok = 1u;   /* the verified image on chip is good */
  }
}

void APIE_Bkp_Init(void)
{
  s_hw_ok = 0u;
  s_retention_ok = 0u;
  s_breg_retry_at = 0u;
  s_model_period_at = 0u;
  s_model_obs_last = 0u;

  s_retention_ok = bkp_hw_enable();
  s_hw_ok = 1u;   /* bkp_hw_enable() sets DBP + BKPRAMEN before anything else */

  bkp_restore();

  if (s_loaded_ok != 0u)
  {
    APP_LOG_Printf("bkp: BKPSRAM ready%s, %u owner profile(s) + %u learned "
                   "profile(s) + model restored (seq %lu, %lu write(s))\r\n",
                   (s_retention_ok != 0u) ? ", VBAT-retained" :
                                            " (reset-only, breg not ready)",
                   (unsigned)APP_PROFILE_Count(), (unsigned)APIE_Db_Count(),
                   (unsigned long)s_seq, (unsigned long)s_writes);
  }
  else
  {
    APP_LOG_Write("bkp: BKPSRAM ready, no valid image (first boot or "
                  "blank/corrupt - starting empty)\r\n");
  }
  s_inited = 1u;
}

uint8_t APIE_Bkp_Save(const char *reason)
{
  uint32_t total;
  uint32_t words;
  APIE_BkpHeader_t h;

  (void)reason;   /* diagnostics only */
  if (s_inited == 0u || s_hw_ok == 0u)
  {
    return 0u;
  }

  total = APIE_Bkp_BuildBlob(s_stage, (uint32_t)sizeof(s_stage));
  if (total == 0u)
  {
    s_write_fails++;
    s_last_write_ok = 0u;
    return 0u;
  }

  words = (total + 3u) / 4u;
  bkp_mem_write(s_stage, words);

  /* Read back and CRC-verify: prove the image actually reached the backup
     SRAM, not just the CPU's store buffer. */
  bkp_mem_read(s_stage, words);
  memcpy(&h, s_stage, sizeof(h));
  if ((h.magic != APIE_BKP_MAGIC) ||
      (h.payload_crc32 != APIE_Crc32(&s_stage[APIE_BKP_HDR_LEN], h.payload_len)))
  {
    s_write_fails++;
    s_last_write_ok = 0u;
    return 0u;
  }

  s_writes++;
  s_seq = h.seq;
  s_last_write_ok = 1u;
  /* A verified physical checkpoint is also a logical one: the counter in
     apie_db.c then reports exactly the writes that really happened. */
  APIE_Db_Checkpoint();
  return 1u;
}

void APIE_Bkp_Task(void)
{
  uint32_t now = HAL_GetTick();
  const APIE_MlModel_t *m;

  if (s_inited == 0u || s_hw_ok == 0u)
  {
    return;
  }

#if APIE_BKP_HW_ENABLED
  /* Retry a backup regulator that was not ready at boot (about 1 Hz, same
     pattern as the LSE retry in ext_dts.c).  Once it comes up, the image is
     VBAT-retained from the next checkpoint on. */
  if ((s_retention_ok == 0u) && ((int32_t)(now - s_breg_retry_at) >= 0))
  {
    s_breg_retry_at = now + APIE_BKP_BREG_RETRY_MS;
    if (HAL_PWREx_EnableBkUpReg() == HAL_OK)
    {
      s_retention_ok = 1u;
      APP_LOG_Write("bkp: backup regulator ready - image is now VBAT-retained\r\n");
    }
  }
#endif

  /* Aggregated model checkpoint: only when the online model actually moved
     (new query outcomes), at most once per APIE_BKP_MODEL_PERIOD_MS. */
  m = APIE_Ml_GetModel();
  if (m != NULL)
  {
    uint32_t obs = m->nclass[0u] + m->nclass[1u];
    if ((obs != s_model_obs_last) &&
        (s_model_period_at == 0u || ((int32_t)(now - s_model_period_at) >= 0)))
    {
      s_model_period_at = now + APIE_BKP_MODEL_PERIOD_MS;
      s_model_obs_last = obs;
      (void)APIE_Bkp_Save("model");
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  Observable state                                                         */
/* ------------------------------------------------------------------------ */
uint8_t  APIE_Bkp_HwOk(void)        { return s_hw_ok; }
uint8_t  APIE_Bkp_RetentionOk(void) { return s_retention_ok; }
uint8_t  APIE_Bkp_LoadedOk(void)    { return s_loaded_ok; }
uint8_t  APIE_Bkp_LastWriteOk(void) { return s_last_write_ok; }
uint32_t APIE_Bkp_Writes(void)      { return s_writes; }
uint32_t APIE_Bkp_WriteFails(void)  { return s_write_fails; }
uint32_t APIE_Bkp_Loads(void)       { return s_loads; }
uint32_t APIE_Bkp_Seq(void)         { return s_seq; }

void APIE_Bkp_Status(char *out, uint32_t outsz)
{
  if (out == NULL || outsz == 0u)
  {
    return;
  }
  if (s_hw_ok == 0u)
  {
    snprintf(out, outsz, "bkp: gate FAILED - store is RAM-only");
    return;
  }
  snprintf(out, outsz,
           "bkp: BKPSRAM ready, %s, %s, %lu write(s) (%lu failed), seq %lu",
           (s_retention_ok != 0u) ? "VBAT-retained" : "reset-only (breg not ready)",
           (s_loaded_ok != 0u) ? "image restored at boot" : "no valid image at boot",
           (unsigned long)s_writes, (unsigned long)s_write_fails,
           (unsigned long)s_seq);
}

#if !APIE_BKP_HW_ENABLED
/* Host-test helper: re-run only the restore path against the host backing
   store (hardware enables are meaningless on the host). */
void APIE_Bkp_TestRestore(void)
{
  bkp_restore();
}
#endif
