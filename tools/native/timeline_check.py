#!/usr/bin/env python3
"""The scheduler's timeline as a check (N0-39).

    python3 tools/native/timeline_check.py --reference timing.txt --candidate EVENTS [--allow FILE] [--show N]

REFERENCE is the `timing.txt` of a recording (the reference runner's `--trace-timing`: one
`tick=<ticks> event=<name> core=<core> late=<ticks> from=<who>` line per timer event, after a `#` header) and
CANDIDATE the file the native runner writes for `MH4N_TRACE_EVENTS` with `--deterministic` (one
`tick=<ticks> core=<core> late=<ticks>` line per timer event, and `stretch ...` and `ended ...` lines for the stretches
of the threads, which are not events and are skipped).  The events are paired in order and compared in tick, core and
lateness; the candidate's lines have no event name, so the name (the event's kind) is the reference's.  Both files
are streamed: a recording of G4 has 143,294 events and nothing here keeps more than a few of them.

An event that differs is a *difference*: `differs` (the paired events are not equal), `missing` (the reference has an
event the candidate has not) or `extra` (the candidate has an event the reference has not).  A difference is told
apart from a missing or an extra event by the next three events of both sides: if they pair up equal after the
difference as it stands, it is one event that differs; else if they pair up after skipping one reference event it is
`missing`, after skipping one candidate event it is `extra`; else it is `differs`.

Allow file (`--allow`): one allowed difference per line, `#` starts a comment and a reason after a `#` is required:

    ref=<tick>:<core>:<late>:<event> cand=<tick>:<core>:<late>   # reason
    ref=<tick>:<core>:<late>:<event> cand=-                      # a missing event
    ref=-                            cand=<tick>:<core>:<late>   # an extra event

A line allows exactly the difference it names (both events, with the reference's event name), once: a difference that
occurs twice needs two lines.  Allow lines that match no difference are listed and are not an error.

Exit status: 0 every difference is listed in the allow file (or there is none); 1 a difference that is not allowed;
2 a malformed input (a line of a file that is not an event, an unreadable file, an empty timeline, a bad allow file).
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path
from typing import Iterator, NamedTuple, Sequence

LOOKAHEAD = 3          # the events after a difference that decide whether it is one event or a missing/extra one
DEFAULT_SHOW = 20      # differences whose events are printed (all of them are counted)

EVENT = re.compile(r"tick=(\d+) (?:event=(\S+) )?core=(\d+) late=(-?\d+)(?: .*)?")
CANDIDATE_SKIPPED = ("stretch ", "ended ")


class TimelineError(Exception):
    """A malformed input (exit status 2)."""


class Event(NamedTuple):
    tick: int
    core: int
    late: int
    name: str          # "" when the file has no event names (the native runner's)
    line: int          # line number in its file

    @property
    def key(self) -> tuple[int, int, int]:
        return (self.tick, self.core, self.late)

    def text(self) -> str:
        return f"tick={self.tick} core={self.core} late={self.late}" + (f" event={self.name}" if self.name else "")


class Allowed(NamedTuple):
    ref: tuple[int, int, int, str] | None
    cand: tuple[int, int, int] | None


def read_events(path: Path, skip: Sequence[str] = ()) -> Iterator[Event]:
    """The events of a timeline file, one at a time.  Blank lines and `#` lines are skipped, and the lines that start
    with one of `skip`; any other line that is not an event raises TimelineError."""
    try:
        f = open(path, "r", encoding="utf-8", errors="replace", newline=None)
    except OSError as e:
        raise TimelineError(f"cannot read {path}: {e.strerror or e}") from None
    with f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(tuple(skip)):
                continue
            m = EVENT.fullmatch(line)
            if not m:
                raise TimelineError(f"{path}:{n}: not an event line (tick=<n> [event=<name>] core=<n> late=<n>): "
                                    f"{line[:80]!r}")
            yield Event(int(m.group(1)), int(m.group(3)), int(m.group(4)), m.group(2) or "", n)


class Stream:
    """An event iterator with a small look-ahead."""

    def __init__(self, events: Iterator[Event]) -> None:
        self._it = events
        self._buf: collections.deque[Event] = collections.deque()
        self.read = 0           # events taken from the file so far

    def peek(self, k: int = 0) -> Event | None:
        while len(self._buf) <= k:
            try:
                self._buf.append(next(self._it))
                self.read += 1
            except StopIteration:
                return None
        return self._buf[k]

    def pop(self) -> Event:
        e = self.peek()
        assert e is not None
        return self._buf.popleft()


class Difference(NamedTuple):
    kind: str                       # differs | missing | extra
    index: int                      # the paired position (0-based) where it happened
    ref: Event | None
    cand: Event | None
    before: tuple[Event | None, Event | None]       # the last events consumed on each side
    after: tuple[Event | None, Event | None]        # the next events of each side
    allowed: bool = False

    @property
    def allow_key(self) -> Allowed:
        return Allowed((self.ref.tick, self.ref.core, self.ref.late, self.ref.name) if self.ref else None,
                       self.cand.key if self.cand else None)


class Report(NamedTuple):
    reference_events: int
    candidate_events: int
    equal: int
    differences: int
    allowed: int
    shown: list[Difference]         # the first differences, with their events (bounded)
    unused_allow: list[Allowed]


def parse_allow(text: str, where: str = "allow file") -> list[Allowed]:
    entries: list[Allowed] = []
    for n, raw in enumerate(text.splitlines(), 1):
        body, hash_, reason = raw.partition("#")
        body = body.strip()
        if not body:
            continue
        if not reason.strip():
            raise TimelineError(f"{where}:{n}: a reason after a '#' is required")
        fields = body.split()
        if len(fields) != 2 or not fields[0].startswith("ref=") or not fields[1].startswith("cand="):
            raise TimelineError(f"{where}:{n}: expected 'ref=<tick>:<core>:<late>:<event> cand=<tick>:<core>:<late>'")
        ref_text, cand_text = fields[0][4:], fields[1][5:]
        ref: tuple[int, int, int, str] | None = None
        cand: tuple[int, int, int] | None = None
        if ref_text != "-":
            m = re.fullmatch(r"(\d+):(\d+):(-?\d+):(\S+)", ref_text)
            if not m:
                raise TimelineError(f"{where}:{n}: bad ref {ref_text!r} (<tick>:<core>:<late>:<event>, or -)")
            ref = (int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4))
        if cand_text != "-":
            m = re.fullmatch(r"(\d+):(\d+):(-?\d+)", cand_text)
            if not m:
                raise TimelineError(f"{where}:{n}: bad cand {cand_text!r} (<tick>:<core>:<late>, or -)")
            cand = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        if ref is None and cand is None:
            raise TimelineError(f"{where}:{n}: ref and cand are both '-'")
        entries.append(Allowed(ref, cand))
    return entries


def load_allow(path: Path | None) -> list[Allowed]:
    if path is None:
        return []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as e:
        raise TimelineError(f"cannot read {path}: {getattr(e, 'strerror', None) or e}") from None
    return parse_allow(text, str(path))


def _same(a: Event | None, b: Event | None) -> bool:
    return a is not None and b is not None and a.key == b.key


def _pairs_up(ref: Stream, ri: int, cand: Stream, ci: int) -> bool:
    """The next LOOKAHEAD events of both sides, from ref[ri] and cand[ci], are equal pairwise.  Fewer than LOOKAHEAD
    left on a side counts when all of the events it has pair up and both sides end together."""
    for k in range(LOOKAHEAD):
        r, c = ref.peek(ri + k), cand.peek(ci + k)
        if r is None and c is None:
            return True
        if not _same(r, c):
            return False
    return True


def compare(reference: Path, candidate: Path, allow: Sequence[Allowed] = (), show: int = DEFAULT_SHOW) -> Report:
    """Stream the two files and pair their events in order.  Raises TimelineError for a malformed input."""
    ref = Stream(read_events(reference))
    cand = Stream(read_events(candidate, CANDIDATE_SKIPPED))
    remaining = collections.Counter(allow)
    equal = differences = allowed = index = 0
    shown: list[Difference] = []
    last_ref: Event | None = None
    last_cand: Event | None = None

    while True:
        r, c = ref.peek(), cand.peek()
        if r is None and c is None:
            break
        if _same(r, c):
            last_ref, last_cand = ref.pop(), cand.pop()
            equal += 1
            index += 1
            continue
        if r is None:
            kind = "extra"
        elif c is None:
            kind = "missing"
        elif _pairs_up(ref, 1, cand, 1):
            kind = "differs"
        elif _pairs_up(ref, 1, cand, 0):
            kind = "missing"
        elif _pairs_up(ref, 0, cand, 1):
            kind = "extra"
        else:
            kind = "differs"
        before = (last_ref, last_cand)
        taken_ref = ref.pop() if kind in ("differs", "missing") else None
        taken_cand = cand.pop() if kind in ("differs", "extra") else None
        diff = Difference(kind, index, taken_ref, taken_cand, before, (ref.peek(), cand.peek()))
        differences += 1
        if remaining[diff.allow_key] > 0:
            remaining[diff.allow_key] -= 1
            allowed += 1
            diff = diff._replace(allowed=True)
        if len(shown) < show:
            shown.append(diff)
        last_ref = taken_ref or last_ref
        last_cand = taken_cand or last_cand
        index += 1

    if ref.read == 0:
        raise TimelineError(f"{reference}: no events")
    if cand.read == 0:
        raise TimelineError(f"{candidate}: no events (was MH4N_TRACE_EVENTS set, with --deterministic?)")
    unused = [a for a, n in remaining.items() for _ in range(n)]
    return Report(ref.read, cand.read, equal, differences, allowed, shown, unused)


def summary(report: Report) -> str:
    text = f"{report.equal} of {report.reference_events} equal"
    if report.candidate_events != report.reference_events:
        text += f" (the candidate has {report.candidate_events} events)"
    if report.differences:
        text += (f"; {report.differences} difference(s), {report.allowed} allowed, "
                 f"{report.differences - report.allowed} not allowed")
    return text


def _fmt(label: str, e: Event | None) -> str:
    return f"    {label} " + (f"line {e.line}: {e.text()}" if e else "(none)")


def format_report(report: Report) -> str:
    lines = [summary(report)]
    for n, d in enumerate(report.shown, 1):
        lines.append(f"difference {n} ({d.kind}, {'allowed' if d.allowed else 'NOT ALLOWED'}) at event {d.index}:")
        lines.append("  before:")
        lines += [_fmt("ref ", d.before[0]), _fmt("cand", d.before[1])]
        lines.append("  here:")
        lines += [_fmt("ref ", d.ref), _fmt("cand", d.cand)]
        lines.append("  after:")
        lines += [_fmt("ref ", d.after[0]), _fmt("cand", d.after[1])]
    hidden = report.differences - len(report.shown)
    if hidden > 0:
        lines.append(f"... and {hidden} more difference(s) not printed")
    for a in report.unused_allow:
        ref = ":".join(map(str, a.ref)) if a.ref else "-"
        cand = ":".join(map(str, a.cand)) if a.cand else "-"
        lines.append(f"note: allow line ref={ref} cand={cand} matched no difference")
    return "\n".join(lines)


def exit_code(report: Report) -> int:
    return 0 if report.differences == report.allowed else 1


def main(argv: Sequence[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Compare the reference's timing.txt with the native runner's "
                                            "MH4N_TRACE_EVENTS file, event by event.")
    p.add_argument("--reference", required=True, type=Path, help="the recording's timing.txt")
    p.add_argument("--candidate", required=True, type=Path, help="the MH4N_TRACE_EVENTS file of the candidate")
    p.add_argument("--allow", type=Path, help="the file of allowed differences (see the module's text)")
    p.add_argument("--show", type=int, default=DEFAULT_SHOW, metavar="N",
                   help=f"differences to print with their neighbours (default {DEFAULT_SHOW})")
    a = p.parse_args(argv)
    try:
        report = compare(a.reference, a.candidate, load_allow(a.allow), a.show)
    except TimelineError as e:
        print(f"timeline_check.py: {e}", file=sys.stderr)
        return 2
    print(format_report(report))
    return exit_code(report)


if __name__ == "__main__":
    sys.exit(main())
