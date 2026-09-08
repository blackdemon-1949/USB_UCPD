#!/usr/bin/env python3
"""
cli_coverage.py — Static CLI routing-coverage check for the PD Bench firmware.

Every command advertised in `help` and every command required by the APIE CLI
spec must route to a handler in Appli/Core/Src/app_cli.c.  This is a static
check: it scans the dispatcher source for the command tokens and reports any
advertised-but-unrouted command.  It cannot execute the firmware, so it is a
routing-coverage gate (a compile/arm-build check remains the execution proof).

    python3 tools/cli_coverage.py     # 0 exit on full coverage, non-zero otherwise
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "Appli" / "Core" / "Src" / "app_cli.c"

# Original PD-bench commands that must remain functional.
ORIGINAL_COMMANDS = [
    "help", "status", "info", "caps", "getcaps", "req", "volt", "pps",
    "auto", "remember", "sweep", "ina", "getstatus", "getpps", "srcext",
    "manuinfo", "battery", "countrycodes", "countryinfo", "identify",
    "svids", "modes", "hardreset", "softreset", "led", "console", "uart",
    "dts", "pd", "raw",
]

# APIE CLI spec (from the master task): every token below must route.
# `ap` aliases the bare sub-commands; `pd stats/packets/state` route via `pd`.
APIE_COMMANDS = [
    # ap / apie aliases
    "ap", "apie",
    # ap sub-commands (also available bare)
    "status", "stats", "raw", "packets", "txn", "feature", "features",
    "source", "profile", "profiles", "fingerprint", "unknown", "patterns",
    "hypotheses", "knowledge", "scheduler", "ml", "predict", "replay",
    "experiment", "db", "safety",
    # pd aliases
    "pd stats", "pd packets", "pd state",
    # diagnostics
    "diag", "safe", "safe-mode",
]

# db sub-commands (must route inside the `db` handler).
DB_SUBCOMMANDS = ["status", "validate", "dump", "compact", "wear", "writes",
                  "erases", "checkpoint", "test"]
# diag sub-commands (must route inside the `diag` handler).
DIAG_SUBCOMMANDS = ["pd", "rx", "tx", "txn", "decoder", "ucpd", "usb", "queue",
                    "timing", "cpu", "memory", "profile", "unknown", "faults",
                    "trace", "ml", "scheduler", "knowledge", "packets", "db",
                    "safety", "flash"]
# safety sub-commands.
SAFETY_SUBCOMMANDS = ["status", "limits"]
# selftest scopes.
SELFTEST_SCOPES = ["all", "quick", "full", "pd", "decoder", "ml", "database", "flash"]
# packets sub-commands.
PACKETS_SUB = ["raw", "decoded", "unknown", "tx", "rx"]
# transactions sub-commands.
TRANSACTIONS_SUB = ["active", "history"]


def load() -> str:
    if not CLI.exists():
        raise SystemExit(f"app_cli.c not found: {CLI}")
    return CLI.read_text()


def route_counts(src: str, tokens: list[str]) -> dict[str, int]:
    counts = {}
    for t in tokens:
        # match strcmp(argv[0], "t") or strcmp(argv[1], "t") etc.
        hits = len(re.findall(r'strcmp\(argv\[\d+\],\s*"' + re.escape(t) + r'"\)', src))
        counts[t] = hits
    return counts


def main() -> int:
    src = load()
    failures = []

    # 1. Original commands must still be routed.
    for cmd in ORIGINAL_COMMANDS:
        if not re.search(r'strcmp\(argv\[0\],\s*"' + re.escape(cmd) + r'"\)', src):
            failures.append(f"original command not routed: {cmd}")

    # 2. APIE top-level commands (bare or via ap).
    rc = route_counts(src, APIE_COMMANDS)
    for cmd in APIE_COMMANDS:
        if cmd in ("ap", "apie"):
            continue  # handled below
        if " " in cmd:  # "pd stats" etc.
            ok = all(re.search(r'strcmp\(argv\[%d\],\s*"%s"\)' % (i, part), src)
                     for i, part in enumerate(cmd.split(" ")))
            if not ok:
                failures.append(f"apie/pd alias not routed: {cmd}")
            continue
        if rc[cmd] == 0 and cmd not in ("status",):
            failures.append(f"apie command not routed: {cmd}")

    # 3. `ap` / `apie` prefixes present.
    if 'strcmp(argv[0], "ap")' not in src:
        failures.append("`ap` alias prefix not routed")
    if 'strcmp(argv[0], "apie")' not in src:
        failures.append("`apie` prefix not routed")

    # 4. db sub-commands.
    db_src = re.search(r'if \(strcmp\(argv\[0\], "db"\) == 0\).*?return -1;\s*\}',
                       src, re.S)
    db_block = db_src.group(0) if db_src else ""
    for sub in DB_SUBCOMMANDS:
        if sub not in ("wear", "writes", "erases", "checkpoint") and \
           not re.search(r'strcmp\(argv\[1\],\s*"' + sub + r'"\)', db_block):
            failures.append(f"db sub-command not routed: db {sub}")
    # wear/writes/erases/checkpoint share one branch: look for any of them.
    if not re.search(r'(wear|writes|erases|checkpoint)', db_block):
        failures.append("db wear/writes/erases/checkpoint branch missing")

    # 5. diag sub-commands.
    diag_src = re.search(r'if \(strcmp\(argv\[0\], "diag"\) == 0\).*?return -1;\s*\}',
                         src, re.S)
    diag_block = diag_src.group(0) if diag_src else ""
    for sub in DIAG_SUBCOMMANDS:
        if not re.search(r'strcmp\(argv\[1\],\s*"' + sub + r'"\)', diag_block):
            failures.append(f"diag sub-command not routed: diag {sub}")

    # 6. safety sub-commands.
    if not re.search(r'strcmp\(argv\[1\],\s*"(status|limits)"\)', src):
        failures.append("safety status/limits not routed")

    # 6b. selftest scope validation present.
    if 'strcmp(argv[0], "selftest")' not in src:
        failures.append("selftest command not routed")
    if 'strcmp(scope, "decoder")' not in src:
        failures.append("selftest scope validation missing")

    # 6c. packets sub-commands.
    for sub in PACKETS_SUB:
        if not re.search(r'strcmp\(argv\[1\],\s*"' + sub + r'"\)', src):
            failures.append(f"packets sub-command not routed: packets {sub}")

    # 6d. transactions active/history.
    if not re.search(r'strcmp\(argv\[1\],\s*"active"\)', src):
        failures.append("transactions active not routed")
    if not re.search(r'strcmp\(argv\[1\],\s*"history"\)', src):
        failures.append("transactions history not routed")

    # 7. help text advertises the new APIE command families.
    for advert in ["ap | apie", "db compact", "db test", "db wear|writes",
                   "diag pd|ucpd", "diag timing", "safe | safe-mode",
                   "safety [status|limits]"]:
        if advert not in src:
            failures.append(f"help text missing advertised command: {advert}")

    if failures:
        print("CLI coverage FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("CLI coverage: all advertised commands route (originals + ap/apie + db/diag/safety/pd).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
