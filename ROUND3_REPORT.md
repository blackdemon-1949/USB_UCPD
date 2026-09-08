# Round 3 — OLED page + CLI help audit

Branch `arena/01a07fb0-usb-ucpd`, commits `1962f56..8478fb5` (parent `b03122f`, round 2).

Two independent jobs. Neither one changes anything that was already working:
the INA226, the PD negotiation, the USB CDC console, USART2 and the NVIC
priority ordering are all byte-for-byte the round-2 behaviour.

---

## 1. CLI help audit — every real command is now listed

**What I did:** extracted every `argv[0]` string out of the two dispatchers
(`apie_cli_dispatch()` and `handle_line()` in `Appli/Core/Src/app_cli.c`), then
checked each one against the text `APP_CLI_PrintHelp()` prints. 60 top-level
commands. I also walked the two delegated handlers, `DTSMON_Cli()` and
`INA226_Cli()`, for their sub-commands.

**What was actually missing:**

| Command | Where it is implemented | Status before |
|---|---|---|
| `oled ...` | `handle_line()` → `APP_OLED_Cli()` | new this round, not listed |
| `dts read`, `dts temp` | `DTSMON_Cli()` | implemented, not listed |
| `dts status` | `DTSMON_Cli()` | implemented, not listed |

**What was listed but misleading** (this is the part that would have cost you
time, so I rewrote the whole APIE block):

* It presented the intelligence commands as `ap <sub>` only. In fact **every
  one of them works bare too** — `apie_cli_dispatch()` strips a leading `ap`
  and recurses, so `stats` and `ap stats` are literally the same call. The one
  exception is bare `status`, which `handle_line()` claims first and routes to
  the PD status; use `apie` or `ap status` for the intelligence status.
* It wrote `ap raw|packets [all]` as if `raw` and `packets` were aliases.
  They are not: `raw` takes `clear|dump [all]|stats|export`, `packets` takes
  `raw|decoded|unknown|tx|rx|all`.
* Same for `ap knowledge|db`: `knowledge` and `db` are separate commands, and
  `db` has eight sub-commands (`dump|status|validate|compact|test|wear|writes|
  erases|checkpoint`) of which only two were shown.
* `selftest` was listed twice with different-looking arguments; it takes one
  optional scope from `all|quick|full|pd|decoder|ml|database|flash`.

**Verification:** re-ran the extraction against the new text — 60/60 commands
present, and nothing appears in the listing that the dispatcher does not
implement. No invented commands.

---

## 2. 0.96" 128x64 white I2C OLED on I2C2, sharing the bus with the INA226

### Files

| File | What it is |
|---|---|
| `tools/gen_oled_font.py` | ASCII-art source for the 5x7 font; regenerates the table |
| `Appli/Core/{Src/oled_font.c,Inc/oled_font.h}` | generated, 57 glyphs — do not hand-edit |
| `Appli/Core/{Src/ssd1306.c,Inc/ssd1306.h}` | minimal SSD1306 driver, framebuffer + chunked push |
| `Appli/Core/{Src/app_oled.c,Inc/app_oled.h}` | the five pages, the PC13 key, the `oled` command |
| `Appli/Core/Src/main.c` | two calls added: `APP_OLED_Init()`, `APP_OLED_Poll()` |
| `Appli/Core/Src/app_cli.c` | `oled` dispatch + the help work above |

### Pages

1. **BUS VOLTAGE** — INA226 bus volts, `20.05 V`
2. **CURRENT** — INA226 current, `1.53 A`
3. **POWER** — INA226 power, `30.70 W`
4. **REQUESTED** — requested volts big, requested amps at 2x, `PDO 3 / LIVE` in the footer
5. **PROTOCOL** — `PD 3.0`, the PDO type (`SPR FIXED / PPS APDO / BATTERY / VARIABLE`), `PDO 3  CC1`

Chrome: inverse-video header bar with the page indicator punched out of its
right end, corner brackets, bold italic seven-segment digits (24 px tall,
sheared), CRT scanlines over the digit band only so the text stays crisp, and
a one-line footer.

### Layout

The whole thing is a fixed row budget, written out at the top of
`app_oled.c`, and no two things are drawn into the same rows:

```
 0..12  header bar (title left, page dots right)
13..14  gap
   15   top corner brackets
16..39  big seven-segment band
41..54  secondary 2x line   (REQUEST page)
35..49  two 1x lines        (PROTOCOL page, which draws no big band)
   54   bottom corner brackets
55..61  footer text
62..63  gap
```

This was not cosmetic. The first cut put the page dots in the footer at rows
56..59, which overlapped the footer text (rows 56..62) and made
`INA226 BUS` render as `INA226PBBB-`. I only caught it by rendering the
framebuffer on the host and decoding the pixels back to characters — see
"Verification" below.

### PC13

