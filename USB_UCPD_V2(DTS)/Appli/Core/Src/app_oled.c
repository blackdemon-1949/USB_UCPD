/**
  ******************************************************************************
  * @file    app_oled.c
  * @brief   0.96" OLED page renderer - see app_oled.h.
  *
  * Everything here is defensive on purpose: the display is a convenience, and
  * it must never be able to take the PD sink, the USB CDC console or the USART2
  * console down with it.
  *
  *   * no delay loops, no blocking waits - one I2C chunk per super-loop pass;
  *   * all INA226 / PD reads go through the existing accessors, read-only;
  *   * the key is polled (no EXTI, no NVIC change);
  *   * a bus error switches the page off and re-probes slowly.
  ******************************************************************************
  */

#include "app_oled.h"
#include "ssd1306.h"
#include "oled_font.h"
#include "app_log.h"
#include "app_pd.h"
#include "app_board.h"
#include "ina226.h"
#include "ext_i2c.h"
#include "usbpd_def.h"
#include "usbpd_hw_if.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  Tunables
 * ------------------------------------------------------------------------- */

#define OLED_REFRESH_MS       200U   /* redraw period                        */
#define OLED_I2C_ADDR         SSD1306_ADDR_DEFAULT

#define KEY_POLL_MS             5U   /* key sampling period                  */
#define KEY_DEBOUNCE_MS        30U   /* ignore bounce for this long          */
#define KEY_DOUBLE_MS         320U   /* second press must land inside this   */
#define KEY_CALIBRATE_MS      250U   /* idle-level auto-detect window        */

#define MSG_SHOW_MS          1600U   /* transient message duration           */

/* ---------------------------------------------------------------------------
 *  Vertical budget - the whole point of this table is that no two things are
 *  ever drawn into the same rows.  64 rows total:
 *
 *    0..12   header bar (filled) - title left, page dots right, punched out
 *    13..14  gap
 *    15      top corner brackets
 *    16..39  big seven-segment band (SSD1306_SEG_H = 24)
 *    40      gap
 *    41..54  secondary 2x line - REQUEST page, directly under the big band
 *    35..49  two 1x lines - PROTOCOL page, which draws no big band at all
 *            (the two bands are never used by the same page)
 *    54      bottom corner brackets
 *    55..61  footer text (5x7)
 *    62..63  gap
 * ------------------------------------------------------------------------- */

#define HDR_H                  13    /* header bar height, rows 0..12        */
#define FRAME_Y0               15    /* top corner brackets                  */
#define FRAME_Y1               54    /* bottom corner brackets               */
#define FRAME_ARM                5   /* corner bracket arm length            */
#define BIG_Y                  16    /* big seven-segment band, rows 16..39  */
#define SUB2X_Y                41    /* secondary 2x line, rows 41..54       */
#define SUB1X_Y                43    /* secondary 1x line,  rows 43..49      */
#define FTR_Y                  55    /* footer text,        rows 55..61      */
#define MSG_BOX_Y              41    /* message box,        rows 41..54      */
#define MSG_TXT_Y              44    /* message text,       rows 44..50      */

/* ---------------------------------------------------------------------------
 *  State
 * ------------------------------------------------------------------------- */

typedef enum
{
  PAGE_VOLT = 0,
  PAGE_CURRENT,
  PAGE_POWER,
  PAGE_REQUEST,
  PAGE_PROTOCOL
} Page_t;

static uint8_t   s_enabled = 1U;      /* CLI 'oled off'                      */
static Page_t    s_page    = PAGE_VOLT;
static uint32_t  s_next_draw_ms;
static uint32_t  s_next_key_ms;

/* transient user message ("NO SOURCE", "REQ PDO 3", ...) */
static char      s_msg[20];
static uint32_t  s_msg_until_ms;

/* --- key ---------------------------------------------------------------- */
static uint8_t   s_active_high;       /* 1 = pin reads high when pressed     */
static uint8_t   s_calibrating;
static uint32_t  s_cal_end_ms;
static uint8_t   s_cal_first;
static uint8_t   s_cal_mixed;

