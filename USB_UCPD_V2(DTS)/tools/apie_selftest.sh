#!/usr/bin/env bash
# Host-side verification of the firmware PD decoder.
# Builds the actual firmware apie_decode.c against the host gcc and runs the
# self-test, so the decoder's field layout is verified independently of the
# cross-toolchain.
set -u
cd "$(dirname "$0")/.."

gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I Appli/Core/Inc \
    tools/apie_decode_selftest.c Appli/Core/Src/apie_decode.c \
    -o /tmp/apie_decode_selftest || { echo "build FAILED"; exit 1; }

/tmp/apie_decode_selftest
rc=$?
rm -f /tmp/apie_decode_selftest
exit $rc
