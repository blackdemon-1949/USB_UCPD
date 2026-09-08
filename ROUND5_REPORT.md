# Round 5 — PC13 key timing, and a SoC temperature page

Branch `arena/01a07fb0-usb-ucpd`, three commits on top of the round-4 chain.

Build: **90 sources, 0 failed, 0 warnings, link OK.** FLASH 216 488 B (2.58 %),
RAM 45 440 B (10.09 %). `check_syntax.sh` 59/0. cppcheck → the single
pre-existing `sysmem.c:59` finding, nothing new.

---

## 1. Why PC13 "kinda worked but the timing was trash"

Your description of the board is the answer:

```
3V3 --- button --- 330 R --- PC13
```

There is **no resistor from PC13 to either rail.** Pressed, the pin is driven
HIGH through 330 Ω. Released, it is **completely floating** — it does not fall
to 0 V, it bleeds down through the pin's own leakage current over an
unpredictable time.

`gpio.c` had the pin as `GPIO_MODE_INPUT` / `GPIO_NOPULL`, justified by a
comment claiming an external 10 k pull-up that this board does not have. With
NOPULL the release edge arrives tens to hundreds of milliseconds late and at
random. That is precisely the reported symptom: the press registers, but the
*timing* is garbage, so the debounce either swallows the press or sees the
release as a second press.

**Fix — `gpio.c`: `GPIO_NOPULL` → `GPIO_PULLDOWN`.** The internal ~40 kΩ
pull-down gives a definite LOW with the button open, and 330 Ω to 3V3 still
wins easily against it when the button closes (≈3.28 V at the pin). This is
what the `.ioc` originally generated; the comment now records the schematic so
it does not get "fixed" back to NOPULL next time someone regenerates.

`APP_KEY_PRESSED()` in `app_board.h` said `RESET` (active low) — the opposite
of the schematic. Nothing calls it (the OLED key code samples the pin itself),
but it now reads `SET` so it cannot mislead the next reader.

## 2. Key timing rewrite — `app_oled.c`

The old logic inferred press/release from **one level change plus one timer**.
A timer only proves that the level changed once and then time passed, so a
single glitch could arm it. Replaced with:

| Change | Why |
|---|---|
| **Debounce by consecutive-sample count**, not by timer — a level is believed only after `KEY_STABLE_N` (5) polls in a row agree, 25 ms at the 5 ms poll rate | One spike can no longer become a press; mechanical bounce is still covered |
| **Explicit state machine** — `K_IDLE / K_DOWN / K_WAIT_2ND / K_IGNORE` instead of counters reset from several places | The old flow had three places that cleared `s_clicks` and no single place that owned the sequence |
| **Double press fires the instant the second press is released** instead of waiting out the rest of the window | Removes ~350 ms of dead time from the action you perform most |
| **`KEY_STUCK_MS` (4 s)** — a level held that long is driven or shorted, not pressed, so it is ignored until it releases | A stuck pin can no longer sit in the machine quietly accumulating clicks |
| Window is **350 ms**, measured from the release of the first press | Forgiving enough for a real double press, still snappy |
| Start-up calibration no longer falls back to *active low* when the level is unstable during sampling | That fallback is what left the key dead on a board whose schematic is active HIGH. It now keeps the documented polarity; the idle re-learn corrects it within 5 s if a board really is wired the other way |

Verified on the host render harness against the real `app_oled.c`:

```
idle=0 / active_high=1
1 press                     -> page 1
2nd press                   -> page 2
5.5 s hold                  -> ignored, page unchanged
40 ms bounce + 80 ms solid  -> exactly one press
double press                -> APP_PD_SendRequest(index=1), page unchanged, "REQ PDO 1"
```

Nothing else in the file changed: pages, layout, timings and the I2C chunking
are untouched.

## 3. Page 5 — SoC die temperature

`APP_OLED_PAGES` 5 → 6. The new page reuses the proven `page_sensor`
layout — header, frame, big value with unit, footer — so it cannot disturb the
vertical budget pages 0..4 depend on.

* Big value: current die temperature, unit `C` or `F` following your
  `dts unit c|f` setting. The HAL only reports whole degrees, so there is
  nothing to round.
* Footer: a **MIN/MAX window tracked since boot**, updated every poll rather
  than only while page 5 is on screen, so the numbers mean something the
  moment you switch to it. Built without printf.
* No reading yet → `--` and `NO READING`. The DTS is clocked from the LSE,
  which the Boot project does not start, so showing a plausible-looking zero
  would be a lie.

The header page dots and the `oled page <n>` bound check pick the new count up
automatically; `help` lists `oled page <0-5>` with 5 = SoC die temperature.

## 4. USB CDC HS

You said it is still trashy. The concrete defects I could prove are fixed
(round 4): the 7165-byte `help` dump was being written into a 2048-byte ring,
and the writer was not atomic against the priority-0 UCPD interrupt. The
clock chain and the device serial number were already correct.

To tell whether what remains is the firmware or the host, **`stats` and `info`
print the console drop count** (`APP_LOG_Dropped()`). If that number climbs
only while the console is misbehaving, the host is not draining the port fast
enough and the firmware is protecting itself; if it stays at zero while text
still goes missing, the loss is on the wire or in the host driver. That
distinction is what tells me where to look next — I would rather not change
USB init on a guess, because the working setup is easy to break.

## 5. Re-test list

1. PC13, one press → next page. Should feel immediate and never double-step.
2. PC13, two presses → `REQ PDO n` and the voltage changes.
3. Hold PC13 down for 6 s → nothing should happen, no runaway page stepping.
4. Press PC13 five or six times quickly → the pages should advance one per
   press, no skipped or repeated steps.
5. `oled page 5` → SoC temperature, big number, `MIN`/`MAX` in the footer.
6. `dts unit f` → page 5 should follow and report in °F.
7. Press through all six pages and confirm the header dots now show six.
8. `stats` → note the drop count before and after opening the console.
