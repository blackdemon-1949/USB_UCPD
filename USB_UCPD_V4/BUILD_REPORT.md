# USB_UCPD_V4 — restart, root causes, fixes and build report

Branch: `arena/01a0a09b-usb-ucpd` (PR #1 → `MASTER`)
Base: the original `USB_UCPD_V4.zip` upload, re-extracted; `Boot/` untouched,
`Appli/STM32H7R3Z8JX_ROMxspi1.ld` kept as the application linker script and
`FLASH.ld` kept for the bootloader.

---

## 1. Why the previous drop was "broken even more"

Three independent defects, all of them *invisible in a compile* — each one
produces a board that looks dead with no console:

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

### 2.5 Console / CLI
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

## 4. Build result (`make clean && make -j2 all`, exit 0)

Only warning: `Boot/Core/Src/w25qxx_xspi.c:224: 'W25QXX_Wait_Busy' defined but
not used` — pre-existing, bootloader left alone on purpose.

### Application (`Appli/USB_UCPD_Appli.elf`)

| Region | Used | Size | % |
|---|---|---|---|
| FLASH (XiP) | 273 428 B | 8 MB | 3.26 % |
| RAM (AXI SRAM, cacheable) | 78 096 B | 440 KB | 17.33 % |
| RAM_NONCACHEABLEBUFFER | 7 712 B | 8 KB | 94.14 % |
| ITCM (NOR RAM-mode driver) | 1 904 B | 64 KB | 2.91 % |
| DTCM (MSP stack) | 4 KB | 64 KB | 6.25 % |
| SRAMAHB | 0 | 32 KB | 0 % |
| BKPSRAM (CMOS + store) | 4 KB @ `0x38800000` | 4 KB | addressed by pointer |

`text 272 812 / data 616 / bss 89 288`.

### Bootloader (`Boot/USB_UCPD_Boot.elf`, unmodified)

| Region | Used | Size | % |
|---|---|---|---|
| FLASH | 20 152 B | 64 KB | 30.75 % |
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
  assertions, but external-flash programming timings/status polling can only be
  confirmed with the PY25Q64HA fitted. First check: `store selftest`, then
  `store format`, then `learn save` / `learn load`.
* **DTS (LSE)**: `ext_dts.c` retries every second; if the LSE is not started by
  the bootloader the readout stays unavailable (non-fatal by design).
