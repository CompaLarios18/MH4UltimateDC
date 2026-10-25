#!/usr/bin/env python3
"""Report how two runs of one session on two timing models compare (N0-28, D31, D50).

`jit` against `recomp`, and later the reference against the native runtime, run the same input script
on clocks that differ.  What a different clock may change is calibrated with this report (and with
`golden.py check --profile cross`); `docs/native/cross-model.md` keeps the numbers.  The report reads the
two runs' folders (a golden recording, and the `check/run1` folder of a cross-profile check), prints
markdown with frames and counts only (never a value of the game's memory, a file name or a buffer), and
needs nothing but the standard library:

  - **Scene changes.**  Each *real* scene change of the state trace (N0-25: between two readable scene
    values; the first record and a change from `-` to a value are not real; `state_diff.real_scene_changes`):
    its frame on each side and the difference.  The k-th change of one side is matched with the k-th of the
    other, as `state_diff.py` does.
  - **Input events.**  Each event of the session script: the frame of the last real scene change at or
    before it on each side (`-` before the first), and the margin, the event's frame minus that frame.  A
    slower or faster runtime's slack shows as a margin that differs between the sides.
  - **SVC calls per thread.**  Each side's number of calls of every thread, under the cross-model names of
    N0-25 (`main`, `<parent name>/<entry>#<k>`), and for each thread that differs the SVC numbers whose
    counts differ most.
  - **IPC requests per command** (when both folders hold an `ipc.txt`).  For every command (service and
    request header) whose per-thread request counts differ between the sides: the total on each side and the
    threads that differ.  What a reply or a descriptor holds is not looked at here: `compare_ipc_traces.py`
    does that.

usage: cross_model_report.py --session FILE --reference DIR --candidate DIR [--observable FILE] [--tolerance N]
                             [--min-margin N] [--no-svc] [--no-ipc]
       cross_model_report.py --images --session FILE --reference DIR --candidate DIR [--proposal FILE]
                             [--min-frames N] [--long-scene N] [--margin-db DB] [--margin-block N]
                             [--not-judged FIRST-LAST]... [--no-per-frame]

`--images` (N2b-17) reads the screenshots of the two folders (`shots/f<frame>_top.ppm` and `_bottom.ppm`, every 60
frames from frame 300) and the reference's state trace, lists per frame and screen the PSNR and the worst 16x16 block
(`imgdiff.py`'s rule, ROADMAP section 6) and per scene the frames proposed, and proposes the frames where both
screens pass with a margin (D58: `--margin-db` 2.0 and `--margin-block` 1.0, so PSNR at least 42 dB and block at most
7.0 out of 255 on both screens; 0 and 0 give the plain rule; the gate itself keeps 40 dB and 8/255).  The proposal is
the image frames of the session (`--proposal FILE`: lines `<session> <frame>`; the session's lines are replaced).
Frames that pass the plain rule but not the margin are listed as narrow.  Frames in the candidate's `shots/gas.txt`
are never proposed.  Exit status: 0 when the proposal has at least `--min-frames` frames (3) and one in every scene
longer than `--long-scene` frames (300), except the scenes that start inside a `--not-judged FIRST-LAST` range (D58
names G4 903-1575 and G2 5086-9000, which the jit-calibrated gate does not judge; without the option those two
scenes exit 1); 1 when it falls short (each shortfall is named on stderr; the report and the proposal are still
written); 2 bad input (a negative number included).
exit status: 0 the report was written, every real scene change falls on the same scene within --tolerance
frames (default 4, D50) and every input event is at least --min-margin frames (default 60, D50) after the
last real scene change on both sides; 1 a real scene change is more than --tolerance frames apart, goes to
another scene or exists on one side only, or an input event that follows a real scene change has a smaller
margin on either side (each problem is named on stderr, the event by its frame and kind; the report is
still written); 2 bad input (a missing file, a trace that cannot be read, a negative number).  An input
event before the first real scene change has no margin and is not checked.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import imgdiff  # noqa: E402
import state_diff  # noqa: E402
import tracefmt  # noqa: E402

DEFAULT_OBSERVABLE = HERE.parents[1] / "config" / "native" / "observable.txt"
DEFAULT_TOLERANCE = 4       # frames a real scene change may move by across timing models (D50; D33 said 2)
DEFAULT_MIN_MARGIN = 60     # frames an input must come after the last real scene change on both sides (D50)

# The SVC numbers of the 3DS (facts of its interface), for the names printed beside a number.
SVC_NAMES = {
    "01": "ControlMemory", "08": "CreateThread", "09": "ExitThread", "0A": "SleepThread",
    "13": "CreateMutex", "14": "ReleaseMutex", "15": "CreateSemaphore", "16": "ReleaseSemaphore",
    "17": "CreateEvent", "18": "SignalEvent", "19": "ClearEvent", "1A": "CreateTimer", "1B": "SetTimer",
    "1E": "CreateMemoryBlock", "1F": "MapMemoryBlock", "20": "UnmapMemoryBlock", "21": "CreateAddressArbiter",
    "22": "ArbitrateAddress", "23": "CloseHandle", "24": "WaitSynchronization1", "25": "WaitSynchronizationN",
    "28": "GetSystemTick", "2D": "ConnectToPort", "32": "SendSyncRequest", "4F": "ReplyAndReceive",
}


class ReportError(Exception):
    """An input that cannot be read (exit status 2)."""


# ── scene changes ───────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Change:
    scene: int
    frame: int


def read_scene_changes(path: Path) -> list[Change]:
    """The real scene changes of a state trace, in order."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ReportError(f"cannot read {path}: {getattr(error, 'strerror', None) or error}") from None
    try:
        records = state_diff.parse_trace(text)
    except state_diff.FormatError as error:
        raise ReportError(f"{path}: {error}") from None
    if not records:
        raise ReportError(f"{path}: the state trace has no records")
    if any(state_diff.SCENE not in r.values for r in records[:1]):
        raise ReportError(f"{path}: the state trace has no {state_diff.SCENE}= value")
    return [Change(r.values[state_diff.SCENE], r.frame) for r in state_diff.real_scene_changes(records)]


