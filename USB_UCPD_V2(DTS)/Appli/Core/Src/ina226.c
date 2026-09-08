/**
 * @file    ina226.c
 * @brief   INA226 output voltage / current monitor (I2C2, PB10/PB11).
 *
 * Registers on the INA226 (TI datasheet SBVS610):
 *   0x00 Configuration   0x01 Shunt Voltage  0x02 Bus Voltage
 *   0x03 Power           0x04 Current        0x05 Calibration
 *   0x06 Mask/Enable     0x07 Alert Limit
 *   0xFE Manufacturer ID (0x5449 = "TI")     0xFF Die ID (0x2260)
 *
 * Scaling for THIS module (5 milli-ohm shunt):
 *   Bus voltage LSB  = 1.25 mV   (fixed by the chip)
 *   Shunt volt LSB   = 2.5 uV    (fixed by the chip)
 *   Shunt current    = V_shunt / R_shunt = LSB * 2.5uV / 5mOhm
 *                     = 0.5 mA per LSB (exact, cal-independent)
 *   Calibration reg  = 0.00512 / (CurrentLSB * R_shunt)
 *                     = 0.00512 / (400uA * 5mOhm) = 2560 (0x0A00)
 *     so the chip's own Current register also reads 400 uA/LSB
 *     (+-13.1 A range, shunt +-81.92 mV saturates first at +-16.4 A).
 *
 * The driver plugs into the I2C2 extension footprint (ext_i2c.c) by
 * overriding the two weak hooks EXT_I2C_FeatureInit / EXT_I2C_FeaturePoll;
 * main.c does not need to know about it.
 *
 * If the chip is absent nothing blocks: every transfer uses a finite
 * timeout, absence is reported on the console, and the probe is retried
 * every few seconds.
 */
#include "ina226.h"
#include "ext_i2c.h"
#include "app_log.h"
#include "main.h"
#include <string.h>
#include <stdlib.h>

/* ---- registers ------------------------------------------------------------ */
#define INA226_REG_CONFIG       0x00U
#define INA226_REG_SHUNT        0x01U
#define INA226_REG_BUS          0x02U
#define INA226_REG_CAL          0x05U
#define INA226_REG_MASK_ENABLE  0x06U
#define INA226_REG_MFR_ID       0xFEU
#define INA226_REG_DIE_ID       0xFFU

#define INA226_MFR_ID_TI        0x5449U  /* "TI"  */
#define INA226_DIE_ID           0x2260U

/* ---- configuration -------------------------------------------------------- */
/* CFG register layout (INA226 datasheet SBVS610):
 *   [15:12] AVG      0010 = 16 averages
 *   [11:9]  VBUSCT   100  = 1.1 ms bus voltage conversion
 *   [8:6]   VSHCT    100  = 1.1 ms shunt voltage conversion
 *   [5:3]   reserved 000
 *   [2:0]   MODE     111  = CONTINUOUS shunt + bus measurements
 * -> 0010 100 100 000 111 = 0x2907.  One full conversion ~= 16*(1.1+1.1)
 * ms ~= 35 ms.
 *
 * BUGFIX: the previous value 0x7520 decoded to AVG=1024, VBUSCT=332us and
 * MODE=000 (POWER-DOWN).  After the driver's soft reset the chip briefly
 * converted on its default (continuous) config, then powered down with the
 * last conversion frozen in the registers - so every read returned the
 * same stale voltage/current until the module was physically power-cycled
 * (soft reset again -> one fresh conversion -> frozen again). */
#define INA226_CFG_VALUE        0x2907U

#define INA226_SHUNT_R_MOHM     5          /* 5 milli-ohm shunt            */
#define INA226_CURRENT_LSB_UA   400        /* 0.4 mA -> cal 2560           */
#define INA226_CAL_VALUE        (uint16_t)(5120000UL / \
        ((uint32_t)INA226_CURRENT_LSB_UA * (uint32_t)INA226_SHUNT_R_MOHM))

/* ---- timing --------------------------------------------------------------- */
#define INA226_I2C_TIMEOUT_MS   10    /* never block the super loop longer */
#define INA226_PROBE_PERIOD_MS  5000  /* re-probe while absent             */
#define INA226_SAMPLE_PERIOD_MS 250   /* measurement rate                  */
#define INA226_REPORT_PERIOD_MS 1000  /* default auto-report rate          */
#define INA226_BUS_RECOVER_N    3     /* consecutive bus errors -> reinit  */

