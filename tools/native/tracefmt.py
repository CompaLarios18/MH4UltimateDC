#!/usr/bin/env python3
"""Parse and normalise system-call traces (mh4u_runner --trace-svc, and the native runtime's).

Two runtimes number their threads and handles differently, and interleave their threads
differently, so a trace can only be compared after its names have been replaced by ones
that follow from what the program did. This module reads the trace format and does that.

Trace format (N0-04, docs/native/wp/N0.md); hex is 8 uppercase digits without 0x:
    # svctrace v1 <ISO date> game=0x<title id> cpu=<jit|recomp|interp|native> new3ds=<on|off>
    # thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>
    <thread> <svc> pc=<hex> lr=<hex> <r0> <r1> <r2> <r3> t=<ticks> in4=<r4>,<r5>,<r6> out=<r0>,<r1>,<r2>,<r3>
A call's line is written when the call completes for its thread (so a call that blocked comes
after other threads' later calls; within one thread the order is the order of the calls).
`out=-` marks a call that had not completed when the run ended. Every other line starting
with `#` is metadata. Traces from before N0-04 end at `t=`; they parse, with no in4 or out.

Normalisation (`normalise`):
  - a thread becomes `main` (the one created with parent=0) or `<entry>#<k>`, the k-th thread
    created with that entry, counting from 0 in the order of the `# thread` lines; a thread
    with no thread line keeps its number;
  - a handle in a register that the SVC takes or returns as a handle becomes `<kind>#<k>`
    (kind: event, mutex, semaphore, timer, arbiter, memblock, thread, port; k counts from 0 per
    kind, in the order the handles are created in the file). A handle nobody here created (one
    that came in an IPC reply) becomes `?`. The pseudo handles of the current thread and
    process, and 0, stay as they are. A closed handle is forgotten;
  - values that depend on time are dropped (shown as `~`): `t=`, and the outputs of
    GetSystemTick (SVC 0x28);
  - when a call failed (bit 31 of r0 set) or timed out (09401BFE), r1 to r3 are dropped too:
    Azahar's SVC wrapper writes its output variables whether or not the call set them, so they
    hold stack leftovers (WaitSynchronizationN's index after a timeout, for one).
Handles are counted in the file's order, so two runtimes that create handles in another order
on different threads name them differently; the order of one thread's own calls never changes.

IPC traces (N0-21, `mh4u_runner --trace-ipc`) are read by the same module (`parse_ipc`,
`normalise_ipc`); tools/compare_ipc_traces.py documents their format. `tracefmt.py` tells an IPC
trace from an SVC trace by its header (`# ipctrace`).

With an IPC trace of the same run (`normalise(svc_trace, ipc=ipc_trace)`, `--ipc FILE` on the
command line), the handles that came in IPC replies are named after the IPC trace
(`<service>/<kind>#<k>`) instead of `?`. A run's n-th SendSyncRequest of a thread (not counting
the ones that failed with 'invalid handle', which the recorder never sees) is that thread's n-th
IPC request; the handles its reply handed out start to exist then, and the ones its request
moved (MoveHandle descriptors, which the kernel closes) are forgotten.

usage: tracefmt.py TRACE [--normalize] [--ipc FILE]
                                           print the trace's calls (normalised or as read)
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import deque
from dataclasses import dataclass, field, replace

OK = "00000000"
RESULT_TIMEOUT = "09401BFE"
UNKNOWN_HANDLE = "?"
DROPPED = "~"
PSEUDO_HANDLES = frozenset({"FFFF8000", "FFFF8001"})  # the current thread, the current process
SVC_GET_SYSTEM_TICK = "28"
SVC_CLOSE_HANDLE = "23"
SVC_SEND_SYNC_REQUEST = "32"
RESULT_INVALID_HANDLE = "D8E007F7"   # SendSyncRequest on a handle that is not a session: no IPC record

# SVC -> registers (r0..r3, as the call is made) that hold a handle. Facts about the SVC
# interface (the numbers and the register assignment follow the hardware's), read from Azahar's
# SVC table and its wrapper; no code is copied.
INPUT_HANDLES: dict[str, tuple[int, ...]] = {
    "0B": (1,),   # GetThreadPriority
    "0C": (0,),   # SetThreadPriority
    "14": (0,),   # ReleaseMutex
    "16": (1,),   # ReleaseSemaphore
    "18": (0,),   # SignalEvent
    "19": (0,),   # ClearEvent
    "1B": (0,),   # SetTimer
    "1C": (0,),   # CancelTimer
    "1D": (0,),   # ClearTimer
    "1F": (0,),   # MapMemoryBlock
    "20": (0,),   # UnmapMemoryBlock
    "22": (0,),   # ArbitrateAddress
    "23": (0,),   # CloseHandle
    "24": (0,),   # WaitSynchronization1
    "27": (1,),   # DuplicateHandle
    "29": (1,),   # GetHandleInfo
    "2B": (1,),   # GetProcessInfo
    "2C": (1,),   # GetThreadInfo
    "32": (0,),   # SendSyncRequest
    "35": (1,),   # GetProcessId
    "36": (1,),   # GetProcessIdOfThread
    "37": (1,),   # GetThreadId
    "48": (1,),   # CreateSessionToPort
    "4A": (1,),   # AcceptSession
    "4F": (3,),   # ReplyAndReceive (the reply target; the handles themselves are in memory)
}

# SVC -> {register of a handle it returns (r0 is the result): kind}. None: a handle of a kind
# the traces don't name (sessions, processes, resource limits): it is `?`. "dup": the object
# of the handle that was passed in.
OUTPUT_HANDLES: dict[str, dict[int, str | None]] = {
    "08": {1: "thread"},
    "13": {1: "mutex"},
    "15": {1: "semaphore"},
    "17": {1: "event"},
    "1A": {1: "timer"},
    "1E": {1: "memblock"},
    "21": {1: "arbiter"},
    "27": {1: "dup"},
    "2D": {1: "port"},
    "33": {1: None},                 # OpenProcess
    "34": {1: None},                 # OpenThread
    "38": {1: None},                 # GetResourceLimit
    "47": {1: "port", 2: "port"},    # CreatePort: server, client
    "48": {1: None},                 # CreateSessionToPort
    "49": {1: None, 2: None},        # CreateSession: server, client
    "4A": {1: None},                 # AcceptSession
}


@dataclass(frozen=True)
class Call:
    thread: str
    svc: str
    pc: str
    lr: str
    args: tuple[str, ...]                    # r0..r3 when the call was made
    ticks: int
    line: int
    in4: tuple[str, ...] | None = None       # r4..r6 when the call was made
    out: tuple[str, ...] | None = None       # r0..r3 when it completed; None: unknown

    def matches(self, other: "Call", with_args: bool = True, with_outputs: bool = False,
                with_in4: bool = False) -> bool:
        """Same call in the same place; optionally with the same arguments, the same extra
        arguments (r4-r6) and the same results. What one trace doesn't have is not compared."""
        if (self.svc, self.pc, self.lr) != (other.svc, other.pc, other.lr):
            return False
        if with_args and self.args != other.args:
            return False
        if with_in4 and None not in (self.in4, other.in4) and self.in4 != other.in4:
            return False
        if with_outputs and None not in (self.out, other.out) and self.out != other.out:
            return False
        return True

    def __str__(self) -> str:
        text = f"svc {self.svc} pc={self.pc} lr={self.lr} {' '.join(self.args)}"
        if self.in4 is not None:
            text += f" in4={','.join(self.in4)}"
        if self.out is not None:
            text += f" out={','.join(self.out)}"
        return f"{text} (line {self.line}, t={self.ticks})"


