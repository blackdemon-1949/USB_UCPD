# Knowledge Database

## Runtime store (`apie_db.c`)

The runtime knowledge store holds learned **source profiles**
(`APIE_Profile_t`) in RAM:

- hard identity (VID/PID/FW/HW),
- protocol signature (PDO array, PPS/EPR/SVID flags, PD revision),
- behaviour (advertisement interval, query latencies, reset count,
  negative capabilities).

Each record is wrapped in a versioned, CRC-32-protected envelope
(`APIE_DbProfile_t`: magic `'ADPB'`, version, len, crc32). Lookup deduplicates
by identity or by PDO signature. `db validate` recomputes every CRC;
`db compact` re-indexes and drops invalid records; `db test` does a scratch
store/read-back/validate self-test.

## Endurance counters

`db wear|writes|erases|checkpoint` report real counters:

| Counter | Meaning |
| --- | --- |
| `writes`   | DB store operations (RAM records) |
| `checkpoints` | logical checkpoints recorded |
| `erases`   | physical NOR erases performed (0 while persistence off) |
| `wear`     | physical NOR program/erase wear (0 while persistence off) |
| `compacts` | compaction runs |
| `selftest` | DB self-test runs |

**NOR persistence is currently DISABLED** — `persist=RAM (NOR off: XIP)`.
See [FLASH_ENDURANCE.md](FLASH_ENDURANCE.md) for why, and how the architecture
supports an append-oriented, wear-levelled backend once XIP safety is proven.

## Knowledge package (host)

`tools/build_knowledge.py` generates a deterministic, schema-versioned,
CRC-protected embedded knowledge package into `research/`:

- `research/usb_pd_knowledge.json` — human/machine-readable knowledge document
  (message tables, PDO/APDO metadata, safety policy, hardware capability gates,
  query catalogue, experiment levels, model metadata, vendor observations).
- `research/pd_knowledge.bin` — compact firmware-importable blob
  (magic `KPD1`, schema version, tagged sections, trailing CRC).
- `research/pd_knowledge.h` — C header embedding the blob, import-ready.

```sh
python3 tools/build_knowledge.py            # generate the package
python3 tools/build_knowledge.py --verify research/pd_knowledge.bin   # validate
```

The blob is deterministic (no timestamps/random data), so firmware imports are
reproducible. The firmware refuses a blob with a mismatched schema version.

## Static vs. dynamic knowledge

- **Static** (in the package): protocol definitions, decoder tables, field
  metadata, safety rules, compatibility metadata, model metadata,
  **charging-transport table** (`TRAN`), **packet schemas** (`PKSC`), scale
  notes (`SCAL`).
- **Dynamic** (RAM store): learned source profiles, negative capabilities,
  statistics, hypotheses, unknown-message behaviour.

The package is compact and machine-readable — it is not a dump of arbitrary
data, and the firmware never stores "the entire internet".

## Vendor observations

Vendor observations (e.g. PB722) are recorded as **observations** in the
package (`vendor_observations`), never as universal protocol rules, and never
as scattered `if (vendor)` branches in the PD engine.
