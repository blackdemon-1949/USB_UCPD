#!/usr/bin/env python3
"""
build_knowledge.py — Generate the APIE embedded knowledge package.

Produces, from one source of truth, three artefacts that share a single schema
version and a single CRC-32:

  research/usb_pd_knowledge.json   human/machine-readable knowledge document
  research/pd_knowledge.bin        compact firmware-importable blob
  research/pd_knowledge.h          C header embedding the blob (import-ready)

The field positions and message-type numbers are taken from the authoritative
normative USB-PD layout already implemented and host-tested in the firmware
decoder (Appli/Core/Src/apie_decode.c).  This tool does not invent protocol
behaviour; it only packages what the firmware actually decodes plus the
documented safety / capability policies and known vendor observations.

The blob is deterministic (no timestamps or random data), so building the
firmware knowledge is reproducible and CRC-stable.

Usage:
    python3 tools/build_knowledge.py [--out-dir research]
"""
import argparse
import json
import os
import struct
import sys

# --------------------------------------------------------------------------
# Schema version.  Bump when the layout of the blob or the meaning of any
# packaged field changes.  The firmware should refuse to import a blob whose
# schema version differs.
# --------------------------------------------------------------------------
SCHEMA_VERSION = 1

BLOB_MAGIC = b"KPD1"          # 4 bytes
CRC_HEADER = b"PKCD"          # trailing CRC marker

# --------------------------------------------------------------------------
# Message tables — must match apie_decode.c exactly.
# --------------------------------------------------------------------------
CONTROL = [
    (0x01, "GoodCRC"), (0x02, "GotoMin"), (0x03, "Accept"), (0x04, "Reject"),
    (0x05, "Ping"), (0x06, "PS_RDY"), (0x07, "Get_Source_Cap"),
    (0x08, "Get_Sink_Cap"), (0x09, "DR_Swap"), (0x0A, "PR_Swap"),
    (0x0B, "VCONN_Swap"), (0x0C, "Wait"), (0x0D, "Soft_Reset"),
    (0x0E, "Data_Reset"), (0x0F, "Data_Reset_Complete"),
    (0x10, "Not_Supported"), (0x11, "Get_Source_Cap_Ext"),
    (0x12, "Get_Status"), (0x13, "FR_Swap"), (0x14, "Get_PPS_Status"),
    (0x15, "Get_Country_Codes"), (0x16, "Get_Sink_Cap_Ext"),
    (0x17, "Get_Source_Info"), (0x18, "Get_Revision"),
]

DATA = [
    (0x01, "Source_Capabilities"), (0x02, "Request"), (0x03, "BIST"),
    (0x04, "Sink_Capabilities"), (0x05, "Battery_Status"), (0x06, "Alert"),
    (0x07, "Get_Country_Info"), (0x08, "Enter_USB"), (0x09, "EPR_Request"),
    (0x0A, "EPR_Mode"), (0x0B, "Source_Info"), (0x0C, "Revision"),
    (0x0F, "Vendor_Defined"),
]

EXTENDED = [
    (0x01, "Ext_Source_Capabilities"), (0x02, "Ext_Status"),
    (0x03, "Ext_Get_Battery_Cap"), (0x04, "Ext_Get_Battery_Status"),
    (0x05, "Ext_Battery_Capabilities"), (0x06, "Ext_Get_Manufacturer_Info"),
    (0x07, "Ext_Manufacturer_Info"), (0x08, "Ext_Security_Request"),
    (0x09, "Ext_Security_Response"), (0x0A, "Ext_FW_Update_Request"),
    (0x0B, "Ext_FW_Update_Response"), (0x0C, "Ext_PPS_Status"),
    (0x0D, "Ext_Country_Info"), (0x0E, "Ext_Country_Codes"),
    (0x0F, "Ext_Sink_Capabilities"), (0x10, "Ext_Control"),
    (0x11, "Ext_EPR_Source_Capa"), (0x12, "Ext_EPR_Sink_Capa"),
    (0x1E, "Ext_VDM"),
]

SVDM = [
    (0x01, "Discover_Identity"), (0x02, "Discover_SVIDs"),
    (0x03, "Discover_Modes"), (0x04, "Enter_Mode"), (0x05, "Exit_Mode"),
    (0x06, "Attention"), (0x10, "Specific"),
]

SOP = {
    0: "SOP", 1: "SOP'", 2: "SOP''", 3: "SOP' dbg", 4: "SOP'' dbg",
    5: "HARD_RESET", 6: "CABLE_RESET", 7: "BIST_MODE2",
}