def trace_cpu(path: Path) -> str:
    """The `cpu=` field of a trace's header line, `?` when there is none."""
    try:
        with open(path, encoding="utf-8") as f:
            first = f.readline()
    except (OSError, UnicodeError):
        return "?"
    m = re.search(r"\bcpu=(\S+)", first)
    return m.group(1) if m else "?"


def _scene(value: int) -> str:
    return f"0x{value:08x}"


def scene_table(ref: Sequence[Change], cand: Sequence[Change], tolerance: int) -> tuple[list[str], list[str]]:
    """(markdown lines, problems): every real change of either side, k-th against k-th."""
    lines = ["| # | scene (reference) | scene (candidate) | frame (reference) | frame (candidate) | difference | within |",
             "|---|---|---|---|---|---|---|"]
    problems: list[str] = []
    for k in range(max(len(ref), len(cand))):
        r = ref[k] if k < len(ref) else None
        c = cand[k] if k < len(cand) else None
        if r is None or c is None:
            side = "candidate" if r is None else "reference"
            lines.append(f"| {k + 1} | {_scene(r.scene) if r else '-'} | {_scene(c.scene) if c else '-'} | "
                         f"{r.frame if r else '-'} | {c.frame if c else '-'} | - | no: only the {side} has it |")
            problems.append(f"real scene change #{k + 1} exists on the {side} only")
            continue
        diff = c.frame - r.frame
        same_scene = r.scene == c.scene
        within = same_scene and abs(diff) <= tolerance
        lines.append(f"| {k + 1} | {_scene(r.scene)} | {_scene(c.scene)} | {r.frame} | {c.frame} | {diff:+d} | "
                     f"{'yes' if within else 'no'} |")
        if not same_scene:
            problems.append(f"real scene change #{k + 1}: another scene on each side")
        elif abs(diff) > tolerance:
            problems.append(f"real scene change #{k + 1}: {abs(diff)} frames apart, more than {tolerance}")
    return lines, problems


# ── input events ────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Event:
    frame: int
    label: str      # `A`, `TOUCH`, `CIRCLE`, never the coordinates' meaning: a button or a kind of input


_EVENT = re.compile(r"^f(\d+):([A-Za-z]+)(?::.*)?$")


def parse_events(text: str) -> list[Event]:
    """The input events of a session script (golden.py's format: header lines start with `#`, an event
    line holds comma-separated `f<frame>:BUTTON[:hold]`, `f<frame>:TOUCH:x:y[:hold]`,
    `f<frame>:CIRCLE:x:y[:hold]`)."""
    events: list[Event] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        for item in line.split(","):
            item = item.strip()
            if not item:
                continue
            m = _EVENT.match(item)
            if not m:
                raise ReportError(f"session line {number}: bad event {item!r}")
            events.append(Event(int(m.group(1)), m.group(2).upper()))
    return events


def last_change_at_or_before(changes: Sequence[Change], frame: int) -> Optional[Change]:
    found: Optional[Change] = None
    for change in changes:
        if change.frame > frame:
            break
        found = change
    return found


def _num(value: Optional[int]) -> str:
    return "-" if value is None else str(value)


