/**
 * @file    dtsmon.c
 * @brief   SoC temperature monitor on the on-die DTS sensor.
 *
 * Plugs into the DTS footprint (ext_dts.c) by overriding the two weak hooks
 * EXT_DTS_FeatureInit / EXT_DTS_FeaturePoll, exactly the way ina226.c plugs
 * into ext_i2c.c.  main.c does not know about this file.
 *
 * Nothing blocks: EXT_DTS_ReadTempC() is non-blocking, a sensor that is not
 * converting is reported instead of waited for, and the periodic report only
 * starts once a first reading has succeeded - so a board whose LSE is not
 * running stays quiet instead of printing a failure every second.
 */
#include "dtsmon.h"
#include "ext_dts.h"
#include <stdlib.h>
#include "app_log.h"
#include "main.h"
#include <string.h>

/* ---- timing -------------------------------------------------------------- */
#define DTSMON_SAMPLE_PERIOD_MS 500    /* how often the die is read          */
#define DTSMON_REPORT_PERIOD_MS 1000   /* default auto-report rate           */
#define DTSMON_PERIOD_MIN_MS    250
#define DTSMON_PERIOD_MAX_MS    60000

typedef struct
{
  uint8_t  has_reading;    /* at least one conversion succeeded              */
  uint8_t  auto_report;    /* periodic console reporting on/off              */
  uint8_t  unit_f;         /* 0 = degrees C, 1 = degrees F                   */
  int32_t  deg_c;          /* last reading, whole degrees Celsius            */
  uint32_t meas_tick;      /* HAL_GetTick() of the last good reading         */
  uint32_t report_ms;      /* periodic reporting interval                    */
  uint32_t next_meas;
  uint32_t next_report;
} DTSMON_State_t;

static DTSMON_State_t s;

/* Strict decimal parser: rejects trailing garbage. */
static int dtsmon_parse_u(const char *str, unsigned *out)
{
  char *end = NULL;
  unsigned long v;

  if ((str == NULL) || (*str == '\0'))
  {
    return -1;
  }
  v = strtoul(str, &end, 10);
  if ((end == str) || (end == NULL) || (*end != '\0'))
  {
    return -1;
  }
  *out = (unsigned)v;
  return 0;
}

static int32_t c_to_f(int32_t deg_c)
{
  return (deg_c * 9) / 5 + 32;
}

/* ========================================================================== */

void EXT_DTS_FeatureInit(void)
{
  memset(&s, 0, sizeof(s));
  s.auto_report = 1U;
  s.report_ms = DTSMON_REPORT_PERIOD_MS;
  s.unit_f = 0U;
  s.next_meas = HAL_GetTick();

  if (EXT_DTS_IsRunning() == 0U)
  {
    APP_LOG_Printf("dts    : no SoC temperature yet (LSE ready=%u) - "
                   "type 'dts status' for the reference-clock note\r\n",
                   (unsigned)EXT_DTS_LseRunning());
  }
}

static void dtsmon_sample(void)
{
  int32_t deg_c = 0;

  if (EXT_DTS_ReadTempC(&deg_c) == HAL_OK)
  {
    s.deg_c = deg_c;
    s.has_reading = 1U;
    s.meas_tick = HAL_GetTick();
  }
}

void EXT_DTS_FeaturePoll(void)
{
  uint32_t now = HAL_GetTick();

  if ((int32_t)(now - s.next_meas) >= 0)
  {
    s.next_meas = now + DTSMON_SAMPLE_PERIOD_MS;
    dtsmon_sample();
  }

  /* Only report once something has actually been measured, so an unclocked
   * sensor does not flood the console. */
  if ((s.auto_report != 0U) && (s.has_reading != 0U) &&
      ((int32_t)(now - s.next_report) >= 0))
  {
    s.next_report = now + s.report_ms;
    DTSMON_Print();
  }
}

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

uint8_t DTSMON_HasReading(void)
{
  return s.has_reading;
}

int32_t DTSMON_GetTempC(void)
{
  return s.deg_c;
}

int32_t DTSMON_GetTempF(void)
{
  return c_to_f(s.deg_c);
}

uint8_t DTSMON_DataFresh(void)
{
  return (s.has_reading != 0U) &&
         ((HAL_GetTick() - s.meas_tick) < (2U * DTSMON_SAMPLE_PERIOD_MS))
           ? 1U : 0U;
}

uint8_t DTSMON_UnitIsF(void)
{
  return s.unit_f;
}

