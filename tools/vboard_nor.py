#!/usr/bin/env python3
"""
XSPI1 + NOR flash model for the virtual board (tools/vboard_cli.py).

Why this exists
---------------
Everything the application does to the external flash - probing the JEDEC id,
the store self test (erase + program + read back), writing records and reading
them again - happens with the XSPI out of memory-mapped mode, executed from the
ITCM-resident .ramfunc driver.  On a flat memory model that is invisible: the
registers read back as whatever was last written, so the driver would either
spin forever or take an error path.  This model implements the indirect-mode
protocol the driver uses, so the whole store path can be exercised:

  * CR / SR / DLR / AR / DR / CCR / TCR / IR / ABR / LPTR registers
  * CR.ABORT self-clearing, FMODE = memory-mapped at reset (Boot leaves it so)
  * SR.TCF / SR.FLEVEL / SR.BUSY
  * command set: 0x9F RDID, 0x05 RDSR, 0x06 WREN, 0x04 WRDI, 0x03/0x0B read,
    0x02 page program, 0x20 4 KB erase, 0xD8 64 KB erase, 0xC7 chip erase
  * DWT->CYCCNT advances on every read, so the driver's cycle-based timeouts
    still expire instead of looping forever (a real stall then shows up as a
    timeout error path rather than an emulator hang).

What it does NOT prove: the model was written from the driver's own register
sequence, so it validates the software (no dead loop, bounds/sector arithmetic,
record persistence, error latching) - not the chip's real timing, status
semantics or electrical behaviour.  Those still need the bench.

Usage (usually through vboard_cli.py):
    python3 tools/vboard_cli.py <elf> --nor-load img.bin --nor-save img.bin ...
"""
import struct

XSPI1_REG_BASE = 0x52005000        # AHB5PERIPH_BASE (0x52000000) + 0x5000
XSPI1_REG_SIZE = 0x400

NOR_BASE = 0x90000000              # XSPI1 memory-mapped window
NOR_SIZE = 8 * 1024 * 1024         # PY25Q64HA
STORE_OFF = 0x700000               # EXT_NOR_STORE_OFF
STORE_SIZE = 0x100000              # EXT_NOR_STORE_SIZE
SECTOR = 0x1000

JEDEC = 0x856017                   # 0x85 Puya, 0x60 SPI NOR, 0x17 = 8 MB

OFF = dict(CR=0x00, DCR1=0x08, DCR2=0x0C, DCR3=0x10, DCR4=0x14,
           SR=0x20, FCR=0x24, DLR=0x40, AR=0x48, DR=0x50,
           CCR=0x100, TCR=0x108, IR=0x110, ABR=0x120, LPTR=0x130)
REG_BY_OFF = {v: k for k, v in OFF.items()}

CR_ABORT = 1 << 1
CR_FMODE_SHIFT = 28
SR_TCF = 1 << 1
SR_BUSY = 1 << 5
SR_FLEVEL_SHIFT = 8

DWT_CYCCNT = 0xE0001004