def event_table(events: Sequence[Event], ref: Sequence[Change], cand: Sequence[Change]) -> list[str]:
    lines = ["| frame | input | last scene change (reference) | last scene change (candidate) | margin (reference) | "
             "margin (candidate) | margin difference |",
             "|---|---|---|---|---|---|---|"]
    for event in events:
        r = last_change_at_or_before(ref, event.frame)
        c = last_change_at_or_before(cand, event.frame)
        mr = event.frame - r.frame if r else None
        mc = event.frame - c.frame if c else None
        delta = f"{mc - mr:+d}" if mr is not None and mc is not None else "-"
        lines.append(f"| {event.frame} | {event.label} | {_num(r.frame if r else None)} | "
                     f"{_num(c.frame if c else None)} | {_num(mr)} | {_num(mc)} | {delta} |")
    return lines


def margin_problems(events: Sequence[Event], ref: Sequence[Change], cand: Sequence[Change],
                    min_margin: int) -> list[str]:
    """One problem per event and side whose margin is below `min_margin` (D50); an event with no real scene
    change before it on a side has no margin there and is not checked on that side."""
    problems: list[str] = []
    for event in events:
        for side, changes in (("reference", ref), ("candidate", cand)):
            change = last_change_at_or_before(changes, event.frame)
            if change is not None and event.frame - change.frame < min_margin:
                problems.append(f"input event at frame {event.frame} ({event.label}): margin "
                                f"{event.frame - change.frame} frames on the {side}, less than {min_margin}")
    return problems


def margin_verdict(events: Sequence[Event], ref: Sequence[Change], cand: Sequence[Change], min_margin: int) -> str:
    bad = [e for e in events if margin_problems([e], ref, cand, min_margin)]
    if not bad:
        return (f"- every input event is at least {min_margin} frames after the last real scene change on both "
                f"sides")
    n = len(bad)
    return (f"- {n} input event{' is' if n == 1 else 's are'} less than {min_margin} frames after the last real "
            f"scene change on one side or both")


def margin_summary(events: Sequence[Event], ref: Sequence[Change], cand: Sequence[Change]) -> list[str]:
    """The smallest margin on each side among the events that follow a real scene change."""
    out: list[str] = []
    for label, changes in (("reference", ref), ("candidate", cand)):
        margins = [(e.frame - c.frame, e.frame) for e in events if (c := last_change_at_or_before(changes, e.frame))]
        if margins:
            small = min(margins)
            out.append(f"- smallest margin on the {label}: {small[0]} frames (the event at frame {small[1]})")
        else:
            out.append(f"- no input event follows a real scene change on the {label}")
    return out


# ── SVC calls per thread ────────────────────────────────────────────────────────────

@dataclass
class SvcCounts:
    names: dict[str, str]                       # thread id -> cross-model name
    calls: Counter                              # (thread name, svc) -> calls

    def per_thread(self) -> Counter:
        total: Counter = Counter()
        for (thread, _svc), n in self.calls.items():
            total[thread] += n
        return total


def count_svc(path: Path) -> SvcCounts:
    """Stream an SVC trace: the thread lines give the names, every other line is one call of its thread."""
    raw_calls: Counter = Counter()
    thread_lines: list[str] = []
    try:
        with open(path, "rb") as f:
            for raw in f:
                if raw.startswith(b"#"):
                    if raw.startswith(b"# thread"):
                        thread_lines.append(raw.decode("utf-8", "replace"))
                    continue
                parts = raw.split(None, 2)
                if len(parts) < 2:
                    continue
                raw_calls[(parts[0].decode("ascii", "replace"), parts[1].decode("ascii", "replace").upper())] += 1
    except OSError as error:
        raise ReportError(f"cannot read {path}: {error.strerror}") from None
    if not raw_calls:
        raise ReportError(f"{path}: the SVC trace holds no call")
    trace = tracefmt.parse_lines(thread_lines)
    names = tracefmt.thread_names(trace, per_parent=True)
    calls: Counter = Counter()
    for (thread, svc), n in raw_calls.items():
        calls[(names.get(thread, thread), svc)] += n
    return SvcCounts(names, calls)


def _thread_key(name: str) -> tuple:
    return (0, name) if name == "main" else (1, name)


def _svc_label(svc: str) -> str:
    return f"0x{svc} {SVC_NAMES[svc]}" if svc in SVC_NAMES else f"0x{svc}"