* one press → next page (wraps 4 → 0)
* two presses → request the next SPR **fixed** PDO, i.e. `req 1`, `req 2`, …
  up to `req 7`, skipping APDO/battery/variable entries
* polled every 5 ms, 30 ms debounce, 320 ms double-press window, counted on
  release (so a single press has a 320 ms latency — unavoidable if a double
  press is to mean something)
* **`oled key high|low|auto`** overrides the polarity
* **auto** (the default) samples the pin for 250 ms at start-up: whatever
  level is stable while nobody is touching the key is the idle level, so
  pressed is the other one. Your board is wired high-when-pressed, the
  silkscreen and `app_board.h:14` say active low — with `auto` neither one
  has to be right. I did **not** touch `gpio.c` or `app_board.h`.
* no EXTI, no NVIC change, nothing else uses PC13

### Sharing I2C2 with the INA226

* every access is one short, complete, blocking `HAL_I2C_Master_Transmit`, so
  the INA226 driver never finds `hi2c2` mid-transfer
* a 1024-byte frame goes out as sixteen 64-byte chunks, **one chunk per
  super-loop pass**, ≥ 2 ms apart — about 1.5 ms of bus time each at 400 kHz
* the column/page window is re-pointed at the start of every frame, so a
  frame aborted by a bus error cannot leave the panel's address counter in the
  middle of the screen
* bus error → mark absent, re-probe every 5 s
* nothing answers at boot → every entry point is a no-op, forever

Both I2C2 consumers run from the super loop (`EXT_I2C_Poll()` →
`EXT_I2C_FeaturePoll()`, and `APP_OLED_Poll()`), so there is no ISR/main-loop
concurrency on the handle at all.

### Verification

| Check | Result |
|---|---|
| ARM build (`tools/check_arm_build.py`) | **PASS** — 89 sources, 0 failed, **0 warnings** |
| Flash / RAM | 207 808 B (2.48%) / 35 104 B (7.79%) — round 2 was 196 652 / 34 008, so +11 156 B flash, +1 096 B RAM (1 024 of that is the framebuffer) |
| Boot image | unchanged, still 35 sources / 1 pre-existing warning |
| `bash tools/check_syntax.sh` | 58 passed, 0 failed |
| cppcheck 2.17.1 (full Appli + Boot) | 3 findings, **all three are the round-2 baseline** `comparePointers` false positives in `cmsis_gcc.h` and `sysmem.c`. Nothing new. |
| Host render harness | rendered all five pages plus the no-INA226 and transient-message variants, decoded the framebuffer back to text: headers, footers, right-aligned footers and big digits all read correctly; page indicator highlights the right dot on every page |
| Key simulation | 1 press → page advances; double press → `APP_PD_SendRequest(index=1)` on the first double press (i.e. `req 1`), page unchanged, message `REQ PDO 1` |

The host harness compiled the real `ssd1306.c`, `oled_font.c` and `app_oled.c`
into one translation unit against stub HAL/PD/INA226 headers. It lives in
`/tmp/oledsim` (scratch, not committed); `oled_preview.png` in the workspace
root is its output — the six rendered screens.

---

## What should now be true on the bench

* With **no display connected**: the firmware behaves exactly as it did in
  round 2. One extra line on the console at boot (`oled: SSD1306 not found at
  0x3C on I2C2`), and `oled` becomes a no-op command. Nothing else changes.
* With the display on **PB10/PB11** alongside the INA226: pages 1–3 track the
  INA226, page 4 shows what you requested, page 5 shows the protocol. The
  INA226 readings keep coming on the console at their normal rate.
* **PC13** steps pages; two quick presses walks the fixed PDOs 1..7.
* If the display is at `0x3D` instead of `0x3C` (some modules), `oled addr 3d`
  switches it live.

## Owner re-test list

1. Flash and confirm the boot banner still appears on **both** USB CDC and
   USART2, and that `ina` still reads the INA226.
2. `help` — confirm the new OLED section and the APIE block read correctly,
   and that `oled`, `dts read` and `dts status` all work as described.
3. With the display wired: check all five pages, then press PC13 once (page
   should step ~320 ms after release) and twice (should request the next fixed
   PDO and show `REQ PDO n` for 1.6 s).
4. If PC13 does the opposite of what you expect, `oled key high` or
   `oled key low` — no rebuild needed.
5. `oled off` then `oled on` — the panel should blank and come back.
6. Pull the SDA wire while running: the console should keep working and the
   display should come back on its own within ~5 s of reconnection.

## One thing to know

I lost the round-3 edits to `main.c` and `app_cli.c` partway through by
running `git reset --hard` to sync the branch to the round-2 tip — the working
tree had those files untracked while the fetched history had them tracked, so
the reset overwrote them. They were reapplied and re-verified (build, syntax,
cppcheck, render harness all re-run after). No committed state was affected.
Flagging it because it is the kind of thing worth knowing happened rather than
having it turn up as a mystery diff later.
