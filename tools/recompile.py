#!/usr/bin/env python3
"""Recompile the game's ARM code into C (the generated code is never committed).

usage: recompile.py [--exheader ...] [--code ...] [--functions config/functions.tsv]
                    [--extra build/export/missing2.txt] -o build/recomp
Writes <out>/src/funcs.h, <out>/src/fNNN.c, <out>/src/table.c and <out>/report.txt.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import os
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import build_function_table as bft  # noqa: E402
import codebin2elf  # noqa: E402
import exheader as exh  # noqa: E402
from recomp.analysis import Analyzer, Image  # noqa: E402
from recomp.emit import FunctionEmitter, fname  # noqa: E402

FILE_SHIFT = 16


def load_entries(functions_tsv: str, extra: list[str]) -> dict[int, str]:
    entries = {}
    for r in bft.read_tsv(functions_tsv):
        entries[int(r["address"], 16)] = r["mode"]
    for path in extra:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    parts = line.split()
                    a = int(parts[0], 16)
                    mode = "thumb" if (a & 1 or (len(parts) > 1 and parts[1] == "thumb")) else "arm"
                    entries.setdefault(a & ~1, mode)
    return entries


def code_cover(funcs: dict) -> tuple[dict[str, set[int]], dict[str, set[int]]]:
    """Halfword addresses covered by decoded instructions, and addresses that some
    instruction falls through into, per instruction set."""
    em = FunctionEmitter({})
    cover = {"arm": set(), "thumb": set()}
    fall_into = {"arm": set(), "thumb": set()}
    for f in funcs.values():
        cv, fi = cover[f.mode], fall_into[f.mode]
        for a, ins in f.insns.items():
            cv.update(range(a, a + ins.size, 2))
            # calls may not return (assert handlers), so code after a call can be a function
            if em.falls_through(f, ins) and ins.kind not in ("bl", "icall") and a not in f.link_calls:
                fi.add(ins.next)
    return cover, fall_into


def near(sorted_addrs: list[int], a: int, distance: int) -> bool:
    i = bisect.bisect_left(sorted_addrs, a)
    return any(0 <= j < len(sorted_addrs) and abs(sorted_addrs[j] - a) <= distance for j in (i - 1, i))


def looks_like_arm(img: Image, a: int) -> bool:
    """Most ARM instructions are unconditional (condition field 0xE); in Thumb code a
    word with that top nibble is the exception."""
    words = [img.read32((a & ~3) + 4 * k) for k in range(-8, 8)]
    return sum(1 for w in words if w is not None and w >> 28 == 0xE) >= 10


def data_pointer_entries(img: Image, an: Analyzer, segments: list[tuple[int, int]],
                         known: dict[int, str], cover: dict[str, set[int]],
                         fall_into: dict[str, set[int]]) -> dict[int, str]:
    """Code addresses stored in data (vtables, callback tables): words in .rodata/.data
    that point into .text at an instruction that starts clean, trap-free code. Words that
    are really numbers are weeded out: the target must not be reached by falling through
    from the instruction before it, and the code must not overlap code of the other
    instruction set."""
    found: dict[int, str] = {}
    thumb_entries = sorted(a for a, m in known.items() if m == "thumb")
    for start, size in segments:
        for a in range(start, start + size - 3, 4):
            v = img.read32(a)
            if v is None or not img.in_text(v & ~1):
                continue
            t, m = (v & ~1, "thumb") if v & 1 else (v, "arm")
            if (m == "arm" and t & 3) or t in known or t in found or t in fall_into[m]:
                continue
            if m == "thumb" and (looks_like_arm(img, t) or not near(thumb_entries, t, 0x4000)):
                continue    # odd pointers into data; real Thumb code sits next to other Thumb code
            ins = an.decode(t, m)
            if ins.kind == "trap" or (m == "arm" and ins.word >> 28 != 0xE):
                continue
            f = an.analyze(t, m, known)
            if f.problems or any(i.kind == "trap" for i in f.insns.values()):
                continue
            other = cover["thumb" if m == "arm" else "arm"]
            if any(h in other for x, i in f.insns.items() for h in range(x, x + i.size, 2)):
                continue
            found[t] = m
    return found


def recompile(img: Image, entries: dict[int, str], out: str, log=print,
              data_segments: list[tuple[int, int]] | None = None) -> dict:
    t0 = time.time()
    an = Analyzer(img)
    outside = [a for a in entries if not img.in_text(a)]
    if outside:
        entries = {a: m for a, m in entries.items() if img.in_text(a)}
        log(f"  ignoring {len(outside)} entry points outside .text")
    extra = {}
    if data_segments:
        first = an.analyze_all(entries)
        cover, fall_into = code_cover(first)
        known = {a: f.mode for a, f in first.items()}
        extra = data_pointer_entries(img, an, data_segments, known, cover, fall_into)
        entries = {**entries, **extra}
        log(f"  {len(extra)} entry points found through data pointers")
    funcs = an.analyze_all(entries, progress=lambda n, m: log(f"  analysed {n}/{m}"))
    t1 = time.time()
    known = {a: f.mode for a, f in funcs.items()}
    em = FunctionEmitter(known)
    src = os.path.join(out, "src")
    os.makedirs(src, exist_ok=True)

    def write_if_changed(path: str, text: str) -> None:
        if os.path.exists(path):
            with open(path) as fh:
                if fh.read() == text:
                    return
        with open(path, "w") as fh:
            fh.write(text)

    order = sorted(funcs)
    write_if_changed(os.path.join(src, "funcs.h"),
                     '#include "recomp.h"\n' + "".join(f"void {fname(a)}(RCtx *ctx);\n" for a in order))
    stats = collections.Counter()
    problems = []
    files = []
    # one file per 64 KiB of code, so adding or changing functions only rebuilds their file
    buckets = collections.defaultdict(list)
    for a in order:
        buckets[a >> FILE_SHIFT].append(a)
    for bucket, addrs in sorted(buckets.items()):
        parts = ['#include "funcs.h"\n']
        for a in addrs:
            f = funcs[a]
            parts.append("\n".join(em.function(f)))
            stats["insns"] += len(f.insns)
            stats["traps"] += sum(1 for i in f.insns.values() if i.kind == "trap")
            stats["jump_tables"] += len(f.jtabs)
            problems += [f"{fname(a)}: {p}" for p in f.problems]
        path = os.path.join(src, f"f{bucket << FILE_SHIFT:08x}.c")
        write_if_changed(path, "\n".join(parts) + "\n")
        files.append(path)
    keep = {os.path.basename(p) for p in files} | {"funcs.h", "table.c"}
    for old in os.listdir(src):
        if old not in keep:
            os.remove(os.path.join(src, old))
    table = ['#include "funcs.h"', "", "const RFuncEntry rc_func_table[] = {"]
    table += [f"    {{0x{a:08x}u, {1 if funcs[a].mode == 'thumb' else 0}, {fname(a)}}}," for a in order]
    table += ["};", f"const u32 rc_func_count = {len(funcs)};",
              "/* the code this was recompiled from; runtimes check it before running */",
              f"const u32 rc_text_base = 0x{img.base:08x}u;",
              f"const u32 rc_text_size = 0x{img.text_size:08x}u;",
              f"const u32 rc_text_crc32 = 0x{zlib.crc32(img.code[:img.text_size]):08x}u;", ""]
    write_if_changed(os.path.join(src, "table.c"), "\n".join(table))
    t2 = time.time()
    report = {
        "functions": len(funcs), "discovered": an.discovered, "from_data_pointers": len(extra), "instructions": stats["insns"],
        "trap_instructions": stats["traps"], "jump_tables": stats["jump_tables"],
        "problems": len(problems), "c_files": len(files) + 1,
        "analysis_s": round(t1 - t0, 1), "emit_s": round(t2 - t1, 1),
    }
    with open(os.path.join(out, "report.txt"), "w") as fh:
        for k, v in report.items():
            fh.write(f"{k}: {v}\n")
        fh.write("\n".join(problems) + "\n")
    return {"report": report, "funcs": funcs, "files": files, "problems": problems}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exheader", default=os.path.join(ROOT, "baserom", "update_v130", "exheader.bin"))
    ap.add_argument("--code", default=os.path.join(ROOT, "baserom", "update_v130", "code.bin"))
    ap.add_argument("--functions", default=os.path.join(ROOT, "config", "functions.tsv"))
    ap.add_argument("--extra", action="append", default=[])
    ap.add_argument("--no-data-pointers", action="store_true", help="don't scan .rodata/.data for code pointers")
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build", "recomp"))
    args = ap.parse_args(argv)
    with open(args.exheader, "rb") as f:
        header = exh.parse(f.read())
    with open(args.code, "rb") as f:
        code = codebin2elf.load_code(header, f.read())
    img = Image(code, header.text.address, header.text.size)
    segs = None if args.no_data_pointers else [(header.ro.address, header.ro.size), (header.data.address, header.data.size)]
    res = recompile(img, load_entries(args.functions, args.extra), args.out, data_segments=segs)
    for k, v in res["report"].items():
        print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
