#!/usr/bin/env python3
# Derived in part from Azahar src/audio_core/hle/shared_memory.h and src/audio_core/hle/hle.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "Licensed under GPLv2 or any later version"): it uses dsp_oracle.py's reading of the offsets in the shared memory, and compiles Azahar's own DSP code through the replay driver (which includes the oracle's own copy of the frame code).
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
# Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
"""A DSP capture made by Azahar's own DSP code (N3-04's second oracle).

    replay_capture.py --azahar ~/azahar --cases tools/native/dsp_oracle/replay_cases.txt --out build/x.mdsp

Plays the frame case of the cases file (the language of dsp_oracle.py, with the `read` lines of
replay_cases.txt) on Azahar's source, mixer and effect code through replay_driver.cpp, and writes an MDSP
capture (tools/native/capfmt.py's format) as the reference runner's --capture-dsp does: per frame the
type-1 record (both regions as the frame starts), the type-3 records of the sample data the frame read
(bytes only the first time an (address, size, CRC) key appears, which capfmt.Writer does) and the type-2
record (the 160 stereo samples Azahar's mixer made). `mh4n_dsp_replay` must replay it bit-exact.

What the oracle gives and what it does not: the regions (statuses written by Azahar's sources, the aux
buses' intermediate samples, the frame counters) and the output samples are Azahar's own; the sample reads
are named by the script (`read` lines), because Azahar's source.cpp reads through a stand-in memory that
cannot report them. The capture holds synthetic data only.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_CASES = os.path.join(HERE, "replay_cases.txt")
REGIONS = 2 * 0x8000
PCM = 640
FRAME_TICKS = 1310720


def _load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module     # dataclasses look their module up
    spec.loader.exec_module(module)
    return module


oracle = sys.modules.get("dsp_oracle") or _load("dsp_oracle", os.path.join(HERE, "dsp_oracle.py"))
capfmt = sys.modules.get("capfmt") or _load("capfmt", os.path.join(ROOT, "tools", "native", "capfmt.py"))


class ReplayError(Exception):
    pass


def split_reads(text: str) -> tuple[str, list[tuple[int, int, int, bool]]]:
    """The cases text without its `read` lines, and the reads: (frame, address, bytes, every)."""
    kept: list[str] = []
    reads: list[tuple[int, int, int, bool]] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        words = line.split()
        if words and words[0] == "read":
            if len(words) not in (4, 5) or (len(words) == 5 and words[4] != "every"):
                raise ReplayError(f"read <frame> <address> <bytes> [every]: {raw!r}")
            reads.append((oracle.integer(words[1]), oracle.integer(words[2]), oracle.integer(words[3]),
                          len(words) == 5))
        else:
            kept.append(raw)
    return "\n".join(kept) + "\n", reads


def compile_replay_driver(tree: str, cxx: str) -> str:
    exe = os.path.join(tree, "replay_driver")
    src = os.path.join(tree, "src")
    cmd = [cxx, "-std=c++20", "-O1", "-ffp-contract=off", "-fwrapv", "-Wall", "-Wno-unused", "-I", src,
           "-I", os.path.join(src, "common"), "-I", HERE, *(os.path.join(src, s) for s in oracle.SOURCES),
           os.path.join(HERE, "replay_driver.cpp"), "-o", exe]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError("compiling Azahar's source code failed:\n" + done.stderr[-4000:])
    return exe


def make_capture(azahar: str, cases_text: str, out_path: str, cxx: str = "clang++", keep: str | None = None) -> dict:
    """Runs the scenario and writes the capture; returns what it holds (counts only)."""
    text, reads = split_reads(cases_text)
    cases = oracle.parse_cases(text)
    if len(cases) != 1 or cases[0].mode != "frame":
        raise ReplayError("the scenario is exactly one frame case")
    case = cases[0]
    azahar = os.path.abspath(os.path.expanduser(azahar))
    if oracle.azahar_commit(azahar) != oracle.pinned_commit():
        raise ReplayError(f"{azahar} is not at the pinned commit {oracle.pinned_commit()}")

    tmp = keep or tempfile.mkdtemp(prefix="dsp_replay_oracle_")
    try:
        os.makedirs(tmp, exist_ok=True)
        oracle.prepare_tree(azahar, tmp)
        exe = compile_replay_driver(tmp, cxx)
        raw_path = os.path.join(tmp, "frames.bin")
        done = subprocess.run([exe, raw_path], input=oracle.driver_input([case]), capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(f"the driver failed ({done.returncode}):\n{done.stderr[-2000:]}")
        with open(raw_path, "rb") as f:
            raw = f.read()
    finally:
        if keep is None:
            shutil.rmtree(tmp, ignore_errors=True)

    asleep = [e for e in case.events if e.op in "SW"]
    if asleep:
        raise ReplayError("the scenario sleeps the DSP: frames without a record would shift the reads' frames")
    frames = len(raw) // (REGIONS + PCM)
    if frames != case.frames or len(raw) != frames * (REGIONS + PCM):
        raise ReplayError(f"the driver wrote {len(raw)} bytes for {case.frames} frames")

    # The memory the sample reads see: the case's fills, then the game's pokes at their frames.
    memory = bytearray(oracle.WINDOW)
    for fill in case.fills:
        data = fill.data()
        memory[fill.address - oracle.FCRAM:fill.address - oracle.FCRAM + len(data)] = data
    pokes: dict[int, list] = {}
    for e in case.events:
        if e.op == "m":
            pokes.setdefault(e.frame, []).append(e)

    counts = {1: 0, 2: 0, 3: 0, "bytes": 0}
    with capfmt.Writer(out_path, "dsp", tool="replay_capture.py", manifest=True) as w:
        for frame in range(frames):
            for e in pokes.get(frame, []):
                memory[e.offset - oracle.FCRAM:e.offset - oracle.FCRAM + len(e.data)] = e.data
            base = frame * (REGIONS + PCM)
            ticks = frame * FRAME_TICKS
            w.dsp_regions(frame // 2, ticks, raw[base:base + 0x8000], raw[base + 0x8000:base + REGIONS])
            counts[1] += 1
            for at, address, size, every in reads:
                if at == frame or (every and at < frame):
                    offset = address - oracle.FCRAM
                    if offset < 0 or offset + size > len(memory):
                        raise ReplayError(f"read of {size} bytes at {address:#x} is outside the case's memory")
                    new = w.dsp_samples(frame // 2, ticks, address, bytes(memory[offset:offset + size]))
                    counts[3] += 1
                    counts["bytes"] += 1 if new else 0
            w.dsp_pcm(frame // 2, ticks, raw[base + REGIONS:base + REGIONS + PCM])
            counts[2] += 1
    return counts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True, help="Azahar's sources at the pinned commit (~/azahar)")
    parser.add_argument("--cases", default=DEFAULT_CASES)
    parser.add_argument("--out", required=True, help="the MDSP capture to write")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--keep", help="build in this folder and keep it")
    args = parser.parse_args(argv)
    try:
        with open(args.cases, encoding="utf-8") as f:
            text = f.read()
        counts = make_capture(args.azahar, text, args.out, args.cxx, args.keep)
    except (ReplayError, oracle.CaseError, RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(f"replay_capture: {error}", file=sys.stderr)
        return 1
    print(f"replay_capture: {counts[1]} regions, {counts[2]} pcm, {counts[3]} sample reads "
          f"({counts['bytes']} with bytes) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
