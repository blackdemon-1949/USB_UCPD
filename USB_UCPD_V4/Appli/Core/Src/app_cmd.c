/**
  ******************************************************************************
  * @file    app_cmd.c
  * @brief   Owner command macros in the NOR store - see app_cmd.h.
  ******************************************************************************
  */
#include "app_cmd.h"
#include "app_store.h"
#include "app_cmos.h"
#include "app_cli.h"
#include "app_log.h"
#include "app_wdt.h"
#include <string.h>
#include <stdio.h>

#define CMD_NAME_MAX   16U
#define CMD_TEXT_MAX   96U

static uint16_t s_count;
static uint8_t  s_running;      /* guard against 'cmd run' recursing       */

static uint8_t cmd_pack(uint8_t slot, char *out, uint32_t max,
                        const char *name, const char *text)
{
  int n = snprintf(out, max, "%s|%s", name, text);
  return (uint8_t)((n > 0) && ((uint32_t)n < max));
}

int APP_CMD_Store(const char *name, const char *command)
{
  char rec[APP_STORE_MAX_PAYLOAD];

  if ((name == NULL) || (command == NULL) || (name[0] == '\0'))
  {
    return -1;
  }
  if (!APP_STORE_Ready())
  {
    APP_LOG_Write("cmd: NOR store unavailable (was it formatted? 'store format')\r\n");
    return -1;
  }
  if (strlen(name) >= CMD_NAME_MAX)
  {
    APP_LOG_Write("cmd: name too long\r\n");
    return -1;
  }
  if (strlen(command) >= CMD_TEXT_MAX)
  {
    APP_LOG_Write("cmd: command too long\r\n");
    return -1;
  }

  for (uint8_t slot = 0U; slot < APP_STORE_CMD_SLOTS; slot++)
  {
    char existing[APP_STORE_MAX_PAYLOAD];
    uint32_t len = 0U;

    if (APP_STORE_Read((uint8_t)(APP_STORE_T_CMD_BASE + slot), existing,
                       sizeof(existing), &len) != 0)
    {
      continue;
    }
    existing[(len < sizeof(existing)) ? len : (sizeof(existing) - 1U)] = '\0';
    if (strncmp(existing, name, strlen(name)) == 0 && (existing[strlen(name)] == '|'))
    {
      if (!cmd_pack(slot, rec, sizeof(rec), name, command))
      {
        return -1;
      }
      s_count = APP_CMD_Count();
      return APP_STORE_Write((uint8_t)(APP_STORE_T_CMD_BASE + slot), rec, strlen(rec));
    }
  }

  /* New name: use the next free slot. */
  if (s_count >= APP_STORE_CMD_SLOTS)
  {
    APP_LOG_Write("cmd: all slots used, delete one first\r\n");
    return -1;
  }
  for (uint8_t slot = 0U; slot < APP_STORE_CMD_SLOTS; slot++)
  {
    char probe[8];
    uint32_t len = 0U;

    if (APP_STORE_Read((uint8_t)(APP_STORE_T_CMD_BASE + slot), probe, sizeof(probe), &len) != 0)
    {
      if (!cmd_pack(slot, rec, sizeof(rec), name, command))
      {
        return -1;
      }
      if (APP_STORE_Write((uint8_t)(APP_STORE_T_CMD_BASE + slot), rec, strlen(rec)) != 0)
      {
        return -1;
      }
      APP_CMOS()->cmd_count = APP_CMD_Count();
      APP_CMOS_Save();
      return 0;
    }
  }
  return -1;
}

int APP_CMD_Fetch(const char *name, char *out, uint32_t max)
{
  for (uint8_t slot = 0U; slot < APP_STORE_CMD_SLOTS; slot++)
  {
    char rec[APP_STORE_MAX_PAYLOAD];
    uint32_t len = 0U;
    const char *bar;

    if (APP_STORE_Read((uint8_t)(APP_STORE_T_CMD_BASE + slot), rec, sizeof(rec), &len) != 0)
    {
      continue;
    }
    rec[(len < sizeof(rec)) ? len : (sizeof(rec) - 1U)] = '\0';
    bar = strchr(rec, '|');
    if (bar == NULL)
    {
      continue;
    }
    if ((strlen(name) == (size_t)(bar - rec)) && (strncmp(rec, name, strlen(name)) == 0))
    {
      snprintf(out, max, "%s", bar + 1);
      return 0;
    }
  }
  return -1;
}

uint16_t APP_CMD_Count(void)
{
  uint16_t n = 0U;

  for (uint8_t slot = 0U; slot < APP_STORE_CMD_SLOTS; slot++)
  {
    char probe[8];
    uint32_t len = 0U;

    if (APP_STORE_Read((uint8_t)(APP_STORE_T_CMD_BASE + slot), probe, sizeof(probe), &len) == 0)
    {
      n++;
    }
  }
  return n;
}

