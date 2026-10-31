#!/usr/bin/env python3
"""Find and validate the observables of the 60/120 fps audit: the hunter's position and the quest's clock
(N3-21, docs/native/wp/N3.md, ROADMAP section 6's high-frame-rate rule, docs/research/fps-observables.md).

The audit (N3-22) compares a session played at 60 fps with the same session, scaled, played at 120 fps. Besides the
scenes it compares "positions and times within the tolerance the audit sets"; this tool holds what N3-21 needs to
find those two values in the game's memory and to say whether a candidate is one. Five commands:

  scale SESSION --factor K -o OUT
      Writes the session with every frame key, held count (`f<N>` holds; holds in milliseconds stay), checkpoint
      frame, `# hit:` frame and the `# frames:` total multiplied by K, and `# name:` with `-x<K>` appended. The
      other lines (captures, env, init-time, comments) are copied as they are. Name the output `<name>-x<K>.txt`
      (golden.py wants the file name to equal the header). N3-22 imports `scale_session`.

  validate --trace STATE --session S --rule position|quest_time [--names A,B,C] [--fps N] [--from F] [--to F]
           [--float] [--expect-rate R] [--settle N]
      Checks a state trace (mh4u_runner --state-trace, recorded with the candidate lines) against the session it
      was recorded with. Prints `valid` (details on stderr, exit 0) or `invalid: <why>` (exit 1).
      The hunt is the last scene's stretch of the trace from the first record in which every named value is
      readable (or --from .. --to). Both rules are first guesses (N3 rule 7): 60, 100000 and 1 % are set below
      and are never retuned to make a candidate pass.
        position    three 4-byte floats (--names, default hunter_x,hunter_y,hunter_z) that are finite with a
                    magnitude under 100000 in every record of the hunt; that stay constant over every stretch of
                    at least 60 frames (one second of game time: 60 x fps/60 frames) without movement input
                    (a CIRCLE event with a non-zero direction, for its hold) in the session script; and that
                    change (the three values as a vector) over every stretch with the circle pad held. A stretch
                    that contains a frame marked `# hit: F` in the session's notes (a hit taken) is left out.
        quest_time  one 4-byte counter (--names, default quest_time; --float: read as an IEEE float) that moves by
                    the same amount per second of game time (frames over fps) over the hunt: its value stays on
                    the straight line from the first to the last record of the hunt within 1 % of the whole
                    change. --expect-rate R (units per second, for example the rate measured at 60 fps) also
                    requires the measured rate to be within 1 % of R.
      --fps defaults to 60 times the K of a session named `...-x<K>`. --settle N (default 0) leaves the first N
      frames of every stretch without input out of the constancy check; the spec has no such margin, so a run
      that needs it is a `needs-decision`, not a pass.

  segments --trace STATE --session S [--name quest_time] [--fps N] [--float] [--from F] [--to F]
      Cuts the hunt into windows of one second of game time and prints where a counter runs (moves in every window)
      and where it pauses, with the rate and the distance from the straight line of each running stretch. Information
      only (exit 0): it is how a counter that pauses, which `validate` rejects, is described.

  find-position DUMPDIR --base ADDR --still F,F;F,F --moving F,F,F [--limit N]
      Reads the memory dumps a run wrote (`--dump f<N>:<ADDR>:<SIZE>`, files mem_f<frame>_<addr>.bin) and lists
      every triple of consecutive 4-byte words that is equal in the dumps of each --still group (groups separated
      by `;`: frames without movement input), changes (as a vector) between each two consecutive --moving dumps
      (the circle pad held), and is a finite float of magnitude under 100000 in all of them. Ranked by how
      constant the speed of the change is. Addresses and counts only.

  find-counter DUMPDIR --base ADDR --frames F,F,F... [--tolerance 0.01] [--float]
      Lists the words that move linearly with the game frame over the dumps (every dump within the tolerance of
      the straight line from the first to the last, the first and last different).

  find-monotone DUMPDIR --base ADDR --frames F,F,F... [--min-moving 6] [--down] [--float]
      Lists the words that never turn back over the dumps and move in at least --min-moving of the gaps: counters
      that pause (a quest's countdown stops during a talk or a cutscene), which find-counter's straight line rejects.

  words DUMPDIR --base ADDR --address A[,A...]
      Prints the words at the addresses in every dump (hex, u32 and float): a way to look at a candidate.

Exit status: 0 ok / valid, 1 invalid or nothing found, 2 a usage problem or an unreadable input.
Only addresses, offsets, counts and frames are printed or written: no game text.
"""
from __future__ import annotations

