# USB_UCPD — Windows 11 USB enumeration failure: analysis, fixes and verification

Target: **`USB_UCPD_V2(DTS)`** (STM32H7R3Z8Jx, XIP from external NOR at `0x90000000`,
USB Type-C UCPD sink + USB CDC virtual COM + USART console).
Branch: `arena/01a07fb0-usb-ucpd` · base commit `210b37a`.

Symptom reported: on Windows 11 the device enumerates as *unknown device* —
"device descriptor request failed" / Code 10.

---

## 1. Executive summary

Two suspected root causes were investigated independently. **One was confirmed
and fixed; the other was investigated to the register level and found to be
already correct.**

| Suspected cause | Verdict | Action |
| --- | --- | --- |
| **Interrupt priority misordering** | **CONFIRMED — real defect found** | Fixed (commit `3e2640b`) |
| **USB PHY / 48 MHz clocking** | **NOT A DEFECT — verified correct at register level** | No change; math shown in §3 |

The confirmed defect is not merely "the wrong order at the top of the table".
The single highest-priority interrupt in the whole system was **the USBPD trace
DMA channel (GPDMA1 Channel 2)** — higher than UCPD1 *and* higher than the USB
OTG_HS controller — as a result of a missing macro definition that silently
selected a `0` fallback. That channel is active from the moment
`MX_USBPD_Init()` runs, i.e. throughout every subsequent enumeration attempt.
This is the mechanism that most plausibly produces exactly the reported symptom
on Windows 11 while leaving the PD stack apparently working.

A second, smaller fix (commit `70135aa`) removes a code path that turned a
recoverable USB init failure into a permanently dead board.

---

## 2. Root cause 1 — NVIC priorities (confirmed, fixed)

### 2.1 Priority grouping

`HAL_Init()` installs `NVIC_PRIORITYGROUP_4`. On this part
`__NVIC_PRIO_BITS == 4`, so `PRIGROUP = 3` gives:

* 4 bits of **pre-emption** priority (0…15, 0 = most urgent)
* 0 bits of sub-priority

Consequence: the second argument to `HAL_NVIC_SetPriority()` **is** the
pre-emption level verbatim, and `NVIC_EncodePriority(grouping, n, 0) == n`.
The grouping was already adequate (16 pre-emption levels for 7 used
interrupts) and was **not** changed — but it is now restated explicitly in
`main()` (`HAL_NVIC_SetPriorityGrouping(IRQ_PRIORITY_GROUP)`) so the levels
cannot silently change meaning after a CubeMX regeneration.

### 2.2 Before / after table

Verified from the pre-processed sources (§6.2 tool output) and cross-checked
against the linked ELF vector table.

| Interrupt | IRQn | Vector slot | **Before** | **After** | Macro |
| --- | ---: | ---: | ---: | ---: | --- |
| UCPD1 | 128 | 144 | 5 | **0** | `IRQ_PRIO_UCPD` |
| GPDMA1 Ch0 (UCPD RX) | 39 | 55 | 5 | **0** | `IRQ_PRIO_UCPD_DMA` |
| GPDMA1 Ch1 (UCPD TX) | 40 | 56 | 5 | **0** | `IRQ_PRIO_UCPD_DMA` |
| **OTG_HS (USB CDC)** | 91 | 107 | 4 | **1** | `IRQ_PRIO_CDC_USB` |
| USART1 (PD trace) | 82 | 98 | 6 | **2** | `IRQ_PRIO_USART1` |
| GPDMA1 Ch2 (trace DMA) | 41 | 57 | **0** ⚠ | **3** | `IRQ_PRIO_TRACE_DMA` |
| USART2 (console) | 83 | 99 | 0 → 7 | **4** | `IRQ_PRIO_CONSOLE` |
| SysTick | −1 | 15 | 15 | 15 | HAL default |

Required ordering — **`UCPD > CDC > USART1 > everything else`** — is now
satisfied with no exceptions.

### 2.3 The actual defect: `TRACER_EMB_TX_DMA_PRIORITY` was never defined

`middlewares/.../tracer_emb_hw.c` (in `Appli/Core/Src/tracer_emb_hw.c`):

```c
#ifdef TRACER_EMB_TX_DMA_PRIORITY
    NVIC_SetPriority(TRACER_EMB_TX_DMA_IRQ, TRACER_EMB_TX_DMA_PRIORITY);
#else
    NVIC_SetPriority(TRACER_EMB_TX_DMA_IRQ, 0);   /* <-- taken */
#endif
```

