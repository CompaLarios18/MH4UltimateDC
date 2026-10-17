#!/usr/bin/env python3
"""Build a port test program (any port/tests/<name>.c: svc_test, bench, io_test and the
conf_* kernel conformance programs) into a 3DS ELF that Azahar can boot, and recompile it.

usage: python3 port/tests/build_svc_test.py [-o build/port-test] [--program NAME] [--list]
Writes <out>/<program>.elf and its recompiled code in <out>/src/; build the code library
with `python3 -m recomp.build <out> --code-library` and run it with mh4u_runner --code.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

import difftest as dt  # noqa: E402
import recompile  # noqa: E402
from recomp.analysis import Image  # noqa: E402

BASE = 0x00100000
# Azahar's ELF loader wants one PT_LOAD each for code (RX), rodata (R) and data (RW).
LINKER_SCRIPT = f"""
ENTRY(_start)
PHDRS {{ text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }}
SECTIONS {{
  . = 0x{BASE:08x};
  .text : {{ *(.text.start) *(.text .text.*) }} :text
  . = ALIGN(0x1000);
  .rodata : {{ *(.rodata .rodata.*) }} :rodata
  . = ALIGN(0x1000);
  .data : {{ *(.data .data.*) }} :data
  .bss : {{ *(.bss .bss.* COMMON) }} :data
  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }}
}}
"""


def programs() -> list[str]:
    """The names of the test programs: every C file here with a _start (svc.h is a header)."""
    names = []
    for f in sorted(os.listdir(HERE)):
        if f.endswith(".c"):
            with open(os.path.join(HERE, f), encoding="utf-8") as fh:
                if "_start" in fh.read():
                    names.append(f[:-2])
    return names


def build(out: str, program: str = "svc_test") -> str:
    if program not in programs():
        raise SystemExit(f"unknown program {program!r}; one of {', '.join(programs())}")
    os.makedirs(out, exist_ok=True)
    # Full paths from PATH (PATHEXT aware): on Windows CreateProcess only appends ".exe" to a
    # name without an extension, so a bare "ld.lld" is not found although ld.lld.exe is on PATH.
    clang, lld, objcopy, readelf = (dt.tool(n) for n in ("clang", "ld.lld", "llvm-objcopy", "llvm-readelf"))
    objs = []
    for src in (os.path.join(HERE, f"{program}.c"), dt.RT_SOURCE):
        obj = os.path.join(out, os.path.basename(src) + ".o")
        subprocess.run([clang, *dt.CFLAGS, f"-I{HERE}", "-c", src, "-o", obj], check=True)
        objs.append(obj)
    script = os.path.join(out, f"{program}.ld")
    with open(script, "w") as f:
        f.write(LINKER_SCRIPT)
    elf = os.path.join(out, f"{program}.elf")
    subprocess.run([lld, "-T", script, *objs, "-o", elf], check=True)
    image = os.path.join(out, f"{program}.bin")
    subprocess.run([objcopy, "-O", "binary", elf, image], check=True)

    entries, text_end = {}, BASE
    for line in subprocess.run([readelf, "-sS", elf], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[3] == "FUNC":
            a = int(parts[1], 16)
            entries[a & ~1] = "thumb" if a & 1 else "arm"
        if "] .text" in line:
            i = parts.index(".text")
            text_end = int(parts[i + 2], 16) + int(parts[i + 4], 16)
    with open(image, "rb") as f:
        code = f.read()
    res = recompile.recompile(Image(code, BASE, text_end - BASE), entries, out, log=lambda *a: None)
    if res["problems"] or res["report"]["trap_instructions"]:
        raise SystemExit(f"recompilation problems: {res['problems']}")
    return elf


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build", "port-test"))
    ap.add_argument("--program", default="svc_test", help="a program name (see --list)")
    ap.add_argument("--list", action="store_true", help="print the program names and exit")
    args = ap.parse_args(argv)
    if args.list:
        print("\n".join(programs()))
        return 0
    print(build(args.out, args.program))
    return 0


if __name__ == "__main__":
    sys.exit(main())