def svc_table(ref: SvcCounts, cand: SvcCounts, detail: int = 3) -> list[str]:
    rt, ct = ref.per_thread(), cand.per_thread()
    threads = sorted(set(rt) | set(ct), key=_thread_key)
    lines = ["| thread | calls (reference) | calls (candidate) | difference | the SVC numbers that differ most |",
             "|---|---|---|---|---|"]
    differing = 0
    for thread in threads:
        a, b = rt.get(thread, 0), ct.get(thread, 0)
        extra = ""
        if a != b:
            differing += 1
            svcs = {s for (t, s) in list(ref.calls) + list(cand.calls) if t == thread}
            deltas = sorted(((ref.calls.get((thread, s), 0), cand.calls.get((thread, s), 0), s) for s in svcs),
                            key=lambda x: (-abs(x[1] - x[0]), x[2]))
            extra = ", ".join(f"{_svc_label(s)} {x}/{y}" for x, y, s in deltas[:detail] if x != y)
        lines.append(f"| {thread} | {a} | {b} | {b - a:+d} | {extra or '-'} |")
    total_r, total_c = sum(rt.values()), sum(ct.values())
    lines.append(f"| **all {len(threads)} threads** | {total_r} | {total_c} | {total_c - total_r:+d} | "
                 f"{differing} of {len(threads)} threads differ |")
    return lines


# ── IPC requests per command ────────────────────────────────────────────────────────

def count_ipc(path: Path) -> Counter:
    """(session without its instance number, header, thread name) -> requests."""
    try:
        trace = tracefmt.parse_ipc(str(path))
    except (OSError, UnicodeError) as error:
        raise ReportError(f"cannot read {path}: {getattr(error, 'strerror', None) or error}") from None
    if trace.header is None or trace.header.get("tool") != "ipctrace":
        raise ReportError(f"{path}: not an IPC trace (no '# ipctrace' header)")
    names = tracefmt.thread_names(trace, per_parent=True)
    counts: Counter = Counter()
    for call in trace.calls:
        session = re.sub(r"#\d+$", "", call.session)
        counts[(session, call.cmd or "-", names.get(call.thread, call.thread))] += 1
    return counts


def ipc_table(ref: Counter, cand: Counter) -> list[str]:
    commands = sorted({(s, h) for (s, h, _t) in list(ref) + list(cand)})
    rows: list[str] = []
    for session, header in commands:
        threads = sorted({t for (s, h, t) in list(ref) + list(cand) if (s, h) == (session, header)},
                         key=_thread_key)
        diffs = [(t, ref.get((session, header, t), 0), cand.get((session, header, t), 0)) for t in threads]
        diffs = [d for d in diffs if d[1] != d[2]]
        if not diffs:
            continue
        total_r = sum(ref.get((session, header, t), 0) for t in threads)
        total_c = sum(cand.get((session, header, t), 0) for t in threads)
        shown = ", ".join(f"{t} {a}/{b}" for t, a, b in diffs[:6]) + (f", and {len(diffs) - 6} more" if len(diffs) > 6 else "")
        rows.append(f"| {session} | {header} | {total_r} | {total_c} | {total_c - total_r:+d} | {shown} |")
    total_r, total_c = sum(ref.values()), sum(cand.values())
    out = [f"{total_r} requests on the reference, {total_c} on the candidate, {len(commands)} distinct commands; "
           f"{len(rows)} of them with a different count on at least one thread.", ""]
    if rows:
        out += ["| service | header | requests (reference) | requests (candidate) | difference | threads that differ "
                "(reference/candidate) |", "|---|---|---|---|---|---|"] + rows
    return out


# ── the report ──────────────────────────────────────────────────────────────────────

def build_report(session_text: str, ref_dir: Path, cand_dir: Path, session_name: str = "",
                 tolerance: int = DEFAULT_TOLERANCE, svc: bool = True, ipc: bool = True,
                 min_margin: int = DEFAULT_MIN_MARGIN) -> tuple[str, list[str]]:
    """(markdown, problems): `problems` are the real scene changes that are not within the tolerance and the
    input events whose margin on either side is below `min_margin`."""
    ref_state, cand_state = ref_dir / "state.txt", cand_dir / "state.txt"
    ref_changes, cand_changes = read_scene_changes(ref_state), read_scene_changes(cand_state)
    events = parse_events(session_text)
    out = [f"## {session_name or 'session'}: {trace_cpu(cand_state)} (candidate) against {trace_cpu(ref_state)} "
           f"(reference)", "",
           f"### Real scene changes ({len(ref_changes)} on the reference, {len(cand_changes)} on the candidate; "
           f"tolerance {tolerance} frames)", ""]
    table, problems = scene_table(ref_changes, cand_changes, tolerance)
    out += table
    if ref_changes and cand_changes and not problems:
        worst = max(abs(c.frame - r.frame) for r, c in zip(ref_changes, cand_changes))
        out += ["", f"The largest difference is {worst} frame{'' if worst == 1 else 's'}."]
    out += ["", f"### Input events ({len(events)}; minimum margin {min_margin} frames)", ""]
    out += event_table(events, ref_changes, cand_changes)
    out += [""] + margin_summary(events, ref_changes, cand_changes)
    out += [margin_verdict(events, ref_changes, cand_changes, min_margin)]
    problems += margin_problems(events, ref_changes, cand_changes, min_margin)
    if svc:
        ref_svc, cand_svc = count_svc(ref_dir / "svc.txt"), count_svc(cand_dir / "svc.txt")
        out += ["", "### SVC calls per thread (cross-model names)", ""] + svc_table(ref_svc, cand_svc)
    if ipc and (ref_dir / "ipc.txt").is_file() and (cand_dir / "ipc.txt").is_file():
        out += ["", "### IPC requests per command", ""] + ipc_table(count_ipc(ref_dir / "ipc.txt"),
                                                                     count_ipc(cand_dir / "ipc.txt"))
    return "\n".join(out) + "\n", problems


