# USB_UCPD — Round 2 report (2026-09-08)

Branch: `arena/01a07fb0-usb-ucpd`
Base (round 1 tip): `c635a7a`
Scope: Bug A (CDC COM-port instability), Bug B (undefined), Bug C (CLI help),
Bug D (re-open `FIX_REPORT.md` §5 items 1–5 + a fresh sweep), standing
requirement 1 corrected (USART2, not USART1, in the third priority slot).

Round 1 is untouched: every commit below is appended on top of `c635a7a`.

---

## 1. Summary of what changed

| # | Commit | Area |
| --- | --- | --- |
| 1 | `6757239` | Standing req 1 — priority ordering corrected to **UCPD > CDC/USB > USART2 > rest** |
| 2 | `e167a0e` | Bug C — CLI help text now matches the real dispatcher |
| 3 | `28cfd8b` | Bug A(a) — serial descriptor built once, before the pull-up |
| 4 | `2d3d2a3` | Bug A(a) — serial descriptor can no longer be a run of NULs |
| 5 | `ef5c092` | Bug A(b)/(c) — never assert the pull-up on an unverified PHY |
| 6 | `e110406` | Bug A(b) — re-check `USB33RDY` immediately before connecting; `vbus_sensing_enable` documented as intentional |
| 7 | `8af8ff0` | Bug D sweep — 1 real OOB read, 2 latent defects, 1 build-time guard |

Bug B produced **no change** — see §3.

---

## 2. Bug A — CDC COM port instability

**Symptom as reported.** The device usually enumerates as a healthy COM8, but
sometimes — often right after a reset or power-cycle — Windows mints a new
COM10 that is always corrupt and only self-heals on the next reset.

### 2.1 What I could and could not establish

Being explicit, because this matters for how much weight to put on each fix.

**Established from the code (high confidence).**

* There is **no USB re-init path in Appli at all**. `MX_USB_DEVICE_Init()` is
  called exactly once, at `main.c:155`. Nothing in the tree calls `USBD_Stop`,
  `USBD_LL_DeInit`, `HAL_PCD_Stop` or `NVIC_SystemReset`. Candidate (c) — "a
  full clean disconnect on any re-init path" — therefore has **nothing to
  clean up**: there is no partial-reset window.
* The **bootloader→application warm handoff carries no USB state**. `Boot`
  disables `HAL_PCD_MODULE_ENABLED` and `HAL_HCD_MODULE_ENABLED`
  (`Boot/Core/Inc/stm32h7rsxx_hal_conf.h:53,68`) and contains no USB source
  file. It enables the USB HS *regulator* (`HAL_PWREx_EnableUSBHSregulator()`,
  `Boot/Core/Src/stm32h7rsxx_hal_msp.c:82`) but never the `USB_OTG_HS` or
  `USBPHYC` clocks. Across the jump the core is unclocked and the PHY is
  unpowered, so no pull-up is asserted during the handoff.
* The **clock configuration is correct** (unchanged from round 1, re-verified):
  `CCIPR1.USBPHYCSEL = 0` (HSE, 24 MHz) and `CCIPR1.USBREFCKSEL = 0xA`
  (`LL_RCC_USBREF_CLKSOURCE_24M == USBREFCKSEL_3|USBREFCKSEL_1`, 24 MHz in →
  ×2 → 48 MHz). HSE is 24 MHz per the `.ioc`.
* The **USB console TX engine is sound**. `app_log.c` clears `s_tx_busy` on
  connect, on transmit-complete, on suspend and on a `BUSY` return; the
  ring-buffer cursors are dropped for sinks that cannot drain. I looked hard
  for a "port exists but is dead" state machine bug here and did not find one.

**Answered by the owner (2026-09-08).** The board **can be powered from VBUS
and can also be powered externally**, and **it does not use VBUS sensing
pins** — there is no VBUS-sense pin wired to the OTG controller.

Two consequences, and they change the picture:

* `vbus_sensing_enable = DISABLE` is **correct and must stay that way**.
  Enabling it would make the core gate its session state on
  `GCCFG.VBUSBSEN`/`VBUSASEN` reading an unconnected pin, which can prevent
  the device from connecting at all. A comment now records this at the
  assignment so nobody "fixes" it.
