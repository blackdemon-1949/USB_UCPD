# Diagnostics

APIE provides enough diagnostics to explain any compatibility failure, without
continuous diagnostic spam.

## Status / PD / counters

- `status` — PD + USB + INA226 + DTS + APIE summary.
- `pd stats` — PD PHY/PE counters (irq/rx/tx/hard-reset), transaction history.
- `pd` — UCPD registers + PHY counters.
- `apie` / `ap status` — state, safe mode, experiment level.
- `ap stats` — analyzer / transaction / unknown / ML counters.

## Raw capture

- `raw` / `raw dump [all]` / `pd packets [all]` — bounded raw packet ring
  (timestamp, direction, SOP, header, message ID, revision, roles, type,
  object count, extended, payload).
- `raw stats` — counters.
- `raw export` — machine-readable capture for the host replay tool
  (`tools/apie_replay.py`).
- `raw clear` — reset the ring.

## Intelligence

- `source` / `ap source` — source profile + cable + EPR diagram.
- `fingerprint` / `ap fingerprint` — signature, PDO count, PPS/EPR/SVID/
  battery/identity.
- `txn` / `ap txn` — completed/active transactions (start, end, latency, SOP,
  request, response, result).
- `unknown` / `ap unknown` — unknown-protocol buckets.
- `ml` / `ap ml` — model id/version/CRC + class counts.
- `predict <q>` / `ap predict <q>` — query usefulness classification.
- `scheduler` / `ap scheduler` — per-query scheduler state (cooldown, score,
  attempts, successes/failures).
- `feature` / `ap feature` — current feature vector.

## Database / endurance

- `db [status|dump]` — stored profiles + CRC state.
- `db validate` — recompute every record CRC.
- `db compact` — re-index / drop invalid.
- `db test` — scratch store/read-back self-test.
- `db wear|writes|erases|checkpoint` — endurance accounting (see
  [KNOWLEDGE_DATABASE.md](KNOWLEDGE_DATABASE.md)).

## `diag` sub-commands

| Command | Reports |
| --- | --- |
| `diag pd` / `diag rx` / `diag tx` | PD PHY/PE RX/TX counters |
| `diag txn` | transaction history |
| `diag decoder` | points to `selftest decoder` |
| `diag ucpd` | UCPD registers + PHY |
| `diag usb` | USB/PD status |
| `diag queue` | active txn / txn history / unknown / raw ring depth |
| `diag timing` | super-loop task calls, period max/avg (ms), DWT on/off |
| `diag cpu` | APIE per-call compute budget (max cycles via DWT) |
| `diag memory` | board / memory / clocks |
| `diag profile` | source profile + cable + EPR |
| `diag unknown` | UNKNOWN_SIGNATURE buckets |
| `diag faults` | APIE status summary (safe mode, state) |
| `diag trace` | pointer to the USART1 USBPD tracer (CUBEMONITOR_UCPD.md) |
| `diag ml` | model status + anomaly detector |
| `diag scheduler` | scheduler state |
| `diag knowledge` / `diag db` | knowledge database status |
| `diag packets [all]` | raw packet ring |
| `diag safety` | safety limits + hardware gates |
| `diag flash` | NOR persistence status + erase/wear/checkpoint counters |

Packet / transaction views are also available directly:

- `packets`, `packets raw [all]`, `packets decoded [all]`,
  `packets unknown`, `packets tx`, `packets rx`.
- `transactions`, `transactions active`, `transactions history`.

## One-command self-test

`selftest` runs every non-destructive check automatically (see
`apie_selftest.c`): decoder, stats, CRC, ML (NB + tree + anomaly), transaction
engine, database, PD/safety/INA/DTS status, and flash status.  Scopes:
`selftest quick|full|pd|decoder|ml|database|flash`.  It never issues a power
request, never changes voltage, and never programs/erases NOR.

## Instrumentation

- Main-loop period and the APIE per-call cycle budget are measured in
  `APIE_Task` using the DWT cycle counter (enabled once at init; never in an
  ISR). This is the "main-loop timing" / "CPU" diagnostic.
- RX/TX/event/transaction/duplicate/unknown counts and decoder-error state are
  tracked in the bounded analyzer.

## Fault capture

`APIE_SAFE_MODE` (see [SAFETY_MODEL.md](SAFETY_MODEL.md)) disables the
intelligence components on an unexpected runtime condition while keeping the
PD sink alive. Fault registers (PC/LR/SP/CFSR/HFSR/BFAR/MMFAR and register
context) are captured where the platform fault handler provides them.
