#!/usr/bin/env python3
"""Compare two system-call traces of the same program (mh4u_runner --trace-svc), thread by
thread, and show where they first differ.

Threads interleave differently on two CPU backends, but each thread should make the same
calls, from the same places, with the same arguments. The first call that differs within a
thread points at the code that ran between it and the previous (matching) call.

usage: compare_svc_traces.py reference.txt candidate.txt [--context 3] [--ignore-args]
                             [--normalize] [--outputs] [--in4] [--prefix] [--ignore-regs FILE]...
                             [--collapse-polling FILE | --cross-model FILE]
Trace lines: "<thread> <svc> pc=<hex> lr=<hex> <r0> <r1> <r2> <r3> t=<ticks>", and since N0-04
"in4=<r4>,<r5>,<r6> out=<r0>,<r1>,<r2>,<r3>" after that, `# thread` lines and a header
(tools/native/tracefmt.py describes the format). Traces in the old format still compare.
  --normalize  rename threads (to their entry point and creation order) and handles (to their
               kind and creation order) and drop times first, so that two runtimes whose ids
               and handle values differ can be compared
  --outputs    also compare the results (out=): r0 always, r1-r3 except handles (normalised
               with --normalize) and time; a call with no out= in either trace isn't compared
  --in4        also compare r4-r6 as the call was made (timeouts and the like)
  --ignore-regs FILE  registers a call does not define are not compared (repeatable). Each line is
               `<svc> [in=r1,r2,r3] [out=r1,r2,r3]  # reason`: the svc number in hex (`0x14`),
               the registers (r0-r3) to leave out of the arguments and of the results. Only for
               registers the system call neither reads nor writes: they hold whatever the guest
               code left there (svc_test leaves a timing-dependent partial sum in r1 at
               ReleaseMutex), which two runtimes with different timing do not share.
               config/native/svc_unused_registers.txt lists them.
  --collapse-polling FILE  (N0-25, D30's fallback for two runs of one backend; use it with --normalize)
               FILE lists the calls whose result depends only on the clock and that change nothing
               (`<svc hex>  # reason`, config/native/svc_polling.txt). In each thread a maximal run of
               consecutive calls of one of them becomes one record that matches any run of the same
               call of length 1 or more that starts at the same place (pc and lr of its first call)
               and whose places (pc and lr of its calls) include those of the shorter one: a longer
               run is a shorter one with more iterations of the loop (the lengths are printed, not
               compared; runs of one length must have the same places). A collapsed record compares
               the svc number, pc and lr of its calls and nothing else: not their registers, not t=,
               so what the call before it left in them is not a difference. A run on one side with
               no run at that place on the other is a difference. Every other call compares as
               without the flag, in order.
  --cross-model FILE  (N0-25, D31; implies --normalize) for two timing models (jit against recomp, the
               reference against the native runtime). FILE lists the calls compared and, for each, the
               registers (`<svc hex> in=<regs> out=<regs>  # reason`, config/native/svc_cross_model.txt);
               each thread's calls are filtered to the listed ones and compared in order. svc, pc, lr and
               the result code (out r0) are always compared, the other outputs with --outputs; handles by
               their normalised name. Threads are named `main` and `<parent>/<entry>#<k>` (k counts the
               threads that parent created with that entry), handles `<kind>#<thread>.<k>`. Prints, per
               thread, the number of compared calls and each side's total number of calls.
Exit status 0 when every thread matches (up to the shorter trace if --prefix is given), 1 when a thread
differs, 2 when an option file or a trace cannot be read or holds no call line, 3 when nothing was compared
(D34: after the --cross-model filter neither trace has a call to compare; a difference wins over 3).
"""
from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass, replace
from typing import Callable

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "native"))
import tracefmt  # noqa: E402

Call = tracefmt.Call

MAX_LISTED = 20          # differing records listed for a thread when the traces have the same length


class RegisterRuleError(ValueError):
    pass


def _svc_number(text: str, where: str) -> int:
    try:
        number = int(text, 16)
    except ValueError:
        raise RegisterRuleError(f"{where}: the svc number {text!r} is not hex") from None
    if not 0 <= number <= 0xFF:
        raise RegisterRuleError(f"{where}: the svc number {text!r} is not in 0x00-0xFF")
    return number


