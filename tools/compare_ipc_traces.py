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
                             [--fs-reads-multiset [--max-surplus N] [--fs-reads-coalesce [--fs-reads-prefix]]]
                             [--fs-write-mask FILE --fs-write-mask-session S]
  --strict     also compare every word of every command (with handles as names). Not
               reproducible on the oracle itself: a service that pushes a sub-word reply value can
               leave stale bytes above it in Azahar (seen: cfg:u, one run in about eight), so two
               Azahar runs of the same program can differ in a word. Compare two runs of the same
               CPU first and treat the words that differ there as noise.
  --ignore F   drop the commands listed in F from both traces (repeatable). Each line is
               `<service> <header>  # reason`: the session name (`fs:USER`; `fs:USER/file` for all
               the file sessions of fs:USER), the request's header word in hex. `#` starts a comment.
               config/native/ipc_time_dependent.txt will list the commands whose replies depend on
               time as they are found (it exists since N1-11 and is empty so far; mh4n_ipc_replay reads
               the same format).
  --prefix     a thread may stop earlier in the candidate
  --context N  requests to show before the first difference (default 3)

Across two timing models (D52; the cross profile of tools/native/golden.py and the native runtime's
checks use these two options, and nothing else is loosened for `fs:`; the `native` profile adds --fs-reads-coalesce):
  --fs-reads-multiset   per thread, the Read requests of the file sessions of fs:USER (`fs:USER/file`,
               header 0x080200c2) are compared as a multiset of (offset, size, result, CRC-32 of the data
               the reply wrote), order ignored: the smaller side's multiset must be contained in the
               larger one's, which may hold at most --max-surplus more reads (default 16). The clock
               decides how many reads a streaming thread makes before the run ends and in what order two
               readers interleave, never what a read returns. Every other `fs:` request of the thread is
               compared in order and in full: the session, header, result, descriptors (a Write's CRC-32
               included) and the header and normal parameters of the request and the reply (offset, size,
               flags, paths' sizes...); the words that follow them are translate parameters (descriptors,
               handles, buffer addresses) and are not parameters of the command. No `fs:` command goes in
               the --ignore files.
  --max-surplus N       reads the larger multiset may hold beyond the smaller (default 16; with
               --fs-reads-multiset only)
  --fs-reads-coalesce   (D76, the `native` profile of tools/native/golden.py; needs --fs-reads-multiset) before the
               multiset is built, the Reads of one thread and one file session (`fs:USER/file#3`) whose byte ranges
               touch (offset + size of one is the offset of the next) are merged into one Read, whatever order
               they were sent in (the multiset ignores order, D52, and two readers that interleave differently send
               the same Reads in another order: N0-33 found that merging in the order of the requests fails the
               jit pairs that the cross profile passes): a session's Reads are taken by offset, and each joins the
               oldest run that ends at its offset or starts a new run. A run is the Read of the summed size, its
               CRC-32 the combination of the parts' CRC-32s (tools/native/tracefmt.py crc32_combine). Only a Read
               that succeeded, filled its buffer (the reply says it read `size` bytes) and has its CRC-32 in the
               trace is merged; any other Read is compared on its own. The surplus is then counted in runs, so a
               piece read twice is one run more, not a way to hide a missing piece. A reader that works in another
               clock reads the same bytes in other pieces (one 28672-byte Read against 4096- and 2048-byte pieces
               of the same range): the bytes must still be the same, so a gap, a piece read twice, a piece with
               another CRC-32 or a run that goes on past where the other side's stops is a difference
               (unless --fs-reads-prefix is given).
  --fs-reads-prefix     (needs --fs-reads-coalesce; not in D76's text, found necessary on the first real pairs: see
               docs/native/comparators.md) a streaming reader stops where its clock says, so one side's run of
               touching Reads can go on further than the other's. A run that the other side has no equal of is then
               also contained when it is a prefix of a run of the other side, or that run is a prefix of it: the
               same start and result, and a part boundary of the longer run exactly where the shorter ends, with the
               same CRC-32 of the bytes up to there (combined from the longer run's parts). Without a part boundary
               at that offset the longer run's bytes there are not known and the runs differ; each run is in one
               pair at most. The Reads of the longer run beyond the prefix count toward the surplus
               (--max-surplus) with the other surplus.
  --fs-write-mask FILE  (needs --fs-write-mask-session S) byte ranges of a Write that depend on the clock,
               one per line `<session> <write-size> <write-offset> <mask-offset> <mask-size>  <reason>`
               (config/native/fs_write_masks.txt). A Write of the file sessions of fs:USER whose size and
               offset are those of a line of session S is compared by the CRC-32 of its data with the
               masked ranges set to zero, on both sides; a difference outside a masked range still fails.
               The trace must hold the Write's data (mh4u_runner --trace-ipc-full). A mask file that is
               missing, empty, malformed, or one of whose lines of session S matches no Write of either
               trace is an error (exit 2), never a silent pass.
  --fs-write-mask-session S   the golden session the traces belong to (`G2`): the lines of other sessions
               are not looked at.
With --fs-reads-multiset the report also has an `fs coverage:` line, directly after the `fs reads:` line (N0-36,
docs/native/comparators.md section 11; an `fs write masks:` line can follow it): for each thread and file session, the
union of the byte ranges the Reads returned (whatever the pieces) on each side, and how many bytes (in how many
separate ranges) only one side covers. It is information and no rule looks at it: the jit and native pairs of the
sessions without a quest have at most 10 KB per thread that only one side reads (a stream cut at another place), the
quest sessions G4 and G3 have 133 KB and 3 MB (the game loads other resources on the two models), and the number tells
how far a pair is from the calibrated ones.
Exit status 0 when every thread matches (up to the candidate's end with --prefix), 1 when
something differs, 2 for unreadable input: a file that is missing, has no `# ipctrace` header (an
SVC trace, an empty file, a runner that ignored --trace-ipc), is not UTF-8 text, holds no request
line, or has none left after --ignore is refused, so a wrong path or a missing hook never reads as
"all threads match"; and for a wrong option or a write mask that cannot be applied.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import zlib
from collections import Counter
from dataclasses import dataclass, replace
from typing import NamedTuple

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "native"))
import tracefmt  # noqa: E402