import argparse
import math
import mmap
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import state_diff  # noqa: E402

SCENE = state_diff.SCENE
FRAME_BASE_FPS = 60
MIN_IDLE_SECONDS = 1.0           # 60 frames at 60 fps (first guess)
MAGNITUDE_LIMIT = 100000.0       # first guess
RATE_TOLERANCE = 0.01            # 1 % (first guess)
DEFAULT_POSITION = ("hunter_x", "hunter_y", "hunter_z")
DEFAULT_QUEST_TIME = ("quest_time",)
DEFAULT_HOLD_MS = 100            # the runner's default hold of a press
TICKS_PER_SECOND = 268111856


class UsageError(Exception):
    """A command line or an input file that cannot be used."""


# -- sessions ------------------------------------------------------------------------------------------------

_FRAME_KEY = re.compile(r"^f(\d+)$")
_HEADER = re.compile(r"^#\s*(name|frames|checkpoints|hit)\s*:\s*(.*)$")
BUTTONS = {"A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "L", "R", "START", "SELECT", "ZL", "ZR", "HOME"}


def _scale_time(text: str, factor: int) -> str:
    """`f<N>` becomes `f<N*factor>`; a plain number (milliseconds) stays."""
    match = _FRAME_KEY.match(text)
    return f"f{int(match.group(1)) * factor}" if match else text


def _scale_event(item: str, factor: int) -> str:
    fields = item.strip().split(":")
    if len(fields) < 2:
        return item
    fields[0] = _scale_time(fields[0], factor)
    hold_index = 4 if fields[1].upper() in ("TOUCH", "CIRCLE") else 2
    if len(fields) > hold_index:
        fields[hold_index] = _scale_time(fields[hold_index], factor)
    return ":".join(fields)


def scale_session(text: str, factor: int) -> str:
    """The session file with its frame counts multiplied by `factor` (N3-21): the keys and holds of events written
    in frames, the checkpoint frames, the `# frames:` total and the `# hit:` frames; `# name:` gets `-x<factor>`.
    Milliseconds, the checkpoint scene values, captures and every other line are unchanged."""
    if factor < 1:
        raise UsageError("--factor must be at least 1")
    out: list[str] = []
    for raw in text.splitlines():
        stripped = raw.strip()
        if not stripped:
            out.append(raw)
            continue
        if stripped.startswith("#"):
            match = _HEADER.match(stripped)
            if not match:
                out.append(raw)
                continue
            key, value = match.group(1), match.group(2).strip()
            if key == "name":
                out.append(f"# name: {value}-x{factor}")
            elif key == "frames":
                if not value.isdigit():
                    raise UsageError(f"bad '# frames:' line {raw!r}")
                out.append(f"# frames: {int(value) * factor}")
            elif key == "checkpoints":
                items = []
                for part in (p.strip() for p in value.split(",") if p.strip()):
                    frame, sep, scene = part.partition(":")
                    if not sep or not frame.isdigit():
                        raise UsageError(f"bad checkpoint {part!r}")
                    items.append(f"{int(frame) * factor}:{scene}")
                out.append("# checkpoints: " + ", ".join(items))
            else:   # hit
                frames = [p.strip() for p in value.split(",") if p.strip()]
                if not all(f.isdigit() for f in frames):
                    raise UsageError(f"bad '# hit:' line {raw!r}")
                out.append("# hit: " + ", ".join(str(int(f) * factor) for f in frames))
            continue
        body, hash_, comment = raw.partition("#")
        items = [_scale_event(item, factor) for item in body.split(",") if item.strip()]
        out.append(",".join(items) + ((" #" + comment) if hash_ else ""))
    return "\n".join(out) + ("\n" if text.endswith("\n") else "")


@dataclass(frozen=True)
class Press:
    kind: str                # BUTTON name, TOUCH or CIRCLE
    start: int               # frame
    hold: int                # frames
    x: float = 0.0
    y: float = 0.0

    @property
    def end(self) -> int:    # first frame after the hold
        return self.start + self.hold

    @property
    def moves(self) -> bool:     # the circle pad held away from the centre
        return self.kind == "CIRCLE" and (self.x != 0.0 or self.y != 0.0)


@dataclass(frozen=True)
class Session:
    name: str
    frames: int
    presses: list[Press]
    hits: list[int]
    factor: int              # K of a name ending in -x<K>, else 1


def _hold_frames(text: Optional[str], fps: int) -> int:
    if text is None:
        return max(1, math.ceil(DEFAULT_HOLD_MS * fps / 1000))
    match = _FRAME_KEY.match(text)
    if match:
        return max(1, int(match.group(1)))
    if text.isdigit():
        return max(1, math.ceil(int(text) * fps / 1000))
    raise UsageError(f"bad hold {text!r}")


def parse_session(text: str, fps: Optional[int] = None) -> Session:
    name, frames = "", 0
    hits: list[int] = []
    items: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            match = _HEADER.match(line)
            if match:
                key, value = match.group(1), match.group(2).strip()
                if key == "name":
                    name = value
                elif key == "frames" and value.isdigit():
                    frames = int(value)
                elif key == "hit":
                    hits += [int(p) for p in (q.strip() for q in value.split(",")) if p.isdigit()]
            continue
        items += [i.strip() for i in line.split("#", 1)[0].split(",") if i.strip()]
    suffix = re.search(r"-x(\d+)$", name)
    factor = int(suffix.group(1)) if suffix else 1
    rate = fps if fps is not None else FRAME_BASE_FPS * factor
    presses: list[Press] = []
    for item in items:
        fields = item.split(":")
        key = _FRAME_KEY.match(fields[0])
        if len(fields) < 2 or not key:
            raise UsageError(f"event {item!r}: want f<frame>:KIND[:...]")
        kind = fields[1].upper()
        if kind in ("TOUCH", "CIRCLE"):
            if len(fields) < 4:
                raise UsageError(f"event {item!r}: want f<frame>:{kind}:x:y[:hold]")
            try:
                x, y = float(fields[2]), float(fields[3])
            except ValueError:
                raise UsageError(f"event {item!r}: bad coordinates") from None
            hold = _hold_frames(fields[4] if len(fields) > 4 else None, rate)
            presses.append(Press(kind, int(key.group(1)), hold, x, y))
        elif kind in BUTTONS:
            hold = _hold_frames(fields[2] if len(fields) > 2 else None, rate)
            presses.append(Press(kind, int(key.group(1)), hold))
        else:
            raise UsageError(f"event {item!r}: unknown button {fields[1]!r}")
    return Session(name, frames, presses, hits, factor)


# -- traces --------------------------------------------------------------------------------------------------

def f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def hunt_records(records: list[state_diff.Record], names: tuple[str, ...], start: Optional[int],
                 end: Optional[int]) -> list[state_diff.Record]:
    """The records of the hunt: from `start` (default: the last scene's stretch, from the first record in which every
    name is readable) to `end` (default: the last record)."""
    if not records:
        raise UsageError("the trace has no records")
    for name in names:
        if not any(name in r.values for r in records):
            raise UsageError(f"the trace has no {name}= value (record it with the candidate lines)")
    first = start
    if first is None:
        first = records[0].frame
        previous: Optional[int] = None
        for r in records:
            scene = r.values.get(SCENE)
            if scene is None:
                continue
            if previous is not None and scene != previous:
                first = r.frame
            previous = scene
    last = end if end is not None else records[-1].frame
    chosen = [r for r in records if first <= r.frame <= last]
    if start is None:
        while chosen and any(chosen[0].values.get(n) is None for n in names):
            chosen.pop(0)
    if not chosen:
        raise UsageError("no record of the hunt reads every named value")
    return chosen


def _busy(session: Session) -> list[tuple[int, int]]:
    """Merged [start, end) frame ranges with the circle pad held away from the centre."""
    spans = sorted((p.start, p.end) for p in session.presses if p.moves)
    merged: list[tuple[int, int]] = []
    for s, e in spans:
        if merged and s <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], e))
        else:
            merged.append((s, e))
    return merged


