# Flashing PD Bench (WeAct STM32H7R3Z8)

You need STM32CubeIDE (to build) and STM32CubeProgrammer (to write both
images). The Puya PY25Q64HA is **not** in ST's stock loader list — use the
WeAct external loader shipped in `tools/`.

## 1. Build

1. File → Open Projects from File System → select this `USB_UCPD` folder.
2. Build **USB_UCPD_Boot** (Debug). ELF: `Boot/Debug/USB_UCPD_Boot.elf`
3. Build **USB_UCPD_Appli** (Debug). ELF: `Appli/Debug/USB_UCPD_Appli.elf`

Both Debug configurations already point at:

- Boot → `STM32H7R3Z8JX_FLASH.ld` (`0x08000000`, 64 KB)
- Appli → `STM32H7R3Z8JX_ROMxspi1.ld` (`0x90000000`, 8 MB)

## 2. Install the WeAct NOR loader

In STM32CubeProgrammer:

1. Open **External loaders**
2. Add `tools/STM32H7R3Z8Jx_8MB_WeAct.stldr`
3. Enable it for this session

The loader talks QuadSPI/XSPI to the Puya part using W25Q-compatible
commands. Stock ST SFDP loaders will fail or brick the map.

## 3. Connect

- SWD: `PA13` SWDIO, `PA14` SWCLK, GND, 3V3
- Board powered from USB-HS **or** a 3V3/5V header supply
- NRST if the probe provides it

## 4. Program Boot (internal FLASH)

- Address `0x08000000`
- File: `Boot/Debug/USB_UCPD_Boot.elf` (or `.bin`)
- Download

## 5. Program Appli (external NOR, mapped)

- Address `0x90000000`
- File: `Appli/Debug/USB_UCPD_Appli.elf`
- External loader **must** be selected
- Download

## 6. Reset and check

1. Reset the MCU.
2. PB2: brief flash during Boot, then Appli heartbeat (~1 Hz, 80 ms on).
3. Plug USB-HS Type-C into the PC. Device: **WeAct / PD Bench — H7R3 PD Sink**.
4. Open the serial port (115200 8N1). Banner + `help` should print.
5. Wire a powered USB-PD source to **one** MCU CC input and common ground:
   source CC1 → PM0 **or** source CC2 → PM1, plus GND. Do not connect both
   CC inputs together or connect the source to the USB-HS connector. Do not use the USB-HS
   Type-C for PD — that connector is CDC only.

### If it does not boot

Count PB2 blinks (see README fail codes).

| Symptom | Likely cause |
| --- | --- |
| 3 blinks | Appli not at `0x90000000`, or linker still on the MMT template |
| 1–2 blinks | NOR / mmap — wrong loader, or SFDP path re-enabled |
| CDC missing | `MX_USB_DEVICE_Init` / `USBD_Start` not in Appli, or USB-HS unplugged |
| PD silent | No powered USB-PD source, both CC inputs wired together, or source connected to USB-HS instead of one of PM0/PM1 |

## CubeIDE debug

- Debug **Boot** if you need the XiP bring-up (breakpoint before the jump).
- Debug **Appli** with “load image” disabled if Boot already mapped NOR;
  set VTOR/`0x90000000` in the debug configuration.
- Do not single-step across the Boot→Appli jump with caches enabled
  without invalidating I-cache.
