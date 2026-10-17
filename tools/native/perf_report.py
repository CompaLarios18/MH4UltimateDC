#!/usr/bin/env python3
"""Report on a per-frame performance log (mh4u_runner --perf-log, mh4u-app.ini's perf_log; N0-15).

The log is CSV, one row per frame (a vertical blank to the next):

    # perf v1 <ISO date> game=0x<title id> cpu=<cpu> new3ds=<on|off> ...
    frame,present_us,recomp_us,svc_us,gpu_us
    0,8333.512,2210.004,640.220,1105.931
    ...
    # frames=<n> dropped=<n>

`present_us` is the time from one frame to the next, `recomp_us` the time in the game's
recompiled code, `svc_us` the time in system calls and `gpu_us` the time in the GPU's command
processing on the emulation thread (each microsecond counted once, in the innermost of the three).
Lines starting with `#` are metadata. Columns ending in `_us` are reported in milliseconds.

The report adds the derived column `other_us` = present_us - recomp_us - svc_us - gpu_us (never
below 0): what no timer covers, mostly waiting for the renderer's worker threads, the frame
limiter and vsync. Azahar has no GPU timestamps, so this is the closest to "GPU time" there is.

For every column: p50, p95, p99 and max (nearest-rank percentiles), then the frames over the
frame budget (--budget-ms, or --fps: 1000 / fps) in the --column (default present_us) and the
frames over 50 ms (a stutter). The output is a markdown table.

usage: perf_report.py CSV [--budget-ms MS | --fps FPS] [--column NAME] [--skip N]
exit status: 0 reported, 2 the file is not a performance log.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from typing import Sequence

PRESENT = "present_us"
PARTS = ("recomp_us", "svc_us", "gpu_us")
OTHER = "other_us"
STUTTER_MS = 50.0
DEFAULT_FPS = 120.0
PERCENTILES = (50, 95, 99)


class FormatError(ValueError):
    """A file that is not a performance log."""


def parse(text: str) -> tuple[list[str], list[list[float]], list[str]]:
    """The column names, the rows (floats) and the `#` metadata lines of a log."""
    columns: list[str] = []
    rows: list[list[float]] = []
    meta: list[str] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            meta.append(line)
            continue
        fields = [f.strip() for f in line.split(",")]
        if not columns:
            if fields[0] != "frame" or len(fields) < 2 or len(set(fields)) != len(fields):
                raise FormatError(f"line {number}: expected the header 'frame,{PRESENT},...', got {line!r}")
            columns = fields
            continue
        if len(fields) != len(columns):
            raise FormatError(f"line {number}: {len(fields)} fields, the header has {len(columns)}")
        try:
            values = [float(f) for f in fields]
        except ValueError:
            raise FormatError(f"line {number}: not numbers: {line!r}") from None
        if any(not math.isfinite(v) or v < 0 for v in values):
            raise FormatError(f"line {number}: negative or non-finite value: {line!r}")
        rows.append(values)
    if not columns:
        raise FormatError("no header line (frame,present_us,...)")
    if PRESENT not in columns:
        raise FormatError(f"no {PRESENT} column")
    return columns, rows, meta


def percentile(sorted_values: Sequence[float], p: float) -> float:
    """Nearest-rank percentile of an ascending list."""
    if not sorted_values:
        raise ValueError("no values")
    rank = max(1, math.ceil(p / 100.0 * len(sorted_values)))
    return sorted_values[min(rank, len(sorted_values)) - 1]


def columns_of(columns: list[str], rows: list[list[float]]) -> dict[str, list[float]]:
    """name -> values, without `frame`, with `other_us` derived when the parts are there."""
    data = {name: [r[i] for r in rows] for i, name in enumerate(columns) if name != "frame"}
    if OTHER not in data and all(p in data for p in PARTS):
        data[OTHER] = [max(0.0, pr - sum(data[p][i] for p in PARTS)) for i, pr in enumerate(data[PRESENT])]
    return data


def unit(name: str) -> tuple[str, float]:
    """(label, divisor) for showing a column: `_us` columns are shown in ms."""
    if name.endswith("_us"):
        return name[: -len("_us")] + " (ms)", 1000.0
    return name, 1.0


def report(text: str, budget_ms: float = 1000.0 / DEFAULT_FPS, column: str = PRESENT, skip: int = 0,
           title: str = "") -> str:
    columns, rows, meta = parse(text)
    rows = rows[max(0, skip):]
    if not rows:
        raise FormatError("no frames" + (f" after skipping {skip}" if skip else ""))
    data = columns_of(columns, rows)
    if column not in data:
        raise FormatError(f"no column {column!r} (have: {', '.join(data)})")
    out: list[str] = []
    out.append(f"## Performance report{': ' + title if title else ''}")
    out.append("")
    for line in meta:
        if line.startswith("# perf "):
            out.append(f"Log: `{line[2:]}`")
    out.append(f"Frames: {len(rows)}" + (f" (the first {skip} skipped)" if skip else ""))
    out.append("")
    out.append("| column | p50 | p95 | p99 | max |")
    out.append("|---|---:|---:|---:|---:|")
    for name, values in data.items():
        label, div = unit(name)
        ordered = sorted(values)
        cells = [f"{percentile(ordered, p) / div:.3f}" for p in PERCENTILES] + [f"{ordered[-1] / div:.3f}"]
        out.append(f"| {label} | " + " | ".join(cells) + " |")
    out.append("")
    _, div = unit(column)
    values_ms = [v / div for v in data[column]]
    over = sum(1 for v in values_ms if v > budget_ms)
    stutters = sum(1 for v in values_ms if v > STUTTER_MS)
    n = len(values_ms)
    out.append(f"Frames over the budget of {budget_ms:.3f} ms ({column}): {over} of {n} ({100.0 * over / n:.2f}%)")
    out.append(f"Frames over {STUTTER_MS:.0f} ms ({column}): {stutters} of {n} ({100.0 * stutters / n:.2f}%)")
    return "\n".join(out) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Report on a per-frame performance log.")
    ap.add_argument("csv", type=Path)
    budget = ap.add_mutually_exclusive_group()
    budget.add_argument("--budget-ms", type=float, help="frame budget in ms (default 1000/120)")
    budget.add_argument("--fps", type=float, help="frame budget as a frame rate")
    ap.add_argument("--column", default=PRESENT, help="column the budget applies to (default present_us)")
    ap.add_argument("--skip", type=int, default=0, help="frames to leave out at the start (warm-up)")
    args = ap.parse_args(argv)
    ms = args.budget_ms if args.budget_ms is not None else 1000.0 / (args.fps or DEFAULT_FPS)
    if not ms > 0 or args.skip < 0:
        print("perf_report: the budget must be above 0 and --skip not negative", file=sys.stderr)
        return 2
    try:
        text = args.csv.read_text(encoding="utf-8")
        sys.stdout.write(report(text, ms, args.column, args.skip, args.csv.name))
    except (OSError, UnicodeDecodeError, FormatError) as exc:
        print(f"perf_report: {args.csv}: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