def idle_stretches(session: Session, first: int, last: int) -> list[tuple[int, int]]:
    """Inclusive frame ranges inside [first, last] without movement input."""
    stretches: list[tuple[int, int]] = []
    cursor = first
    for s, e in _busy(session):
        if e <= first or s > last:
            continue
        if s - 1 >= cursor:
            stretches.append((cursor, s - 1))
        cursor = max(cursor, e)
    if cursor <= last:
        stretches.append((cursor, last))
    return stretches


def _values(records: list[state_diff.Record], names: tuple[str, ...], lo: int, hi: int):
    return [tuple(r.values[n] for n in names) for r in records if lo <= r.frame <= hi]


def validate_position(records: list[state_diff.Record], session: Session, names: tuple[str, ...], fps: int,
                      start: Optional[int] = None, end: Optional[int] = None, settle: int = 0) -> tuple[bool, str, list[str]]:
    """(valid, one-line reason, detail lines)."""
    if len(names) != 3:
        raise UsageError("the position rule wants three names")
    hunt = hunt_records(records, names, start, end)
    first, last = hunt[0].frame, hunt[-1].frame
    for r in hunt:
        for n in names:
            bits = r.values.get(n)
            if bits is None:
                return False, f"{n} is unreadable at frame {r.frame}", []
            value = f32(bits)
            if not math.isfinite(value) or abs(value) >= MAGNITUDE_LIMIT:
                return False, f"{n} is not a float under {MAGNITUDE_LIMIT:g} at frame {r.frame} (0x{bits:08x})", []
    min_frames = round(MIN_IDLE_SECONDS * fps)
    details = [f"hunt frames {first} to {last}, {len(hunt)} records, fps {fps}"]
    idle = 0
    for lo, hi in idle_stretches(session, first, last):
        if hi - lo + 1 < min_frames:
            continue
        if any(lo <= h <= hi for h in session.hits):
            details.append(f"stretch {lo}..{hi} left out (a hit taken)")
            continue
        rows = _values(hunt, names, lo + settle, hi)
        if len(rows) < 2:
            continue
        idle += 1
        moved = next((i for i, row in enumerate(rows) if row != rows[0]), None)
        if moved is not None:
            frame = [r.frame for r in hunt if lo + settle <= r.frame <= hi][moved]
            return False, (f"the position changes inside the stretch {lo}..{hi} without movement input "
                           f"(first at frame {frame})"), details
    held = 0
    for s, e in _busy(session):
        lo, hi = max(s, first), min(e - 1, last)
        if hi <= lo:
            continue
        rows = _values(hunt, names, lo, hi)
        if len(rows) < 2:
            continue
        held += 1
        if rows[0] == rows[-1]:
            return False, f"the position does not change while the circle pad is held, frames {s}..{e - 1}", details
    if idle == 0:
        return False, f"no stretch of {min_frames} frames or more without movement input in the hunt", details
    if held == 0:
        return False, "no stretch with the circle pad held in the hunt", details
    details.append(f"{idle} stretches without movement input constant, {held} with the circle pad held change")
    return True, "valid", details


