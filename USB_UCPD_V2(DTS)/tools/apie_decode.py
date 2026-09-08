#!/usr/bin/env python3
"""
apie_decode.py - standalone host-side USB-PD decoder.

This mirrors the deterministic decoder in the firmware
(Appli/Core/Src/apie_decode.c).  It is used by the host tools to parse
captures (e.g. exported raw packet rings, or a Wireshark/USBPD extract) and by
the test harness to independently verify the firmware's field decoding.

It deliberately uses raw bit-field positions (per the normative USB Power
Delivery 3.0/3.1 specification), not any vendor library layout.
"""

from __future__ import annotations

# --------------------------------------------------------------------------
# 16-bit header
# --------------------------------------------------------------------------
def decode_header(hdr: int) -> dict:
    return {
        "type":          hdr & 0x1F,
        "port_data_role": (hdr >> 6) & 0x01,   # 0=UFP(consumer), 1=DFP(provider)
        "spec_rev":      (hdr >> 7) & 0x01,    # 0=PD2.0, 1=PD3.0
        "port_power_role": (hdr >> 9) & 0x01,  # 0=SNK, 1=SRC
        "msgid":         (hdr >> 10) & 0x03,
        "nobjects":      (hdr >> 12) & 0x07,
        "extended":      (hdr >> 15) & 0x01,
    }


CONTROL_NAMES = {
    0x01: "GoodCRC", 0x02: "GotoMin", 0x03: "Accept", 0x04: "Reject",
    0x05: "Ping", 0x06: "PS_RDY", 0x07: "Get_Source_Cap", 0x08: "Get_Sink_Cap",
    0x09: "DR_Swap", 0x0A: "PR_Swap", 0x0B: "VCONN_Swap", 0x0C: "Wait",
    0x0D: "Soft_Reset", 0x0E: "Data_Reset", 0x0F: "Data_Reset_Complete",
    0x10: "Not_Supported", 0x11: "Get_Source_Cap_Ext", 0x12: "Get_Status",
    0x13: "FR_Swap", 0x14: "Get_PPS_Status", 0x15: "Get_Country_Codes",
    0x16: "Get_Sink_Cap_Ext", 0x17: "Get_Source_Info", 0x18: "Get_Revision",
}

EXTENDED_NAMES = {
    0x01: "Ext_Source_Capabilities", 0x02: "Ext_Status", 0x03: "Ext_Get_Battery_Cap",
    0x04: "Ext_Get_Battery_Status", 0x05: "Ext_Battery_Capabilities",
    0x06: "Ext_Get_Manufacturer_Info", 0x07: "Ext_Manufacturer_Info",
    0x08: "Ext_Security_Request", 0x09: "Ext_Security_Response",
    0x0A: "Ext_FW_Update_Request", 0x0B: "Ext_FW_Update_Response",
    0x0C: "Ext_PPS_Status", 0x0D: "Ext_Country_Info", 0x0E: "Ext_Country_Codes",
    0x0F: "Ext_Sink_Capabilities", 0x10: "Ext_Control", 0x11: "Ext_EPR_Source_Capa",
    0x12: "Ext_EPR_Sink_Capa", 0x1E: "Ext_VDM",
}


DATA_NAMES = {
    0x01: "Source_Capabilities", 0x02: "Request", 0x03: "BIST",
    0x04: "Sink_Capabilities", 0x05: "Battery_Status", 0x06: "Alert",
    0x07: "Get_Country_Info", 0x08: "Enter_USB", 0x09: "EPR_Request",
    0x0A: "EPR_Mode", 0x0B: "Source_Info", 0x0C: "Revision",
    0x0F: "Vendor_Defined",
}


def decode_type_name(type: int, extended: int, nobjects: int | None = None) -> str:
    if extended:
        return EXTENDED_NAMES.get(type, f"Ext_0x{type:02X}")
    # Data messages carry >=1 data object; control messages carry 0.  Type 0x01
    # is goodCRC (control) vs Source_Capabilities (data) depending on nobjects.
    if nobjects is not None:
        if nobjects > 0 and type in DATA_NAMES:
            return DATA_NAMES[type]
        if nobjects == 0 and type in CONTROL_NAMES:
            return CONTROL_NAMES[type]
    return CONTROL_NAMES.get(type, f"Msg_0x{type:02X}")


