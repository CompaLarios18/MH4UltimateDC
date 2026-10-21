#!/usr/bin/env python3
"""Find and validate the game's own scene value (N0-27, docs/native/wp/N0.md, decision D32).

The scene observable is a value in the game's memory that names the current scene (title,
character creation, village, quest, ...), changes at each scene change and repeats across runs and
backends. Three commands:

  dumps    --session FILE --object ADDR:SIZE... [--every N] [--first F] [--last F]
      Prints, one per line, the mh4u_runner options that dump each candidate object every N game
      frames (default 50) from frame F (default N) to frame L (default the session's `# frames:`
      total): `--dump f<N>:<addr>:<size>`.

  rank     DUMPDIR --phases F1,F2,... [--window N] [--top N] [--all]
      Reads the dumps a runner wrote (mem_f<frame>_<addr>.bin) and lists, for each aligned u8, u16
      and u32 offset of each dumped object, how many distinct values it takes, the dump frames
      where it changes and how many of its changes fall within `--window` (default 60) frames after
      a phase frame (a frame where you know a scene begins). Candidates first. A change is dated by
      the first dump that shows it, so it happened somewhere in (previous dump, that dump].

  validate --observable FILE --expect-distinct S:F1,F2,... [--tolerance N] STATE...
      Checks state traces (mh4u_runner --state-trace) recorded with the observable FILE against the
      rules a scene observable has to meet. The traces are named build/scene/<S>-<cpu>-<n>.txt
      (S the session, cpu jit|recomp|interp|native, which must match the header's cpu=). Exits 0
      when, in every trace, `scene` is never `-` after its first readable value; in every trace of
      session S the values at the --expect-distinct frames are pairwise distinct; every G2 trace has
      at least 3 distinct readable values; the traces of one session show the same sequence of
      values (state_diff.py --order-only; traces of different sessions are never compared); and the
      scene changes of the traces of one session and one backend fall within N frames of each
      other (state_diff.py --tolerance N, default 0; the largest delta is printed per session).
      Only the `scene` value is gated here: the other observables are not compared.

Exit status: 0 ok, 1 a check failed, 2 a usage problem or an unreadable input, 3 a trace without a
`scene` field. The output holds offsets, value counts, values of the scene word and frames only.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import state_diff  # noqa: E402

SCENE = state_diff.SCENE
BACKENDS = ("jit", "recomp", "interp", "native")
TRACE_NAME = re.compile(r"(?P<session>[A-Za-z0-9]+)-(?P<cpu>jit|recomp|interp|native)-(?P<n>\d+)\.txt")
DUMP_NAME = re.compile(r"mem_f(?P<frame>\d+)_(?P<addr>[0-9a-fA-F]+)\.bin")
HEADER_CPU = re.compile(r"\bcpu=(\w+)")
DEFAULT_EVERY = 50
DEFAULT_WINDOW = 60
G2_MIN_VALUES = 3
WIDTHS = ((4, "u32", "<I"), (2, "u16", "<H"), (1, "u8", "<B"))


class UsageError(Exception):
    """A command line or an input file that cannot be used."""


def _int(text: str, what: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise UsageError(f"{what}: {text!r} is not a number") from None


# ── dumps ──────────────────────────────────────────────────────────────────────────

def session_frames(path: str) -> int:
    try:
        text = Path(path).read_text()
    except OSError as error:
        raise UsageError(f"cannot read {path}: {error.strerror}") from None
    match = re.search(r"^#\s*frames:\s*(\d+)\s*$", text, re.MULTILINE)
    if not match:
        raise UsageError(f"{path} has no '# frames: N' line")
    return int(match.group(1))


def parse_object(text: str) -> tuple[int, int]:
    address, sep, size = text.partition(":")
    if not sep:
        raise UsageError(f"--object {text!r}: want ADDR:SIZE")
    start, length = _int(address, "--object address"), _int(size, "--object size")
    if length <= 0 or not 0 <= start <= 0xFFFFFFFF:
        raise UsageError(f"--object {text!r}: want a 32-bit address and a size above zero")
    return start, length


def dump_options(first: int, last: int, objects: list[tuple[int, int]], every: int) -> list[str]:
    return [f"--dump f{frame}:0x{address:08x}:0x{size:x}"
            for frame in range(first, last + 1, every) for address, size in objects]


def cmd_dumps(args: argparse.Namespace) -> int:
    if args.every < 1:
        raise UsageError("--every must be at least 1")
    objects = [parse_object(text) for text in args.object]
    total = session_frames(args.session)
    first = args.first if args.first is not None else args.every
    last = min(args.last, total) if args.last is not None else total
    if first < 1 or last < first:
        raise UsageError("--first must be at least 1 and not after --last or the session's last frame")
    for line in dump_options(first, last, objects, args.every):
        print(line)
    return 0


# ── rank ───────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Field:
    address: int       # of the object
    offset: int
    width: int
    label: str
    distinct: int
    changes: list[int]   # the dump frames that show a change
    near: int            # changes whose interval touches a window after a phase
    windows: int         # phases with at least one such change
    score: float


def load_dumps(folder: str) -> dict[int, list[tuple[int, bytes]]]:
    """{object address: [(frame, bytes)] sorted by frame}; only whole files of equal size."""
    base = Path(folder)
    if not base.is_dir():
        raise UsageError(f"{folder} is not a folder")
    objects: dict[int, dict[int, bytes]] = defaultdict(dict)
    for path in sorted(base.rglob("mem_f*.bin")):
        match = DUMP_NAME.fullmatch(path.name)
        if match:
            objects[int(match["addr"], 16)][int(match["frame"])] = path.read_bytes()
    if not objects:
        raise UsageError(f"no mem_f<frame>_<addr>.bin dumps in {folder}")
    return {addr: sorted(frames.items()) for addr, frames in sorted(objects.items())}


def rank_object(address: int, dumps: list[tuple[int, bytes]], phases: list[int], window: int) -> list[Field]:
    size = min(len(data) for _, data in dumps)
    fields: list[Field] = []
    for width, label, fmt in WIDTHS:
        for offset in range(0, size - width + 1, width):
            values = [struct.unpack_from(fmt, data, offset)[0] for _, data in dumps]
            changes = [i for i in range(1, len(values)) if values[i] != values[i - 1]]
            near_phase: set[int] = set()
            near = 0
            for i in changes:
                lo, hi = dumps[i - 1][0], dumps[i][0]   # the change happened in (lo, hi]
                hit = [p for p in phases if lo < p + window and hi >= p]
                near += bool(hit)
                near_phase.update(hit)
            precision = near / len(changes) if changes else 0.0
            recall = len(near_phase) / len(phases)
            score = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
            fields.append(Field(address, offset, width, label, len(set(values)),
                                [dumps[i][0] for i in changes], near, len(near_phase), score))
    return fields


def cmd_rank(args: argparse.Namespace) -> int:
    phases = sorted({_int(text, "--phases") for text in args.phases.split(",") if text.strip()})
    if not phases:
        raise UsageError("--phases needs at least one frame")
    if args.window < 0 or args.top < 1:
        raise UsageError("--window must not be negative and --top must be at least 1")
    ranked: list[Field] = []
    objects = load_dumps(args.dumpdir)
    for address, dumps in objects.items():
        if len(dumps) < 2:
            raise UsageError(f"object 0x{address:08x} has fewer than two dumps")
        ranked += rank_object(address, dumps, phases, args.window)
    ranked.sort(key=lambda f: (-f.score, -f.near, f.distinct < 2, -f.width, f.address, f.offset))
    shown = [f for f in ranked if args.all or f.score > 0]
    print(f"# {len(objects)} object(s), {len(phases)} phase(s), window {args.window} frames; "
          f"{len(shown)} of {len(ranked)} fields shown")
    for f in shown[:args.top]:
        frames = ",".join(map(str, f.changes[:12])) + (",..." if len(f.changes) > 12 else "")
        print(f"0x{f.address:08x}+0x{f.offset:02x} {f.label} score={f.score:.3f} distinct={f.distinct} "
              f"changes={len(f.changes)} near={f.near} windows={f.windows}/{len(phases)} frames={frames or '-'}")
    return 0


# ── validate ───────────────────────────────────────────────────────────────────────

def parse_trace_name(path: str) -> Optional[tuple[str, str, int]]:
    """(session, cpu, n) of a file named <S>-<cpu>-<n>.txt, or None."""
    match = TRACE_NAME.fullmatch(Path(path).name)
    return (match["session"], match["cpu"], int(match["n"])) if match else None


@dataclass
class Trace:
    name: str
    session: str
    cpu: str
    n: int
    records: list[state_diff.Record]   # from the first readable scene on

    @property
    def readable(self) -> list[int]:
        return [r.values[SCENE] for r in self.records]   # type: ignore[misc]

    def sequence(self) -> list[int]:
        return [r.values[SCENE] for r in state_diff.scene_changes(self.records)]  # type: ignore[misc]

    def value_at(self, frame: int) -> Optional[int]:
        found = None
        for record in self.records:
            if record.frame > frame:
                break
            found = record.values[SCENE]
        return found


def load_trace(path: str) -> tuple[Trace, bool]:
    """The trace and whether it has a `scene` field. Raises UsageError for anything unusable."""
    parsed = parse_trace_name(path)
    if parsed is None:
        raise UsageError(f"{path}: the file name must be <session>-<jit|recomp|interp|native>-<n>.txt")
    try:
        text = Path(path).read_text()
    except OSError as error:
        raise UsageError(f"cannot read {path}: {error.strerror}") from None
    header = next((l for l in text.splitlines() if l.strip()), "")
    found = HEADER_CPU.search(header) if header.startswith("#") else None
    if not found or found.group(1) != parsed[1]:
        raise UsageError(f"{path}: the name says cpu={parsed[1]}, the header says "
                         f"{'nothing' if not found else 'cpu=' + found.group(1)}")
    try:
        records = state_diff.parse_trace(text)
    except state_diff.FormatError as error:
        raise UsageError(f"{path}: {error}") from None
    if not records:
        raise UsageError(f"{path}: no records")
    has_scene = any(SCENE in r.values for r in records)
    kept = records
    if has_scene:
        if any(SCENE not in r.values for r in records):
            raise UsageError(f"{path}: line {next(r.line for r in records if SCENE not in r.values)} "
                             f"has no {SCENE}= value")
        first = next((i for i, r in enumerate(records) if r.values.get(SCENE) is not None), len(records))
        kept = records[first:]
    return Trace(Path(path).name, *parsed, kept), has_scene


def parse_expect(texts: list[str]) -> dict[str, list[int]]:
    expect: dict[str, list[int]] = defaultdict(list)
    for text in texts:
        session, sep, frames = text.partition(":")
        if not sep or not session or not frames:
            raise UsageError(f"--expect-distinct {text!r}: want <session>:<frame>,<frame>,...")
        expect[session] += [_int(f, "--expect-distinct frame") for f in frames.split(",") if f.strip()]
        if len(expect[session]) < 2:
            raise UsageError(f"--expect-distinct {text!r}: at least two frames")
    return expect


def check_trace(trace: Trace, expect: dict[str, list[int]]) -> list[str]:
    failures: list[str] = []
    values = trace.readable
    if not values:
        return [f"{trace.name}: scene is never readable"]
    unreadable = [r.frame for r in trace.records if r.values[SCENE] is None]
    if unreadable:
        failures.append(f"{trace.name}: scene is unreadable ('-') at {len(unreadable)} frame(s) after "
                        f"its first readable value, from frame {unreadable[0]}")
    distinct = {v for v in values if v is not None}
    if trace.session == "G2" and len(distinct) < G2_MIN_VALUES:
        failures.append(f"{trace.name}: {len(distinct)} distinct value(s), a G2 trace needs at least "
                        f"{G2_MIN_VALUES}")
    frames = expect.get(trace.session)
    if frames:
        seen: dict[int, int] = {}
        for frame in frames:
            value = trace.value_at(frame)
            if value is None:
                failures.append(f"{trace.name}: no readable scene at or before frame {frame}")
            elif frame > trace.records[-1].frame:
                failures.append(f"{trace.name}: the trace ends at frame {trace.records[-1].frame}, "
                                f"before frame {frame}")
            elif value in seen:
                failures.append(f"{trace.name}: frames {seen[value]} and {frame} are in the same scene "
                                f"(0x{value:x})")
            else:
                seen[value] = frame
    return failures


def compare(first: Trace, other: Trace, observables: list[state_diff.Observable], tolerance: int,
            order_only: bool) -> tuple[Optional[str], int]:
    """(first divergence or None, largest frame delta over the scene changes both have)."""
    problem = state_diff.diff(first.records, other.records, observables, tolerance, order_only)
    a, b = state_diff.scene_changes(first.records), state_diff.scene_changes(other.records)
    return problem, max((abs(x.frame - y.frame) for x, y in zip(a, b)), default=0)


def cmd_validate(args: argparse.Namespace) -> int:
    if args.tolerance < 0:
        raise UsageError("--tolerance must not be negative")
    try:
        observables = state_diff.parse_observable(Path(args.observable).read_text())
    except OSError as error:
        raise UsageError(f"cannot read {args.observable}: {error.strerror}") from None
    except state_diff.FormatError as error:
        raise UsageError(f"{args.observable}: {error}") from None
    scene = next((o for o in observables if o.name == SCENE), None)
    if scene is None:
        raise UsageError(f"{args.observable} has no `{SCENE}` observable")
    gated = [state_diff.Observable(scene.expr, scene.size, SCENE, True)]   # the scene alone is gated
    expect = parse_expect(args.expect_distinct or [])
    loaded = [load_trace(path) for path in args.state]
    if any(not has_scene for _, has_scene in loaded):
        for trace, has_scene in loaded:
            if not has_scene:
                print(f"{trace.name}: no `{SCENE}` field (record it with {args.observable})")
        return 3
    traces = [trace for trace, _ in loaded]
    failures: list[str] = []
    print(f"scene = {scene.expr} ({scene.size} bytes)")
    for trace in traces:
        failures += check_trace(trace, expect)
        if trace.readable:
            print(f"{trace.name}: {len(trace.records)} records, "
                  f"{len({v for v in trace.readable if v is not None})} distinct value(s), "
                  f"{len(trace.sequence()) - 1} change(s)")
    by_session: dict[str, list[Trace]] = defaultdict(list)
    for trace in traces:
        if trace.readable:
            by_session[trace.session].append(trace)
    compared_pairs = 0
    for session, group in sorted(by_session.items()):
        reference, largest, pairs = group[0], 0, 0
        for other in group[1:]:
            problem, _ = compare(reference, other, gated, args.tolerance, True)
            if problem:
                failures.append(f"{session}: {other.name} against {reference.name}: the sequence of "
                                f"values differs: {problem}")
        for cpu in sorted({t.cpu for t in group}):
            same = [t for t in group if t.cpu == cpu]
            for other in same[1:]:
                pairs += 1
                problem, delta = compare(same[0], other, gated, args.tolerance, False)
                largest = max(largest, delta)
                if problem:
                    failures.append(f"{session}: {other.name} against {same[0].name}: {problem}")
        compared_pairs += pairs
        print(f"{session}: largest scene-change delta between runs of one backend: "
              + (f"{largest} frame(s) over {pairs} pair(s) (tolerance {args.tolerance})" if pairs
                 else "none (one run per backend)"))
    if not compared_pairs:
        print("no two traces of one session and backend: the frame tolerance was not checked")
    for text in failures:
        print(f"FAIL {text}")
    if failures:
        return 1
    print("scene observable validates")
    return 0


# ── command line ───────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Find and validate the game's scene observable.")
    commands = parser.add_subparsers(dest="command", required=True)

    dumps = commands.add_parser("dumps", help="print the runner's --dump options for a session")
    dumps.add_argument("--session", required=True, metavar="FILE")
    dumps.add_argument("--object", action="append", required=True, metavar="ADDR:SIZE")
    dumps.add_argument("--every", type=int, default=DEFAULT_EVERY, metavar="N")
    dumps.add_argument("--first", type=int, metavar="F", help="first dumped frame (default: N)")
    dumps.add_argument("--last", type=int, metavar="F", help="last dumped frame (default: the session total)")

    rank = commands.add_parser("rank", help="rank the fields of dumped objects by how they change")
    rank.add_argument("dumpdir", metavar="DUMPDIR")
    rank.add_argument("--phases", required=True, metavar="F1,F2,...")
    rank.add_argument("--window", type=int, default=DEFAULT_WINDOW, metavar="N")
    rank.add_argument("--top", type=int, default=40, metavar="N")
    rank.add_argument("--all", action="store_true", help="also list fields that never change near a phase")

    validate = commands.add_parser("validate", help="check state traces against the scene rules")
    validate.add_argument("--observable", required=True, metavar="FILE")
    validate.add_argument("--expect-distinct", action="append", metavar="S:F1,F2,...")
    validate.add_argument("--tolerance", type=int, default=0, metavar="N")
    validate.add_argument("state", nargs="+", metavar="STATE")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "dumps":
            return cmd_dumps(args)
        if args.command == "rank":
            return cmd_rank(args)
        return cmd_validate(args)
    except UsageError as error:
        print(f"scene_probe: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
