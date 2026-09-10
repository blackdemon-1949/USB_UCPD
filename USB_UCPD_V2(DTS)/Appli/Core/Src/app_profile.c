/**
  ******************************************************************************
  * @file    app_profile.c
  * @brief   Owner-programmable voltage profiles - see app_profile.h.
  *
  * Deliberately boring: a fixed-size array, integer parsing, and one call into
  * the existing APP_PD_SendRequest().  Nothing here runs from an interrupt and
  * nothing here can touch the PD state machine directly.
  ******************************************************************************
  */

#include "app_profile.h"
#include "app_pd.h"
#include "app_log.h"
#include "apie_bkp.h"
#include "usbpd_def.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_profile_entry_t s_list[APP_PROFILE_MAX];
static uint8_t             s_count;
static uint8_t             s_pos;          /* 1-based, 0 = none */

/* ---------------------------------------------------------------------------
 *  Storage
 * ------------------------------------------------------------------------- */

void APP_PROFILE_Init(void)
{
  memset(s_list, 0, sizeof(s_list));
  s_count = 0U;
  s_pos   = 0U;
}

uint8_t APP_PROFILE_Count(void)
{
  return s_count;
}

const app_profile_entry_t *APP_PROFILE_Get(uint8_t i)
{
  if (i >= s_count)
  {
    return NULL;
  }
  return &s_list[i];
}

uint8_t APP_PROFILE_AddFixed(uint8_t index, uint16_t ma)
{
  if ((s_count >= APP_PROFILE_MAX) || (index == 0U))
  {
    return 0U;
  }
  s_list[s_count].kind  = (uint8_t)APP_PROFILE_FIXED;
  s_list[s_count].index = index;
  s_list[s_count].mv    = 0U;
  s_list[s_count].ma    = ma;
  s_count++;
  if (s_pos == 0U) { s_pos = 1U; }
  return 1U;
}

uint8_t APP_PROFILE_AddPps(uint16_t mv, uint16_t ma)
{
  if (s_count >= APP_PROFILE_MAX)
  {
    return 0U;
  }
  s_list[s_count].kind  = (uint8_t)APP_PROFILE_PPS;
  s_list[s_count].index = 0U;
  s_list[s_count].mv    = mv;
  s_list[s_count].ma    = ma;
  s_count++;
  if (s_pos == 0U) { s_pos = 1U; }
  return 1U;
}

uint8_t APP_PROFILE_Del(uint8_t n)
{
  uint8_t i;

  if ((n == 0U) || (n > s_count))
  {
    return 0U;
  }
  for (i = (uint8_t)(n - 1U); i < (uint8_t)(s_count - 1U); i++)
  {
    s_list[i] = s_list[i + 1U];
  }
  memset(&s_list[s_count - 1U], 0, sizeof(s_list[0]));
  s_count--;
  if (s_count == 0U)
  {
    s_pos = 0U;
  }
  else if (s_pos > s_count)
  {
    s_pos = 1U;
  }
  return 1U;
}

void APP_PROFILE_Clear(void)
{
  memset(s_list, 0, sizeof(s_list));
  s_count = 0U;
  s_pos   = 0U;
}

uint8_t APP_PROFILE_Pos(void)
{
  return (s_count == 0U) ? 0U : s_pos;
}

void APP_PROFILE_SetPos(uint8_t n)
{
  if (n <= s_count)
  {
    s_pos = n;
  }
}

/* ---------------------------------------------------------------------------
 *  Applying a step
 * ------------------------------------------------------------------------- */

/** Describe step `n` (1-based) into `out`. */
static void describe(uint8_t n, char *out, uint16_t outsz)
{
  const app_profile_entry_t *e = APP_PROFILE_Get((uint8_t)(n - 1U));

  if (e == NULL)
  {
    (void)snprintf(out, outsz, "  [%u] (empty)", (unsigned)n);
    return;
  }
  if (e->kind == (uint8_t)APP_PROFILE_FIXED)
  {
    if (e->ma != 0U)
    {
      (void)snprintf(out, outsz, "  [%u] FIXED  PDO %u  %u mA",
                     (unsigned)n, (unsigned)e->index, (unsigned)e->ma);
    }
    else
    {
      (void)snprintf(out, outsz, "  [%u] FIXED  PDO %u  (PDO max current)",
                     (unsigned)n, (unsigned)e->index);
    }
  }
  else
  {
    (void)snprintf(out, outsz, "  [%u] PPS    %u mV  %u mA",
                   (unsigned)n, (unsigned)e->mv, (unsigned)e->ma);
  }
}

