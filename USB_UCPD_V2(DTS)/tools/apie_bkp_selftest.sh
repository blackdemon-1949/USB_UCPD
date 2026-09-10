#!/usr/bin/env bash
# Host-side verification of the APIE BKPSRAM persistence backend.
# Builds the real firmware apie_bkp.c (hardware path compiled out: a RAM
# array stands in for the backup SRAM window) together with the real
# apie_db.c / apie_ml.c / apie_stats.c against the host gcc, and runs the
# blob round-trip / CRC-gate self-test.
set -u
cd "$(dirname "$0")/.."

gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -DAPIE_BKP_HW_ENABLED=0 \
    -I Appli/Core/Inc \
    tools/apie_bkp_selftest.c \
    Appli/Core/Src/apie_bkp.c \
    Appli/Core/Src/apie_db.c \
    Appli/Core/Src/apie_ml.c \
    Appli/Core/Src/apie_stats.c \
    -o /tmp/apie_bkp_selftest || { echo "build FAILED"; exit 1; }

/tmp/apie_bkp_selftest
rc=$?
rm -f /tmp/apie_bkp_selftest
exit $rc
