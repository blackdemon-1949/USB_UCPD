# Flash Endurance

## The hard requirement

External NOR: **Puya PY25Q64HA-SUH-IR, 8 MB**, used as the **XIP** storage for
the application. It has finite program/erase endurance, so the firmware must
**never** program or erase it for every packet, GoodCRC, inference,
transaction, or main-loop cycle.

## Current policy: NOR writes DISABLED

The APIE knowledge store is **RAM-resident**. External NOR persistence is
**deliberately disabled** because the application **executes from the same NOR
via XiP** at `0x90000000`.

[XIP safety]: any flash program/erase must not execute code from the flash
region being disabled/modified. It is not enough to place one wrapper function
in `.RamFunc` while a dependent HAL function still executes from XIP. The
full required command/erase/program sequence — **and its dependencies** — must
be proven safe from RAM first.

> Per the design rule: *"If this cannot be proven: KEEP NOR WRITES DISABLED and
> use a RAM persistence backend."* That is exactly the state here.

The firmware does **not** claim NOR persistence is active. The `diag flash`
and `db wear|erases` reports show `persist=RAM (NOR off: XIP)` / `erases=0` /
`wear=0` so the true state is always visible.

## What stays in RAM (high-frequency)

- raw packets
- events
- transactions
- features
- running statistics
- ML observations
- temporary hypotheses

The learning engine continues functioning entirely in RAM even though flash
persistence is unavailable.

## Persistent-write design (for a future safe backend)

When an XIP-safe NOR path is proven, the architecture supports (but does not
yet enable):

- **batched / coalesced** writes
- **append-oriented** records
- **wear-levelled sector rotation** with sector/program/erase counters
- **CRC-protected, power-loss-safe** records
- **recovery scanning** and bad-record handling

Meaningful checkpoints only:

- a new source
- a materially changed profile
- a newly confirmed protocol signature
- a materially changed model
- periodic aggregated checkpoints

Never "packet → flash write".

## Current accounting

`apie_db.c` maintains real, meaningful counters so the future backend and the
current RAM store are both observable:

| Counter | Meaning | Now |
| --- | --- | --- |
| `writes` | DB store operations | RAM record count |
| `checkpoints` | logical checkpoints | counts APIE_Db_Checkpoint() |
| `erases` | physical NOR erases | **0** (backend off) |
| `wear` | physical NOR program/erase wear | **0** (backend off) |
| `compacts` | compaction runs | counts APIE_Db_Compact() |
| `selftest` | DB self-tests | counts APIE_Db_SelfTest() |

`db test`, `db compact`, `db validate`, `db wear|writes|erases|checkpoint` all
route and report real values.

## XIP / memory notes

- The application runs from XiP NOR; Boot maps it at `0x90000000`.
- Do not run a flash command/erase/program sequence unless every dependency is
  proven resident in RAM.
- The RAM heap is in AXI SRAM (DMA-accessible); GPDMA1 cannot reach DTCM
  (AN6062) — see the README memory notes.
