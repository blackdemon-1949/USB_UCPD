# PROTOCOL_SOURCES

Every protocol rule, bit-field position, message type, and policy decision that
the APIE intelligence layer depends on, with the source it comes from.  The rule
is: **no fabricated protocol rules, no guessed vendor commands, no random AI.**
If a rule is not traceable to a standard, a documented implementation, lawfully
provided vendor material, or empirically captured evidence, it is not used.

Status labels: `IMPLEMENTED`, `BUILD VERIFIED`, `HOST VERIFIED`,
`HARDWARE VERIFIED`, `RESEARCHED`, `OBSERVATION ONLY`, `HARDWARE-LIMITED`,
`DISABLED`, `UNTESTED`, `FUTURE`.

## Field layouts (decoder, `apie_decode.c`)

All field positions were cross-checked against BOTH the normative USB PD
specification and the authoritative ST USBPD bit-field structs
(`Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_def.h`), which match
(`USBPD_PDO_TYPE_Pos == 30U`).  The firmware decoder is written from the raw
spec field positions (not the ST struct) so it is host-testable and independent
of the ST layout.

| Rule / field | Position | Source | Status |
| --- | --- | --- | --- |
| PD message header low 16 bits, little-endian wire order | byte0 = header[7:0], byte1 = header[15:8] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Message type | bits[4:0] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Port data role | bit[6] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Specification revision | bit[7] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Port power role | bit[9] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Message ID | bits[11:10] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Number of data objects | bits[14:12] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| Extended (chunked) flag | bit[15] | USB PD 3.0 §6.2.1.1 | BUILD VERIFIED |
| **PDO type** | **bits[31:30]** | USB PD 3.0 §6.4.2; matches `USBPD_PDO_TYPE_Pos=30` | BUILD VERIFIED |
| Fixed PDO voltage | bits[19:10], 50 mV/unit | USB PD 3.0 §6.4.2 | BUILD VERIFIED |
| Fixed PDO max current | bits[9:0], 10 mA/unit | USB PD 3.0 §6.4.2 | BUILD VERIFIED |
| Battery PDO min/max voltage | bits[19:10] / bits[29:20], 50 mV | USB PD 3.0 §6.4.2 | BUILD VERIFIED |
| Battery PDO max power | bits[9:0], 250 mW/unit | USB PD 3.0 §6.4.2 | BUILD VERIFIED |
| Variable PDO min/max voltage | bits[19:10] / bits[29:20], 50 mV | USB PD 3.0 §6.4.2 | BUILD VERIFIED |
| Variable PDO max current | bits[9:0], 10 mA/unit | USB PD 3.0 §6.4.2 | BUILD VERIFIED |
| **APDO sub-type** | **bits[29:28]** | USB PD 3.1 §6.4.2.3.1.3; matches ST `USBPD_PDO_SRC_APDO_PPS_Pos=28` | BUILD VERIFIED |
| PPS min/max voltage | bits[15:8] / bits[24:17], 100 mV | USB PD 3.0 §6.4.2.3.1.3 | BUILD VERIFIED |
| PPS max current | bits[6:0], 50 mA/unit | USB PD 3.0 §6.4.2.3.1.3 | BUILD VERIFIED |
| AVS min voltage | bits[15:8], 100 mV | USB PD 3.1 §6.4.2.3.1.3.2 | BUILD VERIFIED |
| AVS max voltage | bits[25:17], 100 mV | USB PD 3.1 §6.4.2.3.1.3.2 | BUILD VERIFIED |
| AVS PDP | bits[7:0], 1 W/unit | USB PD 3.1 §6.4.2.3.1.3.2 (source/sink) | BUILD VERIFIED |
| **VDM: Structured-VDM type** | **bit[15]** (=1 structured) | USB PD 3.0 §6.4.3 | BUILD VERIFIED |
| VDM: SVID field | bits[31:16] | USB PD 3.0 §6.4.3 | BUILD VERIFIED |
| VDM: Structured VDM version | bits[14:13] | USB PD 3.0 §6.4.3 | BUILD VERIFIED |
| VDM: command type (REQ/ACK/NAK/BUSY) | bits[7:6] | USB PD 3.0 §6.4.3 | BUILD VERIFIED |
| VDM: command | bits[4:0] | USB PD 3.0 §6.4.3 | BUILD VERIFIED |
| SVDM Discover Identity / SVIDs / Modes / Enter / Exit / Attention | command 0x01..0x06 | USB PD 3.0 §6.4.3 | BUILD VERIFIED |
| Cable (SOP'/SOP'') current capability | cable VDO bits[9:7] | USB Type-C / PD (cable VDO) | BUILD VERIFIED |

## Message types (names & numeric IDs)

The control/data instruction set follows USB PD 3.0 §6.3 (control) and §6.5
(data).  `apie_decode.c` names them from the specification tables directly
(e.g., `Get_Status = 0x12`, `Source_Capabilities = 0x01`, `Accept = 0x03`,
`Reject = 0x04`, `Not_Supported = 0x10`, `Vendor_Defined = 0x0F`).

## Policy rules (scheduler / experiments / safety)

| Rule | Source | Status |
| --- | --- | --- |
| Informational queries only issued once a source contract is established | ST USBPD stack requires PE_READY for extended/VDM informational messages | IMPLEMENTED |
| Never issue more than `APIE_QUERY_MAX_PENDING` informational queries at once | local bounded-resource policy (no spec "storm" is permitted) | IMPLEMENTED |
| Respect a per-query cooldown; back off on repeated failure, relax on success | local bounded-rate policy (keeps the source acceptable) | IMPLEMENTED |
| Permanently suppress a query after 3 confirmed failures with 0 successes (negative capability) | local evidence-based policy | IMPLEMENTED |
| R0 observe always allowed; R1 info-query, R2 power-request gated by experiment level | safe-experiment gating | IMPLEMENTED |
| R3/R4 require the compile-time gate `APIE_EXP_ALLOW_R3` / `APIE_EXP_ALLOW_R4` (both 0) | board safety policy — state-changing / unknown-transmission experiments are OFF | IMPLEMENTED (DISABLED) |
| Absolute voltage ceil `APIE_MAX_VOLTAGE_MV` = 21 V; current ceil 5 A; PPS step 100 mV | **board hardware limitation** (do not energise >20 V; PPS step kept coarse for safe ramping) | HARDWARE-LIMITED |
| **EPR/AVS never energised** — `APIE_EPR_PowerAllowed()` returns 1 only when `APIE_HW_EPR_POWER_ENABLED` is set (0 on this board) | board validation policy | HARDWARE-LIMITED |
| AVS/EPR is **decoded and tracked** so a future validated board can enable it | USB PD 3.1 spec (architecture only) | FUTURE |

## Empirical / evidence-only facts

| Fact | Source | Status |
| --- | --- | --- |
| A plain charge source often does **not** answer `Discover_Identity` (NAK on first attempt) | observed behaviour in session notes (PB722-style source in hand) | OBSERVATION ONLY |
| `Get_Status` / `Get_PPS` are answered when a source supports PPS | documented PPS sources (USB-IF); cross-checked | RESEARCHED |
| Battery capability is frequently `Not_Supported` on adapters/power-banks | observed + USB-IF policy | OBSERVATION ONLY |

These are treated as **priors / hypotheses**, never as hardcoded VID/PID
decisions.  They are encoded only as the *initial belief* in the ML seed model
and are re-weighted by real online evidence at runtime.

## Embedded ML (no random AI)

The classifier (`apie_ml.c`) is an **online Naive Bayes** updated from observed
outcomes with Laplace smoothing.  It has **no randomly initialised weights**.
The shipped seed parameters are produced by the host pipeline
`tools/train_apie.py` from a labelled **documented-behaviour seed set** (not
random; see `tools/train_apie.py::build_seed_rows()` and
`tools/apie_model_seed.json`).  Model metadata carries a version, feature
version and CRC so a corrupt/mismatched model is detected
(`APIE_Ml_Validate`).  The seed is the only part not yet shown to be predictive
on real hardware → status `UNTESTED` until capture data is fed back through
the training pipeline.

## Charging-transport facts (non-PD systems)

Many vendor "fast charging" systems are **not** USB-PD and do **not** ride the
CC line.  Their physical transport must be recorded correctly so a proprietary
D+/D- protocol is never mis-classified as USB-PD:

| Charging system | Transport | Observable through UCPD (CC) here? |
| --- | --- | --- |
| USB-PD / PPS / EPR / VDM | USB-C CC | **Yes** |
| USB-C Rp / default current | USB-C CC | **Yes** |
| BC1.2 | D+/D- | No (D+/D- not wired) |
| Qualcomm Quick Charge (QC) | D+/D- | No |
| Samsung AFC | D+/D- | No |
| Huawei SuperCharge (SCP/FCP) | D+/D- | No |
| OPPO/OnePlus/Realme VOOC/SUPERVOOC | D+/D- + VBUS signalling | No |
| Vivo FlashCharge | proprietary D+/D- | No |
| Xiaomi Mi/Redmi | USB-PD + proprietary D+/D- | Partial (only the USB-PD part) |

The complete table is embedded in the knowledge package as the `TRAN` section
(`research/usb_pd_knowledge.json` → `charging_transport`).  Because the board's
PD connector does not expose D+/D-, the firmware makes no D+/D- or non-PD
charging claim; the transport facts exist for correctness and for a future
external D+/D- analyzer.

## Host tooling

`tools/apie_decode.py` and `tools/apie_decode_selftest.c` independently verify
the firmware decoder's field layout on the host (`bash tools/apie_selftest.sh`,
currently 67/67 checks passing, including PB722 vectors).
`tools/apie_unknown_selftest.sh` verifies the UNKNOWN_SIGNATURE
characterization (36/36).  `tools/train_apie.py` converts a labelled capture
CSV into a C model seed.  `tools/apie_replay.py --mode=synthetic|mutate|fuzz`
generates synthetic sessions and mutation/fuzz-tests the decoder (0 crashes).
`tools/build_knowledge.py` emits the knowledge package.  `tools/cli_coverage.py`
proves every advertised CLI command routes.  `tools/check_arm_build.py`
cross-compiles and links the whole application for Cortex-M7 (PASS).