# PDO / APDO field metadata (normative bit positions).
PDO_META = {
    "type_bits": [30, 31],
    "fixed": {
        "voltage_mv": {"bits": [10, 19], "step": 50, "unit": "mV"},
        "max_current_ma": {"bits": [0, 9], "step": 10, "unit": "mA"},
    },
    "battery": {
        "min_mv": {"bits": [10, 19], "step": 50},
        "max_mv": {"bits": [20, 29], "step": 50},
        "max_power_mw": {"bits": [0, 9], "step": 250, "unit": "mW"},
    },
    "variable": {
        "min_mv": {"bits": [10, 19], "step": 50},
        "max_mv": {"bits": [20, 29], "step": 50},
        "max_current_ma": {"bits": [0, 9], "step": 10, "unit": "mA"},
    },
    "apdo": {
        "subtype_bits": [28, 29],
        "pps": {
            "min_mv": {"bits": [8, 15], "step": 100},
            "max_mv": {"bits": [17, 24], "step": 100},
            "max_current_ma": {"bits": [0, 6], "step": 50, "unit": "mA"},
        },
        "avs": {
            "min_mv": {"bits": [8, 15], "step": 100},
            "max_mv": {"bits": [17, 25], "step": 100},
            "pdp_w": {"bits": [0, 7], "unit": "W"},
        },
    },
}

# Safety / capability policies — must match apie.h.
SAFETY = {
    "max_voltage_mv": 21000,
    "max_current_ma": 5000,
    "pps_step_mv": 100,
    "query_cooldown_ms": 500,
    "query_timeout_ms": 1200,
    "query_max_pending": 2,
}

HW_CAPS = {
    "epr_power_enabled": 0,      # EPR (>20 V) never energised on this board
    "has_vbus_adc": 0,           # CC-only rig, synthetic VBUS
    "has_dplus_dminus": 0,       # D+/D- not wired to this connector
    "cable_emark_observable": 1, # SOP'/SOP'' observable over UCPD
    "pd_revision": 3,            # PD 3.x SPR negotiated
}

QUERIES = [
    "getstatus", "getpps", "identify", "svids", "modes", "srcext",
    "manuinfo", "battery", "country",
]

EXPERIMENTS = {
    "R0": {"name": "observation", "default": True},
    "R1": {"name": "standard informational query", "default": True},
    "R2": {"name": "standard power request within validated limits", "default": True},
    "R3": {"name": "state-changing experiment", "default": False},
    "R4": {"name": "unknown/vendor transmission", "default": False},
}

# Model metadata (matches tools/apie_model_seed.json).
MODEL = {
    "model_id": 1,
    "version": 1,
    "feature_version": 12,
    "kind": 1,                       # APIE_MODEL_NAIVE_BAYES
    "kind_name": "naive_bayes",
    "accuracy": 1.0,
    "trained": "seed-online (host pipeline; firmware re-learns online)",
    "n_samples": 64,
    "n_valid": 16,
    "useful_rate": 0.5312,
}

# Known vendor observations — OBSERVATION ONLY, never universal rules.
OBSERVATIONS = [
    {
        "label": "PB722 (observation only)",
        "vid": 0x2DC0, "pid": 0x020B, "fw": 48, "hw": 16,
        "pdo_volts": [5000, 9000, 12000, 15000, 20000],
        "pps": "3.3-21V",
        "battery": "Not_Supported",
        "identity": "NAK",
        "src_svid": "none observed",
        "cable_sop_svid": 0xFF00,   # observed on SOP' cable traffic, NOT source
        "note": "Cable SOP' SVID 0xFF00 must not be attributed to the source.",
    },
]

MESSAGE_CLASSES = [
    (0, "UNKNOWN"), (1, "CONTROL"), (2, "DATA"), (3, "EXTENDED"),
    (4, "VDM_SVDM"), (5, "VDM_UVDM"), (6, "EPR"),
]

