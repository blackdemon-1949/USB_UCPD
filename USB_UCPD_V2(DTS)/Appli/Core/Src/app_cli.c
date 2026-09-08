#include "app_cli.h"
#include "app_log.h"
#include "app_pd.h"
#include "app_board.h"
#include "apie.h"
#include "apie_analyzer.h"
#include "apie_unknown.h"
#include "apie_db.h"
#include "apie_selftest.h"
#include "usbpd_dpm_user.h"
#include "usbpd_core.h"
#include "ina226.h"
#include "ext_uart.h"
#include "ext_dts.h"
#include "dtsmon.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define CLI_LINE_MAX  96U
#define CLI_RX_MAX    256U

static char     s_line[CLI_LINE_MAX];
static uint16_t s_len;
static uint8_t  s_rx[CLI_RX_MAX];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static uint8_t  s_greeted;

/* ==========================================================================
 *  USART2 CONSOLE BRIDGE
 *  --------------------------------------------------------------------------
 *  USART2 (PD5 TX / PD6 RX) is a second, fully equivalent console:
 *    - output: app_log.c mirrors every printed byte to it (see `console`)
 *    - input : the two hooks below feed the very same line editor that the
 *              USB-HS CDC port feeds, so either port can drive the MCU
 *  Both ports work at the same time; commands typed on one are answered on
 *  both.
 * ========================================================================== */

void EXT_UART_FeatureInit(void)
{
  /* Send one line straight out of the peripheral, bypassing the log queue
   * entirely.  If this arrives but the console does not, the fault is in the
   * queue path; if nothing arrives at all, it is the pin, the wiring or the
   * adapter.  `uart selftest` dumps the hardware state either way. */
  (void)EXT_UART_Printf("\r\n[USART2 PD5 TX OK - 115200 8N1]\r\n");

  /* Arm 1-byte interrupt reception so console input does not have to be
   * polled.  HAL_UART_ErrorCallback (ext_uart.c) re-arms after a line
   * error, so a framing glitch cannot kill the port. */
  if (EXT_UART_ReceiveByteIT() != HAL_OK)
  {
    APP_LOG_Write("console: USART2 RX could not be armed\r\n");
    return;
  }
  APP_LOG_Write("console: USART2 is live (PD5 TX / PD6 RX, 115200 8N1)\r\n");
}

void EXT_UART_RxByteReceived(uint8_t b)
{
  /* Interrupt context: only queue the byte, the super loop parses it. */
  APP_CLI_OnRx(&b, 1U);
}

void APP_CLI_Init(void)
{
  s_len = 0;
  s_rx_head = 0;
  s_rx_tail = 0;
  s_greeted = 0;
}

void APP_CLI_OnRx(const uint8_t *data, uint32_t len)
{
  uint32_t i;

  if (data == NULL)
  {
    return;
  }
  for (i = 0; i < len; i++)
  {
    uint16_t next;
    uint32_t primask;

    /* Two producers run in interrupt context at different priorities:
     * OTG_HS (4) for the CDC port and USART2 (7) for the serial console.
     * Without this guard the higher-priority one can preempt the other
     * between the read and the write of s_rx_head and corrupt the ring.
     * The critical section is a handful of instructions per byte, so it
     * cannot measurably delay USB or the PD stack. */
    primask = __get_PRIMASK();
    __disable_irq();

    next = (uint16_t)((s_rx_head + 1U) % CLI_RX_MAX);
    if (next != s_rx_tail)
    {
      s_rx[s_rx_head] = data[i];
      s_rx_head = next;
    }

    __set_PRIMASK(primask);
  }
}

