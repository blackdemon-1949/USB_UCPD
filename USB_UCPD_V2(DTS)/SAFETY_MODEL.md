# Safety Model

Safety is a first-class constraint in APIE. The guarantees, in order:

1. **The real-time PD sink must never be jeopardised by the intelligence layer.**
2. **Electrical limits are guard rails, not advisory.**
3. **ML never overrides safety.**
4. **Unknown/vendor transmission is never enabled by an ML prediction.**

## Hard electrical limits (`apie.h`)

| Guard rail | Value |
| --- | --- |
| Max voltage | 21000 mV (all policies) |
| Max current | 5000 mA |
| PPS step | 100 mV (safe ramp) |
| Query cooldown | 500 ms |
| Query timeout | 1200 ms |
| Max pending queries | 2 |

PPS requests are validated against the advertised source range and these guard
rails; APIE never requests outside the source-advertised range.

## Hardware capability gates

| Gate | Value | Meaning |
| --- | --- | --- |
| `APIE_HW_EPR_POWER_ENABLED` | 0 | EPR (>20 V) never energised |
| `APIE_HW_HAS_VBUS_ADC` | 0 | synthetic VBUS policy model |
| `APIE_HW_HAS_DPLUS_DMINUS` | 0 | D+/D- not wired — no non-PD charging claims |
| `APIE_HW_CABLE_EMARKER` | 1 | SOP'/SOP'' observable over UCPD |

`PROTOCOL_SUPPORT` (decode/observe) is deliberately separate from
`ELECTRICAL_HARDWARE_SUPPORT` (safe to energise). EPR/AVS is decoded and
tracked but never energised on this board.

## Experiment levels

| Level | Name | Default | Notes |
| --- | --- | --- | --- |
| R0 | observation | ON | capture/decode/analyse |
| R1 | standard informational query | ON | Get_Status, Get_PPS_Status, VDM discovery, etc. |
| R2 | standard power request within validated limits | ON | only inside validated source/hardware range |
| R3 | state-changing experiment | OFF (compile-gated `APIE_EXP_ALLOW_R3=0`) | |
| R4 | unknown/vendor transmission | OFF (compile-gated `APIE_EXP_ALLOW_R4=0`) | never auto-enabled by ML |

`experiment` / `experiment set <0-4>` on the CLI only raises the level if that
level is compiled in. R3/R4 cannot be turned on at runtime on this build.

## Fault containment — APIE_SAFE_MODE

If the intelligence layer fails (queue corruption, memory failure, decoder
panic, ML failure, database failure, scheduler runaway, unexpected runtime
condition), `APIE_SetSafeMode(1)` disables **only** the intelligence/analytics
components (`APIE_Task` returns immediately) while the underlying PD sink keeps
negotiating. `safe-mode on|off` / `safe on|off` drives it from the CLI.

## Compute budget

All heavy analysis runs in the super loop with bounded jobs; the APIE per-call
compute budget is measured in cycles via the DWT cycle counter
(`diag cpu`). UCPD, USB, PD timers and CDC always take priority
(see [DIAGNOSTICS.md](DIAGNOSTICS.md)).

## CLI

`safety` / `safety status` / `safety limits` / `ap safety` print the guard
rails and capability gates. `diag safety` and `diag flash` print related state.