def _svc_key(svc: str) -> str:
    """A trace's svc number (`0A`, `28`) or a list's (`0xa`) as two upper-case hex digits."""
    try:
        return f"{int(svc, 16):02X}"
    except ValueError:
        return svc.upper()


def load_register_rules(paths: list[str]) -> dict[str, dict[str, set[int]]]:
    """svc number (upper-case hex, no prefix, as the trace writes it) -> {"in": registers, "out": registers}."""
    rules: dict[str, dict[str, set[int]]] = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                text = line.split("#", 1)[0].split()
                if not text:
                    continue
                number = _svc_number(text[0], f"{path}:{n}")
                svc = format(number, "X")
                entry = rules.setdefault(svc, {"in": set(), "out": set()})
                if len(text) < 2:
                    raise RegisterRuleError(f"{path}:{n}: expected `<svc> [in=r1,..] [out=r1,..]`")
                for part in text[1:]:
                    kind, _, regs = part.partition("=")
                    if kind not in ("in", "out") or not regs:
                        raise RegisterRuleError(f"{path}:{n}: expected in=... or out=..., got {part!r}")
                    for reg in regs.split(","):
                        if reg not in ("r0", "r1", "r2", "r3"):
                            raise RegisterRuleError(f"{path}:{n}: {reg!r} is not one of r0-r3")
                        entry[kind].add(int(reg[1]))
    return rules


def load_polling(path: str) -> set[str]:
    """The calls --collapse-polling treats as polling: `<svc hex>  # reason` lines, as two-digit svc numbers."""
    calls: set[str] = set()
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            text = line.split("#", 1)[0].split()
            if not text:
                continue
            if len(text) != 1:
                raise RegisterRuleError(f"{path}:{n}: expected `<svc hex>  # reason`, got {' '.join(text)!r}")
            key = f"{_svc_number(text[0], f'{path}:{n}'):02X}"
            if key in calls:
                raise RegisterRuleError(f"{path}:{n}: svc 0x{key} is listed twice")
            calls.add(key)
    if not calls:
        raise RegisterRuleError(f"{path}: lists no call")
    return calls


@dataclass(frozen=True)
class CrossRule:
    inputs: frozenset[int]       # registers r0-r6 compared as the call was made (r4-r6 are the trace's in4=)
    outputs: frozenset[int]      # registers r0-r3 compared as the call returned (with --outputs)


def load_cross_model_rules(path: str) -> dict[str, CrossRule]:
    """svc number (two upper-case hex digits) -> CrossRule, from `<svc hex> [in=<regs>] [out=<regs>]  # reason`
    lines. An empty file is not an error here: the comparison then has nothing to compare (exit status 3)."""
    rules: dict[str, CrossRule] = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            text = line.split("#", 1)[0].split()
            if not text:
                continue
            where = f"{path}:{n}"
            key = f"{_svc_number(text[0], where):02X}"
            if key in rules:
                raise RegisterRuleError(f"{where}: svc 0x{key} is listed twice")
            lists: dict[str, frozenset[int]] = {}
            for part in text[1:]:
                kind, _, regs = part.partition("=")
                if kind not in ("in", "out") or not regs:
                    raise RegisterRuleError(f"{where}: expected in=... or out=..., got {part!r}")
                if kind in lists:
                    raise RegisterRuleError(f"{where}: {kind}= is given twice")
                last = 6 if kind == "in" else 3
                numbers = set()
                for reg in regs.split(","):
                    if len(reg) != 2 or reg[0] != "r" or not reg[1].isdigit() or int(reg[1]) > last:
                        raise RegisterRuleError(f"{where}: {reg!r} is not one of r0-r{last}")
                    numbers.add(int(reg[1]))
                lists[kind] = frozenset(numbers)
            rules[key] = CrossRule(lists.get("in", frozenset()), lists.get("out", frozenset()))
    return rules


def mask_registers(calls: list[Call], rules: dict[str, dict[str, set[int]]]) -> list[Call]:
    """The calls with the registers the rules name replaced by `*`. Both traces go through the same rules, so
    the masked registers are equal on both sides and carry no value into the comparison."""
    out = []
    for call in calls:
        rule = rules.get(call.svc.upper().lstrip("0") or "0")
        if rule is None:
            out.append(call)
            continue
        args = tuple("*" if i in rule["in"] else a for i, a in enumerate(call.args))
        res = call.out if call.out is None else tuple("*" if i in rule["out"] else a for i, a in enumerate(call.out))
        out.append(replace(call, args=args, out=res))
    return out


