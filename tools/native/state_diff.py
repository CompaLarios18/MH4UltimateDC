#!/usr/bin/env python3
"""Compare two game-state traces (mh4u_runner --state-trace, and the native runtime's).

A state trace records, at every game frame, the values listed in config/native/observable.txt
(N0-07, docs/native/wp/N0.md):
    # statetrace v1 <ISO date> game=0x<title id> cpu=<jit|recomp|interp|native> new3ds=<on|off>
    f=<frame> t=<ticks> <name>=<0x hex | -> ...
A line is written whenever the game's frame counter changes. `-` means the value could not be
read (a null pointer in its chain, or unmapped memory). Other lines starting with `#` are
metadata.

Two runtimes never agree on the exact frame of an event, only roughly, so the comparison aligns
the traces on the scene byte (the observable named `scene`): the k-th scene change of one is
matched with the k-th of the other. The traces agree when every scene change
  - leads to the same scene,
  - happens within --tolerance frames (default 2) of its counterpart (not checked with
    --order-only, for runs at another frame rate), and
  - finds equal values for every `eq` observable in the two records of the change
    (`frame_counter` is not compared there: the tolerance covers it; --order-only compares
    nothing but the sequence of scenes).
`ignore` observables are recorded but never compared.

The observable file also defines the expression syntax; `evaluate` is what the runners'
C++ reader implements.

usage: state_diff.py REFERENCE CANDIDATE [--tolerance N] [--observable FILE] [--order-only]
exit status: 0 the traces agree, 1 they diverge (the first divergence is printed), 2 bad input.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

DEFAULT_OBSERVABLE = Path(__file__).resolve().parents[2] / "config" / "native" / "observable.txt"
SCENE = "scene"
FRAME_COUNTER = "frame_counter"
SIZES = (1, 2, 4)

Reader = Callable[[int, int], Optional[int]]  # (address, size) -> value, None if unreadable


class FormatError(ValueError):
    """A trace or observable file that cannot be parsed."""


@dataclass(frozen=True)
class Observable:
    expr: str
    size: int
    name: str
    compare: bool  # True: eq, False: ignore


# ── observable.txt ─────────────────────────────────────────────────────────────────

_HEX = re.compile(r"0x[0-9a-fA-F]+")


def _split_top(text: str) -> list[str]:
    """Split on '+' outside brackets."""
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth < 0:
                raise FormatError(f"unbalanced ']' in {text!r}")
        if ch == "+" and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if depth != 0:
        raise FormatError(f"unbalanced '[' in {text!r}")
    parts.append(cur)
    return parts


def _wrapped(text: str) -> bool:
    """True if the first '[' closes at the last character."""
    if not (text.startswith("[") and text.endswith("]")):
        return False
    depth = 0
    for i, ch in enumerate(text):
        depth += ch == "["
        depth -= ch == "]"
        if depth == 0:
            return i == len(text) - 1
    return False


def _check_addr(text: str) -> None:
    for term in _split_top(text):
        if _wrapped(term):
            _check_addr(term[1:-1])
        elif not _HEX.fullmatch(term):
            raise FormatError(f"bad term {term!r} in expression {text!r}")


def check_expr(expr: str) -> None:
    """Raise FormatError unless `expr` is an address or a pointer chain."""
    _check_addr(expr[1:-1] if _wrapped(expr) else expr)


def _address(text: str, read: Reader) -> Optional[int]:
    total = 0
    for term in _split_top(text):
        if _wrapped(term):
            inner = _address(term[1:-1], read)
            value = None if inner is None else read(inner, 4)
            if not value:  # unreadable or a null pointer
                return None
            total += value
        else:
            total += int(term, 16)
    return total & 0xFFFFFFFF


def evaluate(expr: str, size: int, read: Reader) -> Optional[int]:
    """Value of an observable, or None (`-` in a trace) if a pointer in its chain is null or
    the memory cannot be read. `[[A]+O]` reads the u32 at A, adds O and reads `size` bytes."""
    address = _address(expr[1:-1] if _wrapped(expr) else expr, read)
    return None if address is None else read(address, size)


def parse_observable(text: str) -> list[Observable]:
    result: list[Observable] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 4:
            raise FormatError(f"observable line {number}: want '<expr> <size> <name> <eq|ignore>'")
        expr, size, name, mode = fields
        check_expr(expr)
        if not size.isdigit() or int(size) not in SIZES:
            raise FormatError(f"observable line {number}: size must be 1, 2 or 4")
        if mode not in ("eq", "ignore"):
            raise FormatError(f"observable line {number}: last field must be eq or ignore")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or any(o.name == name for o in result):
            raise FormatError(f"observable line {number}: bad or repeated name {name!r}")
        result.append(Observable(expr, int(size), name, mode == "eq"))
    return result


# ── traces ─────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Record:
    frame: int
    ticks: int
    values: dict[str, Optional[int]]
    line: int


def parse_trace(text: str) -> list[Record]:
    records: list[Record] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        values: dict[str, Optional[int]] = {}
        frame = ticks = None
        for field in line.split():
            key, sep, value = field.partition("=")
            if not sep:
                raise FormatError(f"line {number}: {field!r} is not key=value")
            try:
                if key == "f":
                    frame = int(value, 0)
                elif key == "t":
                    ticks = int(value, 0)
                else:
                    values[key] = None if value == "-" else int(value, 0)
            except ValueError:
                raise FormatError(f"line {number}: bad value in {field!r}") from None
        if frame is None or ticks is None:
            raise FormatError(f"line {number}: a record needs f= and t=")
        records.append(Record(frame, ticks, values, number))
    return records


def scene_changes(records: list[Record]) -> list[Record]:
    """The first record, and every record whose scene differs from the one before."""
    changes: list[Record] = []
    previous: object = object()
    for record in records:
        if SCENE not in record.values:
            raise FormatError(f"line {record.line}: no {SCENE}= value")
        if record.values[SCENE] != previous:
            changes.append(record)
            previous = record.values[SCENE]
    return changes


def _fmt(value: Optional[int]) -> str:
    return "-" if value is None else f"0x{value:x}"


def diff(reference: list[Record], candidate: list[Record], observables: list[Observable],
         tolerance: int = 2, order_only: bool = False) -> Optional[str]:
    """None if the traces agree, otherwise a description of the first divergence."""
    ref, cand = scene_changes(reference), scene_changes(candidate)
    compared = [o for o in observables if o.compare and o.name not in (SCENE, FRAME_COUNTER)]
    if not order_only:
        for name in (o.name for o in compared):
            for label, records in (("reference", reference), ("candidate", candidate)):
                if records and name not in records[0].values:
                    raise FormatError(f"{label} trace has no {name}= value (observable file mismatch)")
    for k in range(max(len(ref), len(cand))):
        if k >= len(ref):
            return (f"scene change #{k}: the candidate goes to scene {_fmt(cand[k].values[SCENE])} "
                    f"at frame {cand[k].frame}, the reference has no further change")
        if k >= len(cand):
            return (f"scene change #{k}: the reference goes to scene {_fmt(ref[k].values[SCENE])} "
                    f"at frame {ref[k].frame}, the candidate has no further change")
        r, c = ref[k], cand[k]
        if r.values[SCENE] != c.values[SCENE]:
            return (f"scene change #{k}: the reference goes to scene {_fmt(r.values[SCENE])} "
                    f"(frame {r.frame}), the candidate to scene {_fmt(c.values[SCENE])} (frame {c.frame})")
        if order_only:
            continue
        if abs(r.frame - c.frame) > tolerance:
            return (f"scene change #{k} (to scene {_fmt(r.values[SCENE])}): reference frame {r.frame}, "
                    f"candidate frame {c.frame}, more than {tolerance} apart")
        for o in compared:
            if r.values[o.name] != c.values[o.name]:
                return (f"scene change #{k} (to scene {_fmt(r.values[SCENE])}, frames {r.frame}/{c.frame}): "
                        f"{o.name} is {_fmt(r.values[o.name])} in the reference, "
                        f"{_fmt(c.values[o.name])} in the candidate")
    return None


def _read(path: str) -> str:
    try:
        return Path(path).read_text()
    except OSError as error:
        raise FormatError(f"cannot read {path}: {error.strerror}") from None


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Compare two game-state traces.")
    parser.add_argument("reference")
    parser.add_argument("candidate")
    parser.add_argument("--tolerance", type=int, default=2, metavar="N",
                        help="frames a scene change may move by (default 2)")
    parser.add_argument("--observable", default=str(DEFAULT_OBSERVABLE), metavar="FILE")
    parser.add_argument("--order-only", action="store_true",
                        help="compare only the sequence of scenes, not the frames or other values")
    args = parser.parse_args(argv)
    if args.tolerance < 0:
        parser.error("--tolerance must not be negative")
    try:
        observables = parse_observable(_read(args.observable))
        reference = parse_trace(_read(args.reference))
        candidate = parse_trace(_read(args.candidate))
        if not reference or not candidate:
            raise FormatError("a trace has no records")
        problem = diff(reference, candidate, observables, args.tolerance, args.order_only)
    except FormatError as error:
        print(f"state_diff: {error}", file=sys.stderr)
        return 2
    if problem:
        print(f"DIVERGENCE: {problem}")
        return 1
    print(f"traces agree: {len(scene_changes(reference))} scene changes"
          + ("" if args.order_only else f", within {args.tolerance} frames"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
