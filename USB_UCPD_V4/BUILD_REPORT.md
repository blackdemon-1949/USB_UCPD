# USB_UCPD_V4 — restart, root causes, fixes and build report

Branch: `arena/01a0a09b-usb-ucpd` (PR #1 → `MASTER`)
Base: the original `USB_UCPD_V4.zip` upload, re-extracted; `Boot/` untouched,
`Appli/STM32H7R3Z8JX_ROMxspi1.ld` kept as the application linker script and
`FLASH.ld` kept for the bootloader.

---

## 1. Root causes

Three independent defects, all of them *invisible in a compile* — each one
produces a board that looks dead with no console.

Where they come from:

Provenance, checked against the committed upload (`USB_UCPD_V4.zip`):

* defects 2 and 3 are in the **upload itself**. `main.c` defines `Appli_Fail()`
  as `__disable_irq(); while(1) { blink }`, `Error_Handler()` calls it, so any
  bring-up error is a permanently dead board - the virtual board reproduces it:
  the uploaded tree stops (interrupts off) at ~86 k instructions in
  `MX_DTS_Init → Error_Handler → Appli_Fail`. `usbd_conf.c` already gated
  `USBD_LL_Init()` on `PWR_CSR2.USB33RDY` (`s_usb_clock_ok`) and
  `usb_device.c` brought the device up exactly once: if that one attempt lands
  while the rail is still rising, the console is gone for the whole power
  cycle.
* defects 1 and 1.1 come with the **external-NOR driver added in this work**
  (the same two mistakes were in the rejected v1). The upload has no
  `ext_nor.c`/`app_store.c`/`app_cmos.c`/`app_cmd.c`/`app_wdt.c`/`app_fault.c`
  at all - those modules are what replaces the SD card.

| # | Defect | Consequence |
|---|--------|-------------|
| 1 | **`.ramfunc` was an orphan section.** `ext_nor.c` marked its XSPI RAM-mode driver `.ramfunc`, but the linker script collects `.RamFunc` (capital R) into `.data`. The linker therefore placed `.ramfunc` **inside the XiP flash window** (`0x90036848`, `ext_nor_xspi_run` at `0x90036DFF`). | The moment anything touched the external flash (the boot-time JEDEC probe is enough), the driver switched XSPI out of memory-mapped mode *while executing from it*. The next instruction fetch came from a disabled window → HardFault / lockup. No console, no startup, on every board with the NOR chip fitted. |
| 2 | **Init failures were fatal.** `Error_Handler()` (LED blink + `for(;;)`, interrupts off) was reached from ordinary peripheral bring-up paths such as `HAL_PWREx_EnableUSBVoltageDetector()` timing out because `PWR_CSR2.USB33RDY` was not ready yet. | A rail that is a few milliseconds late = permanently dead board. This is the "no CDC, no startup in **any** condition" report: the failure is deterministic, not intermittent. |
| 3 | **CDC had a single attempt.** `USBD_LL_Init()` refuses to hand the core to the stack while the USB 3.3 V detector reports not-ready; nothing ever tried again. | If the attempt lands in that window, the human interface is gone for the whole power cycle even though the PD stack is fine. |

Defect 1 is now **proven** statically (section addresses in the ELF) and its
failure signature is reproduced in the virtual board; defects 2 and 3 are
reproduced end-to-end in the virtual board (`Appli_Fail` lockup on the original
tree, full boot with the fixes).

## 2. What was changed

### 1.1 A second real defect, found by modelling the flash

Modelling the XSPI controller (`tools/vboard_nor.py`) turned the store from
"answers *no device*" into a working device, and immediately exposed this:

```c
/* ext_nor.c, nor_run() - before */
if ((op != 0U) && ((off < EXT_NOR_STORE_OFF) || ...))   /* any non-read op */
{
  s_errors++;
  return EXT_NOR_ERR_OFF;
}
```

The reserved-window guard was meant for **writes**, but `op != 0` also covered
the JEDEC-id (op 3) and status (op 4) reads, which legitimately address offset
0. So the one call that makes the device known - the id probe in
`EXT_NOR_Init()` - was refused with `EXT_NOR_ERR_OFF`, `s_ready` stayed 0 and
`EXT_NOR_Init()` always returned 0: **the persistent store could never come up
on any board, with or without the chip fitted**. Every `store`, `cmd`, `learn`
and settings path answered "unavailable". Fixed: the guard now applies to the
write operations (program/erase) only, and `EXT_NOR_Read()` keeps re-checking
its own window.

### 2.0 New files relative to `USB_UCPD_V4.zip`

`ext_nor.c/h`, `app_store.c/h`, `app_cmos.c/h`, `app_wdt.c/h`, `app_cmd.c/h`,
`app_fault.c/h`, `app_config.h` (feature switches), plus the console commands
in `app_cli.c` and the boot-order/failsafe changes in `main.c`. Everything in
the upload is still in place and still built; `Boot/` is byte-identical.

### 2.1 External NOR driver can no longer strand the CPU
* `.ramfunc` is now a real output section `>ITCM AT>FLASH` in `ROMxspi1.ld`.
  ITCM is CPU-local, always executable, never cached, and outside every MPU
  region the application programs.
* The startup file enables ITCM (`SCB->ITCMCR`, `0xE000EF50`) and copies the
  section from its load address before `__libc_init_array()`.
* The driver is 17 functions / 1904 B, **zero calls leave the window** (checked
  with a capstone disassembly audit), so it is genuinely self-contained: no
  libc, no HAL, no floating point, interrupts masked by the caller.
* The XSPI scratch buffer moved to `.nor_scratch` inside the non-cacheable
  window (`RW_NONCACHEABLE`, NOLOAD) — the XSPI controller reads it as a bus
  master and would otherwise see stale D-cache lines.
* Three link-time assertions turn the whole class of bug into a **link error**:
  `.ramfunc` must be in ITCM, must be non-empty, and the scratch must be in the
  non-cacheable window. Verified by negative test — the linker stops with
  `BUG: .ramfunc must be linked into ITCM (0x00000000..0x0000FFFF)`, exit 2.

### 2.2 No-init-failure-is-fatal failsafes
* `APP_INIT_Fail(tag, status)` records a CMOS breadcrumb (with a 4-character
  tag) and continues; every peripheral bring-up path uses it
  (DTS, I2C2, USART2, OLED, NOR, ...). The board boots and prints
  `[init] DTS failed (status=2) - continuing without it` instead of dying.
* Fault handlers write a breadcrumb (`CFSR`, `HFSR`, `LR`, tag) and reset. The
  reset is logged on the next boot (`[boot] previous boot ended in fault N`).
* A fault taken **while the XiP window is off** is recorded as
  `APP_FAULT_RAMFUNC` (not a generic HardFault), and the next boot then
  deliberately skips the external flash (`[boot] store skipped ...`) until the
  operator runs `store selftest` / `store on`.
* IWDG (~4 s) is fed only by the super loop; three abnormal boots in a row put
  the firmware into safe mode (optional subsystems off) instead of a boot loop.

### 2.3 CDC bring-up: one attempt → bounded retries
* `MX_USB_DEVICE_Init()` makes one attempt; `USB_DEVICE_Task()` in the super
  loop retries up to 5 times, 1 s apart, non-blocking. Each retry resets the
  PHY/VDD33 gate (`s_usb_clock_ok`), tears the core down cleanly
  (`USBD_DeInit`) and never leaves a half-attached device behind (no D+ pull-up
  without a valid PHY clock = no Windows "Code 10").
* New console commands: `usb status`, `usb retry` (spends an extra attempt).

### 2.4 Fault log / CMOS hygiene
* Erased (all-ones) backup-SRAM slots are no longer printed as faults.
* `cmos faults` decodes the real `APP_FAULT_*` symbols (`init-fail`,
  `nor-ramfunc`) and prints the failing subsystem tag, e.g.
  `boot 1: init-fail extra=0x00445453` (`"DTS"`).
* A factory reset (battery removed) wipes the fault ring as well as the
  settings, so a fresh board shows `(fault ring empty ...)`, never 16 rows of
  `0xFFFFFFFF` noise.

### 2.5 The flash allocation that replaces the SD card

`EXT_NOR_STORE_OFF = 0x700000`, `EXT_NOR_STORE_SIZE = 1 MB` (256 x 4 KB
sectors) inside the 8 MB PY25Q64HA, everything below it is application code
(the driver refuses to run if `_etext` reaches into the window):

| record type | id | use |
|---|---|---|
| settings mirror | 1 | backup copy of the CMOS settings |
| learned signatures | 2 | the ML/learn database |
| decision-model state | 3 | model parameters |
| owner profiles | 4 | profile list |
| event log / packet log | 5 / 6 | text and binary logs |
| signature CSV export | 7 | what `learn export` prints |
| user command macros | 0x40..0x7F | 64 slots (`cmd add/run/del`) |
| learned-signature chunks | 0x80..0x8F | 16 x 1 KB chunks |

Each record carries its type, length and CRC-32; 1 MB is ~4000 x 256-byte
records, far more than the learned data and the 64 command slots need. The
window is one contiguous block so `store format` can rebuild it in place, and
a record that fails to write latches writes off instead of corrupting the
superblock.

### 2.6 Console / CLI
Added on top of the existing CLI: `cmos [save|reset|faults|clear]`,
`store [status|events|selftest|format|on|off|save|load]`, `cmd [list|add|show|del|run]`,
`wdt [kick]`, `safe [on|off]`, `usb [status|retry]`. The ready banner reports
`pd=`, `store=`, `safe=`, `init-fails=`, `store-skipped=`.

## 3. How it was verified (no board on this machine)

`tools/vboard.py` + `tools/vboard_cli.py` emulate the STM32H7R3 in Unicorn:
ELF loaded straight from the build, virtual SysTick/HAL tick, the H7RS
peripheral map (note: APB2 is `0x42000000` on this family, *not* the classic-H7
`0x40011000` — getting that wrong makes the PD tracer look stuck), USART TX
captured to a log, `PWR_CSR2.USB33RDY`/`RCC_CR.HSERDY` modelled, and the CLI
ring buffer fed exactly the way the RX interrupt feeds it.

Results on this build:

* boots from reset to `[boot] ready` (console live, PD up, watchdog running);
* `usb: CDC console ready (vbus/phy=1, try 1)`;
* same boot with the USB rail modelled as late: attempt 1 fails cleanly,
  attempt 2 succeeds — `usb: CDC console ready (vbus/phy=1, try 2)`;
* every new command answered (`cmos`, `cmos faults`, `store`, `store selftest`,
  `wdt`, `usb`, `cmd list`), no fault/`Error_Handler`/trap taken in 19–61 M
  emulated instructions;
* the original tree in the same harness dies at ~86 k instructions in
  `MX_DTS_Init` → `Error_Handler` → `Appli_Fail` (interrupts off, forever).

Reproduce:

```
python3 tools/vboard_cli.py USB_UCPD_V4/Appli/USB_UCPD_Appli.elf \
        --commands "cmos;cmos faults;store;store selftest;wdt;usb;cmd list"
```

### 3.1 The external flash, exercised instead of assumed

`tools/vboard_nor.py` adds a model of the XSPI1 controller and one NOR device
to the virtual board: CR/SR/DLR/AR/DR/CCR/TCR/IR/ABR/LPTR, CR.ABORT
self-clearing, FMODE (indirect vs memory-mapped - which is also what arms a
transaction, so the register restore at the end of a job cannot be mistaken for
a command), SR.TCF/FLEVEL/BUSY, the commands the driver uses (0x9F, 0x05, 0x06,
0x04, 0x03/0x0B, 0x02, 0x20, 0xD8, 0xC7) and an 8 MB array with
program-only-clears-bits semantics. `DWT->CYCCNT` advances on every read, so
the driver's cycle-based timeouts can expire instead of hanging the emulator.

What the model shows on this build (command trace as printed by `--nor-trace`):

```
IR=0x9F AR=0x000000 DLR=3      JEDEC id read      -> 0x856017, device found
IR=0x06 AR=0x000000 DLR=0      WREN
IR=0x05 AR=0x000000 DLR=0      RDSR               -> WEL set
IR=0x20 AR=0x7FF000 DLR=0      4 KB erase, self test sector
IR=0x02 AR=0x7FF000 DLR=31     page program, 32 B
IR=0x0B AR=0x7FF000 DLR=31     read back          -> pattern matches
IR=0x20 AR=0x700000 DLR=0      store superblock: erase first data sector
IR=0x02 AR=0x700000 DLR=31     write the 'EXST' header
IR=0x0B AR=0x701000..0x7FF000  scan the 256 sector headers
```

The saved store window contains exactly what the driver meant to write:

```
@0x700000  "EXST" 01 00 00 00 | 00 01 00 00 | 00 10 70 00 | f0 66 1e 65 ...
           magic   version      record count  free sector  CRC-32
@0x7FF000  a5 a4 a7 a6 a1 a0 a3 a2 ...      (the self-test pattern 0xA5^i,
                                              read back and verified)
```

and the console confirms it from the running firmware:

```
$ store
  store: ready (formatted now), head=0x701000 sector=0 records=0 writes=0
         window 0x700000..0x800000 (1024 KB), nor: ext-nor: ready, id=0x856017 errors=0
$ store selftest
  store: NOR self test PASS
```

so the probe, the erase/program/read-back self test, the superblock write and
the header scan all work end to end - before the fix the same command answered
`unavailable ... nor: ext-nor: no device on XSPI1` on a board that has the
chip fitted.

Two caveats, stated plainly:

* the model was written from the driver's own register sequence, so it
  validates the **software** (no dead loop, window arithmetic, sector
  boundaries, record persistence, the latched-off-on-error policy) - not the
  chip's real timing, status semantics or electrical behaviour. Those still
  need the board (`store selftest` first).
* Unicorn in M-profile mode cannot fetch instructions from address
  `0x00000000`, which is where `.ramfunc` lives (`ITCM`). For these runs the
  image was relinked with the ITCM region based at `0x00001000` (identical
  code, same offsets - the startup copy uses `_siramfunc/_sramfunc/_eramfunc`,
  so it is base-independent). The **shipped** `ROMxspi1.ld` and its ELF are
  unchanged: ITCM at `0x00000000` is the region's real address and what the
  original script specified.

### 3.2 The record layer, tested on the host (fast, and it covers the reboot)

`tools/store_host_test.c` compiles `Appli/Core/Src/app_store.c` natively against
an in-RAM NOR with the chip's semantics (4 KB sector erase, programming only
clears bits, an error path that can be armed), so the parts that are pure
software can be checked in milliseconds instead of minutes of emulator time:

```
gcc -std=c11 -Wall -I tools/host_shim -I USB_UCPD_V4/Appli/Core/Inc \
    -o /tmp/store_host_test tools/store_host_test.c \
       USB_UCPD_V4/Appli/Core/Src/app_store.c && /tmp/store_host_test
```

35 checks, all passing: mounting a blank chip formats it (one erase + the
superblock); every record type is written and read back with the payload
intact; 400 event records and a full 1 KB record are accepted; **re-initialising
the store - the reboot path - finds all the records again and writes nothing**
(that is the persistence guaranteed by the external flash instead of the SD
card); writing past the end of a sector rolls over into a fresh one (48 sector
erases) and records from the first sector stay readable; an armed flash error
makes the write *refuse* instead of leaving a half-written record behind, and
the store still mounts afterwards; `format` clears the window.

The same run exposed a small reporting bug, now fixed: when the store mounts
through its CMOS head hint it skips the scan, so `records=` was printed as 0
even with records present. It now prints `records=(not scanned)` (or the count
after a real scan) instead of a misleading zero.

### 3.3 What the emulator gets wrong (so its numbers are not taken as gospel)

Every console line in 3.1 was produced by block execution of the virtual board
(20 000 instructions per `emu_start`), and that shortcut is not faithful. The
harness was chasing missing digits in `cmos` output - `seq=,` instead of
`seq=0,`, `ovp=mV` instead of `ovp=0mV` - and the answer turned out to be the
emulator, not the firmware:

