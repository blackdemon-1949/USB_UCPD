# Round 4 — console corruption, PD hard resets, PC13, hidden commands, voltage profiles

Branch `arena/01a07fb0-usb-ucpd`. Display UI frozen: `app_oled.c` / `ssd1306.c` /
`oled_font.c` visuals are untouched; the only edits to `app_oled.c` are key
handling and one status line.

Build: `tools/check_arm_build.py` → **90 sources, 0 failed, 0 warnings, link OK**,
FLASH 215 132 B (2.56 %), RAM 45 416 B (10.08 %).
`tools/check_syntax.sh` → 59 passed / 0 failed.
cppcheck 2.17.1 (`-D__GNUC__`) → one finding, the pre-existing `sysmem.c:59`
`comparePointers`. Nothing new.

---

## 1. Garbled / truncated USB-CDC console — **root cause found and fixed**

The symptom was text cut off mid-word with PD lines landing inside the `help`
listing, e.g.

```
[P[PD] hard reset
D] SNK ready
```

Two independent defects, both in `Appli/Core/Src/app_log.c`.

**(a) The log ring was smaller than one console message.**
`APP_CLI_PrintHelp()` writes the entire listing with a single `APP_LOG_Write()`
call. Measured by extracting every string literal from that one function, the
text is **7165 bytes**. The ring (`LOG_Q_SIZE`) was **2048 bytes**. The ring
filled, the rest of the listing was dropped, and the next PD line arrived in the
middle of it. That is the whole "corruption".

`LOG_Q_SIZE` is now **12288**, so a full banner plus the help dump fits with
headroom for PD and INA226 traffic while it drains.

**(b) `APP_LOG_WriteRaw()` was not atomic.** It pushed bytes into the ring one
at a time with interrupts enabled. UCPD1 runs at priority 0 and can pre-empt a
writer between bytes, so two messages could be interleaved byte by byte — which
is exactly what `[P` … `[PD] hard reset` … `D]` looks like. The whole enqueue
now runs with PRIMASK masked, restoring the *prior* state rather than blindly
re-enabling interrupts, so a caller already inside a critical section is not
released early. A NULL buffer is now ignored instead of dereferenced.

Now that the ring cannot be overrun mid-message, an over-long message is
truncated at the tail and counted in the drop counter (exposed by the log
statistics) rather than wrapping over earlier text.

**What I did not find a cause for: the COM port renumbering.** That is a
host-side enumeration artefact, not firmware — the port number is assigned by
Windows/macOS when the USB device is enumerated. It is aggravated by the device
re-enumerating, and re-enumeration comes from the physical USB cable or from a
genuine MCU reset. I checked for spurious resets and there are none: no
watchdog is enabled anywhere in the tree, and the fault handlers are bare
`while (1)` loops, so nothing in the firmware is resetting the part. The
duplicate banner the owner sees is *not* a reset either — `APP_CLI_OnHostOpen()`
deliberately re-arms the greeting when the host asserts DTR
(`usbd_cdc_if.c:225`) and when a host that never toggles DTR sends its first
character (`:262`). That is intended; with the 2 kB ring it was re-issuing a
7 kB banner into a buffer that could not hold it, which is what looked like a
reset.

**Clock chain re-verified, no change needed** (round-2 work is intact):
`USB_DEVICE/Target/usbd_conf.c` sets `USBREFCKSEL` = 24 MHz and
`RCC_USBPHYCCLKSOURCE_HSE`, reads both fields back into `s_usb_clock_ok`, gates
on `PWR_CSR2_USB33RDY` via `HAL_PWREx_EnableUSBVoltageDetector()`, and sets
`OTG_HS_IRQn` to `IRQ_PRIO_CDC_USB`. A clock misconfiguration would show as no
enumeration at all or as framing errors, not as truncated text; the text itself
was complete and correctly ordered inside each queued chunk, which is what
pointed at the ring rather than the PHY.

---

## 2. Constant PD hard resets — **a transmitting-by-default bug found and fixed**

`APIE_EXP_LEVEL_DEFAULT` was **2**, i.e. R2. Because `APIE_Exp_Allows(R1)` is
true at R2, `APIE_Task()` treated autonomous queries as permitted *from
power-on*: as soon as a contract was up it would transmit Get_Status,
Get_PPS_Status, Get_Source_Cap_Ext, Get_Manufacturer_Info, Get_Battery and VDM
Discover Identity roughly every 500 ms (`APIE_QUERY_COOLDOWN_MS`). Many
chargers respond to an extended or VDM message they do not implement with a
hard reset — giving the observed reset → re-attach → query → reset loop.

Fixes:

* `APIE_EXP_LEVEL_DEFAULT` is now **0 (R0, observe only)**. The board never
  transmits unless the owner asks it to. R1/R2 are unchanged and still
  selectable with `experiment set 1|2`.
* `APIE_OnHardReset()` gains a **guard**: three hard resets within
  `APIE_HR_WINDOW_MS` (10 s) while the level is not R0 drops back to R0 and
  prints a one-line explanation. An owner who does enable R1/R2 against a
  hostile charger can no longer wedge the console in a reset loop.
  New API: `APIE_HardResetGuardTripped()` / `APIE_HardResetGuardClear()`.
* Hard resets now report **which side generated them**:
  `[PD] hard reset (from us)` vs `[PD] hard reset (from source)`.
  `APP_PD_OnNotify()` already received `_TX` and `_RX` separately but printed
  the same line for both, so the owner's log could not show who was resetting
  whom. This is the one fact needed to finish the diagnosis.