@dataclass(frozen=True)
class ThreadInfo:
    id: str
    parent: str
    entry: str
    arg: str = ""
    stack: str = ""
    prio: int = 0
    core_requested: int = 0
    core_resolved: int = 0


@dataclass
class Trace:
    header: dict[str, str] | None = None     # tool, version, date, and the key=value fields
    threads: dict[str, ThreadInfo] = field(default_factory=dict)   # in creation order
    calls: list[Call] = field(default_factory=list)                # in file order

    def by_thread(self) -> dict[str, list[Call]]:
        out: dict[str, list[Call]] = {}
        for c in self.calls:
            out.setdefault(c.thread, []).append(c)
        return out


_HEADER = re.compile(r"^#\s+(\S+)\s+v(\d+)\s+(\S+)(.*)$")


def _fields(tokens: list[str]) -> dict[str, str]:
    return dict(t.split("=", 1) for t in tokens if "=" in t)


def _int(text: str | None, default: int = 0) -> int:
    try:
        return int(text, 0) if text is not None else default
    except ValueError:
        return default


def _parse_thread_line(tokens: list[str]) -> ThreadInfo | None:
    """tokens after `# thread`: <id> parent=.. entry=.. arg=.. stack=.. prio=.. core=a/b"""
    if not tokens or "=" in tokens[0]:
        return None
    f = _fields(tokens[1:])
    req, _, res = f.get("core", "0/0").partition("/")
    return ThreadInfo(tokens[0], f.get("parent", "0"), f.get("entry", "").upper(), f.get("arg", "").upper(),
                      f.get("stack", "").upper(), _int(f.get("prio")), _int(req), _int(res, _int(req)))