# --------------------------------------------------------------------------
# Charging-protocol transport table (section I).
#
# Correctly records the physical/electrical TRANSPORT of each charging family.
# The current board's PD connector does NOT expose D+/D-, so only USB-PD /
# USB-C CC signalling is observable through UCPD.  Non-PD protocols are listed
# for completeness of the research knowledge, never claimed as supported.
# --------------------------------------------------------------------------
TRANSPORT = [
    {"name": "USB-PD", "family": "standard", "transport": "USB-C CC", "observable": True},
    {"name": "USB-C default/Rp current", "family": "standard", "transport": "USB-C CC (Rp)", "observable": True},
    {"name": "USB-PD PPS", "family": "standard", "transport": "USB-C CC", "observable": True},
    {"name": "USB-PD EPR/AVS", "family": "standard", "transport": "USB-C CC (protocol), >20V electrical", "observable": True},
    {"name": "USB-PD VDM/SVDM", "family": "standard", "transport": "USB-C CC", "observable": True},
    {"name": "BC1.2", "family": "proprietary", "transport": "D+/D-", "observable": False},
    {"name": "QC (Quick Charge)", "family": "proprietary", "transport": "D+/D-", "observable": False},
    {"name": "AFC (Adaptive Fast Charging)", "family": "proprietary", "transport": "D+/D- (Samsung)", "observable": False},
    {"name": "Huawei SuperCharge (SCP/FCP)", "family": "proprietary", "transport": "D+/D-", "observable": False},
    {"name": "VOOC/SUPERVOOC (OPPO/OnePlus/Realme)", "family": "proprietary", "transport": "D+/D- + VBUS signalling", "observable": False},
    {"name": "Vivo FlashCharge", "family": "proprietary", "transport": "proprietary D+/D-", "observable": False},
    {"name": "Xiaomi Mi/Redmi charging", "family": "proprietary", "transport": "USB-PD + proprietary D+/D-", "observable": "partial"},
    {"name": "USB-PD + vendor UVDM", "family": "vendor", "transport": "USB-C CC (UVDM)", "observable": True},
]

# --------------------------------------------------------------------------
# Compact packet schemas (section J): header field masks/positions, payload
# layout, expected response, SOP.  These are the normative layouts the decoder
# already implements; the package records them as machine-readable metadata.
# --------------------------------------------------------------------------
PACKET_SCHEMAS = [
    {"message": "Source_Capabilities", "protocol": "USB-PD", "sop": "SOP",
     "type": 0x01, "kind": "data", "nobjects": "1..7",
     "header_bits": {"type": [0, 4], "port_data_role": [6, 6], "spec_rev": [7, 7],
                     "port_power_role": [9, 9], "msgid": [10, 11], "nobjects": [12, 14], "extended": [15, 15]},
     "payload": "PDO[0..n-1] (Fixed/Variable/Battery/PPS/AVS/EPR)", "response": "Request->Accept->PS_RDY"},
    {"message": "Request", "protocol": "USB-PD", "sop": "SOP", "type": 0x02, "kind": "data",
     "nobjects": 1, "payload": "RDO", "response": "Accept | Reject | Wait | Not_Supported"},
    {"message": "Accept", "protocol": "USB-PD", "sop": "SOP", "type": 0x03, "kind": "control", "nobjects": 0, "payload": "none", "response": "PS_RDY"},
    {"message": "Reject", "protocol": "USB-PD", "sop": "SOP", "type": 0x04, "kind": "control", "nobjects": 0, "payload": "none", "response": "None"},
    {"message": "Wait", "protocol": "USB-PD", "sop": "SOP", "type": 0x0C, "kind": "control", "nobjects": 0, "payload": "none", "response": "None"},
    {"message": "PS_RDY", "protocol": "USB-PD", "sop": "SOP", "type": 0x06, "kind": "control", "nobjects": 0, "payload": "none", "response": "None"},
    {"message": "GoodCRC", "protocol": "USB-PD", "sop": "SOP/SOP'/SOP''", "type": 0x01, "kind": "control", "nobjects": 0, "payload": "RxMessageID", "response": "None (link-level)"},
    {"message": "Get_Source_Cap", "protocol": "USB-PD", "sop": "SOP", "type": 0x07, "kind": "control", "nobjects": 0, "payload": "none", "response": "Source_Capabilities"},
    {"message": "Get_Status", "protocol": "USB-PD", "sop": "SOP", "type": 0x12, "kind": "control", "nobjects": 0, "payload": "none", "response": "Ext_Status"},
    {"message": "Get_PPS_Status", "protocol": "USB-PD", "sop": "SOP", "type": 0x14, "kind": "control", "nobjects": 0, "payload": "none", "response": "Ext_PPS_Status"},
    {"message": "PPS_Status", "protocol": "USB-PD", "sop": "SOP", "type": 0x0C, "kind": "extended", "nobjects": 0, "payload": "extended header + PPS status", "response": "None"},
    {"message": "Vendor_Defined", "protocol": "USB-PD", "sop": "SOP/SOP'/SOP''", "type": 0x0F, "kind": "data",
     "nobjects": "1..7", "payload": "VDM header + VDOs", "response": "VDM ACK/NAK/BUSY"},
    {"message": "Discover_Identity", "protocol": "USB-PD VDM", "sop": "SOP/SOP'/SOP''", "type": 0x0F,
     "kind": "vdm", "payload": "VDM hdr cmd=0x01", "response": "Identity VDOs or NAK"},
    {"message": "Discover_SVIDs", "protocol": "USB-PD VDM", "sop": "SOP/SOP'/SOP''", "type": 0x0F,
     "kind": "vdm", "payload": "VDM hdr cmd=0x02", "response": "SVID list or NAK"},
    {"message": "Discover_Modes", "protocol": "USB-PD VDM", "sop": "SOP/SOP'/SOP''", "type": 0x0F,
     "kind": "vdm", "payload": "VDM hdr cmd=0x03", "response": "Mode VDOs or NAK"},
    {"message": "Soft_Reset", "protocol": "USB-PD", "sop": "SOP", "type": 0x0D, "kind": "control", "nobjects": 0, "payload": "none", "response": "Accept->PS_RDY"},
    {"message": "Hard_Reset", "protocol": "USB-PD", "sop": "SOP", "type": None, "kind": "ordered_set", "nobjects": 0, "payload": "ordered set", "response": "Source_Capabilities"},
]