void APP_CMD_Init(void)
{
  s_count = APP_STORE_Ready() ? APP_CMD_Count() : 0U;
  APP_LOG_Printf("[cmd] %u user command(s) in the NOR store\r\n", (unsigned)s_count);
}

void APP_CMD_Print(void)
{
  APP_LOG_Printf("cmd: %u stored command(s) in the external flash\r\n", (unsigned)s_count);
  for (uint8_t slot = 0U; slot < APP_STORE_CMD_SLOTS; slot++)
  {
    char rec[APP_STORE_MAX_PAYLOAD];
    uint32_t len = 0U;

    if (APP_STORE_Read((uint8_t)(APP_STORE_T_CMD_BASE + slot), rec, sizeof(rec), &len) != 0)
    {
      continue;
    }
    rec[(len < sizeof(rec)) ? len : (sizeof(rec) - 1U)] = '\0';
    APP_LOG_Printf("      [%u] %s\r\n", (unsigned)slot, rec);
  }
}

void APP_CMD_Cli(int argc, char *argv[])
{
  if (argc < 2)
  {
    APP_CMD_Print();
    return;
  }

  if (strcmp(argv[1], "list") == 0)
  {
    s_count = APP_STORE_Ready() ? APP_CMD_Count() : 0U;
    APP_CMD_Print();
  }
  else if (strcmp(argv[1], "add") == 0)
  {
    char text[CMD_TEXT_MAX];
    uint32_t n = 0U;

    if (argc < 4)
    {
      APP_LOG_Write("usage: cmd add <name> <command...>\r\n");
      return;
    }
    text[0] = '\0';
    for (int i = 3; i < argc; i++)
    {
      uint32_t len = (uint32_t)strlen(text);
      int w = snprintf(&text[len], sizeof(text) - len, "%s%s", (i > 3) ? " " : "", argv[i]);
      if (w <= 0)
      {
        break;
      }
      n += (uint32_t)w;
      if (n >= (sizeof(text) - 1U))
      {
        break;
      }
    }
    if (APP_CMD_Store(argv[2], text) == 0)
    {
      APP_LOG_Printf("cmd: stored '%s' -> %s\r\n", argv[2], text);
    }
  }
  else if (strcmp(argv[1], "del") == 0)
  {
    if (argc < 3)
    {
      APP_LOG_Write("usage: cmd del <name>\r\n");
      return;
    }
    for (uint8_t slot = 0U; slot < APP_STORE_CMD_SLOTS; slot++)
    {
      char rec[APP_STORE_MAX_PAYLOAD];
      uint32_t len = 0U;

      if (APP_STORE_Read((uint8_t)(APP_STORE_T_CMD_BASE + slot), rec, sizeof(rec), &len) != 0)
      {
        continue;
      }
      rec[(len < sizeof(rec)) ? len : (sizeof(rec) - 1U)] = '\0';
      if ((strlen(argv[2]) < len) && (strncmp(rec, argv[2], strlen(argv[2])) == 0) &&
          (rec[strlen(argv[2])] == '|'))
      {
        (void)APP_STORE_Delete((uint8_t)(APP_STORE_T_CMD_BASE + slot));
        APP_LOG_Printf("cmd: deleted '%s'\r\n", argv[2]);
        s_count = APP_CMD_Count();
        APP_CMOS()->cmd_count = s_count;
        APP_CMOS_Save();
        return;
      }
    }
    APP_LOG_Write("cmd: no such name\r\n");
  }
  else if (strcmp(argv[1], "show") == 0)
  {
    char line[CMD_TEXT_MAX];

    if ((argc >= 3) && (APP_CMD_Fetch(argv[2], line, sizeof(line)) == 0))
    {
      APP_LOG_Printf("cmd: %s -> %s\r\n", argv[2], line);
    }
    else
    {
      APP_LOG_Write("cmd: no such name\r\n");
    }
  }
  else if (strcmp(argv[1], "run") == 0)
  {
    char line[CMD_TEXT_MAX];

    if (argc < 3)
    {
      APP_LOG_Write("usage: cmd run <name>\r\n");
      return;
    }
    if (s_running)
    {
      APP_LOG_Write("cmd: nested 'cmd run' refused\r\n");
      return;
    }
    if (APP_CMD_Fetch(argv[2], line, sizeof(line)) != 0)
    {
      APP_LOG_Write("cmd: no such name\r\n");
      return;
    }
    if (APP_WDT_InSafeMode())
    {
      APP_LOG_Write("cmd: safe mode - stored commands are not executed\r\n");
      return;
    }
    APP_LOG_Printf("cmd: %s -> %s\r\n", argv[2], line);
    s_running = 1U;
    APP_CLI_Execute(line);
    s_running = 0U;
  }
  else
  {
    APP_LOG_Write("usage: cmd [list|add <name> <command...>|del <name>|show <name>|run <name>]\r\n");
  }
}