# ── images (N2b-17) ─────────────────────────────────────────────────────────────────

IMAGE_MIN_FRAMES = 3        # image frames every calibrated session needs (N2b-17's first guess, rule 8)
IMAGE_LONG_SCENE = 300      # a scene longer than this (frames) needs an image frame of its own
IMAGE_MARGIN_DB = 2.0       # D58: a frame is proposed with this many dB over the PSNR threshold ...
IMAGE_MARGIN_BLOCK = 1.0    # ... and this much (of 255) under the block threshold
_SHOT = re.compile(r"^f?(\d+)_(top|bottom)\.ppm$")


@dataclass(frozen=True)
class Scene:
    scene: Optional[int]
    start: int
    end: int            # exclusive

    @property
    def length(self) -> int:
        return self.end - self.start


@dataclass
class ShotRow:
    frame: int
    top: Optional[imgdiff.Metrics] = None
    bottom: Optional[imgdiff.Metrics] = None
    problem: str = ""   # a screenshot missing on one side, or sizes that differ
    gas: bool = False

    @property
    def passes(self) -> bool:
        """The plain rule (ROADMAP section 6) on both screens."""
        return not self.problem and not self.gas and bool(self.top and self.top.passes
                                                          and self.bottom and self.bottom.passes)

    def proposed(self, margin_db: float = 0.0, margin_block: float = 0.0) -> bool:
        """Both screens pass with `margin_db` dB and `margin_block` (of 255) to spare (D58)."""
        if self.problem or self.gas or not (self.top and self.bottom):
            return False
        return all(m.psnr >= imgdiff.MIN_PSNR_DB + margin_db and m.worst_block <= imgdiff.MAX_BLOCK_ERROR - margin_block
                   for m in (self.top, self.bottom))


def shot_files(folder: Path) -> dict[int, dict[str, Path]]:
    """`<folder>/shots/f<frame>_top.ppm` and `_bottom.ppm`, by frame."""
    found: dict[int, dict[str, Path]] = {}
    shots = folder / "shots"
    if shots.is_dir():
        for p in shots.iterdir():
            m = _SHOT.match(p.name)
            if m:
                found.setdefault(int(m.group(1)), {})[m.group(2)] = p
    return found


def read_gas(folder: Path) -> set[int]:
    """The shot numbers of `<folder>/shots/gas.txt` (N2b-14: one per line, unpadded); empty when the file is
    missing, which is a runner that does not report gas."""
    path = folder / "shots" / "gas.txt"
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return set()
    except (OSError, UnicodeError) as error:
        raise ReportError(f"cannot read {path}: {getattr(error, 'strerror', None) or error}") from None
    frames: set[int] = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if not (line.isascii() and line.isdigit()):
            raise ReportError(f"{path}: line {number}: {line!r} is not a shot number")
        frames.add(int(line))
    return frames


