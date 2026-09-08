# PD Bench — STM32H7R3Z8J6 UCPD sink + USB CDC

WeActStudio STM32H7R3Z8 Core Board firmware.

The board enumerates as a USB CDC serial device on the **USB-HS Type-C**
connector. A PD source is wired **only** to the MCU header: **one CC line (CC1 or CC2), GND**.
Source capabilities print on the serial port; you pick a PDO / PPS voltage
and the UCPD sink negotiates it.

## Hardware (physical board, not the schematic silkscreen)

| Item | Reality |
| --- | --- |
| MCU | STM32H7R3Z8J6, 600 MHz, 64 KB internal FLASH, ~620 KB RAM |
| External NOR | **Puya PY25Q64HA-SUH-IR** 8 MB (schematic says Winbond W25Q64) |
| XiP | Boot in internal FLASH maps NOR at `0x90000000` and jumps |
| USB-HS Type-C (J1, PM5/PM6) | CDC serial to the PC — **not** the PD port |
| UCPD1 CC1 / CC2 | **PM0 / PM1** on the header |
| I2C2 | **PB10 (SCL) / PB11 (SDA)**, 400 kHz fast mode, wired as `INA226_SCL` / `INA226_SDA` |
| INA226 | module on I2C2, address `0x40` (0x41–0x43 probed as fallback), **5 mΩ shunt**, no external pull-ups (MCU internal pull-ups) |
| USART2 | **PD5 (TX) / PD6 (RX)**, 115200 8N1 — **second console**, mirrors the CDC port |
| DTS | on-die temperature sensor, LSE reference clock — SoC temperature readings |
| LED | PB2 (`Built-IN_LED`) |
| KEY | PC13 (`User_Button`), active low, external 10 kΩ pull-up |
| PD trace | USART1 **PA9 (TX) / PA10 (RX)** @ 921600, hand-configured (not in the `.ioc`) |

PY25Q64HA SFDP tables are incomplete. Do **not** use ST ExtMem SFDP bring-up.
The Boot project uses the WeAct W25Qxx XSPI driver (reset, QE, `0xEB` 1-4-4
memory-mapped). The WeAct `.stldr` in `tools/` programs the same chip.

## Projects (CubeIDE)

Open the folder as an existing STM32CubeIDE workspace. Two projects:

| Project | Link | Runs from |
| --- | --- | --- |
| **USB_UCPD_Boot** | `Boot/STM32H7R3Z8JX_FLASH.ld` | Internal FLASH `0x08000000` / 64 KB |
| **USB_UCPD_Appli** | `Appli/STM32H7R3Z8JX_ROMxspi1.ld` | XiP `0x90000000` / 8 MB |

Build **Debug** for both. Flash Boot first, then Appli (see [FLASHING.md](FLASHING.md)).

## Peripheral extension footprints (I2C2 / USART2 / DTS)

Every peripheral the application does not need for the PD contract itself is
exposed through a small *footprint* module. Each one follows the same shape,
so a feature project only ever adds its own file — no edits to `main.c` and
no edits to the footprint module:

| Peripheral | Pins | Config | CubeMX file | Footprint |
| --- | --- | --- | --- | --- |
| I2C2 | PB10 SCL / PB11 SDA | 400 kHz fast mode, 7-bit | `Appli/Core/Src/i2c.c` | `Appli/Core/Src/ext_i2c.c` |
| USART2 | PD5 TX / PD6 RX | 115200 8N1, TX+RX, no flow control | `Appli/Core/Src/usart.c` | `Appli/Core/Src/ext_uart.c` |
| DTS | on-die sensor | LSE ref. clock, 15-cycle sampling | `Appli/Core/Src/dts.c` | `Appli/Core/Src/ext_dts.c` |

Two features are built on top of those footprints by overriding the weak hooks,
exactly the way the INA226 monitor does:

| Feature | Files | Overrides |
| --- | --- | --- |
| Second console on USART2 | `app_log.c` (output), `app_cli.c` (input) | `EXT_UART_FeatureInit`, `EXT_UART_RxByteReceived` |
| SoC temperature monitor | `Appli/Core/Src/dtsmon.c` | `EXT_DTS_FeatureInit`, `EXT_DTS_FeaturePoll` |

All three are already wired into `main.c`:

- `EXT_I2C_Init()` / `EXT_UART_Init()` / `EXT_DTS_Init()` run once after the
  CubeMX peripheral init;
- `EXT_I2C_Poll()` / `EXT_UART_Poll()` / `EXT_DTS_Poll()` run every
  super-loop pass.

To add a feature, implement the weak hooks in your own source file:

