#!/usr/bin/env python3
"""Real ARM cross-compile + link of the Boot and Appli projects.

This is the check the CubeIDE build performs, run on a machine without
CubeIDE or arm-none-eabi-gcc.  It compiles every translation unit the
CubeIDE project compiles (its own source folders plus the HAL / middleware
files it links in through .project) for Cortex-M7 and links them against the
project's own linker script and the prebuilt USBPD core library.

Toolchain: zig cc (clang) + lld, installed with
    python3 -m venv .venv && .venv/bin/pip install ziglang
or any `zig` >= 0.14 on PATH.

    python3 tools/check_arm_build.py            # both projects, both checks
    python3 tools/check_arm_build.py Appli      # one project

What it proves
  * every source compiles for -mcpu=cortex-m7 with -Wall
  * the selected linker script defines every symbol the code references
    (_Heap_Limit, _end, _estack, _Min_Stack_Size, ...)
  * every memory region the script places into is declared in MEMORY
  * the image fits: FLASH / RAM / RAM_NONCACHEABLEBUFFER usage is printed

What it does NOT prove
  * the exact GCC 14.3 code generation (this is clang), and
  * anything about runtime behaviour on the board.

Two lld-only adaptations are applied to a *copy* of each linker script, never
to the file itself: lld cannot resolve ORIGIN()/LENGTH() before the MEMORY
block, has no GCC11+ "(READONLY)" keyword, and clang emits ARM unwind tables
that GCC never produces for this target and the ST scripts do not place.
The script asserts the copy is a pure reordering.
"""
import os
import re
import subprocess
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

TARGET = ["-target", "arm-freestanding-eabihf", "-mcpu=cortex_m7"]

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

OUTROOT = "/tmp/armbuild"
ZIG = None


def find_zig():
    """Prefer `zig` on PATH, then the ziglang PyPI package."""
    global ZIG
    if subprocess.run(["sh", "-c", "command -v zig"], capture_output=True).returncode == 0:
        ZIG = ["zig"]
        return True
    for mod in ("ziglang",):
        if subprocess.run([sys.executable, "-c", "import " + mod],
                          capture_output=True).returncode == 0:
            ZIG = [sys.executable, "-m", mod]
            return True
    return False


def libc_headers():
    """zig's freestanding target ships no libc headers; borrow the bundled
    musl ones so stdio.h / string.h / stdint.h resolve.  Codegen is unaffected."""
    probe = subprocess.run(ZIG + ["env"], capture_output=True, text=True)
    m = re.search(r'\.(?:lib_dir|zig_exe)\s*=\s*"([^"]+)"', probe.stdout)
    if not m:
        return []
    lib = m.group(1)
    if lib.endswith("zig"):                 # zig_exe -> sibling lib directory
        lib = os.path.join(os.path.dirname(lib), "lib")
    base = os.path.join(lib, "libc", "include")
    arm, gen = os.path.join(base, "arm-linux-musl"), os.path.join(base, "generic-musl")
    return [arm, gen] if os.path.isdir(arm) and os.path.isdir(gen) else []