* Bus-powered operation makes the supply ramp a first-class suspect. On a cold
  plug the sequence is VBUS → board 3.3 V → MCU out of reset, and the CPU
  starts executing as soon as VDD crosses the POR threshold — **well before
  the USB 3.3 V domain is stable**. That is the (b) race, and it now has a
  concrete mechanism (see §2.2, third bullet).

### 2.2 Fixes applied

**(a) Serial-number descriptor — two defects, both fixed.**

1. *Built too late.* `Get_SerialNum()` ran inside
   `USBD_CDC_SerialStrDescriptor()`, i.e. from the USB interrupt on the host's
   `GET_DESCRIPTOR(STRING)` request — after `USBD_Start()` had already
   asserted the pull-up. Windows keys the COM port number off VID+PID+serial,
   so any window in which that buffer is not yet filled presents a different
   identity.
   → `USBD_CDC_BuildSerialNum()` now fills it once, and
   `MX_USB_DEVICE_Init()` calls it **before** `USBD_Start()`. It is
   idempotent, and it is no longer touched from the USB ISR.

2. *Could be an invalid (NUL) serial.* The stock test
   `if (deviceserial0 != 0)` means that on a zero UID read the buffer keeps
   its static initialiser: twelve `U+0000` characters. **Windows rejects a
   serial number containing non-printable characters, discards the serial and
   falls back to deriving the device instance ID from the USB port path.** The
   COM number then depends on enumeration order and on how many devices
   Windows has previously seen on that path — *not* on the device. That is, in
   mechanism, precisely "usually COM8, sometimes a fresh COM10".
   → The descriptor is now always filled; if both UID words read as zero a
   constant is substituted so the string is never empty and never NUL-filled.
   On a healthy device the serial is byte-for-byte what it was before.

**(b) PHY/clock readiness gate — the pull-up is now conditional.**

`HAL_PCD_MspInit()` enabled the OTG core and let `USBD_Start()` connect
*unconditionally*:

* `HAL_RCCEx_PeriphCLKConfig()` failure → `Error_Handler()`, which never
  returns (solid LED, dead board — the same bricking pattern round 1 removed
  in `70135aa`);
* `HAL_PWREx_EnableUSBVoltageDetector()`'s return value was **discarded
  outright** — and that function is the only check that VDD33USB
  (`PWR_CSR2_USB33RDY`) is actually up.

Continuing into `HAL_PCD_Init()` with either of those failed touches an
unclocked/unpowered peripheral and then asserts the pull-up anyway. A device
that is attached but cannot answer `GET_DESCRIPTOR` is exactly what Windows
reports as "device descriptor request failed" / Code 10.

→ Both now set a module flag `s_usb_clock_ok = 0` and return; both muxes are
**read back** from `RCC->CCIPR1` and compared with what was programmed; and
`USBD_LL_Init()` refuses to program the FIFOs or start the core when the gate
is down. No pull-up is ever asserted on a PHY the firmware has not verified.
The PD sink, the USART2 console and the CLI keep running; only the USB console
is lost.

The same trap in `Appli/Core/Src/stm32h7rsxx_hal_msp.c:75` (called from
`HAL_Init()`) is removed for the same reason — a VDD33USB failure should not
hang the whole board.

**(b, supply ramp) A final gate in `USBD_LL_Start()`, at the last possible
moment.** `HAL_PCD_MspInit()` checks the detector, but that still runs inside
`HAL_PCD_Init()`, tens of microseconds before `HAL_PCD_Start()` clears
`DCTL.SFTDISCON`. `USBD_LL_Start()` now re-checks both the gate flag **and**
`PWR_CSR2.USB33RDY` immediately before connecting, and returns `USBD_FAIL`
instead of connecting if either is down.

This is the mechanism that best fits the reported pattern. On a cold
bus-powered plug, VDD33USB has to come up from nothing; `USB33RDY` is the only
supply-ready signal available without a VBUS-sense pin, and the old code
connected without ever looking at it. On a warm reset the domain is already
charged and `USB33RDY` is already set — **which is exactly why the fault
clears itself on the next reset.**