# ── polling runs (--collapse-polling) ─────────────────────────────────────────────────

@dataclass(frozen=True)
class Run:
    """A maximal run of consecutive calls of one polling svc in one thread. Two runs match when they are of the
    same svc, start at the same place (pc and lr of the first call) and the places of the shorter run are all
    places of the longer one (of the same length: the same places): a longer run is a shorter one with more
    iterations of the polling loop, which may pass places the shorter one never reached. Their lengths, registers
    and times are not compared."""
    svc: str
    sites: frozenset[tuple[str, str]]
    length: int
    line: int                      # the file line of the first call
    first: tuple[str, str] = ("", "")
    pc: str = "*"
    lr: str = "*"

    def start_matches(self, other: "Run") -> bool:
        return self.svc == other.svc and self.first == other.first

    def places_match(self, other: "Run") -> bool:
        if self.length == other.length:
            return self.sites == other.sites
        shorter, longer = (self, other) if self.length < other.length else (other, self)
        return shorter.sites <= longer.sites

    def matches(self, other: object, with_args: bool = True, with_outputs: bool = False,
                with_in4: bool = False) -> bool:
        return isinstance(other, Run) and self.start_matches(other) and self.places_match(other)

    def __str__(self) -> str:
        n = len(self.sites)
        return f"svc {self.svc} run of {self.length} polling calls from {n} place{'' if n == 1 else 's'} (line {self.line})"


Record = Call | Run


def collapse_polling(calls: list[Call], polling: set[str]) -> list[Record]:
    """The thread's calls with each maximal run of one polling svc replaced by a Run."""
    out: list[Record] = []
    i = 0
    while i < len(calls):
        call = calls[i]
        if _svc_key(call.svc) not in polling:
            out.append(call)
            i += 1
            continue
        j = i
        while j < len(calls) and calls[j].svc == call.svc:
            j += 1
        out.append(Run(call.svc, frozenset((c.pc, c.lr) for c in calls[i:j]), j - i, call.line, (call.pc, call.lr)))
        i = j
    return out


def call_count(records: list[Record]) -> int:
    return sum(r.length if isinstance(r, Run) else 1 for r in records)


def parse(path: str, normalize: bool = False, rules: dict[str, dict[str, set[int]]] | None = None,
          cross_model: bool = False, polling: set[str] | None = None) -> dict[str, list]:
    trace = tracefmt.parse(path)
    if normalize:
        trace = tracefmt.normalise(trace, cross_model=cross_model)
    by_thread = trace.by_thread()
    if rules:
        by_thread = {tid: mask_registers(calls, rules) for tid, calls in by_thread.items()}
    if polling:
        by_thread = {tid: collapse_polling(calls, polling) for tid, calls in by_thread.items()}
    return by_thread


def _thread_order(tid: str) -> tuple:
    plain = tid.isascii() and tid.isdigit() and len(tid) <= 18
    return (0, int(tid), "") if plain else (1, 0, tid)


def compare(ref: dict[str, list], cand: dict[str, list], with_args: bool = True,
            prefix: bool = False, with_outputs: bool = False, with_in4: bool = False,
            match: Callable[[object, object], bool] | None = None) -> list[dict]:
    """One report per thread that differs: thread, index of the first difference, the two
    calls (None past the end of a trace). `match` replaces the comparison of two records."""
    if match is None:
        def match(x, y):
            return x.matches(y, with_args, with_outputs, with_in4)
    out = []
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        a, b = ref.get(tid, []), cand.get(tid, [])
        n = min(len(a), len(b))
        first = next((i for i in range(n) if not match(a[i], b[i])), None)
        if first is None and (len(a) == len(b) or prefix):
            continue
        if first is None:
            first = n
        out.append({"thread": tid, "index": first, "ref": a[first] if first < len(a) else None,
                    "cand": b[first] if first < len(b) else None, "ref_len": len(a), "cand_len": len(b)})
    return out