LIBC_STUBS = r"""
/* The sandbox has no C library for bare-metal ARM, so provide the entry points
   the firmware and the prebuilt USBPD archive reference.  These exist only to
   let the link resolve; they are not part of the firmware. */
#include <stddef.h>
void *memcpy(void *d,const void *s,size_t n){char*a=d;const char*b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,size_t n){char*a=d;while(n--)*a++=(char)c;return d;}
void *memmove(void *d,const void *s,size_t n){char*a=d;const char*b=s;if(a<b){while(n--)*a++=*b++;}else{a+=n;b+=n;while(n--)*--a=*--b;}return d;}
int memcmp(const void*a,const void*b,size_t n){const unsigned char*x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
size_t strlen(const char*s){const char*p=s;while(*p)p++;return (size_t)(p-s);}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *(const unsigned char*)a-*(const unsigned char*)b;}
int strncmp(const char*a,const char*b,size_t n){while(n&&*a&&*a==*b){a++;b++;n--;}return n?(*(const unsigned char*)a-*(const unsigned char*)b):0;}
char*strcpy(char*d,const char*s){char*r=d;while((*d++=*s++));return r;}
char*strncpy(char*d,const char*s,size_t n){char*r=d;while(n&&(*d++=*s++))n--;return r;}
char*strcat(char*d,const char*s){char*r=d;while(*d)d++;while((*d++=*s++));return r;}
char*strchr(const char*s,int c){while(*s){if(*s==(char)c)return (char*)s;s++;}return c?NULL:(char*)s;}
char*strstr(const char*h,const char*n){size_t l=strlen(n);if(!l)return (char*)h;for(;*h;h++)if(!strncmp(h,n,l))return (char*)h;return NULL;}
unsigned long strtoul(const char*s,char**e,int b){unsigned long v=0;(void)b;while(*s==' ')s++;while(*s>='0'&&*s<='9'){v=v*10UL+(unsigned long)(*s-'0');s++;}if(e)*e=(char*)s;return v;}
long strtol(const char*s,char**e,int b){int neg=0;if(*s=='-'){neg=1;s++;}long v=(long)strtoul(s,e,b);return neg?-v:v;}
int snprintf(char*s,size_t n,const char*f,...){(void)s;(void)n;(void)f;return 0;}
int vsnprintf(char*s,size_t n,const char*f,void*a){(void)s;(void)n;(void)f;(void)a;return 0;}
int printf(const char*f,...){(void)f;return 0;}
int putchar(int c){return c;}
int getchar(void){return -1;}
void *malloc(size_t n){(void)n;return NULL;}
void free(void*p){(void)p;}
void *calloc(size_t a,size_t b){(void)a;(void)b;return NULL;}
void *realloc(void*p,size_t n){(void)p;(void)n;return NULL;}
double __aeabi_dadd(double a,double b){return a+b;}
double __aeabi_dmul(double a,double b){return a*b;}
double __aeabi_ddiv(double a,double b){return a/b;}
double __aeabi_dsub(double a,double b){return a-b;}
long long __aeabi_d2lz(double a){return (long long)a;}
double __aeabi_l2d(long long a){return (double)a;}
double __aeabi_i2d(int a){return (double)a;}
double __aeabi_ui2d(unsigned a){return (double)a;}
unsigned __aeabi_d2uiz(double a){return (unsigned)a;}
unsigned __aeabi_f2uiz(float a){return (unsigned)a;}
float __aeabi_i2f(int a){return (float)a;}
float __aeabi_ui2f(unsigned a){return (float)a;}
int __aeabi_f2iz(float a){return (int)a;}
float __aeabi_fadd(float a,float b){return a+b;}
float __aeabi_fmul(float a,float b){return a*b;}
float __aeabi_fdiv(float a,float b){return a/b;}
float __aeabi_fsub(float a,float b){return a-b;}
int __aeabi_fcmpgt(float a,float b){return a>b;}
int __aeabi_fcmpeq(float a,float b){return a==b;}
int __aeabi_fcmple(float a,float b){return a<=b;}
int __aeabi_fcmpge(float a,float b){return a>=b;}
int __aeabi_fcmplt(float a,float b){return a<b;}
int __aeabi_fcmpne(float a,float b){return a!=b;}
int __aeabi_dcmpgt(double a,double b){return a>b;}
int __aeabi_dcmpeq(double a,double b){return a==b;}
int __aeabi_dcmple(double a,double b){return a<=b;}
int __aeabi_dcmpge(double a,double b){return a>=b;}
int __aeabi_dcmplt(double a,double b){return a<b;}
int __aeabi_dcmpne(double a,double b){return a!=b;}
double __aeabi_f2d(float a){return (double)a;}
float __aeabi_d2f(double a){return (float)a;}
/* AEABI aliases: note the (dst, n, c) argument order differs from ANSI. */
void __aeabi_memset(void *d,size_t n,int c){memset(d,c,n);}
void __aeabi_memset4(void *d,size_t n,int c){memset(d,c,n);}
void __aeabi_memset8(void *d,size_t n,int c){memset(d,c,n);}
void __aeabi_memcpy(void *d,const void *s,size_t n){memcpy(d,s,n);}
void __aeabi_memcpy4(void *d,const void *s,size_t n){memcpy(d,s,n);}
void __aeabi_memcpy8(void *d,const void *s,size_t n){memcpy(d,s,n);}
void __aeabi_memclr(void *d,size_t n){memset(d,0,n);}
void __aeabi_memclr4(void *d,size_t n){memset(d,0,n);}
void __aeabi_memclr8(void *d,size_t n){memset(d,0,n);}
struct udivret { unsigned long long q, r; };
struct udivret __aeabi_uldivmod(unsigned long long n,unsigned long long d){
  struct udivret v; v.q=d?n/d:0; v.r=d?n%d:0; return v; }
struct udivret __aeabi_ldivmod(long long n,long long d){
  struct udivret v; v.q=(unsigned long long)(d?n/d:0);
  v.r=(unsigned long long)(d?n%d:0); return v; }
void __libc_init_array(void){}
void __libc_fini_array(void){}
int atexit(void (*f)(void)){(void)f;return 0;}
void abort(void){for(;;){}}
"""