**(b) VBUS sensing: confirmed correct, comment added.** Given no VBUS-sense
pin, `hpcd_USB_OTG_HS.Init.vbus_sensing_enable = DISABLE` stays, with a
comment explaining why enabling it would break the device. Supply readiness is
tracked through `USB33RDY` instead.

**(b) Init order left alone, deliberately.** I considered moving
`MX_USB_DEVICE_Init()` after `MX_USBPD_Init()` so the Type-C layer runs first.
It buys nothing and I am not doing it: since the board can cold-start from
VBUS, it must have hardware Rd on CC (otherwise there would be no VBUS to boot
from — no power, no MCU, no Rd). VBUS is therefore present as soon as the cable
is plugged, independent of firmware, so the PD stack is not what brings the
supply up. Reordering would only delay the console and change log ordering.

**(b, secondary) CDC EP2 TX FIFO.** `HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 2,
0x40)` added. `CDC_CMD_EP` (0x82, the notification endpoint) is opened by
`USBD_CDC_Init`; with `DIEPTXF[1]` left at its reset value of depth 0 at
offset 0 it aliases the RX FIFO. ST's own H7RS reference omits this and the
endpoint is never transmitted on, so this is defensive, not a diagnosis.

**(b, diagnostics) `info` now prints the USB/PHY clock state**: the CCIPR1
read-back, `HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USBPHYC)` (want
24 000 000), `PWR->CSR2` `USB33RDY` / `USBHSREGEN`, and the gate result. This
is the only way to settle on the bench what static analysis cannot prove.

**(c)** Verified clean, no change needed — see §2.1.

### 2.3 What is *not* claimed

I have **not** proved which of the two (a) mechanisms actually fired on the
owner's bench. Both are real defects and both are fixed; neither can be
confirmed or excluded without hardware. The PHY gate in (b) removes a genuine
failure mode but I have no evidence it was firing — the voltage detector
passes at `HAL_Init()` time on a healthy board. Honest confidence:

| Fix | Confidence it was the bug | Confidence it is correct and safe |
| --- | --- | --- |
| Serial built before connect | Low–medium | High |
| Never emit a NUL serial | Medium | High |
| PHY/clock + `USB33RDY` readiness gate | **Medium–high** (raised, see below) | High |
| EP2 TX FIFO | Very low | High (defensive) |
| `info` clock read-back | n/a (diagnostic) | High |

The readiness gate is the one I now rate highest. With "bus-powered is a real
case" confirmed, there is a concrete, non-speculative mechanism: on a cold plug
the CPU runs before VDD33USB is stable, `USB33RDY` is the only available
supply-ready indicator, and the old code connected without ever reading it —
while a warm reset starts from an already-charged domain. That last detail is
the reported "only self-heals on the next reset". I still cannot *prove* it
fired, and it does not explain why the healthy case is the majority case
rather than the exception; a marginal ramp would be expected to correlate with
plug timing, which is worth watching on the bench.

**No delay or retry hack was added.** Every change is either a correctness fix
or a gate that refuses to connect on a *proven* bad state. The `USB33RDY` gate
is a readiness check on a hardware signal, not a `HAL_Delay()`.

---

## 3. Bug B — still not defined

Bug B is referenced in the round-2 directive but is **not defined in any root
document** (`README.md`, `DIAGNOSTICS.md`, `HARDWARE_VALIDATION.md`,
`SAFETY_MODEL.md`, `FIX_REPORT.md`) and not in the round-1 or round-2
directive text. Per standing requirement 4 I did not invent a definition.

**Status: TBD / blocked on the owner.** Please describe the Bug B symptom and
I will treat it as a must-fix in the same way as Bug A.

---

## 4. Bug C — CLI help text

`APP_CLI_PrintHelp()` was compared against `apie_cli_dispatch()`
(`app_cli.c:276–686`) and `handle_line()` (`app_cli.c:735+`), command by
command. Five real deltas, all fixed. **No command was invented and none
removed** — the help output was changed to match the dispatcher, not the
other way round.

| Help said | Reality |
| --- | --- |
| `ina` | `ina` **or** `ina226` (both accepted at `app_cli.c:767`) |
| `help` | `help` **or** `?` (`app_cli.c:777`) |
| `selftest quick\|full\|pd\|...` | `selftest [all\|quick\|full\|pd\|decoder\|ml\|database\|flash]` — `all` was missing |
| *(absent)* | `raw [clear\|dump [all]\|stats\|export]` exists bare, without the `ap` prefix |
| `diag pd\|ucpd\|usb\|queue\|...` | the dispatcher also accepts `rx`, `tx`, `txn`, `decoder`, `profile`, `unknown`, `knowledge` |