class Nor:
    """XSPI1 controller + one NOR device (its own bus master)."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.cycles = 0
        self.published = {}
        self.mem = bytearray(b"\xff" * NOR_SIZE)
        self.reg = {name: 0 for name in OFF}
        # Boot left the controller in memory-mapped mode with the read command
        # programmed; EXT_NOR_Init() refuses to touch the device otherwise.
        self.reg["CR"] = (3 << CR_FMODE_SHIFT) | 1
        self.reg["DCR1"] = 0x00000100
        self.tcf = False
        self.fifo = bytearray()
        self.cursor = 0
        self.remaining = 0
        self.wr = bytearray()
        self.expect = 0
        self.wel = False
        self.ir_last = 0
        self.stats = dict(cmd=0, read=0, program=0, erase=0, ids=0, unknown=0)
        self.reg_access = {}
        self.log = []

    # ---------------------------------------------------------------- hooks
    def on_read(self, uc, address):
        """Called for every load; patches XSPI registers and the DWT counter."""
        if XSPI1_REG_BASE <= address < XSPI1_REG_BASE + XSPI1_REG_SIZE:
            self.reg_read(uc, address)
        elif NOR_BASE + STORE_OFF <= address < NOR_BASE + STORE_OFF + STORE_SIZE:
            self.window_read(uc, address)
        elif address == DWT_CYCCNT:
            self.cycles = (self.cycles + 512) & 0xFFFFFFFF
            uc.mem_write(address, struct.pack("<I", self.cycles))

    def on_write(self, uc, address, size, value):
        """Called for every store; runs the modelled XSPI command state machine."""
        if XSPI1_REG_BASE <= address < XSPI1_REG_BASE + XSPI1_REG_SIZE:
            # Returning True skips the guest store, so the model owns the
            # register file and publishes values on reads instead.
            self.reg_write(uc, address, value)
            self.published[address] = value & 0xFFFFFFFF
            return True
        if NOR_BASE + STORE_OFF <= address < NOR_BASE + STORE_OFF + STORE_SIZE:
            off = address - (NOR_BASE + STORE_OFF)
            data = value.to_bytes(size, "little") if size else b""
            self.mem[STORE_OFF + off:STORE_OFF + off + len(data)] = data
        return False

    # ---------------------------------------------------------------- window
    def window_read(self, uc, addr):
        """Serve a memory-mapped load inside the store window from the array."""
        off = addr - (NOR_BASE + STORE_OFF)
        if 0 <= off <= STORE_SIZE - 4:
            uc.mem_write(addr, bytes(self.mem[STORE_OFF + off:STORE_OFF + off + 4]))

    # ------------------------------------------------------------ registers
    def _sr_value(self):
        lvl = len(self.fifo)
        if self.remaining:
            lvl = min(self.remaining, 32)
        sr = (min(lvl, 0x7F) << SR_FLEVEL_SHIFT)
        if self.tcf:
            sr |= SR_TCF
        if self.expect and self.wr is not None and len(self.wr) < self.expect and self.ir_last == 0x02:
            sr |= SR_BUSY
        return sr

    def _publish(self, uc, addr, val):
        """Mirror a value into guest memory, but only when it changed.

        Every uc.mem_write from a hook invalidates Unicorn's translation
        cache, and the driver polls these registers in tight loops, so writing
        the same value back would dominate the run time.
        """
        val &= 0xFFFFFFFF
        if self.published.get(addr) == val:
            return
        self.published[addr] = val
        uc.mem_write(addr, struct.pack("<I", val))

    def reg_read(self, uc, addr):
        name = REG_BY_OFF.get(addr - XSPI1_REG_BASE)
        if name:
            self.reg_access['r:' + name] = self.reg_access.get('r:' + name, 0) + 1
        if name is None:
            return
        if name == "SR":
            val = self._sr_value()
        elif name == "CR":
            val = self.reg["CR"] & ~CR_ABORT
        elif name == "DR":
            val = self._read_dr()
        else:
            val = self.reg[name]
        self._publish(uc, addr, val)

    def reg_write(self, uc, addr, value):
        name = REG_BY_OFF.get(addr - XSPI1_REG_BASE)
        if name:
            self.reg_access['w:' + name] = self.reg_access.get('w:' + name, 0) + 1
        if name is None:
            return
        if name == "SR":
            if value & SR_TCF:
                self.tcf = False
            return
        if name == "FCR":
            return
        if name == "DR":
            self._write_dr(value & 0xFF)
            return
        if name == "AR":
            self.reg["AR"] = value
            if ((self.reg["CR"] >> CR_FMODE_SHIFT) & 3) != 3:
                self._start_command()      # indirect mode: a real transaction
            return
        if name == "CR":
            # ABORT is self-clearing once the controller has finished the
            # current transfer; the driver polls until it reads back 0.
            self.reg["CR"] = value & ~CR_ABORT
            if ((value >> CR_FMODE_SHIFT) & 3) == 3:
                self.wel = False           # back in memory-mapped mode
            return
        self.reg[name] = value

    # -------------------------------------------------------------- commands
    def _start_command(self):
        ir = self.reg["IR"] & 0xFF
        addr = self.reg["AR"]
        self.ir_last = ir
        self.stats["cmd"] += 1
        if self.verbose:
            line = f"  [nor] IR=0x{ir:02X} AR=0x{addr:06X} DLR={self.reg['DLR']}"
            self.log.append(line)
            if len(self.log) < 60:
                import sys as _sys
                print(line, file=_sys.stderr, flush=True)
        n = self.reg["DLR"] + 1
        self.fifo = bytearray()
        self.wr = bytearray()
        self.cursor = addr
        self.remaining = 0
        self.expect = 0
        self.tcf = False

        if ir == 0x05:                                  # RDSR
            # bit0 = WIP (always ready here), bit1 = WEL, bit7 = SRP0.
            # The driver checks WEL after every WREN, so a status register
            # that always reads 0 would make every write look refused.
            status = 0x02 if self.wel else 0x00
            self.fifo = bytearray([status])
            self.tcf = True
        elif ir == 0x06:                                # WREN
            self.wel = True
            self.tcf = True
        elif ir == 0x04:                                # WRDI
            self.wel = False
            self.tcf = True
        elif ir == 0x9F:                                # RDID
            self.stats["ids"] += 1
            self.fifo = bytearray([(JEDEC >> 16) & 0xFF, (JEDEC >> 8) & 0xFF,
                                   JEDEC & 0xFF, 0x00])
            self.tcf = True
        elif ir in (0x03, 0x0B, 0x13, 0x0C):            # read / fast read
            self.stats["read"] += 1
            self.remaining = n
        elif ir == 0x02:                                # page program
            self.expect = n
        elif ir in (0x20, 0x21):                        # 4 KB sector erase
            self._erase(addr & ~(SECTOR - 1), SECTOR)
        elif ir in (0xD8, 0xDC):                        # 64 KB block erase
            self._erase(addr & ~0xFFFF, 0x10000)
        elif ir in (0xC7, 0x60):                        # chip erase
            self._erase(STORE_OFF, STORE_SIZE)
        else:
            self.stats["unknown"] += 1
            self.tcf = True

    def _erase(self, addr, size):
        self.stats["erase"] += 1
        lo = max(0, min(addr, NOR_SIZE))
        hi = max(0, min(addr + size, NOR_SIZE))
        self.mem[lo:hi] = b"\xff" * (hi - lo)
        self.tcf = True

    def _read_dr(self):
        if self.remaining:
            v = self.mem[self.cursor % NOR_SIZE] if self.cursor < NOR_SIZE else 0xFF
            self.cursor += 1
            self.remaining -= 1
            if self.remaining == 0:
                self.tcf = True
            return v
        if self.fifo:
            v = self.fifo[0]
            del self.fifo[0]
            return v
        return 0x00

    def _write_dr(self, byte):
        if self.expect:
            self.wr.append(byte)
            if len(self.wr) >= self.expect:
                self._program(self.cursor, bytes(self.wr))
                self.expect = 0
                self.wel = False
                self.tcf = True

    def _program(self, addr, data):
        self.stats["program"] += 1
        for i, b in enumerate(data):
            p = addr + i
            if p < NOR_SIZE:
                self.mem[p] &= b          # NOR programming can only clear bits

    # ------------------------------------------------------------ image I/O
    def save(self, path):
        with open(path, "wb") as fh:
            fh.write(bytes(self.mem[STORE_OFF:STORE_OFF + STORE_SIZE]))

    def load(self, path):
        with open(path, "rb") as fh:
            data = fh.read(STORE_SIZE)
        self.mem[STORE_OFF:STORE_OFF + len(data)] = data

    def summary(self):
        s = self.stats
        acc = ", ".join(f"{k}={v}" for k, v in sorted(self.reg_access.items()))
        return (f"nor model: {s['cmd']} commands, {s['ids']} id, {s['read']} reads, "
                f"{s['program']} programs, {s['erase']} erases, "
                f"{s['unknown']} unknown\n  register access: {acc or '(none)'}")


def prepare(board_class):
    """Make room for the model's hooks *before* the base board's own hooks.

    tools/vboard.py installs a write hook that returns True for every store
    (it only counts USART traffic).  Unicorn stops the hook chain at the first
    hook that returns True, so a model hook added afterwards would never see
    the XSPI register writes and the modelled device would look dead.  Wrapping
    the class methods before the board is constructed puts the model first.
    """
    if getattr(board_class, "_nor_wrapped", False):
        return
    orig_read = board_class._hook_read
    orig_write = board_class._hook_write

    def _hook_read(self, uc, access, address, size, value, user):
        nor = getattr(self, "nor", None)
        if nor is not None:
            nor.on_read(uc, address)
        return orig_read(self, uc, access, address, size, value, user)

    def _hook_write(self, uc, access, address, size, value, user):
        nor = getattr(self, "nor", None)
        if nor is not None and nor.on_write(uc, address, size, value):
            return True
        return orig_write(self, uc, access, address, size, value, user)

    board_class._hook_read = _hook_read
    board_class._hook_write = _hook_write
    board_class._nor_wrapped = True


def install(cli_board, load=None, save=None, verbose=False):
    """Attach the model to a tools/vboard_cli.Board instance."""
    nor = Nor(verbose=verbose)
    if load:
        nor.load(load)
    if save:
        cli_board.nor_save = lambda: nor.save(save)
    # The wrapped hooks belong to tools/vboard.py's Board, which the console
    # driver instantiates as an inner object - both need to see the model.
    cli_board.nor = nor
    inner = getattr(cli_board, "b", None)
    if inner is not None:
        inner.nor = nor
    return nor