IpcCall = tracefmt.IpcCall
DEFAULT_MAX_SURPLUS = 16        # D52: reads one side's multiset may hold beyond the other's


class IgnoreError(ValueError):
    pass


class MaskError(ValueError):
    """A write-mask file or a mask that cannot be applied (exit status 2)."""


@dataclass(frozen=True)
class WriteMask:
    """One line of the write-mask file: bytes [offset, offset + size) of the data of the Write of
    `write_size` bytes at file offset `write_offset`, in golden session `session`."""
    number: int
    session: str
    write_size: int
    write_offset: int
    offset: int
    size: int
    reason: str


class MaskUse:
    """What the write masks did to one trace."""

    def __init__(self) -> None:
        self.lines = Counter()
        self.writes = 0


def load_write_masks(path: str, session: str) -> list[WriteMask]:
    """The lines of `session` in the write-mask file (D52). The file must exist, hold at least one line,
    and every line must be well formed, whatever its session."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        raise MaskError(f"{path}: cannot read the write-mask file: {e.strerror}") from None
    except UnicodeError:
        raise MaskError(f"{path}: not a write-mask file (not UTF-8 text)") from None
    lines: list[WriteMask] = []
    for n, raw in enumerate(text.splitlines(), 1):
        body = raw.split("#", 1)[0].split()
        if not body:
            continue
        if len(body) < 6:
            raise MaskError(f"{path}:{n}: expected `<session> <write-size> <write-offset> <mask-offset> "
                            f"<mask-size>  <reason>` (a reason is required)")
        numbers = []
        for field in body[1:5]:
            try:
                value = int(field, 0)
            except ValueError:
                raise MaskError(f"{path}:{n}: {field!r} is not a number") from None
            if value < 0:
                raise MaskError(f"{path}:{n}: {field!r} is negative")
            numbers.append(value)
        write_size, write_offset, offset, size = numbers
        if size == 0 or offset + size > write_size:
            raise MaskError(f"{path}:{n}: the range {offset}+{size} is empty or outside the {write_size}-byte Write")
        lines.append(WriteMask(n, body[0], write_size, write_offset, offset, size, " ".join(body[5:])))
    if not lines:
        raise MaskError(f"{path}: the write-mask file holds no line")
    return [m for m in lines if m.session == session]


def _zero_ranges(data: bytes, ranges: list[tuple[int, int]]) -> bytes:
    out = bytearray(data)
    for offset, size in ranges:
        out[offset:offset + size] = bytes(size)
    return bytes(out)


def apply_write_mask(path: str, call: IpcCall, masks: list[WriteMask]) -> IpcCall:
    """The Write with the CRC-32 of its data replaced by the CRC-32 of the data with the masked ranges set to
    zero. The data must be in the trace, and must be the data the trace's own CRC-32 says."""
    size = tracefmt.fs_file_request(call)[1]
    candidates = [d for d in call.desc if d.kind in tracefmt.IPC_READ_BUFFERS and d.size == size
                  and tracefmt.ipc_data(call, d.index) is not None]
    if not candidates:
        raise MaskError(f"{path}: the Write of {size} bytes (seq {call.seq}) has no data in the trace: "
                        f"record it with --trace-ipc-full")
    d = candidates[0]
    try:
        data = tracefmt.ipc_data(call, d.index)
    except ValueError:
        raise MaskError(f"{path}: the data of the Write at seq {call.seq} is not hex") from None
    if data is None or len(data) != d.size:
        raise MaskError(f"{path}: the data of the Write at seq {call.seq} holds {0 if data is None else len(data)} "
                        f"bytes, its descriptor says {d.size}")
    if d.token.startswith("0x"):
        try:
            recorded = int(d.token, 16)
        except ValueError:
            raise MaskError(f"{path}: the Write at seq {call.seq} has a malformed CRC-32") from None
        if recorded != zlib.crc32(data):
            raise MaskError(f"{path}: the data of the Write at seq {call.seq} does not match its CRC-32")
    crc = zlib.crc32(_zero_ranges(data, [(m.offset, m.size) for m in masks]))
    return replace(call, desc=tuple(replace(x, token=f"0x{crc:08x}") if x is d else x for x in call.desc))