* a hook on the firmware's own `APP_LOG_WriteRaw` shows the string handed over
  *already* missing the zero, and a hook on `vsnprintf` shows the format string
  is the expected `"... seq=%lu, ..."`;
* a stub injected into the loaded image that calls the firmware's own
  `APP_LOG_Printf` prints `[A=][B=7][C=0][D=0]` for `"[A=%lu][B=%lu][C=%u][D=%ld]"`
  with arguments 0/7/0/0 - only the `%lu`-of-zero cases lose their digit;
* the *same* stub, run with one instruction per `emu_start` instead of blocks,
  prints `[A=0][B=7][C=0][D=0]`.

So Unicorn's cached translation of that conversion path skips the branch that
gives a lone `0` its digit: block mode steps straight over `lsls`/`uxth.w`/`bpl`
at `0x900344bc..0x900344c3` in `vfprintf`. It is intermittent - an earlier
block-mode transcript of the same `store` answer (3.1) came out complete, a
later one lost the digits - which is what a translation-block artefact looks
like, and why it took a stub and a single-stepped run to pin down. The firmware
is correct: on silicon `seq=0` and `ovp=0mV` are printed.

`tools/vboard_cli.py --precise` therefore single-steps the command phase: the
boot stays fast (block mode), the answers become exact. Same firmware, same
flash image, same command, only the stepping changed:

```
block mode   (boot banner)   [store] ready: mounted, head=0x701000, records=, writes=

precise      ($ store)       store: ready, head=0x701000 sector=0 records=0 writes=0
                             window 0x700000..0x800000 (1024 KB), nor: ext-nor: ready, id=0x856017 errors=0
```

Everything the report claims about *stored data* does not depend on the
transcript either - it is read back out of the modelled flash image and checked
byte by byte.

## 4. Build result (from scratch: `rm -rf build && make -j4 all`, exit 0)

0 compilation errors, 0 linker errors. Only warning:
`Boot/Core/Src/w25qxx_xspi.c:224: 'W25QXX_Wait_Busy' defined but not used` —
pre-existing, bootloader left alone on purpose. The two assembler notices
("end of file not at end of a line, newline inserted") come with the vendor
startup files and are not ours to fix.

The same build was repeated on a **fresh export of the pushed commit**
(`git archive HEAD | tar -x`, 385 files, empty build directory) and produced
byte-identical numbers, so nothing needed to build is missing from the branch:

```
             RAM:       78096 B       440 KB     17.33%     (Appli)
           FLASH:      270440 B         8 MB      3.22%
             RAM:         300 B       455 KB      0.06%     (Boot)
           FLASH:       20148 B        64 KB     30.74%
```