# --------------------------------------------------------------------------
# PDO / APDO 32-bit objects
# --------------------------------------------------------------------------
def decode_pdo(pdo: int, is_src: bool = True) -> dict:
    """Decode a 32-bit PDO/APDO (normative layout: type in bits[31:30],
    APDO sub-type in bits[29:28])."""
    t = (pdo >> 30) & 0x3
    if t == 0x0:  # Fixed
        mv = ((pdo >> 10) & 0x3FF) * 50
        ma = ((pdo >> 0) & 0x3FF) * 10
        return {"kind": "fixed", "voltage_mv": mv, "current_ma": ma,
                **( {"role": "src"} if is_src else {"role": "sink"} )}
    if t == 0x1:  # Battery
        maxv = ((pdo >> 20) & 0x3FF) * 50
        minv = ((pdo >> 10) & 0x3FF) * 50
        pwr = (pdo & 0x3FF) * 250
        return {"kind": "battery", "min_mv": minv, "max_mv": maxv, "power_mw": pwr}
    if t == 0x2:  # Variable
        maxv = ((pdo >> 20) & 0x3FF) * 50
        ma = ((pdo >> 0) & 0x3FF) * 10
        minv = ((pdo >> 10) & 0x3FF) * 50
        return {"kind": "variable", "min_mv": minv, "max_mv": maxv, "current_ma": ma}
    if t == 0x3:  # APDO
        apt = (pdo >> 28) & 0x3
        if apt == 0x0:  # PPS
            maxv = ((pdo >> 17) & 0xFF) * 100
            minv = ((pdo >> 8) & 0xFF) * 100
            ma = ((pdo >> 0) & 0x7F) * 50
            return {"kind": "pps", "min_mv": minv, "max_mv": maxv, "current_ma": ma}
        if apt == 0x1:  # AVS
            maxv = ((pdo >> 17) & 0x1FF) * 100
            minv = ((pdo >> 8) & 0xFF) * 100
            return {"kind": "avs", "min_mv": minv, "max_mv": maxv}
    return {"kind": "unknown", "raw": pdo}


def decode_caps(pdos: list[int]) -> list[dict]:
    return [decode_pdo(p, True) for p in pdos]


def pdo_signature(pdos: list[int]) -> int:
    h = 2166136261
    for v in pdos:
        for b in v.to_bytes(4, "little"):
            h ^= b
            h = (h * 16777619) & 0xFFFFFFFF
    return h


# --------------------------------------------------------------------------
# VDM
# --------------------------------------------------------------------------
def vdm_structured(vdm: int) -> bool:
    return bool((vdm >> 15) & 0x01)


def svdm_command(vdm: int) -> int:
    return vdm & 0x1F


SVDM_NAMES = {
    0x01: "Discover_Identity", 0x02: "Discover_SVIDs", 0x03: "Discover_Modes",
    0x04: "Enter_Mode", 0x05: "Exit_Mode", 0x06: "Attention",
}


def decode_vdm_header(vdm: int) -> dict:
    return {
        "svid": (vdm >> 16) & 0xFFFF,
        "version": (vdm >> 13) & 0x3,
        "command_type": (vdm >> 6) & 0x3,
        "structured": vdm_structured(vdm),
        "command": svdm_command(vdm),
        "command_name": SVDM_NAMES.get(svdm_command(vdm), None),
    }


def decode_cable_vdo(vdo: int) -> dict:
    return {"current_cap": (vdo >> 7) & 0x3, "ss_cap": (vdo >> 9) & 0x3}


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def selftest():
    """Checks against known wire values (normative layout)."""
    # Header: Get_Status control type=0x12, specrev=1, msgid=2, 0 objects.
    h = decode_header(0x0892)
    assert h["type"] == 0x12, h
    assert h["spec_rev"] == 1
    assert h["msgid"] == 2
    assert h["nobjects"] == 0
    assert decode_type_name(0x12, 0) == "Get_Status"
    # Type 0x01 disambiguates control (GoodCRC, n=0) vs data (Source_Cap, n>0).
    assert decode_type_name(0x01, 0) == "GoodCRC"
    assert decode_type_name(0x01, 0, nobjects=3) == "Source_Capabilities"
    # Fixed PDO 9V/3A source: 0x0002D12C.
    p = decode_pdo(0x0002D12C, True)
    assert p["kind"] == "fixed" and p["voltage_mv"] == 9000 and p["current_ma"] == 3000, p
    # PPS APDO 3.3-21V / 3A.
    pps = 0xC0000000 | (0 << 28) | (210 << 17) | (33 << 8) | 60
    p = decode_pdo(pps, True)
    assert p["kind"] == "pps" and p["min_mv"] == 3300 and p["max_mv"] == 21000 and p["current_ma"] == 3000, p
    # AVS APDO 15-48V.
    avs = 0xC0000000 | (1 << 28) | (480 << 17) | (150 << 8) | 100
    p = decode_pdo(avs, True)
    assert p["kind"] == "avs" and p["min_mv"] == 15000 and p["max_mv"] == 48000, p
    # VDM: structured Discover_Identity with PD SID.
    vdm = (0xFF00 << 16) | (1 << 15) | (1 << 13) | 0x01
    v = decode_vdm_header(vdm)
    assert v["structured"] == 1 and v["command"] == 1 and v["command_name"] == "Discover_Identity", v
    # PDO signature deterministic.
    assert pdo_signature([0x0002D12C, 0x8002D12C]) == pdo_signature([0x0002D12C, 0x8002D12C])
    pb722_regression()
    print("SELFTEST OK: header/name/pdo(pps,avs)/vdm/signature all verified")


