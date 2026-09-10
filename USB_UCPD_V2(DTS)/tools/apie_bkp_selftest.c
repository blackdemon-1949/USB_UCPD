/**
  ******************************************************************************
  * @file    apie_bkp_selftest.c
  * @brief   Host-side verification of the APIE BKPSRAM persistence backend.
  *
  * Compiles the real firmware apie_bkp.c (with APIE_BKP_HW_ENABLED=0, so a
  * plain RAM array stands in for the backup SRAM window and the register
  * path is compiled out) together with the real apie_db.c / apie_ml.c /
  * apie_stats.c, and exercises:
  *
  *   - whole-image build/restore round trip (owner profile list + learned
  *     profiles + online ML model)
  *   - the CRC gate: corrupted, bad-magic, wrong-schema, truncated and
  *     oversized images are rejected, never loaded as data
  *   - a structurally valid image whose profile section carries an invalid
  *     entry kind (CRC recomputed) is rejected by the kind validator
  *   - a smaller image over stale trailing bytes from a larger one
  *   - explicit APIE_Bkp_LoadProfiles() (the 'profile load' path)
  *   - deterministic rebuild (same state -> identical image bytes)
  *   - worst-case capacity: 8 owner profiles + 12 learned profiles + model
  *     fit inside the 4 KiB window
  *
  * What this PROVES: the blob format, the validation gate and the
  * save/restore logic are correct on the host.  What it CANNOT prove (no
  * hardware here): the real VBAT retention through a power cycle, the
  * RM0477 register enable sequence, and the non-cacheable MPU window.
  * Those are bench items - see HARDWARE_VALIDATION.md.
  *
  * Run via tools/apie_bkp_selftest.sh.
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "apie_bkp.h"
#include "apie_db.h"
#include "apie_ml.h"
#include "app_profile.h"

/* ---- firmware dependencies stubbed for the host ------------------------- */
uint32_t HAL_GetTick(void) { return 1000000UL; }

static char s_out[4096];
static uint32_t s_outlen;
void APP_LOG_Write(const char *s) { while (*s) { s_out[s_outlen++ & 4095] = *s++; } }
void APP_LOG_Printf(const char *fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  vsnprintf(s_out, sizeof(s_out), fmt, ap);
  va_end(ap);
}

/* Host stand-in for the app_profile list (the real app_profile.c pulls the
   PD stack; apie_bkp.c only uses the five functions below, and their
   semantics match app_profile.c exactly: AddFixed sets mv=0, AddPps sets
   index=0, both append and keep the position). */
static app_profile_entry_t s_prof_list[APP_PROFILE_MAX];
static uint8_t s_prof_count;
uint8_t APP_PROFILE_Count(void) { return s_prof_count; }
const app_profile_entry_t *APP_PROFILE_Get(uint8_t i)
{
  return (i < s_prof_count) ? &s_prof_list[i] : NULL;
}
uint8_t APP_PROFILE_AddFixed(uint8_t index, uint16_t ma)
{
  if ((s_prof_count >= APP_PROFILE_MAX) || (index == 0U)) { return 0U; }
  s_prof_list[s_prof_count].kind = (uint8_t)APP_PROFILE_FIXED;
  s_prof_list[s_prof_count].index = index;
  s_prof_list[s_prof_count].mv = 0U;
  s_prof_list[s_prof_count].ma = ma;
  s_prof_count++;
  return 1U;
}
uint8_t APP_PROFILE_AddPps(uint16_t mv, uint16_t ma)
{
  if (s_prof_count >= APP_PROFILE_MAX) { return 0U; }
  s_prof_list[s_prof_count].kind = (uint8_t)APP_PROFILE_PPS;
  s_prof_list[s_prof_count].index = 0U;
  s_prof_list[s_prof_count].mv = mv;
  s_prof_list[s_prof_count].ma = ma;
  s_prof_count++;
  return 1U;
}
void APP_PROFILE_Clear(void) { s_prof_count = 0U; }

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static APIE_Profile_t mkprofile(uint16_t vid, uint16_t pid, uint32_t pdo0)
{
  APIE_Profile_t p;
  memset(&p, 0, sizeof(p));
  p.valid = 1U;
  p.has_hard = 1U;
  p.vid = vid;
  p.pid = pid;
  p.n_pdo = 1U;
  p.pdo[0] = pdo0;
  return p;
}