static uint8_t   s_last_raw;
static uint8_t   s_stable_raw;
static uint32_t  s_last_change_ms;
static uint8_t   s_down;
static uint8_t   s_clicks;
static uint32_t  s_last_click_ms;

static uint8_t   s_fixed_idx;         /* last SPR fixed PDO we requested     */

/* ---------------------------------------------------------------------------
 *  Small helpers
 * ------------------------------------------------------------------------- */

static void show_msg(const char *text)
{
  s_msg[0] = '\0';

  if (text != NULL)
  {
    /* snprintf, not a hand-rolled loop: the destination is a fixed 20-byte
       buffer and this cannot over-read a short string literal either. */
    (void)snprintf(s_msg, sizeof(s_msg), "%s", text);
  }
  s_msg_until_ms = HAL_GetTick() + MSG_SHOW_MS;
  s_next_draw_ms = 0U;                /* draw it on the very next pass */
}

/**
 * @brief  Format val/scale as a fixed-point decimal, integer arithmetic only.
 *
 *   fmt_val(out, n, 20050, 1000, 2)  ->  "20.05"     (mV -> V)
 *   fmt_val(out, n, 1500000, 1000000, 2) -> "1.50"   (uA -> A)
 *
 * The fraction is emitted most-significant digit first, which is what gives
 * the correct zero padding ("20.05", not "20.5").
 *
 * Anything that would not fit the big-digit font becomes "----" rather than
 * running off the edge of the display.
 */
static void fmt_val(char *out, uint16_t outsz, int32_t val, int32_t scale,
                    uint8_t decimals)
{
  int32_t  pow10s = 1;
  uint64_t mag;
  uint64_t q;
  uint32_t ipart;
  uint32_t fpart;
  char     tmp[16];
  uint8_t  n = 0U;
  uint16_t o = 0U;
  uint8_t  neg;

  if ((out == NULL) || (outsz < 2U))
  {
    return;
  }
  if (scale <= 0)
  {
    scale = 1;
  }
  for (uint8_t i = 0U; i < decimals; i++)
  {
    pow10s *= 10;
  }

  neg = (val < 0) ? 1U : 0U;
  mag = (val < 0) ? (uint64_t)(-(int64_t)val) : (uint64_t)val;

  /* q = |val| * 10^decimals / scale  ==  displayed value * 10^decimals */
  q     = (mag * (uint64_t)pow10s) / (uint64_t)scale;
  ipart = (uint32_t)(q / (uint64_t)pow10s);
  fpart = (uint32_t)(q % (uint64_t)pow10s);

  /* Guard: 7 big cells is the widest thing that fits next to a unit. */
  if ((ipart >= 10000UL) && (decimals > 0U))
  {
    (void)snprintf(out, outsz, "----");
    return;
  }
  if (ipart >= 1000000UL)
  {
    (void)snprintf(out, outsz, "----");
    return;
  }

  if (ipart == 0UL)
  {
    tmp[n++] = '0';
  }
  while (ipart > 0UL)
  {
    tmp[n++] = (char)('0' + (char)(ipart % 10UL));
    ipart /= 10UL;
  }

  if ((neg != 0U) && (o < (outsz - 1U))) { out[o++] = '-'; }
  while ((n > 0U) && (o < (outsz - 1U)))
  {
    out[o++] = tmp[--n];
  }
  if ((decimals > 0U) && (o < (outsz - 1U)))
  {
    out[o++] = '.';
    for (uint8_t d = decimals; (d > 0U) && (o < (outsz - 1U)); d--)
    {
      uint32_t place = 1UL;
      for (uint8_t k = 1U; k < d; k++) { place *= 10UL; }
      out[o++] = (char)('0' + (char)((fpart / place) % 10UL));
    }
  }
  out[o] = '\0';
}

