#ifndef APP_CLI_H
#define APP_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void APP_CLI_Init(void);
void APP_CLI_Poll(void);
void APP_CLI_OnRx(const uint8_t *data, uint32_t len);
void APP_CLI_OnHostOpen(void);
void APP_CLI_PrintBanner(void);
void APP_CLI_PrintHelp(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CLI_H */