`TRACER_EMB_TX_DMA_PRIORITY` was **not defined anywhere in the tree**, so the
`#else` branch ran and forced **GPDMA1 Channel 2 to priority 0** — numerically
the most urgent interrupt on the device, above UCPD1 (5) *and* above OTG_HS (4).

This silently undid the deliberate `6` that `gpdma.c` had just programmed for
the same channel, because `MX_USBPD_Init()` runs **after** `MX_GPDMA1_Init()`.

Why this produces the reported symptom rather than a total failure:

* `_TRACE` is defined in `Appli/.cproject` (Debug **and** Release), so the trace
  path is compiled in and this call executes on every boot.
* The trace DMA fires continuously while the PD stack is running, and at
  priority 0 it **pre-empts the USB OTG_HS ISR and everything else**.
* Windows 11 enumeration timing is tight: the host issues `GET_DESCRIPTOR`
  immediately after bus reset, and the ST device stack answers it from the
  OTG_HS interrupt/`USBD_LL_*` path. A pre-empted control-endpoint response
  shows up precisely as *"device descriptor request failed"* / Code 10 /
  unknown device.
* A bulk trace stream is bursty, so the window is marginal — which also
  explains the "works after several replugs / intermittent" character the
  README describes.

**Fix:** `Appli/Core/Inc/tracer_emb_conf.h` now defines
`TRACER_EMB_TX_DMA_PRIORITY` as `IRQ_PRIO_TRACE_DMA` (3) — below USART1 (2),
which is the UART that channel actually feeds. The dead `#else` branch is now
unreachable.

### 2.4 The ordering inversion at the top of the table

Before: OTG_HS = 4, UCPD1 = 5 — USB ranked **above** UCPD. That contradicts the
hard requirement (UCPD must be highest, no exceptions) and was corrected by
moving UCPD1 and its two DMA channels to 0 and OTG_HS to 1.

Note this is *not* the mechanism behind the enumeration failure — moving the USB
interrupt from 4 to 1 is a strict improvement, but the enumeration killer was
the priority-0 trace DMA pre-empting it, not the relative rank of UCPD.

### 2.5 A runtime re-application path that earlier fixes missed

`Appli/USBPD/Target/usbpd_devices_conf.h` defines `UCPD_INSTANCE0_ENABLEIRQ`,
which the CAD layer executes **at runtime** (attach/detach), not only at init.
It re-programs the UCPD1 priority, so anything set only in `ucpd.c` is
overwritten later. Both sites now use `IRQ_PRIO_UCPD`.

### 2.6 Centralised definition

New file **`Appli/Core/Inc/irq_priority.h`** is the single source of truth for
every priority in the firmware, with the ordering contract documented at the
top. All seven call sites were converted to its macros.

---

## 3. Root cause 2 — USB / PHY clocking (investigated; **no defect found**)

### 3.1 Clock tree as configured

There is exactly one `SystemClock_Config()` in the tree — in **Boot**
(`Boot/Core/Src/main.c:224`, called at `:164`). Appli inherits it.

`Boot/Core/Src/main.c`:
```c
RCC_OscInitStruct.OscillatorType   = RCC_OSCILLATORTYPE_HSE;
RCC_OscInitStruct.HSEState         = RCC_HSE_ON;          /* crystal, not bypass */
RCC_OscInitStruct.PLL1.PLLState    = RCC_PLL_ON;
RCC_OscInitStruct.PLL1.PLLSource   = RCC_PLLSOURCE_HSE;
RCC_OscInitStruct.PLL1.PLLM = 2;  .PLLN = 50;  .PLLP = 1;
RCC_OscInitStruct.PLL3.PLLState    = RCC_PLL_NONE;
```

**Math:**
```
HSE                       = 24 000 000 Hz      (stm32h7rsxx_hal_conf.h:104 HSE_VALUE 24000000UL
                                                and .ioc RCC.HSE_VALUE=24000000)
VCO1 input  = HSE / PLLM  = 24 MHz / 2  =  12 MHz   (.ioc VCOInput1Freq_Value  = 12 000 000 ✓)
VCO1 output = 12 MHz × 50 =                600 MHz
PLL1_P      = 600 MHz / 1 =                600 MHz   (.ioc DIVP1Freq_Value     = 600 000 000 ✓)
```