def parse_lines(lines) -> Trace:
    trace = Trace()
    for n, line in enumerate(lines, 1):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "#":
            if len(parts) > 1 and parts[1] == "thread":
                info = _parse_thread_line(parts[2:])
                if info is not None:
                    trace.threads.setdefault(info.id, info)
            elif trace.header is None and (m := _HEADER.match(line.strip())):
                trace.header = {"tool": m.group(1), "version": m.group(2), "date": m.group(3),
                                **_fields(m.group(4).split())}
            continue
        if parts[0].startswith("#") or len(parts) < 9:
            continue
        in4 = out = None
        for extra in parts[9:]:
            if extra.startswith("in4="):
                in4 = tuple(extra[4:].upper().split(","))
            elif extra.startswith("out="):
                out = None if extra[4:] == "-" else tuple(extra[4:].upper().split(","))
        ticks = _int(parts[8][2:]) if parts[8].startswith("t=") else 0
        trace.calls.append(Call(parts[0], parts[1].upper(), parts[2][3:].upper(), parts[3][3:].upper(),
                                tuple(p.upper() for p in parts[4:8]), ticks, n, in4, out))
    return trace


def parse(path: str) -> Trace:
    with open(path, encoding="utf-8") as f:
        return parse_lines(f)


def thread_names(trace: Trace) -> dict[str, str]:
    """Thread number -> normalised name, from the thread lines."""
    names: dict[str, str] = {}
    per_entry: dict[str, int] = {}
    have_main = False
    for info in trace.threads.values():
        if info.parent == "0" and not have_main:
            names[info.id] = "main"
            have_main = True
        else:
            k = per_entry.get(info.entry, 0)
            per_entry[info.entry] = k + 1
            names[info.id] = f"{info.entry}#{k}"
    return names


class _Handles:
    """The handles a trace has created so far, by value."""

    def __init__(self) -> None:
        self.names: dict[str, str] = {}
        self.counts: dict[str, int] = {}

    def name(self, value: str) -> str:
        if value == OK or value in PSEUDO_HANDLES:
            return value
        return self.names.get(value, UNKNOWN_HANDLE)

    def create(self, value: str, kind: str | None) -> str:
        if kind is None:
            return UNKNOWN_HANDLE
        k = self.counts.get(kind, 0)
        self.counts[kind] = k + 1
        self.names[value] = f"{kind}#{k}"
        return self.names[value]


def _outputs_undefined(result: str) -> bool:
    """The call failed (bit 31) or timed out: it did not set its output registers."""
    try:
        return int(result, 16) & 0x80000000 != 0 or result == RESULT_TIMEOUT
    except ValueError:
        return False