```c
void EXT_I2C_FeatureInit(void);   void EXT_I2C_FeaturePoll(void);
void EXT_UART_FeatureInit(void);  void EXT_UART_FeaturePoll(void);
void EXT_UART_RxByteReceived(uint8_t b);   /* from the USART2 ISR */
void EXT_DTS_FeatureInit(void);   void EXT_DTS_FeaturePoll(void);
```

Generic helpers are provided: `EXT_I2C_ReadReg` / `EXT_I2C_WriteReg` (8-bit
register access, plus `…RegTO` variants with a caller-supplied timeout),
`EXT_UART_Write` / `EXT_UART_Printf` / `EXT_UART_Read` / `EXT_UART_ReceiveByteIT`,
and `EXT_DTS_ReadTempC`. Boot-time status of all three prints on the serial
port (`ext-i2c:` / `ext-uart:` / `ext-dts:` lines). Every blocking helper uses
a **finite timeout** so a missing or wedged peripheral can never stall the
super loop (and with it USB CDC, the CLI and the PD stack).

The INA226 monitor below is itself just such a feature: it overrides
`EXT_I2C_FeatureInit` / `EXT_I2C_FeaturePoll` and does not touch `main.c`.

`uart` and `dts` on the CLI exercise both new footprints without any code
changes — see the command list below.

## Two consoles at once (USB-HS CDC + USART2)

USART2 is wired straight into the CDC console: the MCU prints to **both**
ports and listens on **both**, at the same time. Open the USB-HS CDC port and
a 115200 8N1 terminal on PD5/PD6 together and either one can drive the MCU —
a command typed on one is echoed and answered on both.

```
  PC ─ USB-HS Type-C ─> USB CDC ─┐           ┌─> USB CDC ─> PC
                                 ├─ app_cli ─┤
  PC ─ USB-UART ─> PD6 (RX) ─────┘  (one     └─> USART2 PD5 ─> PC
                                      line
                                    editor)
```

How it works:

- **Output** — `app_log.c` keeps one 2 KiB ring with a *separate cursor per
  sink*. Each super-loop pass drains up to 256 bytes to CDC and up to 64 bytes
  to USART2 (≈5.6 ms on the wire — the longest the loop is held). A muted or
  absent sink never holds the other back: after three failed UART transmits the
  chunk is dropped and counted.
- **Input** — `CDC_Receive_HS` and the USART2 RX interrupt both call
  `APP_CLI_OnRx()`, which feeds the single line editor. The enqueue sits in a
  short critical section because the two producers run at different interrupt
  priorities (OTG_HS 4, USART2 7).
- USART2 reception is interrupt driven (`EXT_UART_ReceiveByteIT`), and
  `HAL_UART_ErrorCallback` re-arms it after an overrun or framing error.

```
console                which console is live, counters, drop statistics
console usb on|off     mirror output to the USB-HS CDC port
console uart on|off    mirror output to USART2
console both on|off    mirror to both (the default)
```

Muting *both* sinks is refused — that would leave the board unreachable until a
reset. `uart <text>`, `uart rx [ms]` and `uart selftest` remain as raw-port
tests.

A sink that cannot drain must not pin the queue: `APP_LOG_Flush()` advances the
cursor of any sink that is muted **or** has no consumer (CDC with no host
attached, or not yet configured). Without that, one inactive sink would fill the
ring and silence *both* consoles, because the producer stops queueing once the
slowest reader is a full ring behind.

**Confirmed on hardware:** on the WeAct STM32H7R3Z8 board both consoles stream
output and accept commands — `help` and the rest work from USART2 and from the
USB-HS CDC port, either one or both at the same time. Three changes landed
together (the queue fix, the PD6 pull-up, the RX re-arm), so it is not known
which one was decisive; all three are correct in their own right and none of
them touch the CDC or USBPD paths.

**Troubleshooting USART2.** At boot the peripheral emits
`[USART2 PD5 TX OK - 115200 8N1]` *directly*, bypassing the log queue. If that
line arrives but the console does not, the fault is in the queue path; if
nothing arrives, it is the pin, the wiring or the adapter. `uart selftest`
dumps everything needed to tell those apart: the `APB1ENR1.USART2EN` clock bit,
the kernel clock source, PD5/PD6 `MODER`/`AFR` (want `2` and `7`), the NVIC
enable bit and priority, `CR1`/`ISR`/`BRR`, the HAL `gState`/`RxState`/
`ErrorCode`, the RX counters, both console mirrors, and one live transmit.

PD6 (RX) is re-initialised with an internal pull-up in `usart.c` — CubeMX
generates it as `GPIO_NOPULL`, and a floating UART input reports a stream of
framing errors. `EXT_UART_Poll()` also re-arms the byte receiver whenever the
HAL reports it ready with nothing outstanding; without that, a `HAL_BUSY` from
the error path left the port deaf until reset.

## SoC temperature (DTS, `dtsmon.c`)