### 3.2 USBPHYC path — register level

`Appli/USB_DEVICE/Target/usbd_conf.c:84-94` (`HAL_PCD_MspInit`):
```c
LL_RCC_SetUSBREFClockSource(LL_RCC_USBREF_CLKSOURCE_24M);   /* USER CODE */
PeriphClkInit.PeriphClockSelection  = RCC_PERIPHCLK_USBPHYC;
PeriphClkInit.UsbPhycClockSelection = RCC_USBPHYCCLKSOURCE_HSE;
__HAL_RCC_USB_OTG_HS_CLK_ENABLE();
__HAL_RCC_USBPHYC_CLK_ENABLE();
```

Register decode (`Drivers/CMSIS/.../stm32h7r3xx.h:15025-15043`,
`Drivers/STM32H7RSxx_HAL_Driver/Inc/stm32h7rsxx_ll_rcc.h:851-857`):

| Field | Bits | Value written | Meaning |
| --- | --- | --- | --- |
| `RCC->CCIPR1.USBPHYCSEL` | [13:12] | `0b00` = `RCC_USBPHYCCLKSOURCE_HSE` | USBPHYC kernel clock ← **HSE** |
| `RCC->CCIPR1.USBREFCKSEL` | [11:8] | `0b1010` = `LL_RCC_USBREF_CLKSOURCE_24M` | declares the reference as **24 MHz** |

The LL header documents `USBREFCKSEL` as *"Configure USBREF clock frequency"*
(`stm32h7rsxx_ll_rcc.h:3352`) — it is a **frequency declaration** for the
USBPHYC PLL, not another source mux. The legal encodings are all the standard
USB reference frequencies, and every one of them multiplies to 48 MHz:

```
0b0011 = 16   MHz  × 3.000 = 48 MHz
0b1000 = 19.2 MHz  × 2.500 = 48 MHz
0b1001 = 20   MHz  × 2.400 = 48 MHz
0b1010 = 24   MHz  × 2.000 = 48 MHz   <-- this project
0b1110 = 26   MHz  × 1.846 = 48 MHz
0b1011 = 32   MHz  × 1.500 = 48 MHz
0b0000 = reset value, NOT a legal selection
```

**USB clock math for this board:**
```
USBPHYC ref   = HSE                 = 24.000 MHz
USBREFCKSEL   = 0b1010 (24 MHz)     -> PLL multiplier ×2
48 MHz out    = 24.000 MHz × 2      = 48.000 MHz   (0 ppm nominal)
```

**Before / after:** *unchanged.* The value was already correct when I started —
`0b1010` matching a 24 MHz HSE is exactly right, and the two fields agree with
each other. There is **no clock defect to fix** and none was introduced.

### 3.3 Corroborating evidence

* **CubeMX's own clock solver agrees**: `USB_UCPD.ioc` line 455
  `RCC.USBPHYFreq_Value=24000000` and line 454 `RCC.USBOFSFreq_Value=48000000`.
  The generated intention and the compiled code match.
* **ST's reference project matches**: ST's `NUCLEO-H7S3L8 / USB_Device /
  CDC_Standalone` `HAL_PCD_MspInit` uses the identical
  `RCC_PERIPHCLK_USBPHYC` / `RCC_USBPHYCCLKSOURCE_HSE` pair. This project is a
  strict superset (it additionally sets `USBREFCKSEL`, which ST omits).
* **HSE is provably running**: it is the source of PLL1, which produces the
  600 MHz CPU clock. If HSE were not oscillating or `USBPHYCSEL` were dead,
  the board would not boot at all — yet the PD stack and console demonstrably
  run. So the PHY reference is physically present and at the right frequency.
* **The undocumented `USBHSREG` trap is already handled**: on H7RS the OTG_HS
  PHY needs `HAL_PWREx_EnableUSBHSregulator()`, which CubeMX does not emit.
  Both `Appli/Core/Src/stm32h7rsxx_hal_msp.c` and the Boot equivalent already
  call it, together with `HAL_PWREx_EnableUSBVoltageDetector()`.
  (`.ioc` line 334: `PWR.USB_HS_REGEN=Enable`.)
* **HSI48 is irrelevant here** and correctly left alone: `OTGFSSEL`
  (`CCIPR1[15:14]`) feeds the separate OTG_FS 48 MHz domain. This design uses
  OTG_HS with `USB_OTG_HS_EMBEDDED_PHY`, clocked from USBPHYC. ST's reference
  does not enable HSI48 either.