The host test of 3.2 also passes from that export (35/35).

### Application (`Appli/USB_UCPD_Appli.elf`)

| Region | Used | Size | % |
|---|---|---|---|
| FLASH (XiP) | 270 440 B | 8 MB | 3.22 % |
| RAM (AXI SRAM, cacheable) | 78 096 B | 440 KB | 17.33 % |
| RAM_NONCACHEABLEBUFFER | 7 712 B | 8 KB | 94.14 % |
| ITCM (NOR RAM-mode driver) | 1 904 B | 64 KB | 2.91 % |
| DTCM (MSP stack) | 4 KB | 64 KB | 6.25 % |
| SRAMAHB | 0 | 32 KB | 0 % |
| BKPSRAM (CMOS + store) | 4 KB @ `0x38800000` | 4 KB | addressed by pointer |

`text 269 824 / data 616 / bss 89 288`.

### Bootloader (`Boot/USB_UCPD_Boot.elf`, unmodified)

| Region | Used | Size | % |
|---|---|---|---|
| FLASH | 20 148 B | 64 KB | 30.74 % |
| RAM | 300 B | 455 KB | 0.06 % |
| DTCM | 2 KB | 64 KB | 3.12 % |

Note: the non-cacheable window is 94 % full because it now also carries the
2 KB XSPI scratch buffer. It is a hard limit — the linker fails on overflow.

## 5. Feature inventory (compiled and reachable)

* USB-PD sink only, PD3.0/EPR-capable core variant
  (`USBPDCORE_LIB_PD3_FULL` ⇒ `USBPDCORE_EPR`, `USBPDCORE_PPS`,
  `USBPDCORE_DRP`, `USBPDCORE_FASTROLESWAP`, `USBPDCORE_DATA_SWAP`,
  `USBPDCORE_USBDATA`), VDM/e-marker discovery enabled
  (`PE_VDMSupport`, `PE_RespondsToDiscovSOP`, `PE_AttemptsDiscovSOP`).
* EPR/AVS and cable identity are decoded and reported (`cable`, `epr`,
  `identify`, `svids`, `modes`); EPR **power** stays gated by
  `APIE_HW_EPR_POWER_ENABLED` (0) — requesting >21 V is not enabled on this
  board revision, see §6.
* RoleSwap/DataSwap are off in `usbpd_dpm_conf.h` on purpose: this is a sink
  port with no role-swap wiring.
* PPS/APDO requests, sweeps, autos, `req/volt/pps/caps`, INA226 monitoring,
  OLED page, learn/ML packet engine, persistent command macros (`cmd`),
  settings in battery-backed SRAM (`cmos`), watchdog + safe mode (`wdt`,
  `safe`), external NOR store (`store`), full CDC console.
* SD/FatFs: present in the project but disabled (`APP_SD_ENABLED 0`), so no SD
  code is compiled or linked and nothing can fail on a missing card.

## 6. Still needs the bench

* **ER >21 V / EPR power**: the sink policy for EPR is not enabled because the
  board's VBUS feedback and protection for the 28 V range has not been
  qualified here; the decode/display path is complete and the flag is one line.
* **NOR writes on real silicon**: the RAM-mode path is now correct by
  construction (ITCM, self-contained, interrupts masked) and guarded by
  assertions; the indirect-mode sequences, the erase/program/read-back self
  test and the superblock write are verified against a modelled device, and the
  record layer is unit-tested on the host. What a model cannot cover is the
  chip's real timing, status semantics and electrical behaviour, plus one
  performance number worth measuring: a boot-time scan of the 1 MB window costs
  ~5000 indirect reads (roughly 0.1-0.2 s at typical NOR timings) - if that
  matters, the CMOS hint already lets a normal boot skip the scan entirely.
  First check on the bench: `store selftest`, then `store format`, then
  `cmd add` / `cmd list` and `learn save` / `learn load`.
* **DTS (LSE)**: `ext_dts.c` retries every second; if the LSE is not started by
  the bootloader the readout stays unavailable (non-fatal by design).