/* ---------------------------------------------------------------------------
 *  Chrome - the cyberpunk frame
 * ------------------------------------------------------------------------- */

static void draw_header(const char *title, Page_t page)
{
  /* Solid bar with the title punched out of it. */
  SSD1306_FillRect(0, 0, SSD1306_WIDTH, HDR_H, 1U);
  SSD1306_DrawText(3, 3, title, 0U);          /* black text on white bar */

  /* Page indicator, punched out of the right-hand end of the same bar.  It
     lives up here (not in the footer) because the footer row is exactly as
     tall as one line of text and anything else drawn there overwrites it. */
  {
    const int16_t dot_w = 7;
    const int16_t gap   = 3;
    int16_t total = (int16_t)((int32_t)APP_OLED_PAGES * (dot_w + gap) - gap);
    int16_t x     = (int16_t)(SSD1306_WIDTH - 3 - total);
    const int16_t y = 4;                       /* rows 4..8, inside the bar */

    for (uint8_t i = 0U; i < APP_OLED_PAGES; i++)
    {
      SSD1306_Rect(x, y, (uint16_t)dot_w, 5U, 0U);
      if (i == (uint8_t)page)
      {
        SSD1306_FillRect((int16_t)(x + 1), (int16_t)(y + 1),
                         (uint16_t)(dot_w - 2), 3U, 0U);
      }
      x = (int16_t)(x + dot_w + gap);
    }
  }
}

static void draw_frame(void)
{
  /* Corner brackets around the content area.  They sit in the gaps between
     the bands, never inside one. */
  const int16_t y0 = FRAME_Y0;
  const int16_t y1 = FRAME_Y1;
  const int16_t x0 = 0;
  const int16_t x1 = SSD1306_WIDTH - 1;
  const uint8_t L  = FRAME_ARM;

  SSD1306_HLine(x0, y0, (uint16_t)L, 1U);
  SSD1306_VLine(x0, y0, (uint16_t)L, 1U);
  SSD1306_HLine((int16_t)(x1 - L + 1), y0, (uint16_t)L, 1U);
  SSD1306_VLine(x1, y0, (uint16_t)L, 1U);
  SSD1306_HLine(x0, y1, (uint16_t)L, 1U);
  SSD1306_VLine(x0, (int16_t)(y1 - L + 1), (uint16_t)L, 1U);
  SSD1306_HLine((int16_t)(x1 - L + 1), y1, (uint16_t)L, 1U);
  SSD1306_VLine(x1, (int16_t)(y1 - L + 1), (uint16_t)L, 1U);
}

/* Draw the big value, unit, and the CRT scanlines over the number band. */
static void draw_big_value(const char *value, const char *unit)
{
  uint16_t vw = SSD1306_BigWidth(value);
  uint16_t uw = (unit != NULL) ? SSD1306_TextWidth2x(unit) : 0U;
  uint16_t gap = (uw != 0U) ? 6U : 0U;
  int16_t  x   = (int16_t)((SSD1306_WIDTH - (vw + gap + uw)) / 2);

  if (x < 2) { x = 2; }

  (void)SSD1306_DrawBig(x, BIG_Y, value, 1U);
  if (unit != NULL)
  {
    (void)SSD1306_DrawText2x((int16_t)(x + (int16_t)vw + (int16_t)gap),
                             (int16_t)(BIG_Y + 8), unit, 1U);
  }
  /* Scanlines over the digit band only - text stays crisp. */
  SSD1306_Scanlines((int16_t)(x - 1), BIG_Y,
                    (uint16_t)(vw + gap + uw + 2), SSD1306_SEG_H, 4U);
}

/**
 * @brief  Draw 5x7 text but never past `x_limit` (exclusive).
 *
 * The footer row is 7 px tall and nothing else may be drawn into it, so the
 * left-hand string gets clipped if a right-hand string needs the space.
 */