/* ---- addresses ------------------------------------------------------------ */
#define INA226_DEFAULT_ADDR     0x40U
#define INA226_ADDR_MIN         0x40U
#define INA226_ADDR_MAX         0x43U

typedef struct
{
  uint8_t  present;        /* chip detected and configured                 */
  uint8_t  addr;           /* 7-bit I2C address in use                     */
  uint8_t  auto_report;    /* periodic console reporting on/off            */
  uint8_t  vbus_real;      /* 1 = PD stack reads INA226 as VBUS            */
  uint32_t report_ms;      /* periodic reporting interval                  */
  int32_t  bus_mv;
  int32_t  cur_ua;
  int32_t  pwr_mw;
  uint32_t meas_tick;      /* HAL_GetTick() of last good measurement       */
  uint32_t next_probe;
  uint32_t next_meas;
  uint32_t next_report;
  uint8_t  bus_errs;       /* consecutive HAL_BUSY/timeout failures        */
} INA226_State_t;

static INA226_State_t s;

/* ========================================================================== */
/*  Low level register access (finite timeout, big-endian 16-bit registers)   */
/* ========================================================================== */

static HAL_StatusTypeDef ina_read16(uint8_t reg, uint16_t *val)
{
  uint8_t d[2];
  HAL_StatusTypeDef st = EXT_I2C_ReadRegTO(s.addr, reg, d, 2U, INA226_I2C_TIMEOUT_MS);
  if (st != HAL_OK)
  {
    return st;
  }
  *val = (uint16_t)(((uint16_t)d[0] << 8) | (uint16_t)d[1]);
  return HAL_OK;
}

static HAL_StatusTypeDef ina_write16(uint8_t reg, uint16_t val)
{
  uint8_t d[2];
  d[0] = (uint8_t)(val >> 8);
  d[1] = (uint8_t)(val & 0xFFU);
  return EXT_I2C_WriteRegTO(s.addr, reg, d, 2U, INA226_I2C_TIMEOUT_MS);
}

/* If the I2C peripheral gets wedged (SDA held low by a half-written chip,
 * BUSY flag stuck) a peripheral re-init usually recovers it.  The GPIO
 * pull-ups keep the lines high so this is safe with no device attached. */
static void ina_bus_recover(const char *why)
{
  s.bus_errs = 0U;
  APP_LOG_Printf("[ina226] i2c bus recovery (%s)\r\n", why);
  (void)HAL_I2C_DeInit(&hi2c2);
  MX_I2C2_Init();
}

/* Read both measurement registers and recompute the engineering values. */
static void ina_sample(void)
{
  uint16_t bus = 0U;
  uint16_t shunt = 0U;
  HAL_StatusTypeDef bus_ok   = ina_read16(INA226_REG_BUS, &bus);
  HAL_StatusTypeDef shunt_ok = HAL_OK;

  if (bus_ok != HAL_OK)
  {
    goto sample_fail;
  }
  shunt_ok = ina_read16(INA226_REG_SHUNT, &shunt);
  if (shunt_ok != HAL_OK)
  {
    goto sample_fail;
  }

  s.bus_mv = ((int32_t)bus * 1250L) / 1000L;            /* 1.25 mV LSB  */
  s.cur_ua = (int32_t)(int16_t)shunt * 500L;            /* 0.5 mA LSB   */
  s.pwr_mw = (int32_t)(((int64_t)s.bus_mv * (int64_t)s.cur_ua) / 1000000LL);
  s.meas_tick = HAL_GetTick();
  s.bus_errs = 0U;
  return;

sample_fail:
  /* HAL_ERROR (address NACK) -> module unplugged, bus is fine.
     HAL_TIMEOUT / HAL_BUSY or a stuck HAL state -> wedged bus, recover it
     after INA226_BUS_RECOVER_N consecutive failures. */
  {
    HAL_StatusTypeDef st = (bus_ok != HAL_OK) ? bus_ok : shunt_ok;
    if ((st == HAL_TIMEOUT) || (st == HAL_BUSY) ||
        (hi2c2.State == HAL_I2C_STATE_BUSY))
    {
      s.bus_errs++;
      if (s.bus_errs >= INA226_BUS_RECOVER_N)
      {
        ina_bus_recover("stuck");
      }
    }
    else
    {
      s.bus_errs = 0U;
    }
  }
  s.present = 0U;
  s.meas_tick = 0U;              /* drop the stale measurement */
  s.vbus_real = 0U;              /* PD falls back to synthetic vbus */
  s.next_probe = HAL_GetTick() + INA226_PROBE_PERIOD_MS;
  APP_LOG_Write("[ina226] lost connection - no ina226 connected\r\n");
}

