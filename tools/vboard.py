#!/usr/bin/env python3
"""
Virtual-board smoke test for the STM32H7R3 application image.

Loads the ELF, starts at the reset vector with the reset MSP, maps RAM/DTCM/
BKPSRAM/XSPI windows and a flat peripheral window, patches the "ready" bits of
the USART status register so the HAL's polling transmit can proceed, and then
emulates until either the super-loop is reached or something goes wrong.

Usage:  vboard.py <elf> [--max-insn N] [--stop-at-symbol NAME]

Outputs:  - whether reset -> main
          - every character the firmware wrote to the USART transmit register
          - the first unmapped / faulting access with a register dump
"""
import argparse
import sys
import unicorn
from unicorn import arm_const
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile

# ----------------------------------------------------------------- memory map
REGIONS = [
    (0x00000000, 0x00020000, "ITCM"),
    (0x08000000, 0x04000000, "system area: UID 0x08FFF800, option bytes"),
    (0x20000000, 0x00020000, "DTCM"),
    (0x24000000, 0x00080000, "AXI SRAM"),
    (0x30000000, 0x00010000, "SRAM AHB"),
    (0x38800000, 0x00002000, "BKPSRAM"),
    (0x40000000, 0x08000000, "peripherals APB1/2/3 + AHB1/2"),
    (0x50000000, 0x08000000, "peripherals AHB3/4"),
    (0x58000000, 0x00400000, "RCC/PWR/XSPI/DTS regs"),
    (0x5C000000, 0x00400000, "DBGMCU 0x5C001000 + friends"),
    (0x60000000, 0x00100000, "peripherals"),
    (0x70000000, 0x00010000, "XSPI2 window"),
    (0x90000000, 0x00800000, "XSPI1 XiP (flash image)"),
    (0xE0000000, 0x00020000, "system control"),
]

# USART registers we care about (H7RS: USART1 0x40007000, USART2 0x40004400)
# H7RS peripheral map: APB1 = 0x40000000 (USART2/3, UART4/5), APB2 = 0x42000000
# (USART1/6, UART9/10/12), APB4 = 0x58000000 (LPUART1).  NOT the classic-H7
# 0x40011000/0x40011400 addresses - getting this wrong stalls the PD tracer.
USART_BASES = [0x40004400, 0x40004800, 0x40004C00, 0x40005000,
               0x42001000, 0x42001400, 0x42002400, 0x42002800,
               0x58000C00]
USART_ISR_OFF = 0x1C       # ISR
USART_RDR_OFF = 0x24       # RDR
USART_TDR_OFF = 0x28       # TDR
USART_ICR_OFF = 0x20       # ICR
USART_ISR_TXE = 1 << 7
USART_ISR_TC = 1 << 6
USART_ISR_RXNE = 1 << 5
USART_ISR_TEACK = 1 << 21     # transmitter enabled (HAL waits for it after init)
USART_ISR_REACK = 1 << 22     # receiver enabled


