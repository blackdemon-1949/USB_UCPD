/**
 * @file    dtsmon.h
 * @brief   On-die Digital Temperature Sensor (DTS) monitor and CLI.
 *
 * Reads the SoC die temperature and reports it on the console in degrees C
 * or degrees F, with the same periodic-reporting model the INA226 monitor
 * uses (`dts auto on|off`, `dts period <ms>`).
 *
 * The driver plugs into the DTS footprint (ext_dts.c) by overriding the two
 * weak hooks EXT_DTS_FeatureInit / EXT_DTS_FeaturePoll - main.c does not need
 * to know about it.
 *
 * Fault tolerant by design: the HAL only returns whole degrees, every read is
 * non-blocking, and if the sensor is not producing conversions (the .ioc
 * clocks it from the LSE, which the Boot project does not start - see
 * ext_dts.h) the firmware keeps running and simply reports "no reading".
 */
#ifndef DTSMON_H
#define DTSMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* --- last measurement ----------------------------------------------------- */
/** 1 = the sensor has produced at least one reading since boot. */
uint8_t DTSMON_HasReading(void);
/** Last die temperature in whole degrees Celsius. */
int32_t DTSMON_GetTempC(void);
/** Last die temperature in whole degrees Fahrenheit. */
int32_t DTSMON_GetTempF(void);
/** 1 = last reading is younger than ~2 sample periods. */
uint8_t DTSMON_DataFresh(void);
/** 1 = reporting in Fahrenheit. */
uint8_t DTSMON_UnitIsF(void);

/* --- console -------------------------------------------------------------- */
/** Print a one-shot temperature line (or the "no reading" message). */
void DTSMON_Print(void);
/** Short status line for the `status` command. */
void DTSMON_PrintStatus(void);
/**
 * `dts` CLI command:              print one measurement
 *   dts auto on|off              periodic reporting (default on, 1 s)
 *   dts period <ms>              periodic interval, 250..60000 ms
 *   dts unit c|f                 report in degrees C or degrees F
 *   dts status                   sensor / clock state
 */
void DTSMON_Cli(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* DTSMON_H */
