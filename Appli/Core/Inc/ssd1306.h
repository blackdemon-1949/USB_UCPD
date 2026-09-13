/**
  ******************************************************************************
  * @file    ssd1306.h
  * @brief   Minimal SSD1306 (128x64, I2C) driver for the 0.96" OLED page.
  *
  * Shares I2C2 with the INA226.  Every bus access is a short, self-contained
  * blocking transfer and the frame is pushed in chunks from the super loop, so
  * the driver never holds the bus (or the CPU) long enough to disturb the
  * INA226 sampling, the PD stack, the USB CDC console or the CLI.
  *
  * If no display answers at init the driver switches itself off and every
  * entry point becomes a no-op; it re-probes slowly so a display plugged in
  * later is picked up without a reset.
  ******************************************************************************
  */

#ifndef SSD1306_H
#define SSD1306_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SSD1306_WIDTH        128U
#define SSD1306_HEIGHT        64U
#define SSD1306_PAGES          8U
#define SSD1306_FB_SIZE      (SSD1306_WIDTH * SSD1306_PAGES)   /* 1024 */

/* Default 7-bit address of a 0.96" SSD1306 module.  0x3D is the other one. */
#define SSD1306_ADDR_DEFAULT  0x3CU

/* --- large seven-segment digits ------------------------------------------ */
#define SSD1306_SEG_W         12U   /* cell advance, glyph is 10 px wide */
#define SSD1306_SEG_H         24U
#define SSD1306_SEG_SLANT      3U   /* italic shear, px over the glyph height */

/* ---------------------------------------------------------------------------
 *  Lifecycle
 * ------------------------------------------------------------------------- */

/** Probe and configure the display.  Safe to call with nothing connected. */
void     SSD1306_Init(uint8_t addr_7bit);

/** 1 if a display answered the last probe and init succeeded. */
uint8_t  SSD1306_IsPresent(void);

/** 7-bit address in use. */
uint8_t  SSD1306_GetAddr(void);
void     SSD1306_SetAddr(uint8_t addr_7bit);

/** Re-probe after a bus fault.  Returns 1 if the display came back. */
uint8_t  SSD1306_ReProbe(void);

void     SSD1306_DisplayOn(uint8_t on);
void     SSD1306_SetContrast(uint8_t val);

/* ---------------------------------------------------------------------------
 *  Frame transfer (non-blocking, chunked)
 * ------------------------------------------------------------------------- */

/**
 * @brief  Begin pushing the framebuffer to the display.
 * @retval 1 = transfer started, 0 = nothing to do (not present / busy).
 */
uint8_t  SSD1306_UpdateStart(void);

/**
 * @brief  Push at most one chunk.  Call once per super-loop pass.
 * @retval 1 = still busy, 0 = idle (frame finished or nothing pending),
 *        -1 = bus error (the driver backs off and re-probes).
 */
int      SSD1306_UpdateStep(void);

/** 1 while a frame transfer is in flight. */
uint8_t  SSD1306_UpdateBusy(void);

/* ---------------------------------------------------------------------------
 *  Drawing primitives (all clipped to the 128x64 framebuffer)
 * ------------------------------------------------------------------------- */

void     SSD1306_Clear(void);
void     SSD1306_SetPixel(int16_t x, int16_t y, uint8_t on);
void     SSD1306_FillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t on);
void     SSD1306_Rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t on);
void     SSD1306_HLine(int16_t x, int16_t y, uint16_t w, uint8_t on);
void     SSD1306_VLine(int16_t x, int16_t y, uint16_t h, uint8_t on);

/* ---------------------------------------------------------------------------
 *  Text
 * ------------------------------------------------------------------------- */

/** 5x7 text at 1x.  Returns the x position just past the last glyph. */
int16_t  SSD1306_DrawText(int16_t x, int16_t y, const char *s, uint8_t on);
/** 5x7 text at 2x (10x14). */
int16_t  SSD1306_DrawText2x(int16_t x, int16_t y, const char *s, uint8_t on);
/** Width in pixels `s` would occupy at 1x / 2x. */
uint16_t SSD1306_TextWidth(const char *s);
uint16_t SSD1306_TextWidth2x(const char *s);

/* ---------------------------------------------------------------------------
 *  Big seven-segment numbers ("cyberpunk" look)
 * ------------------------------------------------------------------------- */

/**
 * @brief  Draw a big value made only of 0-9, '.' and '-' (anything else is a
 *         blank cell).  Returns the x position just past the last cell.
 */
int16_t  SSD1306_DrawBig(int16_t x, int16_t y, const char *s, uint8_t on);

/** Width in pixels SSD1306_DrawBig() would use for `s`. */
uint16_t SSD1306_BigWidth(const char *s);

/** CRT scanlines across a band - clears every `period`-th row. */
void     SSD1306_Scanlines(int16_t x, int16_t y, uint16_t w, uint16_t h,
                           uint8_t period);

#ifdef __cplusplus
}
#endif

#endif /* SSD1306_H */