void APP_CLI_PrintHelp(void)
{
  APP_LOG_Write(
    "Commands (type one, then Enter):\r\n"
    "\r\n"
    "  -- ask the source for power --\r\n"
    "  caps                   list the power levels the source offers\r\n"
    "  req <n> [ma]           ask for fixed level n (1 = first) at this current\r\n"
    "  volt <mv> [ma]         ask for the closest level (fixed or PPS) to this voltage\r\n"
    "  pps <mv> [ma]          ask for an exact PPS voltage (20 mV steps) and current\r\n"
    "\r\n"
    "  -- automations --\r\n"
    "  auto <mv> [ma]         automatically ask for this voltage after attach\r\n"
    "  auto off               go back to the default 5 V on attach\r\n"
    "  remember on|off        re-apply your last request after the next attach\r\n"
    "  sweep <from> <to> <step> [ma]   step the PPS voltage from..to (mV)\r\n"
    "  sweep stop             stop a running sweep\r\n"
    "\r\n"
    "  -- output monitor (INA226 on I2C2) --\r\n"
    "  ina | ina226           one-shot output V / I / P reading\r\n"
    "  ina auto on|off        periodic reading every second (default on)\r\n"
    "  ina period <ms>        change the periodic reading interval\r\n"
    "  ina addr <hex>         use a different INA226 address (default 40)\r\n"
    "  ina scan               scan the I2C2 bus (0x08-0x77)\r\n"
    "  ina vbus real|synth    feed the PD stack real INA226 vbus\r\n"
    "\r\n"
    "  -- read details from the source --\r\n"
    "  status                 what is connected and what we asked for\r\n"
    "  getstatus              read the source's status (temperature, faults)\r\n"
    "  getpps                 read the source's real output voltage and current\r\n"
    "  srcext                 read extended info (VID/PID, firmware, max power)\r\n"
    "  manuinfo               read the manufacturer name and IDs\r\n"
    "  battery                read battery capability and status (if it has one)\r\n"
    "  countrycodes           list the country codes the source supports\r\n"
    "  countryinfo <XX>       read country info (XX = two letters, e.g. US)\r\n"
    "  identify               ask the source to identify itself (VDM)\r\n"
    "  svids                  list alternate-mode SVIDs the source supports\r\n"
    "  modes <svid>           list the modes of an SVID (e.g. modes ff01)\r\n"
    "\r\n"
    "  -- control / tools --\r\n"
    "  getcaps                re-read the source's power levels\r\n"
    "  softreset / hardreset  PD soft / hard reset\r\n"
    "  pd                     UCPD registers + PHY counters\r\n"
    "  info                   board / memory / clocks\r\n"
    "\r\n"
    "  -- consoles (USB-HS CDC and USART2 run together) --\r\n"
    "  console                which console is live, and where output goes\r\n"
    "  console usb on|off     mirror output to the USB-HS CDC port\r\n"
    "  console uart on|off    mirror output to USART2 (PD5 TX / PD6 RX)\r\n"
    "  console both on|off    mirror output to both (the default)\r\n"
    "  uart <text>            send <text> straight out of USART2\r\n"
    "  uart rx [ms]           dump what USART2 received\r\n"
    "  uart selftest          USART2 register / clock / GPIO / NVIC dump\r\n"
    "\r\n"
    "  -- SoC temperature (on-die DTS) --\r\n"
    "  dts                    one-shot SoC temperature\r\n"
    "  dts auto on|off        periodic reading every second (default on)\r\n"
    "  dts period <ms>        change the periodic reading interval\r\n"
    "  dts unit c|f           report in degrees C or degrees F\r\n"
    "  led on|off|hb          LED override\r\n"
    "  help | ?               this list\r\n"
    "\r\n"
    "  -- Advanced PD Intelligence Engine (APIE) --\r\n"
    "  ap | apie              intelligence status (state, safe, exp level)\r\n"
    "  ap <sub>               alias for any intelligence sub-command below\r\n"
    "  ap status|stats        status / analyzer+txn+unknown counters\r\n"
    "  ap raw|packets [all]   dump the bounded raw packet ring\r\n"
    "  ap source|profile|fingerprint   source profile / fingerprint\r\n"
    "  ap txn                 list completed/active transactions\r\n"
    "  ap feature             print the feature vector\r\n"
    "  ap unknown|patterns|hypotheses  unrecognized/recurring behavior\r\n"
    "  ap knowledge|db        knowledge database status + validation\r\n"
    "  ap scheduler           adaptive query scheduler state\r\n"
    "  ap ml                  model status + online learning counters\r\n"
    "  ap predict <q>         classify a query (0-8) as useful or not\r\n"
    "  ap experiment [set <0-4>]  experiment level (default 2)\r\n"
    "  ap replay              host-side replay pointer (no live transmit)\r\n"
    "  ap safety              safety limits + hardware capability flags\r\n"
    "  selftest [scope]       one-command non-destructive self-test\r\n"
    "  selftest [all|quick|full|pd|decoder|ml|database|flash]  scoped\r\n"
    "  packets [raw|decoded|unknown|tx|rx] [all]  packet ring views\r\n"
    "  transactions [active|history]  transaction views\r\n"
    "  safe | safe-mode on|off  disable/enable intelligence (PD sink stays up)\r\n"
    "  pd stats | packets [all] | state   PD PHY/PE counters + raw ring\r\n"
    "  db [dump|status]       knowledge database status\r\n"
    "  db validate            CRC-validate every stored profile\r\n"
    "  db compact             compact/re-index the store\r\n"
    "  db test                scratch store/readback self-test\r\n"
    "  db wear|writes|erases|checkpoint   flash-endurance accounting\r\n"
    "  raw [clear|dump [all]|stats|export]  raw packet ring, no ap prefix\r\n"

    "\r\n"

    "  Every \"ap <sub>\" above also works bare, without the ap prefix:\r\n"

    "    apie stats knowledge replay raw txn transactions source profile\r\n"

    "    profiles fingerprint feature features unknown patterns hypotheses\r\n"

    "    ml predict scheduler experiment safety db diag selftest safe\r\n"

    "  (bare \"status\" is the PD status above, not \"ap status\")\r\n"

    "\r\n"

    "  safety [status|limits] safety limits + hardware capability flags\r\n"
    "  diag pd|rx|tx|txn|ucpd|usb|queue|timing|cpu|memory|faults|decoder|\r\n"
    "       profile|unknown|knowledge|trace|ml|scheduler|packets|db|safety|flash\r\n"
    "\r\n"
    "wire a PD source to PM0 (CC1) or PM1 (CC2) plus GND. The USB Type-C port is this\r\n"
    "console; the USB-PD trace for STM32CubeMonitor-UCPD is on USART1 PA9/PA10 @921600.\r\n"
  );
}

void APP_CLI_PrintBanner(void)
{
  APP_LOG_Write(
    "\r\n"
    "========================================\r\n"
    "  PD Bench  —  STM32H7R3 UCPD sink\r\n"
    "  WeAct H7R3Z8  |  XiP PY25Q64HA\r\n"
    "========================================\r\n"
    "USB CDC up. Type help\r\n"
    "\r\n"
  );
  APP_CLI_PrintHelp();
  if (APP_PD_IsAttached())
  {
    APP_PD_PrintCaps();
  }
  else
  {
    APP_LOG_Write("waiting for PD source on CC1/CC2...\r\n");
  }
  INA226_PrintStatus();
  APP_LOG_Write("> ");
}

void APP_CLI_OnHostOpen(void)
{
  s_greeted = 0;
}

static void prompt(void)
{
  APP_LOG_Write("> ");
}

static int parse_u(const char *s, unsigned *out)
{
  char *end = NULL;
  unsigned long v;
  if ((s == NULL) || (*s == '\0'))
  {
    return -1;
  }
  v = strtoul(s, &end, 0);
  /* Reject trailing garbage: "5abc" used to be accepted as 5. */
  if ((end == s) || (end == NULL) || (*end != '\0'))
  {
    return -1;
  }
  *out = (unsigned)v;
  return 0;
}

static int parse_h(const char *s, unsigned *out)
{
  char *end = NULL;
  unsigned long v;
  if ((s == NULL) || (*s == '\0'))
  {
    return -1;
  }
  v = strtoul(s, &end, 16);
  if ((end == s) || (end == NULL) || (*end != '\0'))
  {
    return -1;
  }
  *out = (unsigned)v;
  return 0;
}

/* --------------------------------------------------------------------------
 *  APIE CLI dispatch: every command in the Advanced PD Intelligence Engine.
 * ------------------------------------------------------------------------- */
