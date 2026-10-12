#!/usr/bin/env python3
"""Compile recompiled C (from tools/recompile.py) plus a runtime/harness into a shared library.

usage: python3 -m recomp.build build/recomp [--harness] [-j N] [--opt O1] [-o game.so]
Incremental: objects newer than their source and recomp.h are reused.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")
HARNESS = os.path.join(HERE, "harness", "harness.c")
BASE_FLAGS = ["-c", "-fPIC", "-ffp-contract=off", "-fno-strict-aliasing", "-fwrapv", "-w", f"-I{RUNTIME}"]


def compile_one(src: str, obj: str, flags: list[str]) -> tuple[str, float, str]:
    t = time.time()
    r = subprocess.run(["clang", *flags, src, "-o", obj], capture_output=True, text=True)
    return src, time.time() - t, (r.stderr if r.returncode else "")


def build(outdir: str, jobs: int, opt: str, defines: list[str], lib_name: str, extra_sources: list[str]) -> str:
    src_dir = os.path.join(outdir, "src")
    obj_dir = os.path.join(outdir, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    # funcs.h only gains or loses declarations; a file whose calls change is rewritten itself
    hdr_time = os.path.getmtime(os.path.join(RUNTIME, "recomp.h"))
    flags = [f"-{opt}", *BASE_FLAGS, f"-I{src_dir}", *[f"-D{d}" for d in defines]]
    sources = sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith(".c")) + extra_sources
    todo, objs = [], []
    for s in sources:
        tag = "-".join(d.split("=")[0] for d in defines)
        o = os.path.join(obj_dir, f"{os.path.basename(s)[:-2]}{'-' + tag if tag else ''}-{opt}.o")
        objs.append(o)
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(s), hdr_time):
            todo.append((s, o))
    t0 = time.time()
    errors = []
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(compile_one, s, o, flags) for s, o in todo]
        for i, fut in enumerate(cf.as_completed(futs), 1):
            src, dt, err = fut.result()
            if err:
                errors.append((src, err))
            if i % 20 == 0 or i == len(futs):
                print(f"  compiled {i}/{len(futs)} ({time.time() - t0:.0f}s)", flush=True)
    if errors:
        for src, err in errors[:5]:
            print(f"error in {src}:\n{err[-2000:]}", file=sys.stderr)
        raise SystemExit(1)
    lib = os.path.join(outdir, lib_name)
    r = subprocess.run(["clang", "-shared", *objs, "-o", lib, "-lm"], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr[-4000:])
    print(f"linked {lib} ({len(objs)} objects, {len(todo)} recompiled in {time.time() - t0:.0f}s)")
    return lib


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--opt", default="O1")
    ap.add_argument("--harness", action="store_true", help="link the test harness (Linux/macOS)")
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("-o", "--lib", default="game.so")
    args = ap.parse_args(argv)
    extra = [HARNESS] if args.harness else []
    build(args.outdir, args.jobs, args.opt, args.defines, args.lib, extra)
    return 0


if __name__ == "__main__":
    sys.exit(main())