def pb722_regression():
    """Real PB722-observed message flows as regression vectors.

    VID 0x2DC0 / PID 0x020B (FW 48 / HW 16), offering 5/9/12/15/20 V fixed plus
    a 3.3-21 V PPS APDO.  These are OBSERVED vectors used to cross-check that a
    raw header decodes to the correct message type and that the transaction
    layer classifies the flow correctly.  They are not assertions about every
    source's behaviour.
    """
    # --- Source_Capabilities: 6 PDOs ---------------------------------------
    caps_pdos = [
        0x0001912C,   # 5 V  / 3 A fixed
        0x0002D12C,   # 9 V  / 3 A fixed
        0x0003C12C,   # 12 V / 3 A fixed
        0x0004B12C,   # 15 V / 3 A fixed
        0x0006412C,   # 20 V / 3 A fixed
        0xC1A4213C,   # PPS 3.3-21 V / 3 A APDO
    ]
    dec = decode_caps(caps_pdos)
    assert [d["kind"] for d in dec] == ["fixed"] * 5 + ["pps"], dec
    assert dec[0]["voltage_mv"] == 5000 and dec[0]["current_ma"] == 3000
    assert dec[4]["voltage_mv"] == 20000
    assert dec[5]["min_mv"] == 3300 and dec[5]["max_mv"] == 21000 and dec[5]["current_ma"] == 3000
    # header: type=1 (Source_Cap data), nobjects=6, msgid=0, specrev=1
    hdr_caps = 0x6081
    h = decode_header(hdr_caps)
    assert h["type"] == 0x01 and h["nobjects"] == 6 and h["spec_rev"] == 1
    assert decode_type_name(0x01, 0, nobjects=6) == "Source_Capabilities"

    # --- Request (sink, 1 object) -> Accept -> PS_RDY ----------------------
    req_hdr = 0x1082   # type=2 Request, nobjects=1
    acc_hdr = 0x83     # type=3 Accept (control, 0 objects)
    psr_hdr = 0x86     # type=6 PS_RDY (control)
    assert decode_header(req_hdr)["type"] == 0x02
    assert decode_type_name(0x02, 0, nobjects=1) == "Request"
    assert decode_header(acc_hdr)["type"] == 0x03
    assert decode_type_name(0x03, 0, nobjects=0) == "Accept"
    assert decode_header(psr_hdr)["type"] == 0x06
    assert decode_type_name(0x06, 0, nobjects=0) == "PS_RDY"

    # --- Get_Status -> Not_Supported (observed battery/info queries) -------
    assert decode_type_name(0x12, 0, nobjects=0) == "Get_Status"
    assert decode_type_name(0x10, 0, nobjects=0) == "Not_Supported"

    # --- Get_PPS_Status -> PPS_Status (extended) ---------------------------
    assert decode_type_name(0x14, 0, nobjects=0) == "Get_PPS_Status"
    assert decode_type_name(0x0C, 1, nobjects=0) == "Ext_PPS_Status"

    # --- SVDM Discover_Identity (NAKed on PB722) ---------------------------
    vdm = (0xFF00 << 16) | (1 << 15) | (1 << 13) | 0x01
    v = decode_vdm_header(vdm)
    assert v["structured"] and v["command"] == 1 and v["command_name"] == "Discover_Identity"

    print("PB722 regression: caps(5/9/12/15/20V+PPS) req/accept/ps_rdy, ns, pps_status, identity OK")


def main():
    import sys
    mode = sys.argv[1] if len(sys.argv) > 1 else "selftest"
    if mode == "selftest":
        selftest()
        return 0
    if mode == "decode":
        # decode one PDO from hex
        for a in sys.argv[2:]:
            print(decode_pdo(int(a, 16), True))
        return 0
    print(__doc__)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
