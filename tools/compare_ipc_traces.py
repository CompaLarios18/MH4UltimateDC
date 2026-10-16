#!/usr/bin/env python3
"""Compare two IPC traces of the same program (mh4u_runner --trace-ipc, and the native runtime's),
thread by thread, and show where they first differ.

Two runtimes interleave their threads differently and number their threads and handles
differently, but each thread should send the same requests to the same services and get the same
results back. The traces are normalised first (tools/native/tracefmt.py: threads become `main` or
`<entry>#<k>`, handles in words become their names, times are dropped), then each thread's
requests, in the order they were sent, are compared. The first one that differs points at the code
that ran between it and the previous (matching) request.

Trace format (N0-21, docs/native/wp/N0.md; hex as `0x` and 8 lowercase digits):
    # ipctrace v1 <ISO date> game=<title id> cpu=<jit|recomp|interp|native> new3ds=<on|off>
    # thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>
    <thread> seq=<n> session=<name> cmd=<header> in=<words> desc=<descriptors> out=<words>
             outdesc=<descriptors> t=<ticks> [data=<index>:<hex>]...        (one line, no break)
  - One line per request, written when its reply is recorded. A reply a service defers comes after
    the lines of requests other threads sent later; `seq` numbers the requests in the order they
    were sent (from 1). A request that never got a reply (the run ended first, or the call failed
    before the service saw it) has `out=-` `outdesc=-`; one the recorder never saw the contents of
    has `cmd=-` `in=-`.
  - session: the service's port name for a session that has one (`fs:USER`, `gsp::Gpu`); for a
    session a reply handed out, `<service>/<kind>#<k>` (`fs:USER/file#3`: the fourth file session
    of fs:USER; kinds: file, dir, session, or the lowercase kernel object type). A session the
    trace saw nothing about is `unnamed#<k>`. Never a file name.
  - in: every word of the command the client wrote (the header, the normal parameters, the
    translate parameters with their descriptors), out: every word of the reply as the client read
    it (the header, the result code, ...). The first reply parameter (out word 1) is the result.
  - desc, outdesc: comma-separated `<kind>:<index>:<size>:<token>`, `-` for none (see
    tracefmt.IpcDesc). desc is what the request carried: buffers with the CRC-32 of the bytes the
    service could read, handles with their names. outdesc is what the reply did: the static buffers
    it wrote (CRC-32 of the bytes), the request's writable mapped buffers after the reply (CRC-32),
    the handles it handed out with their names.
  - data (only with --trace-ipc-full): the bytes of an input buffer, hex, by descriptor index.
Traces hold game data (file names, save bytes): keep them in build/ and never paste lines from them.
This tool prints only session names, headers, result codes, descriptor indices, sizes, CRCs and
counts; with --strict it names the words that differ, never their values.

usage: compare_ipc_traces.py REF CAND [--strict] [--ignore FILE]... [--prefix] [--context N]
  --strict     also compare every word of every command (with handles as names). Not
               reproducible on the oracle itself: a service that pushes a sub-word reply value can
               leave stale bytes above it in Azahar (seen: cfg:u, one run in about eight), so two
               Azahar runs of the same program can differ in a word. Compare two runs of the same
               CPU first and treat the words that differ there as noise.
  --ignore F   drop the commands listed in F from both traces (repeatable). Each line is
               `<service> <header>  # reason`: the session name (`fs:USER`; `fs:USER/file` for all
               the file sessions of fs:USER), the request's header word in hex. `#` starts a comment.
               config/native/ipc_time_dependent.txt will list the commands whose replies depend on
               time (N1-11 creates it).
  --prefix     a thread may stop earlier in the candidate
  --context N  requests to show before the first difference (default 3)
Exit status 0 when every thread matches (up to the candidate's end with --prefix), 1 when
something differs, 2 for unreadable input: a file that is missing, has no `# ipctrace` header (an
SVC trace, an empty file, a runner that ignored --trace-ipc) or holds no request line is refused,
so a wrong path or a missing hook never reads as "all threads match".
"""
from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "native"))
import tracefmt  # noqa: E402

IpcCall = tracefmt.IpcCall


class IgnoreError(ValueError):
    pass


def load_ignore(paths: list[str]) -> set[tuple[str, int]]:
    """The (service, header) pairs of the ignore files."""
    ignored: set[tuple[str, int]] = set()
    for path in paths:
        with open(path) as f:
            for n, line in enumerate(f, 1):
                text = line.split("#", 1)[0].split()
                if not text:
                    continue
                if len(text) != 2:
                    raise IgnoreError(f"{path}:{n}: expected `<service> <header>`")
                try:
                    header = int(text[1], 16)
                except ValueError:
                    raise IgnoreError(f"{path}:{n}: the header {text[1]!r} is not hex") from None
                ignored.add((text[0], header))
    return ignored


def _instance_free(session: str) -> str:
    """`fs:USER/file#3` -> `fs:USER/file`."""
    return re.sub(r"#\d+$", "", session)


def is_ignored(call: IpcCall, ignored: set[tuple[str, int]]) -> bool:
    if call.cmd is None or not ignored:
        return False
    try:
        header = int(call.cmd, 16)
    except ValueError:
        return False
    return (call.session, header) in ignored or (_instance_free(call.session), header) in ignored


