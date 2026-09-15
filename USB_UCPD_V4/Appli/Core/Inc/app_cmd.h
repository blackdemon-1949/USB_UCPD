/**
  ******************************************************************************
  * @file    app_cmd.h
  * @brief   Owner-defined command macros ("my command allocation").
  *
  * Each macro is one NOR-store record holding 'name|command line'.  Up to
  * APP_STORE_CMD_SLOTS of them live in the reserved external-flash window, so
  * they survive a power cycle, a battery pull and a firmware update, and they
  * are never part of the flash image.  'cmd run <name>' feeds the stored line
  * straight back into the console parser.
  ******************************************************************************
  */
#ifndef APP_CMD_H
#define APP_CMD_H

#include <stdint.h>

void APP_CMD_Init(void);
void APP_CMD_Cli(int argc, char *argv[]);   /* cmd list|add|del|run|show        */
void APP_CMD_Print(void);

/* Programmatic access (used by the loader and by tests). */
int  APP_CMD_Store(const char *name, const char *command);
int  APP_CMD_Fetch(const char *name, char *out, uint32_t max);
uint16_t APP_CMD_Count(void);

#endif /* APP_CMD_H */