static int apie_cli_dispatch(int argc, char *argv[])
{
  if (argc < 1)
  {
    return -1;
  }

  /* `ap <sub>` is an alias for the same intelligence sub-commands. */
  if (strcmp(argv[0], "ap") == 0)
  {
    int i;
    if (argc < 2)
    {
      APIE_CliStatus();
      return 0;
    }
    for (i = 1; i < argc; i++)
    {
      argv[i - 1] = argv[i];
    }
    argc--;
    argv[argc] = NULL;
    return apie_cli_dispatch(argc, argv);
  }

  if (strcmp(argv[0], "apie") == 0 || strcmp(argv[0], "status") == 0)
  {
    APIE_CliStatus();
    return 0;
  }
  if (strcmp(argv[0], "stats") == 0)
  {
    APIE_CliCounters();
    return 0;
  }
  if (strcmp(argv[0], "packets") == 0)
  {
    if (argc >= 2 && strcmp(argv[1], "raw") == 0)
    {
      APIE_CliPdPackets((argc >= 3) && (strcmp(argv[2], "all") == 0));
      return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "decoded") == 0)
    {
      APIE_CliPdPackets((argc >= 3) && (strcmp(argv[2], "all") == 0));
      return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "unknown") == 0)
    {
      APIE_CliUnknown();
      return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "tx") == 0)
    {
      APP_LOG_Write("packets tx: see 'diag tx' / 'pd stats' for TX counters.\r\n");
      APIE_CliPdStats();
      return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "rx") == 0)
    {
      APP_LOG_Write("packets rx: see 'diag rx' / 'pd stats' for RX counters.\r\n");
      APIE_CliPdStats();
      return 0;
    }
    APIE_CliPdPackets((argc >= 2) && (strcmp(argv[1], "all") == 0));
    return 0;
  }
  if (strcmp(argv[0], "knowledge") == 0)
  {
    APIE_CliDbStatus();
    return 0;
  }
  if (strcmp(argv[0], "replay") == 0)
  {
    APP_LOG_Write("replay: host-side replay only via tools/apie_replay.py; no live PD transmit.\r\n");
    return 0;
  }
  if (strcmp(argv[0], "selftest") == 0)
  {
    /* one-command, non-destructive self-test.  Optional scope argument. */
    const char *scope = (argc >= 2) ? argv[1] : NULL;
    if (scope != NULL &&
        strcmp(scope, "all") != 0 && strcmp(scope, "quick") != 0 &&
        strcmp(scope, "full") != 0 && strcmp(scope, "pd") != 0 &&
        strcmp(scope, "decoder") != 0 && strcmp(scope, "ml") != 0 &&
        strcmp(scope, "database") != 0 && strcmp(scope, "flash") != 0)
    {
      APP_LOG_Write("usage: selftest [all|quick|full|pd|decoder|ml|database|flash]\r\n");
      return 0;
    }
    APIE_SelfTest_Run(scope);
    return 0;
  }
  if (strcmp(argv[0], "safe-mode") == 0 || strcmp(argv[0], "safe") == 0)
  {
    if ((argc >= 2) && (strcmp(argv[1], "on") == 0))
    {
      APIE_SetSafeMode(1U);
      APP_LOG_Write("safe-mode ON: intelligence disabled, PD sink keeps running.\r\n");
    }
    else if ((argc >= 2) && (strcmp(argv[1], "off") == 0))
    {
      APIE_SetSafeMode(0U);
      APP_LOG_Write("safe-mode OFF: intelligence enabled.\r\n");
    }
    else
    {
      APP_LOG_Printf("safe-mode: %s\r\n", APIE_IsSafeMode() ? "ON" : "off");
      APP_LOG_Write("usage: safe-mode on|off\r\n");
    }
    return 0;
  }
  if (strcmp(argv[0], "pd") == 0 && argc >= 2)
  {
    if (strcmp(argv[1], "stats") == 0)
    {
      APIE_CliPdStats();
      return 0;
    }
    if (strcmp(argv[1], "packets") == 0)
    {
      APIE_CliPdPackets((argc >= 3) && (strcmp(argv[2], "all") == 0));
      return 0;
    }
    if (strcmp(argv[1], "state") == 0)
    {
      APIE_CliStatus();
      return 0;
    }
    return -1;
  }
  if (strcmp(argv[0], "raw") == 0)
  {
    if (argc < 2)
    {
      APIE_CliPdPackets(0);
      return 0;
    }
    if (strcmp(argv[1], "clear") == 0)
    {
      APIE_Analyzer_Clear();
      APP_LOG_Write("raw ring cleared.\r\n");
      return 0;
    }
    if (strcmp(argv[1], "dump") == 0)
    {
      APIE_CliPdPackets((argc >= 3) && (strcmp(argv[2], "all") == 0));
      return 0;
    }
    if (strcmp(argv[1], "stats") == 0)
    {
      APIE_CliCounters();
      return 0;
    }
    if (strcmp(argv[1], "export") == 0)
    {
      APP_LOG_Write("raw: capture export (replayable)\r\n");
      APIE_Analyzer_Export();
      return 0;
    }
    APP_LOG_Write("raw: usage raw [clear|dump|stats|export]\r\n");
    return 0;
  }
  if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], "profile") == 0 ||
      strcmp(argv[0], "profiles") == 0 || strcmp(argv[0], "fingerprint") == 0)
  {
    if (strcmp(argv[0], "fingerprint") == 0)
    {
      APIE_CliFingerprint();
      return 0;
    }
    APIE_CliSource();
    return 0;
  }
  if (strcmp(argv[0], "txn") == 0 || strcmp(argv[0], "transactions") == 0)
  {
    if (strcmp(argv[0], "transactions") == 0 && argc >= 2)
    {
      if (strcmp(argv[1], "active") == 0)
      {
        APP_LOG_Printf("transactions active: %u\r\n", (unsigned)APIE_Txn_ActiveCount());
        return 0;
      }
      if (strcmp(argv[1], "history") == 0)
      {
        APIE_CliTxnList();
        return 0;
      }
    }
    APIE_CliTxnList();
    return 0;
  }
  if (strcmp(argv[0], "feature") == 0 || strcmp(argv[0], "features") == 0)
  {
    APIE_CliFeature();
    return 0;
  }
  if (strcmp(argv[0], "unknown") == 0 || strcmp(argv[0], "patterns") == 0 ||
      strcmp(argv[0], "hypotheses") == 0)
  {
    APIE_CliUnknown();
    return 0;
  }
  if (strcmp(argv[0], "ml") == 0)
  {
    APIE_CliMlStatus();
    return 0;
  }
  if (strcmp(argv[0], "predict") == 0)
  {
    APIE_CliPredict((argc >= 2) ? argv[1] : (char *)"0");
    return 0;
  }
  if (strcmp(argv[0], "scheduler") == 0)
  {
    APIE_CliScheduler();
    return 0;
  }
  if (strcmp(argv[0], "experiment") == 0)
  {
    if ((argc >= 3) && (strcmp(argv[1], "set") == 0))
    {
      unsigned lvl = 0U;
      if ((parse_u(argv[2], &lvl) != 0) || (lvl > 4U))
      {
        APP_LOG_Write("usage: experiment set <0-4>\r\n");
      }
      else
      {
        APIE_SetExperimentLevel((uint8_t)lvl);
        APP_LOG_Printf("experiment level set to %u\r\n", (unsigned)lvl);
      }
    }
    else
    {
      APIE_CliExperiment();
    }
    return 0;
  }
  if (strcmp(argv[0], "db") == 0)
  {
    if (argc >= 2)
    {
      if ((strcmp(argv[1], "dump") == 0) || (strcmp(argv[1], "status") == 0))
      {
        APIE_CliDbStatus();
        return 0;
      }
      if (strcmp(argv[1], "validate") == 0)
      {
        APP_LOG_Printf("db validate: %s\r\n", APIE_Db_ValidateAll() ? "OK" : "CORRUPT / empty");
        return 0;
      }
      if (strcmp(argv[1], "compact") == 0)
      {
        APP_LOG_Printf("db compact: %u profile(s)\r\n", (unsigned)APIE_Db_Compact());
        return 0;
      }
      if (strcmp(argv[1], "test") == 0)
      {
        APP_LOG_Printf("db test: %s\r\n", APIE_Db_SelfTest() ? "OK (scratch store/readback/validate)" : "FAILED");
        return 0;
      }
      if ((strcmp(argv[1], "wear") == 0) || (strcmp(argv[1], "writes") == 0) ||
          (strcmp(argv[1], "erases") == 0) || (strcmp(argv[1], "checkpoint") == 0))
      {
        APIE_DbCounters_t c;
        APIE_Db_GetCounters(&c);
        APP_LOG_Printf("db %s: checkpoints=%lu writes=%lu erases=%lu wear=%lu compact=%lu selftest=%lu persist=%s\r\n",
                       argv[1], (unsigned long)c.checkpoints, (unsigned long)c.writes,
                       (unsigned long)c.erases, (unsigned long)c.wear,
                       (unsigned long)c.compacts, (unsigned long)c.self_tests,
                       c.nor_persist ? "NOR-ACTIVE" : "RAM (NOR off: XIP)");
        return 0;
      }
    }
    APIE_CliDbStatus();
    return 0;
  }
  if (strcmp(argv[0], "safety") == 0)
  {
    if ((argc >= 2) && ((strcmp(argv[1], "status") == 0) || (strcmp(argv[1], "limits") == 0)))
    {
      APIE_CliSafety();
      return 0;
    }
    APIE_CliSafety();
    return 0;
  }
  if (strcmp(argv[0], "diag") == 0)
  {
    if (argc >= 2)
    {
      if (strcmp(argv[1], "pd") == 0 || strcmp(argv[1], "rx") == 0 ||
          strcmp(argv[1], "tx") == 0 || strcmp(argv[1], "txn") == 0)
      {
        APIE_CliPdStats();
        return 0;
      }
      if (strcmp(argv[1], "decoder") == 0)
      {
        APP_LOG_Write("diag decoder: run 'selftest decoder' for deterministic decoder checks.\r\n");
        return 0;
      }
      if (strcmp(argv[1], "profile") == 0)
      {
        APIE_CliSource();
        return 0;
      }
      if (strcmp(argv[1], "unknown") == 0)
      {
        APIE_CliUnknown();
        return 0;
      }
      if (strcmp(argv[1], "knowledge") == 0)
      {
        APIE_CliDbStatus();
        return 0;
      }
      if (strcmp(argv[1], "ucpd") == 0)
      {
        APP_BOARD_PrintUcpd();
        return 0;
      }
      if (strcmp(argv[1], "usb") == 0)
      {
        APP_PD_PrintStatus();
        return 0;
      }
      if (strcmp(argv[1], "queue") == 0)
      {
        APP_LOG_Printf("active txn=%u txn_hist=%u unknown=%u raw=%u\r\n",
                       (unsigned)APIE_Txn_ActiveCount(),
                       (unsigned)APIE_Txn_HistoryCount(),
                       (unsigned)APIE_Unknown_Count(),
                       (unsigned)APIE_Analyzer_Count());
        return 0;
      }
      if (strcmp(argv[1], "timing") == 0)
      {
        APP_LOG_Printf("diag timing: task_calls=%lu period_max=%lu ms period_avg=%lu ms dwt=%s\r\n",
                       (unsigned long)APIE_Diag_TaskCalls(),
                       (unsigned long)APIE_Diag_TaskPeriodMaxMs(),
                       (unsigned long)APIE_Diag_TaskPeriodAvgMs(),
                       APIE_Diag_DwtReady() ? "on" : "off");
        return 0;
      }
      if (strcmp(argv[1], "cpu") == 0)
      {
        APP_LOG_Printf("diag cpu: apiE max per-call compute budget = %lu cycles (dwt %s)\r\n",
                       (unsigned long)APIE_Diag_DwtCyclesMax(),
                       APIE_Diag_DwtReady() ? "on" : "off");
        return 0;
      }
      if (strcmp(argv[1], "memory") == 0)
      {
        APP_BOARD_PrintInfo();
        return 0;
      }
      if (strcmp(argv[1], "ml") == 0)
      {
        APIE_CliMlStatus();
        return 0;
      }
      if (strcmp(argv[1], "scheduler") == 0)
      {
        APIE_CliScheduler();
        return 0;
      }
      if (strcmp(argv[1], "packets") == 0)
      {
        APIE_CliPdPackets((argc >= 3) && (strcmp(argv[2], "all") == 0));
        return 0;
      }
      if (strcmp(argv[1], "db") == 0)
      {
        APIE_CliDbStatus();
        return 0;
      }
      if (strcmp(argv[1], "safety") == 0)
      {
        APIE_CliSafety();
        return 0;
      }
      if (strcmp(argv[1], "faults") == 0)
      {
        APIE_CliStatus();
        return 0;
      }
      if (strcmp(argv[1], "trace") == 0)
      {
        APP_LOG_Write("diag trace: USBPD tracer on USART1 PA9/PA10 @921600 (see CUBEMONITOR_UCPD.md)\r\n");
        return 0;
      }
      if (strcmp(argv[1], "flash") == 0)
      {
        APIE_DbCounters_t c;
        APIE_Db_GetCounters(&c);
        APP_LOG_Printf("diag flash: NOR persistence %s; erases=%lu wear=%lu checkpoints=%lu\r\n",
                       c.nor_persist ? "ACTIVE" : "DISABLED (XIP safety)",
                       (unsigned long)c.erases, (unsigned long)c.wear,
                       (unsigned long)c.checkpoints);
        return 0;
      }
    }
    APIE_CliStatus();
    return 0;
  }
  return -1;
}

