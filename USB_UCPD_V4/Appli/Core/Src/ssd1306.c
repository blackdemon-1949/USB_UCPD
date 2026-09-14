/**
  ******************************************************************************
  * @file    ssd1306.c
  * @brief   Minimal SSD1306 (128x64, I2C) driver - see ssd1306.h.
  *
  * Bus discipline (important - I2C2 is shared with the INA226):
  *   * every transfer is a complete, short, blocking HAL transaction, so the
  *     INA226 driver never sees a half-finished transfer on the handle;
  *   * a full 1024-byte frame is split into SSD1306_CHUNK-byte chunks, one per
  *     super-loop pass, rate limited by SSD1306_CHUNK_SPACING_MS;
  *   * any HAL error aborts the frame, backs off, and re-probes later.
  ******************************************************************************
  */

#include "ssd1306.h"
#include "oled_font.h"
#include "i2c.h"
#include "ext_i2c.h"

/* Bytes of framebuffer data per I2C transaction (plus one control byte). */
#define SSD1306_CHUNK            64U
/* Minimum gap between two chunks, ms.  Keeps the super loop breathing. */
#define SSD1306_CHUNK_SPACING_MS  2U
/* Bus timeout for a single chunk, ms.  Short: the bus is shared. */
#define SSD1306_CHUNK_TIMEOUT_MS  5U
/* Re-probe interval when no display was found, ms. */
#define SSD1306_REPROBE_MS     5000U

#define SSD1306_CTRL_CMD        0x00U   /* Co=0, D/C#=0 */
#define SSD1306_CTRL_DATA       0x40U   /* Co=0, D/C#=1 */

/* Seven-segment bit map: a b c d e f g */
#define SEG_A 0x01U
#define SEG_B 0x02U
#define SEG_C 0x04U
#define SEG_D 0x08U
#define SEG_E 0x10U
#define SEG_F 0x20U
#define SEG_G 0x40U

static const uint8_t kSegDigits[10] =
{
  0x3FU, /* 0 */
  0x06U, /* 1 */
  0x5BU, /* 2 */
  0x4FU, /* 3 */
  0x66U, /* 4 */
  0x6DU, /* 5 */
  0x7DU, /* 6 */
  0x07U, /* 7 */
  0x7FU, /* 8 */
  0x6FU  /* 9 */
};

static uint8_t s_fb[SSD1306_FB_SIZE];
static uint8_t s_present;
static uint8_t s_addr      = SSD1306_ADDR_DEFAULT;
static uint8_t s_tx_busy;
static uint16_t s_tx_idx;
static uint32_t s_tx_last_ms;
static uint32_t s_next_probe_ms;

/* ========================================================================== */
/*  Bus helpers                                                               */
/* ========================================================================== */

static HAL_StatusTypeDef bus_cmd(const uint8_t *bytes, uint16_t len)
{
  uint8_t buf[1 + 16U];

  if ((EXT_I2C_IsReady() == 0U) || (len > sizeof(buf) - 1U))
  {
    return HAL_ERROR;
  }
  buf[0] = SSD1306_CTRL_CMD;
  for (uint16_t i = 0U; i < len; i++)
  {
    buf[1U + i] = bytes[i];
  }
  return HAL_I2C_Master_Transmit(&hi2c2, (uint16_t)(s_addr << 1), buf,
                                 (uint16_t)(1U + len), SSD1306_CHUNK_TIMEOUT_MS);
}

static HAL_StatusTypeDef bus_cmd1(uint8_t c)
{
  return bus_cmd(&c, 1U);
}

static uint8_t probe_device(void)
{
  if (EXT_I2C_IsReady() == 0U)
  {
    return 0U;
  }
  return (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(s_addr << 1), 1U, 5U) == HAL_OK)
         ? 1U : 0U;
}

/* ========================================================================== */
/*  Configuration                                                             */
/* ========================================================================== */

/** Re-point the hardware write window at column 0 / page 0.  Sent at the start
 *  of every frame: if a previous frame was aborted by a bus error the
 *  controller's address counter is left somewhere in the middle, and without
 *  this the next frame would be drawn rotated. */