Added: a note that every `ap <sub>` command also works bare (with the
exception that bare `status` is the PD status, not `ap status`).

`tools/cli_coverage.py` and the dispatcher were re-read to confirm the
inventory; `pd stats|packets|state` delegation verified at `app_cli.c:754-764`.

---

## 5. Bug D — sweep

### 5.1 Re-opened `FIX_REPORT.md` §5 items 1–5

| # | Item | Round-2 verdict | Action |
| --- | --- | --- | --- |
| 1 | `_FRS` 150 µs busy-wait in the UCPD ISR (`usbpd_hw_if_it.c:254`) | Still dead code (`_FRS` is defined nowhere). Reassessed against Bug A: at UCPD priority 0 that wait stalls OTG_HS, so it remains a genuine latent Bug-A mechanism. | **Guarded, not forked** — `#error` added in the project-owned `usbpd_dpm_conf.h`, so enabling `_FRS` fails the build with an explanation instead of silently installing an enumeration killer. |
| 2 | Stale `SYSCFG_OTG_HS_PHY_CLK_SELECT_4` comment at `usbd_conf.c:75` | Confirmed wrong: STM32H7RS has **no** SYSCFG USB-PHY mux. | **Fixed** — rewritten with the real mechanism (`USBPHYCSEL` source + `USBREFCKSEL` frequency code 0xA). Load-bearing: a wrong justification invites reverting the fix that made enumeration work. |
| 3 | `Error_Handler()` in `HAL_PCD_MspInit` (`usbd_conf.c:92`) | Real, and worse than round 1 scored it — the return of `HAL_PWREx_EnableUSBVoltageDetector()` was also discarded. | **Fixed** — see §2.2. Exactly the round-1 suggested remedy (module flag, checked where a status can be returned). |
| 4 | PLL3 in the `.ioc` (`DIVM3=2 / DIVN3=34 / DIVQ3=17`), `Boot` sets `PLL3 = RCC_PLL_NONE` | Re-confirmed latent-only: `USBPHYCSEL` uses HSE, not PLL3Q. | **No change.** Still flagged so nobody assumes PLL3Q exists. |
| 5 | `README.md` stale figures (44/44 syntax checks, 148 240 B FLASH, old priority table) | Still stale. | **Logged, not corrected** — kept out of the firmware diff. Current figures are in §7 below; the priority table in `README.md:367` still describes the pre-round-1 scheme. |

### 5.2 Fresh sweep — auto-fixed

| File:line | Defect | Fix |
| --- | ---: | --- |
| `Appli/Core/Src/app_pd.c:1181` | `APP_PD_PrintModes()` loops `i < md->NumModes && i < 16U` over `md->Modes[]`, which is `Modes[MAX_MODES_PER_SVID]` with `MAX_MODES_PER_SVID == 6u` (`usbpd_def.h:1733`). **Indices 6–15 read past the array.** Found by cppcheck (`arrayIndexOutOfBounds`). | Bound changed to `MAX_MODES_PER_SVID`, mirroring the sibling `APP_PD_PrintSvids()` which already clamps to the real `SVIDs[12]` size. |
| `Appli/Core/Src/app_pd.c:581` | `APP_PD_SendRequest()` indexes `APP_PD_Port[port]` with no bounds check, unlike its sibling `APP_PD_Evaluate()` which has one. | Added `if (port >= USBPD_PORT_COUNT) return USBPD_ERROR;`. Hardening only — every caller passes 0 today. |
| `Appli/USBPD/Target/usbpd_pwr_user.c:499` | `BSP_USBPD_PWR_VBUSGetVoltage()` guards `(Instance >= N) \|\| (NULL == pVoltage)` but writes `*pVoltage` **after** the closing brace, so a NULL argument HardFaults instead of being rejected. Found by cppcheck (`nullPointerRedundantCheck`). | Write moved inside the `else`, matching `BSP_USBPD_PWR_VBUSGetCurrent()`. |
| `Appli/USBPD/Target/usbpd_pwr_user.c:388` | `BSP_USBPD_PWR_VBUSSetVoltage_Variable()` `__weak` definition takes `(…MinInmv, …MaxInmv)` while the declaration (`usbpd_pwr_user.h:200`) takes `(…MaxInmv, …MinInmv)`. Found by cppcheck (`funcArgOrderDifferent`). | Parameter names and `@param` docs swapped. Behaviour-neutral today (the body ignores both) but a silent swap trap. |