* `SYSCFG_OTG_HS_PHY_CLK_SELECT_4`, named in a code comment in `usbd_conf.c`,
  **does not exist on this part** — H7RS has no `SYSCFG->OTG_HS_PHY_CTRL`; the
  system-config block is `SBS` and has no USB PHY clock field. The comment is
  inherited from another STM32 family. The conclusion it draws (set
  `USBREFCKSEL` explicitly) is nevertheless correct and harmless; the *file*
  `USBREFCKSEL` writes is the right one. The comment has been left as-is to
  keep this change set minimal — see §5.

### 3.4 Bench confirmation to run on hardware

Static analysis can prove the *configuration* is right but not that the PHY PLL
has locked. Add this once during bring-up, or read it in a debugger:

```c
/* expected: phyc = 24000000, refsel = 0x00000A00 */
uint32_t phyc   = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USBPHYC);
uint32_t refsel = READ_BIT(RCC->CCIPR1, RCC_CCIPR1_USBREFCKSEL);
```

Logging this at boot is deliberately **not** part of this change set — it
changes runtime behaviour for a purely diagnostic purpose and belongs in a
bring-up build, not in the shipped image.

---

## 4. File-by-file change list

### Commit `3e2640b` — *Fix NVIC priority ordering: UCPD > CDC > USART1 > everything else*

| File | Change |
| --- | --- |
| `Appli/Core/Inc/irq_priority.h` | **NEW.** Single source of truth for the priority map; documents the grouping, the `EncodePriority == n` identity and the required ordering. |
| `Appli/Core/Src/main.c` | Include `irq_priority.h`; call `HAL_NVIC_SetPriorityGrouping(IRQ_PRIORITY_GROUP)` explicitly so the levels cannot drift. |
| `Appli/Core/Src/ucpd.c` | `UCPD1_IRQn`: 5 → `IRQ_PRIO_UCPD` (0). |
| `Appli/USBPD/Target/usbpd_devices_conf.h` | `UCPD_INSTANCE0_ENABLEIRQ` (runtime re-application by the CAD layer) now encodes `IRQ_PRIO_UCPD`. |
| `Appli/Core/Src/gpdma.c` | Ch0/Ch1 → `IRQ_PRIO_UCPD_DMA` (0); Ch2 → `IRQ_PRIO_TRACE_DMA` (3). |
| `Appli/Core/Inc/tracer_emb_conf.h` | **Defines `TRACER_EMB_TX_DMA_PRIORITY`** = `IRQ_PRIO_TRACE_DMA`; `TRACER_EMB_TX_IRQ_PRIORITY` = `IRQ_PRIO_USART1`. This is the fix for the priority-0 trace DMA. |
| `Appli/USB_DEVICE/Target/usbd_conf.c` | `OTG_HS_IRQn`: 4 → `IRQ_PRIO_CDC_USB` (1). |
| `Appli/Core/Src/usart.c` | `USART2_IRQn`: CubeMX's generated `0` overridden with `IRQ_PRIO_CONSOLE` (4) inside a USER CODE block, so it survives regeneration. |

8 files, +147 / −32.

### Commit `70135aa` — *Do not brick the board when HAL_PCD_Init fails*

| File | Change |
| --- | --- |
| `Appli/USB_DEVICE/Target/usbd_conf.c` | `USBD_LL_Init` returned `Error_Handler()` → `Appli_Fail(7)` (permanent blink loop, PD bench dead) if `HAL_PCD_Init()` failed. Now returns `USBD_FAIL`, so `MX_USB_DEVICE_Init()`'s existing graceful-degradation path logs the failure and the board keeps running with PD + UART. |

1 file, +5 / −1.

### Uncommitted (added with this report)

| File | Change |
| --- | --- |
| `tools/verify_irq_and_vectors.py` | **NEW.** Post-link verification tool (§6.2). Not part of the firmware image. |

**Not touched:** both linker scripts (`Appli/STM32H7R3Z8JX_ROMxspi1.ld`,
`Boot/STM32H7R3Z8JX_FLASH.ld`) are used **verbatim** — no defect was
demonstrated that required a change. The Bootloader was **not edited**.

---

## 5. Bugs found and deliberately left alone (with reasoning)