static int16_t draw_text_clipped(int16_t x, int16_t y, const char *s,
                                 int16_t x_limit)
{
  int16_t cx = x;

  if (s == NULL)
  {
    return cx;
  }
  while (*s != '\0')
  {
    char one[2];

    if ((int32_t)(cx + (int32_t)OLED_FONT_W) > (int32_t)x_limit)
    {
      break;                                  /* would cross the limit */
    }
    one[0] = *s;
    one[1] = '\0';
    (void)SSD1306_DrawText(cx, y, one, 1U);   /* one glyph at a time */
    s++;
    cx = (int16_t)(cx + (int16_t)OLED_FONT_ADVANCE);
  }
  return cx;
}

static void draw_footer(const char *left, const char *right)
{
  if ((left == NULL) && (right == NULL))
  {
    return;
  }

  if (right == NULL)
  {
    (void)draw_text_clipped(2, FTR_Y, left,
                            (int16_t)(SSD1306_WIDTH - 2));
    return;
  }

  {
    /* Right string wins its space, the left one is clipped to what is left. */
    uint16_t rw = SSD1306_TextWidth(right);
    int16_t  rx = (int16_t)(SSD1306_WIDTH - 2 - (int16_t)rw);

    if (rx < 2) { rx = 2; }
    (void)draw_text_clipped(2, FTR_Y, left, (int16_t)(rx - 3));
    (void)SSD1306_DrawText(rx, FTR_Y, right, 1U);
  }
}

/* ---------------------------------------------------------------------------
 *  Pages
 * ------------------------------------------------------------------------- */

static void page_sensor(const char *title, const char *value, const char *unit,
                        const char *tag, Page_t page)
{
  draw_header(title, page);
  draw_frame();
  draw_big_value(value, unit);
  draw_footer(tag, NULL);
}

static void draw_page_volt(void)
{
  char v[10];

  if (INA226_IsPresent() == 0U)
  {
    page_sensor("BUS VOLTAGE", "--.--", "V", "INA226 NOT FOUND", PAGE_VOLT);
    return;
  }
  fmt_val(v, sizeof(v), INA226_GetBusMv(), 1000, 2);
  page_sensor("BUS VOLTAGE", v, "V", "INA226 BUS", PAGE_VOLT);
}

static void draw_page_current(void)
{
  char a[10];

  if (INA226_IsPresent() == 0U)
  {
    page_sensor("CURRENT", "--.--", "A", "INA226 NOT FOUND", PAGE_CURRENT);
    return;
  }
  /* INA226_GetCurUa() is microamps; show amps with two decimals. */
  fmt_val(a, sizeof(a), INA226_GetCurUa(), 1000000, 2);
  page_sensor("CURRENT", a, "A", "INA226 SHUNT", PAGE_CURRENT);
}

static void draw_page_power(void)
{
  char w[10];

  if (INA226_IsPresent() == 0U)
  {
    page_sensor("POWER", "--.--", "W", "INA226 NOT FOUND", PAGE_POWER);
    return;
  }
  fmt_val(w, sizeof(w), INA226_GetPwrMw(), 1000, 2);
  page_sensor("POWER", w, "W", "INA226 PWR", PAGE_POWER);
}