void DTSMON_Print(void)
{
  if (s.has_reading == 0U)
  {
    APP_LOG_Printf("[dts]    no SoC temperature reading (LSE ready=%u)\r\n",
                   (unsigned)EXT_DTS_LseRunning());
    return;
  }
  if (s.unit_f != 0U)
  {
    APP_LOG_Printf("[dts]    soc %ld F   (= %ld C)%s\r\n",
                   (long)c_to_f(s.deg_c), (long)s.deg_c,
                   DTSMON_DataFresh() ? "" : "   [stale]");
  }
  else
  {
    APP_LOG_Printf("[dts]    soc %ld C   (= %ld F)%s\r\n",
                   (long)s.deg_c, (long)c_to_f(s.deg_c),
                   DTSMON_DataFresh() ? "" : "   [stale]");
  }
}

void DTSMON_PrintStatus(void)
{
  if (s.has_reading == 0U)
  {
    APP_LOG_Write("dts    : no SoC temperature reading (see 'dts status')\r\n");
    return;
  }
  APP_LOG_Printf("dts    : soc %ld C / %ld F   auto=%s every %lu ms\r\n",
                 (long)s.deg_c, (long)c_to_f(s.deg_c),
                 (s.auto_report != 0U) ? "on" : "off",
                 (unsigned long)s.report_ms);
}

/* ========================================================================== */
/*  CLI                                                                       */
/* ========================================================================== */

static void dtsmon_print_state(void)
{
  APP_LOG_Printf("dts    : %s, converting=%s, LSE ready=%u, last HAL status=%d\r\n",
                 EXT_DTS_IsReady() ? "initialised" : "NOT initialised",
                 EXT_DTS_IsRunning() ? "yes" : "no",
                 (unsigned)EXT_DTS_LseRunning(),
                 (int)EXT_DTS_LastStatus());
  APP_LOG_Printf("         unit=%s, auto=%s every %lu ms, last reading=%s\r\n",
                 (s.unit_f != 0U) ? "F" : "C",
                 (s.auto_report != 0U) ? "on" : "off",
                 (unsigned long)s.report_ms,
                 (s.has_reading != 0U) ? "ok" : "none");
  if ((EXT_DTS_IsRunning() == 0U) || (s.has_reading == 0U))
  {
    APP_LOG_Write("         This project clocks the DTS from the 32.768 kHz LSE\r\n"
                  "         (DTS.RefClock=DTS_REFCLKSEL_LSE in the .ioc).  The\r\n"
                  "         firmware switches it on by itself and retries once a\r\n"
                  "         second, so a reading should appear within ~2 s.\r\n"
                  "         If it never does, no 32.768 kHz crystal is fitted on\r\n"
                  "         PC14/PC15 - see Appli/Core/Inc/ext_dts.h.\r\n");
  }
}

void DTSMON_Cli(int argc, char *argv[])
{
  if (argc < 2)
  {
    dtsmon_sample();
    DTSMON_Print();
    return;
  }

  if (strcmp(argv[1], "auto") == 0)
  {
    if ((argc >= 3) && (strcmp(argv[2], "off") == 0))
    {
      s.auto_report = 0U;
      APP_LOG_Write("dts periodic reporting off\r\n");
    }
    else
    {
      s.auto_report = 1U;
      s.next_report = HAL_GetTick();
      APP_LOG_Write("dts periodic reporting on\r\n");
    }
  }
  else if (strcmp(argv[1], "period") == 0)
  {
    unsigned ms = 0U;
    if ((argc < 3) || (dtsmon_parse_u(argv[2], &ms) != 0) ||
        (ms < DTSMON_PERIOD_MIN_MS) || (ms > DTSMON_PERIOD_MAX_MS))
    {
      APP_LOG_Write("usage: dts period <ms 250..60000>\r\n");
    }
    else
    {
      s.report_ms = ms;
      APP_LOG_Printf("dts reporting every %u ms\r\n", ms);
    }
  }
  else if (strcmp(argv[1], "unit") == 0)
  {
    if ((argc < 3) || ((strcmp(argv[2], "c") != 0) && (strcmp(argv[2], "C") != 0) &&
                       (strcmp(argv[2], "f") != 0) && (strcmp(argv[2], "F") != 0)))
    {
      APP_LOG_Write("usage: dts unit c|f\r\n");
    }
    else
    {
      s.unit_f = ((argv[2][0] == 'f') || (argv[2][0] == 'F')) ? 1U : 0U;
      APP_LOG_Printf("dts reports in degrees %s\r\n",
                     (s.unit_f != 0U) ? "Fahrenheit" : "Celsius");
      DTSMON_Print();
    }
  }
  else if ((strcmp(argv[1], "read") == 0) || (strcmp(argv[1], "temp") == 0))
  {
    dtsmon_sample();
    DTSMON_Print();
  }
  else if (strcmp(argv[1], "status") == 0)
  {
    dtsmon_print_state();
  }
  else
  {
    APP_LOG_Write("usage: dts [auto on|off | period <ms> | unit c|f | read | status]\r\n");
  }
}