def differing_fields(x: object, y: object, with_args: bool = True, with_outputs: bool = False,
                     with_in4: bool = False) -> list[str]:
    """The names of the fields that make two records differ (Call.matches, field by field): `svc`, `pc`, `lr`,
    `r0`-`r3`, `in4.r4`-`in4.r6`, `out.r0`-`out.r3`; `run` for a run against a call, `start` and `places` for two runs."""
    if isinstance(x, Run) or isinstance(y, Run):
        if not (isinstance(x, Run) and isinstance(y, Run)):
            return ["run"]
        return [f for f, bad in (("svc", x.svc != y.svc), ("start", x.first != y.first),
                                  ("places", not x.places_match(y))) if bad]
    fields = [name for name, bad in (("svc", x.svc != y.svc), ("pc", x.pc != y.pc), ("lr", x.lr != y.lr)) if bad]
    if with_args:
        fields += [f"r{i}" for i, (p, q) in enumerate(zip(x.args, y.args)) if p != q]
    if with_in4 and None not in (x.in4, y.in4):
        fields += [f"in4.r{4 + i}" for i, (p, q) in enumerate(zip(x.in4, y.in4)) if p != q]
    if with_outputs and None not in (x.out, y.out):
        fields += [f"out.r{i}" for i, (p, q) in enumerate(zip(x.out, y.out)) if p != q]
    return fields


def cross_differing_fields(x: Call, y: Call, rules: dict[str, CrossRule], with_outputs: bool = False) -> list[str]:
    """The fields of two filtered calls that --cross-model compares and in which they differ."""
    fields = [name for name, bad in (("svc", _svc_key(x.svc) != _svc_key(y.svc)), ("pc", x.pc != y.pc),
                                     ("lr", x.lr != y.lr)) if bad]
    rule = rules.get(_svc_key(x.svc))
    if rule is None:
        return fields
    for i in sorted(rule.inputs):
        if i < 4:
            if x.args[i] != y.args[i]:
                fields.append(f"r{i}")
        elif None not in (x.in4, y.in4) and i - 4 < min(len(x.in4), len(y.in4)) and x.in4[i - 4] != y.in4[i - 4]:
            fields.append(f"in4.r{i}")
    if None not in (x.out, y.out):
        if x.out[0] != y.out[0]:
            fields.append("out.r0")
        if with_outputs:
            fields += [f"out.r{i}" for i in sorted(rule.outputs - {0}) if x.out[i] != y.out[i]]
    return fields


class TraceError(Exception):
    pass


def load(path: str, normalize: bool, rules: dict[str, dict[str, set[int]]] | None,
         cross_model: bool = False, polling: set[str] | None = None) -> dict[str, list]:
    """parse(), but a file that cannot be read, is not UTF-8 text or holds no call line is a TraceError."""
    try:
        by_thread = parse(path, normalize, rules, cross_model, polling)
    except OSError as e:
        raise TraceError(f"cannot read {path}: {e.strerror or e}") from None
    except UnicodeError:
        raise TraceError(f"{path}: not a system-call trace (not UTF-8 text)") from None
    if not any(by_thread.values()):
        raise TraceError(f"{path}: the trace holds no system call")
    return by_thread


def _polling_summary(a: list[Record], b: list[Record]) -> str:
    def stats(records: list[Record]) -> tuple[int, int]:
        runs = [r.length for r in records if isinstance(r, Run)]
        return len(runs), max(runs, default=0)
    (na, la), (nb, lb) = stats(a), stats(b)
    return (f"; {len(a)} vs {len(b)} records after collapsing, polling runs {na} vs {nb}"
            f" (longest {la} vs {lb} calls)")