static void draw_page_request(void)
{
  char v[10];
  char a[10];
  char line[24];

  draw_header("REQUESTED", PAGE_REQUEST);
  draw_frame();

  /* Requested voltage, big, in the 16..39 band. */
  fmt_val(v, sizeof(v), (int32_t)APP_PD_Port[0].RequestedVoltage, 1000, 2);
  {
    uint16_t vw = SSD1306_BigWidth(v);
    uint16_t uw = SSD1306_TextWidth2x("V");
    int16_t  x  = (int16_t)((SSD1306_WIDTH - (vw + 6 + uw)) / 2);

    if (x < 2) { x = 2; }
    (void)SSD1306_DrawBig(x, BIG_Y, v, 1U);
    (void)SSD1306_DrawText2x((int16_t)((int32_t)x + (int32_t)vw + 6),
                             (int16_t)(BIG_Y + 8), "V", 1U);
    SSD1306_Scanlines((int16_t)(x - 1), BIG_Y,
                      (uint16_t)(vw + 6 + uw + 2), SSD1306_SEG_H, 4U);
  }

  /* Requested current, 2x, in the 41..54 band. */
  fmt_val(a, sizeof(a), (int32_t)APP_PD_Port[0].RequestedCurrent, 1000, 2);
  {
    const uint16_t gap = 6U;
    uint16_t w = (uint16_t)(SSD1306_TextWidth2x(a) + gap +
                            SSD1306_TextWidth2x("A"));
    int16_t  ax = (int16_t)((SSD1306_WIDTH - w) / 2);

    if (ax < 6) { ax = 6; }
    (void)SSD1306_DrawText2x(ax, SUB2X_Y, a, 1U);
    (void)SSD1306_DrawText2x((int16_t)((int32_t)ax +
                                       (int32_t)SSD1306_TextWidth2x(a) +
                                       (int32_t)gap), SUB2X_Y, "A", 1U);
  }

  /* "PDO n" / "NO REQUEST" on the left of the footer. */
  if (APP_PD_Port[0].RDOPosition != 0U)
  {
    (void)snprintf(line, sizeof(line), "PDO %lu",
                   (unsigned long)APP_PD_Port[0].RDOPosition);
  }
  else
  {
    (void)snprintf(line, sizeof(line), "NO REQUEST");
  }
  draw_footer(line, (APP_PD_Port[0].Contract != 0U) ? "LIVE" : "IDLE");
}

/** Short label for the PDO the active contract is using. */
static void pdo_mode_label(char *out, uint16_t outsz)
{
  uint32_t pos = APP_PD_Port[0].RDOPosition;

  if ((pos == 0U) || (pos > APP_PD_Port[0].NumberOfRcvSRCPDO))
  {
    (void)snprintf(out, outsz, "NO CONTRACT");
    return;
  }
  {
    USBPD_PDO_TypeDef pdo;
    pdo.d32 = APP_PD_Port[0].ListOfRcvSRCPDO[pos - 1U];

    switch (pdo.GenericPDO.PowerObject)
    {
      case USBPD_CORE_PDO_TYPE_FIXED:
        (void)snprintf(out, outsz, "SPR FIXED");
        break;
      case USBPD_CORE_PDO_TYPE_APDO:
        (void)snprintf(out, outsz, "PPS APDO");
        break;
      case USBPD_CORE_PDO_TYPE_BATTERY:
        (void)snprintf(out, outsz, "BATTERY");
        break;
      case USBPD_CORE_PDO_TYPE_VARIABLE:
        (void)snprintf(out, outsz, "VARIABLE");
        break;
      default:
        (void)snprintf(out, outsz, "UNKNOWN");
        break;
    }
  }
}

static void draw_page_protocol(void)
{
  char line[22];

  draw_header("PROTOCOL", PAGE_PROTOCOL);
  draw_frame();

  /* Row 1 - the protocol name, large. */
  if (APP_PD_Port[0].Attached == 0U)
  {
    (void)SSD1306_DrawText2x(8, 20, "NO SOURCE", 1U);
  }
  else
  {
    const char *rev = "PD";

    if ((Ports[0].settings != NULL))
    {
      switch (Ports[0].settings->PE_SpecRevision)
      {
        case USBPD_SPECIFICATION_REV3: rev = "PD 3.0"; break;
        case USBPD_SPECIFICATION_REV2: rev = "PD 2.0"; break;
        default:                       rev = "PD 1.0"; break;
      }
    }
    (void)SSD1306_DrawText2x(8, 20, rev, 1U);
  }

  /* Row 2 - power mode. */
  pdo_mode_label(line, sizeof(line));
  (void)SSD1306_DrawText(8, 35, line, 1U);

  /* Row 3 - CC line and PDO index. */
  if (APP_PD_Port[0].RDOPosition != 0U)
  {
    (void)snprintf(line, sizeof(line), "PDO %lu  CC%lu",
                   (unsigned long)APP_PD_Port[0].RDOPosition,
                   (unsigned long)APP_PD_Port[0].CCx);
  }
  else
  {
    (void)snprintf(line, sizeof(line), "CC%lu  NEGOTIATING",
                   (unsigned long)APP_PD_Port[0].CCx);
  }
  (void)SSD1306_DrawText(8, SUB1X_Y, line, 1U);

  draw_footer("USB TYPE-C", (APP_PD_Port[0].Contract != 0U) ? "ON" : "OFF");
}

