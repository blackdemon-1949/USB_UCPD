/* Host-test shim for the application's main.h: the store layer only needs the
 * millisecond tick, and pulling the real HAL/CMSIS tree into a native build
 * would drag in the whole device header. */
#ifndef MAIN_H
#define MAIN_H
#include <stdint.h>
uint32_t HAL_GetTick(void);
#endif
