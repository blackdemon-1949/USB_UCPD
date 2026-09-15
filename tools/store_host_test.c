/**
  ******************************************************************************
  * @file    tools/store_host_test.c
  * @brief   Host-side unit test for the external-flash store record layer
  *          (Appli/Core/Src/app_store.c).
  *
  * Why this exists: the virtual board proves the XSPI driver talks to a NOR
  * device correctly (indirect-mode sequences, erase, program, read back), but
  * reaching a *record* round trip there means booting the whole application
  * twice inside Unicorn, which is minutes of host time per attempt.  The record
  * layer itself is portable C: this test compiles app_store.c natively against
  * an in-RAM NOR with the same semantics as the chip (4 KB sector erase,
  * programming only clears bits) and drives it directly, including the path the
  * emulator is slowest at - "power cycle and read it all back".
  *
  * Build / run:
  *     gcc -std=c11 -Wall -I Appli/Core/Inc -I tools/host_shim \
  *         -o /tmp/store_host_test tools/store_host_test.c \
  *            Appli/Core/Src/app_store.c && /tmp/store_host_test
  *
  * Exit code: 0 = every check passed, 1 = a check failed.
  ******************************************************************************
  */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_store.h"
#include "app_cmos.h"
#include "ext_nor.h"

/* ---------------------------------------------------------------- in-RAM NOR */
#define NOR_SIZE   (EXT_NOR_STORE_OFF + EXT_NOR_STORE_SIZE)
static uint8_t nor[NOR_SIZE];
static uint32_t nor_programs, nor_erases, nor_fail_after = 0xFFFFFFFFu;
static uint32_t nor_ops;

void nor_reset(void)
{
  memset(nor, 0xFF, sizeof(nor));
  nor_programs = 0;
  nor_erases = 0;
  nor_ops = 0;
  nor_fail_after = 0xFFFFFFFFu;
}

static int nor_fail(void)
{
  nor_ops++;
  return (nor_ops > nor_fail_after) ? 1 : 0;
}

int EXT_NOR_Read(uint32_t off, void *buf, uint32_t len)
{
  if (nor_fail()) return EXT_NOR_ERR_TIMEOUT;
  if ((off + len) > NOR_SIZE) return EXT_NOR_ERR_OFF;
  memcpy(buf, &nor[off], len);
  return EXT_NOR_OK;
}

int EXT_NOR_Program(uint32_t off, const void *buf, uint32_t len)
{
  const uint8_t *src = (const uint8_t *)buf;
  if (nor_fail()) return EXT_NOR_ERR_TIMEOUT;
  if ((off + len) > NOR_SIZE) return EXT_NOR_ERR_OFF;
  for (uint32_t i = 0; i < len; i++)
  {
    nor[off + i] &= src[i];               /* NOR can only clear bits */
  }
  nor_programs++;
  return EXT_NOR_OK;
}

int EXT_NOR_EraseSector(uint32_t off)
{
  if (nor_fail()) return EXT_NOR_ERR_TIMEOUT;
  if (((off % EXT_NOR_SECTOR_SIZE) != 0U) || ((off + EXT_NOR_SECTOR_SIZE) > NOR_SIZE))
  {
    return EXT_NOR_ERR_PARAM;
  }
  memset(&nor[off], 0xFF, EXT_NOR_SECTOR_SIZE);
  nor_erases++;
  return EXT_NOR_OK;
}

uint8_t  EXT_NOR_Ready(void)   { return 1; }
uint8_t  EXT_NOR_Present(void) { return 1; }
uint32_t EXT_NOR_JedecId(void) { return 0x856017; }
uint32_t EXT_NOR_Errors(void)  { return 0; }
uint32_t EXT_NOR_Writes(void)  { return nor_programs; }
const char *EXT_NOR_Status(void) { return "host-test in-RAM NOR"; }
uint32_t EXT_NOR_SelfTest(void) { return 1; }

/* ------------------------------------------------------------------ CMOS/log */
static app_settings_t settings;
app_settings_t *APP_CMOS(void) { return &settings; }
void APP_CMOS_Save(void) { }

void APP_LOG_Write(const char *s) { (void)s; }
void APP_LOG_Printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}
void APP_LOG_WriteRaw(const void *p, uint32_t n) { (void)p; (void)n; }

/* ------------------------------------------------------------------- checks */
static uint32_t tick;
uint32_t HAL_GetTick(void) { return tick; }

static int failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
      printf("  FAIL  "); printf(__VA_ARGS__); printf("\n");    \
      failures++;                                               \
    } else {                                                    \
      printf("  ok    "); printf(__VA_ARGS__); printf("\n");    \
    }                                                           \
  } while (0)

static void fill(uint8_t *buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0; i < len; i++)
  {
    buf[i] = (uint8_t)(seed + i);
  }
}