/* Recompute the payload CRC after an in-place payload mutation, so the
   gate that is being exercised is the structural validator, not the CRC. */
static void fix_crc(uint8_t *img)
{
  APIE_BkpHeader_t h;
  memcpy(&h, img, sizeof(h));
  h.payload_crc32 = APIE_Crc32(&img[APIE_BKP_HDR_LEN], h.payload_len);
  memcpy(img, &h, sizeof(h));
}

int main(void)
{
  uint8_t img[4096];
  uint32_t n;

  /* --- 1) fresh boot: no image, backend up, store empty ------------------ */
  APP_PROFILE_Clear();
  APIE_Db_Init();
  APIE_Ml_Init();
  APIE_Bkp_Init();
  CHECK(APIE_Bkp_HwOk() != 0, "backend gate up");
  CHECK(APIE_Bkp_LoadedOk() == 0, "no image at first boot");
  CHECK(APIE_Db_Count() == 0, "store empty at first boot");
  CHECK(APP_PROFILE_Count() == 0, "profile list empty at first boot");

  /* --- 2) owner profiles + learned data, then checkpoint ----------------- */
  {
    APIE_Profile_t a = mkprofile(0x0483u, 0x5740u, 0x2DC154u);   /* 5V/3A */
    APIE_Profile_t b = mkprofile(0x18D1u, 0x4EE7u, 0x2DC19Cu);   /* 9V/3A */
    uint8_t i;
    CHECK(APP_PROFILE_AddFixed(1U, 3000U) != 0, "owner step 1 (fixed PDO 1)");
    CHECK(APP_PROFILE_AddPps(11000U, 1000U) != 0, "owner step 2 (PPS 11 V)");
    CHECK(APP_PROFILE_AddPps(3305U, 1234U) != 0, "owner step 3 (PPS, off-grid)");
    CHECK(APIE_Db_StoreProfile(&a) >= 0, "profile A stored");
    CHECK(APIE_Db_StoreProfile(&b) >= 0, "profile B stored");
    for (i = 0U; i < 12U; i++)
    {
      APIE_Ml_Observe(0U, 0U, 1U, 1U, (uint8_t)(i < 10U));
    }
    CHECK(APIE_Bkp_Save("test") != 0, "checkpoint write verified");
    CHECK(APIE_Bkp_Writes() == 1UL, "one physical write");
    CHECK(APIE_Bkp_LastWriteOk() != 0, "last write ok");
  }
  {
    APIE_DbCounters_t c;
    APIE_Db_GetCounters(&c);
    CHECK(c.checkpoints == 1UL, "checkpoint counter moved (1 verified save)");
  }

  /* --- 3) image geometry (section table) ---------------------------------- */
  n = APIE_Bkp_BuildBlob(img, sizeof(img));
  CHECK(n == (APIE_BKP_HDR_LEN + (2u + 1u + 3u * 8u) +
              (2u + (uint32_t)(2u * sizeof(APIE_DbProfile_t))) +
              (2u + (uint32_t)sizeof(APIE_MlModel_t))),
        "image size exact (profile+db+ml sections)");
  CHECK(n <= 4096u, "image fits the 4 KiB window");

  /* --- 4) simulate a reset: everything volatile is wiped, restore works --- */
  {
    uint16_t restored;
    const APIE_MlModel_t *m;
    APP_PROFILE_Clear();
    APIE_Db_Init();          /* the RAM store is gone...                  */
    APIE_Ml_Init();          /* ...and so is the model                    */
    CHECK(APIE_Db_Count() == 0, "store wiped by simulated reset");
    CHECK(APP_PROFILE_Count() == 0, "profile list wiped by simulated reset");
    APIE_Bkp_TestRestore();  /* re-run the boot-time restore path         */
    restored = APIE_Db_Count();
    CHECK(restored == 2U, "both learned profiles restored from image");
    CHECK(APIE_Db_ValidateAll() != 0, "restored records pass per-record CRC");
    m = APIE_Ml_GetModel();
    CHECK((m->nclass[0] + m->nclass[1]) == 12UL, "model observation counts restored");
    CHECK(APIE_Ml_Validate() != 0, "restored model structurally valid");
    CHECK(APP_PROFILE_Count() == 3U, "all 3 owner profile steps restored");
    {
      const app_profile_entry_t *e;
      e = APP_PROFILE_Get(0);
      CHECK((e != NULL) && (e->kind == (uint8_t)APP_PROFILE_FIXED) &&
            (e->index == 1U) && (e->ma == 3000U), "step 1 fields exact");
      e = APP_PROFILE_Get(1);
      CHECK((e != NULL) && (e->kind == (uint8_t)APP_PROFILE_PPS) &&
            (e->mv == 11000U) && (e->ma == 1000U), "step 2 fields exact");
      e = APP_PROFILE_Get(2);
      CHECK((e != NULL) && (e->mv == 3305U) && (e->ma == 1234U),
            "step 3 fields exact (off-grid values survive verbatim)");
    }
    CHECK(APIE_Bkp_LoadedOk() != 0, "loaded-ok flag set by restore");
  }

  /* --- 5) explicit LoadProfiles ('profile load') --------------------------- */
  {
    APP_PROFILE_Clear();
    CHECK(APP_PROFILE_Count() == 0U, "list cleared before LoadProfiles");
    CHECK(APIE_Bkp_LoadProfiles() != 0, "LoadProfiles re-applies the section");
    CHECK(APP_PROFILE_Count() == 3U, "LoadProfiles restored 3 steps");
    CHECK(APIE_Db_Count() == 2U, "LoadProfiles leaves the DB untouched");
  }

  /* --- 6) the CRC gate: corrupted images must be rejected ------------------ */
  {
    APIE_DbCounters_t c;

    n = APIE_Bkp_BuildBlob(img, sizeof(img));   /* fresh valid image */

    img[APIE_BKP_HDR_LEN + 5] ^= 0x01u;         /* single payload bit flip */
    APP_PROFILE_Clear(); APIE_Db_Init(); APIE_Ml_Init();
    CHECK(APIE_Bkp_LoadBlob(img, n) == 0, "single-bit corruption rejected");
    CHECK(APP_PROFILE_Count() == 0U, "corrupt image imports no profiles");
    img[APIE_BKP_HDR_LEN + 5] ^= 0x01u;         /* restore                */

    img[0] ^= 0xFFu;                            /* magic destroyed        */
    CHECK(APIE_Bkp_LoadBlob(img, n) == 0, "bad magic rejected");
    img[0] ^= 0xFFu;

    img[4] = (uint8_t)(APIE_BKP_SCHEMA + 1u);   /* schema bumped          */
    CHECK(APIE_Bkp_LoadBlob(img, n) == 0, "schema mismatch rejected");
    img[4] = (uint8_t)APIE_BKP_SCHEMA;

    CHECK(APIE_Bkp_LoadBlob(img, n - 1U) == 0, "truncated image rejected");

    img[8] = 0xFFu;                             /* payload_len (u32 @8)   */
    img[9] = 0xFFu;
    CHECK(APIE_Bkp_LoadBlob(img, n) == 0, "oversized payload_len rejected");
    img[8] = (uint8_t)((n - APIE_BKP_HDR_LEN) & 0xFFu);
    img[9] = (uint8_t)(((n - APIE_BKP_HDR_LEN) >> 8) & 0xFFu);

    CHECK(APIE_Bkp_LoadBlob(NULL, 100u) == 0, "NULL buffer rejected");
    CHECK(APIE_Bkp_LoadBlob(img, 4u) == 0, "too-short buffer rejected");

    /* after all the rejections the store is still the empty one - nothing
       was half-imported */
    CHECK(APIE_Db_Count() == 0, "rejections leave the store untouched");
    CHECK(APP_PROFILE_Count() == 0U, "rejections leave the list untouched");
    APIE_Db_GetCounters(&c);
    CHECK(c.writes == 0UL, "rejections perform no store writes");
  }

  /* --- 7) valid CRC but invalid profile kind -> structural reject --------- */
  {
    uint32_t off = APIE_BKP_HDR_LEN + 2u + 1u;  /* first entry's kind u16 */
    n = APIE_Bkp_BuildBlob(img, sizeof(img));
    APP_PROFILE_Clear(); APIE_Db_Init(); APIE_Ml_Init();
    img[off] = 0x07u;                           /* no such kind           */
    fix_crc(img);
    CHECK(APIE_Bkp_LoadBlob(img, n) == 0, "invalid profile kind rejected (CRC valid)");
    CHECK(APP_PROFILE_Count() == 0U, "kind rejection leaves the list empty");
    img[off] = 0x00u;
    fix_crc(img);
    CHECK(APIE_Bkp_LoadBlob(img, n) != 0, "image valid again after restore");
  }

  /* --- 8) a smaller image over stale trailing bytes ------------------------ */
  {
    uint8_t small[4096];
    uint32_t m;
    /* reference: a full image (owner profiles + learned records) */
    APP_PROFILE_Clear(); APIE_Db_Init(); APIE_Ml_Init();
    (void)APP_PROFILE_AddFixed(1U, 3000U);
    (void)APP_PROFILE_AddPps(11000U, 1000U);
    {
      APIE_Profile_t a = mkprofile(0x0483u, 0x5740u, 0x2DC154u);
      APIE_Profile_t b = mkprofile(0x18D1u, 0x4EE7u, 0x2DC19Cu);
      (void)APIE_Db_StoreProfile(&a);
      (void)APIE_Db_StoreProfile(&b);
    }
    n = APIE_Bkp_BuildBlob(img, sizeof(img));
    CHECK(n > 0U, "full reference image builds");
    /* the empty-store image must be smaller */
    APP_PROFILE_Clear(); APIE_Db_Init(); APIE_Ml_Init();
    m = APIE_Bkp_BuildBlob(small, sizeof(small));   /* empty-store image    */
    CHECK(m > 0U && m < n, "empty-store image smaller");
    /* keep the stale tail of the old image: restore must ignore it         */
    memcpy(&small[m], &img[m], n - m);
    APP_PROFILE_Clear(); APIE_Db_Init(); APIE_Ml_Init();
    CHECK(APIE_Bkp_LoadBlob(small, 4096u) != 0, "small image over stale tail accepted");
    CHECK(APIE_Db_Count() == 0U, "small image restores the empty store");
    CHECK(APP_PROFILE_Count() == 0U, "small image restores the empty list");
    CHECK(APIE_Ml_Validate() != 0, "model restored from small image");
  }

  /* --- 9) worst-case capacity ----------------------------------------------- */
  {
    uint8_t i;
    uint32_t big;
    APP_PROFILE_Clear(); APIE_Db_Init(); APIE_Ml_Init();
    for (i = 0U; i < APP_PROFILE_MAX; i++)
    {
      CHECK(APP_PROFILE_AddPps((uint16_t)(3300U + 20U * i), 5000U) != 0,
            "owner step added (capacity run)");
    }
    for (i = 0U; i < APIE_DB_PROFILES; i++)
    {
      APIE_Profile_t p = mkprofile((uint16_t)(0x1000u + i), (uint16_t)(0x2000u + i),
                                   0x2DC154u + (uint32_t)i);   /* unique signature */
      (void)APIE_Db_StoreProfile(&p);
    }
    CHECK(APIE_Db_Count() == APIE_DB_PROFILES, "store filled to capacity");
    big = APIE_Bkp_BuildBlob(img, sizeof(img));
    CHECK(big > 0U, "worst-case image builds");
    CHECK(big == (APIE_BKP_HDR_LEN + (2u + 1u + APP_PROFILE_MAX * 8u) +
                  (2u + (uint32_t)(APIE_DB_PROFILES * sizeof(APIE_DbProfile_t))) +
                  (2u + (uint32_t)sizeof(APIE_MlModel_t))),
          "worst-case image size exact");
    CHECK(big <= 4096u, "worst-case image fits the 4 KiB window");
    CHECK(APIE_Bkp_Save("capacity") != 0, "worst-case checkpoint verifies");
  }

  /* --- 10) determinism: same state -> byte-identical image ------------------ */
  {
    uint8_t img2[4096];
    uint32_t a = APIE_Bkp_BuildBlob(img, sizeof(img));
    uint32_t b = APIE_Bkp_BuildBlob(img2, sizeof(img2));
    CHECK(a == b && memcmp(img, img2, a) == 0, "rebuild is byte-identical");
  }

  printf("apie_bkp_selftest: %d checks, %d failures -> %s\n",
         checks, failures, failures ? "FAIL" : "ALL OK");
  return failures ? 1 : 0;
}