static uint8_t set_window(void)
{
  static const uint8_t col[] =
  {
    0x21U, 0x00U, (SSD1306_WIDTH - 1U)      /* set column address */
  };
  static const uint8_t pag[] =
  {
    0x22U, 0x00U, (SSD1306_PAGES - 1U)      /* set page address   */
  };

  if (bus_cmd(col, sizeof(col)) != HAL_OK) { return 0U; }
  if (bus_cmd(pag, sizeof(pag)) != HAL_OK) { return 0U; }
  return 1U;
}

/* [command bytes][length] pairs - one I2C transfer each.  Keeping them short
   and separate makes the sequence readable and keeps any single transfer well
   inside the shared-bus timeout. */
typedef struct
{
  const uint8_t *bytes;
  uint8_t        len;
} Cmd_t;

static uint8_t configure(void)
{
  static const uint8_t c_off[]       = { 0xAEU };                      /* display off            */
  static const uint8_t c_mode[]      = { 0x20U, 0x00U };               /* horizontal addressing  */
  static const uint8_t c_startline[] = { 0x40U };                      /* start line 0           */
  static const uint8_t c_remap[]     = { 0xA1U };                      /* segment remap         */
  static const uint8_t c_comscan[]   = { 0xC8U };                      /* COM scan direction    */
  static const uint8_t c_mux[]       = { 0xA8U, 0x3FU };               /* 64 rows               */
  static const uint8_t c_offset[]    = { 0xD3U, 0x00U };               /* display offset 0      */
  static const uint8_t c_clock[]     = { 0xD5U, 0x80U };               /* clock div / osc       */
  static const uint8_t c_prechg[]    = { 0xD9U, 0xF1U };               /* pre-charge            */
  static const uint8_t c_compins[]   = { 0xDAU, 0x12U };               /* COM pin config        */
  static const uint8_t c_vcom[]      = { 0xDBU, 0x30U };               /* VCOMH deselect        */
  static const uint8_t c_pump[]      = { 0x8DU, 0x14U };               /* charge pump on        */
  static const uint8_t c_contrast[]  = { 0x81U, 0xCFU };               /* contrast              */
  static const uint8_t c_ram[]       = { 0xA4U };                      /* follow RAM            */
  static const uint8_t c_normal[]    = { 0xA6U };                      /* not inverted          */
  static const uint8_t c_noscroll[]  = { 0x2EU };                      /* stop scroll           */
  static const uint8_t c_on[]        = { 0xAFU };                      /* display on            */

  static const Cmd_t seq[] =
  {
    { c_off,       1U },
    { c_mode,      2U },
    { c_startline, 1U },
    { c_remap,     1U },
    { c_comscan,   1U },
    { c_mux,       2U },
    { c_offset,    2U },
    { c_clock,     2U },
    { c_prechg,    2U },
    { c_compins,   2U },
    { c_vcom,      2U },
    { c_pump,      2U },
    { c_contrast,  2U },
    { c_ram,       1U },
    { c_normal,    1U },
    { c_noscroll,  1U },
    { c_on,        1U }
  };

  for (uint16_t i = 0U; i < (sizeof(seq) / sizeof(seq[0])); i++)
  {
    if (bus_cmd(seq[i].bytes, seq[i].len) != HAL_OK)
    {
      return 0U;
    }
  }
  return 1U;
}

void SSD1306_Init(uint8_t addr_7bit)
{
  s_addr = (uint8_t)(addr_7bit & 0x7FU);
  s_present = 0U;
  s_tx_busy = 0U;
  s_tx_idx  = 0U;
  s_next_probe_ms = 0U;

  SSD1306_Clear();

  if (probe_device() == 0U)
  {
    return;
  }
  if (configure() == 0U)
  {
    return;
  }
  s_present = 1U;
  (void)SSD1306_UpdateStart();
}

uint8_t SSD1306_IsPresent(void)
{
  return s_present;
}

uint8_t SSD1306_GetAddr(void)
{
  return s_addr;
}

void SSD1306_SetAddr(uint8_t addr_7bit)
{
  s_addr = (uint8_t)(addr_7bit & 0x7FU);
  s_present = 0U;
  s_tx_busy = 0U;
  s_next_probe_ms = 0U;
  SSD1306_Init(s_addr);
}

