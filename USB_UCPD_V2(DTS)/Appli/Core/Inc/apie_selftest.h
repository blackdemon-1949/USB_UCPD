/**
  ******************************************************************************
  * @file    apie_selftest.h
  * @brief   One-command, all non-destructive self-test.
  *
  * `selftest` runs every check that cannot damage hardware or disturb the
  * active PD contract.  It never issues a power request, never changes the
  * voltage, never erases/programs NOR, and never transmits unknown packets.
  *
  * Scopes: all, quick, full, pd, decoder, ml, database, flash.
  ******************************************************************************
  */
#ifndef APIE_SELFTEST_H
#define APIE_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct
{
  uint16_t pass;
  uint16_t fail;
  uint8_t  ok;         /* 1 = all passed */
  uint32_t ms;         /* elapsed ms     */
} APIE_SelfTestResult_t;

/* Run the requested scope.  scope: "all","quick","full","pd","decoder",
   "ml","database","flash" (NULL => "all").  Prints a report. */
void APIE_SelfTest_Run(const char *scope);

#ifdef __cplusplus
}
#endif

#endif /* APIE_SELFTEST_H */
