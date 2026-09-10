#!/usr/bin/env python3
"""Real arm-none-eabi-gcc build of the Boot and Appli projects.

check_arm_build.py (zig/clang) is a link/geometry check.  This script is the
byte-level one: it drives a real GNU Arm Embedded toolchain with the flags
STM32CubeIDE uses for these projects' Debug configurations and produces the
actual .elf/.map for each project, plus region-accurate memory usage.

Toolchain (install once): any arm-none-eabi-gcc >= 12 on PATH.

    python3 tools/build_gcc.py            # both projects
    python3 tools/build_gcc.py Appli      # one project
    GCC=arm-none-eabi-gcc-14 python3 tools/build_gcc.py   # explicit prefix

If the toolchain ships nano.specs (stock Arm GNU toolchains), the link uses
--specs=nano.specs/--specs=nosys.specs like CubeIDE.  Otherwise (e.g. the
GCC-only wheel plus a separately built nano newlib used in CI), set
NEWLIB_LIB_DIR to a directory containing libc_nano.a/libm_nano.a and
NEWLIB_INC_DIR to its include tree, and the link uses those directly with
the same -lc_nano semantics.  (The project linker scripts /DISCARD/ the
plain `libc.a`/`libm.a` archive names, so the _nano names are required to
reproduce the CubeIDE link regardless of how the toolchain was obtained.)

Flags mirror the CubeIDE Debug configuration parsed from .cproject:
  -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb
  -Og -g3 -ffunction-sections -fdata-sections -Wall
  (Appli) -DDEBUG -DUSE_HAL_DRIVER -DSTM32H7R3xx -DUSE_FULL_LL_DRIVER
          -DUSBPD_PORT_COUNT=1 -D_SNK -D_TRACE -DUSBPDCORE_LIB_PD3_FULL
  link: -T <project linker script> --gc-sections --specs=nano.specs
        --specs=nosys.specs, newlib-nano + libm + libgcc + (Appli) the
        prebuilt USBPD core archive.
"""
import os
import re
import subprocess
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

PREFIX = os.environ.get("GCC", "arm-none-eabi-gcc")
SIZE = PREFIX[:-4] + "-size" if PREFIX.endswith("-gcc") else "arm-none-eabi-size"

# How to get newlib-nano + headers: either from the toolchain itself
# (nano.specs present) or from NEWLIB_LIB_DIR / NEWLIB_INC_DIR env vars.
NANO_SPECS = os.popen(f"{PREFIX} -print-file-name=nano.specs").read().strip()
USE_SPECS = NANO_SPECS and os.path.isfile(NANO_SPECS)
if not USE_SPECS:
    NEWLIB_LIB_DIR = os.environ.get(
        "NEWLIB_LIB_DIR", os.path.expanduser(
            "~/toolchains/newlib-install/arm-none-eabi/lib"))
    NEWLIB_INC_DIR = os.environ.get(
        "NEWLIB_INC_DIR", os.path.expanduser(
            "~/toolchains/newlib-install/arm-none-eabi/include"))
    if not os.path.isfile(os.path.join(NEWLIB_LIB_DIR, "libc_nano.a")):
        print(f"no nano.specs in the toolchain and no libc_nano.a under "
              f"NEWLIB_LIB_DIR={NEWLIB_LIB_DIR}; set NEWLIB_LIB_DIR.")
        sys.exit(2)