### 5.3 Fresh sweep — found, deliberately **not** changed

| # | File:line | Issue | Risk of changing | Suggested fix |
| --- | ---: | --- | --- | --- |
| 6 | `Boot/Core/Src/main.c:103` `JumpToApplication()` | Boot enables GPDMA1 and XSPI1 interrupts and leaves them **enabled and possibly pending**. It then sets `SCB->VTOR = APP_XIP_BASE`, restores PRIMASK (re-enabling interrupts) and branches — so an IRQ can fire into *Appli's* handler with Appli's handles still uninitialised, before `HAL_Init()` runs. | **Bootloader.** Non-deterministic and hard to trigger, but it is the only genuine cross-handoff hazard I found. | After `__disable_irq()` and before `__set_PRIMASK(primask_bit)`, clear `NVIC->ICPR[]` and write `NVIC->ICER[]` for every IRQ Boot enabled (or `HAL_NVIC_DisableIRQ(XSPI1_IRQn)` / the GPDMA1 channels). Appli re-enables what it needs. |
| 7 | `Appli/USB_DEVICE/Target/usbd_conf.c:483` | `hpcd_USB_OTG_HS.Init.vbus_sensing_enable = DISABLE`. The device asserts the D+ pull-up regardless of VBUS and never notices a host-side disconnect. | **Owner confirmed there is no VBUS-sense pin on the board**, so this is now *correct by construction*: enabling it would gate the session on `VBUSBSEN`/`VBUSASEN` reading an unconnected pin and could stop the device connecting at all. | **Leave DISABLE.** A comment now records the reason at the assignment so it is not "corrected" later. Supply readiness is tracked through `PWR_CSR2.USB33RDY` instead (see §2.2). |
| 8 | `Appli/USB_DEVICE/App/usbd_cdc_if.c:167` | `CDC_DeInit_HS()` calls `APP_LOG_OnUsbConnect()` — the wrong name for a de-init path. Harmless (the function only clears `s_tx_busy`), but it reads as a mistake and will mislead the next reader. | None functionally; cosmetic. | Call `APP_LOG_SetUsbReady(0)` and clear `s_tx_busy` explicitly, or rename the hook to something neutral such as `APP_LOG_OnUsbSessionChange()`. |
| 9 | `Appli/Core/Src/usart.c:55,59,63,67,92`; `i2c.c:51,58,65,90`; `dts.c:50` | CubeMX-generated `Error_Handler()` traps in `HAL_UART_Init` / `HAL_I2C_Init` / `HAL_DTS_Init` failure paths — the same bricking pattern, but on static configurations that cannot fail at runtime. | Low value; enlarges the diff without changing behaviour. | If parity is wanted, degrade to "log and continue with the peripheral unavailable" as was done for USB. |
| 10 | `Boot/Core/Src/w25qxx_xspi.c:224` | `W25QXX_Wait_Busy` unused (`-Wunused-function`). The only compiler warning in the tree. | Bootloader. | Delete the function, or mark it `__attribute__((unused))`. |

### 5.4 Verified-clean areas (no action)

* **`APP_PD_StoreSrcPDO`** clamps `n` to `USBPD_MAX_NB_PDO` (7), so every
  `NumberOfRcvSRCPDO`-bounded loop in `app_pd.c` is in range.
* **`build_rdo`** rejects `index == 0` and `index > NumberOfRcvSRCPDO` before
  indexing; `APP_PD_SendRequest` and `APP_PD_Evaluate` both check it.
* **`req` / `pps` CLI handlers** (`app_cli.c:796-857`) validate `argv` and
  route through `APP_PD_FindBestPdo` / `APP_PD_SendRequest`. No defect.
