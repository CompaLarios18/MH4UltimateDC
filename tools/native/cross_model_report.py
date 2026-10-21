#!/usr/bin/env python3
"""Report how two runs of one session on two timing models compare (N0-28, D31, D33).

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
                             [--no-svc] [--no-ipc]
exit status: 0 the report was written and every real scene change falls on the same scenes within
--tolerance frames (default 2, D33); 1 a real scene change is more than --tolerance frames apart, goes to
another scene, or one side has more of them (the report is still written: it is the table D33 asks for);
2 bad input (a missing file, a trace that cannot be read).
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

import state_diff  # noqa: E402
import tracefmt  # noqa: E402

DEFAULT_OBSERVABLE = HERE.parents[1] / "config" / "native" / "observable.txt"
DEFAULT_TOLERANCE = 2

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
                 tolerance: int = DEFAULT_TOLERANCE, svc: bool = True, ipc: bool = True) -> tuple[str, list[str]]:
    """(markdown, problems): `problems` are the real scene changes that are not within the tolerance."""
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
    out += ["", f"### Input events ({len(events)})", ""]
    out += event_table(events, ref_changes, cand_changes)
    out += [""] + margin_summary(events, ref_changes, cand_changes)
    if svc:
        ref_svc, cand_svc = count_svc(ref_dir / "svc.txt"), count_svc(cand_dir / "svc.txt")
        out += ["", "### SVC calls per thread (cross-model names)", ""] + svc_table(ref_svc, cand_svc)
    if ipc and (ref_dir / "ipc.txt").is_file() and (cand_dir / "ipc.txt").is_file():
        out += ["", "### IPC requests per command", ""] + ipc_table(count_ipc(ref_dir / "ipc.txt"),
                                                                     count_ipc(cand_dir / "ipc.txt"))
    return "\n".join(out) + "\n", problems


def main(argv: Optional[Iterable[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--session", required=True, metavar="FILE", help="the session script (tools/native/golden/<S>.txt)")
    ap.add_argument("--reference", required=True, metavar="DIR", help="the reference run (a golden recording)")
    ap.add_argument("--candidate", required=True, metavar="DIR", help="the other run (a check's run folder)")
    ap.add_argument("--observable", default=str(DEFAULT_OBSERVABLE), metavar="FILE",
                    help="the observable file; it must define `scene` (default config/native/observable.txt)")
    ap.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE, metavar="N",
                    help=f"frames a real scene change may move by (default {DEFAULT_TOLERANCE}, D33)")
    ap.add_argument("--no-svc", action="store_true", help="leave out the SVC counts (a large trace)")
    ap.add_argument("--no-ipc", action="store_true", help="leave out the IPC counts")
    args = ap.parse_args(list(argv) if argv is not None else None)
    if args.tolerance < 0:
        ap.error("--tolerance must not be negative")
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
                                        args.tolerance, not args.no_svc, not args.no_ipc)
    except ReportError as error:
        print(f"cross_model_report: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(report)
    for problem in problems:
        print(f"cross_model_report: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