# Scalar fixed-point note for transport/entropy (kept for the package reader).
SCALE_NOTES = {
    "log2_table_scale": 4096,
    "max_nibble_entropy_bits": 4,
    "unknown_signature_freq_x1000": "packets/sec * 1000",
    "confidence_range": [0, 100],
}


# --------------------------------------------------------------------------
# CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor) — matches
# APIE_Crc32() in the firmware.
# --------------------------------------------------------------------------
def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc ^ 0xFFFFFFFF


def build_blob(doc: dict) -> bytes:
    """Deterministic compact binary knowledge blob."""
    sections = []

    def add(tag: str, payload: bytes):
        sections.append((tag, tag.encode("ascii") + struct.pack("<I", len(payload)) + payload))

    def pairs(items):
        return struct.pack("<I", len(items)) + b"".join(
            struct.pack("<H", t) + n.encode("ascii") + b"\x00" for t, n in items)

    add("MSGS", pairs(CONTROL))          # tag "MSGS"
    add("MSGD", pairs(DATA))
    add("MSGE", pairs(EXTENDED))
    add("SVDM", pairs(SVDM))
    # SOP names
    sop_payload = struct.pack("<I", len(SOP)) + b"".join(
        struct.pack("<B", k) + v.encode("ascii") + b"\x00" for k, v in sorted(SOP.items()))
    add("SOPN", sop_payload)
    # PDO metadata as JSON bytes (kept compact; used by host tools, referenced
    # by firmware importers that only need the message tables).
    add("PDOM", json.dumps(PDO_META, separators=(",", ":")).encode("ascii"))
    # Safety + hardware caps
    safety_payload = struct.pack("<I", SCHEMA_VERSION) + json.dumps(
        {"safety": SAFETY, "hw": HW_CAPS}, separators=(",", ":")).encode("ascii")
    add("CAPS", safety_payload)
    # Query catalog
    add("QURY", struct.pack("<I", len(QUERIES)) + b"".join(
        q.encode("ascii") + b"\x00" for q in QUERIES))
    # Model metadata
    add("MODL", json.dumps(MODEL, separators=(",", ":")).encode("ascii"))
    # Observations
    add("OBSV", json.dumps(OBSERVATIONS, separators=(",", ":")).encode("ascii"))
    # Charging-transport table
    add("TRAN", json.dumps(TRANSPORT, separators=(",", ":")).encode("ascii"))
    # Packet schemas
    add("PKSC", json.dumps(PACKET_SCHEMAS, separators=(",", ":")).encode("ascii"))
    # Scale/units notes
    add("SCAL", json.dumps(SCALE_NOTES, separators=(",", ":")).encode("ascii"))

    body = BLOB_MAGIC + struct.pack("<H", SCHEMA_VERSION) + struct.pack("<H", len(sections))
    for _tag, payload in sections:
        body += payload
    body += CRC_HEADER
    body += struct.pack("<I", crc32(body))
    return body


