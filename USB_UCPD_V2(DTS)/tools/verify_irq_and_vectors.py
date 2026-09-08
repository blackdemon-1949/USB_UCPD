#!/usr/bin/env python3
"""Verify the vector table and the compiled NVIC priorities of the linked image.

1. Parses the linked Appli ELF, dumps the exception/IRQ vector table and
   resolves each slot to a handler symbol.
2. Pre-processes the real NVIC call sites with the project's own ARM flags so
   every IRQ_PRIO_* / TRACER_EMB_* macro is resolved to the literal value the
   compiler actually emits, and prints the resulting priority table.

Usage:  python3 tools/check_arm_build.py     # produce the ELF
        python3 tools/verify_irq_and_vectors.py
"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

ELF = "/tmp/armbuild/Appli/Appli.elf"
ZIG = [sys.executable, "-m", "ziglang", "cc"]
TARGET = ["-target", "arm-freestanding-eabihf", "-mcpu=cortex_m7"]

APPLI_INC = ["Appli/Core/Inc", "Appli/USBPD/App", "Appli/USBPD/Target",
             "Appli/USB_DEVICE/App", "Appli/USB_DEVICE/Target",
             "Drivers/STM32H7RSxx_HAL_Driver/Inc",
             "Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy",
             "Middlewares/ST/STM32_USBPD_Library/Core/inc",
             "Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc",
             "Middlewares/ST/STM32_USB_Device_Library/Core/Inc",
             "Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc",
             "Drivers/CMSIS/Device/ST/STM32H7RSxx/Include", "Drivers/CMSIS/Include"]
APPLI_DEF = ["-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32H7R3xx", "-DUSE_FULL_LL_DRIVER",
             "-DUSBPD_PORT_COUNT=1", "-D_SNK", "-D_TRACE", "-DUSBPDCORE_LIB_PD3_FULL"]

# IRQ numbers from stm32h7r3xx.h (IRQn + 16 = vector-table slot)
IRQS = {39: "GPDMA1_Channel0", 40: "GPDMA1_Channel1", 41: "GPDMA1_Channel2",
        82: "USART1", 83: "USART2", 91: "OTG_HS", 128: "UCPD1"}
EXC = ["initial MSP", "Reset", "NMI", "HardFault", "MemManage", "BusFault",
       "UsageFault", "Res", "Res", "Res", "Res", "SVC", "DebugMon", "Res",
       "PendSV", "SysTick"]

# Same call sites as the firmware; the pre-processor resolves the macros.
PROBE = """#include "irq_priority.h"
#include "tracer_emb_conf.h"
#include "usbpd_devices_conf.h"
void __prio_probe(void)
{
  HAL_NVIC_SetPriorityGrouping(IRQ_PRIORITY_GROUP);
  UCPD_INSTANCE0_ENABLEIRQ;
  NVIC_SetPriority(TRACER_EMB_TX_DMA_IRQ, TRACER_EMB_TX_DMA_PRIORITY);
  NVIC_SetPriority(TRACER_EMB_USART_IRQ, TRACER_EMB_TX_IRQ_PRIORITY);
}
"""


def elf_sections(path):
    f = open(path, "rb").read()
    assert f[:4] == b"\x7fELF", "not an ELF"
    (e_shoff, ) = struct.unpack_from("<I", f, 0x20)
    (e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHH", f, 0x2E)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (name, styp, flags, addr, off, size, link, info, align,
         entsize) = struct.unpack_from("<10I", f, o)
        secs.append(dict(name=name, type=styp, addr=addr, off=off, size=size,
                         link=link, entsize=entsize))
    shstr = secs[e_shstrndx]
    for s in secs:
        end = f.index(b"\0", shstr["off"] + s["name"])
        s["sname"] = f[shstr["off"] + s["name"]:end].decode()
    return f, secs


def symbols(f, secs):
    syms = {}
    for s in secs:
        if s["type"] != 2:            # SHT_SYMTAB
            continue
        strtab = secs[s["link"]]
        for i in range(s["size"] // 16):
            o = s["off"] + i * 16
            nm, val, sz, info, other, shndx = struct.unpack_from("<IIIBBH", f, o)
            end = f.index(b"\0", strtab["off"] + nm)
            name = f[strtab["off"] + nm:end].decode()
            if name and not name.startswith("$"):
                syms.setdefault(val, name)
    return syms


def vector_table():
    f, secs = elf_sections(ELF)
    sec = next((s for s in secs if s["sname"] == ".isr_vector"), None)
    if sec is None:
        print("  !! no .isr_vector section")
        return
    syms = symbols(f, secs)
    print("Vector table  %s  @ 0x%08X  (%d entries, %d bytes)"
          % (sec["sname"], sec["addr"], sec["size"] // 4, sec["size"]))
    print("  %-5s %-12s %-10s %s" % ("slot", "address", "value", "handler"))
    for i in range(sec["size"] // 4):
        (w, ) = struct.unpack_from("<I", f, sec["off"] + i * 4)
        label = EXC[i] if i < 16 else IRQS.get(i - 16)
        if label is None or label == "Res":
            continue
        tgt = "" if i == 0 else syms.get(w & ~1, "")
        addr = sec["addr"] + i * 4
        if i == 0:
            print("  %-5d 0x%08X   0x%08X  %s (DTCM top)" % (i, addr, w, tgt))
        elif w == 0:
            print("  %-5d 0x%08X   0x%08X  %-18s UNHANDLED" % (i, addr, w, label))
        else:
            print("  %-5d 0x%08X   0x%08X  %-18s %s%s"
                  % (i, addr, w, label, tgt, "(thumb)" if (w & 1) else " <-- NOT THUMB!"))


def compiled_priorities():
    print()
    print("Compiled NVIC priorities (macros resolved by the pre-processor)")
    pat = re.compile(r"(?:HAL_|__)?NVIC_SetPriority\s*\(\s*(\w+)\s*,\s*([^;]*?)\)\s*;",
                     re.S)
    enc = re.compile(r"NVIC_EncodePriority\s*\(\s*__?NVIC_GetPriorityGrouping\s*\(\s*\)\s*,"
                     r"\s*(\w+)\s*,\s*0\s*\)")
    grp = re.compile(r"HAL_NVIC_SetPriorityGrouping\s*\(\s*(\w+)\s*\)")
    srcs = ["Appli/USB_DEVICE/Target/usbd_conf.c", "Appli/Core/Src/ucpd.c",
            "Appli/Core/Src/gpdma.c", "Appli/Core/Src/usart.c",
            "Appli/Core/Src/main.c", "Appli/Core/Src/tracer_emb_hw.c"]
    probe = "/tmp/__prio_probe.c"
    with open(probe, "w") as fh:
        fh.write(PROBE)
    srcs.append(probe)
    cmd = ZIG + TARGET + ["-E", "-P"] + ["-I" + p for p in APPLI_INC] + APPLI_DEF
    seen = set()
    rows = []
    for fn in srcs:
        r = subprocess.run(cmd + [fn], capture_output=True, text=True)
        out = enc.sub(lambda m: "ENC(%s)" % m.group(1), r.stdout)
        for m in pat.finditer(out):
            irq, val = m.group(1), re.sub(r"\s+", " ", m.group(2)).strip()
            if irq.endswith("_IRQn") or irq.endswith("_IRQ"):
                key = (irq, val)
                if key in seen:
                    continue
                seen.add(key)
                rows.append((os.path.basename(fn) if fn != probe else "probe",
                             irq, val))
        for m in grp.finditer(out):
            key = ("GROUPING", m.group(1))
            if key not in seen:
                seen.add(key)
                rows.append((os.path.basename(fn) if fn != probe else "probe",
                             "PRIGROUP", m.group(1)))
    for where, irq, val in sorted(rows, key=lambda r: (r[1] != "PRIGROUP", r[1])):
        print("    %-16s %-22s = %s" % (where, irq, val))
    print()
    print("    ENC(n) == NVIC_EncodePriority(NVIC_GetPriorityGrouping(), n, 0) == n")
    print("    under NVIC_PRIORITYGROUP_4 (4 pre-emption bits, 0 sub-priority bits).")


print("=" * 78)
print("Appli image verification")
print("=" * 78)
if os.path.isfile(ELF):
    vector_table()
else:
    print("!! %s missing - run tools/check_arm_build.py first" % ELF)
compiled_priorities()