def scene_intervals(state_path: Path) -> list[Scene]:
    """The scenes of a state trace between its real scene changes: from frame 0 to the first change, and so on up to
    the last record."""
    try:
        records = state_diff.parse_trace(state_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as error:
        raise ReportError(f"cannot read {state_path}: {getattr(error, 'strerror', None) or error}") from None
    except state_diff.FormatError as error:
        raise ReportError(f"{state_path}: {error}") from None
    if not records:
        raise ReportError(f"{state_path}: the state trace has no records")
    changes = state_diff.real_scene_changes(records)
    starts = [0] + [r.frame for r in changes]
    ends = starts[1:] + [records[-1].frame + 1]
    values = [next((r.values.get(state_diff.SCENE) for r in records if r.frame >= s and
                    r.values.get(state_diff.SCENE) is not None), None) for s in starts]
    return [Scene(v, s, e) for v, s, e in zip(values, starts, ends)]


def image_rows(ref_dir: Path, cand_dir: Path) -> list[ShotRow]:
    """One row per screenshot frame of the reference, compared with the candidate's at that frame."""
    ref, cand = shot_files(ref_dir), shot_files(cand_dir)
    gas = read_gas(cand_dir)
    rows: list[ShotRow] = []
    for frame in sorted(ref):
        row = ShotRow(frame, gas=frame in gas)
        for screen in ("top", "bottom"):
            a, b = ref[frame].get(screen), cand.get(frame, {}).get(screen)
            if a is None or b is None:
                row.problem = f"the {screen} screenshot is missing on the {'reference' if a is None else 'candidate'}"
                break
            try:
                setattr(row, screen, imgdiff.compare_files(a, b))
            except imgdiff.ImageError as error:
                row.problem = f"{screen}: {error}"
                break
        rows.append(row)
    if not rows:
        raise ReportError(f"{ref_dir}: the reference has no screenshots (shots/f<frame>_top.ppm)")
    if not cand:
        raise ReportError(f"{cand_dir}: the candidate has no screenshots")
    return rows


def scene_of(scenes: Sequence[Scene], frame: int) -> int:
    """The index of the scene a frame falls in."""
    for i, s in enumerate(scenes):
        if s.start <= frame < s.end:
            return i
    return len(scenes) - 1


def not_judged(scene: Scene, ranges: Sequence[tuple[int, int]]) -> bool:
    """A scene that starts inside one of the `--not-judged` ranges (first, last; both inclusive)."""
    return any(a <= scene.start <= b for a, b in ranges)


def image_coverage(rows: Sequence[ShotRow], scenes: Sequence[Scene], min_frames: int,
                   long_scene: int, margin_db: float = 0.0, margin_block: float = 0.0,
                   exempt: Sequence[tuple[int, int]] = ()) -> list[str]:
    """The ways the proposed frames (the margin set) fall short of the rule: fewer than `min_frames`, or a scene
    longer than `long_scene` frames with none, except the scenes named in `exempt` (D58)."""
    good = [r.frame for r in rows if r.proposed(margin_db, margin_block)]
    problems: list[str] = []
    if len(good) < min_frames:
        problems.append(f"only {len(good)} image frame{'' if len(good) == 1 else 's'}, at least {min_frames} needed")
    for i, s in enumerate(scenes):
        if not_judged(s, exempt):
            continue
        if s.length > long_scene and not any(s.start <= f < s.end for f in good):
            problems.append(f"scene #{i + 1} (frames {s.start} to {s.end - 1}, {s.length} frames) has no image frame")
    return problems


def _cell(m: Optional[imgdiff.Metrics]) -> str:
    if m is None:
        return "- | -"
    return f"{'inf' if m.psnr == float('inf') else format(m.psnr, '.1f')} | {m.worst_block:.1f}"


def _ranges(frames: Sequence[int], step: int) -> str:
    """`300-420 (3), 600` style: runs of frames `step` apart."""
    out: list[str] = []
    i = 0
    while i < len(frames):
        j = i
        while j + 1 < len(frames) and frames[j + 1] - frames[j] == step:
            j += 1
        out.append(str(frames[i]) if i == j else f"{frames[i]}-{frames[j]} ({j - i + 1})")
        i = j + 1
    return ", ".join(out) or "none"


def image_report(ref_dir: Path, cand_dir: Path, session_name: str = "",
                 min_frames: int = IMAGE_MIN_FRAMES, long_scene: int = IMAGE_LONG_SCENE,
                 per_frame: bool = True, margin_db: float = IMAGE_MARGIN_DB,
                 margin_block: float = IMAGE_MARGIN_BLOCK,
                 exempt: Sequence[tuple[int, int]] = ()) -> tuple[str, list[int], list[str]]:
    """(markdown, proposed frames, problems of coverage).  A frame is proposed when both screens pass with the
    margins to spare (D58); the gate itself judges with ROADMAP section 6's thresholds."""
    rows = image_rows(ref_dir, cand_dir)
    scenes = scene_intervals(ref_dir / "state.txt")
    proposed = [r.frame for r in rows if r.proposed(margin_db, margin_block)]
    narrow = [r.frame for r in rows if r.passes and not r.proposed(margin_db, margin_block)]
    problems = image_coverage(rows, scenes, min_frames, long_scene, margin_db, margin_block, exempt)
    step = min((b.frame - a.frame for a, b in zip(rows, rows[1:])), default=1) or 1
    thr = f"PSNR at least {imgdiff.MIN_PSNR_DB:g} dB and no {imgdiff.BLOCK_SIZE}x{imgdiff.BLOCK_SIZE} block above " \
          f"{imgdiff.MAX_BLOCK_ERROR:g}/255"
    plain = sum(1 for r in rows if r.passes)
    rule = (f"a frame is proposed when both screens pass with {margin_db:g} dB and {margin_block:g}/255 to spare "
            f"(PSNR at least {imgdiff.MIN_PSNR_DB + margin_db:g} dB and no block above "
            f"{imgdiff.MAX_BLOCK_ERROR - margin_block:g}/255)")
    out = [f"## {session_name or 'session'}: screenshots, {trace_cpu(cand_dir / 'state.txt')} (candidate) against "
           f"{trace_cpu(ref_dir / 'state.txt')} (reference)", "",
           f"{len(rows)} screenshot frames ({rows[0].frame} to {rows[-1].frame}, every {step}); the gate's rule is {thr}; "
           f"{rule}. **{len(proposed)} of {len(rows)} frames are proposed** ({plain} pass the plain rule).", "",
           f"### Scenes ({len(scenes)}; the reference's real scene changes)", "",
           "| # | scene | frames | length | screenshot frames | pass | proposed frames |", "|---|---|---|---|---|---|---|"]
    for i, s in enumerate(scenes):
        inside = [r for r in rows if scene_of(scenes, r.frame) == i]
        good = [r.frame for r in inside if r.proposed(margin_db, margin_block)]
        note = " (not judged by the jit-calibrated gate)" if not_judged(s, exempt) else ""
        out.append(f"| {i + 1} | {'-' if s.scene is None else _scene(s.scene)} | {s.start} to {s.end - 1} | {s.length} | "
                   f"{len(inside)} | {len(good)} | {_ranges(good, step)}{note} |")
    bad = [r for r in rows if not r.passes]
    out += ["", f"### Frames that do not pass the plain rule ({len(bad)})", ""]
    if bad:
        out += [", ".join(str(r.frame) for r in bad)]
    else:
        out += ["none"]
    out += ["", f"### Frames that pass the plain rule but are not proposed: narrow ({len(narrow)})", "",
            ", ".join(str(f) for f in narrow) or "none"]
    if per_frame:
        out += ["", "### Per frame", "",
                "| frame | scene # | top PSNR (dB) | top worst block | bottom PSNR (dB) | bottom worst block | proposed |",
                "|---|---|---|---|---|---|---|"]
        for r in rows:
            verdict = ("gas" if r.gas else r.problem) if (r.gas or r.problem) else (
                "yes" if r.proposed(margin_db, margin_block) else ("narrow" if r.passes else "no"))
            out.append(f"| {r.frame} | {scene_of(scenes, r.frame) + 1} | {_cell(r.top)} | {_cell(r.bottom)} | "
                       f"{verdict} |")
    out += ["", "### Coverage", ""]
    out += [f"- {p}" for p in problems] or [f"- at least {min_frames} image frames, and one in every scene longer than "
                                           f"{long_scene} frames: met"]
    skipped = [f"#{i + 1} (frames {s.start} to {s.end - 1})" for i, s in enumerate(scenes) if not_judged(s, exempt)]
    if skipped:
        out += [f"- not judged by the jit-calibrated gate, as named with --not-judged: scene {', '.join(skipped)}"]
    return "\n".join(out) + "\n", proposed, problems


def write_proposal(path: Path, session: str, frames: Sequence[int]) -> None:
    """Replace the lines of `session` in the proposal file (`<session> <frame>` per line); other sessions' lines stay."""
    keep: list[str] = []
    if path.is_file():
        keep = [ln for ln in path.read_text(encoding="utf-8").splitlines()
                if ln.strip() and ln.split()[0] != session]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(keep + [f"{session} {f}" for f in frames]) + "\n", encoding="utf-8", newline="\n")


