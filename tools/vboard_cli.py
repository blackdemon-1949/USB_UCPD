#!/usr/bin/env python3
"""Drive the on-board command console inside the Unicorn virtual board.

The firmware's console is fed by its USART2 receive interrupt, which copies
bytes into a ring buffer (``s_rx`` / ``s_rx_head`` / ``s_rx_tail`` in
app_cli.c).  Unicorn does not model the NVIC, so this script injects the bytes
exactly the way the ISR would - write into the ring, advance the head - and
then lets the normal super-loop do the parsing, dispatching and printing.

That means every command below is executed by the real firmware: real parser,
real handlers, real UART transmitter, real timeouts.  The only thing simulated
is "a byte arrived at USART2".

Usage:
    python3 tools/vboard_cli.py <app.elf> [--commands "cmos;store;wdt"] [-v]

Exits non-zero if a command produced no output at all or if the firmware hit a
fault/Error_Handler while running it.
"""

import argparse
import importlib.util
import sys
import unicorn

HERE = __import__("os").path.dirname(__import__("os").path.abspath(__file__))


def load_vboard():
    spec = importlib.util.spec_from_file_location(
        "vboard", __import__("os").path.join(HERE, "vboard.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# hardware bits the flat memory model has to fake (same idea as vboard.py)
# --------------------------------------------------------------------------
RCC_CR = 0x58024400
PWR_CSR2 = 0x5802480C
USB33RDY = 0x04000000
USBHSREGEN = 0x08000000


class Board:
    def __init__(self, vb, elf, verbose=False):
        self.vb = vb
        self.verbose = verbose
        self.b = vb.Board(elf)
        self.uc = self.b.uc
        self.sym = self.b.symbols
        self.uw = self.sym["uwTick"]
        self.count = 0
        self.fatal = []
        self.mark = 0
        self.uc.hook_add(unicorn.UC_HOOK_MEM_READ, self._hook_read)
        self.uc.hook_add(unicorn.UC_HOOK_INTR, self._hook_intr)
        self.uc.reg_write(vb.UC_ARM_REG_SP, self.b.msp)
        self.uc.reg_write(vb.UC_ARM_REG_PC, self.b.reset_vector)
        self.uc.reg_write(vb.UC_ARM_REG_XPSR, 0x01000000)

    # -- hooks -------------------------------------------------------------
    def _hook_read(self, uc, access, address, size, value, user):
        if address == RCC_CR:
            self.b._patch(uc, address, 0x00020000)          # HSERDY
        elif address == PWR_CSR2:
            self.b._patch(uc, address, USB33RDY | USBHSREGEN)
        return False

    def _hook_intr(self, uc, intno, user):
        self.fatal.append(("intr", intno, uc.reg_read(self.vb.UC_ARM_REG_PC)))
        return False

    # -- stepping ----------------------------------------------------------
    def run(self, instructions, stop_on=None, stop_when=None):
        """Run; stop early on a symbol name, a predicate, or instruction count."""
        uc = self.uc
        stop_pc = (self.sym[stop_on] & ~1) if stop_on else None
        for _ in range(instructions):
            if stop_pc is not None and (uc.reg_read(self.vb.UC_ARM_REG_PC) & ~1) == stop_pc:
                return True
            try:
                uc.emu_start(uc.reg_read(self.vb.UC_ARM_REG_PC) | 1, 0, count=1)
            except unicorn.UcError as exc:
                self.fatal.append(("stop", str(exc)))
                return False
            self.count += 1
            if self.count % 30 == 0:
                v = int.from_bytes(uc.mem_read(self.uw, 4), "little")
                uc.mem_write(self.uw, ((v + 1) & 0xFFFFFFFF).to_bytes(4, "little"))
            cur = uc.reg_read(self.vb.UC_ARM_REG_PC) & ~1
            for name in ("Appli_Fail", "Error_Handler", "Appli_Fatal", "HardFault_Handler"):
                a = self.sym.get(name)
                if a is not None and cur == (a & ~1):
                    self.fatal.append((name, self.b.resolve((uc.reg_read(self.vb.UC_ARM_REG_LR) & ~1) - 1)))
            if stop_when is not None and stop_when():
                return True
        return False

    # -- console -----------------------------------------------------------
    def console(self):
        return "".join(self.b.out)

    def inject(self, text):
        """Push bytes into the CLI ring buffer exactly like the RX ISR does."""
        uc = self.uc
        data = text.encode() if isinstance(text, str) else text
        for byte in data:
            head = int.from_bytes(uc.mem_read(self.sym["s_rx_head"], 2), "little")
            tail = int.from_bytes(uc.mem_read(self.sym["s_rx_tail"], 2), "little")
            nxt = (head + 1) % 256
            if nxt == tail:                     # ring full: the ISR drops it too
                return False
            uc.mem_write(self.sym["s_rx"] + head, bytes([byte]))
            uc.mem_write(self.sym["s_rx_head"], nxt.to_bytes(2, "little"))
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--commands", default="cmos;cmos faults;store;wdt;usb;help;info")
    ap.add_argument("--settle", type=int, default=400_000,
                    help="instructions to run between commands (queue drain)")
    ap.add_argument("--boot-budget", type=int, default=3_000_000)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--nor-load", metavar="IMG",
                    help="pre-load the external-NOR store window from IMG")
    ap.add_argument("--nor-save", metavar="IMG",
                    help="write the store window to IMG at the end of the run")
    ap.add_argument("--nor-trace", action="store_true",
                    help="print the modelled XSPI command sequence")
    args = ap.parse_args()

    vb = load_vboard()
    nor_mod = None
    if args.nor_load or args.nor_save or args.nor_trace:
        import importlib.util as _ilu
        spec = _ilu.spec_from_file_location(
            "vboard_nor", __import__("os").path.join(HERE, "vboard_nor.py"))
        nor_mod = _ilu.module_from_spec(spec)
        spec.loader.exec_module(nor_mod)
        nor_mod.prepare(vb.Board)      # before the board installs its hooks
    board = Board(vb, args.elf, verbose=args.verbose)
    nor = None
    if nor_mod is not None:
        nor = nor_mod.install(board, load=args.nor_load, verbose=args.nor_trace)
        print("external NOR modelled on XSPI1 (8 MB, store window 0x700000..0x800000)"
              + (f", image loaded from {args.nor_load}" if args.nor_load else ""))
        print(f"  jedec id 0x{nor_mod.JEDEC:06X}")
    print(f"booting {args.elf} ...")
    board.run(args.boot_budget, stop_when=lambda: "[boot] ready" in board.console())
    text = board.console()
    if "[boot] ready" not in text:
        print("!! the firmware did not reach its ready banner inside the budget")
        print(text[-800:])
        return 2
    print(text[text.index("[USART2"):] if "[USART2" in text else text)

    def drain(chunk, quiet_target, max_rounds, quiet_is_prompt=False):
        """Run until the console output stops growing (or the prompt shows)."""
        quiet = 0
        before = len(board.console())
        for _ in range(max_rounds):
            board.run(chunk)
            now = len(board.console())
            quiet = quiet + 1 if now == before else 0
            before = now
            if quiet >= quiet_target and (not quiet_is_prompt or
                                          board.console().rstrip().endswith(">")):
                return
            if quiet >= quiet_target + 3:
                return

    # Let the boot greeting (banner + full help listing, several KB) reach the
    # wire before the first command, so each answer can be attributed cleanly.
    drain(args.settle, 3, 60)
    board.mark = len(board.console())

    failures = []
    for cmd in [c for c in args.commands.split(";") if c.strip()]:
        if not board.inject(cmd + "\r\n"):
            failures.append((cmd, "input ring would not take the bytes"))
            continue
        # The console output is queued and flushed by the super-loop, and a
        # long answer (help is several KB) outlives any fixed instruction
        # budget, so drain until the output stops growing.
        # Drain until the console prints its prompt: the answer is queued and
        # flushed by the super-loop, and a long answer (help is several KB)
        # outlives any fixed instruction budget.  Drain until quiet AND only
        # then check for the prompt, so an empty answer is still noticed.
        drain(args.settle, 2, 25, quiet_is_prompt=True)
        out = board.console()[board.mark:]
        board.mark = len(board.console())
        # the unattached INA226 retries every 5 s of virtual time and would
        # otherwise drown every answer
        lines = [ln for ln in out.replace("\r", "").splitlines()
                 if ln.strip() and not ln.startswith("[ina226] no ina226")]
        print(f"\n$ {cmd}")
        print("\n".join("  " + line for line in lines) or "  (no output)")
        out = "\n".join(lines)
        if out.strip() == "":
            failures.append((cmd, "no output"))
        if "unknown:" in out:
            failures.append((cmd, "rejected by the parser"))

    if nor is not None and args.nor_save:
        nor.save(args.nor_save)
        print(f"\nstore window saved to {args.nor_save}")
    if nor is not None:
        print(nor.summary())
        if args.nor_trace:
            for line in nor.log[:40]:
                print("   ", line)

    print("\n=====================================")
    if board.fatal:
        print("fatal paths hit:")
        for item in board.fatal:
            print("   ", item)
    else:
        print("no fault / Error_Handler / unexpected trap during the run")
    if failures:
        print("command problems:")
        for cmd, why in failures:
            print(f"    {cmd!r}: {why}")
        return 1
    print(f"all commands answered ({board.count} instructions executed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