def load_ignore(paths: list[str]) -> set[tuple[str, int]]:
    """The (service, header) pairs of the ignore files."""
    ignored: set[tuple[str, int]] = set()
    for path in paths:
        try:
            with open(path, encoding="utf-8") as f:
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
        except UnicodeError:
            raise IgnoreError(f"{path}: not an ignore file (not UTF-8 text)") from None
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


def what_differs(a: IpcCall, b: IpcCall, strict: bool, fs_full: bool = False) -> list[str]:
    """The names of the parts that differ, without their values. `fs_full`: the requests of `fs:` sessions
    are also compared by their header and normal parameters (`fs_key`)."""
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
    elif fs_full and a.session.startswith("fs:") and b.session.startswith("fs:"):
        for name, x, y in (("in", a.words_in, b.words_in), ("out", a.words_out, b.words_out)):
            idx = _word_diffs(tracefmt.command_words(x), tracefmt.command_words(y))
            if idx:
                out.append(f"{name} parameter word{'s' if len(idx) > 1 else ''} {','.join(map(str, idx))}")
    return out


def describe(call: IpcCall) -> str:
    """One request as the report shows it: session, header, result, descriptors (never words)."""
    result = call.result if call.result is not None else "-"
    return (f"seq={call.seq} {call.session} cmd={call.cmd or '-'} result={result} "
            f"desc={tracefmt.join_items(call.desc)} outdesc={tracefmt.join_items(call.outdesc)}")


class TraceError(Exception):
    """The file is not a usable IPC trace."""