uint8_t APP_PROFILE_Apply(uint8_t n)
{
  const app_profile_entry_t *e = APP_PROFILE_Get((uint8_t)(n - 1U));
  USBPD_StatusTypeDef st;
  uint8_t index;
  uint16_t mv;
  uint16_t ma;

  if (e == NULL)
  {
    return 0U;
  }
  if (APP_PD_IsAttached() == 0U)
  {
    APP_LOG_Write("profile: no source attached\r\n");
    return 0U;
  }
  if (APP_PD_Port[0].NumberOfRcvSRCPDO == 0U)
  {
    APP_LOG_Write("profile: no source capabilities yet - try getcaps\r\n");
    return 0U;
  }

  ma = e->ma;

  if (e->kind == (uint8_t)APP_PROFILE_FIXED)
  {
    if (e->index > APP_PD_Port[0].NumberOfRcvSRCPDO)
    {
      APP_LOG_Printf("profile step %u: PDO %u is not offered by this source\r\n",
                     (unsigned)n, (unsigned)e->index);
      return 0U;
    }
    index = e->index;
    mv    = 0U;
  }
  else
  {
    /* Ask the PDO table which APDO covers this voltage.  Same helper the
       'pps' and 'volt' commands use, so a profile cannot pick a PDO the CLI
       would reject. */
    uint8_t is_pps = 0U;

    index = 0U;
    APP_PD_FindBestPdo((uint32_t)e->mv, &index, &is_pps);
    if ((index == 0U) || (is_pps == 0U))
    {
      APP_LOG_Printf("profile step %u: %u mV is outside the source's PPS range\r\n",
                     (unsigned)n, (unsigned)e->mv);
      return 0U;
    }
    mv = e->mv;
  }

  st = APP_PD_SendRequest(0U, index, mv, ma);
  if (st != USBPD_OK)
  {
    APP_LOG_Printf("profile step %u: request not accepted by the stack (%d)\r\n",
                   (unsigned)n, (int)st);
    return 0U;
  }

  s_pos = n;
  {
    char line[64];
    describe(n, line, sizeof(line));
    APP_LOG_Printf("profile: applying%s\r\n", line);
  }
  return 1U;
}

uint8_t APP_PROFILE_Next(void)
{
  if (s_count == 0U)
  {
    return 0U;
  }
  s_pos++;
  if ((s_pos == 0U) || (s_pos > s_count))
  {
    s_pos = 1U;
  }
  return APP_PROFILE_Apply(s_pos);
}

/* ---------------------------------------------------------------------------
 *  CLI
 * ------------------------------------------------------------------------- */

static int parse_u(const char *s, unsigned long *out)
{
  char *end = NULL;
  unsigned long v;

  if ((s == NULL) || (*s == '\0'))
  {
    return -1;
  }
  v = strtoul(s, &end, 0);
  if ((end == s) || (end == NULL) || (*end != '\0'))
  {
    return -1;
  }
  *out = v;
  return 0;
}

static void print_usage(void)
{
  APP_LOG_Write(
    "usage:\r\n"
    "  profile                 list the steps\r\n"
    "  profile add fixed <pdo> [ma]   step to a fixed supply PDO (1..7)\r\n"
    "  profile add pps <mv> [ma]      step to an exact PPS voltage\r\n"
    "  profile del <n>                delete step n\r\n"
    "  profile clear                  delete every step\r\n"
    "  profile apply <n>              apply step n now\r\n"
    "  profile next                   apply the next step (what PC13 does)\r\n"
    "  profile pos [n]                show or set the current position\r\n"
    "  profile save|load              persist to / restore from the BKPSRAM\r\n"
    "                                 backup store (VBAT domain, if fitted)\r\n");
}

