# Using STM32CubeMonitor-UCPD with this firmware

This build streams the USB-PD **TRACER_EMB** trace (TLV framed) out of a UART so
**STM32CubeMonitor-UCPD** can monitor the sink port (PD messages, CC state,
contracts, hard resets, …) live. The text CLI on the USB-HS Type-C port is still
there for status/diagnostics (`status`, `info`, `pd`, `caps`, …), but the trace
for CubeMonitor-UCPD is on the dedicated UART below.

## 1. Hardware wiring (WeAct STM32H7R3Z8J6)

| Board pin | Function         | USB-UART adapter |
|-----------|------------------|------------------|
| PA9       | USART1_TX (trace)| RX               |
| PA10      | USART1_RX        | TX               |
| GND       | GND              | GND              |

Both pins are on the 2.54 mm header (the 144-pin H7R3Z8 has **no PD9**, so
USART3's PD8/PD9 pairing was not usable). Use a 3.3 V USB-UART adapter
(FTDI / CH340 / CP2102). Do **not** use 5 V logic.

(Alternatives also routed on the header, if you ever need PA9/PA10 for
something else: USART3 on PB10/PB11 — edit
`Appli/Core/Inc/tracer_emb_conf.h`, the values are listed at the top.
USART2 is **not** an alternative any more: it is now configured by CubeMX on
PD5/PD6 and reserved as a feature footprint, see `ext_uart.c`.)

## 2. Build & flash

1. Open the `USB_UCPD` project in STM32CubeIDE.
2. **Clean** and **Rebuild** the **Appli** (Debug uses `STM32H7R3Z8JX_ROMxspi1.ld`,
   same as Release). The clean build matters because `_TRACE` and the new tracer
   files were added.
3. Flash as usual (bootloader `Boot` in internal flash + `Appli` in XiP NOR).

## 3. Connect STM32CubeMonitor-UCPD

1. Plug the USB-UART adapter into the PC and note its COM port.
2. Start STM32CubeMonitor-UCPD.
3. Create/open a project, add the board node, and select the **TRACER** interface.
4. Point the TRACER to the adapter's COM port at **921600 baud, 8N1**.
5. Attach a PD source to PM0 (CC1) or PM1 (CC2) + GND.

You should see CC attach, Source_Capabilities, the Request/Accept exchange and the
explicit contract in the monitor.

## Reading the trace

Once a second the firmware emits a `PHY ...` debug line with the raw UCPD
counters, so you can tell what the CC line is actually doing:

- `ord=N` — the UCPD detected N ordered-sets (SOP/hard reset). If this stays
  0 while a source is attached, the source is not transmitting anything.
- `ok=N` / `err=N` — PD messages received with / without a CRC error.
- `rxhdr=0xXXXX` — header of the last received message. `0x7141`-style values
  are Source_Capabilities; `0x0036`-style (type 0x16) is a Status message.
- `rxbuf=0x2400xxxx` — the DMA RX buffer must be in AXI SRAM (0x24000000).

`ERROR with Trace_Notif :78` is **not** an error: notify 78 is
`USBPD_NOTIFY_STATUS_RECEIVED` (PD3.1), which CubeMonitor's decoder predates.
It means the sink successfully received a *Status* message — proof the RX path
is working.

A full successful negotiation looks like this in the trace:

```
ATTACHED -> PE_SNK_WAIT_FOR_CAPABILITIES
IN  SRC_CAPABILITIES   ->  OUT GOODCRC
PE_SNK_EVALUATE_CAPABILITY -> PE_SNK_SEND_REQUEST -> OUT REQUEST
IN  GOODCRC -> IN ACCEPT -> OUT GOODCRC -> IN PS_RDY -> OUT GOODCRC
POWER_EXPLICIT_CONTRACT -> PE_STATE_READY -> STATE_SNK_READY
```

The sink may emit up to three `HRST` lines first. That is *expected* with slow
sources: `tTypeCSinkWaitCap` is 400 ms and the sink is allowed three attempts,
so if the charger takes ~2 s before its first Source_Capabilities (many do),
you see three hard resets and then the negotiation completes on its own. The
`PHY` line proves the source was silent during that window: `ord=0` means the
UCPD saw no ordered-sets at all, so the sink simply had nothing to receive.
`sop=0` in the `PHY` line means SOP (SOP'=1, SOP''=2...).

## Console command reference (USB Type-C CDC)

The console answers in plain English. Most useful commands:

```
caps                 list the power levels the source offers
req <n> [ma]         ask for fixed level n (1 = first) at a current
volt <mv> [ma]       ask for the closest level (fixed or PPS) to a voltage
pps <mv> [ma]        ask for an exact PPS voltage (20 mV steps) + current

auto <mv> [ma]       after each attach, automatically ask for this voltage
auto off             back to the default 5 V
remember on|off      re-apply the last request after the next attach
sweep <from> <to> <step> [ma]   step PPS voltage from..to (mV)
sweep stop

getstatus            source status: temperature + protection faults
getpps               source real output voltage + current (PPS)
srcext               extended source info: VID/PID/XID, firmware, max power
manuinfo             manufacturer name + IDs
battery              battery capability + status (if the source has one)
countrycodes         country codes the source supports
countryinfo <XX>     country-specific info (e.g. countryinfo US)
identify             VDM: ask the source to identify itself
svids                VDM: alternate-mode SVIDs
modes <svid>         VDM: modes of an SVID (e.g. modes ff01)

status               what is connected and what we asked for
getcaps              re-read the power levels
softreset / hardreset
pd / info / led / help
```

Notes:

- `getstatus` / `getpps` / `srcext` / `manuinfo` / `battery` / `country*` are
  standard PD requests a sink is allowed to send; the reply is decoded and
  printed automatically when it arrives (usually < 50 ms later).
- `identify` / `svids` / `modes` are VDM messages normally initiated by a
  *source*; some chargers NAK them from a sink. If so, the console says so —
  `srcext` + `manuinfo` are the reliable alternatives.
- EPR (28 V / 36 V / 48 V) and PRS/DRS role swaps are not enabled in this
  build (sink-only, SPR PDOs + PPS).

## Files added for the trace

- `Appli/Core/Src/tracer_emb.c` / `.h`      – TRACER_EMB protocol + ring buffer
- `Appli/Core/Src/tracer_emb_hw.c` / `.h`   – USART1 + GPDMA1 CH2 TX transport
- `Appli/Core/Inc/tracer_emb_conf.h`        – UART / pin / DMA selection
- `Drivers/STM32H7RSxx_HAL_Driver/.../stm32h7rsxx_ll_usart.c` / `.h` – LL USART driver
- `_TRACE` added to the compiler defines (Debug + Release)
- `USBPD_TRACE_Init()` called in `usbpd_dpm_core.c`
- `USART1_IRQHandler` / `GPDMA1_Channel2_IRQHandler` in `stm32h7rsxx_it.c`
