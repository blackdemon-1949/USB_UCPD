/**
  ******************************************************************************
  * @file    apie_bkp.h
  * @brief   APIE persistence backend: on-chip Backup SRAM (BKPSRAM, 4 KB).
  *
  * Why BKPSRAM and not the external NOR: the application executes from the
  * PY25Q64HA at 0x90000000 (XiP), and NOR program/erase while executing from
  * the same part is not XIP-safe (see FLASH_ENDURANCE.md - NOR writes stay
  * DISABLED).  The STM32H7R3's on-chip backup SRAM is a separate die SRAM in
  * the VBAT domain at 0x38800000 on a different bus; using it cannot violate
  * the XIP-safety rule.  It has no program/erase endurance budget at all.
  *
  * Retention model (be precise about what is actually promised):
  *   - content survives any system reset while VDD stays up (no regulator
  *     needed, DBP + BKPRAMEN only), and
  *   - content survives VDD removal ONLY when the VBAT rail holds the backup
  *     domain up AND the backup regulator was enabled (PWR_CSR1.BREN, ready
  *     flag PWR_CSR1.BRRDY) before power went away.
  * The backup regulator is therefore enabled (and waited for) at init, and
  * the CLI reports the difference honestly ("VBAT-RETAINED" vs
  * "RESET-ONLY").  Tamper events and backup-domain resets can auto-erase
  * BKPSRAM (SBS_MESR.MEF, and errata ES0596 2.2.4 can leave the backup
  * domain un-reset with garbage), so the blob carries a whole-image CRC-32
  * gate: an invalid image is discarded in favour of a clean empty store,
  * never loaded as data.
  *
 * Format (whole-image, independent of the per-record CRCs already inside
 * APIE_DbProfile_t):
 *
 *   offset 0x00  APIE_BkpHeader_t (32 bytes, magic + schema + CRC-32)
 *   offset 0x20  payload, section table (each section is u16 length + data):
 *                 u16 prof_len
 *                 profiles: u8 count, count x 8 bytes
 *                           (u16 kind, u16 index, u16 mv, u16 ma)
 *                 u16 db_len
 *                 db: APIE_Db_Export image (u16 n + n x APIE_DbProfile_t)
 *                 u16 ml_len
 *                 ml: APIE_MlModel_t (raw struct bytes)
 *
 * The payload covers everything the user or the engine ever customizes:
 * the PC13 owner profile list, the learned source profiles and the online
 * ML model.  The staging buffer is allocated for the FULL 4 KiB window
 * minus the header, so adding sections later needs no re-sizing.
 *
 * The raw-struct layout is tied to this build; APIE_BKP_SCHEMA guards it.
 * Anything that changes sizeof(APIE_DbProfile_t) / sizeof(APIE_MlModel_t)
 * must bump the schema so an older image is rejected instead of parsed.
 ******************************************************************************
 */
#ifndef APIE_BKP_H
#define APIE_BKP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "apie.h"

/* Whole-image magic + schema. */
#define APIE_BKP_MAGIC       0x41424B50u   /* 'ABKP'                        */
#define APIE_BKP_SCHEMA      2u            /* 2 = + profile section         */
#define APIE_BKP_HDR_LEN     32u           /* fixed header size, bytes     */
/* Structural payload minimum: three (u16 length + data) section headers,
 * all sections empty.  The section parser and the per-section importers do
 * the real validation. */
#define APIE_BKP_PAYLOAD_MIN 6u

/* Enable sequence is per RM0477 (STM32H7R3/S3 backup domain) and matches the
 * H7RS HAL: PWR_CR1.DBP gates backup-SRAM writes, RCC_AHB4ENR.BKPRAMEN gates
 * any access, PWR_CSR1.BREN + BRRDY make the content survive VBAT mode.
 * The host self-test builds apie_bkp.c with this set to 0: a RAM array then
 * stands in for the backup SRAM and the register path is compiled out. */
#ifndef APIE_BKP_HW_ENABLED
#define APIE_BKP_HW_ENABLED  1u
#endif

