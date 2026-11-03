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

The native runner's log (mh4u_native_runner --perf-log) has other columns. Version 2 (N1-32):

    # perf v2 <ISO date> game=0x<title id> cpu=native new3ds=off clock=<host|virtual> mode=<realtime|deterministic>
    frame,present_us,main_us,guest_us,svc_us,service_us,idle_us,switches,preempts,main_svc_us,main_service_us

`main_us` is the game's main thread in guest code (with the scheduler work run at its preemption checks), `guest_us`
every thread, `svc_us` and `service_us` the system calls and the services of every thread, `idle_us` every core
waiting, `switches` and `preempts` counts per frame, `main_svc_us` and `main_service_us` the main thread's own part of
`svc_us` and `service_us`. `frame` is the game's frame counter, the `f=` of a --state-trace. Version 1 (N1-23) has the
first nine columns only (the scheduler did not count system calls and services by thread then). A native log is v2
when its columns hold `main_svc_us` and `main_service_us`, v1 when they hold `main_us` and neither; a log with one of
the two only, with both but without `main_us`, whose `# perf v<N>` line says another version than its columns, or
with a `# perf v2` line over the reference runner's columns, is refused (exit status 2).

For a native log the report adds

  busy_us     = guest_us + svc_us + service_us             every guest thread's counted work on the one host thread
                                                           (the scheduler's host loop is in no column)
  game_us     = main_us + main_svc_us + main_service_us    (v2) the game thread's time, an UPPER bound: it holds the
                                                           scheduler's host work run at the main thread's preemption
                                                           checks and system calls
  game_all_us = main_us + svc_us + service_us              (v2) N1-23's upper bound, every thread's calls and services,
                                                           for comparison
  game_us     = main_us + svc_us + service_us              (v1) the game thread's time, an UPPER bound: it holds the
                                                           other threads' system calls and services too

and a verdict line: the p99 of `game_us` against --thread-budget-ms (4 ms, ROADMAP section 7), and the p99 of
`main_us` alone, the lower bound. WITHIN: the upper bound is within the budget. OVER: even the lower bound is over
it. UNDECIDED: the budget lies between the two. A v2 log adds a line with the p99 of `game_all_us` (it takes no part
in the verdict); then, for both versions, the line of `busy_us`.

--state-trace FILE (the runner's --state-trace of the same run) keeps only the frames the hunt's scene
(--hunt-scene, default aQuest, 0x00e655c8) is shown in. A run whose hunt has fewer than --min-frames rows left after
--skip does not count (NOT COUNTED, exit status 1), and neither does a log made with --deterministic
(clock=virtual): that clock is virtual time, not the CPU's. --state-trace takes the native runner's log only: the
reference runner's `frame` is a vertical-blank index, not the game's frame counter, so such a log is refused (exit
status 2).

usage: perf_report.py CSV [--budget-ms MS | --fps FPS] [--column NAME] [--skip N]
                          [--state-trace FILE [--hunt-scene VALUE] [--min-frames N]] [--thread-budget-ms MS]
exit status: 0 reported, 1 the run does not count (above), 2 the file is not a performance log.
"""
from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path
from typing import Collection, Optional, Sequence

PRESENT = "present_us"
PARTS = ("recomp_us", "svc_us", "gpu_us")
OTHER = "other_us"
STUTTER_MS = 50.0
DEFAULT_FPS = 120.0
PERCENTILES = (50, 95, 99)

# The native runner's log (N1-23; v2 since N1-32).
MAIN = "main_us"
NATIVE_PARTS = ("guest_us", "svc_us", "service_us")
MAIN_SVC = "main_svc_us"          # v2: the main thread's own part of svc_us
MAIN_SERVICE = "main_service_us"  # v2: the main thread's own part of service_us
BUSY = "busy_us"
GAME = "game_us"
GAME_ALL = "game_all_us"          # v2: N1-23's upper bound, with every thread's calls and services
THREAD_BUDGET_MS = 4.0       # ROADMAP section 7: the game thread at 120 fps
HUNT_SCENE = 0x00E655C8      # aQuest, the scene of the hunt (the HUNT_SCENE of fps_audit.py)
MIN_HUNT_FRAMES = 2400       # 20 s at 120 fps, after --skip


class FormatError(ValueError):
    """A file that is not a performance log."""


class NotCounted(Exception):
    """A well-formed log of a run that does not count: it never reached the hunt, or it is in virtual time."""


_STATE_FRAME = re.compile(r"^f=(\d+)\s")
_PERF_VERSION = re.compile(r"^#\s*perf\s+v(\d+)\b")


def native_version(columns: Sequence[str], meta: Sequence[str]) -> Optional[int]:
    """2 for a native log with the main thread's calls and services (N1-32), 1 for N1-23's, None for a log that is
    not the native runner's. FormatError for one of the two v2 columns without the other, for both without main_us,
    and for a `# perf v<N>` line whose version is not the columns' (a v2 line over another runner's columns too)."""
    has_svc, has_service = MAIN_SVC in columns, MAIN_SERVICE in columns
    if has_svc != has_service:
        present, missing = (MAIN_SVC, MAIN_SERVICE) if has_svc else (MAIN_SERVICE, MAIN_SVC)
        raise FormatError(f"the column {present} without {missing}: a v2 log has both")
    if has_svc and MAIN not in columns:
        raise FormatError(f"the columns {MAIN_SVC} and {MAIN_SERVICE} without {MAIN}")
    version = 2 if has_svc else (1 if MAIN in columns else None)
    for line in meta:
        match = _PERF_VERSION.match(line)
        if not match:
            continue
        said = int(match.group(1))
        if version is None:
            if said == 2:
                raise FormatError(f"the line {line!r} says v2, but the columns are not the native runner's v2")
        elif said != version:
            raise FormatError(f"the line {line!r} says v{said}, but the columns are those of a v{version} native log")
    return version


def hunt_frames(state_trace: str, scene: int = HUNT_SCENE) -> set[int]:
    """The frames of a --state-trace (N0-07's format: `f=<frame> t=<ticks> <name>=0x<hex> ...`) whose `scene` is
    `scene`."""
    frames: set[int] = set()
    wanted = f"scene=0x{scene:08x}"
    for line in state_trace.splitlines():
        match = _STATE_FRAME.match(line)
        if match and wanted in line.split():
            frames.add(int(match.group(1)))
    return frames


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
    native_version(columns, meta)
    return columns, rows, meta


def percentile(sorted_values: Sequence[float], p: float) -> float:
    """Nearest-rank percentile of an ascending list."""
    if not sorted_values:
        raise ValueError("no values")
    rank = max(1, math.ceil(p / 100.0 * len(sorted_values)))
    return sorted_values[min(rank, len(sorted_values)) - 1]


def columns_of(columns: list[str], rows: list[list[float]]) -> dict[str, list[float]]:
    """name -> values, without `frame`, with `other_us` derived when the parts are there, and for a native log
    `busy_us`, `game_us` and (v2) `game_all_us`."""
    data = {name: [r[i] for r in rows] for i, name in enumerate(columns) if name != "frame"}
    if OTHER not in data and all(p in data for p in PARTS):
        data[OTHER] = [max(0.0, pr - sum(data[p][i] for p in PARTS)) for i, pr in enumerate(data[PRESENT])]
    if MAIN in data and all(p in data for p in NATIVE_PARTS):
        n = len(data[PRESENT])
        all_calls = [data[MAIN][i] + data["svc_us"][i] + data["service_us"][i] for i in range(n)]
        data[BUSY] = [sum(data[p][i] for p in NATIVE_PARTS) for i in range(n)]
        if MAIN_SVC in data and MAIN_SERVICE in data:  # v2: the main thread's own calls and services
            data[GAME] = [data[MAIN][i] + data[MAIN_SVC][i] + data[MAIN_SERVICE][i] for i in range(n)]
            data[GAME_ALL] = all_calls
        else:  # v1: every thread's
            data[GAME] = all_calls
    return data


def thread_verdict(data: dict[str, list[float]], thread_budget_ms: float) -> list[str]:
    """The lines about the game thread's budget for a native log (empty for any other)."""
    if GAME not in data:
        return []
    upper = percentile(sorted(data[GAME]), 99) / 1000.0
    lower = percentile(sorted(data[MAIN]), 99) / 1000.0
    busy = percentile(sorted(data[BUSY]), 99) / 1000.0
    if upper <= thread_budget_ms:
        state = "WITHIN"
    elif lower > thread_budget_ms:
        state = "OVER"
    else:
        state = "UNDECIDED"
    busy_line = f"Every guest thread together ({BUSY}, every guest thread's counted work), p99: {busy:.3f} ms"
    if GAME_ALL not in data:  # v1, as N1-23 wrote it
        return [
            f"Game thread, p99 against its budget of {thread_budget_ms:.3f} ms: "
            f"{GAME} (upper bound) {upper:.3f} ms, {MAIN} (lower bound) {lower:.3f} ms: {state}",
            busy_line,
        ]
    every = percentile(sorted(data[GAME_ALL]), 99) / 1000.0
    return [
        f"Game thread, p99 against its budget of {thread_budget_ms:.3f} ms: "
        f"{GAME} (upper bound, its own calls and services) {upper:.3f} ms, {MAIN} (lower bound) {lower:.3f} ms: "
        f"{state}",
        f"With every thread's calls and services ({GAME_ALL}, N1-23's upper bound), p99: {every:.3f} ms",
        busy_line,
    ]


def unit(name: str) -> tuple[str, float]:
    """(label, divisor) for showing a column: `_us` columns are shown in ms."""
    if name.endswith("_us"):
        return name[: -len("_us")] + " (ms)", 1000.0
    return name, 1.0


def report(text: str, budget_ms: float = 1000.0 / DEFAULT_FPS, column: str = PRESENT, skip: int = 0,
           title: str = "", frames: Optional[Collection[int]] = None, min_frames: int = MIN_HUNT_FRAMES,
           thread_budget_ms: float = THREAD_BUDGET_MS) -> str:
    """The markdown report. `frames` (the hunt's, from hunt_frames) keeps only the rows of those frames before
    `skip` is applied; NotCounted if the log is in virtual time or fewer than `min_frames` rows are left."""
    columns, rows, meta = parse(text)
    if any(" clock=virtual" in line for line in meta):
        raise NotCounted("the log is in virtual time (--deterministic), not the host's: it measures no CPU")
    if MAIN in columns and not any(" clock=host" in line for line in meta):
        raise NotCounted("the log does not say clock=host: its clock is unknown, so it is no CPU measurement")
    if frames is not None and MAIN not in columns:
        raise FormatError("--state-trace needs the native runner's log (its frame column is the game's frame "
                          "counter; the reference runner's is a vertical-blank index)")
    in_hunt = len(rows)
    if frames is not None:
        wanted = frames
        rows = [r for r in rows if int(r[0]) in wanted]
        in_hunt = len(rows)
        if not rows:
            raise NotCounted("the state trace never shows the hunt's scene in a frame of this log")
    rows = rows[max(0, skip):]
    if frames is not None and len(rows) < min_frames:
        raise NotCounted(f"the hunt has {in_hunt} rows, {len(rows)} after skipping {skip}: fewer than {min_frames}")
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
    if frames is not None:
        out.append(f"Hunt: {in_hunt} rows of this log are in the hunt's scene")
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
    verdict = thread_verdict(data, thread_budget_ms)
    if verdict:
        out.append("")
        out.extend(verdict)
    return "\n".join(out) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Report on a per-frame performance log.")
    ap.add_argument("csv", type=Path)
    budget = ap.add_mutually_exclusive_group()
    budget.add_argument("--budget-ms", type=float, help="frame budget in ms (default 1000/120)")
    budget.add_argument("--fps", type=float, help="frame budget as a frame rate")
    ap.add_argument("--column", default=PRESENT, help="column the budget applies to (default present_us)")
    ap.add_argument("--skip", type=int, default=0, help="frames to leave out at the start (warm-up)")
    ap.add_argument("--state-trace", type=Path, help="the same run's --state-trace: keep the hunt's frames only")
    ap.add_argument("--hunt-scene", default=hex(HUNT_SCENE), metavar="VALUE",
                    help="the scene value of the hunt in the state trace (default aQuest, 0x00e655c8)")
    ap.add_argument("--min-frames", type=int, default=MIN_HUNT_FRAMES,
                    help=f"with --state-trace: rows the hunt needs after --skip to count (default {MIN_HUNT_FRAMES})")
    ap.add_argument("--thread-budget-ms", type=float, default=THREAD_BUDGET_MS,
                    help="the game thread's budget for a native log (default 4)")
    args = ap.parse_args(argv)
    try:
        scene = int(args.hunt_scene, 0)
    except ValueError:
        print(f"perf_report: --hunt-scene {args.hunt_scene!r}: want a number such as 0x00e655c8", file=sys.stderr)
        return 2
    if args.min_frames < 1 or not (math.isfinite(args.thread_budget_ms) and args.thread_budget_ms > 0):
        print("perf_report: --min-frames must be at least 1 and --thread-budget-ms above 0", file=sys.stderr)
        return 2
    if args.budget_ms is not None:
        ms = args.budget_ms
    elif args.fps is not None:
        ms = 1000.0 / args.fps if args.fps != 0 else math.nan
    else:
        ms = 1000.0 / DEFAULT_FPS
    if not (math.isfinite(ms) and ms > 0) or args.skip < 0:
        print("perf_report: the budget must be a finite number above 0 and --skip not negative", file=sys.stderr)
        return 2
    try:
        text = args.csv.read_text(encoding="utf-8")
        frames = None
        if args.state_trace is not None:
            frames = hunt_frames(args.state_trace.read_text(encoding="utf-8"), scene)
        sys.stdout.write(report(text, ms, args.column, args.skip, args.csv.name, frames, args.min_frames,
                                args.thread_budget_ms))
    except NotCounted as exc:
        print(f"NOT COUNTED: {args.csv}: {exc}")
        return 1
    except (OSError, UnicodeDecodeError, FormatError) as exc:
        print(f"perf_report: {args.csv}: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