def validate_quest_time(records: list[state_diff.Record], names: tuple[str, ...], fps: int, as_float: bool = False,
                        start: Optional[int] = None, end: Optional[int] = None,
                        expect_rate: Optional[float] = None) -> tuple[bool, str, list[str]]:
    if len(names) != 1:
        raise UsageError("the quest_time rule wants one name")
    hunt = hunt_records(records, names, start, end)
    name = names[0]
    for r in hunt:
        if r.values.get(name) is None:
            return False, f"{name} is unreadable at frame {r.frame}", []
    first, last = hunt[0], hunt[-1]
    if last.frame <= first.frame:
        return False, "the hunt is one record long", []

    def number(r: state_diff.Record) -> float:
        bits = r.values[name]
        return f32(bits) if as_float else float(bits)

    if as_float and any(not math.isfinite(number(r)) for r in hunt):
        return False, f"{name} is not a finite float", []
    seconds = (last.frame - first.frame) / fps
    change = number(last) - number(first)
    details = [f"hunt frames {first.frame} to {last.frame}, {len(hunt)} records, {seconds:.3f} s of game time at {fps} fps"]
    if change == 0:
        return False, f"{name} does not move over the hunt", details
    rate = change / seconds
    details.append(f"rate {rate:.6g} per second")
    stalls = sum(1 for s in quest_segments(records, name, fps, as_float, start, end) if not s.running)
    if stalls:
        details.append(f"{stalls} stretch(es) in which it does not move (see the segments command)")
    worst, worst_frame = 0.0, first.frame
    for r in hunt:
        line = number(first) + change * (r.frame - first.frame) / (last.frame - first.frame)
        error = abs(number(r) - line) / abs(change)
        if error > worst:
            worst, worst_frame = error, r.frame
    details.append(f"largest distance from the straight line {worst * 100:.4f} % of the change (frame {worst_frame})")
    if worst > RATE_TOLERANCE:
        return False, (f"{name} leaves the straight line by {worst * 100:.3f} % of the whole change at frame "
                       f"{worst_frame}; the limit is {RATE_TOLERANCE * 100:g} %"), details
    if expect_rate is not None:
        if expect_rate == 0 or abs(rate - expect_rate) / abs(expect_rate) > RATE_TOLERANCE:
            return False, f"the rate {rate:.6g} per second is not within {RATE_TOLERANCE * 100:g} % of {expect_rate:g}", details
    return True, "valid", details


