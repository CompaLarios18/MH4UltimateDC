#!/usr/bin/env python3
"""Test data for native/tests/interp_test.cpp (N1-28): synthetic only, made from the repository's own test program and
decoders.

  svc_test.elf, svc_all.so, svc_workers.so, svc_none.so
      the 3DS test program port/tests/svc_test.c and three native code libraries made from it: every function
      recompiled; without its worker functions and the functions it calls through pointers; without any function
      (the whole program runs in the interpreter).
  vectors.bin
      random states run through the recompiled C of random instruction words of every form (the C the
      recompiler emits, which the handlers are generated from), with the results: what the interpreter must
      reproduce. Format (little endian u32 unless noted): magic "IHV1", image count, then per image
      (base, size, bytes padded to 4); vector count, then per vector
          image, address, thumb, word, kind (0 return, 1 svc, 2 trap),
          in : r0-r14, n z c v q ge, s0-s31, fpscr, tls, seed,
          out: kind 0: r0-r15, n z c v q ge, s0-s31, fpscr, crc32 of the 64 KiB buffer; kind 1: imm, next pc; kind 2: pc, code.
      The buffer at 0x30000000 is zero except the 16 KiB at +0x6000, which holds the output of the generator
      x = x * 1664525 + 1013904223 (mod 2^32), 4096 words, started from `seed`.

usage: make_test_data.py OUTDIR [--vectors N] [--seed S] [--cc CC] [--no-vectors]

--cc builds the code libraries with another compiler (llvm-mingw's, for a Windows .dll). --no-vectors leaves out
vectors.bin: the harness that makes it (tools/recomp/harness/harness.c) runs on Linux and macOS only.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
TOOLS = os.path.join(ROOT, "tools")
PORT_TESTS = os.path.join(ROOT, "port", "tests")
sys.path.insert(0, TOOLS)
sys.path.insert(0, PORT_TESTS)

BUF_BASE, BUF_SIZE, WINDOW, WINDOW_WORDS = 0x30000000, 0x10000, 0x6000, 4096
TLS = 0x1FF80000


def fill_window(seed: int) -> bytes:
    x, out = seed & 0xFFFFFFFF, []
    for _ in range(WINDOW_WORDS):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(x)
    return struct.pack(f"<{WINDOW_WORDS}I", *out)


def buffer(seed: int) -> bytes:
    return bytes(WINDOW) + fill_window(seed) + bytes(BUF_SIZE - WINDOW - 4 * WINDOW_WORDS)


def build_svc_libraries(out: str, cc: str = "clang") -> None:
    import build_svc_test
    from recomp import build as rbuild
    elf = build_svc_test.build(out, "svc_test")
    if os.path.abspath(elf) != os.path.abspath(os.path.join(out, "svc_test.elf")):
        raise SystemExit(f"unexpected ELF path {elf}")
    syms = {}
    for line in subprocess.run(["llvm-readelf", "-s", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC":
            syms[p[7]] = int(p[1], 16) & ~1

    def lib(name: str) -> None:
        got = rbuild.build(out, 2, "O1", rbuild.native_defines(True, False), name, [rbuild.DLL_GLUE], cc=cc,
                           extra_flags=["-fvisibility=hidden"])
        if os.path.abspath(got) != os.path.abspath(os.path.join(out, name)):
            raise SystemExit(f"unexpected library path {got}")

    table = os.path.join(out, "src", "table.c")
    with open(table, encoding="utf-8") as f:
        original = f.read().splitlines()
    lib("svc_all.so")

    def write_table(lines: list[str]) -> None:
        count = sum(1 for ln in lines if ln.strip().startswith("{0x"))
        text = "\n".join(re.sub(r"rc_func_count = \d+;", f"rc_func_count = {count};", ln) for ln in lines) + "\n"
        with open(table, "w", encoding="utf-8") as f:
            f.write(text)

    drop = {syms[n] for n in ("worker", "op_add", "op_xor", "op_mul")}
    write_table([ln for ln in original if not any(f"{{0x{a:08x}u," in ln for a in drop)])
    lib("svc_workers.so")
    write_table([ln for ln in original if not ln.strip().startswith("{0x")])
    lib("svc_none.so")
    write_table(original)


# ── the vectors ─────────────────────────────────────────────────────────────────────────

def make_vectors(out: str, per_form: int, seed: int) -> int:
    from recomp import fuzz, interp_gen as ig
    from recomp.arm import ArmTranslator
    from recomp.harness import machines
    from recomp.insn import OP, TRAP
    from recomp.thumb import ThumbTranslator

    rng = random.Random(f"n1-28-vectors:{seed}")
    images = {"arm": (0x00100000, bytearray()), "thumb": (0x00200000, bytearray())}
    entries: list[tuple[str, int, int, int]] = []        # (mode, offset, word, vec)
    for form in ig.ALL_FORMS:
        step = 2 if form.mode == "thumb" else 4
        for word, vec in ig.sample_words(form, rng, per_form):
            wide = form.mode == "thumb" and word > 0xFFFF
            img = images[form.mode][1]
            entries.append((form.mode, len(img), word, vec))
            img += struct.pack("<HH", word & 0xFFFF, word >> 16) if wide else struct.pack("<I" if step == 4 else "<H", word)
    # words that decode to a trap, or that are undecodable, so the dispatcher's default route is covered
    for _ in range(per_form):
        w = rng.getrandbits(32)
        img = images["arm"][1]
        entries.append(("arm", len(img), w, 0))
        img += struct.pack("<I", w)

    code = {m: bytes(images[m][1]) for m in images}

    def readers(mode: str):
        base, data = images[mode][0], code[mode]

        def read32(a: int):
            off = a - base
            return struct.unpack_from("<I", data, off)[0] if 0 <= off <= len(data) - 4 else None

        def read16(a: int):
            off = a - base
            return struct.unpack_from("<H", data, off)[0] if 0 <= off <= len(data) - 2 else None
        return read16, read32

    arm_tr = ArmTranslator(readers("arm")[1])
    th_tr = ThumbTranslator(*readers("thumb"))
    items = []
    for mode, off, word, vec in entries:
        base = images[mode][0]
        ins = arm_tr.decode(base + off, word, vec) if mode == "arm" else th_tr.decode(base + off)
        if ins.kind == TRAP and ins.trap_reason.startswith("unpredictable"):
            continue
        try:
            ref = ig.reference_c(ins)
        except ig.GenError:
            continue
        items.append((mode, ins, word, vec, ref))

    c = ['#include "recomp.h"']
    table = []
    for i, (mode, ins, word, vec, ref) in enumerate(items):
        c.append(f"static void rf_{i}(RCtx *ctx) {{ {' '.join(ref)} }}")
        table.append((ins.addr, 1 if mode == "thumb" else 0, i))
    table.sort()
    c.append("const RFuncEntry rc_func_table[] = {")
    c += [f"  {{0x{a:08x}u, {m}, rf_{i}}}," for a, m, i in table]
    c += ["};", f"const u32 rc_func_count = {len(table)};", "const u32 rc_hook_addrs[] = {0};",
          "const u32 rc_hook_count = 0;", "RHookFn rc_hooks[1];", "u32 rc_tunables[1];",
          "const u32 rc_tunable_addrs[] = {0};", "const char *const rc_tunable_names[] = {0};",
          "const u32 rc_tunable_count = 0;"]
    work = os.path.join(out, "vectors-work")
    os.makedirs(work, exist_ok=True)
    src, lib = os.path.join(work, "ref.c"), os.path.join(work, "ref.so")
    with open(src, "w") as f:
        f.write("\n".join(c) + "\n")
    r = subprocess.run(["clang", "-O0", *[x for x in fuzz.CFLAGS if not x.startswith("-O")], "-w", fuzz.HARNESS, src,
                        "-o", lib, "-lm"], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr[-3000:])
    regions = []
    for m in ("arm", "thumb"):
        base, data = images[m]
        regions.append(machines.Region(base, (len(data) + 0x1FFF) & ~0xFFF, False, code[m]))
    regions += [machines.Region(BUF_BASE, BUF_SIZE, True), machines.Region(TLS, 0x1000, True)]
    nat = machines.NativeMachine(lib, regions)
    nat.lib.rh_set_check_return(0)

    image_list = [images["arm"][0], images["thumb"][0]]
    blob = bytearray(b"IHV1" + struct.pack("<I", 2))
    for m in ("arm", "thumb"):
        data = code[m] + bytes(-len(code[m]) % 4)
        blob += struct.pack("<II", images[m][0], len(data)) + data
    vectors = bytearray()
    count = 0
    for mode, ins, word, vec, ref in items:
        for _ in range(3):
            st = fuzz.random_state(rng)
            st.fpscr |= vec
            seed = rng.getrandbits(32) | 1
            nat.write(BUF_BASE, buffer(seed))
            res = nat.run(ins.addr, mode == "thumb", st)
            if res.status == machines.ST_RETURN:
                kind = 0
            elif res.status == machines.ST_SVC:
                kind = 1
            elif res.status == machines.ST_TRAP:
                kind = 2
            else:
                continue                      # a fault: the interpreter would end the process
            rec = struct.pack("<5I", image_list[0 if mode == "arm" else 1], ins.addr, int(mode == "thumb"), ins.word, kind)
            rec += struct.pack("<15I", *st.r[:15])
            rec += struct.pack("<6I", st.n, st.z, st.c, st.v, st.q, st.ge)
            rec += struct.pack("<32I", *st.s) + struct.pack("<3I", st.fpscr, st.tls, seed)
            if kind == 0:
                rec += struct.pack("<16I", *res.state.r)
                rec += struct.pack("<6I", res.state.n, res.state.z, res.state.c, res.state.v, res.state.q, res.state.ge)
                rec += struct.pack("<32I", *res.state.s) + struct.pack("<I", res.state.fpscr)
                rec += struct.pack("<I", zlib.crc32(nat.read(BUF_BASE, BUF_SIZE)))
            else:
                rec += struct.pack("<2I", res.info0, res.info1)
            vectors += rec
            count += 1
    blob += struct.pack("<I", count) + vectors
    with open(os.path.join(out, "vectors.bin"), "wb") as f:
        f.write(blob)
    return count


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--vectors", type=int, default=24, help="encodings per form")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--no-vectors", action="store_true")
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    build_svc_libraries(args.out, args.cc)
    if not args.no_vectors:
        print(f"{make_vectors(args.out, args.vectors, args.seed)} vectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