def _normalise_call(c: Call, thread: str, handles: _Handles) -> Call:
    args = list(c.args)
    for reg in INPUT_HANDLES.get(c.svc, ()):
        args[reg] = handles.name(args[reg])
    out = None
    if c.out is not None:
        out = list(c.out)
        if c.svc == SVC_GET_SYSTEM_TICK:
            out = [DROPPED] * len(out)
        else:
            ok = out[0] == OK
            if _outputs_undefined(out[0]):
                out[1:] = [DROPPED] * (len(out) - 1)
            for reg, kind in OUTPUT_HANDLES.get(c.svc, {}).items():
                if reg >= len(out):
                    continue
                if not ok:
                    out[reg] = DROPPED
                elif kind == "dup":
                    source = handles.name(c.args[1])
                    if source not in (UNKNOWN_HANDLE, OK) and source not in PSEUDO_HANDLES:
                        handles.names[out[reg]] = source
                    out[reg] = source
                else:
                    out[reg] = handles.create(out[reg], kind)
            if c.svc == SVC_CLOSE_HANDLE and ok:
                handles.names.pop(c.args[0], None)
    return replace(c, thread=thread, args=tuple(args), ticks=0,
                   in4=c.in4, out=tuple(out) if out is not None else None)


def _svc_word(word: str) -> str:
    """An IPC trace word (0x%08x) in the SVC trace's style (8 uppercase digits)."""
    try:
        return f"{int(word, 16):08X}"
    except ValueError:
        return word


def _forget_moved_handles(call: "IpcCall", handles: _Handles) -> None:
    """The kernel closed the handles a request moved."""
    for d in call.desc:
        if d.kind == "move" and call.words_in is not None:
            for j in range(d.size):
                if d.index + 1 + j < len(call.words_in):
                    handles.names.pop(_svc_word(call.words_in[d.index + 1 + j]), None)


def _learn_reply_handles(call: "IpcCall", handles: _Handles) -> None:
    """The handles the reply handed out exist from now on, under the names the IPC trace gave them."""
    if call.words_out is None:
        return
    for d in call.outdesc:
        if d.kind not in IPC_HANDLE_KINDS:
            continue
        for j, name in enumerate(d.token.split("+")[:d.size]):
            at = d.index + 1 + j
            if name not in (IPC_NO_NAME, "0", UNKNOWN_HANDLE) and at < len(call.words_out):
                handles.names[_svc_word(call.words_out[at])] = name


def normalise(trace: Trace, ipc: "IpcTrace | None" = None) -> Trace:
    """A copy of the trace with threads and handles renamed and time-dependent values dropped
    (see the module docstring). The calls keep the file's order. `ipc` is the IPC trace of the
    same run, which names the handles that came in IPC replies."""
    names = thread_names(trace)
    handles = _Handles()
    queues: dict[str, deque] = {}
    if ipc is not None:
        for ic in sorted(ipc.calls, key=lambda x: x.seq):
            queues.setdefault(ic.thread, deque()).append(ic)
    calls = []
    for c in trace.calls:
        nc = _normalise_call(c, names.get(c.thread, c.thread), handles)
        calls.append(nc)
        queue = queues.get(c.thread)
        if (queue and c.svc == SVC_SEND_SYNC_REQUEST
                and not (c.out is not None and c.out[0] == RESULT_INVALID_HANDLE)):
            ic = queue.popleft()
            _forget_moved_handles(ic, handles)
            _learn_reply_handles(ic, handles)
    threads = {names[i]: replace(t, id=names[i], parent=names.get(t.parent, t.parent))
               for i, t in trace.threads.items()}
    return Trace(trace.header, threads, calls)


# ── IPC traces (N0-21) ──────────────────────────────────────────────────────────────

IPC_HANDLE_KINDS = ("copy", "move")
IPC_COMMAND_WORDS = 64        # the size of a thread's command buffer, in words
IPC_NO_NAME = "-"


@dataclass(frozen=True)
class IpcDesc:
    """One translate descriptor of a command: `<kind>:<index>:<size>:<token>`.

    kind: `static`, `mapped-r`, `mapped-w`, `mapped-rw`, `pxi` (buffers), `copy`, `move` (handles),
    `pid` (the calling process id). index: the word of the command the descriptor is in. size: the
    buffer's size in bytes, or the number of handles. token: a buffer's CRC-32 (`0x%08x`; `-` when it
    has none: a write-only mapped buffer in a request, a PXI buffer, one that could not be read), or
    the names of a handle descriptor's handles joined by `+` (`0` for a null handle, `-` for none)."""
    kind: str
    index: int
    size: int
    token: str = IPC_NO_NAME

    def __str__(self) -> str:
        return f"{self.kind}:{self.index}:{self.size}:{self.token}"