def load(path: str, ignored: set[tuple[str, int]], masks: list[WriteMask] | None = None,
         used: "MaskUse | None" = None) -> dict[str, list[IpcCall]]:
    """The requests of a trace by thread. `masks`: the write masks of the session (D52); a Write they apply to
    has the CRC-32 of its data replaced by that of the data with the masked ranges zeroed, and `used` counts
    the Writes masked and, per mask line number, the Writes each line applied to."""
    try:
        raw = tracefmt.parse_ipc(path)
    except UnicodeError:
        raise TraceError(f"{path}: not an IPC trace (not UTF-8 text)") from None
    if raw.header is None or raw.header.get("tool") != "ipctrace":
        raise TraceError(f"{path}: not an IPC trace (no '# ipctrace' header)")
    if not raw.calls:
        raise TraceError(f"{path}: the IPC trace holds no request")
    try:
        trace = tracefmt.normalise_ipc(raw)
        calls = [c for c in trace.calls if not is_ignored(c, ignored)]
        if masks:
            for i, c in enumerate(calls):
                request = tracefmt.fs_file_request(c) if tracefmt.fs_file_command(c) == "write" else None
                here = [m for m in masks if request == (m.write_offset, m.write_size)] if request else []
                if here:
                    calls[i] = apply_write_mask(path, c, here)
                    if used is not None:
                        used.lines.update(m.number for m in here)
                        used.writes += 1
        kept = tracefmt.IpcTrace(trace.header, trace.threads, calls)
        by_thread = kept.by_thread() if kept.calls else {}
    except MaskError:
        raise
    except (ValueError, IndexError, KeyError, OverflowError) as e:
        raise TraceError(f"{path}: not a well-formed IPC trace ({type(e).__name__})") from None
    if not kept.calls:
        raise TraceError(f"{path}: --ignore removed every request of the IPC trace")
    return by_thread


def _thread_order(tid: str) -> tuple:
    plain = tid.isascii() and tid.isdigit() and len(tid) <= 18
    return (0, 0, tid) if tid == "main" else ((1, int(tid), "") if plain else (2, 0, tid))


def fs_key(call: IpcCall, strict: bool = False) -> tuple:
    """`key`, and for a request of an `fs:` session also the header and the normal parameters of the request
    and of the reply (offset, size, flags, the sizes of paths...): what an `fs:` request is, in full (D52)."""
    k = key(call, strict)
    if call.session.startswith("fs:"):
        k += (tracefmt.command_words(call.words_in), tracefmt.command_words(call.words_out))
    return k


def is_fs_read(call: IpcCall) -> bool:
    return tracefmt.fs_file_command(call) == "read"


def read_key(call: IpcCall) -> tuple:
    """What a Read must have in common with its counterpart: offset, size, result and the CRC-32 of the data
    the reply wrote (D52). A request with no readable parameters keeps its whole key."""
    request = tracefmt.fs_file_request(call)
    if request is None:
        return ("malformed", key(call))
    return (request[0], request[1], call.result, tracefmt.fs_read_crc(call))


class ReadUnit(NamedTuple):
    """What one Read, or one run of touching Reads, is compared as: `key` = (offset, size, result, CRC-32 token),
    and for a Read or run that can be merged `prefixes` = after each of its Reads (Reads so far, bytes so far,
    CRC-32 token of those bytes), which the prefix rule needs."""
    key: tuple
    prefixes: tuple = ()


def read_units(calls: list[IpcCall], coalesce: bool) -> list[ReadUnit]:
    """What a thread's Reads are compared as: each Read's `read_key`, or with `coalesce` (D76) one unit per run of
    Reads that touch (`tracefmt.coalesce_fs_reads`): offset of the first, sum of the sizes, the result and the
    CRC-32 of the data of the whole run, combined from the parts'."""
    if not coalesce:
        return [ReadUnit(read_key(c)) for c in calls]
    units = []
    for run in tracefmt.coalesce_fs_reads(calls):
        prefixes = tuple(tracefmt.fs_run_prefixes(run))
        if len(run) == 1:
            units.append(ReadUnit(read_key(run[0]), prefixes))
        else:
            offset, size, crc = tracefmt.merged_fs_read(run)
            units.append(ReadUnit((offset, size, run[0].result, crc), prefixes))
    return units


def reads_beyond_prefix(short: ReadUnit, long_: ReadUnit) -> int | None:
    """How many Reads of the unit `long_` come after the bytes that `short` is: `short` must start at the same
    offset, have the same result, be shorter, and `long_` must have a Read that ends exactly where `short` ends
    with the same CRC-32 of the bytes up to there (combined from its parts'). None when `short` is not a prefix of
    `long_`: another start, no part of `long_` ends there (its bytes there are not known), or another CRC-32."""
    if not short.prefixes or not long_.prefixes or short.key[0] != long_.key[0] or short.key[2] != long_.key[2]:
        return None
    size, crc = short.key[1], short.key[3]
    if size >= long_.key[1]:
        return None
    for reads, covered, token in long_.prefixes:
        if covered == size:
            return len(long_.prefixes) - reads if token == crc else None
    return None