`dtsmon.c` reads the on-die temperature sensor and reports it on the console in
**degrees C or degrees F**, using the same periodic model as the INA226.

```
dts                    one-shot SoC temperature
dts auto on|off        periodic reading (default on, 1 s)
dts period <ms>        periodic interval, 250..60000 ms
dts unit c|f           report in degrees C or degrees F
dts read               same as plain `dts`
dts status             sensor / clock state plus the reference-clock hint
```

```
[dts]    soc 41 C   (= 105 F)
```

`status` carries a `dts` line too. The HAL resolves whole degrees only, so both
numbers are integers; Fahrenheit is `C * 9 / 5 + 32`.

As with the INA226 section, the periodic report **only starts after a first
successful reading**, so a sensor that is not converting stays silent instead
of flooding the console once a second.

### Reference clock

The `.ioc` selects `DTS_REFCLKSEL_LSE` and assigns PC14/PC15 as
`OSC32_IN`/`OSC32_OUT`, so the sensor is clocked from the 32.768 kHz LSE — but
Boot's generated `SystemClock_Config()` only starts the HSE, so nothing in the
CubeMX-generated code switches the LSE on and `HAL_DTS_Start()` used to time out
on `TS1_RDY` every time.

`ext_dts.c` now starts the LSE itself: `EXT_DTS_TryStart()` enables backup-domain
access, sets `RCC_LSE_ON`, and only starts the sensor once `RCC_FLAG_LSERDY` is
asserted. That sits inside the existing once-per-second retry, so nothing blocks
and a first reading appears within ~2 s of boot. A board with no 32.768 kHz
crystal just keeps reporting `no SoC temperature reading` instead of hanging.

**Confirmed on hardware:** on the WeAct STM32H7R3Z8 board the LSE comes up and
`dts` returns a real die temperature, so a 32.768 kHz crystal *is* fitted on
PC14/PC15. The no-crystal fallback above is there for other boards and for a
failed crystal, not because the current one is missing.

**Why LSE and not PCLK:** the DTS counter clock must stay below 1 MHz during
calibration, the DTS is on **APB4** (`RCC->APB4ENR.DTSEN`) which runs at 150 MHz
here, and `HSREF_CLK_DIV` is only 7 bits (max ratio 128) — the counter would run
at 1.17 MHz, over the limit. Separately, `HAL_DTS_GetTemperature()` computes the
PCLK case from `HAL_RCC_GetPCLK1Freq()` (APB1) rather than APB4, so a PCLK
configuration would also require APB1 == APB4. LSE is the correct reference for
this clock tree.

If you regenerate from CubeMX with **RCC → LSE** enabled, the generated
`SystemClock_Config()` will start the LSE and `dts_ensure_lse()` becomes a
no-op. Both paths work.

## INA226 output monitor (`ina226.c`)

An INA226 module on I2C2 (PB10/PB11, 5 mΩ shunt) measures the output
voltage and current. It plugs into the extension footprint above by
overriding `EXT_I2C_FeatureInit` / `EXT_I2C_FeaturePoll` — no changes in
`main.c`.

Behaviour:

* On boot addresses 0x40–0x43 are probed; a chip is accepted only if the
  Manufacturer ID (`0x5449` = "TI") **and** Die ID (`0x2260`) match.
* **If no INA226 is connected the firmware keeps running normally** — the
  console repeats `no ina226 connected` and the probe is retried every 5 s.
  Every I2C access uses a 10 ms timeout; a wedged bus (SDA stuck low) is
  recovered automatically by re-initialising I2C2. The PD and CDC stacks
  are never blocked by the monitor.
* Sampled every 250 ms (AVG 16, 1.1 ms conversion times — CFG `0x2907`),
  reported on the console every second by default. The configuration is
  read back after writing: only the MODE bits (continuous shunt+bus) are
  a hard requirement — cheap clone INA226s return non-zero values in the
  reserved bits [5:3] and are accepted with a logged note; a chip that
  refuses continuous mode is rejected with a diagnostic instead of being
  silently connected with frozen readings.

  ```
  [ina226] out 5.262 V   370.0 mA   1946 mW   (0x40)
  ```

* Scaling for the 5 mΩ shunt: bus LSB 1.25 mV, shunt LSB 2.5 µV →
  **0.5 mA per shunt LSB exactly** (±16.3 A range before the shunt
  amplifier clips). The chip calibration register is programmed with
  2560 (`0x0A00`) = `0.00512 / (400 µA × 5 mΩ)` so the INA226's own
  current/power registers read 400 µA / 10 mW LSBs as well.
* `ina vbus real` makes the PD stack use the measured rail instead of the
  synthetic CC-only-tester VBUS (falls back to synthetic when the data is
  stale). Default stays `synth`, which needs no wiring assumptions.

CLI commands:

```
ina                    one-shot V / I / P reading (or "no ina226 connected")
ina auto on|off        periodic reporting on/off (default on, 1 s)
ina period <ms>        periodic interval, 250..60000 ms
ina addr <hex>         use another 7-bit address (e.g. ina addr 41)
ina scan               scan the whole I2C2 bus (0x08-0x77)
ina vbus real|synth    what the PD stack reports as VBUS
```

`status` and the boot banner include the INA226 line.

## Serial CLI (115200 8N1, DTR opens a session)

```
help                 this list
status               PD + USB state (+ INA226 + DTS lines)
info                 board / memory / clocks
caps                 list last source PDOs
getcaps              send Get_Source_Cap
req <n>              request source PDO n (1-based)
volt <mv> [ma]       closest fixed PDO or covering PPS
pps <mv> [ma]        request a PPS APDO
hardreset            PD hard reset
softreset            PD soft reset
ina [args]           INA226 output V/I monitor (see above)
pd                   UCPD registers + PHY counters
console [args]       second-console control (see above)
uart <text>          send <text> straight out of USART2
uart rx [ms]         dump what USART2 received
dts [args]           SoC temperature in C or F (see above)
led on|off|hb        LED override
```

`help` prints the complete list (there are more commands for reading source
details: `getstatus`, `getpps`, `srcext`, `manuinfo`, `battery`,
`countrycodes`, `identify`, `svids`, `modes`, plus `auto` / `remember` /
`sweep` automations).

First Request after attach auto-picks **5 V** (USB-PD tRequest). Then `req`,
`volt`, or `pps` to change the contract.

## CC-only tester

There is no VBUS ADC on this wiring. The firmware synthesizes VBUS so the
policy engine can complete an explicit contract:

- CC attach → report 5 V
- After a Request is built → report the requested voltage
- Detach / hard reset → 0 V or 5 V as appropriate

The source still drives real VBUS on its own cable. The MCU only talks CC.

## LED fail codes (PB2)

Boot blinks N times, pauses, repeats:

| N | Meaning |
| --- | --- |
| 1 | NOR init failed |
| 2 | Memory-mapped mode failed |
| 3 | No valid Appli vector at `0x90000000` |
| 4 | Jump returned (should never) |
| 5 | Boot `Error_Handler` |
| 7 | Appli `Error_Handler` |
| 8 | USBPD stack init failed (`usbpd.c` / DPM error handler) |

## USB + PD stability fixes (this revision)

* **USB PHY reference clock** — CubeMX on STM32H7RS never emits the
  `.ioc` setting `USB_OTG_HS.RefClockSelection` into the code, leaving
  `RCC_CCIPR1.USBREFCKSEL` at its reset value while the PHY runs from the
  24 MHz HSE. Symptom: CDC enumerates only after several replugs / shows
  as corrupt in the Windows device manager. `HAL_PCD_MspInit` now calls
  `LL_RCC_SetUSBREFClockSource(LL_RCC_USBREF_CLKSOURCE_24M)` explicitly
  (see the ST community thread "STM32H7RS USB_HS failure (Device
  Descriptor Request Failed)").
* **Bus reset with a garbage enumeration speed** no longer calls
  `Error_Handler()` from the USB interrupt (that bricked the board: solid
  PB2, dead PD stack). It falls back to full-speed descriptors instead.
* **Console TX stuck busy** — a bus reset / suspend during an IN transfer
  left the logger in `s_tx_busy = 1` forever, so the COM port looked dead
  until the disable/enable dance in the device manager. `CDC_Init_HS`,
  `CDC_DeInit_HS` and suspend now re-arm the TX path.
* **USB init failure is no longer fatal** — if `MX_USB_DEVICE_Init` fails,
  the PD bench keeps running and the failure is logged instead of a
  permanent blink loop.
* **Interrupt priorities rebalanced** — USB OTG_HS = 4, UCPD1 + its DMA
  channels = 5, PD trace = 6 (was: UCPD 0 = above everything, USB 6).
  PD negotiation bursts can no longer starve USB enumeration (and vice
  versa) when the cable and a PD source are connected at the same time.
* **Silent `while(1)` hangs removed** from the USBPD bring-up
  (`MX_USBPD_Init`, `USBPD_DPM_ErrorHandler`); they now blink LED code 8.


Healthy Appli: slow heartbeat (USB up, no PD), fast blink while negotiating,
solid on explicit contract.

## CubeMX notes (already patched — do not regenerate blindly)

`USB_UCPD.ioc` is the trimmed configuration: **SPI2 and the LCD/sd-detect
GPIOs are gone**, and **DTS + USART2 are in**. Everything the PD bench
actually uses (XSPI1/XiP, UCPD1, USBPD, USB_OTG_HS CDC, I2C2, GPDMA1, PB2
LED, PC13 key) is still enabled there.