def _descs(descs) -> tuple:
    return tuple((d.kind, d.index, d.size, d.token) for d in descs)


def key(call: IpcCall, strict: bool = False) -> tuple:
    """What two requests must have in common to match."""
    k = (call.session, call.cmd, call.result, _descs(call.desc), _descs(call.outdesc))
    if strict:
        k += (call.words_in, call.words_out)
    return k


def _word_diffs(a: tuple[str, ...] | None, b: tuple[str, ...] | None) -> list[int]:
    a, b = a or (), b or ()
    return [i for i in range(max(len(a), len(b))) if (a[i] if i < len(a) else None) != (b[i] if i < len(b) else None)]


def what_differs(a: IpcCall, b: IpcCall, strict: bool) -> list[str]:
    """The names of the parts that differ, without their values."""
    out = []
    if a.session != b.session:
        out.append("session")
    if a.cmd != b.cmd:
        out.append("header")
    if a.result != b.result:
        out.append("result")
    for name, x, y in (("desc", a.desc, b.desc), ("outdesc", a.outdesc, b.outdesc)):
        if _descs(x) != _descs(y):
            where = [i for i in range(max(len(x), len(y)))
                     if (_descs(x)[i:i + 1] != _descs(y)[i:i + 1])]
            out.append(f"{name} entr{'y' if len(where) == 1 else 'ies'} {','.join(map(str, where))}")
    if strict:
        for name, x, y in (("in", a.words_in, b.words_in), ("out", a.words_out, b.words_out)):
            idx = _word_diffs(x, y)
            if idx:
                out.append(f"{name} word{'s' if len(idx) > 1 else ''} {','.join(map(str, idx))}")
    return out


def describe(call: IpcCall) -> str:
    """One request as the report shows it: session, header, result, descriptors (never words)."""
    result = call.result if call.result is not None else "-"
    return (f"seq={call.seq} {call.session} cmd={call.cmd or '-'} result={result} "
            f"desc={tracefmt.join_items(call.desc)} outdesc={tracefmt.join_items(call.outdesc)}")


class TraceError(Exception):
    """The file is not a usable IPC trace."""


def load(path: str, ignored: set[tuple[str, int]]) -> dict[str, list[IpcCall]]:
    raw = tracefmt.parse_ipc(path)
    if raw.header is None or raw.header.get("tool") != "ipctrace":
        raise TraceError(f"{path}: not an IPC trace (no '# ipctrace' header)")
    if not raw.calls:
        raise TraceError(f"{path}: the IPC trace holds no request")
    trace = tracefmt.normalise_ipc(raw)
    kept = tracefmt.IpcTrace(trace.header, trace.threads, [c for c in trace.calls if not is_ignored(c, ignored)])
    return kept.by_thread()


def _thread_order(tid: str) -> tuple:
    return (0, 0, tid) if tid == "main" else ((1, int(tid), "") if tid.isdigit() else (2, 0, tid))


def compare(ref: dict[str, list[IpcCall]], cand: dict[str, list[IpcCall]], strict: bool = False,
            prefix: bool = False) -> list[dict]:
    """One report per thread that differs: thread, index of the first difference, the two requests
    (None past the end of a trace), and the lengths."""
    out = []
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        a, b = ref.get(tid, []), cand.get(tid, [])
        n = min(len(a), len(b))
        first = next((i for i in range(n) if key(a[i], strict) != key(b[i], strict)), None)
        if first is None and (len(a) == len(b) or (prefix and len(b) <= len(a))):
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
    ap.add_argument("--strict", action="store_true", help="also compare every word of every command")
    ap.add_argument("--ignore", action="append", default=[], metavar="FILE",
                    help="drop the commands listed in FILE (`<service> <header>  # reason`); repeatable")
    ap.add_argument("--prefix", action="store_true", help="a thread may stop earlier in the candidate")
    ap.add_argument("--context", type=int, default=3, help="requests to show before a difference")
    args = ap.parse_args(argv)
    try:
        ignored = load_ignore(args.ignore)
        ref, cand = load(args.reference, ignored), load(args.candidate, ignored)
    except (OSError, IgnoreError, TraceError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    diffs = compare(ref, cand, args.strict, args.prefix)
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        print(f"thread {tid}: {len(ref.get(tid, []))} vs {len(cand.get(tid, []))} requests")
    if args.prefix:
        absent = [t for t in sorted(ref, key=_thread_order) if ref[t] and not cand.get(t)]
        if absent:
            print(f"note: the candidate sent no request on thread(s) {', '.join(absent)}")
    for d in diffs:
        i, a = d["index"], ref.get(d["thread"], [])
        print(f"\nthread {d['thread']} differs at request {i}:")
        for j in range(max(0, i - args.context), i):
            print(f"    {j}: {describe(a[j])}")
        for label, call in (("ref ", d["ref"]), ("cand", d["cand"])):
            print(f"  {label} {i}: {describe(call) if call else '(no more requests)'}")
        if d["ref"] is not None and d["cand"] is not None:
            print(f"  differs in: {', '.join(what_differs(d['ref'], d['cand'], args.strict)) or 'nothing visible'}")
    if not diffs:
        print("\nall threads match" + (" (candidate may be shorter)" if args.prefix else ""))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
