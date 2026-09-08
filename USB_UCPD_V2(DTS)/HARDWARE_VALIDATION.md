# Hardware Validation

## Status

**NOT hardware-verified in this session.** The APIE platform has been
cross-compiled, linked and host-tested (build + host tests), but no live PD
source was exercised on the bench here. Everything below is the exact
validation procedure for the user.

## Already verified

| Item | Status |
| --- | --- |
| Host compiler (syntax) | `tools/check_syntax.sh` 54/54 |
| ARM cross-build + link | `tools/check_arm_build.py` PASS (Boot + Appli, 0 Appli warnings) |
| Decoder host self-test | `tools/apie_selftest.sh` 67/67 (incl. PB722 vectors) |
| Python decoder cross-check | `tools/apie_decode.py selftest` |
| Knowledge package | `tools/build_knowledge.py` + `--verify` |
| CLI routing coverage | `tools/cli_coverage.py` |

The underlying ST PD foundation was previously validated by the user on
hardware (CC attach, caps discovery, fixed PDO negotiation, PPS, voltage
requests, hard/soft reset, detach/reattach, USB CDC, INA226, DTS, STM32 UCPD,
external NOR/XIP, USART diagnostics).

## Acceptance criteria — regression (critical)

Adding APIE must **not** break the original working firmware. On hardware verify:

1. Boot + CDC console responsive.
2. CC attach reliable.
3. Source_Capabilities arrives reliably.
4. 5 V contract works.
5. Fixed PDO requests (9/12/15/20 V) work.
6. PPS works within safe limits.
7. Hard/soft reset works.
8. Detach/reattach works.
9. INA226 reports real V/I.
10. DTS reports SoC temperature.
11. No APIE queue overflow may cause PD failure (watch `diag queue`).

## Intelligence-layer verification

- `raw` / `pd packets` capture and decode real packets.
- `txn` shows correct request→response transactions.
- `source` / `fingerprint` build a profile.
- `unknown`, `stats`, `ml`, `scheduler`, `diag *` all respond.
- `db test`, `db validate`, `db wear|erases|checkpoint` report real values.
- `safe-mode on` disables intelligence while the PD sink keeps working.

## Required hardware setup

- WeAct STM32H7R3Z8 board.
- PD source wired to PM0 (CC1) or PM1 (CC2) + GND (CC-only rig; no VBUS ADC).
- USB-HS Type-C to the PC for the CDC console.
- (Optional) USART2 on PD5/PD6 as a second console.
- (Optional) INA226 on I2C2 (PB10/PB11), 5 mΩ shunt.
- (Optional) USART1 PA9/PA10 @ 921600 for the STM32CubeMonitor-UCPD trace.

## Procedure

1. Flash Boot (internal FLASH), then Appli (XiP) — see `FLASHING.md`.
2. Open the CDC port, confirm the boot banner and `help`.
3. Attach the source; confirm attach and caps.
4. `caps`, then `req 1..n` / `volt 9000` / `pps 9000 1000` and confirm the
   contract and INA226 readings.
5. Run the intelligence commands above.
6. Toggle `safe-mode on/off` and confirm the PD sink keeps negotiating.

Until this is done, APIE is **HOST/BUILD VERIFIED only**, not **HARDWARE
VERIFIED**.