Hand-edits that a regeneration will revert — re-apply them:

| File | Hand-edit | Why |
| --- | --- | --- |
| `Appli/Core/Src/gpio.c` | PC13 `GPIO_NOPULL` | the `.ioc` says `PULLDOWN`, which fights the board's external 10 kΩ pull-up |
| `Appli/Core/Src/ucpd.c` | `UCPD1_IRQn` priority **5** | ST default 0 starves USB enumeration |
| `Appli/Core/Src/gpdma.c` | channels 0/1 priority **5**, channel 2 priority **6** | UCPD DMA below USB; channel 2 is the PD trace and is *not* modelled in the `.ioc` |
| `Appli/USB_DEVICE/Target/usbd_conf.c` | `OTG_HS_IRQn` priority **4**, `LL_RCC_SetUSBREFClockSource(24M)`, full-speed fallback instead of `Error_Handler()` | CubeMX never emits `USB_OTG_HS.RefClockSelection` on H7RS |
| `Appli/USBPD/Target/usbpd_devices_conf.h` | `UCPD_INSTANCE0_ENABLEIRQ` sets priority **5** | the CAD layer re-applies this at runtime, so it overrides `ucpd.c` |
| `Appli/.cproject` | preprocessor symbol **`_TRACE`** (Debug + Release) | enables the USBPD TRACER_EMB trace; the `.ioc` has no trace option |
| `Appli/.project` | link to `Drivers/…/Src/stm32h7rsxx_ll_usart.c` | the tracer drives USART1 through the LL driver |
| `Appli/Core/Src/main.c` | 5-region `MPU_Config` (the 8 KiB non-cacheable window at `0x2406E000` is region 4) | USB/CDC DMA and the CM7 D-cache must agree, or enumeration fails |
| linker scripts | `__RAM_SIZE 0x6E000` + 8 KiB `RAM_NONCACHEABLEBUFFER`, 16 KiB heap in AXI SRAM, MSP stack in DTCM | the UCPD stack `malloc()`s its DMA buffers; GPDMA1 cannot reach DTCM (AN6062) |

These already live in USER CODE sections and therefore survive regeneration:

- the application init/task calls and the `EXT_*` footprint hooks in
  `main.c`, `usbpd_dpm_user.c`, `usbpd_pwr_user.c`, `usbpd_vdm_user.c`,
  `usbpd_pwr_if.c`, `usb_device.c`, `usbd_conf.c`, `usbd_cdc_if.c`,
  `extmem_manager.c`;
- the tracer ISRs (`USART1_IRQHandler`, `GPDMA1_Channel2_IRQHandler`) in
  `stm32h7rsxx_it.c` and their prototypes in `stm32h7rsxx_it.h`;
- the board aliases and `Appli_Fatal()` in `main.h`.

Other facts worth keeping:

- Appli `main` calls `MX_USB_DEVICE_Init` (which ends in `USBD_Start`) from a
  USER CODE section; a USB init failure is logged, not fatal.
- Sink PDOs: 5/9/12/15/20 V + PPS 3.3–21 V / 3 A, Higher Capability set
  (`usbpd_pdo_defs.h`).
- Debug linker scripts are the real `FLASH` / `ROMxspi1` files, not the MMT
  templates.
- Regenerating restores `MX_DTS_Init` / `MX_USART2_UART_Init` and re-links the
  HAL DTS/UART sources into `Appli/.project` — that part is already correct.

## What changed in this revision (V2 merge)

This tree is the trimmed `USB_UCPD_V2_[UART, i2C,GPIO]` CubeMX project with the
working PD-bench firmware merged into it:

- **Dropped** (unused bloat): SPI2 (PD3/PC1/PB14) with `spi.c` / `ext_spi.c`,
  the LCD GPIOs PA0/PA1/PA4, `sd_dect` on PA8, and the HAL SPI driver sources.
- **Added by the new `.ioc`**: DTS and USART2 (PD5/PD6), both exposed as
  feature footprints (`ext_dts.c`, `ext_uart.c`).
- **Carried over unchanged**: the XiP bootloader, the UCPD/USBPD sink policy,
  the USB CDC console + CLI, the INA226 monitor, the embedded PD tracer, the
  cache/MPU/linker fixes and the interrupt-priority balance — i.e. the
  firmware behaves exactly like the previous revision.

### Features added on top of the merge

- **USART2 is a second, fully equivalent console.** Output is mirrored to the
  USB-HS CDC port *and* USART2 from one queue with per-sink cursors, and input
  from either port feeds the same CLI line editor, so the MCU can be driven
  from one console or from both simultaneously (`console` command).
- **SoC temperature over the DTS** (`dtsmon.c`): `dts` prints the die
  temperature, `dts unit c|f` picks the scale, and `dts auto on|off` /
  `dts period <ms>` control periodic reporting exactly like the INA226
  section. `status` gained a `dts` line.