@dataclass(frozen=True)
class Segment:
    running: bool
    first: int            # frames of the records it covers
    last: int
    seconds: float
    rate: float           # units per second of game time (0 when paused)
    deviation: float      # largest distance from the straight line, as a fraction of the segment's change


def quest_segments(records: list[state_diff.Record], name: str, fps: int, as_float: bool = False,
                   start: Optional[int] = None, end: Optional[int] = None) -> list[Segment]:
    """The hunt cut into windows of one second of game time (`fps` frames) and merged into stretches in which the
    counter moves in every window (running) or in none (paused). A counter that pauses (a cutscene, a talk) is not
    one that moves by the same amount per second over the whole hunt; this says where it moves, and by how much."""
    hunt = hunt_records(records, (name,), start, end)
    for r in hunt:
        if r.values.get(name) is None:
            raise UsageError(f"{name} is unreadable at frame {r.frame}")

    def number(r: state_diff.Record) -> float:
        bits = r.values[name]
        return f32(bits) if as_float else float(bits)

    by_frame = {r.frame: r for r in hunt}
    frames = sorted(by_frame)
    windows: list[tuple[int, int, bool]] = []
    cursor = frames[0]
    while cursor < frames[-1]:
        upper = min(cursor + fps, frames[-1])
        lo = by_frame[max(f for f in frames if f <= cursor)]
        hi = by_frame[max(f for f in frames if f <= upper)]
        windows.append((lo.frame, hi.frame, number(hi) != number(lo)))
        cursor = upper
    segments: list[Segment] = []
    index = 0
    while index < len(windows):
        state = windows[index][2]
        stop = index
        while stop + 1 < len(windows) and windows[stop + 1][2] == state:
            stop += 1
        first, last = windows[index][0], windows[stop][1]
        rows = [r for r in hunt if first <= r.frame <= last]
        if state:      # a running stretch starts at the record before its first change and ends at its last change
            moves = [k for k in range(1, len(rows)) if number(rows[k]) != number(rows[k - 1])]
            rows = rows[moves[0] - 1: moves[-1] + 1]
            first, last = rows[0].frame, rows[-1].frame
        change = number(rows[-1]) - number(rows[0])
        seconds = (last - first) / fps
        deviation = 0.0
        if change != 0 and last > first:
            deviation = max(abs(number(r) - (number(rows[0]) + change * (r.frame - first) / (last - first)))
                            for r in rows) / abs(change)
        segments.append(Segment(state, first, last, seconds, change / seconds if seconds else 0.0, deviation))
        index = stop + 1
    return segments


def cmd_segments(args: argparse.Namespace) -> int:
    try:
        records = state_diff.parse_trace(_read_text(args.trace))
    except state_diff.FormatError as error:
        raise UsageError(str(error)) from None
    probe = parse_session(_read_text(args.session), 60)
    fps = args.fps if args.fps else FRAME_BASE_FPS * probe.factor
    for s in quest_segments(records, args.name, fps, args.float, args.start, args.end):
        if s.running:
            print(f"running frames {s.first}..{s.last} ({s.seconds:.1f} s) rate {s.rate:.6g} per second, "
                  f"largest distance from the straight line {s.deviation * 100:.3f} %")
        else:
            print(f"paused  frames {s.first}..{s.last} ({s.seconds:.1f} s)")
    return 0


def _read_text(path: str) -> str:
    try:
        return Path(path).read_text(encoding="utf-8")
    except OSError as error:
        raise UsageError(f"cannot read {path}: {error.strerror}") from None


