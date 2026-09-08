# APIE — Advanced PD Intelligence Engine (Architecture)

The APIE layer turns the working ST UCPD/USBPD sink into an advanced
**universal** PD analysis/compatibility platform while preserving the proven
real-time foundation exactly.

## Guiding principle

> **Keep ONE authoritative real-time USB-PD protocol engine.** APIE sits
> *above* the ST application/DPM side. It observes and extends; it never
> replaces or re-enters the ST PE/PRL/CAD path.

```
ST UCPD / PHY
    ↓
ST USBPD PRL / PE / CAD   (untouched — the single real-time engine)
    ↓
APPLICATION / DPM  (usbpd_dpm_user.c, usbpd_vdm_user.c, usbpd_pwr_user.c)
    ↓
APIE EVENT HOOKS  (bounded copy from callbacks, never in ISR)
    ↓
RAW OBSERVER      apie_analyzer.c   (bounded packet ring, copy-only)
    ↓
DECODER           apie_decode.c     (deterministic, host-testable)
    ↓
TRANSACTION ENGINE apie_analyzer.c
    ↓
SOURCE/CABLE PROFILE apie_profile.c / apie_cable.c
    ↓
STATISTICS        apie_stats.c      (Welford, bounded)
    ↓
ML / INFERENCE    apie_ml.c         (Naive Bayes + logistic head)
    ↓
KNOWLEDGE         apie_db.c         (RAM, versioned + CRC, dedup)
    ↓
SAFE DECISION / QUERY PLANNER  apie_plan.c   (info-gain scheduler)
    ↓
ST DPM APIs (issue standards-compliant queries only)
```

## Ownership and the RX buffer (critical)

The raw observer **copies** bytes out of the ST-owned RX buffer after a
message-complete event. It never re-arms or replaces `Ports[0].ptr_RxBuff`,
and it never re-introduces `s_rx_buf[2][264]` or any equivalent. There is
exactly **one** DMA RX buffer, owned by the ST PRL. The APIE capture hook is a
bounded copy + enqueue + return.

## Interrupt / super-loop split

| Context | What APIE does |
| --- | --- |
| UCPD / DMA / USB ISRs | **nothing heavy**; only a bounded copy+enqueue of the raw message |
| PE / PRL / DPM callbacks | event feed (`APIE_On*`), bounded updates |
| Super loop (`APIE_Task`) | all analysis, feature build, ML, scheduler, DB, diagnostics |

This guarantees APIE can never starve UCPD, USB CDC, PD timers, or critical
interrupts. Heavy work is bounded by small fixed-size tables.

## Modules

| Module | Role |
| --- | --- |
| `apie.c` | event feed + super-loop pipeline, safe-mode, experiment level, diag timing |
| `apie_analyzer.c` | raw ring, transaction engine, feature extraction |
| `apie_decode.c` | deterministic decoder (no ST-header dependency) |
| `apie_profile.c` | source fingerprint (hard + protocol + behaviour) |
| `apie_cable.c` | cable (SOP'/SOP'') profile, AVS/EPR awareness |
| `apie_unknown.c` | unknown/unnamed message analysis |
| `apie_stats.c` | Welford stats, rate trackers, histograms |
| `apie_ml.c` | Naive Bayes + logistic head, model metadata/CRC |
| `apie_plan.c` | query scheduler, information gain, experiment levels |
| `apie_db.c` | RAM knowledge store, versioned + CRC, endurance counters |

## Data-flow separation

The task requires that *one physical packet* never becomes *multiple logical
transactions*. APIE enforces this by:

- a single capture point (`APIE_Analyzer_CaptureRaw`) fed by the RX-complete
  bridge — every packet is counted once;
- a message-ID + SOP + type-keyed transaction correlation (`APIE_Txn_OnRx`);
- raw packets, events, transactions, features and ML observations are distinct
  structures that reference each other by counters, not by duplicating data.

## Request-origin tracking

Every power request the application originates is tagged by origin
(`MANUAL`, `AUTO`, `REMEMBER`, `SCHEDULER`, `EXPERIMENT`, `REPLAY`, `SYSTEM`,
`UNKNOWN`). Repeated wire traffic is not assumed to be a duplicate request
merely because a callback fires more than once; the transaction engine keys on
message ID and outcome.

## Fault containment / safe mode

If the intelligence layer detects an unexpected condition, `APIE_SAFE_MODE`
disables only the analysis/ML/scheduler components while the PD sink keeps
running. See [SAFETY_MODEL.md](SAFETY_MODEL.md).

## Status

- Build: `tools/check_syntax.sh`, `tools/check_arm_build.py` — see README.
- Host tests: `tools/apie_selftest.sh` (decoder), `tools/apie_decode.py`,
  `tools/cli_coverage.py`, `tools/build_knowledge.py --verify`.
- Hardware validation status: see [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md).