### Link failure fixed: wrong linker script in the Debug configurations

`ld: undefined reference to '_Heap_Limit'` plus
`memory region 'RAM_NONCACHEABLEBUFFER' not declared` came from the **Debug**
configuration of *both* projects linking against CubeMX's
`*__default_MMT_TEMPLATE.ld`. That template neither defines `_Heap_Limit` (which
this project's `sysmem.c` needs to bound the heap at the end of DMA-accessible
RAM) nor declares the `RAM_NONCACHEABLEBUFFER` region its own `RW_NONCACHEABLE`
section is placed into — so the USB/CDC DMA buffers had nowhere valid to go.

V1 used one correct script for both configurations; the trimmed V2 CubeMX
project left Debug pointing at the MMT template, and the merge kept that. Both
configurations of both projects now use the real scripts, matching V1:

| Project | Debug | Release |
| --- | --- | --- |
| Boot  | `STM32H7R3Z8JX_FLASH.ld`    | `STM32H7R3Z8JX_FLASH.ld`    |
| Appli | `STM32H7R3Z8JX_ROMxspi1.ld` | `STM32H7R3Z8JX_ROMxspi1.ld` |

`tools/check_arm_build.py` refuses to run if the two configurations of a project
disagree on `-T`, so this cannot silently regress.

### Build checks in `tools/`

```
bash tools/check_syntax.sh        host gcc -fsyntax-only, every TU, -Wall -Wextra
python3 tools/check_symbols.py    symbol definition / reference report
python3 tools/check_arm_build.py  real Cortex-M7 cross-compile + link of both projects
```

`check_arm_build.py` needs a `zig` on PATH or the `ziglang` PyPI package
(`python3 -m venv .venv && .venv/bin/pip install ziglang`). It compiles every
translation unit for `-mcpu=cortex-m7` and links against the project's own
linker script and the prebuilt USBPD core library, then prints the memory
usage. It is clang rather than GCC 14.3, so treat it as a link/geometry check,
not a byte-for-byte reproduction of the CubeIDE output.

**Last verified:** `check_syntax.sh` 44/44 and `check_arm_build.py` PASS —
`Boot` FLASH 24 584 B / 64 KB, RAM 312 B; `Appli` FLASH 148 240 B / 8 MB,
RAM 23 360 B / 440 KB, `RAM_NONCACHEABLEBUFFER` 5 408 B / 8 KB, DTCM 4 KB /
64 KB. One warning, benign and pre-existing: `w25qxx_xspi.c:224 unused function
'W25QXX_Wait_Busy'` in Boot — a static helper in the vendor flash driver that
nothing calls. Left alone deliberately; removing it would touch the Boot flash
driver for zero functional gain.

Note for anyone running the harness: `zig` reuses its compilation cache and does
not replay compiler diagnostics on a cache hit, so an older version of this tool
reported 0 warnings when the cache was warm and 1 when cold. It now gives each
run its own cache directory, so the count is reproducible.

### Bug fixes in this revision

| Where | Was | Now |
| --- | --- | --- |
| `ext_uart.c` | `EXT_UART_Read()` read the data register directly, stealing bytes from the USART2 interrupt path | reads come from a 256-byte RX FIFO the ISR fills; the direct register read is only used when interrupt reception is *not* armed. RX/TX counters and a drop counter are exposed on `console` |
| `usart.c` | CubeMX gives `USART2_IRQn` priority **0** — the highest in the system, above USB (4), UCPD (5) and the PD trace (6) | priority **7** in a USER CODE section, so a paste on the console cannot delay enumeration or PD handling |
| `app_cli.c` | `APP_CLI_OnRx()` was safe with one producer; with CDC *and* USART2 feeding it, the two ISRs could interleave and corrupt the RX ring | the enqueue is wrapped in a short critical section |
| `app_cli.c` | `parse_u` / `parse_h` accepted trailing garbage (`req 5abc` was taken as `req 5`) | both parsers reject anything after the number |
| `app_cli.c` | the boot banner was gated on the USB DTR line, so a UART-only session got no banner and no prompt | the banner fires for whichever console comes up first |
| `app_log.c` | `APP_LOG_Flush()` dequeued the chunk *before* `CDC_Transmit_HS()`, so a `USBD_BUSY` return silently lost those bytes | copy-then-commit: the cursor only advances once the stack accepted the transfer |
| `app_log.c` | one reader, so a stuck sink could not be muted without losing output | per-sink cursors, per-sink mute, and a drop counter (`console`) |
| `app_cli.c` | `console both off` would have muted every output | refused — at least one sink must stay on |
| `ina226.h` | documented `ina scan` as 0x40–0x4F while the code scans 0x08–0x77 | the doc matches the code |