def build_doc() -> dict:
    """Human/machine-readable knowledge document."""
    return {
        "schema_version": SCHEMA_VERSION,
        "description": "APIE embedded USB-PD knowledge package for STM32H7R3Z8J6 UCPD sink.",
        "generator": "tools/build_knowledge.py",
        "generated": None,  # filled in by caller (excluded from the binary blob)
        "message_classes": [{"id": c, "name": n} for c, n in MESSAGE_CLASSES],
        "control_messages": [{"type": t, "name": n} for t, n in CONTROL],
        "data_messages": [{"type": t, "name": n} for t, n in DATA],
        "extended_messages": [{"type": t, "name": n} for t, n in EXTENDED],
        "svdm_commands": [{"cmd": t, "name": n} for t, n in SVDM],
        "sop": [{"code": k, "name": v} for k, v in sorted(SOP.items())],
        "pdo_metadata": PDO_META,
        "safety": SAFETY,
        "hardware_capabilities": HW_CAPS,
        "queries": QUERIES,
        "experiment_levels": [
            {"level": k, "name": v["name"], "default": v["default"]}
            for k, v in EXPERIMENTS.items()
        ],
        "model": MODEL,
        "vendor_observations": OBSERVATIONS,
        "charging_transport": TRANSPORT,
        "packet_schemas": PACKET_SCHEMAS,
        "scale_notes": SCALE_NOTES,
    }


def c_array_bytes(name: str, data: bytes) -> str:
    out = [f"static const uint8_t {name}[{len(data)}u] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("  " + ",".join(f"0x{b:02X}" for b in chunk) + ",")
    out.append("};")
    out.append(f"#define {name.upper()}_LEN {len(data)}u")
    out.append(f"#define {name.upper()}_CRC 0x{crc32(data):08X}u")
    return "\n".join(out)


def verify_blob(path: str) -> int:
    """Parse a pd_knowledge.bin and validate magic / CRC / section framing."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == BLOB_MAGIC, f"bad magic {data[:4]!r}"
    body = data[:-4]
    assert body[-4:] == CRC_HEADER, "missing CRC header"
    crc = struct.unpack("<I", data[-4:])[0]
    assert crc32(body) == crc, "CRC mismatch"
    sections = body[:-4]
    ver = struct.unpack("<H", sections[4:6])[0]
    nsec = struct.unpack("<H", sections[6:8])[0]
    off = 8
    tags = []
    for _ in range(nsec):
        tag = sections[off:off + 4].decode("ascii")
        ln = struct.unpack("<I", sections[off + 4:off + 8])[0]
        off += 8
        assert off + ln <= len(sections), "section overruns blob"
        tags.append(tag)
        off += ln
    assert off == len(sections), "trailing bytes after sections"
    print(f"verify: magic ok, schema v{ver}, {nsec} sections, crc 0x{crc:08X}")
    print(f"  sections: {', '.join(tags)}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Build APIE knowledge package")
    ap.add_argument("--out-dir", default="research", help="output directory")
    ap.add_argument("--verify", metavar="BIN", help="parse & validate an existing blob")
    args = ap.parse_args()

    if args.verify:
        return verify_blob(args.verify)
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    import datetime
    doc = build_doc()
    doc["generated"] = datetime.datetime.now(datetime.timezone.utc).isoformat()

    json_path = os.path.join(out_dir, "usb_pd_knowledge.json")
    with open(json_path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")

    blob = build_blob(doc)
    bin_path = os.path.join(out_dir, "pd_knowledge.bin")
    with open(bin_path, "wb") as f:
        f.write(blob)

    hdr_path = os.path.join(out_dir, "pd_knowledge.h")
    with open(hdr_path, "w") as f:
        f.write("/* Auto-generated by tools/build_knowledge.py — do not edit. */\n")
        f.write("#ifndef PD_KNOWLEDGE_H\n#define PD_KNOWLEDGE_H\n\n")
        f.write(f"#define PD_KNOWLEDGE_SCHEMA_VERSION {SCHEMA_VERSION}u\n\n")
        f.write(c_array_bytes("pd_knowledge_blob", blob))
        f.write("\n\n#endif /* PD_KNOWLEDGE_H */\n")

    # Sanity: re-read the blob and verify CRC.
    with open(bin_path, "rb") as f:
        data = f.read()
    assert data == blob, "blob changed between write and read-back"
    assert crc32(blob[:-4]) == struct.unpack("<I", blob[-4:])[0], "CRC self-check failed"

    print(f"schema_version   = {SCHEMA_VERSION}")
    print(f"blob size        = {len(blob)} bytes (stored crc 0x{crc32(blob[:-4]):08X})")
    print(f"json             = {json_path}")
    print(f"bin              = {bin_path}")
    print(f"h                = {hdr_path}")
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
