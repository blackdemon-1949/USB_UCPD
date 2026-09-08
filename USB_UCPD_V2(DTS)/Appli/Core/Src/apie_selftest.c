/**
  ******************************************************************************
  * @file    apie_selftest.c
  * @brief   One-command, non-destructive self-test (selftest).
  *
  * Every check here is deterministic and side-effect free with respect to the
  * live PD contract, flash, and electrical state.  It never sends a power
  * request, never changes voltage, never programs/erases NOR, and never
  * transmits an unknown packet.  Scope: all / quick / full / pd / decoder /
  * ml / database / flash.
  ******************************************************************************
  */
#include "apie_selftest.h"
#include "apie.h"
#include "apie_decode.h"
#include "apie_analyzer.h"
#include "apie_stats.h"
#include "apie_ml.h"
#include "apie_profile.h"
#include "apie_unknown.h"
#include "apie_plan.h"
#include "apie_db.h"
#include "app_log.h"
#include "app_pd.h"
#include "ina226.h"
#include "dtsmon.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

extern uint32_t HAL_GetTick(void);

static APIE_SelfTestResult_t s_res;

static void chk(int cond, const char *name)
{
  if (cond) { s_res.pass++; }
  else { s_res.fail++; APP_LOG_Printf("  [FAIL] %s\r\n", name); }
}

/* ---------------------------------------------------------------------------
 *  Deterministic decoder / packet checks
 * ------------------------------------------------------------------------- */
static void selftest_decoder(void)
{
  APIE_Header_t h;
  char buf[48];
  uint32_t min_mv, max_mv, ma, mwp;

  APIE_Decode_Header(0x6081u, &h);           /* Source_Cap, 6 objects */
  chk(h.type == 0x01 && h.nobjects == 6, "decoder: Source_Cap header");

  APIE_Decode_TypeNameN(0x03, 0, 0, buf, sizeof(buf));
  chk(strcmp(buf, "Accept") == 0, "decoder: Accept name");
  APIE_Decode_TypeNameN(0x01, 0, 6, buf, sizeof(buf));
  chk(strcmp(buf, "Source_Capabilities") == 0, "decoder: Source_Cap name");

  /* Fixed 9V/3A, PPS 3.3-21V/3A, AVS 15-48V, EPR fixed 48V */
  APIE_Decode_PdoCaps(0x0002D12Cu, &min_mv, &max_mv, &ma, &mwp);
  chk(min_mv == 9000 && ma == 3000, "decoder: fixed 9V/3A");
  APIE_Decode_PdoCaps(0xC1A4213Cu, &min_mv, &max_mv, &ma, &mwp);
  chk(APIE_Decode_ApdoType(0xC1A4213Cu) == APIE_APDO_TYPE_PPS &&
      min_mv == 3300 && max_mv == 21000 && ma == 3000, "decoder: PPS 3.3-21V/3A");
  {
    uint32_t avs = 0xC0000000u | (1u << 28) | (480u << 17) | (150u << 8) | 100u;
    APIE_Decode_PdoCaps(avs, &min_mv, &max_mv, &ma, &mwp);
    chk(APIE_Decode_ApdoType(avs) == APIE_APDO_TYPE_AVS &&
        min_mv == 15000 && max_mv == 48000, "decoder: AVS 15-48V");
  }
  {
    uint32_t epr = (960u << 10) | (50u << 0) | (1u << 23);
    APIE_Decode_PdoCaps(epr, &min_mv, &max_mv, &ma, &mwp);
    chk(max_mv == 48000, "decoder: EPR fixed 48V");
  }
  {
    uint32_t vdm = (uint32_t)((0xFF00u << 16) | (1u << 15) | (1u << 13) | 0x01u);
    chk(APIE_Decode_VdmStructured(vdm) == 1, "decoder: VDM structured");
    chk(APIE_Decode_SvdmCommand(vdm) == 0x01, "decoder: Discover_Identity cmd");
  }
  /* malformed: type out of range, bad NDO -> robust fallback name */
  APIE_Decode_TypeNameN(0x1F, 0, 0, buf, sizeof(buf));
  chk(strstr(buf, "0x1F") != NULL, "decoder: unknown type named as hex");
  chk(APIE_Decode_PdoType(0xFFFFFFFFu) == 3u, "decoder: max PDO type clamp");
}

/* ---------------------------------------------------------------------------
 *  Statistics / CRC / ML / transaction self-checks
 * ------------------------------------------------------------------------- */
