#!/usr/bin/env python3
"""Compile recompiled C (from tools/recompile.py) into a shared library.

usage: python3 -m recomp.build build/recomp [--harness] [-j N] [--opt O1] [-o game.so]
       python3 -m recomp.build build/recomp --code-library [--cc CC] [-o mh4u_code.so]
       python3 -m recomp.build build/recomp --code-library --android [--cc CC] [-o lib.so]

--harness links the differential-test harness (gametest). --code-library builds the
library the PC port loads with `mh4u_runner --code` (paged memory, callbacks from the
host); --cc picks another compiler, e.g. llvm-mingw's x86_64-w64-mingw32-clang for a
Windows .dll; --android builds it for Android's arm64 app (any clang with lld, Zig's included:
no NDK, see runtime/android/README.md). Incremental: an object is reused while its source is older than it and
recomp.h has the contents it was compiled with (obj/recomp.h.sha1 remembers them, so a
copy of the tools with new dates, as unpacking a new package makes, recompiles nothing).
Files that don't compile while all the jobs run are compiled again one at a time (a compiler
the system stopped for want of memory gets all of it); a compile or a link that fails says how
the compiler ended, also when it printed nothing. An earlier build's object or library goes
before it is made again, so that one the compiler didn't write is missing, not old.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import os
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")
HARNESS = os.path.join(HERE, "harness", "harness.c")
DLL_GLUE = os.path.join(RUNTIME, "rc_dll.c")
BASE_FLAGS = ["-c", "-fPIC", "-ffp-contract=off", "-frounding-math", "-fno-strict-aliasing", "-fwrapv", "-w",
              f"-I{RUNTIME}"]

# ── Android (the arm64 app): the C library's headers are the few in runtime/android, and the
# functions come from the device's libc.so and libm.so, which the library names as needed
# through two empty stand-ins it is linked against. API 29 is the app's minSdk.
ANDROID_TARGET = "aarch64-linux-android29"
ANDROID_INCLUDE = os.path.join(RUNTIME, "android")
ANDROID_SONAME = "libmh4u_code.so"
ANDROID_PAGE = 16384  # Android 15's 16 KB pages; 4 KB devices load such a library too
# what the library may take from the device: Android's libc.so and libm.so have all of these
ANDROID_IMPORTS = frozenset({
    "memcpy", "memmove", "memset", "memcmp", "strlen", "__stack_chk_fail", "fegetround", "fesetround",
    *(f + suffix for f in ("sqrt", "fabs", "trunc", "floor", "ceil", "round", "rint", "nearbyint", "fma",
                           "fmin", "fmax", "copysign", "fmod", "ldexp") for suffix in ("", "f")),
})


def is_zig(cc: str) -> bool:
    return "zig" in os.path.basename(cc).lower()


def android_compiler(cc: str) -> str:
    """The compiler to build for Android with: llvm-mingw's target-named clang (a wrapper that
    insists on Windows) gives way to the plain clang next to it."""
    name = os.path.basename(cc).lower()
    if "mingw32-clang" in name:
        plain = os.path.join(os.path.dirname(cc), "clang" + (".exe" if name.endswith(".exe") else ""))
        if os.path.isfile(plain):
            return plain
    return cc


def android_target_flags(cc: str) -> list[str]:
    return ["-target", "aarch64-linux-android"] if is_zig(cc) else [f"--target={ANDROID_TARGET}"]


def android_link_flags(cc: str, outdir: str) -> list[str]:
    """The link's flags, with the stand-ins for libc.so and libm.so made in outdir/android-stubs."""
    target = android_target_flags(cc)
    common = [*target, "-shared", "-nostdlib", *([] if is_zig(cc) else ["-fuse-ld=lld"])]
    stubs = os.path.join(outdir, "android-stubs")
    os.makedirs(stubs, exist_ok=True)
    empty = os.path.join(stubs, "empty.c")
    with open(empty, "w") as f:
        f.write("/* a stand-in: only its name counts (Android's own library is loaded) */\n")
    libs = []
    for soname in ("libc.so", "libm.so"):
        lib = os.path.join(stubs, soname)
        r = subprocess.run([cc, *common, f"-Wl,-soname,{soname}", empty, "-o", lib], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"{cc} can't link for Android (it needs lld):\n{r.stderr[-2000:]}")
        libs.append(lib)
    return [*common, f"-Wl,-soname,{ANDROID_SONAME}", f"-Wl,-z,max-page-size={ANDROID_PAGE}",
            "-Wl,--build-id=sha1", "-Wl,-z,noexecstack", *libs]