Everything below is either middleware, hardware-critical, or broad enough that a
wrong guess could damage hardware. Logged, not changed.

| # | File:line | Issue | Risk of changing | Suggested fix |
| --- | ---: | --- | --- | --- |
| 1 | `Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_hw_if_it.c:255` | `USBPD_TIM_Start(...,150); while (USBPD_TIM_IsExpired(...)==0);` — a **150 µs busy-wait inside the UCPD ISR**. At UCPD priority 0 that would stall the entire system for 150 µs per call, including the USB OTG_HS ISR — a second, independent way to blow the enumeration window. | Middleware file; changing it forks the vendor stack and breaks future upgrades. | Verify `_FRS` is never defined in a shipping build (it is not, in either `Appli/.cproject` configuration — this is dead code today). If FRS is ever enabled, this must be reworked to a state machine that exits the ISR. **Add a build-time guard/assert that `_FRS` is undefined.** |
| 2 | `Appli/USB_DEVICE/Target/usbd_conf.c:75` | Comment cites `SYSCFG_OTG_HS_PHY_CLK_SELECT_4`, a register field that **does not exist on STM32H7RS** (no `SYSCFG->OTG_HS_PHY_CTRL`; the block is `SBS` and has no USB PHY clock field). The conclusion is right, the justification is from another family, and a future reader may "correct" the code back to a broken state. | Comment-only; out of scope for a minimal change set and touching it risks someone re-litigating a working fix. | Rewrite the comment to reference `RCC->CCIPR1.USBREFCKSEL` / `USBPHYCSEL` and the 24 MHz × 2 = 48 MHz math from §3.2. |
| 3 | `Appli/USB_DEVICE/Target/usbd_conf.c:92` | `HAL_PCD_MspInit` calls `Error_Handler()` if `HAL_RCCEx_PeriphCLKConfig()` fails — the same bricking pattern fixed in `70135aa`. | **Deliberately left.** `HAL_PCD_MspInit` returns `void`, so degrading gracefully here means letting `HAL_PCD_Init()` proceed and touch an unclocked `USB_OTG_HS` — a plausible bus-fault, i.e. trading a visible LED-7 for a hard crash. The configuration is static and valid and HSE is provably running, so this branch is unreachable in practice. | If parity is wanted: set a module-static `usb_clock_ok` flag and check it in `USBD_LL_Init` (which *can* return a status) before calling `HAL_PCD_Init()`, instead of continuing into unclocked register access. |
| 4 | `USB_UCPD.ioc` | PLL3 is configured in the `.ioc` (`DIVM3=2`, `DIVN3=34`, `DIVQ3=17` → PLL3Q = 24 MHz) but `Boot` sets `PLL3 = RCC_PLL_NONE`, so PLL3Q does not exist. Only latent: `USBPHYCSEL` uses HSE, not PLL3Q. | None today. Noted so nobody assumes PLL3Q is available as a USB source. | Either leave as-is (matches ST's NUCLEO-H7S3L8 example) or enable PLL3 and switch `USBPHYCSEL` to `RCC_USBPHYCCLKSOURCE_PLL3Q` if an HSE frequency outside the legal set is ever needed. |
| 5 | `README.md` | Two figures are stale: "44/44 syntax checks" (actual: **55**) and "Appli FLASH 148 240 B" (actual: **188 428 B**). Also the priority table describes the *old* scheme (USB 4, UCPD 5, trace 6). | Documentation only; not corrected here to keep the diff focused on firmware. | Regenerate the build/size table and update the priority table to the §2.2 values. |

### Verified-clean areas (no action needed)

* **Buffer safety** — no `sprintf` / `strcpy` / `strcat` / `gets` anywhere in
  `Appli`; the logger uses `vsnprintf` into a 256-byte buffer with a clamped
  length. No unchecked `malloc`; the USB device library uses its own static
  allocator (`USBD_static_malloc`).
* **`app_log.c` ring buffer** — single producer, two cursors, copy-then-commit
  into the 32-byte-aligned `noncacheable_buffer` section. Correct.
* **`app_cli.c`** — `APP_CLI_OnRx` already wraps each received byte in a
  PRIMASK critical section, which is required because two producers
  (OTG_HS and USART2) feed it. Correct.
* **`ext_uart.c`** — RX FIFO re-arms itself from `EXT_UART_Poll` when
  `huart2.RxState == HAL_UART_STATE_READY`. Correct self-healing.
* **TIM2 / `usbpd_timersserver.c`** — polling only; no TIM NVIC IRQ is ever
  enabled and there is no `HAL_TIM_Base_Start_IT()` in the tree, so TIM2 is not
  a priority-latency concern.
* **Boot `JumpToApplication`** — correctly disables I/D-cache, sets VTOR to
  `APP_XIP_BASE`, loads MSP and restores PRIMASK before branching.
* **Fault handlers** (`stm32h7rsxx_it.c:98-157`) — the `while(1)` traps are in
  `NMI`/`HardFault`/`MemManage`/`BusFault`/`UsageFault`, which is correct
  behaviour; they are *not* in normal code paths.

---

## 6. Build and verification

### 6.1 Clean build with a real ARM toolchain

`tools/check_arm_build.py` (zig `cc`/`lld` cross-compile + link, sources and
linker script taken from the project's own `.project` / `.cproject`, asserting
`-T` consistency across Debug and Release):

```
== Boot: 35 C sources, -T STM32H7R3Z8JX_FLASH.ld
  compiled: 35 ok, 0 failed (1 with warnings)
  link: OK
    RAM:          312 B  / 455 KB      0.07%
    DTCM:           2 KB /  64 KB      3.12%
    FLASH:      24776 B  /  64 KB     37.81%

== Appli: 86 C sources, -T STM32H7R3Z8JX_ROMxspi1.ld
  compiled: 86 ok, 0 failed (0 with warnings)
  link: OK
    RAM:                 34008 B / 440 KB   7.55%
    RAM_NONCACHEABLEBUFFER: 5408 B /   8 KB  66.02%
    DTCM:                   4 KB  /  64 KB   6.25%
    FLASH:             188428 B   /   8 MB   2.25%

ARM build check: PASS
```

* **Zero new warnings.** The single warning
  (`Boot/Core/Src/w25qxx_xspi.c:224: unused function 'W25QXX_Wait_Busy'`) is
  pre-existing and present in the baseline before any of my changes.
* **Fits with margin.** Boot occupies 37.81 % of the 64 KB internal flash
  (62 % free). Appli occupies 2.25 % of the 8 MB XIP NOR window. Appli RAM
  7.55 %, non-cacheable DMA buffer 66 % of its 8 KB region, DTCM stack 6.25 %.
  Appli FLASH grew 188 268 → **188 428 B (+160 B)** from the priority change.
* `tools/check_syntax.sh`: **55 passed, 0 failed.**

### 6.2 Compiled image verification (`tools/verify_irq_and_vectors.py`)

New tool. It parses the linked ELF's `.isr_vector`, resolves each slot to a
handler symbol, and pre-processes the real NVIC call sites with the project's
own ARM flags so every `IRQ_PRIO_*` / `TRACER_EMB_*` macro is resolved to the
literal value the compiler emits.

Vector table (abridged; all 7 priority-managed IRQs shown):

```
Vector table  .isr_vector  @ 0x90000000  (172 entries, 688 bytes)
  slot  address      value      handler
  0     0x90000000   0x20010000   (initial MSP, DTCM top)
  55    0x900000DC   0x90015389  GPDMA1_Channel0    (thumb)
  56    0x900000E0   0x90015391  GPDMA1_Channel1    (thumb)
  57    0x900000E4   0x90015319  GPDMA1_Channel2    (thumb)
  98    0x90000188   0x90015329  USART1             (thumb)
  99    0x9000018C   0x90015399  USART2             (thumb)
  107   0x900001AC   0x900153B1  OTG_HS             (thumb)
  144   0x90000240   0x900153C9  UCPD1              (thumb)
```

Every managed IRQ is populated, every handler has the Thumb bit set, and the
initial stack pointer is inside DTCM. No unhandled slots among the seven.

Compiled priorities (macros resolved by the pre-processor):

```
main.c           PRIGROUP               = 3U                (NVIC_PRIORITYGROUP_4)
gpdma.c          GPDMA1_Channel0_IRQn   = 0U, 0
gpdma.c          GPDMA1_Channel1_IRQn   = 0U, 0
gpdma.c          GPDMA1_Channel2_IRQn   = 3U, 0
tracer_emb_hw.c  GPDMA1_Channel2_IRQn   = 3U                <-- was 0 before
usbd_conf.c      OTG_HS_IRQn            = 1U, 0
ucpd.c           UCPD1_IRQn             = ENC(0U)           <-- == 0
tracer_emb_hw.c  USART1_IRQn            = 2U
usart.c          USART2_IRQn            = 4U, 0             (the CubeMX "0, 0"
                                                             above it is
                                                             immediately
                                                             overridden)
```

`ENC(n) == NVIC_EncodePriority(NVIC_GetPriorityGrouping(), n, 0) == n` under
group 4. **The compiled image matches the intended table in §2.2 exactly.**

### 6.3 Static analysis

`cppcheck` and `clang-tidy` are **not installed** in this environment; neither
is available offline. Static analysis was therefore limited to:

* the ARM cross-compiler's own warnings at the project's flags (**0 new**),
* the project's `tools/check_syntax.sh` (**55/55**),
* the new linker/ELF verification tool above,
* a manual line-by-line read of the whole Appli and Boot source tree.

This is a genuine limitation and is stated honestly in §7.

---

## 7. Confidence statement

**Statically and build-verified (high confidence):**

* The priority-0 USBPD trace DMA defect was real, is now fixed, and the compiled
  image provably carries the intended priority values (ELF + pre-processed
  source evidence).
* The required ordering `UCPD > CDC > USART1 > everything else` is now satisfied
  and is enforced by a single shared header.
* The USB/PHY clock configuration is internally consistent and matches both the
  CubeMX clock solver and ST's own reference project. `USBREFCKSEL = 0b1010`
  (24 MHz) with `USBPHYCSEL = 0b00` (HSE = 24 MHz) yields 48 MHz exactly, 0 ppm
  nominal. **No clock change was needed and none was made.**
* The images build clean, fit their regions with large margins, and the vector
  table is correct.

**Medium confidence (mechanistically sound, not proven on this board):**

* That the priority-0 trace DMA is *the* cause of this particular Windows 11
  enumeration failure. The mechanism is sound and the symptom matches, but
  Windows enumeration involves host-side timing I cannot observe from here. This
  is the change most likely to fix the reported problem.

**Not verified at all — requires the physical board:**

* Whether the USBPHYC PLL actually locks on this specific PCB. §3.2 proves the
  *configuration* is right; only a register read-back (§3.4) or a scope on the
  48 MHz domain can prove the *hardware* follows it.
* Whether VBUS/CC wiring, the external 24 MHz crystal's actual accuracy, and
  signal integrity on DM/DP are within USB tolerance. A 24 MHz crystal with
  poor ppm accuracy or a bad USB connector/ESD clamp will produce exactly the
  same "device descriptor request failed" symptom and **cannot** be detected by
  any amount of source analysis.
* Actual enumeration on Windows 11.

**I do not claim the physical symptom is guaranteed gone.** If it persists after
this change, the next things to check, in order, are: (a) the §3.4 clock
read-back, (b) the crystal frequency/accuracy on DM/DP behaviour with a USB
protocol analyser or USBView, (c) item #1 in §5 if `_FRS` is ever enabled.

---

## 8. Push confirmation

| | |
| --- | --- |
| **Repository** | `https://github.com/blackdemon-1949/USB_UCPD.git` |
| **Branch** | `arena/01a07fb0-usb-ucpd` (from `main` @ `210b37a`) |

| Commit | Subject | Files |
| --- | --- | --- |
| `0194a4f` | Add USB_UCPD_V2(DTS) firmware tree extracted from the bundled archive | 339 |
| `3e2640b` | Fix NVIC priority ordering: UCPD > CDC > USART1 > everything else | 8 |
| `70135aa` | Do not brick the board when HAL_PCD_Init fails | 1 |
| *(with this report)* | `tools/verify_irq_and_vectors.py`, `FIX_REPORT.md` | 2 |

The commits are **logically separated as required** — the extracted sources, the
priority fix, and the USB-init robustness fix are three distinct commits, not
squashed.

`0194a4f` unpacks the project from `USB_UCPD_V2(DTS).zip` because the repository
previously shipped only the archive, which made every subsequent change
unreviewable. The extraction is **byte-identical** to the archive contents
(verified by diff against a fresh unzip); the project's own `.gitignore` is
honoured, so build outputs are excluded. If you would rather keep the tree
packed, say so and I will drop that commit and keep only the two fix commits —
but then the diff cannot be reviewed file-by-file.