MCU = ["-mcpu=cortex-m7", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-mthumb"]

APPLI_INC = ["Appli/Core/Inc", "Appli/USBPD/App", "Appli/USBPD/Target",
             "Appli/USB_DEVICE/App", "Appli/USB_DEVICE/Target",
             "Drivers/STM32H7RSxx_HAL_Driver/Inc",
             "Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy",
             "Middlewares/ST/STM32_USBPD_Library/Core/inc",
             "Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc",
             "Middlewares/ST/STM32_USB_Device_Library/Core/Inc",
             "Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc",
             "Drivers/CMSIS/Device/ST/STM32H7RSxx/Include",
             "Drivers/CMSIS/Include"]
APPLI_DEF = ["-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32H7R3xx", "-DUSE_FULL_LL_DRIVER",
             "-DUSBPD_PORT_COUNT=1", "-D_SNK", "-D_TRACE", "-DUSBPDCORE_LIB_PD3_FULL"]
BOOT_INC = ["Boot/Core/Inc", "Drivers/STM32H7RSxx_HAL_Driver/Inc",
            "Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy",
            "Middlewares/ST/STM32_ExtMem_Manager",
            "Middlewares/ST/STM32_ExtMem_Manager/boot",
            "Middlewares/ST/STM32_ExtMem_Manager/sal",
            "Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp",
            "Middlewares/ST/STM32_ExtMem_Manager/psram",
            "Middlewares/ST/STM32_ExtMem_Manager/sdcard",
            "Middlewares/ST/STM32_ExtMem_Manager/user",
            "Drivers/CMSIS/Device/ST/STM32H7RSxx/Include",
            "Drivers/CMSIS/Include"]
BOOT_DEF = ["-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32H7R3xx"]

OUTROOT = "/tmp/gccbuild"


def project_sources(project):
    """Every .c the CubeIDE project compiles: its own source folders plus the
    HAL / middleware files it links in through .project (same logic as
    check_arm_build.py, so the two tools can be cross-checked)."""
    srcs = []
    dirs = (["Appli/Core/Src", "Appli/USBPD", "Appli/USB_DEVICE"] if project == "Appli"
            else ["Boot/Core/Src"])
    for d in dirs:
        for base, _, files in os.walk(d):
            srcs += [os.path.join(base, f) for f in files if f.endswith(".c")]
    proj = open(f"{project}/.project", encoding="utf-8").read()
    for uri in re.findall(r"<locationURI>PARENT-1-PROJECT_LOC/([^<]+\.c)</locationURI>", proj):
        if os.path.isfile(uri):
            srcs.append(uri)
        else:
            print(f"  !! {project}/.project links a missing file: {uri}")
    return sorted(set(srcs))


def linker_script_of(project):
    """The linker script every configuration of the project uses.  It appears
    as ${workspace_loc:/${ProjName}/<script>} in .cproject - in the explicit
    Linker Script options (Appli) and/or in the CubeMX defaults blob (Boot)."""
    cproj = open(f"{project}/.cproject", encoding="utf-8").read()
    return re.findall(r"workspace_loc:/\$\{ProjName\}/([^\"}]+\.ld)", cproj)


def build(project):
    inc, defs = (APPLI_INC, APPLI_DEF) if project == "Appli" else (BOOT_INC, BOOT_DEF)
    scripts = linker_script_of(project)
    if len(set(scripts)) != 1:
        print(f"  !! {project} build configurations disagree on the linker script: "
              f"{sorted(set(scripts))}")
        return False
    script = scripts[0]
    if not os.path.isfile(os.path.join(project, script)):
        print(f"  !! {project}/{script} does not exist")
        return False

    out = os.path.join(OUTROOT, project)
    subprocess.run(["rm", "-rf", out])
    os.makedirs(out, exist_ok=True)

    cflags = (MCU + ["-std=gnu11", "-Og", "-g3", "-Wall",
                     "-ffunction-sections", "-fdata-sections"] +
              defs + [f"-I{i}" for i in inc])
    if not USE_SPECS:
        # prefer the newlib headers that match the explicitly linked
        # libc_nano.a build (gcc's internal arm-none-eabi/include is only a
        # fallback; -isystem precedes it in the search order)
        cflags += [f"-isystem{NEWLIB_INC_DIR}"]

    srcs = project_sources(project)
    print(f"== {project}: {len(srcs)} C sources, -T {script}")
    print(f"   {PREFIX} {subprocess.run([PREFIX, '--version'], capture_output=True, text=True).stdout.splitlines()[0]}")
    fail = warn = 0
    for src in srcs:
        obj = os.path.join(out, src.replace("/", "_") + ".o")
        r = subprocess.run([PREFIX] + cflags + ["-c", src, "-o", obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  COMPILE FAIL {src}\n    " + r.stderr.strip()[:900].replace("\n", "\n    "))
            fail += 1
        else:
            msgs = [l for l in r.stderr.splitlines() if "warning:" in l or "error:" in l]
            if msgs:
                warn += 1
                print(f"  COMPILE WARN {src}\n    " + "\n    ".join(msgs[:6]))
    for asm in sorted(glob.glob(f"{project}/Core/Startup/*.s")):
        obj = os.path.join(out, asm.replace("/", "_") + ".o")
        r = subprocess.run([PREFIX] + MCU + ["-c", asm, "-o", obj],
                           capture_output=True, text=True)
        if r.returncode:
            print(f"  ASM FAIL {asm}\n    " + r.stderr.strip()[:500])
            fail += 1
    if fail:
        print(f"  {len(srcs) - fail} compiled, {fail} failed")
        return False
    print(f"  compiled: {len(srcs)} ok, 0 failed ({warn} with warnings)")

    elf = os.path.join(out, project + ".elf")
    mapfile = os.path.join(out, project + ".map")
    libs = []
    if project == "Appli":
        libs = [os.path.abspath(
            "Middlewares/ST/STM32_USBPD_Library/Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a")]
    objs = sorted(glob.glob(os.path.join(out, "*.o")))
    if USE_SPECS:
        libflags = ["--specs=nano.specs", "--specs=nosys.specs",
                    "-Wl,--start-group", "-lc", "-lm", "-Wl,--end-group"]
    else:
        # Equivalent explicit form: nano newlib + nosys-equivalent stubs are
        # provided by the project's own syscalls.c/sysmem.c translation units,
        # which is why no nosys archive is needed here.  libinitfini.a carries
        # newlib's init.o/fini.o (__libc_init_array, __libc_fini_array): the
        # startup .s calls them, and a from-source newlib build can compile
        # them out when its configure cannot probe .init_array support.
        libflags = [f"-L{NEWLIB_LIB_DIR}",
                    "-Wl,--start-group", "-lc_nano", "-lm_nano", "-linitfini",
                    "-Wl,--end-group"]
    cmd = ([PREFIX] + MCU +
           ["-T", os.path.join(project, script),
            f"-Wl,-Map={mapfile}", "-nostartfiles"] +
           (["--specs=nano.specs", "--specs=nosys.specs"] if USE_SPECS else []) +
           ["-Wl,--gc-sections", "-Wl,--print-memory-usage", "-o", elf] +
           objs + libs + libflags)
    # -nostartfiles: the project's own startup_stm32h7r3xx.s is the reset
    # entry, and the linker scripts /DISCARD/ _start (and the plain libc/
    # libm archive names), which is how the CubeIDE link behaves too.
    # Note: CubeIDE Debug for these projects does NOT enable float printf
    # (no "-u _printf_float"), so neither does this script - the %f prints in
    # apie_*.c behave exactly as they do in the CubeIDE build.
    r = subprocess.run(cmd, capture_output=True, text=True)
    errs = [l for l in (r.stderr + r.stdout).splitlines()
            if re.search(r"error|undefined|cannot|overflowed", l, re.I)]
    if r.returncode != 0 or errs:
        print("  LINK FAILED")
        for l in errs[:25]:
            print("    " + l)
        return False
    print("  link: OK")
    for l in (r.stderr + r.stdout).splitlines():
        if re.match(r"^\s*(Memory region|[A-Z_]+:)", l):
            print("    " + l.rstrip())

    sz = subprocess.run([SIZE, elf], capture_output=True, text=True)
    if sz.returncode == 0:
        print("    " + sz.stdout.strip().replace("\n", "\n    "))
    print(f"  {project}.elf  {os.path.getsize(elf)} bytes  (map: {mapfile})")
    return True


def main():
    if subprocess.run(["sh", "-c", f"command -v {PREFIX}"], capture_output=True).returncode != 0:
        print(f"{PREFIX} not found on PATH (or set GCC=<prefix>-gcc).")
        return 2
    which = sys.argv[1:] or ["Boot", "Appli"]
    ok = all(build(p) for p in which)
    print("\narm-none-eabi-gcc build:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