def pair_prefixes(left_a: list[ReadUnit], left_b: list[ReadUnit]) -> tuple[list[ReadUnit], list[ReadUnit], list[int]]:
    """Pair up units that no unit of the other side equals when one is a prefix of the other (`reads_beyond_prefix`,
    either way round), each unit in one pair at most. Returns the units of A and of B left over and, for each pair, the
    number of Reads the longer one has beyond the shorter."""
    by_start: dict[int, list[int]] = {}
    for j, u in enumerate(left_b):
        if isinstance(u.key[0], int):
            by_start.setdefault(u.key[0], []).append(j)
    taken: set[int] = set()
    used_a: set[int] = set()
    tails: list[int] = []
    for i, u in enumerate(left_a):
        if not isinstance(u.key[0], int):
            continue
        for j in by_start.get(u.key[0], []):
            if j in taken:
                continue
            v = left_b[j]
            tail = reads_beyond_prefix(u, v) if u.key[1] < v.key[1] else reads_beyond_prefix(v, u)
            if tail is not None:
                taken.add(j)
                used_a.add(i)
                tails.append(tail)
                break
    return ([u for i, u in enumerate(left_a) if i not in used_a],
            [u for j, u in enumerate(left_b) if j not in taken], tails)


def compare_reads(a: list[IpcCall], b: list[IpcCall], max_surplus: int, coalesce: bool = False,
                  read_prefix: bool = False, stats: Counter | None = None) -> dict | None:
    """The Reads of one thread on the two sides, as multisets: the smaller side's must be contained in the larger
    side's, which may hold at most `max_surplus` more. With `coalesce` (D76) the multisets hold the runs of touching
    Reads, not the Reads. With `read_prefix` (needs `coalesce`) a run that the other side has no equal of is also
    contained when it is a verified prefix of a run of the other side, or that run is a prefix of it
    (`reads_beyond_prefix`); the Reads of the longer run beyond the prefix count toward the surplus. `stats` counts
    the prefix pairs (`prefix_pairs`) and those Reads (`prefix_tail`). None when they are contained; else what is
    wrong."""
    ua, ub = read_units(a, coalesce), read_units(b, coalesce)
    ca, cb = Counter(u.key for u in ua), Counter(u.key for u in ub)
    small_is_ref = len(ua) <= len(ub)
    left_a, left_b = list((ca - cb).elements()), list((cb - ca).elements())
    tails: list[int] = []
    if read_prefix:
        units_a, units_b = {}, {}
        for u in ua:
            units_a.setdefault(u.key, u)
        for u in ub:
            units_b.setdefault(u.key, u)
        rest_a, rest_b, tails = pair_prefixes([units_a[k] for k in left_a], [units_b[k] for k in left_b])
        left_a, left_b = [u.key for u in rest_a], [u.key for u in rest_b]
    foreign = left_a if small_is_ref else left_b       # runs of the smaller side that the larger one lacks
    surplus = (len(ub) - len(ua) if small_is_ref else len(ua) - len(ub)) + sum(tails)
    if stats is not None:
        stats["prefix_pairs"] += len(tails)
        stats["prefix_tail"] += sum(tails)
    if not foreign and surplus <= max_surplus:
        return None
    return {"ref_reads": len(ua), "cand_reads": len(ub), "surplus": surplus, "max_surplus": max_surplus,
            "side": "reference" if small_is_ref else "candidate",
            "foreign": sorted(foreign, key=repr),
            "coalesced": coalesce, "ref_raw": len(a), "cand_raw": len(b), "prefix_tail": sum(tails)}


class Coverage(NamedTuple):
    """The bytes the Reads of one thread and one file session cover on each side (N0-36): the union of the ranges
    the Reads returned, whatever the pieces, and how much of it only one side covers."""
    thread: str
    session: str
    ref_bytes: int
    cand_bytes: int
    ref_only: int           # bytes the reference covers and the candidate does not
    cand_only: int
    ref_only_ranges: int
    cand_only_ranges: int


