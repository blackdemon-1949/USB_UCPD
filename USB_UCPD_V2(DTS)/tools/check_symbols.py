#!/usr/bin/env python3
"""Link-level symbol resolution check for the merged projects.

The sandbox has no arm-none-eabi toolchain, so the real ELF link cannot run.
This instead compiles every translation unit that the CubeIDE projects build
(with the project's own include paths and -D flags) to x86-64 assembly with
`gcc -S` - the ARM inline asm in CMSIS only breaks at the *assembler* stage,
which -S never reaches - and then resolves every referenced symbol against:

  * the symbols defined by the other translation units,
  * the symbols defined by the closed USBPD stack archive
    (Middlewares/.../USBPDCORE_PD3_FULL_CM7_wc32.a, read with `nm`),
  * the host C library / libgcc.

Anything still unresolved is a real "declared but never defined" bug, i.e.
exactly the class of breakage a bad merge introduces (a call left behind after
a module was dropped).
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
TMP = "/tmp/symcheck"
os.makedirs(TMP, exist_ok=True)

CFLAGS = ["-S", "-O1", "-std=gnu11", "-w"]

APPLI_INC = [
    "Appli/Core/Inc", "Appli/USBPD/App", "Appli/USBPD/Target",
    "Appli/USB_DEVICE/App", "Appli/USB_DEVICE/Target",
    "Drivers/STM32H7RSxx_HAL_Driver/Inc", "Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy",
    "Middlewares/ST/STM32_USBPD_Library/Core/inc",
    "Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc",
    "Middlewares/ST/STM32_USB_Device_Library/Core/Inc",
    "Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc",
    "Drivers/CMSIS/Device/ST/STM32H7RSxx/Include", "Drivers/CMSIS/Include",
]
APPLI_DEF = ["-DUSE_HAL_DRIVER", "-DSTM32H7R3xx", "-DUSE_FULL_LL_DRIVER",
             "-DUSBPD_PORT_COUNT=1", "-D_SNK", "-D_TRACE", "-DUSBPDCORE_LIB_PD3_FULL"]

BOOT_INC = [
    "Boot/Core/Inc", "Drivers/STM32H7RSxx_HAL_Driver/Inc",
    "Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy",
    "Middlewares/ST/STM32_ExtMem_Manager", "Middlewares/ST/STM32_ExtMem_Manager/boot",
    "Middlewares/ST/STM32_ExtMem_Manager/sal", "Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp",
    "Middlewares/ST/STM32_ExtMem_Manager/psram", "Middlewares/ST/STM32_ExtMem_Manager/sdcard",
    "Middlewares/ST/STM32_ExtMem_Manager/user",
    "Drivers/CMSIS/Device/ST/STM32H7RSxx/Include", "Drivers/CMSIS/Include",
]
BOOT_DEF = ["-DUSE_HAL_DRIVER", "-DSTM32H7R3xx"]


def project_sources(project):
    """Every .c the CubeIDE project compiles: its own source folders plus the
    HAL / middleware files it links in through .project."""
    srcs = []
    if project == "Appli":
        for d in ("Appli/Core/Src", "Appli/USBPD", "Appli/USB_DEVICE"):
            for base, _, files in os.walk(d):
                srcs += [os.path.join(base, f) for f in files if f.endswith(".c")]
    else:
        for base, _, files in os.walk("Boot/Core/Src"):
            srcs += [os.path.join(base, f) for f in files if f.endswith(".c")]

    proj = open(f"{project}/.project", encoding="utf-8").read()
    for uri in re.findall(r"<locationURI>PARENT-1-PROJECT_LOC/([^<]+\.c)</locationURI>", proj):
        if os.path.isfile(uri):
            srcs.append(uri)
        else:
            print(f"  !! {project}/.project links a missing file: {uri}")
    return sorted(set(srcs))


def asm_of(src, inc, defs):
    out = os.path.join(TMP, src.replace("/", "_") + ".s")
    cmd = ["gcc"] + CFLAGS + ["-I" + i for i in inc] + defs + [src, "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  !! compile failed: {src}\n{r.stderr[:800]}")
        return None
    return out


LABEL = re.compile(r"^([A-Za-z_][A-Za-z0-9_$]*):", re.M)
GLOBL = re.compile(r"^\s*\.globl\s+([A-Za-z_][A-Za-z0-9_$]*)", re.M)

# Only genuine symbol references in the generated assembly, i.e. the places
# where the assembler/linker will need a definition.  Everything else in an
# operand field is a register, a mnemonic or an immediate.
REF_PATTERNS = [
    re.compile(r"\bcall\s+([A-Za-z_][A-Za-z0-9_$]*)"),
    re.compile(r"\bjmp\s+([A-Za-z_][A-Za-z0-9_$]*)$"),
    re.compile(r"(?<![.\w])([A-Za-z_][A-Za-z0-9_$]*)@GOTPCREL"),
    re.compile(r"(?<![.\w])([A-Za-z_][A-Za-z0-9_$]*)@PLT"),
    re.compile(r"(?<![.\w])([A-Za-z_][A-Za-z0-9_$]*)\(%rip\)"),
    re.compile(r"\$([A-Za-z_][A-Za-z0-9_$]*)"),
]
COMM = re.compile(r"^\s*\.(?:l?comm|local)\s+([A-Za-z_][A-Za-z0-9_$]*)", re.M)
DATA_DIR = re.compile(r"^\s*\.(?:quad|long|int|word|short|byte|dc[A-Za-z]*)\s+(.+)$", re.M)


def analyse(path):
    """Return (defined symbols, referenced symbols) for one .s file."""
    text = open(path, encoding="utf-8", errors="replace").read()
    labels = set(LABEL.findall(text))
    # every label, global or static: a static function is still a definition.
    # .comm/.lcomm/.local are tentative definitions without a label line.
    defined = labels | set(COMM.findall(text))

    used = set()
    for rx in REF_PATTERNS:
        used |= set(rx.findall(text))
    for operands in DATA_DIR.findall(text):
        # (?<![.\w]) skips assembler locals written .Lnnn / .LCn
        used |= set(re.findall(r"(?<![.\w])([A-Za-z_][A-Za-z0-9_$]*)", operands))
    used = {u for u in used if not u.startswith(".L")}
    return defined, used


def provided_by_libs():
    syms = set()
    lib = "Middlewares/ST/STM32_USBPD_Library/Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a"
    out = subprocess.run(["nm", "--defined-only", lib], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ ([A-Za-z]) (\S+)$", line.strip())
        if m and m.group(1).upper() in "TDBRWV":
            syms.add(m.group(2))
    for hostlib in ("libc.so.6", "libm.so.6", "libgcc_s.so.1"):
        for d in ("/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"):
            p = os.path.join(d, hostlib)
            if os.path.exists(p):
                out = subprocess.run(["nm", "-D", "--defined-only", p],
                                     capture_output=True, text=True).stdout
                for line in out.splitlines():
                    f = line.split()
                    if len(f) == 3:
                        # strip ELF symbol versioning: memcpy@@GLIBC_2.14 -> memcpy
                        syms.add(f[2].split("@")[0])
                break
    return syms


def linker_scripts(project):
    """The linker script(s) the build configurations actually select with -T."""
    cproj = open(f"{project}/.cproject", encoding="utf-8").read()
    return sorted(set(re.findall(
        r'tool\.c\.linker\.option\.script\.\d+"[^/]*'
        r'value="\$\{workspace_loc:/\$\{ProjName\}/([^"}]+)\}"', cproj)))


def provided_by_linker_script(project):
    """Symbols the linker script defines, and a check that every memory region
    it places sections into is declared in MEMORY.

    These are exactly what _sbrk / startup resolve against, so a script missing
    one is a real link error rather than a false positive - that blind spot is
    how a Debug configuration shipping the CubeMX MMT template went unnoticed.
    tools/check_arm_build.py performs the actual link."""
    syms, used, declared = set(), set(), set()
    names = linker_scripts(project)
    if not names:
        print(f"  !! {project}/.cproject selects no linker script")
    for name in names:
        path = os.path.join(project, name)
        if not os.path.isfile(path):
            print(f"  !! {project}/.cproject selects a missing script: {name}")
            continue
        t = open(path, encoding="utf-8", errors="replace").read()
        # leading whitespace matters: `_Heap_Limit` is indented in the ST scripts
        syms |= set(re.findall(r"^\s*([A-Za-z_][A-Za-z_0-9]*)\s*=", t, re.M))
        syms |= set(re.findall(r"PROVIDE\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)", t))
        m = re.search(r"^MEMORY\s*\{.*?^\}", t, re.S | re.M)
        if m:
            declared |= set(re.findall(r"^\s*([A-Z_0-9]+)\s*\(", m.group(0), re.M))
        used |= set(re.findall(r">\s*([A-Z_0-9]+)", t))
    missing = used - declared
    if missing:
        print(f"  !! {project} linker script(s) place sections in undeclared "
              f"memory region(s): {sorted(missing)}")
    print(f"  linker scripts    : {', '.join(names) or 'NONE'}"
          f"  ({len(syms)} symbols, {len(declared)} regions declared)")
    return syms, bool(missing) or not names


def check(project, inc, defs):
    print(f"== {project} ==")
    srcs = project_sources(project)
    defined, used = set(), set()
    for s in srcs:
        a = asm_of(s, inc, defs)
        if a is None:
            continue
        d, u = analyse(a)
        defined |= d
        used |= u
    ld_syms, ld_bad = provided_by_linker_script(project)
    libsyms = provided_by_libs() | ld_syms
    unresolved = sorted(s for s in used if s not in defined and s not in libsyms)
    print(f"  translation units : {len(srcs)}")
    print(f"  defined symbols   : {len(defined)}")
    print(f"  referenced symbols: {len(used)}")
    print(f"  unresolved        : {len(unresolved)}")
    for s in unresolved:
        print(f"     UNRESOLVED {s}")
    return len(unresolved) + (1 if ld_bad else 0)


def main():
    bad = 0
    bad += check("Appli", APPLI_INC, APPLI_DEF)
    bad += check("Boot", BOOT_INC, BOOT_DEF)
    print()
    print("symbol resolution:", "OK" if bad == 0 else f"{bad} unresolved")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