@dataclass(frozen=True)
class IpcCall:
    thread: str
    seq: int
    session: str
    cmd: str | None                           # the request's header word; None: never seen
    words_in: tuple[str, ...] | None          # every word of the request, as the client wrote it
    desc: tuple[IpcDesc, ...]
    words_out: tuple[str, ...] | None         # every word of the reply, as the client read it; None: no reply
    outdesc: tuple[IpcDesc, ...]
    ticks: int
    line: int
    data: tuple[tuple[int, str], ...] = ()    # (descriptor index, hex bytes) of the input buffers (--trace-ipc-full)

    @property
    def result(self) -> str | None:
        """The reply's result code (its first normal parameter), None without a reply."""
        if not self.words_out or len(self.words_out) < 2:
            return None
        try:
            normal = (int(self.words_out[0], 16) >> 6) & 0x3F
        except ValueError:
            return None
        return self.words_out[1] if normal >= 1 else None


@dataclass
class IpcTrace:
    header: dict[str, str] | None = None
    threads: dict[str, ThreadInfo] = field(default_factory=dict)
    calls: list[IpcCall] = field(default_factory=list)      # in file order (the order replies were recorded)

    def by_thread(self) -> dict[str, list[IpcCall]]:
        """Each thread's requests in the order they were sent."""
        out: dict[str, list[IpcCall]] = {}
        for c in self.calls:
            out.setdefault(c.thread, []).append(c)
        for calls in out.values():
            calls.sort(key=lambda x: x.seq)
        return out


def _word(text: str) -> str:
    try:
        return f"0x{int(text, 0) & 0xFFFFFFFF:08x}"
    except ValueError:
        return text


def _words(text: str | None) -> tuple[str, ...] | None:
    if text is None or text in ("", "-"):
        return None
    return tuple(_word(w) for w in text.split(","))


def _descs(text: str | None) -> tuple[IpcDesc, ...]:
    if text is None or text in ("", "-"):
        return ()
    out = []
    for item in text.split(","):
        parts = item.split(":", 3)           # the token of a handle holds service names, which have ':'
        if len(parts) < 3:
            continue
        try:
            desc = IpcDesc(parts[0], int(parts[1]), int(parts[2]), parts[3] if len(parts) > 3 else IPC_NO_NAME)
        except ValueError:
            continue
        # an index is a word of the 64-word command buffer and a handle descriptor holds at most that
        # many handles: anything else is a corrupt line, and would index or loop out of range
        if not 0 <= desc.index < IPC_COMMAND_WORDS or desc.size < 0:
            continue
        if desc.kind in IPC_HANDLE_KINDS and desc.size > IPC_COMMAND_WORDS:
            continue
        out.append(desc)
    return tuple(out)


def is_ipc_header(line: str) -> bool:
    m = _HEADER.match(line.strip())
    return bool(m) and m.group(1) == "ipctrace"


def parse_ipc_lines(lines) -> IpcTrace:
    trace = IpcTrace()
    for n, line in enumerate(lines, 1):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "#":
            if len(parts) > 1 and parts[1] == "thread":
                info = _parse_thread_line(parts[2:])
                if info is not None:
                    trace.threads.setdefault(info.id, info)
            elif trace.header is None and (m := _HEADER.match(line.strip())):
                trace.header = {"tool": m.group(1), "version": m.group(2), "date": m.group(3),
                                **_fields(m.group(4).split())}
            continue
        if parts[0].startswith("#"):
            continue
        f: dict[str, str] = {}
        data: list[tuple[int, str]] = []
        for token in parts[1:]:
            key, eq, value = token.partition("=")
            if not eq:
                continue
            if key == "data":
                index, _, hexbytes = value.partition(":")
                if _is_uint(index):
                    data.append((int(index), hexbytes))
            else:
                f[key] = value
        if "seq" not in f or not _is_uint(f["seq"]):
            continue
        cmd = f.get("cmd")
        trace.calls.append(IpcCall(
            parts[0], int(f["seq"]), f.get("session", "?"),
            None if cmd in (None, "-") else _word(cmd),
            _words(f.get("in")), _descs(f.get("desc")), _words(f.get("out")), _descs(f.get("outdesc")),
            _int(f.get("t")), n, tuple(data)))
    return trace