/* Probe one address: both ID registers must match a real INA226. */
static uint8_t ina_detect(uint8_t addr)
{
  uint16_t mfr = 0U;
  uint16_t die = 0U;
  uint8_t saved = s.addr;

  s.addr = addr;
  if ((ina_read16(INA226_REG_MFR_ID, &mfr) == HAL_OK) &&
      (ina_read16(INA226_REG_DIE_ID, &die) == HAL_OK) &&
      (mfr == INA226_MFR_ID_TI) && (die == INA226_DIE_ID))
  {
    return 1U;
  }
  s.addr = saved;
  return 0U;
}

static uint8_t ina_configure(void)
{
  uint16_t cfg_rd = 0xFFFFU;
  uint8_t attempt;
  uint8_t cfg_ok = 0U;

  /* Soft reset, then write our config, then read it back.  Several tries:
     clone modules on cheap breakouts sometimes need a little longer after
     the soft reset before they accept the configuration write. */
  for (attempt = 0U; attempt < 3U; attempt++)
  {
    if (ina_write16(INA226_REG_CONFIG, 0x8000U) != HAL_OK)
    {
      continue;                                   /* bus error: retry     */
    }
    HAL_Delay(5U);                                /* reset settle time    */
    if (ina_write16(INA226_REG_CONFIG, INA226_CFG_VALUE) != HAL_OK)
    {
      continue;
    }
    if (ina_read16(INA226_REG_CONFIG, &cfg_rd) != HAL_OK)
    {
      continue;
    }
    /* Hard requirement: MODE bits must read back continuous (111).  A
       power-down MODE is what froze all measurements (see BUGFIX above).
       Everything else (averaging, conversion times) is verified as a
       warning only - the reserved bits [5:3] read back non-zero on several
       clone INA226s, which must NOT be treated as "not connected". */
    if ((cfg_rd & 0x0007U) == 0x0007U)
    {
      cfg_ok = 1U;
      break;
    }
  }

  if (cfg_ok == 0U)
  {
    APP_LOG_Printf("[ina226] config failed: MODE not continuous "
                   "(cfg reads 0x%04X, want 0x%04X)\r\n",
                   (unsigned)cfg_rd, (unsigned)INA226_CFG_VALUE);
    return 0U;
  }
  if (cfg_rd != INA226_CFG_VALUE)
  {
    /* Works, but say what the chip kept: diagnostics, not an error. */
    APP_LOG_Printf("[ina226] note: cfg reads 0x%04X (want 0x%04X), "
                   "reserved bits differ - continuing\r\n",
                   (unsigned)cfg_rd, (unsigned)INA226_CFG_VALUE);
  }

  if (ina_write16(INA226_REG_MASK_ENABLE, 0x0000U) != HAL_OK)
  {
    return 0U;
  }
  if (ina_write16(INA226_REG_CAL, INA226_CAL_VALUE) != HAL_OK)
  {
    return 0U;
  }
  /* First conversion completes ~35 ms from now. */
  s.next_meas = HAL_GetTick() + 100U;
  s.meas_tick = 0U;              /* no valid measurement yet */
  s.next_report = HAL_GetTick() + s.report_ms;
  return 1U;
}

/* ========================================================================== */
/*  Extension footprint hooks (strong overrides of the weak hooks in ext_i2c) */
/* ========================================================================== */