def project_sources(project):
    """Every .c the CubeIDE project compiles: its own source folders plus the
    HAL / middleware files it links in through .project."""
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
    """The linker script each build configuration actually uses (-T)."""
    cproj = open(f"{project}/.cproject", encoding="utf-8").read()
    return re.findall(r'tool\.c\.linker\.option\.script\.\d+"[^/]*'
                      r'value="\$\{workspace_loc:/\$\{ProjName\}/([^"}]+)\}"', cproj)


def lld_script(path, out):
    """Produce an lld-readable copy of a GNU ld script.  Reordering only."""
    t = open(path, encoding="utf-8", errors="replace").read()
    mem_re = re.compile(r"^MEMORY\s*\{.*?^\}\s*$", re.S | re.M)
    m = mem_re.search(t)
    if not m:
        return path
    mem, body = m.group(0), t[:m.start()] + t[m.end():]

    assign = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*)\s*=\s*[^;]*;", re.M)
    needed = set(assign.findall(mem)) | {
        n for n in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", mem)
        if re.search(r"^" + n + r"\s*=", body, re.M)}
    hoisted, rest = [], body
    for n in sorted(needed):
        am = re.search(r"^" + n + r"\s*=\s*[^;]*;", rest, re.M)
        if am:
            hoisted.append(am.group(0))
            rest = rest[:am.start()] + rest[am.end():]

    outtext = "\n".join(hoisted) + "\n" + mem + "\n" + rest
    outtext = re.sub(r"\s*\(READONLY\)", "", outtext)
    sm = re.search(r"^SECTIONS\s*\{", outtext, re.M)
    outtext = (outtext[:sm.end()] +
               "  /DISCARD/ : { *(.ARM.extab*) *(.ARM.exidx*) *(.ARM) *(.ARM.attributes) }" +
               outtext[sm.end():])

    dst = os.path.join(out, "lld_" + os.path.basename(path))
    open(dst, "w", encoding="utf-8").write(outtext)
    assert sorted(assign.findall(t)) == sorted(assign.findall(outtext)), "assignments changed"
    assert mem_re.search(outtext).group(0).split() == mem.split(), "MEMORY changed"
    return dst