void APP_PROFILE_Cli(int argc, char *argv[])
{
  if (argc < 2)
  {
    char line[64];

    if (s_count == 0U)
    {
      APP_LOG_Write("profile: empty - PC13 double press steps the source's fixed PDOs\r\n");
      APP_LOG_Write("         add steps with:  profile add fixed <pdo> [ma]\r\n");
      APP_LOG_Write("                          profile add pps <mv> [ma]\r\n");
      return;
    }
    APP_LOG_Printf("profile: %u step(s), position %u\r\n",
                   (unsigned)s_count, (unsigned)s_pos);
    for (uint8_t n = 1U; n <= s_count; n++)
    {
      describe(n, line, sizeof(line));
      APP_LOG_Printf("%s%s\r\n", line, (n == s_pos) ? "   <- next" : "");
    }
    APP_LOG_Write("PC13: one press = next page, two presses = next step\r\n");
    return;
  }

  if (strcmp(argv[1], "add") == 0)
  {
    if (argc >= 5 && strcmp(argv[2], "pps") == 0)
    {
      unsigned long mv = 0UL;
      unsigned long ma = 0UL;

      if ((parse_u(argv[3], &mv) != 0) ||
          (argc >= 5 && parse_u(argv[4], &ma) != 0))
      {
        APP_LOG_Write("usage: profile add pps <mv> [ma]\r\n");
        return;
      }
      if (!APP_PROFILE_AddPps((uint16_t)mv, (uint16_t)ma))
      {
        APP_LOG_Printf("profile is full (%u steps)\r\n", (unsigned)APP_PROFILE_MAX);
        return;
      }
      APP_LOG_Printf("profile: added PPS %lu mV", mv);
      if (ma != 0UL) { APP_LOG_Printf(" at %lu mA", ma); }
      APP_LOG_Write("\r\n");
      return;
    }
    if (argc >= 4 && strcmp(argv[2], "fixed") == 0)
    {
      unsigned long idx = 0UL;
      unsigned long ma  = 0UL;

      if ((parse_u(argv[3], &idx) != 0) || (idx == 0UL) || (idx > 7UL) ||
          (argc >= 5 && parse_u(argv[4], &ma) != 0))
      {
        APP_LOG_Write("usage: profile add fixed <pdo 1-7> [ma]\r\n");
        return;
      }
      if (!APP_PROFILE_AddFixed((uint8_t)idx, (uint16_t)ma))
      {
        APP_LOG_Printf("profile is full (%u steps)\r\n", (unsigned)APP_PROFILE_MAX);
        return;
      }
      APP_LOG_Printf("profile: added FIXED PDO %lu", idx);
      if (ma != 0UL) { APP_LOG_Printf(" at %lu mA", ma); }
      APP_LOG_Write("\r\n");
      return;
    }
    APP_LOG_Write("usage: profile add fixed <pdo> [ma]  |  profile add pps <mv> [ma]\r\n");
    return;
  }

  if (strcmp(argv[1], "del") == 0)
  {
    unsigned long n = 0UL;

    if ((argc < 3) || (parse_u(argv[2], &n) != 0) || !APP_PROFILE_Del((uint8_t)n))
    {
      APP_LOG_Write("usage: profile del <n>\r\n");
      return;
    }
    APP_LOG_Printf("profile: deleted step %lu\r\n", n);
    return;
  }

  if (strcmp(argv[1], "clear") == 0)
  {
    APP_PROFILE_Clear();
    APP_LOG_Write("profile: cleared\r\n");
    return;
  }

  if (strcmp(argv[1], "apply") == 0)
  {
    unsigned long n = 0UL;

    if ((argc < 3) || (parse_u(argv[2], &n) != 0))
    {
      APP_LOG_Write("usage: profile apply <n>\r\n");
      return;
    }
    (void)APP_PROFILE_Apply((uint8_t)n);
    return;
  }

  if (strcmp(argv[1], "next") == 0)
  {
    if (!APP_PROFILE_Next())
    {
      APP_LOG_Write("profile: empty - add steps first (or use PC13 for the built-in\r\n"
                    "         fixed-PDO stepping)\r\n");
    }
    return;
  }

  if (strcmp(argv[1], "pos") == 0)
  {
    if (argc >= 3)
    {
      unsigned long n = 0UL;
      if (parse_u(argv[2], &n) != 0)
      {
        APP_LOG_Write("usage: profile pos [n]\r\n");
        return;
      }
      APP_PROFILE_SetPos((uint8_t)n);
    }
    APP_LOG_Printf("profile: position %u of %u\r\n",
                   (unsigned)APP_PROFILE_Pos(), (unsigned)s_count);
    return;
  }

  if ((strcmp(argv[1], "save") == 0) || (strcmp(argv[1], "load") == 0))
  {
    /* Persistence is the on-chip BKPSRAM backend (apie_bkp.c): the same
       VBAT-domain store that holds the engine's learned data also carries
       the owner profile list, so the list survives resets (and power loss
       when VBAT + the backup regulator hold the domain up).  NOR is still
       not written: XiP safety (FLASH_ENDURANCE.md). */
    if (strcmp(argv[1], "save") == 0)
    {
      if (APIE_Bkp_Save("profile-cmd") != 0U)
      {
        APP_LOG_Printf("profile: saved (%u step(s)) to the BKPSRAM backup store.\r\n",
                       (unsigned)s_count);
      }
      else
      {
        APP_LOG_Write("profile: save FAILED - BKPSRAM backend not available "
                      "(run 'selftest flash' to see why).\r\n");
      }
    }
    else
    {
      if (APIE_Bkp_LoadProfiles() != 0U)
      {
        APP_LOG_Printf("profile: restored %u step(s) from the BKPSRAM backup store.\r\n",
                       (unsigned)s_count);
      }
      else
      {
        APP_LOG_Write("profile: load FAILED - no valid image in the BKPSRAM "
                      "backup store.\r\n");
      }
    }
    return;
  }

  print_usage();
}