typedef struct
{
  uint32_t magic;          /* APIE_BKP_MAGIC                               */
  uint16_t schema;         /* APIE_BKP_SCHEMA                              */
  uint16_t hdr_len;        /* APIE_BKP_HDR_LEN                             */
  uint32_t payload_len;    /* bytes after the header                       */
  uint32_t payload_crc32;  /* CRC-32 over the payload bytes                */
  uint32_t seq;            /* checkpoint sequence number (monotonic)       */
  uint32_t reserved[3];    /* pad to 32 bytes, future use                  */
} APIE_BkpHeader_t;

/* Bring the backend up and restore the last image (if valid) into the RAM
 * database + ML model.  Called once from APIE_Init(), after APIE_Db_Init()
 * and APIE_Ml_Init().  Never fatal: on any failure the store stays RAM-only
 * and the true state is reported by APIE_Bkp_Status(). */
void APIE_Bkp_Init(void);

/* Periodic maintenance, called from APIE_Task() every super-loop pass:
 * retries a failed backup regulator about once a second and takes a slow
 * aggregated model checkpoint (max once per APIE_BKP_MODEL_PERIOD_MS when
 * the model actually changed).  Cheap when there is nothing to do. */
void APIE_Bkp_Task(void);

/* Take a checkpoint now: serialize profiles + DB + model, CRC, write to
 * BKPSRAM, __DSB, read back and CRC-verify.  Returns 1 on success, 0 on
 * failure (backend not up, blob too large, or read-back mismatch). */
uint8_t APIE_Bkp_Save(const char *reason);

/* Re-read the profile section of the stored image and re-apply it to the
 * live profile list ('profile load').  1 = applied, 0 = no valid image or
 * the image's DB/model state was rejected. */
uint8_t APIE_Bkp_LoadProfiles(void);

/* --- observable state, in the style of s_usb_clock_ok in usbd_conf.c ----- */

/* 1 = the BKPSRAM clock + backup-domain write access came up.  When 0 the
 * backend is unusable and nothing is ever written; the store is RAM-only. */
uint8_t APIE_Bkp_HwOk(void);
/* 1 = the backup regulator reported ready, so the image is claimed to
 * survive VDD loss with VBAT present.  0 = survives resets only. */
uint8_t APIE_Bkp_RetentionOk(void);
/* 1 = a valid image was found and restored at boot (0 = first boot or the
 * image was blank/corrupt and was discarded for a clean empty store). */
uint8_t APIE_Bkp_LoadedOk(void);
/* 1 = the last checkpoint write verified; 0 = it failed or none yet. */
uint8_t APIE_Bkp_LastWriteOk(void);

uint32_t APIE_Bkp_Writes(void);        /* physical checkpoint writes        */
uint32_t APIE_Bkp_WriteFails(void);    /* writes that failed verify         */
uint32_t APIE_Bkp_Loads(void);         /* images restored since boot        */
uint32_t APIE_Bkp_Seq(void);           /* current image sequence number     */

/* One-line status for the CLI, e.g.
 *   "bkp: BKPSRAM ready, VBAT-retained, 3 profile(s)+model restored, 5 writes"
 *   "bkp: BKPSRAM ready (reset-only, breg not ready), no valid image"
 *   "bkp: gate FAILED - store is RAM-only"
 * Bounded, snprintf into the caller buffer like APIE_Db_Status(). */
void APIE_Bkp_Status(char *out, uint32_t outsz);

/* --- pure blob functions (host-testable, no hardware) -------------------- */

/* Serialize the current RAM database + ML model into buf as
 * header+payload.  Returns total bytes written, or 0 if it does not fit. */
uint32_t APIE_Bkp_BuildBlob(uint8_t *buf, uint32_t size);

/* Validate a header+payload image and import it into the RAM database and
 * ML model.  1 = imported, 0 = rejected (bad magic/schema/length/CRC or
 * the imported records failed their own per-record validation). */
uint8_t APIE_Bkp_LoadBlob(const uint8_t *buf, uint32_t size);

#if !APIE_BKP_HW_ENABLED
/* Host-build only: re-run the restore path against the host backing store
 * (hardware enables are meaningless there).  Used by the host self-test. */
void APIE_Bkp_TestRestore(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* APIE_BKP_H */
