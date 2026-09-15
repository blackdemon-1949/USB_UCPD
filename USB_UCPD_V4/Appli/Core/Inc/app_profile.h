/**
  ******************************************************************************
  * @file    app_profile.h
  * @brief   Owner-programmable voltage profiles for the PC13 button.
  *
  * A profile is an ordered list of up to APP_PROFILE_MAX steps.  Each step is
  * either
  *
  *     FIXED  <pdo> [ma]     ask for source PDO <pdo> at an optional current
  *     PPS    <mv>  <ma>     ask for an exact PPS voltage in 20 mV steps
  *
  * The list is built from the serial terminal with the `profile` command and
  * is walked by a double press on PC13 (and by `profile next`).  With an empty
  * list the button keeps its built-in behaviour: step through the source's
  * SPR fixed PDOs.
  *
  * Every step goes through the existing APP_PD_SendRequest(), so the profile
  * cannot ask for anything the CLI could not already ask for, and the safety
  * limits in the PD stack still apply.
  *
 * Storage note: the list is persisted by the on-chip BKPSRAM backend
 * (apie_bkp.c) - the VBAT-domain backup SRAM at 0x38800000, enabled with
 * DBP + BKPRAMEN + the backup regulator.  `profile save` writes it (and it
 * rides along with every engine checkpoint), `profile load` re-reads it,
 * and a valid image is restored automatically at boot.  The external NOR is
 * still never written: the application executes from it (XiP), see
 * FLASH_ENDURANCE.md.
 ******************************************************************************
  */

#ifndef APP_PROFILE_H
#define APP_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Maximum number of steps in a profile. */
#define APP_PROFILE_MAX        8U

typedef enum
{
  APP_PROFILE_FIXED = 0,     /* fixed supply PDO, 1-based index */
  APP_PROFILE_PPS   = 1      /* PPS APDO, millivolts */
} app_profile_kind_t;

typedef struct
{
  uint8_t  kind;             /* app_profile_kind_t              */
  uint8_t  index;            /* FIXED: PDO index (1-based)      */
  uint16_t mv;               /* PPS:   target millivolts        */
  uint16_t ma;               /* requested current, 0 = PDO max  */
} app_profile_entry_t;

void APP_PROFILE_Init(void);

/** Number of steps currently defined. */
uint8_t APP_PROFILE_Count(void);

/** Read step `i` (0-based).  NULL when out of range. */
const app_profile_entry_t *APP_PROFILE_Get(uint8_t i);

/** Append a step.  Returns 1 on success, 0 when the list is full. */
uint8_t APP_PROFILE_AddFixed(uint8_t index, uint16_t ma);
uint8_t APP_PROFILE_AddPps(uint16_t mv, uint16_t ma);

/** Delete step `n` (1-based).  Returns 1 on success. */
uint8_t APP_PROFILE_Del(uint8_t n);

void APP_PROFILE_Clear(void);

/** Apply step `n` (1-based) right now.  Returns 1 if a request went out. */
uint8_t APP_PROFILE_Apply(uint8_t n);

/** Advance to the next step (wrapping) and apply it. */
uint8_t APP_PROFILE_Next(void);

/** Current 1-based position, 0 when the list is empty. */
uint8_t APP_PROFILE_Pos(void);
void    APP_PROFILE_SetPos(uint8_t n);

/** CLI: `profile ...`. */
void APP_PROFILE_Cli(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* APP_PROFILE_H */