def _print_difference(d: dict, ref: dict[str, list], cand: dict[str, list], context: int,
                      fields: Callable[[object, object], list[str]] | None) -> None:
    tid = d["thread"]
    i, a = d["index"], ref.get(tid, [])
    print(f"\nthread {tid} differs at call {i}:")
    for c in a[max(0, i - context):i]:
        print(f"    same  {c}")
    print(f"    ref   {d['ref'] or '(trace ends)'}")
    print(f"    cand  {d['cand'] or '(trace ends)'}")
    if fields is None:
        return
    b = cand.get(tid, [])
    if len(a) != len(b):
        return                  # every record after the first difference may be shifted: no list
    differing = [(k, fields(a[k], b[k])) for k in range(len(a))]
    differing = [(k, f) for k, f in differing if f]
    print(f"    {len(differing)} of {len(a)} records differ at the same position:")
    for k, f in differing[:MAX_LISTED]:
        print(f"      call {k}: svc {_svc_key(a[k].svc)} differs in {', '.join(f)}")
    if len(differing) > MAX_LISTED:
        print(f"      ... and {len(differing) - MAX_LISTED} more")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--context", type=int, default=3, help="matching calls to show before a difference")
    ap.add_argument("--ignore-args", action="store_true", help="compare only svc, pc and lr")
    ap.add_argument("--prefix", action="store_true", help="a thread may stop earlier in one trace")
    ap.add_argument("--normalize", action="store_true", help="rename threads and handles, drop times")
    ap.add_argument("--outputs", action="store_true", help="also compare the results (out=)")
    ap.add_argument("--in4", action="store_true", help="also compare r4-r6 as the call was made")
    ap.add_argument("--ignore-regs", action="append", default=[], metavar="FILE",
                    help="registers a call does not define (`<svc> in=r1,r2 out=r1`); repeatable")
    ap.add_argument("--collapse-polling", metavar="FILE",
                    help="a run of calls listed in FILE (`<svc hex>  # reason`) is one record, whatever its length")
    ap.add_argument("--cross-model", metavar="FILE",
                    help="compare only the calls listed in FILE (`<svc hex> in=<regs> out=<regs>  # reason`), "
                         "for two timing models (implies --normalize)")
    args = ap.parse_args(argv)
    if args.cross_model and (args.collapse_polling or args.in4 or args.ignore_args):
        print("error: --cross-model takes the registers to compare from its file: it cannot be combined with "
              "--collapse-polling, --in4 or --ignore-args", file=sys.stderr)
        return 2
    if args.cross_model:
        args.normalize = True
    try:
        rules = load_register_rules(args.ignore_regs)
        polling = load_polling(args.collapse_polling) if args.collapse_polling else None
        cross = load_cross_model_rules(args.cross_model) if args.cross_model else None
    except (OSError, UnicodeError, RegisterRuleError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    try:
        ref = load(args.reference, args.normalize, rules, cross is not None, polling)
        cand = load(args.candidate, args.normalize, rules, cross is not None, polling)
    except TraceError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    threads = sorted(set(ref) | set(cand), key=_thread_order)
    fields: Callable[[object, object], list[str]] | None = None
    match: Callable[[object, object], bool] | None = None
    compared = None
    if cross is not None:
        full_ref, full_cand = ref, cand
        ref = {t: [c for c in calls if _svc_key(c.svc) in cross] for t, calls in full_ref.items()}
        cand = {t: [c for c in calls if _svc_key(c.svc) in cross] for t, calls in full_cand.items()}

        def fields(x, y):
            return cross_differing_fields(x, y, cross, args.outputs)

        def match(x, y):
            return not cross_differing_fields(x, y, cross, args.outputs)
        compared = sum(len(v) for v in ref.values()) + sum(len(v) for v in cand.values())
        for tid in threads:
            print(f"thread {tid}: compared calls {len(ref.get(tid, []))} vs {len(cand.get(tid, []))}"
                  f" (all calls {len(full_ref.get(tid, []))} vs {len(full_cand.get(tid, []))})")
    else:
        if polling:         # without the new flags the report is the one it always was
            def fields(x, y):
                return differing_fields(x, y, not args.ignore_args, args.outputs, args.in4)
        for tid in threads:
            a, b = ref.get(tid, []), cand.get(tid, [])
            text = f"thread {tid}: {call_count(a)} vs {call_count(b)} calls"
            print(text + (_polling_summary(a, b) if polling else ""))
    diffs = compare(ref, cand, not args.ignore_args, args.prefix, args.outputs, args.in4, match)
    for d in diffs:
        _print_difference(d, ref, cand, args.context, fields)
    if diffs:
        print(f"\n{len(diffs)} thread(s) differ")
        return 1
    if compared == 0:
        print("\nNOTHING COMPARED: after the filter neither trace has a call to compare "
              f"({args.cross_model} lists no call that either makes)")
        return 3
    print("\nall threads match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
