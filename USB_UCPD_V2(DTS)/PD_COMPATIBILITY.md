# USB-PD Compatibility

This document records the USB-PD / USB-C coverage of the firmware: what the ST
middleware already supports, what APIE adds at the application level, and what
is protocol awareness vs. electrical hardware capability.

## Electrical hardware truth (do not over-claim)

| Capability | Reality |
| --- | --- |
| UCPD PHY | STM32H7R3 on-die UCPD1, CC1/CC2 on PM0/PM1 |
| VBUS sensing | **no VBUS ADC on this wiring** → synthetic VBUS policy model |
| D+/D- | **not wired** to the PD connector → no D+/D- / BC1.2 / QC / VOOC claims |
| EPR (>20 V, 28–48 V) | **not energised** — gated by `APIE_HW_EPR_POWER_ENABLED=0` |
| Cable e-marker (SOP'/SOP'') | observable over UCPD (cable VDOs) |

Non-PD charging protocols (QC, AFC, Huawei/Vivo/Xiaomi, VOOC/SUPERVOOC) use
D+/D-, other USB data, BLE, or proprietary physical signalling. The current
connector does **not** expose D+/D-, so the firmware makes **no** D+/D-/
proprietary-charging compatibility claim. Protocol awareness is provided only
through the CC/USB-PD path. An external analyzer architecture can be added
later; it is not part of this firmware.

## Type-C coverage

- CC1/CC2 attach/detach, orientation via the ST CAD layer.
- Rp/Rd, advertised current, default/1.5 A/3 A awareness where the ST stack
  exposes it.
- DRP / Try.SRC / Try.SNK: left to the ST CAD configuration.
- VCONN and cable discovery: SOP'/SOP'' identity and cable VDOs are decoded
  and tracked separately from the power source (`apie_cable.c`).
- Dead-battery behaviour: not physically exercised on this bench.

## USB-PD message coverage

The decoder (`apie_decode.c`) and CLI can observe/decode (standards-backed):

**Control:** GoodCRC, GotoMin, Accept, Reject, Ping, PS_RDY, Get_Source_Cap,
Get_Sink_Cap, DR_Swap, PR_Swap, VCONN_Swap, Wait, Soft_Reset, Data_Reset,
Data_Reset_Complete, Not_Supported, Get_Source_Cap_Ext, Get_Status, FR_Swap,
Get_PPS_Status, Get_Country_Codes, Get_Sink_Cap_Ext, Get_Source_Info,
Get_Revision.

**Data:** Source_Capabilities, Request, BIST, Sink_Capabilities,
Battery_Status, Alert, Get_Country_Info, Enter_USB, EPR_Request, EPR_Mode,
Source_Info, Revision, Vendor_Defined.

**Extended:** Source_Capabilities Ext, Status, Battery Cap/Status,
Manufacturer Info, Security, FW Update, PPS Status, Country Info/Codes,
Sink Capabilities, EPR Source/Sink Capa, VDM.

**PDO/APDO:** Fixed, Battery, Variable, PPS APDO, AVS APDO, EPR fixed PDO
awareness.

**VDM:** SVDM Discover Identity/SVIDs/Modes, Enter/Exit Mode, Attention,
Specific; UVDM observation.

**AMS:** request/accept/PS_RDY, PR/DR/VCONN swap, soft/hard reset, Get_Status,
Get_PPS_Status, extended information queries, VDM discovery — surfaced through
the DPM/VDM callbacks and APIE transaction engine.

## What is protocol support vs. electrical support

`PROTOCOL_SUPPORT` (decode/observe/negotiate within the ST stack) is separate
from `ELECTRICAL_HARDWARE_SUPPORT` (physically safe to energise). EPR/AVS is
decoded and tracked (`apie_cable.c`) but **never energised** because the board
electrical capability is not validated. The two are kept separate so that
adding EPR awareness does not imply EPR output.

## Source-agnostic design

There is no `if (UGREEN) ... if (Apple) ...` tree. Source knowledge lives in a
`APIE_Profile_t` (hard identity, PDO signature, PPS/EPR/SVID signature,
behaviour, query results) and is driven by **evidence**, not vendor branches.
Known-vendor observations are recorded as *observations* (see the knowledge
package, `research/usb_pd_knowledge.json`) and are never universal rules.

## PB722 as a validation device (OBSERVATION ONLY)

Observed: VID 0x2DC0 / PID 0x020B (FW 48 / HW 16); 5/9/12/15/20 V + PPS
3.3–21 V; Battery → Not_Supported; Identity → NAK; no source SVID observed.
SOP' cable traffic used SVID 0xFF00 — that is **cable** traffic and must not be
attributed to the source. See the PB722 regression vectors in
`tools/apie_decode_selftest.c` and `tools/apie_decode.py`.
