#!/usr/bin/env python3
"""Compare two system-call traces of the same program (mh4u_runner --trace-svc), thread by
thread, and show where they first differ.

Threads interleave differently on two CPU backends, but each thread should make the same
calls, from the same places, with the same arguments. The first call that differs within a
thread points at the code that ran between it and the previous (matching) call.

usage: compare_svc_traces.py reference.txt candidate.txt [--context 3] [--ignore-args]
                             [--normalize] [--outputs] [--in4] [--prefix] [--ignore-regs FILE]...
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
Exit status 0 when every thread matches (up to the shorter trace if --prefix is given).
"""
from __future__ import annotations

import argparse
import os
import sys
from dataclasses import replace

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "native"))
import tracefmt  # noqa: E402

Call = tracefmt.Call


class RegisterRuleError(ValueError):
    pass


def load_register_rules(paths: list[str]) -> dict[str, dict[str, set[int]]]:
    """svc number (upper-case hex, no prefix, as the trace writes it) -> {"in": registers, "out": registers}."""
    rules: dict[str, dict[str, set[int]]] = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                text = line.split("#", 1)[0].split()
                if not text:
                    continue
                try:
                    svc = format(int(text[0], 16), "X")
                except ValueError:
                    raise RegisterRuleError(f"{path}:{n}: the svc number {text[0]!r} is not hex") from None
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


def mask_registers(calls: list[Call], rules: dict[str, dict[str, set[int]]]) -> list[Call]:
    """The calls with the registers the rules name replaced by `*` (equal to anything but another `*`)."""
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


def parse(path: str, normalize: bool = False,
          rules: dict[str, dict[str, set[int]]] | None = None) -> dict[str, list[Call]]:
    trace = tracefmt.parse(path)
    if normalize:
        trace = tracefmt.normalise(trace)
    by_thread = trace.by_thread()
    if rules:
        by_thread = {tid: mask_registers(calls, rules) for tid, calls in by_thread.items()}
    return by_thread


def _thread_order(tid: str) -> tuple:
    plain = tid.isascii() and tid.isdigit() and len(tid) <= 18
    return (0, int(tid), "") if plain else (1, 0, tid)


def compare(ref: dict[str, list[Call]], cand: dict[str, list[Call]], with_args: bool = True,
            prefix: bool = False, with_outputs: bool = False, with_in4: bool = False) -> list[dict]:
    """One report per thread that differs: thread, index of the first difference, the two
    calls (None past the end of a trace)."""
    out = []
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        a, b = ref.get(tid, []), cand.get(tid, [])
        n = min(len(a), len(b))
        first = next((i for i in range(n) if not a[i].matches(b[i], with_args, with_outputs, with_in4)), None)
        if first is None and (len(a) == len(b) or prefix):
            continue
        if first is None:
            first = n
        out.append({"thread": tid, "index": first, "ref": a[first] if first < len(a) else None,
                    "cand": b[first] if first < len(b) else None, "ref_len": len(a), "cand_len": len(b)})
    return out


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
    args = ap.parse_args(argv)
    try:
        rules = load_register_rules(args.ignore_regs)
    except (OSError, UnicodeError, RegisterRuleError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    ref, cand = parse(args.reference, args.normalize, rules), parse(args.candidate, args.normalize, rules)
    diffs = compare(ref, cand, not args.ignore_args, args.prefix, args.outputs, args.in4)
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        print(f"thread {tid}: {len(ref.get(tid, []))} vs {len(cand.get(tid, []))} calls")
    for d in diffs:
        i, a = d["index"], ref.get(d["thread"], [])
        print(f"\nthread {d['thread']} differs at call {i}:")
        for c in a[max(0, i - args.context):i]:
            print(f"    same  {c}")
        print(f"    ref   {d['ref'] or '(trace ends)'}")
        print(f"    cand  {d['cand'] or '(trace ends)'}")
    print("\nall threads match" if not diffs else f"\n{len(diffs)} thread(s) differ")
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