static void draw_page(void)
{
  SSD1306_Clear();

  switch (s_page)
  {
    case PAGE_VOLT:     draw_page_volt();     break;
    case PAGE_CURRENT:  draw_page_current();  break;
    case PAGE_POWER:    draw_page_power();    break;
    case PAGE_REQUEST:  draw_page_request();  break;
    case PAGE_PROTOCOL: draw_page_protocol(); break;
    default:            draw_page_volt();     break;
  }

  /* A transient message takes over the 41..54 band: it blanks whatever the
     page put there and writes itself in inverse video. */
  if (s_msg[0] != '\0')
  {
    const uint16_t bx = 6U;
    const uint16_t bw = SSD1306_WIDTH - 12U;

    SSD1306_FillRect((int16_t)bx, MSG_BOX_Y, bw, 14, 1U);
    {
      uint16_t w = SSD1306_TextWidth(s_msg);
      int16_t  x = (int16_t)(((int32_t)bx + ((int32_t)bw - (int32_t)w) / 2));

      if (x < (int16_t)(bx + 2)) { x = (int16_t)(bx + 2); }
      (void)SSD1306_DrawText(x, MSG_TXT_Y, s_msg, 0U);
    }
  }
}

/* ---------------------------------------------------------------------------
 *  PD request (double press)
 * ------------------------------------------------------------------------- */

/** Advance to the next SPR *fixed* PDO and ask the source for it. */
static void request_next_fixed(void)
{
  uint8_t n = APP_PD_Port[0].NumberOfRcvSRCPDO;
  uint8_t i;
  char    msg[20];

  if (APP_PD_Port[0].Attached == 0U)
  {
    show_msg("NO SOURCE");
    return;
  }
  if (n == 0U)
  {
    show_msg("NO CAPS YET");
    return;
  }

  for (i = 0U; i < n; i++)
  {
    USBPD_PDO_TypeDef pdo;

    s_fixed_idx++;
    if ((s_fixed_idx == 0U) || (s_fixed_idx > n))
    {
      s_fixed_idx = 1U;
    }
    pdo.d32 = APP_PD_Port[0].ListOfRcvSRCPDO[s_fixed_idx - 1U];
    if (pdo.GenericPDO.PowerObject == USBPD_CORE_PDO_TYPE_FIXED)
    {
      if (APP_PD_SendRequest(0U, s_fixed_idx, 0U, 0U) == USBPD_OK)
      {
        (void)snprintf(msg, sizeof(msg), "REQ PDO %lu",
                       (unsigned long)s_fixed_idx);
      }
      else
      {
        (void)snprintf(msg, sizeof(msg), "PDO %lu REFUSED",
                       (unsigned long)s_fixed_idx);
      }
      show_msg(msg);
      return;
    }
  }
  show_msg("NO FIXED PDO");
}

/* ---------------------------------------------------------------------------
 *  PC13 key
 * ------------------------------------------------------------------------- */

static uint8_t key_pressed_raw(void)
{
  return (HAL_GPIO_ReadPin(APP_KEY_PORT, APP_KEY_PIN) == GPIO_PIN_SET) ? 1U : 0U;
}

/**
 * @brief  Work out which level means "pressed".
 *
 * The board documents PC13 as active low (external 10 k pull-up), and
 * app_board.h says so, but the owner reports the opposite wiring.  Rather than
 * guess, sample the pin for KEY_CALIBRATE_MS at start-up: whatever level is
 * stable while nobody is touching the key must be the idle level.  If the pin
 * moves during the window (key held at boot) fall back to the documented
 * active-low behaviour.
 */
