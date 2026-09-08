#!/usr/bin/env python3
"""
apie_replay.py - host-side capture replay / offline analysis.

Reads the machine-readable capture export produced by the firmware CLI
(`raw export`, which emits one `apie_cap ...` line per captured packet) and
re-runs the same deterministic decode + feature extraction the firmware uses,
so a capture taken on the bench can be analysed and replayed on a host without
the board attached.

Usage:
    python3 tools/apie_replay.py < capture.txt
    python3 tools/apie_replay.py capture.txt          # analyse a file
"""

from __future__ import annotations

import re
import sys

from apie_decode import (decode_header, decode_type_name, decode_pdo,
                         pdo_signature, decode_vdm_header)

CAP_RE = re.compile(
    r"^apie_cap\s+ts=(\d+)\s+dir=(\w+)\s+sop=(\d+)\s+msgid=(\d+)\s+"
    r"type=(\d+)\s+ext=(\d+)\s+n=(\d+)\s+hdr=0x([0-9A-Fa-f]+)\s+hex=(.*)$"
)


def parse_capture(lines):
    packets = []
    for ln in lines:
        ln = ln.strip()
        m = CAP_RE.match(ln)
        if not m:
            continue
        ts, dir_, sop, msgid, type_, ext, n, hdr, hexs = m.groups()
        payload = bytes.fromhex(hexs) if hexs else b""
        p = {
            "ts_ms": int(ts),
            "dir": dir_,
            "sop": int(sop),
            "msgid": int(msgid),
            "type": int(type_),
            "ext": int(ext),
            "n": int(n),
            "hdr": int(hdr, 16),
            "hex": payload,
        }
        p["header"] = decode_header(p["hdr"])
        p["name"] = decode_type_name(p["type"], p["ext"], p["n"])

        # Decode the 32-bit data objects (PDO / APDO / VDM) from the payload
        # little-endian words.  Keep the raw words so we can fingerprint them.
        raws, objs = [], []
        if len(payload) >= 2:
            n_objs = min(p["n"], (len(payload) - 2) // 4)
            for i in range(n_objs):
                off = 2 + i * 4
                v = int.from_bytes(payload[off:off + 4], "little")
                raws.append(v)
                objs.append(decode_pdo(v, True))
        p["raw_pdo"] = raws
        p["pdo"] = objs

        # If the first object looks like a VDM header, decode it.
        if raws:
            hdr_obj = raws[0]
            if (hdr_obj >> 15) & 1:
                p["vdm"] = decode_struct_vdm(hdr_obj)
        packets.append(p)
    return packets


def decode_struct_vdm(v):
    return {
        "svid": (v >> 16) & 0xFFFF,
        "structured": (v >> 15) & 0x1,
        "version": (v >> 13) & 0x3,
        "command_type": (v >> 6) & 0x3,
        "command": v & 0x1F,
        "name": {1: "Discover_Identity", 2: "Discover_SVIDs", 3: "Discover_Modes",
                 4: "Enter_Mode", 5: "Exit_Mode", 6: "Attention"}.get(v & 0x1F, None),
    }


# --------------------------------------------------------------------------
# Knowledge-backed packet-schema validation (section J) + mutation fuzzing
# (section H).  These run entirely on the host; they never transmit PD.
# --------------------------------------------------------------------------
# Minimal schema knowledge (mirrors research/pd_knowledge.json packet_schemas).
SCHEMA = {
    0x01: {"n": "1..7", "kind": "data"},       # Source_Capabilities (data)
    0x02: {"n": 1, "kind": "data"},            # Request
    0x03: {"n": 0, "kind": "control"},         # Accept
    0x04: {"n": 0, "kind": "control"},         # Reject
    0x06: {"n": 0, "kind": "control"},         # PS_RDY
    0x07: {"n": 0, "kind": "control"},         # Get_Source_Cap
    0x0C: {"n": 0, "kind": "control"},         # Wait
    0x0D: {"n": 0, "kind": "control"},         # Soft_Reset
    0x10: {"n": 0, "kind": "control"},         # Not_Supported
    0x12: {"n": 0, "kind": "control"},         # Get_Status
    0x14: {"n": 0, "kind": "control"},         # Get_PPS_Status
}


def schema_violation(p):
    """Return a description if the packet violates its schema, else None."""
    s = SCHEMA.get(p["type"])
    if s is None:
        return "unknown message type"
    if isinstance(s["n"], str):
        lo, hi = 1, 7
    else:
        lo = hi = s["n"]
    if not (lo <= p["n"] <= hi):
        return f"nobjects={p['n']} outside {lo}..{hi} for a {s['kind']} message"
    return None


def mutate_packets(packets, n_mutations=32, seed=0x5EED):
    """Bit-flip mutation fuzzing of payload bytes.  Verifies the host decoder
    stays robust (never raises) on every mutated packet."""
    import random
    rng = random.Random(seed)
    crashes = 0
    violated = 0
    checked = 0
    for p in packets:
        payload = bytearray(p["hex"])
        for _ in range(n_mutations):
            if not payload:
                continue
            m = bytearray(payload)
            bit = rng.randrange(len(m) * 8)
            m[bit // 8] ^= (1 << (bit % 8))
            try:
                probe = dict(p)
                probe["hex"] = bytes(m)
                if len(m) >= 2:
                    nh = int.from_bytes(m[:2], "little")
                    probe["header"] = decode_header(nh)
                    probe["type"] = probe["header"]["type"]
                    probe["n"] = probe["header"]["nobjects"]
                # decode PDOs
                n_objs = min(probe["n"], (len(m) - 2) // 4) if len(m) >= 2 else 0
                for i in range(max(0, n_objs)):
                    off = 2 + i * 4
                    decode_pdo(int.from_bytes(m[off:off + 4], "little"), True)
                if schema_violation(probe):
                    violated += 1
            except Exception as e:  # robustness: decoder must not raise
                crashes += 1
                print(f"  DECODER CRASH on mutation: {e}")
            checked += 1
    print(f"mutation: {checked} mutated packets decoded, {violated} schema violations, "
          f"{crashes} decoder crashes")
    return crashes == 0


def synthetic_session(seed=0xC0DE):
    """Generate a synthetic PD transaction session as packets (never transmitted).
    Source_Capabilities -> Request -> Accept -> PS_RDY, plus Get_Status ->
    Not_Supported and Get_PPS_Status -> Ext_PPS_Status."""
    import random
    rng = random.Random(seed)
    t = 1000
    out = []
    caps = [0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006412C, 0xC1A4213C]

    def add(t, dir_, sop, msgid, type_, ext, n, hdr, payload):
        out.append({"ts_ms": t, "dir": dir_, "sop": sop, "msgid": msgid, "type": type_,
                    "ext": ext, "n": n, "hdr": hdr, "hex": payload, "name": "",
                    "header": {}, "raw_pdo": [], "pdo": []})

    hdr = 0x6081  # Source_Cap 6 objects
    body = bytearray(hdr.to_bytes(2, "little"))
    for p in caps:
        body += p.to_bytes(4, "little")
    add(t, "RX", 0, 0, 0x01, 0, 6, hdr, bytes(body)); t += 200

    rdo = (1 << 31) | (0 << 28) | (5000 // 50 << 10) | (3000 // 10)
    hreq = 0x1082
    add(t, "TX", 0, 0, 0x02, 0, 1, hreq, hreq.to_bytes(2, "little") + rdo.to_bytes(4, "little")); t += 50
    add(t, "RX", 0, 0, 0x03, 0, 0, 0x0083, b"\x83\x00"); t += 100
    add(t, "RX", 0, 0, 0x06, 0, 0, 0x0086, b"\x86\x00"); t += 400

    add(t, "TX", 0, 1, 0x12, 0, 0, 0x0892, b"\x92\x08"); t += 120
    add(t, "RX", 0, 1, 0x10, 0, 0, 0x0490, b"\x90\x04"); t += 300

    add(t, "TX", 0, 2, 0x14, 0, 0, 0x0A94, b"\x94\x0A"); t += 120
    add(t, "RX", 0, 2, 0x0C, 1, 0, 0x0C8E, b"\x8E\x0C\x00\x00"); t += 200
    return out


def fuzz_decode(packets, n=40, seed=0xABCD):
    """Malformed-packet decoder fuzzing (bad length, bad NDO, garbage, partial)."""
    import random
    rng = random.Random(seed)
    crashes = 0
    for p in packets:
        base = bytearray(p["hex"]) if p["hex"] else bytearray(2)
        cases = []
        if base:
            cases.append(base[:1])            # truncated header
            cases.append(base + bytes(rng.randrange(256) for _ in range(rng.randrange(1, 4))))  # bad length
        cases.append(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 16))))  # garbage
        for c in cases:
            try:
                if len(c) >= 2:
                    decode_header(int.from_bytes(c[:2], "little"))
                else:
                    decode_header(int.from_bytes(c + b"\x00" * (2 - len(c)), "little"))
                # try to decode as many PDOs as the (possibly bogus) count allows
                nh = int.from_bytes((c + b"\x00" * 4)[:2], "little")
                hh = decode_header(nh)
                n_objs = min(hh["nobjects"], (len(c) - 2) // 4) if len(c) >= 2 else 0
                for i in range(max(0, n_objs)):
                    decode_pdo(int.from_bytes((c + b"\x00" * 4)[2 + i * 4:6 + i * 4], "little"), True)
            except Exception as e:
                crashes += 1
                print(f"  DECODER CRASH on malformed input: {e}")
    print(f"fuzz: {len(packets) * len([1])} source packets, {crashes} decoder crashes on malformed input")
    return crashes == 0


def analyze(packets):
    n_rx = sum(1 for p in packets if p["dir"] == "RX")
    n_tx = sum(1 for p in packets if p["dir"] == "TX")
    unique = {}
    for p in packets:
        unique[p["name"]] = unique.get(p["name"], 0) + 1

    print(f"replay: {len(packets)} packet(s)  RX={n_rx}  TX={n_tx}")
    if packets:
        print(f"  span={packets[-1]['ts_ms'] - packets[0]['ts_ms']} ms")
    print("  message-type histogram:")
    for name, c in sorted(unique.items(), key=lambda kv: -kv[1]):
        print(f"    {name:30s} {c}")

    # First Source_Capabilities: fingerprint the offered profile.
    for p in packets:
        if p["name"] == "Source_Capabilities" and p["raw_pdo"]:
            print("\n  source-capabilities:")
            print(f"    {len(p['raw_pdo'])} PDO(s), sig=0x{pdo_signature(p['raw_pdo']):08X}")
            for j, d in enumerate(p["pdo"]):
                print(f"    PDO{j+1}: {d}")
            break

    # Show the VDM/SVDM flow.
    for p in packets:
        if p.get("vdm"):
            print(f"\n  VDM @ t={p['ts_ms']}ms {p['dir']}: "
                  f"SVID=0x{p['vdm']['svid']:04X} {p['vdm']['name'] or 'cmd'+str(p['vdm']['command'])}")

    # Cross-correlate Get_Status -> Status latency.
    req_t = None
    for p in packets:
        if p["type"] == 0x12 and p["dir"] == "TX":  # Get_Status (control)
            req_t = p["ts_ms"]
        elif p["type"] in (0x02, 0x0C) and p["dir"] == "RX" and p["ext"]:  # Status ext / PPS
            if req_t is not None:
                print(f"\n  Status reply latency: {p['ts_ms'] - req_t} ms")
                req_t = None

    # Schema validation across the session.
    violations = [schema_violation(p) for p in packets]
    bad = [v for v in violations if v]
    print(f"\n  schema validation: {len(packets) - len(bad)}/{len(packets)} conform to packet schemas")
    for p, v in zip(packets, violations):
        if v:
            print(f"    violation @t={p['ts_ms']}ms type=0x{p['type']:02X}: {v}")


def main():
    args = sys.argv[1:]
    mode = "analyze"
    n_mut = 32
    rest = []
    for a in args:
        if a.startswith("--mode="):
            mode = a.split("=", 1)[1]
        elif a.startswith("--mutations="):
            n_mut = int(a.split("=", 1)[1])
        elif a in ("-", "--"):
            rest.append(a)
        else:
            rest.append(a)

    if mode == "synthetic":
        packets = synthetic_session()
        # fill in decoded fields
        for p in packets:
            p["header"] = decode_header(p["hdr"])
            p["name"] = decode_type_name(p["type"], p["ext"], p["n"])
            raws = []
            if len(p["hex"]) >= 2:
                n_objs = min(p["n"], (len(p["hex"]) - 2) // 4)
                for i in range(n_objs):
                    off = 2 + i * 4
                    raws.append(int.from_bytes(p["hex"][off:off + 4], "little"))
            p["raw_pdo"] = raws
            p["pdo"] = [decode_pdo(v, True) for v in raws]
        print("synthetic session generated (never transmitted):")
        analyze(packets)
        return 0

    if mode in ("mutate", "fuzz"):
        if rest and rest[0] not in ("-", "--"):
            with open(rest[0]) as f:
                lines = f.read().splitlines()
        else:
            lines = sys.stdin.read().splitlines()
        packets = parse_capture(lines)
        ok = mutate_packets(packets, n_mut) if mode == "mutate" else fuzz_decode(packets)
        return 0 if ok else 1

    # default: analyze a capture
    if rest and rest[0] not in ("-", "--"):
        with open(rest[0]) as f:
            lines = f.read().splitlines()
    else:
        lines = sys.stdin.read().splitlines()
    packets = parse_capture(lines)
    analyze(packets)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