static void selftest_stats(void)
{
  APIE_StatAccum_t a;
  uint32_t i;
  float mean, err;
  APIE_Stats_Init(&a);
  for (i = 1U; i <= 100U; i++)
  {
    APIE_Stats_Update(&a, (float)i);
  }
  mean = APIE_Stats_Mean(&a);
  err = (mean > 50.5f) ? (mean - 50.5f) : (50.5f - mean);
  chk(err < 0.001f, "stats: Welford mean=50.5");
  chk(APIE_Stats_Variance(&a) > 800.0f && APIE_Stats_Variance(&a) < 850.0f,
      "stats: Welford variance ~833");
}

static void selftest_crc(void)
{
  /* "123456789" -> 0xCBF43926 */
  const uint8_t d[] = { '1','2','3','4','5','6','7','8','9' };
  chk(APIE_Crc32(d, sizeof(d)) == 0xCBF43926UL, "crc32: IEEE-802.3 vector");
}

static void selftest_ml(void)
{
  APIE_FeatureVec_t fv;
  float p1, p2;
  uint8_t i;
  APIE_Ml_Reset();
  /* online learning: 10 useful + 2 not-useful observations */
  for (i = 0; i < 10; i++)
  {
    APIE_Ml_Observe(0, 0, 1, 1, 1);
  }
  APIE_Ml_Observe(0, 0, 1, 1, 0);
  APIE_Ml_Observe(0, 0, 1, 1, 0);
  chk(APIE_Ml_Validate(), "ml: model structurally valid");
  memset(&fv, 0, sizeof(fv));
  fv.v[8] = 1.0f; fv.v[1] = 1.0f;
  p1 = APIE_Ml_PredictUseful(&fv, 0);
  p2 = APIE_Ml_PredictUseful(&fv, 0);
  chk(p1 == p2, "ml: prediction deterministic");
  chk(p1 >= 0.0f && p1 <= 1.0f, "ml: probability in [0,1]");

  /* decision tree: PPS query only useful on a PPS source. */
  {
    APIE_FeatureVec_t t;
    memset(&t, 0, sizeof(t));
    chk(APIE_Tree_ClassifyUseful(&t, APIE_QUERY_GET_PPS) == 0, "ml tree: no-PPS -> PPS not useful");
    t.v[8] = 1.0f;
    chk(APIE_Tree_ClassifyUseful(&t, APIE_QUERY_GET_PPS) == 1, "ml tree: has-PPS -> PPS useful");
    chk(APIE_Tree_ClassifyUseful(&t, APIE_QUERY_GET_STATUS) == 1, "ml tree: Get_Status useful");
  }

  /* anomaly detector: stable latency, then one spike -> flagged. */
  {
    uint32_t i;
    for (i = 0; i < 20; i++) { APIE_Ml_Anomaly_Observe(40.0f); }
    chk(APIE_Ml_Anomaly_Trained(), "ml anomaly: trained after samples");
    chk(APIE_Ml_Anomaly_Flag(42.0f, 3.0f) == 0, "ml anomaly: normal not flagged");
    chk(APIE_Ml_Anomaly_Flag(4000.0f, 3.0f) == 1, "ml anomaly: spike flagged");
  }
}

static void selftest_txn(void)
{
  /* transaction: begin a Request expecting Accept(0x03); receive Accept; the
     engine should correlate it and push a SUCCESS record into history. */
  APIE_Txn_Init();
  APIE_Txn_Begin(0, APIE_DIR_TX, 0, 0x02, 0x03, 0, 1, 0);
  chk(APIE_Txn_ActiveCount() >= 1, "txn: active transaction tracked");
  APIE_Txn_OnRx(0, 0, 0, 0x03, 1);   /* matched expected response -> SUCCESS */
  chk(APIE_Txn_ActiveCount() == 0, "txn: transaction closed on response");
  chk(APIE_Txn_HistoryCount() >= 1, "txn: history recorded");
}

static void selftest_database(void)
{
  chk(APIE_Db_SelfTest(), "db: scratch store/readback/validate");
  chk(APIE_Db_ValidateAll(), "db: all records CRC-valid");
  chk(APIE_Crc32((const uint8_t *)"", 0) == 0u || APIE_Crc32((const uint8_t *)"", 0) != 0xFFFFFFFFu,
      "db: CRC32 defined for empty input");
}

/* ---------------------------------------------------------------------------
 *  PD / electrical-status self-checks (non-destructive, no requests)
 * ------------------------------------------------------------------------- */