* **`USBPD_DPM_RequestMessageRequest`** (`usbpd_dpm_user.c:503`) — argument
  types match `APP_PD_SendRequest` exactly.
* **`CDC_Receive_HS`** re-arms the OUT endpoint unconditionally; the first
  receive is armed by `USBD_CDC_Init` after `CDC_Init_HS`. Correct.
* **`CDC_Control_HS`** guards `pbuf` and `length` on both line-coding
  requests, and reads `wValue` from the setup packet for
  `SET_CONTROL_LINE_STATE`. Correct.
* **`PCD_ResetCallback`** — the HAL assigns `hpcd->Init.speed` from
  `USB_GetDevSpeed()` *before* invoking the callback
  (`stm32h7rsxx_hal_pcd.c:1399` then `:1409`), so the speed it reports is the
  actual detected speed and the CDC bulk MPS follows it. The `else` branch
  that catches a garbage enumeration speed (0xF) is correct.
* **Log engine** (`app_log.c`) — see §2.1.

---

## 6. Standing requirement 1 — priority ordering

The round-1 fix was **confirmed intact in source before anything else was
changed**, then corrected for the USART1→USART2 error the owner flagged.

`irq_priority.h` now states `UCPD > CDC (USB OTG_HS) > USART2 > everything
else`, with `IRQ_PRIO_CONSOLE` (USART2) moved 4 → 2, `IRQ_PRIO_USART1`
2 → 3 and `IRQ_PRIO_TRACE_DMA` 3 → 4. `usart.c:121` applies
`IRQ_PRIO_CONSOLE` from inside a `USER CODE` block, so a CubeMX regeneration
cannot drop it. `usbpd_devices_conf.h:87` comment corrected to match.

Values verified **in the linked ELF**, not just in source:

| IRQ | Level |
| --- | ---: |
| UCPD1 | 0 |
| GPDMA1 Ch0 / Ch1 (UCPD DMA path) | 0 |
| OTG_HS (CDC/USB) | 1 |
| **USART2 (console)** | **2** |
| USART1 (TRACER_EMB trace) | 3 |
| GPDMA1 Ch2 (USART1 DMA) | 4 |
| default (I2C2/DTS/…) | 5 |
| SysTick | 15 |

`PRIGROUP = 3` (`NVIC_PRIORITYGROUP_4`: 4 pre-emption bits, 0 sub-priority
bits), applied in `main.c:130` after `HAL_Init()`.

---

## 7. Build and verification (actually run)

Toolchain: `python3 -m ziglang` (ARM) from `/home/user/.venv`.
Static analyser: **cppcheck 2.17.1**, installed and run as required.

```
$ bash tools/check_syntax.sh
syntax check: 55 passed, 0 failed

$ python3 tools/check_arm_build.py
== Boot:  35 C sources, -T STM32H7R3Z8JX_FLASH.ld
   compiled: 35 ok, 0 failed (1 with warnings)     <- pre-existing unused-function
   link: OK     FLASH 24776 B / 64 KB (37.81%)   RAM 312 B
== Appli: 86 C sources, -T STM32H7R3Z8JX_ROMxspi1.ld
   compiled: 86 ok, 0 failed (0 with warnings)
   link: OK
     FLASH                  196652 B /   8 MB  ( 2.34%)
     RAM                     34008 B / 440 KB  ( 7.55%)
     RAM_NONCACHEABLEBUFFER   5408 B /   8 KB  (66.02%)
     DTCM                        4 KB /  64 KB  ( 6.25%)
   Appli.elf  1352588 bytes
ARM build check: PASS
```

**cppcheck** (`--enable=warning,performance,portability --std=c99
-D__GNUC__`, project sources only): **3 findings, all false positives** —

* `Drivers/CMSIS/Include/cmsis_gcc.h:151` and `:157` — vendor CMSIS
  intrinsics; **not changed**.
* `Appli/Core/Src/sysmem.c:59` — compares the linker-script symbols `_end`
  and `_Heap_Limit`; **not changed** (linker scripts are off-limits).

The one real finding, `app_pd.c:1181 arrayIndexOutOfBounds`, is fixed.
(`-D__GNUC__` is mandatory: without it `cmsis_compiler.h:278` `#error
Unknown compiler` fires and cppcheck silently analyses almost nothing.)