## APIE — Advanced PD Intelligence Engine

This revision extends the working ST UCPD/USBPD sink into an advanced universal
PD sink platform.  The **ST PE/PRL/CAD stack is untouched**; `apie_*.c` sits
**above** the application/DPM side and only observes + extends it.  There
remains exactly one authoritative real-time PD policy/protocol path (the ST
stack); APIE never re-enters it.  This is the feature set:

| Area | What it does | Status |
| --- | --- | --- |
| Deterministic PD decoder | `apie_decode.c` — header, PDO/APDO (Fixed/Variable/Battery/PPS/AVS), VDM/SVDM/UVDM, cable VDOs, from normative spec bit fields | HOST VERIFIED (selftest 51/51) + BUILD VERIFIED |
| Bounded raw analyzer | `apie_analyzer.c` — 64-slot raw packet ring + capture from the ST RX buffer (copy-only; ST DMA ownership untouched) | BUILD VERIFIED |
| Transaction engine | `apie_analyzer.c` — request/response correlation, latency, outcome history | BUILD VERIFIED |
| Source fingerprint | `apie_profile.c` — hard (VID/PID/FW/HW) + protocol (PDO/PPS/EPR/SVID) + behaviour; **no hardcoded VID/PID tree** | BUILD VERIFIED |
| Unknown-protocol analysis | `apie_unknown.c` — bucket + correlate un-named behaviour, produce **UNKNOWN_SIGNATURE** (frequency, stable/changing bytes, bit changes, entropy, state/VBUS/current/temp/reset correlation, category + confidence hypothesis) | HOST VERIFIED (36/36) + BUILD VERIFIED |
| Statistical learning | `apie_stats.c` — Welford mean/variance, success-rate trackers, outcome histograms | BUILD VERIFIED |
| Embedded ML | `apie_ml.c` — online Naive Bayes + logistic head, **decision-tree classifier**, **online anomaly detector**, model id/version/CRC validation, no random weights | HOST VERIFIED (in `selftest ml`) + BUILD VERIFIED |
| Adaptive query scheduler | `apie_plan.c` — serialized informational queries, learned cooldown, negative-capability suppression | BUILD VERIFIED |
| Information-gain selection | `apie_plan.c` — Bernoulli-variance surrogate (libm-free) drives query priority | BUILD VERIFIED |
| One-command self-test | `apie_selftest.c` — `selftest [all|quick|full|pd|decoder|ml|database|flash]`, fully non-destructive (no power requests / no voltage change / no NOR program-erase) | BUILD VERIFIED |
| Simulation / replay | `tools/apie_replay.py --mode=synthetic|mutate|fuzz` — synthetic sessions, bit-flip mutation, malformed-packet decoder fuzzing; 0 decoder crashes | HOST VERIFIED |
| Knowledge package | `research/` — schema-versioned CRC blob with message tables, PDO/APDO metadata, safety gates, **charging-transport table**, **packet schemas** | HOST VERIFIED |
| Safe experimentation | R0/R1/R2 ON within limits, R3/R4 OFF (compile-gated); never auto-transmits unknown packets | IMPLEMENTED (R3/R4 DISABLED) |
| Knowledge database | `apie_db.c` — versioned, CRC-32, deduped RAM store (NOR persist DISABLED for XIP safety) | BUILD VERIFIED |
| Cable intelligence | `apie_cable.c` — SOP'/SOP'' identity → cable VID/PID/current/active/vconn, separate from source | BUILD VERIFIED |
| EPR/AVS awareness | `apie_cable.c` — AVS/EPR decoded + tracked; **never energised** (`APIE_HW_EPR_POWER_ENABLED=0`) | HARDWARE-LIMITED / FUTURE |
| VDM/SVDM/UVDM observation | via the VDM user callbacks + decoder | BUILD VERIFIED |
| Diagnostics CLI | `apie` command set — status, pdstats, packets, source/fingerprint, txns, ml, unknown, scheduler, db, experiment | BUILD VERIFIED |
| Host training/analysis | `tools/train_apie.py` (model seed), `tools/apie_decode.py`, `apie_decode_selftest.c`, `apie_selftest.sh` | HOST VERIFIED |
| Embedded knowledge package | `tools/build_knowledge.py` → `research/usb_pd_knowledge.json`, `research/pd_knowledge.bin`, `research/pd_knowledge.h` (deterministic, schema-versioned, CRC) | HOST VERIFIED |
| CLI routing coverage | `tools/cli_coverage.py` statically verifies every advertised command routes | HOST VERIFIED |
| Packet regression vectors | PB722 flows (caps/req/accept/ps_rdy, Not_Supported, PPS_Status, identity) in `apie_decode_selftest.c` + `apie_decode.py` | HOST VERIFIED |