uint8_t SSD1306_ReProbe(void)
{
  s_present = 0U;
  s_tx_busy = 0U;
  s_tx_idx  = 0U;

  if (probe_device() == 0U)
  {
    return 0U;
  }
  if (configure() == 0U)
  {
    return 0U;
  }
  s_present = 1U;
  (void)SSD1306_UpdateStart();
  return 1U;
}

void SSD1306_DisplayOn(uint8_t on)
{
  if (s_present == 0U)
  {
    return;
  }
  (void)bus_cmd1(on ? 0xAFU : 0xAEU);
}

void SSD1306_SetContrast(uint8_t val)
{
  uint8_t seq[2];

  if (s_present == 0U)
  {
    return;
  }
  seq[0] = 0x81U;
  seq[1] = val;
  (void)bus_cmd(seq, 2U);
}

/* ========================================================================== */
/*  Chunked frame transfer                                                    */
/* ========================================================================== */

uint8_t SSD1306_UpdateStart(void)
{
  if (s_present == 0U)
  {
    return 0U;
  }
  /* Re-point the window first (see set_window()).  Six bytes, once a frame. */
  if (set_window() != 0U)
  {
    s_tx_idx  = 0U;
    s_tx_busy = 1U;
    s_tx_last_ms = 0U;
    return 1U;
  }
  /* Bus died: fall back to the slow re-probe path. */
  s_present = 0U;
  s_tx_busy = 0U;
  s_next_probe_ms = HAL_GetTick() + SSD1306_REPROBE_MS;
  return 0U;
}

uint8_t SSD1306_UpdateBusy(void)
{
  return s_tx_busy;
}

int SSD1306_UpdateStep(void)
{
  uint8_t  buf[1U + SSD1306_CHUNK];
  uint16_t n;
  uint32_t now;

  if (s_present == 0U)
  {
    /* Nothing connected: look again occasionally so a display fitted later is
       picked up without a reset. */
    now = HAL_GetTick();
    if ((s_next_probe_ms == 0U) || ((int32_t)(now - s_next_probe_ms) >= 0))
    {
      s_next_probe_ms = now + SSD1306_REPROBE_MS;
      (void)SSD1306_ReProbe();
    }
    return 0;
  }
  if (s_tx_busy == 0U)
  {
    return 0;
  }

  now = HAL_GetTick();
  if ((int32_t)(now - s_tx_last_ms) < (int32_t)SSD1306_CHUNK_SPACING_MS)
  {
    return 1;                       /* still busy, wait for the next pass */
  }

  n = (uint16_t)(SSD1306_FB_SIZE - s_tx_idx);
  if (n > SSD1306_CHUNK)
  {
    n = SSD1306_CHUNK;
  }

  buf[0] = SSD1306_CTRL_DATA;
  for (uint16_t i = 0U; i < n; i++)
  {
    buf[1U + i] = s_fb[s_tx_idx + i];
  }

  /* Re-point the window at the start of every frame; after that the
     controller auto-advances through pages, so no per-chunk addressing. */
  if (HAL_I2C_Master_Transmit(&hi2c2, (uint16_t)(s_addr << 1), buf,
                              (uint16_t)(1U + n),
                              SSD1306_CHUNK_TIMEOUT_MS) != HAL_OK)
  {
    s_tx_busy = 0U;
    s_present = 0U;
    s_next_probe_ms = HAL_GetTick() + SSD1306_REPROBE_MS;
    return -1;
  }

  s_tx_last_ms = now;
  s_tx_idx = (uint16_t)(s_tx_idx + n);
  if (s_tx_idx >= SSD1306_FB_SIZE)
  {
    s_tx_busy = 0U;
    s_tx_idx  = 0U;
    return 0;
  }
  return 1;
}

/* ========================================================================== */
/*  Primitives                                                                */
/* ========================================================================== */

void SSD1306_Clear(void)
{
  for (uint16_t i = 0U; i < SSD1306_FB_SIZE; i++)
  {
    s_fb[i] = 0U;
  }
}