**`tools/verify_irq_and_vectors.py`** — vector table `.isr_vector` at
`0x90000000`, 172 entries; every handler resolves to a real symbol, including
`OTG_HS` → `0x90015459`, `UCPD1` → `0x90015471`, `USART2` → `0x90015441`.
Priorities as tabulated in §6.

**Linker scripts: unchanged.** `STM32H7R3Z8JX_ROMxspi1.ld` and
`STM32H7R3Z8JX_FLASH.ld` are byte-identical to round 1.

**Bootloader: 2 files read, 0 files changed.**

---

## 8. What the owner must re-test on hardware

In priority order.

1. **Cold-plug vs warm-reset A/B** (the highest-value test, now that
   bus-powered operation is confirmed). Power the board **from VBUS only**,
   unplug the cable, wait ~30 s for the rails to discharge, then plug it in —
   20 times. Then repeat 20 times with a reset that does **not** remove VBUS
   (reset button / `NVIC_SystemReset`). Expected: identical behaviour in both.
   Before this change the cold-plug case was the one that misbehaved; if it
   still does, the `USB33RDY` gate is not the whole story and the next step is
   to capture the plug with a USB protocol analyser.
2. **Serial stability.** Run `info` and confirm the serial Windows reports is
   identical across 10+ power-cycles and 10+ `NVIC_SystemReset`s. In Device
   Manager → *Details* → *Device instance path*, or
   `Get-WmiObject Win32_SerialPort | Select DeviceID,PNPDeviceID`.
   Expected: `USB\VID_xxxx&PID_xxxx\<same 12 hex chars>` every time.
3. **COM-number stability.** Uninstall the device in Device Manager (tick
   "delete the driver"), then power-cycle 10×. Expected: the **same** COM
   number every time. Any new COM number means the serial is still not being
   accepted — send me the instance path and I will dig further.
4. **USB clock read-back.** Run `info`. Expected:
   `usbphyc_ker_ck = 24000000 Hz`, `USBPHYCSEL = 0`, `USBREFCKSEL = 0xA`,
   `USB33RDY = 1`, `USBHSREGEN = 1`, `usb clock gate = 1`.
   If `usb clock gate = 0`, USB deliberately did not start — record the other
   fields and send them to me; that is new information, not a failure.
5. **Enumeration speed.** Confirm the device consistently lands at
   high speed. A device that flips between HS and FS between boots changes
   the CDC bulk MPS (512 vs 64) and is a separate lead worth chasing.
6. **The original fault.** Power-cycle 20×, watching for COM10 / Code 10.
   If it recurs, capture a USB protocol trace (Ellisys/LeCroy/USBlyzer) of
   the failing `GET_DESCRIPTOR` — at that point the firmware is out of
   candidate mechanisms and the trace is the only way forward.
7. **`help` output.** Confirm the five corrected entries and the new "every
   `ap <sub>` also works bare" note; spot-check that `raw stats`, `?`, and
   `diag decoder` behave as documented.
8. **CLI regression.** `caps`, `req <n>`, `volt <mv>`, `pps <mv>` and the
   `modes <svid>` path (the OOB fix) should behave as before.

---

## 9. Confidence statement

* **Bug C** — high. The help text was compared against the dispatcher
  command-by-command; every change is a string literal in help output.
  Zero behavioural risk.
* **Standing req 1** — high. Verified in the linked ELF, not only in source.
* **Bug D sweep** — high for the four auto-fixed items (one is a genuine
  out-of-bounds read found by a real static analyser; three are
  consistency/robustness defects with no behaviour change on a healthy path).
  Items 6–10 are logged with file, line and reasoning rather than guessed at.
* **Bug A** — **medium, and deliberately stated as such.** Two real defects in
  the serial-number path were fixed and one real readiness gate was added. I
  believe the NUL-serial mechanism is the strongest explanation available for
  the *exact* reported symptom, but I cannot demonstrate that it fired, and I
  have not excluded a board-level cause (VBUS timing, cable, hub). What I can
  say is that no delay or retry hack was added, nothing was changed that could
  make a working board stop working, and the new `info` output plus §8.2–8.4
  will tell us definitively on the next bench session.
* **Bug B** — not started; the bug is not defined anywhere in the
  documentation or the directive.