**Safety / fault containment.**  Max voltage 21 V, max current 5 A, PPS step
100 mV; EPR/AVS power is gated OFF on this board.  All heavy analysis runs in
the super loop (`APIE_Task()`), never in the UCPD/DMA/USB ISRs.  The only thing
done in interrupt context is a bounded copy + enqueue + return from the
RXMSGGEND bridge in `usbpd_hw_if_it.c`, which does **not** change the ST PRL RX
buffer ownership or DMA re-arm ordering (`Ports[0].ptr_RxBuff` remains the
single DMA RX buffer).

**Provenance docs:** [`THIRD_PARTY_SOURCES.md`](THIRD_PARTY_SOURCES.md),
[`PROTOCOL_SOURCES.md`](PROTOCOL_SOURCES.md),
[`LICENSE_MATRIX.md`](LICENSE_MATRIX.md).  No GPL/copyleft code is introduced;
the APIE layer is original work and references only normative spec field
positions and public library APIs.

### CLI

```
apie status          state, counters, analyzer/db/unknown/safety summary
apie pdstats         live UCPD driver counters (irq/rx/tx/hard-reset)
apie packets [all]   raw captured packets (decoded header + payload)
apie source          source profile + cable + EPR diagram
apie fingerprint     signature, PDO count, PPS/EPR/SVID/battery/identity
apie txnlist         transaction history
apie counters        analyzer / txn / unknown / ml counters
apie safety          safety limits + HW capability flags
apie feature         current feature vector
apie ml              model id/version/CRC + class counts
apie predict <Q>     ML "is query Q worth trying" for query id 0..8
apie unknown         unknown-protocol buckets
apie sched           scheduler state per query
apie db              knowledge-database dump
apie exp [0..4]      show / set experiment level (R3/R4 ignored if not compiled in)
```

`ap <sub>` is an alias for the same sub-commands (`ap status`, `ap packets`,
`ap source`, `ap fingerprint`, `ap txn`, `ap feature`, `ap unknown`,
`ap knowledge`, `ap scheduler`, `ap ml`, `ap predict`, `ap experiment`,
`ap replay`, `ap safety`, ...). The same sub-commands also work bare (`ml`,
`predict`, `scheduler`, `db`, `safety`, `diag`, `source`, `fingerprint`,
`txn`, `unknown`, `feature`, `packets`, `stats`, `raw`). Expanded command
families:

```
db [status|dump]        knowledge-database status
db validate             CRC-validate every stored profile
db compact              compact / re-index the store
db test                 scratch store/read-back self-test
db wear|writes|erases|checkpoint   flash-endurance accounting
safety [status|limits]  safety limits + hardware capability flags
selftest [scope]        one-command non-destructive self-test
selftest quick|full|pd|decoder|ml|database|flash   scoped self-test
packets [raw|decoded|unknown|tx|rx] [all]   packet-ring views
transactions [active|history]   transaction views
diag pd|rx|tx|txn|decoder|ucpd|usb|queue|timing|cpu|memory|profile|unknown|faults|trace|ml|scheduler|knowledge|packets|db|safety|flash
```

`diag timing`/`diag cpu` report the super-loop period and the APIE per-call
compute budget (DWT cycle counter). `diag flash` and `db wear|erases` report
the true flash-persistence state (RAM backend, NOR off for XIP safety — see
[FLASH_ENDURANCE.md](FLASH_ENDURANCE.md)). `selftest` runs every non-destructive
check automatically — no power request, no voltage change, no NOR program/erase
— and reports pass/fail counts per scope.

**Status labels used** (per the deliverable convention): IMPLEMENTED, BUILD
VERIFIED, HOST VERIFIED, HARDWARE VERIFIED, RESEARCHED, OBSERVATION ONLY,
HARDWARE-LIMITED, DISABLED, UNTESTED, FUTURE.  Nothing is claimed as
**HARDWARE VERIFIED** — the APIE platform has been cross-compiled, linked and
host-tested, but not yet proven on the bench with a live PD source.

**Last verified (APIE):** `check_syntax.sh` **55/55**; `check_arm_build.py`
**PASS** — `Appli` FLASH 188 268 B / 8 MB, RAM 34 008 B / 440 KB,
`RAM_NONCACHEABLEBUFFER` 5 408 B / 8 KB, DTCM 4 KB / 64 KB; `Boot` unchanged and
PASS.  `Appli` compiles with **0 warnings**; the only warning the clang-based
harness reports is the same pre-existing Boot `W25QXX_Wait_Busy` one noted above
(absent under the real GCC toolchain).  Host tests: `apie_selftest.sh` 67/67,
`apie_unknown_selftest.sh` 36/36, `apie_decode.py selftest`, `build_knowledge.py
--verify`, `cli_coverage.py`, `apie_replay.py --mode=synthetic|mutate|fuzz`
(0 decoder crashes).