void SSD1306_SetPixel(int16_t x, int16_t y, uint8_t on)
{
  uint16_t idx;

  if ((x < 0) || (x >= (int16_t)SSD1306_WIDTH) ||
      (y < 0) || (y >= (int16_t)SSD1306_HEIGHT))
  {
    return;
  }
  idx = (uint16_t)(((uint16_t)y >> 3) * SSD1306_WIDTH) + (uint16_t)x;
  if (on != 0U)
  {
    s_fb[idx] |= (uint8_t)(1U << ((uint16_t)y & 7U));
  }
  else
  {
    s_fb[idx] &= (uint8_t)~(uint8_t)(1U << ((uint16_t)y & 7U));
  }
}

void SSD1306_FillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t on)
{
  for (uint16_t r = 0U; r < h; r++)
  {
    for (uint16_t c = 0U; c < w; c++)
    {
      SSD1306_SetPixel((int16_t)((int32_t)x + (int32_t)c),
                       (int16_t)((int32_t)y + (int32_t)r), on);
    }
  }
}

void SSD1306_HLine(int16_t x, int16_t y, uint16_t w, uint8_t on)
{
  SSD1306_FillRect(x, y, w, 1U, on);
}

void SSD1306_VLine(int16_t x, int16_t y, uint16_t h, uint8_t on)
{
  SSD1306_FillRect(x, y, 1U, h, on);
}

void SSD1306_Rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t on)
{
  if ((w == 0U) || (h == 0U))
  {
    return;
  }
  SSD1306_HLine(x, y, w, on);
  SSD1306_HLine(x, (int16_t)((int32_t)y + (int32_t)h - 1), w, on);
  SSD1306_VLine(x, y, h, on);
  SSD1306_VLine((int16_t)((int32_t)x + (int32_t)w - 1), y, h, on);
}

void SSD1306_Scanlines(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t period)
{
  if (period == 0U)
  {
    return;
  }
  for (uint16_t r = 0U; r < h; r++)
  {
    if (((r + 1U) % period) == 0U)
    {
      SSD1306_HLine(x, (int16_t)((int32_t)y + (int32_t)r), w, 0U);
    }
  }
}

/* ========================================================================== */
/*  Text                                                                      */
/* ========================================================================== */

uint16_t SSD1306_TextWidth(const char *s)
{
  uint16_t n = 0U;

  if (s == NULL)
  {
    return 0U;
  }
  while (*s != '\0')
  {
    n += OLED_FONT_ADVANCE;
    s++;
  }
  return (n == 0U) ? 0U : (uint16_t)(n - 1U);   /* no gap after the last glyph */
}

uint16_t SSD1306_TextWidth2x(const char *s)
{
  return (uint16_t)(SSD1306_TextWidth(s) * 2U);
}

static void draw_glyph(int16_t x, int16_t y, char c, uint8_t on, uint8_t scale)
{
  const uint8_t *g = OLED_Font5x7[OLED_FontIndex(c)];

  for (uint8_t col = 0U; col < OLED_FONT_W; col++)
  {
    uint8_t bits = g[col];
    for (uint8_t row = 0U; row < OLED_FONT_HEIGHT; row++)
    {
      if ((bits & (uint8_t)(1U << row)) != 0U)
      {
        SSD1306_FillRect((int16_t)((int32_t)x + ((int32_t)col * scale)),
                         (int16_t)((int32_t)y + ((int32_t)row * scale)),
                         scale, scale, on);
      }
    }
  }
}

int16_t SSD1306_DrawText(int16_t x, int16_t y, const char *s, uint8_t on)
{
  int16_t cx = x;

  if (s == NULL)
  {
    return x;
  }
  while (*s != '\0')
  {
    draw_glyph(cx, y, *s, on, 1U);
    cx = (int16_t)(cx + (int16_t)OLED_FONT_ADVANCE);
    s++;
  }
  return (int16_t)(cx - 1);
}