def fs_read_extent(call: IpcCall) -> tuple[int, int] | None:
    """[start, end) of the file bytes a Read of a file session returned: its offset and the number of bytes its reply
    says it read, at most the size asked for. A successful Read (result 0) whose reply has no byte count covers the
    whole size asked for. None for a Read that has no reply or no result, failed (result other than 0), read 0 bytes
    or has no readable offset and size: it covers nothing."""
    request = tracefmt.fs_file_request(call)
    if request is None or call.result is None:
        return None
    try:
        if int(call.result, 16) != 0:
            return None
    except ValueError:
        return None
    got = tracefmt.fs_read_bytes(call)
    size = request[1] if got is None else min(got, request[1])
    return (request[0], request[0] + size) if size > 0 else None


def merge_ranges(ranges) -> list[tuple[int, int]]:
    """The union of [start, end) ranges as sorted, disjoint ranges that do not touch."""
    out: list[list[int]] = []
    for start, end in sorted(ranges):
        if out and start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return [(a, b) for a, b in out]


def subtract_ranges(a: list[tuple[int, int]], b: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """The parts of the merged ranges `a` that no range of the merged ranges `b` covers."""
    out = []
    for start, end in a:
        at = start
        for lo, hi in b:
            if hi <= at or lo >= end:
                continue
            if lo > at:
                out.append((at, lo))
            at = max(at, hi)
            if at >= end:
                break
        if at < end:
            out.append((at, end))
    return out


def read_coverage(ref: dict[str, list[IpcCall]], cand: dict[str, list[IpcCall]]) -> list[Coverage]:
    """For each thread and file session that either side read from: what each side covers and what only one covers.
    Information only (N0-36): no rule of this tool looks at it. Two threads that read the same bytes are two rows."""
    def extents(calls: list[IpcCall]) -> dict[str, list[tuple[int, int]]]:
        out: dict[str, list[tuple[int, int]]] = {}
        for c in calls:
            if is_fs_read(c):
                e = fs_read_extent(c)
                out.setdefault(c.session, [])
                if e is not None:
                    out[c.session].append(e)
        return out

    rows = []
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        ea, eb = extents(ref.get(tid, [])), extents(cand.get(tid, []))
        for session in sorted(set(ea) | set(eb)):
            ua, ub = merge_ranges(ea.get(session, [])), merge_ranges(eb.get(session, []))
            only_a, only_b = subtract_ranges(ua, ub), subtract_ranges(ub, ua)
            rows.append(Coverage(tid, session, sum(e - s for s, e in ua), sum(e - s for s, e in ub),
                                 sum(e - s for s, e in only_a), sum(e - s for s, e in only_b),
                                 len(only_a), len(only_b)))
    return rows


def coverage_line(rows: list[Coverage]) -> str:
    """The `fs coverage:` line of the report: bytes that only one side covers, in total and the largest per thread and
    file session. Never gated."""
    here_a = [r for r in rows if r.ref_only]
    here_b = [r for r in rows if r.cand_only]
    text = (f"fs coverage: {len(rows)} thread/file session(s) read; bytes only the reference covers: "
            f"{sum(r.ref_only for r in rows)} (in {len(here_a)}, {sum(r.ref_only_ranges for r in rows)} range(s)), "
            f"only the candidate covers: {sum(r.cand_only for r in rows)} (in {len(here_b)}, "
            f"{sum(r.cand_only_ranges for r in rows)} range(s))")
    worst = []
    for label, field, rs in (("reference", "ref_only", here_a), ("candidate", "cand_only", here_b)):
        if rs:
            top = max(rs, key=lambda r: (getattr(r, field), r.thread))
            worst.append(f"{label} {getattr(top, field)} on thread {top.thread}")
    if worst:
        text += "; the most per thread and file session: " + ", ".join(worst)
    return text + " (information, not a rule)"


def compare(ref: dict[str, list[IpcCall]], cand: dict[str, list[IpcCall]], strict: bool = False,
            prefix: bool = False, fs_reads: bool = False, max_surplus: int = DEFAULT_MAX_SURPLUS,
            coalesce: bool = False, read_prefix: bool = False, stats: Counter | None = None) -> list[dict]:
    """One report per thread that differs: thread, index of the first difference, the two requests
    (None past the end of a trace), and the lengths. With `fs_reads` (D52) a thread's Reads of the file
    sessions of fs:USER are taken out and compared as multisets (a report of kind `reads`), and its other
    `fs:` requests are compared in full; the rest is compared as before (kind `order`). `coalesce` (D76)
    merges the touching Reads of a file session before the multisets are built, and read_prefix lets a run that is a
    verified prefix of another side's run count as contained (compare_reads)."""
    out = []
    kf = fs_key if fs_reads else key
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        a, b = ref.get(tid, []), cand.get(tid, [])
        if fs_reads:
            ra, rb = [c for c in a if is_fs_read(c)], [c for c in b if is_fs_read(c)]
            a, b = [c for c in a if not is_fs_read(c)], [c for c in b if not is_fs_read(c)]
            problem = compare_reads(ra, rb, max_surplus, coalesce, read_prefix, stats) if ra or rb else None
            if problem:
                out.append({"kind": "reads", "thread": tid, **problem})
        n = min(len(a), len(b))
        first = next((i for i in range(n) if kf(a[i], strict) != kf(b[i], strict)), None)
        if first is None and (len(a) == len(b) or (prefix and len(b) <= len(a))):
            continue
        if first is None:
            first = n
        out.append({"kind": "order", "thread": tid, "index": first, "ref": a[first] if first < len(a) else None,
                    "cand": b[first] if first < len(b) else None, "ref_len": len(a), "cand_len": len(b),
                    "series": a, "series_cand": b})
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
    ap.add_argument("--fs-reads-multiset", action="store_true",
                    help="compare each thread's fs:USER/file Reads as a multiset of (offset, size, result, CRC of "
                         "the data read), order ignored; every other fs: request in order and in full (D52)")
    ap.add_argument("--max-surplus", type=int, default=None, metavar="N",
                    help=f"reads the larger multiset may hold beyond the smaller (default {DEFAULT_MAX_SURPLUS}; "
                         f"needs --fs-reads-multiset)")
    ap.add_argument("--fs-reads-coalesce", action="store_true",
                    help="before the multiset is built, merge the consecutive Reads of one thread and one file "
                         "session whose byte ranges touch into one Read (summed size, CRC-32 combined); needs "
                         "--fs-reads-multiset (D76)")
    ap.add_argument("--fs-reads-prefix", action="store_true",
                    help="a merged run that the other side has no equal of is also contained when it is a prefix of "
                         "a run of the other side (or that run of it): same start, the longer run has a part that ends "
                         "where the shorter ends, with the same CRC-32 up to there; the Reads beyond the prefix count "
                         "toward the surplus; needs --fs-reads-coalesce")
    ap.add_argument("--fs-write-mask", metavar="FILE",
                    help="byte ranges of a Write that depend on the clock (config/native/fs_write_masks.txt); "
                         "needs --fs-write-mask-session")
    ap.add_argument("--fs-write-mask-session", metavar="S",
                    help="the golden session of the traces (G2): only the lines of that session are used")
    args = ap.parse_args(argv)
    if args.max_surplus is not None and not args.fs_reads_multiset:
        ap.error("--max-surplus is a rule of --fs-reads-multiset")
    if args.fs_reads_coalesce and not args.fs_reads_multiset:
        ap.error("--fs-reads-coalesce is a rule of --fs-reads-multiset")
    if args.fs_reads_prefix and not args.fs_reads_coalesce:
        ap.error("--fs-reads-prefix is a rule of --fs-reads-coalesce")
    if args.max_surplus is not None and args.max_surplus < 0:
        ap.error("--max-surplus must not be negative")
    if bool(args.fs_write_mask) != bool(args.fs_write_mask_session):
        ap.error("--fs-write-mask and --fs-write-mask-session go together")
    max_surplus = DEFAULT_MAX_SURPLUS if args.max_surplus is None else args.max_surplus
    masks: list[WriteMask] = []
    used_ref, used_cand = MaskUse(), MaskUse()
    try:
        ignored = load_ignore(args.ignore)
        if args.fs_write_mask:
            masks = load_write_masks(args.fs_write_mask, args.fs_write_mask_session)
        ref = load(args.reference, ignored, masks, used_ref)
        cand = load(args.candidate, ignored, masks, used_cand)
        for m in masks:
            if not used_ref.lines[m.number] and not used_cand.lines[m.number]:
                raise MaskError(f"{args.fs_write_mask}:{m.number}: the mask of session {m.session} (Write of "
                                f"{m.write_size} bytes at offset {m.write_offset}) matches no Write of either trace")
    except (OSError, IgnoreError, TraceError, MaskError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    stats: Counter = Counter()
    diffs = compare(ref, cand, args.strict, args.prefix, args.fs_reads_multiset, max_surplus, args.fs_reads_coalesce,
                    args.fs_reads_prefix, stats)
    for tid in sorted(set(ref) | set(cand), key=_thread_order):
        print(f"thread {tid}: {len(ref.get(tid, []))} vs {len(cand.get(tid, []))} requests")
    if args.prefix:
        absent = [t for t in sorted(ref, key=_thread_order) if ref[t] and not cand.get(t)]
        if absent:
            print(f"note: the candidate sent no request on thread(s) {', '.join(absent)}")
    for d in diffs:
        if d["kind"] == "reads":
            print(f"\nthread {d['thread']}: fs:USER/file Reads differ as multisets: {d['ref_reads']} on the "
                  f"reference, {d['cand_reads']} on the candidate (surplus {d['surplus']}, at most {d['max_surplus']})"
                  + (f" (reads merged into runs: from {d['ref_raw']} and {d['cand_raw']} reads)"
                     if d["coalesced"] else "")
                  + (f" (and {d['prefix_tail']} read(s) beyond runs that are a prefix of another)"
                     if d["prefix_tail"] else ""))
            if d["foreign"]:
                print(f"  {len(d['foreign'])} Read(s) of the {d['side']} (the side with fewer) are not on the other "
                      f"side (offset, size, result, CRC-32 of the data):")
                for item in d["foreign"][:5]:
                    print(f"    {', '.join(str(x) for x in item)}")
            continue
        i, a = d["index"], d["series"]
        print(f"\nthread {d['thread']} differs at request {i}:")
        for j in range(max(0, i - args.context), i):
            print(f"    {j}: {describe(a[j])}")
        for label, call in (("ref ", d["ref"]), ("cand", d["cand"])):
            print(f"  {label} {i}: {describe(call) if call else '(no more requests)'}")
        if d["ref"] is not None and d["cand"] is not None:
            what = what_differs(d["ref"], d["cand"], args.strict, args.fs_reads_multiset)
            print(f"  differs in: {', '.join(what) or 'nothing visible'}")
    if not diffs:
        print("\nall threads match" + (" (candidate may be shorter)" if args.prefix else ""))
    if args.fs_reads_multiset:
        mine = {t: ([c for c in ref.get(t, []) if is_fs_read(c)], [c for c in cand.get(t, []) if is_fs_read(c)])
                for t in set(ref) | set(cand)}
        reading = {t: v for t, v in mine.items() if v != ([], [])}
        sizes = {t: (len(v[0]), len(v[1])) for t, v in reading.items()}
        counted = sizes
        merged = ""
        if args.fs_reads_coalesce:        # the surplus is counted in runs
            counted = {t: (len(read_units(v[0], True)), len(read_units(v[1], True))) for t, v in reading.items()}
            merged = (f"; after merging touching reads: {sum(v[0] for v in counted.values())} and "
                      f"{sum(v[1] for v in counted.values())}")
        if stats["prefix_pairs"]:
            merged += (f"; prefix of a longer run: {stats['prefix_pairs']} run(s), {stats['prefix_tail']} read(s) "
                       f"beyond it")
        worst = max(counted.items(), key=lambda kv: (abs(kv[1][0] - kv[1][1]), kv[0]), default=None)
        print(f"fs reads: {len(reading)} thread(s) read; {sum(v[0] for v in sizes.values())} on the reference and "
              f"{sum(v[1] for v in sizes.values())} on the candidate" + merged
              + (f"; the largest difference is {abs(worst[1][0] - worst[1][1])} on thread {worst[0]} "
                 f"(at most {max_surplus})" if worst else ""))
        print(coverage_line(read_coverage(ref, cand)))                  # N0-36: information, never a rule
    if args.fs_write_mask:
        print(f"fs write masks: {len(masks)} line(s) of session {args.fs_write_mask_session}, applied to "
              f"{used_ref.writes} Write(s) of the reference and {used_cand.writes} of the candidate")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
