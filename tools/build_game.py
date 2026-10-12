#!/usr/bin/env python3
"""One command from your own dump to the PC port's code library.

    python3 tools/build_game.py --dump "MH4U v1.3.0 update.cia" [--opt O1] [--install DIR]

1. extracts exheader + ExeFS code from your decrypted dump (.cia update, .3ds/.cci, .cxi),
2. checks it is the version this project targets (config/versions.json),
3. recompiles it to C (build/game/recomp/src, never shared),
4. compiles the code library (clang; on Windows llvm-mingw's x86_64-w64-mingw32-clang),
5. optionally copies it to <Azahar user dir>/recomp/<program id>.<so|dll|dylib>, where the
   port's Azahar build picks it up.

Everything this writes contains game code and stays on your machine.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import codebin2elf  # noqa: E402
import exheader as exh  # noqa: E402
import extract_ncch  # noqa: E402
import recompile  # noqa: E402
from recomp import build as rbuild  # noqa: E402
from recomp.analysis import Image  # noqa: E402


def library_name(program_id: int) -> str:
    ext = "dll" if sys.platform.startswith("win") else "dylib" if sys.platform == "darwin" else "so"
    return f"{program_id:016X}.{ext}"


def find_compiler(requested: str | None) -> str:
    if requested:
        return requested
    candidates = ["x86_64-w64-mingw32-clang", "clang"] if sys.platform.startswith("win") else ["clang", "cc"]
    for c in candidates:
        if shutil.which(c):
            return c
    raise SystemExit("no C compiler found: install LLVM (Linux/macOS: clang; Windows: llvm-mingw from "
                     "https://github.com/mstorsjo/llvm-mingw/releases, then put its bin/ on PATH)")


def check_version(header: exh.ExHeader, code: bytes) -> dict | None:
    with open(os.path.join(ROOT, "config", "versions.json")) as f:
        versions = json.load(f)["versions"]
    sha1 = hashlib.sha1(code).hexdigest()
    for v in versions:
        if v.get("code_sha1") == sha1:
            return v
    return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dump", required=True, help="decrypted .cia / .3ds / .cci / .cxi of your own copy")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "game"))
    ap.add_argument("--opt", default="O1", help="O0 builds fast, O1/O2 run faster")
    ap.add_argument("--cc", help="C compiler (default: clang, or x86_64-w64-mingw32-clang on Windows)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--install", metavar="AZAHAR_USER_DIR",
                    help="copy the library to AZAHAR_USER_DIR/recomp/ (Azahar's user directory)")
    ap.add_argument("--any-version", action="store_true", help="don't insist on a known code.bin")
    ap.add_argument("--functions", default=os.path.join(ROOT, "config", "functions.tsv"), help=argparse.SUPPRESS)
    ap.add_argument("--extra", default=os.path.join(ROOT, "config", "extra_entries.txt"), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    dump_dir = os.path.join(args.out, "dump")
    print(f"extracting {args.dump}")
    extract_ncch.extract(args.dump, dump_dir)
    with open(os.path.join(dump_dir, "exheader.bin"), "rb") as f:
        exheader_bytes = f.read()
    header = exh.parse(exheader_bytes)
    with open(os.path.join(dump_dir, "code.bin"), "rb") as f:
        code = codebin2elf.load_code(header, f.read())
    version = check_version(header, code)
    if version:
        print(f"version: {version['name']}")
    elif not args.any_version:
        raise SystemExit("this code.bin isn't a version the project knows (config/versions.json); "
                         "the function lists are for USA v1.3.0 (use the v1.3.0 update .cia)")
    program_id = struct.unpack_from("<Q", exheader_bytes, 0x200)[0]

    out = os.path.join(args.out, "recomp")
    print("recompiling (a couple of minutes)")
    img = Image(code, header.text.address, header.text.size)
    entries = recompile.load_entries(args.functions, [args.extra] if args.extra else [])
    segs = [(header.ro.address, header.ro.size), (header.data.address, header.data.size)]
    res = recompile.recompile(img, entries, out, data_segments=segs, title_id=program_id,
                              log=lambda m: print(m, flush=True))
    print(f"{res['report']['functions']} functions, {res['report']['problems']} problems")

    cc = find_compiler(args.cc)
    name = library_name(program_id)
    print(f"compiling with {cc} -{args.opt} ({args.jobs} jobs; -O1 takes a while)")
    lib = rbuild.build(out, args.jobs, args.opt, ["RC_PAGED"], name, [rbuild.DLL_GLUE], cc=cc,
                       extra_flags=["-fvisibility=hidden"])
    print(f"code library: {lib}")
    if args.install:
        dest_dir = os.path.join(args.install, "recomp")
        os.makedirs(dest_dir, exist_ok=True)
        shutil.copy2(lib, os.path.join(dest_dir, name))
        print(f"installed to {os.path.join(dest_dir, name)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