def images_main(args: argparse.Namespace, ap: argparse.ArgumentParser) -> int:
    if args.min_frames < 0 or args.long_scene < 0:
        ap.error("--min-frames and --long-scene must not be negative")
    if args.margin_db < 0 or args.margin_block < 0:
        ap.error("--margin-db and --margin-block must not be negative")
    exempt: list[tuple[int, int]] = []
    for spec in args.not_judged:
        m = re.fullmatch(r"([0-9]+)-([0-9]+)", spec)
        if not (m and int(m.group(1)) <= int(m.group(2))):
            ap.error(f"--not-judged {spec!r}: expected FIRST-LAST frames")
        exempt.append((int(m.group(1)), int(m.group(2))))
    session = Path(args.session)
    try:
        report, proposed, problems = image_report(Path(args.reference), Path(args.candidate), session.stem,
                                                  args.min_frames, args.long_scene, not args.no_per_frame,
                                                  args.margin_db, args.margin_block, exempt)
        if args.proposal:
            write_proposal(Path(args.proposal), session.stem, proposed)
    except (ReportError, OSError) as error:
        print(f"cross_model_report: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(report)
    for problem in problems:
        print(f"cross_model_report: {problem}", file=sys.stderr)
    return 1 if problems else 0


def main(argv: Optional[Iterable[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--images", action="store_true",
                    help="compare the two runs' screenshots with imgdiff's rule per frame and screen and propose the "
                         "image frames (N2b-17); the other options of the report do not apply")
    ap.add_argument("--proposal", metavar="FILE",
                    help="with --images: write (replace) this session's lines `<session> <frame>` in FILE")
    ap.add_argument("--min-frames", type=int, default=IMAGE_MIN_FRAMES, metavar="N",
                    help=f"with --images: image frames a session needs (default {IMAGE_MIN_FRAMES})")
    ap.add_argument("--long-scene", type=int, default=IMAGE_LONG_SCENE, metavar="N",
                    help=f"with --images: a scene longer than N frames needs an image frame (default {IMAGE_LONG_SCENE})")
    ap.add_argument("--margin-db", type=float, default=IMAGE_MARGIN_DB, metavar="DB",
                    help=f"with --images: propose a frame only when both screens have a PSNR of at least "
                         f"{imgdiff.MIN_PSNR_DB:g} + DB (default {IMAGE_MARGIN_DB:g}, D58; 0 is the plain rule)")
    ap.add_argument("--margin-block", type=float, default=IMAGE_MARGIN_BLOCK, metavar="N",
                    help=f"with --images: ... and no 16x16 block above {imgdiff.MAX_BLOCK_ERROR:g} - N out of 255 "
                         f"(default {IMAGE_MARGIN_BLOCK:g}, D58; 0 is the plain rule)")
    ap.add_argument("--not-judged", action="append", default=[], metavar="FIRST-LAST",
                    help="with --images: a scene that starts in frames FIRST to LAST is not judged by the jit-calibrated "
                         "gate and needs no image frame (repeatable; D58: G4 903-1575, G2 5086-9000)")
    ap.add_argument("--no-per-frame", action="store_true", help="with --images: leave out the per-frame table")
    ap.add_argument("--session", required=True, metavar="FILE", help="the session script (tools/native/golden/<S>.txt)")
    ap.add_argument("--reference", required=True, metavar="DIR", help="the reference run (a golden recording)")
    ap.add_argument("--candidate", required=True, metavar="DIR", help="the other run (a check's run folder)")
    ap.add_argument("--observable", default=str(DEFAULT_OBSERVABLE), metavar="FILE",
                    help="the observable file; it must define `scene` (default config/native/observable.txt)")
    ap.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE, metavar="N",
                    help=f"frames a real scene change may move by (default {DEFAULT_TOLERANCE}, D50)")
    ap.add_argument("--min-margin", type=int, default=DEFAULT_MIN_MARGIN, metavar="N",
                    help=f"the smallest margin, in frames, of every input event after a real scene change on both "
                         f"sides (default {DEFAULT_MIN_MARGIN}, D50)")
    ap.add_argument("--no-svc", action="store_true", help="leave out the SVC counts (a large trace)")
    ap.add_argument("--no-ipc", action="store_true", help="leave out the IPC counts")
    args = ap.parse_args(list(argv) if argv is not None else None)
    if args.images:
        return images_main(args, ap)
    if args.tolerance < 0:
        ap.error("--tolerance must not be negative")
    if args.min_margin < 0:
        ap.error("--min-margin must not be negative")
    session = Path(args.session)
    try:
        try:
            observables = state_diff.parse_observable(Path(args.observable).read_text(encoding="utf-8"))
        except OSError as error:
            raise ReportError(f"cannot read {args.observable}: {error.strerror}") from None
        except state_diff.FormatError as error:
            raise ReportError(f"{args.observable}: {error}") from None
        if not any(o.name == state_diff.SCENE for o in observables):
            raise ReportError(f"{args.observable} defines no `{state_diff.SCENE}` observable")
        try:
            text = session.read_text(encoding="utf-8")
        except OSError as error:
            raise ReportError(f"cannot read {session}: {error.strerror}") from None
        report, problems = build_report(text, Path(args.reference), Path(args.candidate), session.stem,
                                        args.tolerance, not args.no_svc, not args.no_ipc, args.min_margin)
    except ReportError as error:
        print(f"cross_model_report: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(report)
    for problem in problems:
        print(f"cross_model_report: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