int main(void)
{
  uint8_t buf[APP_STORE_MAX_PAYLOAD];
  uint32_t len;

  printf("external-flash store record layer, host test\n");
  printf("window %u KB = %u sectors, record payload up to %u B\n\n",
         (unsigned)(EXT_NOR_STORE_SIZE / 1024u),
         (unsigned)(EXT_NOR_STORE_SIZE / EXT_NOR_SECTOR_SIZE),
         (unsigned)APP_STORE_MAX_PAYLOAD);

  /* --- 1. a blank chip: init must format it ------------------------------- */
  printf("1. blank flash -> init/format\n");
  nor_reset();
  CHECK(APP_STORE_Init() != 0, "store mounts on a blank chip");
  CHECK(APP_STORE_Ready() != 0, "store reports ready");
  CHECK(nor_erases > 0, "format erased a sector (%u)", (unsigned)nor_erases);
  CHECK(nor_programs > 0, "format wrote the superblock (%u programs)", (unsigned)nor_programs);

  /* --- 2. write one of every record type ---------------------------------- */
  printf("\n2. write records of every type\n");
  for (uint8_t type = APP_STORE_T_SETTINGS; type <= APP_STORE_T_LEARN_CSV; type++)
  {
    for (uint32_t i = 0; i < 32; i++) buf[i] = (uint8_t)(type * 7u + i);
    int rc = APP_STORE_Write(type, buf, 32);
    CHECK(rc == 0, "write type %u -> rc %d", (unsigned)type, rc);
    len = 0;
    uint8_t back[APP_STORE_MAX_PAYLOAD];
    memset(back, 0, sizeof(back));
    rc = APP_STORE_Read(type, back, sizeof(back), &len);
    CHECK((rc == 0) && (len == 32) && (memcmp(back, buf, 32) == 0),
          "read type %u back, %u B, payload matches", (unsigned)type, (unsigned)len);
  }

  /* --- 3. many records + a payload at the maximum size -------------------- */
  printf("\n3. many records, maximum payload, overwrite of a type\n");
  uint32_t written = 0;
  for (uint32_t i = 0; i < 400; i++)
  {
    for (uint32_t k = 0; k < 100; k++) buf[k] = (uint8_t)(i ^ k);
    if (APP_STORE_Write(APP_STORE_T_EVENT, buf, 100) == 0) written++;
    tick += 500;                     /* the event writer is rate limited    */
  }
  CHECK(written > 0, "%u event records accepted", (unsigned)written);
  fill(buf, APP_STORE_MAX_PAYLOAD, 0x5A);
  CHECK(APP_STORE_Write(APP_STORE_T_ML, buf, APP_STORE_MAX_PAYLOAD) == 0,
        "a full-size (%u B) record is accepted", (unsigned)APP_STORE_MAX_PAYLOAD);
  len = 0;
  memset(buf, 0, sizeof(buf));
  CHECK((APP_STORE_Read(APP_STORE_T_ML, buf, sizeof(buf), &len) == 0) && (len == APP_STORE_MAX_PAYLOAD),
        "the full-size record reads back whole (%u B)", (unsigned)len);
  uint8_t want[APP_STORE_MAX_PAYLOAD];
  fill(want, APP_STORE_MAX_PAYLOAD, 0x5A);
  CHECK(memcmp(buf, want, APP_STORE_MAX_PAYLOAD) == 0, "its payload is intact");

  /* --- 4. the reboot: re-init and read everything again ------------------- */
  printf("\n4. power cycle (re-init) and read the records back\n");
  uint32_t progs_before = nor_programs;
  CHECK(APP_STORE_Init() != 0, "store remounts from the flash image");
  CHECK(nor_programs == progs_before, "remount wrote nothing (%u programs stayed)", (unsigned)nor_programs);
  len = 0;
  memset(buf, 0, sizeof(buf));
  CHECK((APP_STORE_Read(APP_STORE_T_SETTINGS, buf, sizeof(buf), &len) == 0) && (len == 32),
        "a record written before the 'reboot' survives (%u B)", (unsigned)len);
  CHECK(buf[0] == (uint8_t)(APP_STORE_T_SETTINGS * 7u), "and keeps its payload (first byte 0x%02X)", buf[0]);
  len = 0;
  CHECK((APP_STORE_Read(APP_STORE_T_LEARN_CSV, buf, sizeof(buf), &len) == 0) && (len == 32),
        "the CSV record survives too");

  /* --- 5. sector roll-over: the window must not run out ------------------- */
  printf("\n5. keep writing until the store rolls into another sector\n");
  uint32_t sectors_used = 0;
  for (uint32_t i = 0; i < 900; i++)
  {
    for (uint32_t k = 0; k < 128; k++) buf[k] = (uint8_t)(i + k);
    tick += 500;
    (void)APP_STORE_Write(APP_STORE_T_EVENT, buf, 128);
  }
  sectors_used = nor_erases;         /* every new sector is erased first */
  CHECK(sectors_used > 1, "records rolled over into fresh sectors (%u erases)", (unsigned)sectors_used);
  len = 0;
  CHECK((APP_STORE_Read(APP_STORE_T_ML, buf, sizeof(buf), &len) == 0) && (len == APP_STORE_MAX_PAYLOAD),
        "a record from the first sector is still readable after the roll-over (%u B)", (unsigned)len);

  /* --- 6. a flash that starts failing must not corrupt anything ----------- */
  printf("\n6. write failure is refused, not half-applied\n");
  nor_fail_after = nor_ops + 1;      /* fail the next operation after one more */
  fill(buf, 64, 0x11);
  int rc = APP_STORE_Write(APP_STORE_T_PROFILE, buf, 64);
  CHECK(rc != 0, "the write is refused while the chip errors (rc %d)", rc);
  nor_fail_after = 0xFFFFFFFFu;      /* chip healthy again */
  len = 0;
  memset(buf, 0, sizeof(buf));
  rc = APP_STORE_Read(APP_STORE_T_PROFILE, buf, sizeof(buf), &len);
  CHECK(rc != 0, "the failed record is not reported as present (rc %d)", rc);
  CHECK(APP_STORE_Init() != 0, "the store still mounts after a failed write");

  /* --- 7. format rebuilds the window ------------------------------------- */
  printf("\n7. format erases the records again\n");
  CHECK(APP_STORE_Format() == 0, "format returns success");
  len = 0;
  rc = APP_STORE_Read(APP_STORE_T_ML, buf, sizeof(buf), &len);
  CHECK(rc != 0, "the old records are gone after a format (rc %d)", rc);
  CHECK(APP_STORE_Init() != 0, "and the store mounts empty again");

  printf("\n%s (%d check%s failed)\n", failures ? "FAILED" : "all checks passed",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