def cmd_scale(args: argparse.Namespace) -> int:
    text = scale_session(_read_text(args.session), args.factor)
    try:
        Path(args.output).write_text(text, encoding="utf-8", newline="\n")
    except OSError as error:
        raise UsageError(f"cannot write {args.output}: {error.strerror}") from None
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    try:
        records = state_diff.parse_trace(_read_text(args.trace))
    except state_diff.FormatError as error:
        raise UsageError(str(error)) from None
    session_text = _read_text(args.session)
    probe = parse_session(session_text, 60)
    fps = args.fps if args.fps else FRAME_BASE_FPS * probe.factor
    session = parse_session(session_text, fps)
    default = DEFAULT_POSITION if args.rule == "position" else DEFAULT_QUEST_TIME
    names = tuple(n.strip() for n in args.names.split(",")) if args.names else default
    if args.rule == "position":
        ok, why, details = validate_position(records, session, names, fps, args.start, args.end, args.settle)
    else:
        ok, why, details = validate_quest_time(records, names, fps, args.float, args.start, args.end, args.expect_rate)
    for line in details:
        print(line, file=sys.stderr)
    print("valid" if ok else f"invalid: {why}")
    return 0 if ok else 1


# -- hunting in memory dumps ---------------------------------------------------------------------------------

_DUMP = re.compile(r"mem_f(\d+)_([0-9a-fA-F]+)\.bin")
BLOCK = 4096


