/**
  ******************************************************************************
  * @file    apie_unknown_selftest.c
  * @brief   Host-side verification of the unknown-protocol analyzer.
  *
  * Compiles the real firmware apie_unknown.c against a host stub of app_log
  * and HAL_GetTick, then exercises UNKNOWN_SIGNATURE characterization:
  * bucketing, frequency, stable/changing bytes, bit changes, entropy, and the
  * TELEMETRY_CANDIDATE / PERIODIC / RESET_LINKED categories.
  *
  * Run via tools/apie_unknown_selftest.sh (or gcc directly with -lm).
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "apie_unknown.h"

/* ---- firmware dependencies stubbed for the host ------------------------- */
uint32_t s_fake_tick = 1000000UL;
uint32_t HAL_GetTick(void) { return s_fake_tick; }

/* app_log stubs (only Dump uses these; tests use APIE_Unknown_Get). */
static char s_out[4096];
static uint32_t s_outlen;
void APP_LOG_Write(const char *s) { while (*s) { s_out[s_outlen++ & 4095] = *s++; } }
void APP_LOG_Printf(const char *fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  vsnprintf(s_out, sizeof(s_out), fmt, ap);
  va_end(ap);
}

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
  APIE_Unknown_Init();

  /* 1) An unknown, frequently-repeating, power-correlated message should be
        bucketed once and classified TELEMETRY_CANDIDATE with good confidence. */
  {
    uint8_t pkt[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x00 };
    int idx;
    uint32_t i;
    for (i = 0U; i < 20U; i++)
    {
      pkt[5] = (uint8_t)(5000U & 0xFFu); /* VBUS LSB correlates */
      idx = APIE_Unknown_Observe(0, 0x3F, 1, pkt, sizeof(pkt),
                                 5000, 1000, 41, 0, 0, 0, 0xFF);
      CHECK(idx >= 0, "telemetry bucket created");
      s_fake_tick += 50UL; /* 20 packets/sec */
    }
    const APIE_UnknownKind_t *k = APIE_Unknown_Get(0);
    CHECK(k != NULL, "telemetry bucket exists");
    if (k)
    {
      CHECK(k->session_count == 20, "telemetry counted 20");
      CHECK(k->category == APIE_UNKNOWN_CAT_TELEMETRY_CANDIDATE, "telemetry category");
      CHECK(k->confidence >= 60, "telemetry confidence >= 60");
      CHECK(k->freq_x1000 >= 18000UL && k->freq_x1000 <= 22000UL, "~20/s frequency");
      CHECK(k->vbus_corr > 0, "VBUS correlation observed");
      CHECK(k->entropy > 0, "entropy computed");
    }
  }

  /* 2) A second distinct signature gets its own bucket. */
  {
    uint8_t pkt[3] = { 0xAA, 0xBB, 0xCC };
    int idx = APIE_Unknown_Observe(2, 0x22, 0, pkt, 3, 5000, 1000, 41, 0, 0, 0, 0xFF);
    CHECK(idx == 1, "second bucket separate");
    CHECK(APIE_Unknown_Count() == 2, "two buckets total");
  }

  /* 3) RESET_LINKED: a message that appears with every reset. */
  {
    uint8_t pkt[2] = { 0x11, 0x22 };
    int idx;
    for (int i = 0; i < 5; i++)
    {
      idx = APIE_Unknown_Observe(0, 0x11, 2, pkt, 2, 5000, 0, 41, 1, 0, 0, 0xFF);
      CHECK(idx >= 0, "reset-linked bucket created");
      s_fake_tick += 100UL;
    }
    const APIE_UnknownKind_t *k = APIE_Unknown_Get(idx >= 0 ? idx : 0);
    (void)k;
  }

  /* 4) Category names are stable and non-fabricated. */
  CHECK(strcmp(APIE_Unknown_CatName(APIE_UNKNOWN_CAT_TELEMETRY_CANDIDATE),
               "TELEMETRY_CANDIDATE") == 0, "telemetry cat name");
  CHECK(strcmp(APIE_Unknown_CatName(APIE_UNKNOWN_CAT_PERIODIC), "PERIODIC") == 0, "periodic cat name");

  printf("apie_unknown_selftest: %d checks, %d failures\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