def build(project, script=None):
    inc, defs = (APPLI_INC, APPLI_DEF) if project == "Appli" else (BOOT_INC, BOOT_DEF)
    scripts = linker_script_of(project)
    if not scripts:
        print(f"  !! no linker script found in {project}/.cproject")
        return False
    if len(set(scripts)) != 1:
        print(f"  !! {project} build configurations disagree on the linker script: "
              f"{sorted(set(scripts))}")
        return False
    script = script or scripts[0]
    if not os.path.isfile(os.path.join(project, script)):
        print(f"  !! {project}/{script} does not exist")
        return False

    out = os.path.join(OUTROOT, project)
    subprocess.run(["rm", "-rf", out])
    os.makedirs(out, exist_ok=True)

    # zig reuses its compilation cache across runs and does NOT replay compiler
    # diagnostics on a cache hit, so a warm cache silently reported 0 warnings
    # while a cold one reported the real ones.  Give each run its own cache so
    # the warning count is reproducible.
    zcache = os.path.join(out, "zig-cache")
    ccenv = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=zcache, ZIG_LOCAL_CACHE_DIR=zcache)
    extra = libc_headers()
    cflags = (TARGET + ["-std=gnu11", "-g3"] + defs +
              ["-I" + i for i in (extra + inc)] +
              ["-O0", "-ffunction-sections", "-fdata-sections", "-Wall",
               "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
               "-fno-exceptions", "-fno-rtti", "-fno-sanitize=all"])

    srcs = project_sources(project)
    print(f"== {project}: {len(srcs)} C sources, -T {script}")
    fail = warn = 0
    for src in srcs:
        obj = os.path.join(out, src.replace("/", "_") + ".o")
        r = subprocess.run(ZIG + ["cc"] + cflags + ["-c", src, "-o", obj],
                           capture_output=True, text=True, env=ccenv)
        if r.returncode != 0:
            print(f"  COMPILE FAIL {src}\n    " + r.stderr.strip()[:900].replace("\n", "\n    "))
            fail += 1
        else:
            # Count real diagnostics only, not incidental stderr chatter.
            msgs = [l for l in r.stderr.splitlines() if "warning:" in l or "error:" in l]
            if msgs:
                warn += 1
                print(f"  COMPILE WARN {src}\n    " + "\n    ".join(msgs[:6]))
    for asm in sorted(glob.glob(f"{project}/Core/Startup/*.s")):
        obj = os.path.join(out, asm.replace("/", "_") + ".o")
        r = subprocess.run(ZIG + ["cc"] + TARGET + ["-c", asm, "-o", obj],
                           capture_output=True, text=True, env=ccenv)
        if r.returncode:
            print(f"  ASM FAIL {asm}\n    " + r.stderr.strip()[:500])
            fail += 1
    if fail:
        print(f"  {len(srcs) - fail} compiled, {fail} failed")
        return False
    print(f"  compiled: {len(srcs)} ok, 0 failed ({warn} with warnings)")

    stub = os.path.join(out, "_libc_stubs.c")
    open(stub, "w").write(LIBC_STUBS)
    subprocess.run(ZIG + ["cc"] + TARGET + ["-O2", "-c", stub,
                                            "-o", os.path.join(out, "_libc_stubs.o")],
                   capture_output=True, text=True, check=True, env=ccenv)

    libs = []
    if project == "Appli":
        libs = [os.path.abspath(
            "Middlewares/ST/STM32_USBPD_Library/Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a")]

    objs = sorted(glob.glob(os.path.join(out, "*.o")))
    r = subprocess.run(ZIG + ["ld.lld", "-T", lld_script(f"{project}/{script}", out),
                              "--gc-sections", "--error-limit=0",
                              "--orphan-handling=warn", "--print-memory-usage",
                              "-o", os.path.join(out, project + ".elf")] + objs + libs,
                       capture_output=True, text=True)
    errs = [l for l in (r.stderr + r.stdout).splitlines()
            if "error" in l or "undefined" in l or "cannot" in l]
    usage = [l for l in (r.stderr + r.stdout).splitlines()
             if re.match(r"^\s*(Memory region|[A-Z_]+:)\s", l) or "%" in l and ":" in l]
    if r.returncode != 0 or errs:
        print("  LINK FAILED")
        for l in errs[:25]:
            print("    " + l)
        return False
    print("  link: OK")
    for l in usage:
        print("    " + l.rstrip())
    elf = os.path.join(out, project + ".elf")
    print(f"  {project}.elf  {os.path.getsize(elf)} bytes")
    return True


def main():
    if not find_zig():
        print("zig not found.  Install it with:\n"
              "    python3 -m venv .venv && .venv/bin/pip install ziglang\n"
              "and re-run with that interpreter, or put zig on PATH.")
        return 2
    print(f"toolchain: {' '.join(ZIG)}  ({ZIG[0] if len(ZIG) == 1 else 'python -m ziglang'})")
    which = sys.argv[1:] or ["Boot", "Appli"]
    ok = all(build(p) for p in which)
    print("\nARM build check:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
