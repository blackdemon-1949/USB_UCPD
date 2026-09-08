# PD Decoder

`apie_decode.c` is a deterministic, standards-backed USB-PD message decoder
that is **independent** of the ST USBPD header layout. Because it has no
dependency on the middleware internals, the exact same decoder can be compiled
on the host and checked against known wire values.

## Field positions (normative)

16-bit header:

| Bit(s) | Field |
| --- | --- |
| [4:0] | message type |
| [5]   | reserved |
| [6]   | Port Data Role (0=UFP consumer, 1=DFP provider) |
| [7]   | Spec Revision (0=PD2.0, 1=PD3.0, 2=PD3.1 in 3.1) |
| [8]   | reserved |
| [9]   | Port Power Role (0=SNK, 1=SRC) |
| [11:10] | Message ID |
| [14:12] | NumDataObjects |
| [15]   | Extended |

PDO type lives in **bits[31:30]** (reverse of a common LSB misreading); APDO
sub-type in **bits[29:28]**. PDO/APDO electrical fields:

- Fixed: voltage bits[19:10] × 50 mV, current bits[9:0] × 10 mA.
- Battery: min bits[19:10] × 50 mV, max bits[29:20] × 50 mV, power bits[9:0] × 250 mW.
- Variable: min bits[19:10], max bits[29:20], current bits[9:0] × 10 mA.
- PPS APDO: min bits[15:8] × 100 mV, max bits[24:17] × 100 mV, current bits[6:0] × 50 mA.
- AVS APDO: min bits[15:8] × 100 mV, max bits[25:17] × 100 mV, PDP bits[7:0] (W).

VDM header: SVID bits[31:16], structured flag bit[15], version bits[14:13],
command bits[4:0].

## One authoritative representation

There is **one** header decode (`APIE_Decode_Header`) shared by the decoder,
the transaction engine, the raw analyzer, the ML features, and the CLI. There
are no competing message-header definitions.

## Semantics are not inferred from type integers alone

Control vs. data messages share numeric values (e.g. `0x01` is GoodCRC control
*and* Source_Capabilities data). `APIE_Decode_TypeNameN` resolves the name
using **message type + number-of-objects + extended bit + context**, never from
the type integer alone. This is why a Request can never display as GotoMin
unless GotoMin was actually transmitted.

## Testability and regression

- Host self-test: `tools/apie_selftest.sh` builds the real `apie_decode.c` with
  host gcc and runs `apie_decode_selftest.c` (67 checks, including PB722
  vectors).
- Python cross-check: `tools/apie_decode.py` mirrors the same field positions
  and verifies independently (`python3 tools/apie_decode.py selftest`).
- Raw header → decoded message → transaction type are cross-checked in the
  PB722 regression vectors.

## Malformed-packet handling

The decoder is robust to bad length, bad NDO, invalid extended header,
unknown types, duplicate/out-of-order packets, message-ID changes, SOP
mismatch, timeout, reset, unsupported query and partial capture — see the test
section in [DIAGNOSTICS.md](DIAGNOSTICS.md) and the test tooling in the
repository (`tools/`, `tests/` if present).