void EXT_I2C_FeatureInit(void)
{
  uint8_t found = 0U;
  uint8_t a;

  memset(&s, 0, sizeof(s));
  s.addr = INA226_DEFAULT_ADDR;
  s.auto_report = 1U;
  s.report_ms = INA226_REPORT_PERIOD_MS;
  s.present = 0U;

  /* Probe the common INA226 addresses. */
  for (a = INA226_ADDR_MIN; a <= INA226_ADDR_MAX; a++)
  {
    if (ina_detect(a) != 0U)
    {
      found = 1U;
      break;
    }
  }

  if (found == 0U)
  {
    s.present = 0U;
    s.addr = INA226_DEFAULT_ADDR;
    s.next_probe = HAL_GetTick() + INA226_PROBE_PERIOD_MS;
    APP_LOG_Printf("[ina226] no ina226 connected "
                   "(probed 0x%02X-0x%02X, will retry every %u s; "
                   "check wiring or run 'ina scan')\r\n",
                   (unsigned)INA226_ADDR_MIN, (unsigned)INA226_ADDR_MAX,
                   (unsigned)(INA226_PROBE_PERIOD_MS / 1000U));
    return;
  }

  if (ina_configure() != 0U)
  {
    s.present = 1U;
    APP_LOG_Printf("[ina226] connected at 0x%02X  shunt %u mOhm  "
                   "cal %u (0x%04X)  avg16 ct1.1ms\r\n",
                   (unsigned)s.addr, (unsigned)INA226_SHUNT_R_MOHM,
                   (unsigned)INA226_CAL_VALUE, (unsigned)INA226_CAL_VALUE);
  }
  else
  {
    s.present = 0U;
    s.next_probe = HAL_GetTick() + INA226_PROBE_PERIOD_MS;
    APP_LOG_Write("[ina226] detected but configuration failed - "
                  "no ina226 connected\r\n");
  }
}

void EXT_I2C_FeaturePoll(void)
{
  uint32_t now = HAL_GetTick();

  if (s.present == 0U)
  {
    if ((int32_t)(now - s.next_probe) >= 0)
    {
      s.next_probe = now + INA226_PROBE_PERIOD_MS;
      if (ina_detect(s.addr) != 0U)
      {
        if (ina_configure() != 0U)
        {
          s.present = 1U;
          APP_LOG_Printf("[ina226] connected at 0x%02X\r\n", (unsigned)s.addr);
        }
      }
      else
      {
        /* Why did the probe fail?  A clean address NACK (HAL_ERROR) simply
           means the module is not there.  A TIMEOUT (or a HAL handle stuck
           BUSY) means the bus itself is wedged - e.g. SDA held low - and
           after a few of those the I2C peripheral is re-initialised to
           clear it; without this a wedged bus would print "not connected"
           for ever. */
        if ((HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(s.addr << 1), 1U,
                                   INA226_I2C_TIMEOUT_MS) == HAL_TIMEOUT) ||
            (hi2c2.State == HAL_I2C_STATE_BUSY))
        {
          s.bus_errs++;
          if (s.bus_errs >= INA226_BUS_RECOVER_N)
          {
            ina_bus_recover("probe");
          }
        }
        else
        {
          s.bus_errs = 0U;
        }
        /* Keep saying it while the module is absent. */
        APP_LOG_Write("[ina226] no ina226 connected\r\n");
      }
    }
    return;
  }

  if ((int32_t)(now - s.next_meas) >= 0)
  {
    s.next_meas = now + INA226_SAMPLE_PERIOD_MS;
    ina_sample();
  }

  if ((s.auto_report != 0U) && (s.meas_tick != 0U) &&
      ((int32_t)(now - s.next_report) >= 0))
  {
    s.next_report = now + s.report_ms;
    INA226_Print();
  }
}

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

uint8_t INA226_IsPresent(void)
{
  return s.present;
}

uint8_t INA226_GetAddr(void)
{
  return s.present ? s.addr : 0U;
}

int32_t INA226_GetBusMv(void)
{
  return s.bus_mv;
}

int32_t INA226_GetCurUa(void)
{
  return s.cur_ua;
}

int32_t INA226_GetPwrMw(void)
{
  return s.pwr_mw;
}

uint8_t INA226_DataFresh(void)
{
  return ((s.present != 0U) &&
          ((int32_t)(HAL_GetTick() - s.meas_tick) < (int32_t)INA226_SAMPLE_PERIOD_MS * 2)) ? 1U : 0U;
}

uint8_t INA226_VbusModeIsReal(void)
{
  return (uint8_t)((s.vbus_real != 0U) && (s.present != 0U));
}

void INA226_Print(void)
{
  if (s.present == 0U)
  {
    APP_LOG_Write("no ina226 connected\r\n");
    return;
  }
  APP_LOG_Printf("[ina226] out %ld.%03lu V   %s%ld.%01lu mA   %ld mW   (0x%02X)\r\n",
                 (long)(s.bus_mv / 1000),
                 (unsigned long)(((s.bus_mv % 1000) < 0 ? (-s.bus_mv % 1000) : (s.bus_mv % 1000))),
                 (s.cur_ua < 0) ? "-" : "",
                 (long)((s.cur_ua < 0 ? -s.cur_ua : s.cur_ua) / 1000),
                 (unsigned long)(((s.cur_ua < 0 ? -s.cur_ua : s.cur_ua) % 1000) / 100),
                 (long)s.pwr_mw,
                 (unsigned)s.addr);
}