static void handle_line(char *line)
{
  char *argv[6];
  int argc = 0;
  char *p = line;

  while (*p && (argc < 6))
  {
    while (*p && isspace((unsigned char)*p)) { p++; }
    if (*p == '\0') { break; }
    argv[argc++] = p;
    while (*p && !isspace((unsigned char)*p)) { p++; }
    if (*p) { *p++ = '\0'; }
  }
  if (argc == 0)
  {
    prompt();
    return;
  }

  if ((strcmp(argv[0], "help") == 0) || (strcmp(argv[0], "?") == 0))
  {
    APP_CLI_PrintHelp();
  }
  else if (strcmp(argv[0], "status") == 0)
  {
    APP_PD_PrintStatus();
    INA226_PrintStatus();
    DTSMON_PrintStatus();
    APIE_CliStatus();
  }
  else if ((strcmp(argv[0], "ina") == 0) || (strcmp(argv[0], "ina226") == 0))
  {
    INA226_Cli(argc, argv);
  }
  else if (strcmp(argv[0], "info") == 0)
  {
    APP_BOARD_PrintInfo();
  }
  else if (strcmp(argv[0], "pd") == 0)
  {
    if (argc == 1)
    {
      APP_BOARD_PrintUcpd();
    }
    else if (apie_cli_dispatch(argc, argv) != 0)
    {
      APP_LOG_Write("pd: usage pd | pd stats | pd packets [all] | pd state\r\n");
    }
  }
  else if (strcmp(argv[0], "caps") == 0)
  {
    APP_PD_PrintCaps();
  }
  else if (strcmp(argv[0], "getcaps") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetSourceCapability(0);
    APP_LOG_Printf("Asked the source for its power levels (%s).\r\n",
                   (st == USBPD_OK) ? "sent" : "the stack refused");
    if (st == USBPD_OK)
    {
      APP_PD_RequestCapsPrint();   /* print the reply even if it did not change */
    }
  }
  else if (strcmp(argv[0], "req") == 0)
  {
    unsigned n = 0;
    unsigned ma = 0;
    if ((argc < 2) || (parse_u(argv[1], &n) != 0) || (n == 0U) || (n > 7U))
    {
      APP_LOG_Write("usage: req <1-7> [current in mA]\r\n");
    }
    else
    {
      if (argc >= 3) { (void)parse_u(argv[2], &ma); }
      (void)APP_PD_SendRequest(0, (uint8_t)n, 0, (uint16_t)ma);
    }
  }
  else if (strcmp(argv[0], "volt") == 0)
  {
    unsigned mv = 0;
    unsigned ma = 0;
    uint8_t idx = 0;
    uint8_t is_pps = 0;
    if ((argc < 2) || (parse_u(argv[1], &mv) != 0))
    {
      APP_LOG_Write("usage: volt <mv> [current in mA]\r\n");
    }
    else
    {
      if (argc >= 3) { (void)parse_u(argv[2], &ma); }
      if (APP_PD_FindBestPdo(mv, &idx, &is_pps) == 0U)
      {
        APP_LOG_Write("No power level covers that voltage (type 'caps' to see the list).\r\n");
      }
      else
      {
        APP_LOG_Printf("Asking for %u mV%s...\r\n", (unsigned)mv,
                       (is_pps != 0U) ? " via PPS" : "");
        (void)APP_PD_SendRequest(0, idx, (uint16_t)mv, (uint16_t)ma);
      }
    }
  }
  else if (strcmp(argv[0], "pps") == 0)
  {
    unsigned mv = 0;
    unsigned ma = 0;
    uint8_t idx = 0;
    uint8_t is_pps = 0;
    if ((argc < 2) || (parse_u(argv[1], &mv) != 0))
    {
      APP_LOG_Write("usage: pps <mv> [current in mA]\r\n");
    }
    else
    {
      if (argc >= 3) { (void)parse_u(argv[2], &ma); }
      if ((APP_PD_FindBestPdo(mv, &idx, &is_pps) == 0U) || (is_pps == 0U))
      {
        APP_LOG_Write("The source has no PPS range covering that voltage.\r\n");
      }
      else
      {
        (void)APP_PD_SendRequest(0, idx, (uint16_t)mv, (uint16_t)ma);
      }
    }
  }
  else if (strcmp(argv[0], "auto") == 0)
  {
    if ((argc >= 2) && (strcmp(argv[1], "off") == 0))
    {
      APP_PD_SetAuto(0U, 0U);
      APP_LOG_Write("Auto request is now off. The default 5 V will be used after attach.\r\n");
    }
    else
    {
      unsigned mv = 0;
      unsigned ma = 0;
      if ((argc < 2) || (parse_u(argv[1], &mv) != 0) || (mv == 0U))
      {
        APP_LOG_Write("usage: auto <mv> [ma]   or   auto off\r\n");
      }
      else
      {
        if (argc >= 3) { (void)parse_u(argv[2], &ma); }
        APP_PD_SetAuto(mv, ma);
        APP_LOG_Printf("Auto request set to %u mV%s. It will be applied after the next attach.\r\n",
                       (unsigned)mv, (ma != 0U) ? " (current limited)" : "");
      }
    }
  }
  else if (strcmp(argv[0], "remember") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Printf("Remember mode is %s.\r\n", APP_PD_GetRemember() ? "on" : "off");
    }
    else if (strcmp(argv[1], "on") == 0)
    {
      APP_PD_SetRemember(1U);
      APP_LOG_Write("Remember mode is on: your last request will be re-applied after re-connect.\r\n");
    }
    else
    {
      APP_PD_SetRemember(0U);
      APP_LOG_Write("Remember mode is off.\r\n");
    }
  }
  else if (strcmp(argv[0], "sweep") == 0)
  {
    if ((argc >= 2) && (strcmp(argv[1], "stop") == 0))
    {
      APP_PD_StopSweep();
      APP_LOG_Write("Sweep stopped.\r\n");
    }
    else
    {
      unsigned from = 0, to = 0, step = 0, ma = 0;
      if ((argc < 4) || (parse_u(argv[1], &from) != 0) ||
          (parse_u(argv[2], &to) != 0) || (parse_u(argv[3], &step) != 0) ||
          (from == 0U) || (to < from) || (step == 0U))
      {
        APP_LOG_Write("usage: sweep <from_mV> <to_mV> <step_mV> [current mA]   or   sweep stop\r\n");
        APP_LOG_Write("example: sweep 5000 20000 500 3000\r\n");
      }
      else
      {
        if (argc >= 5) { (void)parse_u(argv[4], &ma); }
        if (APP_PD_IsAttached() == 0U)
        {
          APP_LOG_Write("Connect a PD source first (PM0/PM1 + GND), then start the sweep.\r\n");
        }
        else
        {
          APP_PD_StartSweep(from, to, step, ma);
          APP_LOG_Printf("Starting a PPS sweep from %u mV to %u mV in %u mV steps.\r\n",
                         (unsigned)from, (unsigned)to, (unsigned)step);
        }
      }
    }
  }
  else if (strcmp(argv[0], "getstatus") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetStatus(0);
    APP_LOG_Printf("Asked the source for its status (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused (is there a contract?)");
  }
  else if (strcmp(argv[0], "getpps") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetPPS_Status(0);
    APP_LOG_Printf("Asked the source for its real output voltage/current (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused (need a PPS contract?)");
  }
  else if (strcmp(argv[0], "srcext") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetSourceCapabilityExt(0);
    APP_LOG_Printf("Asked for extended source info (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "manuinfo") == 0)
  {
    USBPD_GMIDB_TypeDef req;
    req.ManufacturerInfoTarget = USBPD_MANUFINFO_TARGET_PORT_CABLE_PLUG;
    req.ManufacturerInfoRef = 0U;
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetManufacturerInfo(0, USBPD_SOPTYPE_SOP, (uint8_t *)&req);
    APP_LOG_Printf("Asked for the manufacturer information (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "battery") == 0)
  {
    uint8_t ref = 0U;
    USBPD_StatusTypeDef st1 = USBPD_DPM_RequestGetBatteryCapability(0, &ref);
    USBPD_StatusTypeDef st2 = USBPD_DPM_RequestGetBatteryStatus(0, &ref);
    APP_LOG_Printf("Asked for battery capability and status (%s / %s).\r\n",
                   (st1 == USBPD_OK) ? "capability sent" : "capability refused",
                   (st2 == USBPD_OK) ? "status sent" : "status refused");
  }
  else if (strcmp(argv[0], "countrycodes") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestGetCountryCodes(0);
    APP_LOG_Printf("Asked for the country codes (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "countryinfo") == 0)
  {
    if ((argc < 2) || (argv[1][0] == '\0') || (argv[1][1] == '\0'))
    {
      APP_LOG_Write("usage: countryinfo <XX>   (two letters, e.g. US)\r\n");
    }
    else
    {
      uint16_t cc = (uint16_t)(((uint16_t)(uint8_t)argv[1][0] << 8U) | (uint16_t)(uint8_t)argv[1][1]);
      USBPD_StatusTypeDef st = USBPD_DPM_RequestGetCountryInfo(0, cc);
      APP_LOG_Printf("Asked for country info for %c%c (%s).\r\n",
                     argv[1][0], argv[1][1],
                     (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
    }
  }
  else if (strcmp(argv[0], "identify") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestVDM_DiscoveryIdentify(0, USBPD_SOPTYPE_SOP);
    APP_LOG_Printf("Asked the source to identify itself (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
    APP_LOG_Write("(Some sources NAK this from a sink; use 'srcext'/'manuinfo' for a more reliable answer.)\r\n");
  }
  else if (strcmp(argv[0], "svids") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestVDM_DiscoverySVID(0, USBPD_SOPTYPE_SOP);
    APP_LOG_Printf("Asked for the alternate-mode SVID list (%s).\r\n",
                   (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
  }
  else if (strcmp(argv[0], "modes") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Write("usage: modes <svid>   (e.g. modes ff01)\r\n");
    }
    else
    {
      unsigned svid = 0;
      if (parse_h(argv[1], &svid) != 0)
      {
        APP_LOG_Write("usage: modes <svid>   (e.g. modes ff01)\r\n");
      }
      else
      {
        USBPD_StatusTypeDef st = USBPD_DPM_RequestVDM_DiscoveryMode(0, USBPD_SOPTYPE_SOP, (uint16_t)svid);
        APP_LOG_Printf("Asked for the modes of SVID 0x%04X (%s).\r\n",
                       (unsigned)svid, (st == USBPD_OK) ? "sent, waiting for the reply" : "the stack refused");
      }
    }
  }
  else if (strcmp(argv[0], "hardreset") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestHardReset(0);
    APP_LOG_Printf("HARD_RESET %s\r\n", (st == USBPD_OK) ? "sent" : "failed");
  }
  else if (strcmp(argv[0], "softreset") == 0)
  {
    USBPD_StatusTypeDef st = USBPD_DPM_RequestSoftReset(0, USBPD_SOPTYPE_SOP);
    APP_LOG_Printf("SOFT_RESET %s\r\n", (st == USBPD_OK) ? "sent" : "failed");
  }
  else if (strcmp(argv[0], "led") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Write("usage: led on|off|hb\r\n");
    }
    else if (strcmp(argv[1], "on") == 0) { APP_LED_Set(APP_LED_ON); }
    else if (strcmp(argv[1], "off") == 0) { APP_LED_Set(APP_LED_OFF); }
    else { APP_LED_Set(APP_LED_HEARTBEAT); }
  }
  else if (strcmp(argv[0], "console") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Printf("console: usb %s (%s)   uart %s (%s)\r\n",
                     APP_LOG_UsbMirror() ? "on " : "off",
                     APP_LOG_UsbReady() ? "host connected" : "no host yet",
                     APP_LOG_UartMirror() ? "on " : "off",
                     EXT_UART_IsRxArmed() ? "rx armed" : "rx idle");
      APP_LOG_Printf("         uart rx %lu byte(s) total, %lu still queued, "
                     "%lu lost (fifo full - 'uart rx' drains it)\r\n",
                     (unsigned long)EXT_UART_RxCount(),
                     (unsigned long)EXT_UART_RxFifoCount(),
                     (unsigned long)EXT_UART_RxDropped());
      APP_LOG_Printf("         console output queue: %lu byte(s) dropped\r\n",
                     (unsigned long)APP_LOG_Dropped());
      APP_LOG_Write("usage: console usb|uart|both on|off\r\n");
    }
    else
    {
      uint8_t on    = (argc < 3) || (strcmp(argv[2], "off") != 0);
      uint8_t valid = 1U;
      uint8_t touch_uart = 0U;
      uint8_t usb   = APP_LOG_UsbMirror();
      uint8_t uart  = APP_LOG_UartMirror();

      if (strcmp(argv[1], "usb") == 0)
      {
        usb = on;
      }
      else if (strcmp(argv[1], "uart") == 0)
      {
        uart = on;
        touch_uart = 1U;
      }
      else if (strcmp(argv[1], "both") == 0)
      {
        usb = on;
        uart = on;
        touch_uart = 1U;
      }
      else
      {
        APP_LOG_Write("usage: console usb|uart|both on|off\r\n");
        valid = 0U;
      }

      if ((valid != 0U) && (usb == 0U) && (uart == 0U))
      {
        /* Interlock: muting both sinks would make the firmware unreachable
         * until a reset.  Input still works, but nothing would answer. */
        APP_LOG_Write("refused: that would mute every console - "
                      "keep at least one output on\r\n");
        valid = 0U;
      }

      if (valid != 0U)
      {
        if (touch_uart != 0U)
        {
          /* Push the notice out *before* the sink state changes, so a port
           * that is being muted still gets to say goodbye. */
          APP_LOG_Printf("console: USART2 output %s\r\n",
                         on ? "on" : "off");
          APP_LOG_Flush();
        }
        APP_LOG_SetUsbMirror(usb);
        APP_LOG_SetUartMirror(uart);
        APP_LOG_Printf("console: usb output %s, uart output %s\r\n",
                       APP_LOG_UsbMirror() ? "on" : "off",
                       APP_LOG_UartMirror() ? "on" : "off");
      }
    }
  }
  else if (strcmp(argv[0], "uart") == 0)
  {
    if (argc < 2)
    {
      APP_LOG_Printf("uart: USART2 %s  %lu 8N1  TX=PD5 RX=PD6  last status=%d\r\n",
                     EXT_UART_IsReady() ? "ready" : "NOT initialised",
                     (unsigned long)huart2.Init.BaudRate,
                     (int)EXT_UART_LastStatus());
      APP_LOG_Write("usage: uart <text> | uart rx [ms] | uart selftest\r\n");
    }
    else if (strcmp(argv[1], "selftest") == 0)
    {
      /* Read-only hardware dump plus one transmit, so a port that produces
       * nothing can be told apart from one that produces garbage. */
      static const char kTxPattern[] =
        "uart selftest: 0123456789 abcdefghij KLMNOPQRST\r\n";
      uint32_t moder = GPIOD->MODER;
      uint32_t afr   = GPIOD->AFR[0];
      uint32_t isr   = USART2->ISR;
      uint32_t cr1   = USART2->CR1;
      uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

      APP_LOG_Printf("uart selftest: USART2 ready=%u  last HAL status=%d\r\n",
                     (unsigned)EXT_UART_IsReady(), (int)EXT_UART_LastStatus());
      APP_LOG_Printf("  clock : APB1ENR1.USART2EN=%u  kernel src=%lu (0=PCLK1)  "
                     "PCLK1=%lu Hz\r\n",
                     (unsigned)((RCC->APB1ENR1 & RCC_APB1ENR1_USART2EN) != 0U),
                     (unsigned long)__HAL_RCC_GET_USART234578_SOURCE(),
                     (unsigned long)pclk1);
      APP_LOG_Printf("  gpio  : PD5 MODER=%lu AFR=%lu   PD6 MODER=%lu AFR=%lu"
                     "   (want MODER=2 AFR=7)\r\n",
                     (unsigned long)((moder >> (5U * 2U)) & 3U),
                     (unsigned long)((afr   >> (5U * 4U)) & 0xFU),
                     (unsigned long)((moder >> (6U * 2U)) & 3U),
                     (unsigned long)((afr   >> (6U * 4U)) & 0xFU));
      APP_LOG_Printf("  nvic  : USART2_IRQn=%d enabled=%u priority=%ld\r\n",
                     (int)USART2_IRQn,
                     (unsigned)((NVIC->ISER[USART2_IRQn >> 5U] &
                                 (1UL << (USART2_IRQn & 0x1FU))) != 0U),
                     (long)NVIC_GetPriority(USART2_IRQn));
      APP_LOG_Printf("  regs  : CR1=%08lX UE=%lu TE=%lu RE=%lu RXNEIE=%lu\r\n",
                     (unsigned long)cr1,
                     (unsigned long)((cr1 & USART_CR1_UE) != 0U),
                     (unsigned long)((cr1 & USART_CR1_TE) != 0U),
                     (unsigned long)((cr1 & USART_CR1_RE) != 0U),
                     (unsigned long)((cr1 & USART_CR1_RXNEIE) != 0U));
      APP_LOG_Printf("          ISR=%08lX TXE=%lu TC=%lu RXNE=%lu  ORE=%lu FE=%lu NE=%lu\r\n",
                     (unsigned long)isr,
                     (unsigned long)((isr & USART_ISR_TXE_TXFNF) != 0U),
                     (unsigned long)((isr & USART_ISR_TC) != 0U),
                     (unsigned long)((isr & USART_ISR_RXNE_RXFNE) != 0U),
                     (unsigned long)((isr & USART_ISR_ORE) != 0U),
                     (unsigned long)((isr & USART_ISR_FE) != 0U),
                     (unsigned long)((isr & USART_ISR_NE) != 0U));
      APP_LOG_Printf("  baud  : BRR=%lu   PCLK1/115200=%lu   configured=%lu\r\n",
                     (unsigned long)USART2->BRR,
                     (unsigned long)((pclk1 + 57600U) / 115200U),
                     (unsigned long)huart2.Init.BaudRate);
      APP_LOG_Printf("  hal   : gState=%d RxState=%d ErrorCode=%08lX\r\n",
                     (int)huart2.gState, (int)huart2.RxState,
                     (unsigned long)huart2.ErrorCode);
      APP_LOG_Printf("  rx    : armed=%u total=%lu queued=%u dropped=%lu\r\n",
                     (unsigned)EXT_UART_IsRxArmed(),
                     (unsigned long)EXT_UART_RxCount(),
                     (unsigned)EXT_UART_RxFifoCount(),
                     (unsigned long)EXT_UART_RxDropped());
      APP_LOG_Printf("  console: usb mirror=%s(%s) uart mirror=%s  log dropped=%lu\r\n",
                     APP_LOG_UsbMirror() ? "on" : "off",
                     APP_LOG_UsbReady() ? "host up" : "no host",
                     APP_LOG_UartMirror() ? "on" : "off",
                     (unsigned long)APP_LOG_Dropped());
      APP_LOG_Printf("  tx    : %s\r\n",
                     (EXT_UART_Write((const uint8_t *)kTxPattern,
                                     (uint16_t)(sizeof(kTxPattern) - 1U)) == HAL_OK)
                       ? "HAL_OK, bytes accepted by the peripheral"
                       : "FAILED - see last HAL status above");
    }
    else if (strcmp(argv[1], "rx") == 0)
    {
      uint8_t buf[64];
      unsigned ms = 0U;
      uint16_t n;
      if ((argc >= 3) && (parse_u(argv[2], &ms) != 0))
      {
        APP_LOG_Write("usage: uart rx [ms]\r\n");
      }
      else
      {
        APP_LOG_Write("uart rx: (same bytes the CLI reads)\r\n");
        n = EXT_UART_Read(buf, (uint16_t)sizeof(buf), ms);
        APP_LOG_Printf("uart rx: %u byte(s)", (unsigned)n);
        for (uint16_t i = 0U; i < n; i++)
        {
          APP_LOG_Printf(" %02X", (unsigned)buf[i]);
        }
        APP_LOG_Write("\r\n");
      }
    }
    else
    {
      char out[CLI_LINE_MAX];
      size_t used = 0U;
      out[0] = '\0';
      for (int i = 1; i < argc; i++)
      {
        size_t l = strlen(argv[i]);
        if ((used + l + 2U) >= sizeof(out)) { break; }
        if (used > 0U) { out[used++] = ' '; }
        (void)memcpy(&out[used], argv[i], l);
        used += l;
      }
      out[used++] = '\r';
      out[used++] = '\n';
      APP_LOG_Printf("uart tx: %s\r\n",
                     (EXT_UART_Write((const uint8_t *)out, (uint16_t)used) == HAL_OK)
                       ? "sent" : "FAILED");
    }
  }
  else if (strcmp(argv[0], "dts") == 0)
  {
    DTSMON_Cli(argc, argv);
  }
  else if (apie_cli_dispatch(argc, argv) == 0)
  {
    /* handled by the Advanced PD Intelligence Engine CLI */
  }
  else
  {
    APP_LOG_Printf("unknown: %s  (help)\r\n", argv[0]);
  }
  prompt();
}

void APP_CLI_Poll(void)
{
  /* Banner for whichever console comes up first.  A UART-only session used
   * to stay silent because the greeting was gated on the USB DTR line. */
  if ((s_greeted == 0U) && (APP_LOG_UsbReady() || EXT_UART_IsRxArmed()))
  {
    s_greeted = 1U;
    APP_CLI_PrintBanner();
  }

  while (s_rx_tail != s_rx_head)
  {
    uint8_t c = s_rx[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % CLI_RX_MAX);

    if ((c == '\r') || (c == '\n'))
    {
      if (s_len > 0U)
      {
        s_line[s_len] = '\0';
        APP_LOG_Write("\r\n");
        handle_line(s_line);
        s_len = 0;
      }
      else if (c == '\r')
      {
        APP_LOG_Write("\r\n");
        prompt();
      }
      continue;
    }
    if ((c == 0x08U) || (c == 0x7FU))
    {
      if (s_len > 0U)
      {
        s_len--;
        APP_LOG_Write("\b \b");
      }
      continue;
    }
    if ((c >= 32U) && (c < 127U) && (s_len < (CLI_LINE_MAX - 1U)))
    {
      s_line[s_len++] = (char)c;
      APP_LOG_WriteRaw(&c, 1);
    }
  }
}