static void key_start_calibration(void)
{
  s_calibrating = 1U;
  s_cal_mixed   = 0U;
  s_cal_first   = key_pressed_raw();
  s_cal_end_ms  = HAL_GetTick() + KEY_CALIBRATE_MS;
}

static void key_finish_calibration(void)
{
  s_calibrating = 0U;

  if (s_cal_mixed != 0U)
  {
    s_active_high = 0U;               /* documented board behaviour */
    APP_LOG_Write("oled: key level not stable at boot - assuming active low\r\n");
  }
  else
  {
    /* idle level is whatever we sampled; pressed is the other one */
    s_active_high = (s_cal_first == 0U) ? 1U : 0U;
  }
  APP_LOG_Printf("oled: PC13 key is active %s\r\n",
                 s_active_high ? "high" : "low");
}

static void key_tick(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t  raw;
  uint8_t  pressed;

  if (now < s_next_key_ms) { return; }
  s_next_key_ms = now + KEY_POLL_MS;

  raw = key_pressed_raw();

  if (s_calibrating != 0U)
  {
    if (raw != s_cal_first) { s_cal_mixed = 1U; }
    if ((int32_t)(now - s_cal_end_ms) >= 0) { key_finish_calibration(); }
    return;
  }

  /* debounce */
  if (raw != s_last_raw)
  {
    s_last_raw      = raw;
    s_last_change_ms = now;
    return;
  }
  if ((int32_t)(now - s_last_change_ms) < (int32_t)KEY_DEBOUNCE_MS) { return; }
  s_stable_raw = raw;

  pressed = (s_active_high != 0U) ? (raw != 0U) : (raw == 0U);

  if (pressed && (s_down == 0U))
  {
    s_down = 1U;
    return;                            /* count it on release */
  }
  if ((pressed == 0U) && (s_down != 0U))
  {
    s_down = 0U;
    s_clicks++;
    s_last_click_ms = now;
    return;
  }

  /* No further press inside the double-press window: act on what we have. */
  if ((s_clicks != 0U) && ((int32_t)(now - s_last_click_ms) >= (int32_t)KEY_DOUBLE_MS))
  {
    if (s_clicks >= 2U)
    {
      request_next_fixed();
    }
    else
    {
      s_page = (Page_t)(((uint8_t)s_page + 1U) % APP_OLED_PAGES);
      s_next_draw_ms = 0U;             /* redraw immediately */
    }
    s_clicks = 0U;
  }
}

/* ---------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------- */

void APP_OLED_Init(void)
{
  s_page          = PAGE_VOLT;
  s_next_draw_ms  = 0U;
  s_next_key_ms   = 0U;
  s_msg[0]        = '\0';
  s_msg_until_ms  = 0U;
  s_fixed_idx     = 0U;
  s_active_high   = 0U;
  s_down          = 0U;
  s_clicks        = 0U;
  s_last_raw      = 2U;                /* impossible value -> first poll syncs */
  s_stable_raw    = 0U;
  s_last_change_ms = 0U;
  s_last_click_ms = 0U;

  SSD1306_Init((uint8_t)OLED_I2C_ADDR);
  key_start_calibration();

  APP_LOG_Printf("oled: SSD1306 %s at 0x%02X on I2C2 (shared with INA226)\r\n",
                 SSD1306_IsPresent() ? "found" : "not found",
                 (unsigned)SSD1306_GetAddr());
  if (SSD1306_IsPresent() != 0U)
  {
    APP_LOG_Write("oled: PC13 1 press = next page, 2 presses = next fixed PDO\r\n");
  }
}