int16_t SSD1306_DrawText2x(int16_t x, int16_t y, const char *s, uint8_t on)
{
  int16_t cx = x;

  if (s == NULL)
  {
    return x;
  }
  while (*s != '\0')
  {
    draw_glyph(cx, y, *s, on, 2U);
    cx = (int16_t)(cx + (int16_t)(OLED_FONT_ADVANCE * 2U));
    s++;
  }
  return (int16_t)(cx - 1);
}

/* ========================================================================== */
/*  Big seven-segment digits                                                  */
/* ========================================================================== */

/** One horizontal or vertical bar, sheared for the italic look. */
static void seg_bar(int16_t x, int16_t y, uint16_t w, uint16_t h,
                    int16_t base_y, uint8_t on)
{
  for (uint16_t r = 0U; r < h; r++)
  {
    int16_t  yy   = (int16_t)((int32_t)y + (int32_t)r);
    int32_t  from_bottom = (int32_t)SSD1306_SEG_H - 1 - ((int32_t)yy - (int32_t)base_y);
    int16_t  off  = (int16_t)((from_bottom * (int32_t)SSD1306_SEG_SLANT) /
                              ((int32_t)SSD1306_SEG_H - 1));

    if (off < 0) { off = 0; }
    SSD1306_FillRect((int16_t)((int32_t)x + (int32_t)off), yy, w, 1U, on);
  }
}

static void seg_glyph(int16_t x, int16_t y, char c, uint8_t on)
{
  uint8_t segs;
  const int16_t T  = 4;                                  /* bar thickness */
  const int16_t W  = (int16_t)SSD1306_SEG_W;
  const int16_t Hh = (int16_t)SSD1306_SEG_H;
  const int16_t hx = (int16_t)(x + 2);                   /* horizontal bars */
  const int16_t hw = (int16_t)(W - 6);                   /* 6 px wide       */
  const int16_t rv = (int16_t)(x + W - T - 2);           /* right verticals */
  const int16_t h1 = (int16_t)((Hh - T) / 2);            /* 10              */

  if ((c >= '0') && (c <= '9'))
  {
    segs = kSegDigits[(uint8_t)(c - '0')];
  }
  else if (c == '-')
  {
    segs = SEG_G;
  }
  else if (c == '.')
  {
    SSD1306_FillRect((int16_t)(x + (W - T - 2) + 1), (int16_t)(y + Hh - T), T, T, on);
    return;
  }
  else
  {
    return;                                              /* blank cell */
  }

  if ((segs & SEG_A) != 0U) { seg_bar(hx, y,                       (uint16_t)hw, (uint16_t)T,  y, on); }
  if ((segs & SEG_G) != 0U) { seg_bar(hx, (int16_t)(y + h1),       (uint16_t)hw, (uint16_t)T,  y, on); }
  if ((segs & SEG_D) != 0U) { seg_bar(hx, (int16_t)(y + Hh - T),   (uint16_t)hw, (uint16_t)T,  y, on); }

  if ((segs & SEG_F) != 0U) { seg_bar(x,  (int16_t)(y + 1),        (uint16_t)T,  (uint16_t)h1, y, on); }
  if ((segs & SEG_B) != 0U) { seg_bar(rv, (int16_t)(y + 1),        (uint16_t)T,  (uint16_t)h1, y, on); }
  if ((segs & SEG_E) != 0U) { seg_bar(x,  (int16_t)(y + h1 + 1),   (uint16_t)T,  (uint16_t)h1, y, on); }
  if ((segs & SEG_C) != 0U) { seg_bar(rv, (int16_t)(y + h1 + 1),   (uint16_t)T,  (uint16_t)h1, y, on); }
}

uint16_t SSD1306_BigWidth(const char *s)
{
  uint16_t n = 0U;

  if (s == NULL)
  {
    return 0U;
  }
  while (*s != '\0')
  {
    n += SSD1306_SEG_W;
    s++;
  }
  return n;
}

int16_t SSD1306_DrawBig(int16_t x, int16_t y, const char *s, uint8_t on)
{
  int16_t cx = x;

  if (s == NULL)
  {
    return x;
  }
  while (*s != '\0')
  {
    seg_glyph(cx, y, *s, on);
    cx = (int16_t)(cx + (int16_t)SSD1306_SEG_W);
    s++;
  }
  return cx;
}