def _is_uint(text: str) -> bool:
    """Plain decimal digits, at most 18 (str.isdigit() also accepts superscripts and the like, and
    int() refuses more than 4300 digits)."""
    return text.isascii() and text.isdigit() and len(text) <= 18


def parse_ipc(path: str) -> IpcTrace:
    with open(path, encoding="utf-8") as f:
        return parse_ipc_lines(f)


def is_ipc_trace(path: str) -> bool:
    """Whether the file's first non-empty line is an IPC trace header."""
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                return is_ipc_header(line)
    return False


def _name_handle_words(words: tuple[str, ...] | None, descs: tuple[IpcDesc, ...]) -> tuple[str, ...] | None:
    """The words with the handles in them replaced by their names (`?` when the trace gave none;
    a null handle stays 0x00000000)."""
    if words is None:
        return None
    out = list(words)
    for d in descs:
        if d.kind not in IPC_HANDLE_KINDS:
            continue
        names = d.token.split("+")
        for j in range(d.size):
            at = d.index + 1 + j
            if at >= len(out):
                break
            name = names[j] if j < len(names) else IPC_NO_NAME
            if name == "0":
                out[at] = "0x00000000"
            else:
                out[at] = UNKNOWN_HANDLE if name in (IPC_NO_NAME, "") else name
    return tuple(out)


def normalise_ipc(trace: IpcTrace) -> IpcTrace:
    """A copy with threads renamed as `normalise` does, the handles in the words replaced by their
    names, and the times dropped. Sessions and descriptors already carry names."""
    names = thread_names(trace)
    calls = [replace(c, thread=names.get(c.thread, c.thread),
                     words_in=_name_handle_words(c.words_in, c.desc),
                     words_out=_name_handle_words(c.words_out, c.outdesc), ticks=0)
             for c in trace.calls]
    threads = {names[i]: replace(t, id=names[i], parent=names.get(t.parent, t.parent))
               for i, t in trace.threads.items()}
    return IpcTrace(trace.header, threads, calls)


def join_items(items) -> str:
    items = list(items)
    return ",".join(str(i) for i in items) if items else "-"


def format_ipc_call(c: IpcCall) -> str:
    text = (f"{c.thread} seq={c.seq} session={c.session} cmd={c.cmd or '-'} in={join_items(c.words_in or ())} "
            f"desc={join_items(c.desc)} out={join_items(c.words_out or ())} outdesc={join_items(c.outdesc)} t={c.ticks}")
    for index, hexbytes in c.data:
        text += f" data={index}:{hexbytes}"
    return text


def format_call(c: Call) -> str:
    text = f"{c.thread} {c.svc} pc={c.pc} lr={c.lr} {' '.join(c.args)} t={c.ticks}"
    if c.in4 is not None:
        text += f" in4={','.join(c.in4)}"
    if c.out is not None:
        text += f" out={','.join(c.out)}"
    return text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("trace")
    ap.add_argument("--normalize", action="store_true", help="rename threads and handles, drop times")
    ap.add_argument("--ipc", metavar="FILE", help="the IPC trace of the same run: names the handles of an SVC trace "
                                                  "that came in IPC replies (implies --normalize)")
    args = ap.parse_args(argv)
    if args.ipc:
        args.normalize = True          # the names only exist in a normalised SVC trace
    if is_ipc_trace(args.trace):
        ipc_trace = parse_ipc(args.trace)
        if args.normalize:
            ipc_trace = normalise_ipc(ipc_trace)
        for call in ipc_trace.calls:
            print(format_ipc_call(call))
        return 0
    trace = parse(args.trace)
    if args.normalize:
        trace = normalise(trace, parse_ipc(args.ipc) if args.ipc else None)
    for c in trace.calls:
        print(format_call(c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