class Dump:
    """One memory dump, read through a memory map."""

    def __init__(self, path: Path, base: int):
        self.path, self.base = path, base
        self._file = open(path, "rb")
        self.map = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)
        self.words = memoryview(self.map).cast("I") if len(self.map) % 4 == 0 else memoryview(self.map)[: len(self.map) // 4 * 4].cast("I")

    def close(self) -> None:
        self.words.release()
        self.map.close()
        self._file.close()

    def __len__(self) -> int:
        return len(self.words)


def load_dumps(folder: str, base: int, frames: list[int]) -> dict[int, Dump]:
    root = Path(folder)
    if not root.is_dir():
        raise UsageError(f"{folder} is not a folder")
    found: dict[int, Path] = {}
    for path in root.rglob("mem_f*.bin"):
        match = _DUMP.fullmatch(path.name)
        if match and int(match.group(2), 16) == base:
            found[int(match.group(1))] = path
    missing = [f for f in frames if f not in found]
    if missing:
        raise UsageError(f"no dump of address 0x{base:08x} at frame(s) {', '.join(map(str, missing))} in {folder}")
    dumps = {f: Dump(found[f], base) for f in sorted(set(frames))}
    if len({len(d) for d in dumps.values()}) != 1:
        raise UsageError("the dumps are not all the same size")
    return dumps


def changed_words(a: Dump, b: Dump) -> list[int]:
    """Indices of the words that differ, skipping equal blocks quickly."""
    out: list[int] = []
    wa, wb = a.words, b.words
    step = BLOCK // 4
    for start in range(0, len(wa), step):
        stop = min(start + step, len(wa))
        if a.map[start * 4: stop * 4] == b.map[start * 4: stop * 4]:
            continue
        out.extend(i for i in range(start, stop) if wa[i] != wb[i])
    return out


def _small(bits: int) -> bool:
    value = f32(bits)
    return math.isfinite(value) and abs(value) < MAGNITUDE_LIMIT


def _parse_frames(text: str, what: str) -> list[int]:
    try:
        return [int(p) for p in text.split(",") if p.strip()]
    except ValueError:
        raise UsageError(f"{what}: {text!r} is not a list of frames") from None


def find_position(dumps: dict[int, Dump], still: list[list[int]], moving: list[int]) -> list[tuple[int, list[tuple[float, float, float]], float]]:
    """(index of the first word, the three floats in each moving dump, speed spread), best first."""
    if len(moving) < 2:
        raise UsageError("--moving wants at least two frames")
    seeds: set[int] = set()
    for f0, f1 in zip(moving, moving[1:]):
        seeds.update(changed_words(dumps[f0], dumps[f1]))
    size = len(dumps[moving[0]])
    starts: set[int] = set()
    for i in seeds:
        for j in (i - 2, i - 1, i):
            if 0 <= j <= size - 3:
                starts.add(j)
    everything = sorted({f for group in still for f in group} | set(moving))
    results = []
    for j in sorted(starts):
        ok = True
        for group in still:
            ref = [dumps[group[0]].words[j + k] for k in range(3)]
            if any([dumps[f].words[j + k] for k in range(3)] != ref for f in group[1:]):
                ok = False
                break
        if not ok:
            continue
        if not all(_small(dumps[f].words[j + k]) for f in everything for k in range(3)):
            continue
        points = [tuple(f32(dumps[f].words[j + k]) for k in range(3)) for f in moving]
        steps = [tuple(points[i + 1][k] - points[i][k] for k in range(3)) for i in range(len(moving) - 1)]
        if any(all(c == 0 for c in step) for step in steps):
            continue
        rates = [tuple(c / (moving[i + 1] - moving[i]) for c in step) for i, step in enumerate(steps)]
        mean = [sum(r[k] for r in rates) / len(rates) for k in range(3)]
        norm = math.sqrt(sum(c * c for c in mean)) or 1.0
        spread = max(math.sqrt(sum((r[k] - mean[k]) ** 2 for k in range(3))) for r in rates) / norm
        results.append((j, points, spread))
    results.sort(key=lambda item: (item[2], item[0]))
    return results


def find_counter(dumps: dict[int, Dump], frames: list[int], tolerance: float, as_float: bool) -> list[tuple[int, float, float]]:
    """(index, first value, per frame) of the words that move linearly over the dumps."""
    if len(frames) < 3:
        raise UsageError("--frames wants at least three frames")
    first, last = frames[0], frames[-1]
    out = []
    for i in changed_words(dumps[first], dumps[last]):
        values = [dumps[f].words[i] for f in frames]
        if as_float:
            if not all(math.isfinite(f32(v)) for v in values):
                continue
            nums = [f32(v) for v in values]
        else:
            nums = [float(v) for v in values]
        change = nums[-1] - nums[0]
        if change == 0:
            continue
        if all(abs(n - (nums[0] + change * (f - first) / (last - first))) <= tolerance * abs(change)
               for n, f in zip(nums, frames)):
            out.append((i, nums[0], change / (last - first)))
    return out


def find_monotone(dumps: dict[int, Dump], frames: list[int], min_moving: int, as_float: bool,
                  down_only: bool) -> list[tuple[int, str, int, float, float]]:
    """(index, 'up'|'down', gaps in which it moves, first value, last value) of the words that never turn back over the
    dumps and move in at least `min_moving` gaps, but not in every gap by the same amount (a counter that pauses; the
    linear ones are find_counter's)."""
    out = []
    for i in changed_words(dumps[frames[0]], dumps[frames[-1]]):
        values = [dumps[f].words[i] for f in frames]
        if as_float:
            if not all(math.isfinite(f32(v)) and abs(f32(v)) < 1e8 for v in values):
                continue
            nums = [f32(v) for v in values]
        else:
            nums = [float(v) for v in values]
        pairs = list(zip(nums, nums[1:]))
        up, down = all(b >= a for a, b in pairs), all(b <= a for a, b in pairs)
        if not (up or down) or (down_only and not down):
            continue
        moving = sum(1 for a, b in pairs if a != b)
        if moving >= min_moving:
            out.append((i, "up" if up else "down", moving, nums[0], nums[-1]))
    return out


def cmd_find_monotone(args: argparse.Namespace) -> int:
    base = int(args.base, 0)
    frames = _parse_frames(args.frames, "--frames")
    dumps = load_dumps(args.dumps, base, frames)
    try:
        results = find_monotone(dumps, frames, args.min_moving, args.float, args.down)
        print(f"{len(results)} monotone words")
        for i, way, moving, first, last in results[: args.limit]:
            print(f"0x{base + 4 * i:08x}  {way}  moves in {moving} of {len(frames) - 1} gaps  first {first:.8g}  last {last:.8g}")
        return 0 if results else 1
    finally:
        _close(dumps)


def _close(dumps: dict[int, Dump]) -> None:
    for d in dumps.values():
        d.close()


def cmd_find_position(args: argparse.Namespace) -> int:
    base = int(args.base, 0)
    still = [_parse_frames(g, "--still") for g in args.still.split(";") if g.strip()]
    moving = _parse_frames(args.moving, "--moving")
    dumps = load_dumps(args.dumps, base, [f for g in still for f in g] + moving)
    try:
        results = find_position(dumps, still, moving)
        print(f"{len(results)} candidate triples")
        for j, points, spread in results[: args.limit]:
            first, last = points[0], points[-1]
            print(f"0x{base + 4 * j:08x}  spread {spread:.4f}  first ({first[0]:.3f}, {first[1]:.3f}, {first[2]:.3f})  "
                  f"last ({last[0]:.3f}, {last[1]:.3f}, {last[2]:.3f})")
        return 0 if results else 1
    finally:
        _close(dumps)


def cmd_find_counter(args: argparse.Namespace) -> int:
    base = int(args.base, 0)
    frames = _parse_frames(args.frames, "--frames")
    dumps = load_dumps(args.dumps, base, frames)
    try:
        results = find_counter(dumps, frames, args.tolerance, args.float)
        print(f"{len(results)} linear words")
        for i, value, slope in results[: args.limit]:
            print(f"0x{base + 4 * i:08x}  first {value:.6g}  per frame {slope:.6g}")
        return 0 if results else 1
    finally:
        _close(dumps)


def cmd_words(args: argparse.Namespace) -> int:
    base = int(args.base, 0)
    addresses = [int(a, 0) for a in args.address.split(",") if a.strip()]
    root = Path(args.dumps)
    frames = sorted(int(m.group(1)) for p in root.rglob("mem_f*.bin")
                    if (m := _DUMP.fullmatch(p.name)) and int(m.group(2), 16) == base)
    dumps = load_dumps(args.dumps, base, frames)
    try:
        for f in frames:
            cells = []
            for a in addresses:
                index = (a - base) // 4
                if not 0 <= index < len(dumps[f]):
                    raise UsageError(f"0x{a:08x} is outside the dump")
                bits = dumps[f].words[index]
                cells.append(f"0x{bits:08x} {bits} {f32(bits):.6g}")
            print(f"f{f}: " + " | ".join(cells))
        return 0
    finally:
        _close(dumps)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Find and validate the observables of the 60/120 fps audit.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("scale", help="multiply a session's frame counts")
    p.add_argument("session")
    p.add_argument("--factor", type=int, default=2)
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(func=cmd_scale)

    p = sub.add_parser("validate", help="check a state trace against a rule")
    p.add_argument("--trace", required=True)
    p.add_argument("--session", required=True)
    p.add_argument("--rule", required=True, choices=("position", "quest_time"))
    p.add_argument("--names", help="comma separated observable names in the trace")
    p.add_argument("--fps", type=int, help="default: 60 times the K of a session named ...-x<K>")
    p.add_argument("--from", dest="start", type=int, help="first frame of the hunt")
    p.add_argument("--to", dest="end", type=int, help="last frame of the hunt")
    p.add_argument("--float", action="store_true", help="quest_time: the value is an IEEE float")
    p.add_argument("--expect-rate", type=float, help="quest_time: units per second the rate must match within 1 %%")
    p.add_argument("--settle", type=int, default=0, help="position: frames left out at the start of each stretch")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("segments", help="where a counter runs and where it pauses, with its rate")
    p.add_argument("--trace", required=True)
    p.add_argument("--session", required=True)
    p.add_argument("--name", default=DEFAULT_QUEST_TIME[0])
    p.add_argument("--fps", type=int)
    p.add_argument("--from", dest="start", type=int)
    p.add_argument("--to", dest="end", type=int)
    p.add_argument("--float", action="store_true")
    p.set_defaults(func=cmd_segments)

    p = sub.add_parser("find-position", help="triples of floats that follow the circle pad in memory dumps")
    p.add_argument("dumps")
    p.add_argument("--base", required=True)
    p.add_argument("--still", required=True)
    p.add_argument("--moving", required=True)
    p.add_argument("--limit", type=int, default=40)
    p.set_defaults(func=cmd_find_position)

    p = sub.add_parser("find-counter", help="words that move linearly with the frame in memory dumps")
    p.add_argument("dumps")
    p.add_argument("--base", required=True)
    p.add_argument("--frames", required=True)
    p.add_argument("--tolerance", type=float, default=RATE_TOLERANCE)
    p.add_argument("--float", action="store_true")
    p.add_argument("--limit", type=int, default=200)
    p.set_defaults(func=cmd_find_counter)

    p = sub.add_parser("find-monotone", help="words that never turn back and move in many gaps (a counter that pauses)")
    p.add_argument("dumps")
    p.add_argument("--base", required=True)
    p.add_argument("--frames", required=True)
    p.add_argument("--min-moving", type=int, default=6)
    p.add_argument("--down", action="store_true", help="only counters that count down")
    p.add_argument("--float", action="store_true")
    p.add_argument("--limit", type=int, default=200)
    p.set_defaults(func=cmd_find_monotone)

    p = sub.add_parser("words", help="the words at some addresses in every dump")
    p.add_argument("dumps")
    p.add_argument("--base", required=True)
    p.add_argument("--address", required=True)
    p.set_defaults(func=cmd_words)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except UsageError as error:
        print(f"observable_probe: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