void INA226_PrintStatus(void)
{
  if (s.present == 0U)
  {
    APP_LOG_Write("ina226: no ina226 connected (I2C2 PB10/PB11)\r\n");
    return;
  }
  APP_LOG_Printf("ina226: addr 0x%02X   out %ld mV   %ld mA   %ld mW   "
                 "vbus source: %s\r\n",
                 (unsigned)s.addr, (long)s.bus_mv,
                 (long)(s.cur_ua / 1000), (long)s.pwr_mw,
                 (s.vbus_real != 0U) ? "ina226 (real)" : "synthetic");
}

/* ========================================================================== */
/*  CLI                                                                       */
/* ========================================================================== */

static void ina_scan(void)
{
  uint8_t a;
  uint8_t found = 0U;
  APP_LOG_Write("i2c2 scan:\r\n");
  for (a = 0x08U; a <= 0x77U; a++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(a << 1), 2U,
                              INA226_I2C_TIMEOUT_MS) == HAL_OK)
    {
      APP_LOG_Printf("  0x%02X responds%s\r\n", (unsigned)a,
                     (a == s.addr) ? "  <- ina226" : "");
      found++;
    }
  }
  if (found == 0U)
  {
    APP_LOG_Write("  nothing found (no device acknowledges)\r\n");
  }
}

void INA226_Cli(int argc, char *argv[])
{
  if (argc < 2)
  {
    INA226_Print();
    return;
  }

  if (strcmp(argv[1], "auto") == 0)
  {
    if ((argc >= 3) && (strcmp(argv[2], "off") == 0))
    {
      s.auto_report = 0U;
      APP_LOG_Write("ina226 periodic reporting off\r\n");
    }
    else
    {
      s.auto_report = 1U;
      s.next_report = HAL_GetTick();
      APP_LOG_Write("ina226 periodic reporting on\r\n");
    }
  }
  else if (strcmp(argv[1], "period") == 0)
  {
    unsigned ms = 0U;
    if ((argc < 3) || (EXT_I2C_ParseU(argv[2], &ms) != 0) ||
        (ms < 250U) || (ms > 60000U))
    {
      APP_LOG_Write("usage: ina period <ms 250..60000>\r\n");
    }
    else
    {
      s.report_ms = ms;
      APP_LOG_Printf("ina226 reporting every %u ms\r\n", ms);
    }
  }
  else if (strcmp(argv[1], "addr") == 0)
  {
    unsigned a = 0U;
    char *end = NULL;
    if (argc < 3)
    {
      APP_LOG_Write("usage: ina addr <7-bit hex, e.g. 40>\r\n");
      return;
    }
    a = (unsigned)strtoul(argv[2], &end, 16);
    if ((end == argv[2]) || (end == NULL) || (*end != '\0') ||
        (a < 8U) || (a > 0x77U))
    {
      APP_LOG_Write("usage: ina addr <7-bit hex, e.g. 40>\r\n");
      return;
    }
    s.addr = (uint8_t)a;
    s.present = ina_detect(s.addr);
    if ((s.present != 0U) && (ina_configure() != 0U))
    {
      s.present = 1U;
      APP_LOG_Printf("ina226 connected at 0x%02X\r\n", (unsigned)s.addr);
    }
    else
    {
      s.present = 0U;
      s.next_probe = HAL_GetTick() + INA226_PROBE_PERIOD_MS;
      APP_LOG_Printf("no ina226 connected at 0x%02X - will keep retrying\r\n",
                     (unsigned)s.addr);
    }
  }
  else if (strcmp(argv[1], "scan") == 0)
  {
    ina_scan();
  }
  else if (strcmp(argv[1], "vbus") == 0)
  {
    if ((argc >= 3) && (strcmp(argv[2], "real") == 0))
    {
      if (s.present != 0U)
      {
        s.vbus_real = 1U;
        APP_LOG_Write("PD vbus source: ina226 measurement (real)\r\n");
      }
      else
      {
        APP_LOG_Write("no ina226 connected - cannot use real vbus\r\n");
      }
    }
    else
    {
      s.vbus_real = 0U;
      APP_LOG_Write("PD vbus source: synthetic (CC-only tester mode)\r\n");
    }
  }
  else
  {
    APP_LOG_Write(
      "usage: ina [auto on|off | period <ms> | addr <hex> | scan | vbus real|synth]\r\n");
  }
}