def android_library_problems(path: str) -> list[str]:
    """What would keep Android's linker from loading the library at `path` in the app (an empty
    list when nothing): another machine, no libc.so/libm.so needed, text relocations, pages under
    16 KB, or a function the device's C library doesn't have."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return ["not a 64-bit little-endian ELF file"]
    machine = struct.unpack_from("<H", data, 18)[0]
    problems = [] if machine == 183 else [f"built for machine {machine}, not AArch64 (183)"]
    e_phoff, e_shoff = struct.unpack_from("<QQ", data, 32)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", data, 54)
    dynamic = None
    for i in range(e_phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, p_align = struct.unpack_from("<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_align < ANDROID_PAGE:
            problems.append(f"a segment aligned to {p_align} bytes, not {ANDROID_PAGE}")
        if p_type == 2:
            dynamic = (p_offset, p_filesz)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    dynsym = next((s for s in sections if s[1] == 11), None)  # SHT_DYNSYM
    if dynamic is None or dynsym is None:
        return problems + ["no dynamic section: not a shared library"]
    strtab = sections[dynsym[6]]
    def text(offset: int) -> str:
        start = strtab[4] + offset
        return data[start:data.index(b"\0", start)].decode("ascii", "replace")
    needed, soname, textrel = [], "", False
    for off in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, value = struct.unpack_from("<qQ", data, off)
        if tag == 0:
            break
        if tag == 1:
            needed.append(text(value))
        elif tag == 14:
            soname = text(value)
        elif tag == 22 or (tag == 30 and value & 4):  # DT_TEXTREL, DT_FLAGS & DF_TEXTREL
            textrel = True
    for lib in ("libc.so", "libm.so"):
        if lib not in needed:
            problems.append(f"doesn't name {lib} as needed")
    if soname != ANDROID_SONAME:
        problems.append(f"its soname is {soname or 'missing'}, not {ANDROID_SONAME}")
    if textrel:
        problems.append("it has text relocations")
    imports = set()
    for off in range(dynsym[4], dynsym[4] + dynsym[5], 24):
        name, info, _, shndx = struct.unpack_from("<IBBH", data, off)
        if name and shndx == 0 and (info >> 4) != 2:  # undefined, not weak
            imports.add(text(name))
    missing = sorted(imports - ANDROID_IMPORTS)
    if missing:
        problems.append("it calls functions from outside the few the port takes from Android's C library: " + ", ".join(missing))
    return problems


def how_it_ended(code: int) -> str:
    """How a compiler's process ended, for a message: its exit status, or the signal that stopped
    it (the system stops one with SIGKILL when it runs out of memory)."""
    if code < 0:
        try:
            name = f" ({signal.Signals(-code).name})"
        except ValueError:
            name = ""
        why = ", most likely because the computer ran out of memory" if code == -9 else ""
        return f"was stopped by signal {-code}{name}{why}"
    if code > 255:
        return f"ended with status {code} (0x{code:08X})"
    return f"ended with status {code}"


def remove_old(path: str) -> None:
    """An earlier build's output, removed before it is made again: when the compiler's status
    reads 0 but it wrote nothing (a status that can't be read, as with SIGCHLD ignored, reads 0),
    the output is then missing, not the old one in its place. One that can't be removed is
    written over as before."""
    try:
        os.remove(path)
    except OSError:
        pass


def compile_one(cc: str, src: str, obj: str, flags: list[str]) -> tuple[str, float, int, str]:
    """Compiles src into obj: (src, seconds, exit status, what the compiler said if it failed)."""
    t = time.time()
    remove_old(obj)
    r = subprocess.run([cc, *flags, src, "-o", obj], capture_output=True, text=True, errors="replace")
    return src, time.time() - t, r.returncode, ((r.stderr or r.stdout) if r.returncode else "")


def compile_again(cc: str, failed: list[tuple[str, int, str]], objects: dict[str, str],
                  flags: list[str]) -> list[tuple[str, int, str]]:
    """The files that didn't compile while all the jobs ran, compiled again one at a time: a
    compiler the system stopped for want of memory then has all of it. Once one fails again on
    its own, the errors are real ones and the rest stay as they were. Returns the errors left."""
    print(f"  {len(failed)} file(s) didn't compile: again, one at a time", flush=True)
    errors = []
    for i, (src, code, err) in enumerate(failed, 1):
        if not errors:
            # (launcher/tools.cpp, BuildProgress, shows how far this got)
            print(f"  compiling again {i}/{len(failed)}: {os.path.basename(src)}", flush=True)
            _, _, code, err = compile_one(cc, src, objects[src], flags)
        if code:
            errors.append((src, code, err))
    return errors


def build(outdir: str, jobs: int, opt: str, defines: list[str], lib_name: str, extra_sources: list[str],
          cc: str = "clang", extra_flags: list[str] | None = None, runtime_header: str | None = None,
          android: bool = False) -> str:
    src_dir = os.path.join(outdir, "src")
    obj_dir = os.path.join(outdir, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    # funcs.h only gains or loses declarations; a file whose calls change is rewritten itself.
    # recomp.h is in every object: when its contents differ from those the objects were
    # compiled with (obj/recomp.h.sha1), everything is recompiled; its date is not looked at,
    # since a copy of the tools (a new package unpacked) renews it without changing a thing
    with open(runtime_header or os.path.join(RUNTIME, "recomp.h"), "rb") as fh:
        header_hash = hashlib.sha1(fh.read()).hexdigest()
    stamp = os.path.join(obj_dir, "recomp.h.sha1")
    # no stamp yet (objects from before it was kept): they were built against the header
    # of their day, which the old date check kept current, so they stand and get the stamp
    header_changed = os.path.exists(stamp) and open(stamp).read().strip() != header_hash
    if android:
        cc = android_compiler(cc)
        extra_flags = [*android_target_flags(cc), "-nostdlibinc", "-isystem", ANDROID_INCLUDE, *(extra_flags or [])]
    flags = [f"-{opt}", *BASE_FLAGS, f"-I{src_dir}", *[f"-D{d}" for d in defines], *(extra_flags or [])]
    sources = sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith(".c")) + extra_sources
    # objects are named after everything that changes the code, so builds with other
    # settings (harness, code library, another compiler) never reuse each other's objects
    variant = hashlib.sha1(" ".join([os.path.basename(cc), opt, *defines, *(extra_flags or [])]).encode()).hexdigest()[:8]
    todo, objs = [], []
    for s in sources:
        o = os.path.join(obj_dir, f"{os.path.basename(s)[:-2]}-{opt}-{variant}.o")
        objs.append(o)
        if header_changed or not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(s):
            todo.append((s, o))
    if header_changed:
        # the other variants' objects were compiled against the old header too
        for name in os.listdir(obj_dir):
            if name.endswith(".o") and os.path.join(obj_dir, name) not in objs:
                os.remove(os.path.join(obj_dir, name))
    t0 = time.time()
    errors = []
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(compile_one, cc, s, o, flags) for s, o in todo]
        for i, fut in enumerate(cf.as_completed(futs), 1):
            src, dt, code, err = fut.result()
            # (a compiler stopped by a signal says nothing: its status counts, not its words)
            if code:
                errors.append((src, code, err))
            if i % 20 == 0 or i == len(futs):
                print(f"  compiled {i}/{len(futs)} ({time.time() - t0:.0f}s)", flush=True)
    if errors and jobs > 1:
        errors = compile_again(cc, sorted(errors), dict(todo), flags)
    if errors:
        for src, code, err in errors[:5]:
            said = err.strip()
            print(f"error in {src}: {os.path.basename(cc)} {how_it_ended(code)}" + (f"\n{said[-2000:]}" if said else ""),
                  file=sys.stderr)
        raise SystemExit(1)
    # (a status of 0 with nothing written: a process that can't wait for its children, as with
    # SIGCHLD ignored, reads every status as 0; compile_one removed an earlier build's object)
    missing = [o for o in objs if not os.path.isfile(o)]
    if missing:
        raise SystemExit(f"{len(missing)} object(s) missing after compiling, {os.path.basename(missing[0])} the "
                         f"first: {os.path.basename(cc)} reported success (status 0) but didn't write them")
    with open(stamp, "w") as fh:
        fh.write(header_hash + "\n")
    lib = os.path.join(outdir, lib_name)
    os.makedirs(os.path.dirname(lib), exist_ok=True)
    # the object list goes in a response file: Windows limits a command line to 32 KiB
    rsp = os.path.join(outdir, "objects.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objs) + "\n")
    if android:
        link = [cc, *android_link_flags(cc, outdir), f"@{rsp}", "-o", lib]
    else:
        link = [cc, "-shared", f"@{rsp}", "-o", lib, *(["-lm"] if "mingw" not in cc else [])]
    remove_old(lib)
    r = subprocess.run(link, capture_output=True, text=True, errors="replace")
    if r.returncode:
        # how the linker ended first, then what it said (if anything: one stopped by a signal says
        # nothing, and the message would be empty)
        said = (r.stderr.strip() or r.stdout.strip())[-4000:]
        raise SystemExit(f"linking {os.path.basename(lib)} failed: {os.path.basename(cc)} {how_it_ended(r.returncode)}"
                         + (f"\n{said}" if said else ""))
    if not os.path.isfile(lib):
        raise SystemExit(f"linking {os.path.basename(lib)} failed: {os.path.basename(cc)} reported success "
                         "(status 0) but didn't write it")
    print(f"linked {lib} ({len(objs)} objects, {len(todo)} recompiled in {time.time() - t0:.0f}s)")
    if android:
        problems = android_library_problems(lib)
        if problems:
            raise SystemExit(f"{lib} wouldn't load on Android: " + "; ".join(problems))
    return lib


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--opt", default="O1")
    ap.add_argument("--harness", action="store_true", help="link the test harness (Linux/macOS)")
    ap.add_argument("--code-library", action="store_true", help="build the PC port's code library")
    ap.add_argument("--android", action="store_true", help="the code library for Android's arm64 app")
    ap.add_argument("--cc", default="clang")
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("-o", "--lib", default="")
    args = ap.parse_args(argv)
    if args.code_library:
        lib = args.lib or ("mh4u_code.dll" if "mingw" in args.cc and not args.android else "mh4u_code.so")
        build(args.outdir, args.jobs, args.opt, ["RC_PAGED", *args.defines], lib, [DLL_GLUE],
              cc=args.cc, extra_flags=["-fvisibility=hidden"], android=args.android)
    else:
        build(args.outdir, args.jobs, args.opt, args.defines, args.lib or "game.so",
              [HARNESS] if args.harness else [], cc=args.cc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