class Board:
    def __init__(self, elf_path, verbose=False):
        self.verbose = verbose
        self.out = []
        self.tx_writes = 0
        self.trace = []
        self.fault = None
        self.seen = set()
        self.reg_reads = {}
        self.isr_samples = []
        self.reg_writes = {}
        import collections
        self.history = collections.deque(maxlen=24)
        self.elf_path = elf_path
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_MCLASS | unicorn.UC_MODE_THUMB)
        self._map_regions()
        self._load_elf()
        self._install_hooks()

    def _map_regions(self):
        for base, size, name in REGIONS:
            try:
                self.uc.mem_map(base, size, unicorn.UC_PROT_ALL)
            except unicorn.UcError as exc:      # already mapped or overlapping
                if self.verbose:
                    print(f"  (skip map {name}: {exc})")

    def _load_elf(self):
        with open(self.elf_path, "rb") as fh:
            elf = ELFFile(fh)
            self.entry = elf.header["e_entry"]
            self.symbols = {}
            for secname in (".symtab", ".dynsym"):
                sec = elf.get_section_by_name(secname)
                if sec is None:
                    continue
                for sym in sec.iter_symbols():
                    if sym.name and sym["st_value"]:
                        self.symbols.setdefault(sym.name, sym["st_value"])
            for seg in elf.iter_segments():
                if seg["p_type"] != "PT_LOAD":
                    continue
                data = seg.data()
                addr = seg["p_paddr"]
                if seg["p_filesz"] == 0 or len(data) == 0:
                    continue
                # map a little around each segment in case it is not covered
                page = addr & ~0xFFF
                end = (addr + len(data) + 0xFFF) & ~0xFFF
                try:
                    self.uc.mem_map(page, max(end - page, 0x1000), unicorn.UC_PROT_ALL)
                except unicorn.UcError:
                    pass
                self.uc.mem_write(addr, data)
            # initial state straight from the vector table
            vtab = self.uc.mem_read(0x90000000, 8)
            self.msp = int.from_bytes(vtab[0:4], "little")
            reset = int.from_bytes(vtab[4:8], "little")
            self.reset_vector = reset
            # BKPSRAM: erased state (all ones reads as 0xFFFFFFFF)
            self.uc.mem_write(0x38800000, b"\xff" * 0x1000)

    def _install_hooks(self):
        self.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._hook_write)
        self.uc.hook_add(unicorn.UC_HOOK_MEM_READ, self._hook_read)
        self.uc.hook_add(unicorn.UC_HOOK_MEM_UNMAPPED, self._hook_unmapped)
        self.uc.hook_add(unicorn.UC_HOOK_INTR, self._hook_intr)

    # ------------------------------------------------------------- hooks
    def _hook_read(self, uc, access, address, size, value, user):
        # USB OTG global registers: report "AHB master idle", otherwise the HAL
        # reset handshake (USB_CoreReset) polls a bit that a flat memory model
        # can never set.
        # USB_OTG_HS = AHB1PERIPH_BASE + 0x20000 on H7RS; 0x42000000 is APB2 (USART1!)
        for otg in (0x40040000, 0x40080000):
            if otg <= address < otg + 0x1000:
                if (address - otg) == 0x10:                       # GRSTCTL
                    uc.mem_write(address, (0x80000000).to_bytes(4, "little"))
                elif (address - otg) == 0x14:                     # GINTSTS
                    # Read-mostly: CMOD (bit 0) mirrors the current mode.  The
                    # HAL's USB_SetCurrentMode() polls it for up to a second and
                    # gives up with HAL_ERROR if it cannot see device mode, and
                    # USB_StopDevice() leaves stale flag bits behind in a flat
                    # memory model.  Report "device mode, no flags pending".
                    uc.mem_write(address, (0x00000000).to_bytes(4, "little"))
        if 0x40000000 <= address < 0x40040000:
            self.reg_reads[address] = self.reg_reads.get(address, 0) + 1
        for base in USART_BASES:
            if base <= address < base + 0x400:
                off = address - base
                if off == USART_ISR_OFF:
                    # report the transmit/receive ready bits the HAL polls for
                    uc.mem_write(address, (USART_ISR_TXE | USART_ISR_TC | USART_ISR_TEACK | USART_ISR_REACK).to_bytes(4, "little"))
                    if len(self.isr_samples) < 6:
                        self.isr_samples.append((uc.reg_read(UC_ARM_REG_PC),
                                                 uc.reg_read(UC_ARM_REG_LR)))
        return False      # let the load itself execute against the patched value

    def _hook_write(self, uc, access, address, size, value, user):
        if 0x40000000 <= address < 0x40040000:
            self.reg_writes[address] = self.reg_writes.get(address, 0) + 1
        for base in USART_BASES:
            if base <= address < base + 0x400:
                off = address - base
                if off == USART_ISR_OFF:
                    return True
                if off == USART_TDR_OFF:
                    self.tx_writes += 1
                    self.out.append(chr(value & 0xFF) if 32 <= (value & 0xFF) < 127 else
                                    {10: "\n", 13: "", 9: "\t"}.get(value & 0xFF, f"<{value & 0xFF:02x}>"))
        return True

    def _hook_unmapped(self, uc, access, address, size, value, user):
        self.fault = ("unmapped", access, address, size)
        return False

    def _hook_intr(self, uc, intno, user):
        # SVC/HardFault etc.: note it and stop
        self.fault = ("exception", intno, uc.reg_read(UC_ARM_REG_PC), 0)
        return False

    # ------------------------------------------------------------- run
    def run(self, max_insn=4_000_000, stop_sym="main", trace_insn=0, tick_every=200):
        # virtual SysTick: bump the firmware's own uwTick variable, so every
        # HAL timeout in the image behaves like it does on a real board
        uw_tick = self.symbols.get("uwTick")
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, self.msp)
        pc = self.reset_vector          # keep the Thumb bit: M-profile PC LSB selects Thumb
        uc.reg_write(UC_ARM_REG_PC, pc)
        uc.reg_write(UC_ARM_REG_XPSR, 0x01000000)
        stop_addr = self.symbols.get(stop_sym)
        reached_main = False
        start_pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"reset vector      : 0x{self.reset_vector:08X}  (MSP 0x{self.msp:08X})")
        print(f"reset handler     : 0x{start_pc:08X}")
        stop_even = (stop_addr & ~1) if stop_addr else None
        if stop_addr:
            print(f"{stop_sym:<18}: 0x{stop_addr:08X}")
        count = 0
        while count < max_insn:
            try:
                # the LSB of the start address tells Unicorn this is Thumb code
                uc.emu_start(uc.reg_read(UC_ARM_REG_PC) | 1, 0, count=1)
            except unicorn.UcError as exc:
                print(f"!! stopped after {count} instructions: {exc}")
                break
            count += 1
            self.history.append((count, uc.reg_read(UC_ARM_REG_PC)))
            if uw_tick and (count % tick_every) == 0:
                val = int.from_bytes(uc.mem_read(uw_tick, 4), "little")
                uc.mem_write(uw_tick, ((val + 1) & 0xFFFFFFFF).to_bytes(4, "little"))
            cur = uc.reg_read(UC_ARM_REG_PC)
            for watch in ("Appli_Fail", "Error_Handler", "Appli_Fatal",
                          "HardFault_Handler", "MemManage_Handler", "BusFault_Handler"):
                wa = self.symbols.get(watch)
                if wa and cur == (wa & ~1) and watch not in self.seen:
                    self.seen.add(watch)
                    lr = uc.reg_read(UC_ARM_REG_LR)
                    r0 = uc.reg_read(UC_ARM_REG_R0)
                    sp = uc.reg_read(UC_ARM_REG_SP)
                    print(f"[watch] {watch} entered at insn {count}: r0=0x{r0:X} "
                          f"caller={self.resolve((lr & ~1) - 1)} (lr=0x{lr:08X}) sp=0x{sp:08X}")
                    print("        backtrace (innermost first):")
                    for c2, p2 in list(self.history)[::-1][:14]:
                        print(f"          #{c2:<8} 0x{p2:08X} {self.resolve(p2)}")
            if stop_even and cur == stop_even:
                reached_main = True
                print(f"--> reached {stop_sym} after {count} instructions")
                break
            if trace_insn and count <= trace_insn:
                self.trace.append(cur)
            if count % 20000 == 0:
                print(f"    ... {count:>8} insn  PC=0x{cur:08X} {self.resolve(cur)}  SP=0x{uc.reg_read(UC_ARM_REG_SP):08X}")
        else:
            print(f"!! instruction budget exhausted at PC=0x{uc.reg_read(UC_ARM_REG_PC):08X}")
        return reached_main, count

    def resolve(self, addr):
        """nearest preceding function symbol"""
        best = None
        for name, val in self.symbols.items():
            if val <= addr and (best is None or val > best[1]):
                best = (name, val)
        if best is None:
            return "?"
        return f"{best[0]}+0x{addr - best[1]:X}"

    def dump_isr(self):
        if self.isr_samples:
            print("first ISR polls:")
            for pc, lr in self.isr_samples:
                print(f"   pc=0x{pc:08X} {self.resolve(pc)}   lr=0x{lr:08X} {self.resolve((lr & ~1) - 1)}")

    def dump_regs(self, n=8):
        print("most-read peripheral words:")
        for a, c in sorted(self.reg_reads.items(), key=lambda kv: -kv[1])[:n]:
            print(f"   0x{a:08X}  {c} reads")
        print("most-written peripheral words:")
        for a, c in sorted(self.reg_writes.items(), key=lambda kv: -kv[1])[:n]:
            print(f"   0x{a:08X}  {c} writes")

    def dump_state(self):
        uc = self.uc
        print(f"PC=0x{uc.reg_read(UC_ARM_REG_PC):08X} LR=0x{uc.reg_read(UC_ARM_REG_LR):08X} "
              f"SP=0x{uc.reg_read(UC_ARM_REG_SP):08X} xPSR=0x{uc.reg_read(UC_ARM_REG_XPSR):08X}")
        if self.fault:
            print("fault:", self.fault)
        text = "".join(self.out)
        print(f"USART bytes written: {self.tx_writes}")
        if text.strip():
            print("---- serial output ----")
            print(text[:4000])
            print("-----------------------")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--max-insn", type=int, default=4_000_000)
    ap.add_argument("--stop-at", default="main")
    ap.add_argument("--tick", type=int, default=200)
    ap.add_argument("--run-loop", action="store_true",
                    help="keep going past main for the given number of instructions")
    args = ap.parse_args()

    board = Board(args.elf, verbose=True)
    ok, count = board.run(args.max_insn, stop_sym=None if args.run_loop else args.stop_at, tick_every=args.tick)
    board.dump_state()
    board.dump_regs()
    board.dump_isr()
    print("reached:", ok, "instructions:", count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
