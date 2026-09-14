/**
  ******************************************************************************
  * @file    app_cmos.h
  * @brief   Backup-SRAM "CMOS": every setting the owner can change, kept in
  *          VBAT-backed RAM (0x38800000) so it survives a reset or a power
  *          cycle, and is cleared back to factory defaults the moment the VBAT
  *          battery (or the coin cell) is removed.
  *
  * Layout (first 1 KB of the 4 KB backup SRAM; the rest belongs to apie_bkp):
  *
  *   0x000  slot A  (header + settings, CRC-32)
  *   0x180  slot B  (mirror, written alternately, higher sequence wins)
  *   0x300  reset/fault log ring (8 entries)
  *
  * Two mirrored slots mean a power loss in the middle of a write can never
  * destroy both copies: the loader picks the valid slot with the highest
  * sequence number and falls back to factory defaults when neither is valid
  * (which is exactly what a dead/removed battery produces).
  ******************************************************************************
  */
#ifndef APP_CMOS_H
#define APP_CMOS_H

#include <stdint.h>

#define APP_CMOS_MAGIC     0x434D4F53UL   /* 'CMOS' */
#define APP_CMOS_VERSION   1U
#define APP_CMOS_BASE      0x38800000UL
#define APP_CMOS_SIZE      0x00000400UL   /* 1 KB: slot A, slot B, log ring  */
#define APP_CMOS_SLOT_SIZE 0x00000180UL   /* 384 B per slot                  */
#define APP_CMOS_LOG_OFF   0x00000300UL
#define APP_CMOS_LOG_SIZE  0x00000100UL
#define APP_CMOS_FREE_OFF  0x00000400UL   /* apie_bkp window starts here     */
/* The last 16 bytes of the CMOS area hold a fault marker written by the fault
 * handlers themselves (no library calls: register writes only), so a crash is
 * still explained after the watchdog resets the chip. */
#define APP_CMOS_FAULT_OFF (APP_CMOS_SIZE - 16U)
#define APP_CMOS_FAULT_MAGIC 0x46414C54UL   /* 'FALT' */

/* Settings bit field ------------------------------------------------------- */
#define APP_SET_AUTO_REQUEST   0x0001U  /* apply target after attach        */
#define APP_SET_REMEMBER       0x0002U  /* re-apply the last request        */
#define APP_SET_STORE_ENABLED  0x0004U  /* allow writes to the NOR store    */
#define APP_SET_MONITOR        0x0008U  /* voltage/current failsafe active  */
#define APP_SET_PREFER_PPS     0x0010U  /* PPS over fixed at equal power    */
#define APP_SET_FALLBACK       0x0020U  /* step down instead of giving up   */
#define APP_SET_EPR_ALLOWED    0x0040U  /* EPR negotiation allowed          */
#define APP_SET_SAFE_MODE      0x0080U  /* set by the owner to skip auto PD */

/* Fault / reset codes recorded by the handlers ----------------------------- */
#define APP_FAULT_NONE         0U
#define APP_FAULT_HARD         1U
#define APP_FAULT_MEMMANAGE    2U
#define APP_FAULT_BUS          3U
#define APP_FAULT_USAGE        4U
#define APP_FAULT_WATCHDOG     5U
#define APP_FAULT_STACK        6U
#define APP_FAULT_ERROR_HANDLER 7U
#define APP_FAULT_INIT         8U   /* optional peripheral init failed  */
#define APP_FAULT_RAMFUNC      9U   /* NOR driver / RAM-mode fault       */

typedef struct
{
  uint16_t schema;          /* payload layout version                        */
  uint16_t flags;           /* APP_SET_*                                     */
  uint32_t target_mv;       /* wanted voltage in mV (0 = best safe default)  */
  uint32_t target_ma;       /* wanted current in mA                          */
  uint32_t max_mv;          /* never request above this (hard cap)           */
  uint32_t max_ma;          /* never request above this (hard cap)           */
  uint32_t min_mv;          /* never request below this (fallback floor)     */
  uint32_t ovp_mv;          /* monitor: trip above this Vbus (0 = off)       */
  uint32_t ocp_ma;          /* monitor: trip above this current (0 = off)    */
  uint32_t uv_mv;           /* monitor: trip below this Vbus (0 = off)       */
  uint32_t cmd_mask;        /* which CLI command groups the owner allows     */
  uint16_t cmd_count;       /* number of stored user commands (in the NOR)   */
  uint16_t retry;           /* fallback ladder depth                         */
  uint32_t boot_count;
  uint32_t last_fault;      /* APP_FAULT_* of the previous boot              */
  uint32_t fault_count;     /* consecutive abnormal resets                   */
  uint32_t abnormal_boots;
  uint32_t store_gen;       /* generation counter of the NOR store           */
  char     owner[16];       /* free-text label                               */
} app_settings_t;

void            APP_CMOS_Init(void);
app_settings_t *APP_CMOS(void);              /* live settings (never NULL)     */
uint8_t         APP_CMOS_Valid(void);        /* 1 = loaded from a good slot    */
uint32_t        APP_CMOS_Seq(void);          /* sequence of the loaded slot    */
void            APP_CMOS_Save(void);         /* write to the idle slot + CRC   */
void            APP_CMOS_FactoryReset(void); /* defaults + save                */
void            APP_CMOS_Print(void);

/* Reset/fault breadcrumbs (written by the fault handlers, read at boot). */
void     APP_CMOS_LogFault(uint32_t code, uint32_t extra);
uint32_t APP_CMOS_LastFault(void);
uint32_t APP_CMOS_FaultCount(void);
void     APP_CMOS_ClearFaults(void);
void     APP_CMOS_PrintFaults(void);

/* Called from the fault handlers: records code/extra/pc and returns. */
void     APP_CMOS_FaultMark(uint32_t code, uint32_t extra, uint32_t pc);
/* Called once at boot: moves a pending marker into the ring log. */
uint32_t APP_CMOS_TakeFaultMark(uint32_t *extra);

#endif /* APP_CMOS_H */
