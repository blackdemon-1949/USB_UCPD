#!/usr/bin/env bash
# Syntax-check every C translation unit of the merged STM32CubeIDE projects
# against the project's REAL include paths, preprocessor defines and headers.
#
# The sandbox has no arm-none-eabi toolchain, so the host gcc is used with
# -fsyntax-only: it parses and type-checks every file against the actual
# CMSIS / HAL / USBPD / USB-Device headers shipped in the repository.  (The
# ARM inline asm in CMSIS only breaks at the assembler stage, which
# -fsyntax-only never reaches.)
set -u
cd "$(dirname "$0")/.."

CFLAGS="-fsyntax-only -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
        -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-sign-compare"

APPLI_INC="-IAppli/Core/Inc -IAppli/USBPD/App -IAppli/USBPD/Target \
 -IAppli/USB_DEVICE/App -IAppli/USB_DEVICE/Target \
 -IDrivers/STM32H7RSxx_HAL_Driver/Inc -IDrivers/STM32H7RSxx_HAL_Driver/Inc/Legacy \
 -IMiddlewares/ST/STM32_USBPD_Library/Core/inc \
 -IMiddlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc \
 -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
 -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
 -IDrivers/CMSIS/Device/ST/STM32H7RSxx/Include -IDrivers/CMSIS/Include"
APPLI_DEF="-DUSE_HAL_DRIVER -DSTM32H7R3xx -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_SNK -D_TRACE -DUSBPDCORE_LIB_PD3_FULL"

BOOT_INC="-IBoot/Core/Inc -IDrivers/STM32H7RSxx_HAL_Driver/Inc -IDrivers/STM32H7RSxx_HAL_Driver/Inc/Legacy \
 -IMiddlewares/ST/STM32_ExtMem_Manager -IMiddlewares/ST/STM32_ExtMem_Manager/boot \
 -IMiddlewares/ST/STM32_ExtMem_Manager/sal -IMiddlewares/ST/STM32_ExtMem_Manager/nor_sfdp \
 -IMiddlewares/ST/STM32_ExtMem_Manager/psram -IMiddlewares/ST/STM32_ExtMem_Manager/sdcard \
 -IMiddlewares/ST/STM32_ExtMem_Manager/user \
 -IDrivers/CMSIS/Device/ST/STM32H7RSxx/Include -IDrivers/CMSIS/Include"
BOOT_DEF="-DUSE_HAL_DRIVER -DSTM32H7R3xx"

fail=0; pass=0
check () { # $1 = label, $2 = incs, $3 = defs, $4.. = files
  local label="$1" incs="$2" defs="$3"; shift 3
  for f in "$@"; do
    if out=$(gcc $CFLAGS $incs $defs "$f" 2>&1); then
      pass=$((pass+1)); printf 'ok    %-52s %s\n' "$label" "$f"
    else
      fail=$((fail+1)); printf 'FAIL  %-52s %s\n' "$label" "$f"
      printf '%s\n' "$out" | sed 's/^/        /'
    fi
  done
}

check APPLI "$APPLI_INC" "$APPLI_DEF" $(find Appli/Core/Src Appli/USBPD Appli/USB_DEVICE -name '*.c' | sort)
check BOOT  "$BOOT_INC"  "$BOOT_DEF"  $(find Boot/Core/Src -name '*.c' | sort)

echo
echo "syntax check: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
