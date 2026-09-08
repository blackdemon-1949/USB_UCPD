#!/usr/bin/env bash
# Host-side verification of the unknown-protocol analyzer.
# Builds the real firmware apie_unknown.c against host stubs and runs the
# UNKNOWN_SIGNATURE characterization self-test.
set -u
cd "$(dirname "$0")/.."

gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I Appli/Core/Inc \
    tools/apie_unknown_selftest.c Appli/Core/Src/apie_unknown.c \
    -lm -o /tmp/apie_unknown_selftest || { echo "build FAILED"; exit 1; }

/tmp/apie_unknown_selftest
rc=$?
rm -f /tmp/apie_unknown_selftest
exit $rc