**Honest status.** Removing the autonomous transmissions is a definite fix for a
definite bug, and it is by far the most likely cause. I cannot prove from the
log alone that it is the *only* cause. If the loop survives, the new
`(from us)` / `(from source)` text settles it in one capture:

* `(from us)` → the PE stack here is unhappy; send `diag pd` and ` packets all`.
* `(from source)` → the charger dislikes something we send even at R0 (the
  explicit contract request); send `caps` and `diag tx`.

The INA226 collapsing to ~0.5–3 V during a reset is the source disabling its
power path. Expected, not a defect.

---

## 3. PC13 — **made self-recovering**

The key logic was already correct; the failure mode was the start-up
calibration. It samples the pin for 250 ms and infers polarity from that. Your
board is wired **HIGH when pressed**, the opposite of `app_board.h`, so if the
pin was floating or the button was already held at power-on the calibration
guessed wrong and the key stayed dead with no recovery short of typing
`oled key high`.

Change: the polarity is **re-learned** whenever the pin has sat quietly at one
level for `KEY_RELEARN_MS` (5 s) with no press in progress. A long hold cannot
wedge it — the rule is skipped while a press or a click is pending and
self-corrects 5 s after release. `app_board.h` and `gpio.c` are untouched, as
instructed.

`oled` now reports the live pin level, the learned idle level and the number of
presses ever seen, so if it still misbehaves the capture shows why:

```
PC13 now 0 (idle 0)  presses seen 3  profile steps 0
```

Re-verified on the host render harness against the real `app_oled.c`:
idle=0 / active_high=1, one press → page 1, second press → page 2, double press
→ `APP_PD_SendRequest(index=1)` with the message `REQ PDO 1`. Identical to
before.

---

## 4. Hidden commands — **found the real mechanism**

The earlier sweep compared `argv[0]` against literals and asked whether each
literal appeared in the help text as a whitespace-delimited word. That misses
literals that appear only as part of a `|`-separated alias list. Re-run
correctly over all **60** top-level commands:

* `patterns` and `hypotheses` were already documented at
  `unknown|patterns|hypotheses` — the old regex missed them because of the `|`.
  Not hidden after all.
* `fingerprint` **was** effectively hidden: it appeared only in a parenthetical
  next to `source|profile|profiles`. It now has its own help line.
* No prefix/`strncmp`/case-insensitive dispatch exists in the CLI, so those are
  the only mechanisms there could have been.

Every top-level command is now listed. Help text is 7165 bytes.

---

## 5. Voltage profiles — **new feature**

New files `Appli/Core/Inc/app_profile.h` and `Appli/Core/Src/app_profile.c`
(513 lines), wired into `app_cli.c` and `main.c`.

An ordered list of up to 8 steps, built from the terminal, walked by a double
press on PC13:

```
profile                      list the steps
profile add fixed <pdo> [ma] step to a fixed supply PDO (1..7)
profile add pps <mv> [ma]    step to an exact PPS voltage
profile del <n> | clear      delete one step / all steps
profile apply <n> | next     apply a step now / the next one
profile pos [n]              show or set the position
profile save | load          NOT available - see below
```

Example — make the button cycle 9 V, then 5 V at 2 A, then an exact 12.6 V:

```
profile clear
profile add fixed 2
profile add fixed 1 2000
profile add pps 12600 3000
profile
```

Every step goes through the existing `APP_PD_SendRequest()`, and PPS steps use
the same `APP_PD_FindBestPdo()` helper as `pps` and `volt`, so a profile cannot
ask for anything the CLI would reject; it bounds-checks the PDO index against
the source's current capabilities and tells you when a step does not apply.

**No persistence, stated honestly.** The list lives in RAM. The application runs
in place from the external flash (XiP), so there is no spare sector to write
to, and no battery-backed RAM is wired on this board. `profile save` and
`profile load` say exactly that instead of pretending. Say the word and I will
wire it to a spare flash sector.

With an **empty** profile the button keeps its previous behaviour: step through
the source's SPR fixed PDOs. Nothing regresses for anyone who never types
`profile`.

**Name collision fixed.** The APIE source-profile branch already claimed
`profile` and sits earlier in `handle_line()`, so the new command would never
have been reached. That branch now accepts `source`, `profiles` and
`fingerprint`; `profile` (singular) is the voltage-profile list.

---

## 6. Re-test list

1. Open the console, type `help` — the listing must be complete and unbroken,
   with no PD lines inside it. Reconnect several times.
2. Attach a source. The `[PD] hard reset` lines should stop. If any appear, note
   whether they say `(from us)` or `(from source)`.
3. `experiment` should print level 0 (R0).
4. PC13: one press → next page; double press → `REQ PDO n` and the voltage
   changes. Then pull the cable and power up **with the button held** — release
   it and wait ~5 s; the key should start working on its own.
5. `oled` → check `PC13 now … (idle …)  presses seen …  profile steps 0`.
6. Build the profile example above and double-press through it.
7. `profile save` should print the honest "no non-volatile storage" message.
8. Confirm the display still looks exactly as before.

## 7. Not done / needs hardware

* **COM-port renumbering** — host-side, no firmware cause found.
* Final confirmation of the hard-reset fix needs one capture on the bench.
* Profile persistence needs a real non-volatile region to be chosen.