void APP_OLED_Poll(void)
{
  uint32_t now;

  if (s_enabled == 0U)
  {
    return;
  }

  key_tick();

  /* Push at most one frame chunk, then let the loop get on with real work. */
  if (SSD1306_UpdateStep() < 0)
  {
    return;                            /* bus error: driver is re-probing */
  }

  now = HAL_GetTick();
  if ((s_msg[0] != '\0') && ((int32_t)(now - s_msg_until_ms) >= 0))
  {
    s_msg[0] = '\0';
    s_next_draw_ms = 0U;
  }

  if (SSD1306_IsPresent() == 0U)
  {
    return;
  }

  if ((s_next_draw_ms == 0U) || ((int32_t)(now - s_next_draw_ms) >= 0))
  {
    if (SSD1306_UpdateBusy() == 0U)
    {
      draw_page();
      s_next_draw_ms = now + OLED_REFRESH_MS;
      (void)SSD1306_UpdateStart();
    }
    else
    {
      s_next_draw_ms = now + 10U;      /* frame still draining, try shortly */
    }
  }
}

/* ---------------------------------------------------------------------------
 *  CLI
 * ------------------------------------------------------------------------- */

void APP_OLED_Cli(int argc, char *argv[])
{
  if ((argc >= 2) && (strcmp(argv[1], "on") == 0))
  {
    s_enabled = 1U;
    s_next_draw_ms = 0U;
    SSD1306_DisplayOn(1U);            /* 'oled off' turned the panel off */
    APP_LOG_Write("oled: on\r\n");
    return;
  }
  if ((argc >= 2) && (strcmp(argv[1], "off") == 0))
  {
    s_enabled = 0U;
    SSD1306_DisplayOn(0U);
    APP_LOG_Write("oled: off\r\n");
    return;
  }
  if ((argc >= 3) && (strcmp(argv[1], "page") == 0))
  {
    unsigned n = 0U;
    if ((EXT_I2C_ParseU(argv[2], &n) != 0) || (n >= APP_OLED_PAGES))
    {
      APP_LOG_Printf("usage: oled page <0-%u>\r\n", (unsigned)(APP_OLED_PAGES - 1U));
      return;
    }
    s_page = (Page_t)n;
    s_next_draw_ms = 0U;
    APP_LOG_Printf("oled: page %u\r\n", n);
    return;
  }
  if ((argc >= 3) && (strcmp(argv[1], "addr") == 0))
  {
    unsigned a = 0U;
    if ((EXT_I2C_ParseU(argv[2], &a) != 0) || (a == 0U) || (a > 0x7FU))
    {
      APP_LOG_Write("usage: oled addr <7-bit hex, e.g. 3c>\r\n");
      return;
    }
    SSD1306_SetAddr((uint8_t)a);
    APP_LOG_Printf("oled: address 0x%02X, display %s\r\n", a,
                   SSD1306_IsPresent() ? "found" : "not found");
    return;
  }
  if ((argc >= 3) && (strcmp(argv[1], "key") == 0))
  {
    if (strcmp(argv[2], "high") == 0)
    {
      s_active_high = 1U;
      s_calibrating = 0U;
      APP_LOG_Write("oled: key active high\r\n");
      return;
    }
    if (strcmp(argv[2], "low") == 0)
    {
      s_active_high = 0U;
      s_calibrating = 0U;
      APP_LOG_Write("oled: key active low\r\n");
      return;
    }
    if (strcmp(argv[2], "auto") == 0)
    {
      key_start_calibration();
      APP_LOG_Write("oled: re-detecting key level, do not touch the key\r\n");
      return;
    }
  }

  APP_LOG_Printf("oled: %s\r\n", SSD1306_IsPresent() ? "present" : "not found");
  APP_LOG_Printf("      addr 0x%02X   page %u/%u   key active %s\r\n",
                 (unsigned)SSD1306_GetAddr(),
                 (unsigned)s_page, (unsigned)(APP_OLED_PAGES - 1U),
                 s_active_high ? "high" : "low");
  APP_LOG_Write("      usage: oled [on|off | page <n> | addr <hex> | key high|low|auto]\r\n");
}