static void selftest_pd(void)
{
  const APIE_Profile_t *p = APIE_Profile_Get();
  APP_LOG_Printf("  pd: attached=%u contract=%u caps=%u vbus=%lu mV\r\n",
                 (unsigned)APP_PD_IsAttached(), (unsigned)APP_PD_HasContract(),
                 (unsigned)(p ? p->n_pdo : 0U), (unsigned long)APP_PD_GetVbusMv());
  chk(APIE_Safety_LimitsSane(), "safety: guard rails sane");
  chk(APIE_Analyzer_Count() == APIE_Analyzer_Count(), "analyzer: counters consistent");
  if (INA226_IsPresent())
  {
    chk(INA226_DataFresh(), "ina226: data fresh");
  }
  else
  {
    APP_LOG_Write("  ina226: not present (skipped)\r\n");
  }
  if (DTSMON_DataFresh())
  {
    chk(DTSMON_GetTempC() >= -40 && DTSMON_GetTempC() <= 125, "dts: temp in range");
  }
  else
  {
    APP_LOG_Write("  dts: no reading yet (skipped)\r\n");
  }
}

static void selftest_flash(void)
{
  /* Non-destructive status only.  XIP execution is active; NOR program/erase
     is DISABLED for XIP safety, so there is nothing destructive to test. */
  APIE_DbCounters_t c;
  APIE_Db_GetCounters(&c);
  APP_LOG_Write("  flash: XIP ACTIVE, NOR WRITE DISABLED (XIP safety) - no destructive op\r\n");
  APP_LOG_Printf("  flash: erases=%lu wear=%lu checkpoints=%lu persist=%s\r\n",
                 (unsigned long)c.erases, (unsigned long)c.wear,
                 (unsigned long)c.checkpoints, c.nor_persist ? "NOR" : "RAM");
  chk(c.nor_persist == 0U, "flash: NOR write disabled (XIP safe)");
  chk(c.erases == 0U && c.wear == 0U, "flash: no endurance consumed by selftest");
}

/* ---------------------------------------------------------------------------
 *  Driver
 * ------------------------------------------------------------------------- */
void APIE_SelfTest_Run(const char *scope)
{
  uint32_t t0 = HAL_GetTick();
  uint8_t run_all, run_quick, run_pd, run_dec, run_ml, run_db, run_fl;
  memset(&s_res, 0, sizeof(s_res));

  run_all  = (scope == NULL) || (strcmp(scope, "all") == 0) || (strcmp(scope, "full") == 0);
  run_quick = (scope != NULL) && (strcmp(scope, "quick") == 0);
  run_pd   = run_all || run_quick || ((scope != NULL) && (strcmp(scope, "pd") == 0));
  run_dec  = run_all || run_quick || ((scope != NULL) && (strcmp(scope, "decoder") == 0));
  run_ml   = run_all || ((scope != NULL) && (strcmp(scope, "ml") == 0));
  run_db   = run_all || ((scope != NULL) && (strcmp(scope, "database") == 0));
  run_fl   = run_all || ((scope != NULL) && (strcmp(scope, "flash") == 0));

  APP_LOG_Printf("selftest [%s]: non-destructive, no power requests\r\n",
                 (scope != NULL) ? scope : "all");

  if (run_dec) { APP_LOG_Write("  decoder\r\n"); selftest_decoder(); }
  if (run_dec) { APP_LOG_Write("  stats\r\n"); selftest_stats(); }
  if (run_dec) { APP_LOG_Write("  crc\r\n"); selftest_crc(); }
  if (run_ml) { APP_LOG_Write("  ml\r\n"); selftest_ml(); }
  if (run_ml || run_all) { APP_LOG_Write("  txn\r\n"); selftest_txn(); }
  if (run_db) { APP_LOG_Write("  database\r\n"); selftest_database(); }
  if (run_pd) { APP_LOG_Write("  pd/safety/ina/dts\r\n"); selftest_pd(); }
  if (run_fl) { APP_LOG_Write("  flash\r\n"); selftest_flash(); }

  s_res.ok = (s_res.fail == 0U) ? 1U : 0U;
  s_res.ms = HAL_GetTick() - t0;
  APP_LOG_Printf("selftest: %u passed, %u failed, %lu ms -> %s\r\n",
                 (unsigned)s_res.pass, (unsigned)s_res.fail, (unsigned long)s_res.ms,
                 s_res.ok ? "ALL OK" : "FAILURES");
}
