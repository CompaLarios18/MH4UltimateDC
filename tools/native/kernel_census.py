#!/usr/bin/env python3
"""Kernel census (N0-12): what the game asks of the kernel, in numbers.

Reads SVC traces (`mh4u_runner --trace-svc`) as streams, never whole, and IPC traces
(`--trace-ipc`), and reports, per trace:
  - threads: entry (named from config/symbols.txt when it can be), priority, requested and resolved
    core, parent, lifetime (calls and ticks), SVCs by count, what it waits on (by object kind and
    timeout class), its share of the trace;
  - SVCs: the distinct numbers in the trace and in the game's code (build/services.tsv), ControlMemory
    operations, permissions and sizes, the reset types of CreateEvent/CreateTimer, ArbitrateAddress
    types and timeouts, WaitSynchronization timeouts, SleepThread durations, the types asked of
    GetSystemInfo/GetProcessInfo/GetThreadInfo, and the GetSystemTick polling (per thread, per game
    frame and the longest run of consecutive calls);
  - APT: the SetAppCpuTimeLimit value;
  - IPC: per service, each command header, its count and its result codes.
The output holds counts, addresses and names only: no buffer contents.

A "game frame" is not in an SVC trace. The frame marker is one call of the main thread that the game
makes once per frame (`--frame-marker SVC:PC:LR`; the default is the SendSyncRequest in the render
thread's present path, 1699 calls in the first 1700 frames of G1). Other threads' calls are put in a
frame by their position in the file (a call is written when it completes).

usage: kernel_census.py --svc [LABEL=]TRACE... [--ipc [LABEL=]TRACE...] [--services build/services.tsv]
                        [--symbols config/symbols.txt] [--frame-marker SVC:PC:LR] [--out DIR]
       Labels default to the trace's directory name (G1/svc.txt -> G1). An IPC trace with the label of
       an SVC trace names the objects the SVC trace waits on. --out writes census.json and census.md.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, deque
from typing import Iterable

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tracefmt  # noqa: E402

SVC_NAMES = {
    0x01: "ControlMemory", 0x02: "QueryMemory", 0x03: "ExitProcess", 0x08: "CreateThread",
    0x09: "ExitThread", 0x0A: "SleepThread", 0x0B: "GetThreadPriority", 0x0C: "SetThreadPriority",
    0x0D: "GetThreadAffinityMask", 0x0E: "SetThreadAffinityMask", 0x0F: "GetThreadIdealProcessor",
    0x10: "SetThreadIdealProcessor", 0x11: "GetCurrentProcessorNumber", 0x13: "CreateMutex",
    0x14: "ReleaseMutex", 0x15: "CreateSemaphore", 0x16: "ReleaseSemaphore", 0x17: "CreateEvent",
    0x18: "SignalEvent", 0x19: "ClearEvent", 0x1A: "CreateTimer", 0x1B: "SetTimer", 0x1C: "CancelTimer",
    0x1D: "ClearTimer", 0x1E: "CreateMemoryBlock", 0x1F: "MapMemoryBlock", 0x20: "UnmapMemoryBlock",
    0x21: "CreateAddressArbiter", 0x22: "ArbitrateAddress", 0x23: "CloseHandle",
    0x24: "WaitSynchronization1", 0x25: "WaitSynchronizationN", 0x26: "SignalAndWait",
    0x27: "DuplicateHandle", 0x28: "GetSystemTick", 0x29: "GetHandleInfo", 0x2A: "GetSystemInfo",
    0x2B: "GetProcessInfo", 0x2C: "GetThreadInfo", 0x2D: "ConnectToPort", 0x32: "SendSyncRequest",
    0x33: "OpenProcess", 0x34: "OpenThread", 0x35: "GetProcessId", 0x36: "GetProcessIdOfThread",
    0x37: "GetThreadId", 0x38: "GetResourceLimit", 0x39: "GetResourceLimitLimitValues",
    0x3A: "GetResourceLimitCurrentValues", 0x3B: "GetThreadContext", 0x3C: "Break",
    0x3D: "OutputDebugString", 0x47: "CreatePort", 0x48: "CreateSessionToPort", 0x49: "CreateSession",
    0x4A: "AcceptSession", 0x4F: "ReplyAndReceive",
}
ARBITRATION_TYPES = {0: "Signal", 1: "WaitIfLess", 2: "DecrementAndWaitIfLess",
                     3: "WaitIfLessTimeout", 4: "DecrementAndWaitIfLessTimeout"}
RESET_TYPES = {0: "oneshot", 1: "sticky", 2: "pulse"}
ARBITRATION_WITH_TIMEOUT = (3, 4)
SLEEP_SHORT_NS = 1_000_000            # SleepThread: "under 1 ms" is below this
TICK_RUN_BUCKETS = (2, 8, 64)         # a run of consecutive GetSystemTick calls of at least this length
DEFAULT_FRAME_MARKER = "32:0056D968:00BF5E00"
APT_SET_CPU_TIME_LIMIT = "0x004f0080"
APT_GET_CPU_TIME_LIMIT = "0x00500040"
SVC_CONTROL_MEMORY, SVC_EXIT_THREAD, SVC_SLEEP = "01", "09", "0A"
SVC_SET_THREAD_PRIORITY, SVC_CREATE_MUTEX, SVC_CREATE_SEMAPHORE, SVC_CREATE_EVENT = "0C", "13", "15", "17"
SVC_CREATE_TIMER, SVC_ARBITRATE, SVC_CLOSE, SVC_WAIT1, SVC_WAITN = "1A", "22", "23", "24", "25"
SVC_TICK, SVC_DUP, SVC_SYSTEM_INFO, SVC_PROCESS_INFO, SVC_THREAD_INFO = "28", "27", "2A", "2B", "2C"
SVC_SEND, SVC_REPLY_RECEIVE, SVC_HANDLE_INFO = "32", "4F", "29"
HANDLE_KIND = {"13": "mutex", "15": "semaphore", "17": "event", "1A": "timer", "1E": "memblock",
               "21": "arbiter", "08": "thread", "2D": "port"}
HEX_TIMEOUT_INFINITE = 1 << 62        # a signed timeout at or above this is "infinite" (the console treats -1 as such)


def svc_name(number: str) -> str:
    try:
        return SVC_NAMES.get(int(number, 16), f"svc_{number.upper()}")
    except ValueError:
        return f"svc_{number}"


def _h(text: str) -> int:
    try:
        return int(text, 16)
    except ValueError:
        return 0


def _s64(low: int, high: int) -> int:
    value = (low & 0xFFFFFFFF) | ((high & 0xFFFFFFFF) << 32)
    return value - (1 << 64) if value & (1 << 63) else value


def timeout_class(ns: int) -> str:
    """0 (a poll), finite, or infinite (negative, or beyond what a clock reaches)."""
    if ns == 0:
        return "0"
    if ns < 0 or ns >= HEX_TIMEOUT_INFINITE:
        return "infinite"
    return "finite"


def sleep_class(ns: int) -> str:
    if ns <= 0:
        return "0"
    return "under 1 ms" if ns < SLEEP_SHORT_NS else "1 ms or more"


def load_symbols(path: str | None) -> dict[str, str]:
    """`0x<addr> <name> # note` lines -> {ADDR (8 uppercase hex digits): name}."""
    out: dict[str, str] = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            body = line.split("#", 1)[0].split()
            if len(body) >= 2 and body[0].lower().startswith("0x"):
                try:
                    out[f"{int(body[0], 16):08X}"] = body[1]
                except ValueError:
                    pass
    return out


def load_code_svcs(path: str | None) -> dict[str, dict]:
    """The SVC sites of the game's code from `find_services.py`'s services.tsv: {number: {name, sites, kinds}}."""
    out: dict[str, dict] = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        col = {name: i for i, name in enumerate(header)}
        for line in f:
            row = line.rstrip("\n").split("\t")
            if len(row) < len(header) or row[col["service"]] != "svc":
                continue
            try:
                number = f"{int(row[col['header']], 16):02X}"
            except ValueError:
                continue
            entry = out.setdefault(number, {"name": row[col["command"]], "sites": 0, "kinds": Counter()})
            entry["sites"] += 1
            entry["kinds"][row[col["note"]] if "note" in col else "-"] += 1
    return {k: {"name": v["name"], "sites": v["sites"], "kinds": dict(v["kinds"])} for k, v in sorted(out.items())}


class Thread:
    """What one thread did, accumulated while the trace streams by."""
    __slots__ = ("id", "name", "info", "calls", "svcs", "waits", "created_at", "first_call", "last_call",
                 "first_tick", "last_tick", "exited", "unfinished", "priorities", "frame_ticks", "tick_hist",
                 "run", "longest_run", "run_lens", "tick_sites", "started_frame", "ipc_queue", "creates")

    def __init__(self, tid: str, name: str, info, created_at: int) -> None:
        self.id, self.name, self.info, self.created_at = tid, name, info, created_at
        self.calls = 0
        self.svcs: Counter = Counter()
        self.waits: Counter = Counter()
        self.first_call = self.last_call = self.first_tick = self.last_tick = None
        self.exited = False
        self.unfinished = 0
        self.priorities: Counter = Counter()          # SetThreadPriority on this thread's own behalf: new value
        self.frame_ticks = 0
        self.tick_hist: Counter = Counter()           # GetSystemTick calls in a frame -> frames
        self.run = self.longest_run = 0
        self.run_lens: Counter = Counter()
        self.tick_sites: Counter = Counter()
        self.started_frame = 0
        self.ipc_queue: deque | None = None
        self.creates: Counter = Counter()


def _parse_marker(text: str) -> tuple[str, str, str]:
    parts = text.split(":")
    if len(parts) != 3:
        raise ValueError(f"frame marker must be SVC:PC:LR, got {text!r}")
    return (f"{int(parts[0], 16):02X}", "pc=" + f"{int(parts[1], 16):08X}", "lr=" + f"{int(parts[2], 16):08X}")


def _median(hist: Counter) -> float:
    total = sum(hist.values())
    if not total:
        return 0.0
    keys = sorted(hist)
    low, high = (total - 1) // 2, total // 2
    seen = 0
    values = []
    for k in keys:
        seen += hist[k]
        if seen > low and len(values) == 0:
            values.append(k)
        if seen > high:
            values.append(k)
            break
    return (values[0] + values[-1]) / 2


class SvcCensus:
    def __init__(self, symbols: dict[str, str] | None = None, ipc: "tracefmt.IpcTrace | None" = None,
                 frame_marker: str = DEFAULT_FRAME_MARKER) -> None:
        self.symbols = symbols or {}
        self.marker = _parse_marker(frame_marker)
        self.header: dict | None = None
        self.threads: dict[str, Thread] = {}
        self.entry_count: Counter = Counter()
        self.have_main = False
        self.main_id: str | None = None
        self.total = 0
        self.frames = 0
        self.svcs: Counter = Counter()
        self.handle_kind: dict[str, str] = {}
        self.control_memory: dict[str, Counter] = {"ops": Counter(), "perms": Counter(), "sizes": Counter()}
        self.control_memory_bytes: Counter = Counter()
        self.resets = {"event": Counter(), "timer": Counter()}
        self.mutex_initial: Counter = Counter()
        self.semaphores: Counter = Counter()
        self.arbitrate: Counter = Counter()          # "<type> <timeout class>"
        self.wait1: Counter = Counter()              # timeout class
        self.waitn: Counter = Counter()              # "<timeout class> count=<n> all=<0|1>"
        self.finite_timeouts: Counter = Counter()    # ns -> calls (WaitSynchronization)
        self.sleeps: Counter = Counter()
        self.sleep_values: Counter = Counter()
        self.info_types = {"GetSystemInfo": Counter(), "GetProcessInfo": Counter(), "GetThreadInfo": Counter(),
                           "GetHandleInfo": Counter()}
        self.results: Counter = Counter()            # result codes of the waits that timed out or failed
        self.timed_out = 0
        self.ipc_by_thread: dict[str, deque] = {}
        if ipc is not None:
            for c in sorted(ipc.calls, key=lambda x: x.seq):
                self.ipc_by_thread.setdefault(c.thread, deque()).append(c)

    # ── thread lines ──
    def _add_thread(self, info) -> None:
        if info.id in self.threads:
            return
        if info.parent == "0" and not self.have_main:
            name, self.have_main, self.main_id = "main", True, info.id
        else:
            k = self.entry_count[info.entry]
            self.entry_count[info.entry] = k + 1
            name = f"{info.entry}#{k}"
        th = Thread(info.id, name, info, self.total)
        th.started_frame = self.frames
        th.ipc_queue = self.ipc_by_thread.get(info.id)
        self.threads[info.id] = th

    def _thread(self, tid: str) -> Thread:
        th = self.threads.get(tid)
        if th is None:           # a thread with no thread line keeps its number
            th = Thread(tid, tid, None, self.total)
            th.started_frame = self.frames
            th.ipc_queue = self.ipc_by_thread.get(tid)
            self.threads[tid] = th
        return th

    def _name_of(self, tid: str) -> str:
        th = self.threads.get(tid)
        return th.name if th else tid

    # ── one line ──
    def feed(self, line: str) -> None:
        if not line.strip():
            return
        if line[0] == "#":
            parts = line.split()
            if len(parts) > 1 and parts[1] == "thread":
                info = tracefmt._parse_thread_line(parts[2:])
                if info is not None:
                    self._add_thread(info)
            elif self.header is None:
                m = tracefmt._HEADER.match(line.strip())
                if m:
                    self.header = {"tool": m.group(1), "version": m.group(2), "date": m.group(3),
                                   **tracefmt._fields(m.group(4).split())}
            return
        parts = line.split()
        if len(parts) < 9:
            return
        tid, svc = parts[0], parts[1].upper()
        th = self.threads.get(tid) or self._thread(tid)
        self.total += 1
        th.calls += 1
        th.svcs[svc] += 1
        self.svcs[svc] += 1
        if th.first_call is None:
            th.first_call = self.total
        th.last_call = self.total
        t = parts[8]
        if t.startswith("t="):
            try:
                ticks = int(t[2:])
            except ValueError:
                ticks = None
            if ticks is not None:
                if th.first_tick is None:
                    th.first_tick = ticks
                th.last_tick = ticks
        in4 = out = None
        for extra in parts[9:]:
            if extra.startswith("in4="):
                in4 = extra[4:].upper().split(",")
            elif extra.startswith("out="):
                out = None if extra[4:] == "-" else extra[4:].upper().split(",")
        if out is None:
            th.unfinished += 1

        # GetSystemTick runs and per-frame counts
        if svc == SVC_TICK:
            th.run += 1
            th.frame_ticks += 1
            th.tick_sites[(parts[2][3:].upper(), parts[3][3:].upper())] += 1
        elif th.run:
            th.run_lens[th.run] += 1
            if th.run > th.longest_run:
                th.longest_run = th.run
            th.run = 0
        if svc == self.marker[0] and parts[2] == self.marker[1] and parts[3] == self.marker[2] and tid == self.main_id:
            self._end_frame()

        self._classify(th, svc, parts, in4, out)

    def _end_frame(self) -> None:
        self.frames += 1
        for th in self.threads.values():
            if not th.exited:
                th.tick_hist[th.frame_ticks] += 1
            th.frame_ticks = 0

    def _classify(self, th: Thread, svc: str, parts: list[str], in4, out) -> None:
        a = [_h(x) for x in parts[4:8]]
        r4 = _h(in4[0]) if in4 else 0
        r5 = _h(in4[1]) if in4 and len(in4) > 1 else 0
        ok = out is not None and out[0] == tracefmt.OK
        if svc == SVC_WAIT1:
            cls = timeout_class(_s64(a[2], a[3]))
            kind = self.handle_kind.get(parts[4].upper(), "unknown")
            th.waits[f"WaitSynchronization1 {kind} timeout={cls}"] += 1
            self.wait1[cls] += 1
            if cls == "finite":
                self.finite_timeouts[_s64(a[2], a[3])] += 1
            self._wait_result(out)
        elif svc == SVC_WAITN:
            ns = _s64(a[0], r4)
            cls = timeout_class(ns)
            key = f"timeout={cls} count={a[2]} wait_all={a[3] & 1}"
            th.waits[f"WaitSynchronizationN {key}"] += 1
            self.waitn[key] += 1
            if cls == "finite":
                self.finite_timeouts[ns] += 1
            self._wait_result(out)
        elif svc == SVC_REPLY_RECEIVE:
            th.waits[f"ReplyAndReceive handles={a[2]} reply={'yes' if a[3] else 'no'}"] += 1
        elif svc == SVC_SLEEP:
            ns = _s64(a[0], a[1])
            cls = sleep_class(ns)
            th.waits[f"SleepThread {cls}"] += 1
            self.sleeps[cls] += 1
            self.sleep_values[ns] += 1
        elif svc == SVC_ARBITRATE:
            typ = a[2]
            name = ARBITRATION_TYPES.get(typ, f"type{typ}")
            cls = timeout_class(_s64(r4, r5)) if typ in ARBITRATION_WITH_TIMEOUT else "n/a"
            self.arbitrate[f"{typ} {name} timeout={cls}"] += 1
            if typ != 0:
                th.waits[f"ArbitrateAddress {name} timeout={cls}"] += 1
            else:
                th.creates["ArbitrateAddress Signal"] += 1
        elif svc == SVC_SEND:
            if out is not None and out[0] == tracefmt.RESULT_INVALID_HANDLE:
                th.waits["SendSyncRequest invalid-handle"] += 1
            else:
                service = "unknown"
                queue = th.ipc_queue
                if queue and out is not None:
                    ic = queue.popleft()
                    service = tracefmt.instance_free(ic.session)
                    self._ipc_handles(ic)
                th.waits[f"SendSyncRequest {service}"] += 1
        elif svc == SVC_CONTROL_MEMORY:
            op, size = a[0], a[3]
            label = f"op={op & 0xFF:#04x} linear={'yes' if op & 0x10000 else 'no'}"
            self.control_memory["ops"][label] += 1
            self.control_memory["perms"][f"{r4:#x}"] += 1
            self.control_memory["sizes"][f"{label} size={size:#x}"] += 1
            self.control_memory_bytes[label] += size
        elif svc == SVC_CREATE_EVENT:
            self.resets["event"][RESET_TYPES.get(a[1], str(a[1]))] += 1
            self._created(svc, out)
        elif svc == SVC_CREATE_TIMER:
            self.resets["timer"][RESET_TYPES.get(a[1], str(a[1]))] += 1
            self._created(svc, out)
        elif svc == SVC_CREATE_MUTEX:
            self.mutex_initial["locked" if a[1] & 0xFF else "free"] += 1
            self._created(svc, out)
        elif svc == SVC_CREATE_SEMAPHORE:
            self.semaphores[f"initial={a[1]} max={a[2]}"] += 1
            self._created(svc, out)
        elif svc == SVC_SYSTEM_INFO:
            self.info_types["GetSystemInfo"][f"type={a[1]:#x} param={a[2]:#x}"] += 1
        elif svc == SVC_PROCESS_INFO:
            self.info_types["GetProcessInfo"][f"type={a[2]:#x}"] += 1
        elif svc == SVC_HANDLE_INFO:
            self.info_types["GetHandleInfo"][f"type={a[2]:#x}"] += 1
        elif svc == SVC_THREAD_INFO:
            self.info_types["GetThreadInfo"][f"type={a[2]:#x}"] += 1
        elif svc == SVC_SET_THREAD_PRIORITY:
            th.priorities[a[1]] += 1
        elif svc == SVC_EXIT_THREAD:
            th.exited = True
        elif svc == SVC_CLOSE:
            if ok:
                self.handle_kind.pop(parts[4].upper(), None)
        elif svc in HANDLE_KIND:
            self._created(svc, out)
        elif svc == SVC_DUP and ok:
            kind = self.handle_kind.get(parts[5].upper())
            if kind:
                self.handle_kind[out[1]] = kind

    def _created(self, svc: str, out) -> None:
        if out is not None and out[0] == tracefmt.OK and len(out) > 1:
            self.handle_kind[out[1]] = HANDLE_KIND[svc]

    def _wait_result(self, out) -> None:
        if out is not None:
            if out[0] == tracefmt.RESULT_TIMEOUT:
                self.timed_out += 1
            elif out[0] != tracefmt.OK:
                self.results[out[0]] += 1

    def _ipc_handles(self, ic) -> None:
        """Handles a request moved are gone; the ones its reply handed out exist, kinded by their IPC name."""
        for d in ic.desc:
            if d.kind == "move" and ic.words_in is not None:
                for j in range(d.size):
                    if d.index + 1 + j < len(ic.words_in):
                        self.handle_kind.pop(tracefmt._svc_word(ic.words_in[d.index + 1 + j]), None)
        if ic.words_out is None:
            return
        for d in ic.outdesc:
            if d.kind not in tracefmt.IPC_HANDLE_KINDS:
                continue
            for j, name in enumerate(d.token.split("+")[:d.size]):
                at = d.index + 1 + j
                if name in (tracefmt.IPC_NO_NAME, "0", "?") or at >= len(ic.words_out):
                    continue
                kind = name.split("/")[-1].split("#")[0] if "/" in name else "ipc"
                self.handle_kind[tracefmt._svc_word(ic.words_out[at])] = kind

    # ── result ──
    def report(self, code_svcs: dict[str, dict] | None = None) -> dict:
        for th in self.threads.values():     # close the runs still open
            if th.run:
                th.run_lens[th.run] += 1
                th.longest_run = max(th.longest_run, th.run)
                th.run = 0
        threads = []
        for th in self.threads.values():
            info = th.info
            entry = info.entry if info else ""
            ticks_total = sum(k * v for k, v in th.tick_hist.items()) + th.frame_ticks
            frames = sum(th.tick_hist.values())
            runs = th.run_lens
            tick_calls = th.svcs.get(SVC_TICK, 0)
            threads.append({
                "name": th.name, "id": th.id, "entry": entry or None,
                "entry_symbol": self.symbols.get(entry) if entry else None,
                "priority": info.prio if info else None,
                "core_requested": info.core_requested if info else None,
                "core_resolved": info.core_resolved if info else None,
                "parent": self._name_of(info.parent) if info and info.parent != "0" else None,
                "created_after_call": th.created_at, "first_call": th.first_call, "last_call": th.last_call,
                "first_tick": th.first_tick, "last_tick": th.last_tick,
                "exited": th.exited, "unfinished_calls": th.unfinished,
                "calls": th.calls, "share": round(th.calls / self.total, 6) if self.total else 0.0,
                "svcs": {svc_name(k) + f" ({k})": v for k, v in th.svcs.most_common()},
                "waits": dict(th.waits.most_common()),
                "priority_changes": {str(k): v for k, v in sorted(th.priorities.items())},
                "get_system_tick": {
                    "calls": tick_calls, "frames_measured": frames,
                    "per_frame_median": _median(th.tick_hist), "per_frame_max": max(th.tick_hist, default=0),
                    "longest_run": th.longest_run,
                    "calls_in_runs_of_at_least": {str(n): sum(k * v for k, v in runs.items() if k >= n)
                                                   for n in TICK_RUN_BUCKETS},
                    "top_sites": [{"pc": pc, "lr": lr, "calls": n} for (pc, lr), n in th.tick_sites.most_common(4)],
                } if tick_calls else None,
            })
        trace_svcs = {k: {"name": svc_name(k), "calls": v} for k, v in sorted(self.svcs.items())}
        code = code_svcs or {}
        return {
            "header": self.header, "calls": self.total, "frames": self.frames,
            "frame_marker": "{}:{}:{}".format(self.marker[0], self.marker[1][3:], self.marker[2][3:]),
            "threads": threads,
            "svcs": {
                "in_trace": trace_svcs,
                "in_code": code,
                "in_code_not_in_trace": sorted(set(code) - set(trace_svcs)),
                "in_trace_not_in_code": sorted(set(trace_svcs) - set(code)) if code else [],
            },
            "control_memory": {"operations": dict(self.control_memory["ops"].most_common()),
                               "permissions": dict(self.control_memory["perms"].most_common()),
                               "bytes_by_operation": dict(self.control_memory_bytes),
                               "sizes_distinct": len(self.control_memory["sizes"]),
                               "sizes_top": dict(self.control_memory["sizes"].most_common(16))},
            "reset_types": {k: dict(v) for k, v in self.resets.items()},
            "mutex_initial": dict(self.mutex_initial), "semaphores": dict(self.semaphores.most_common()),
            "arbitrate_address": dict(self.arbitrate.most_common()),
            "wait_synchronization1_timeouts": dict(self.wait1),
            "wait_synchronization_n": dict(self.waitn.most_common()),
            "finite_wait_timeouts_ns": {str(k): v for k, v in self.finite_timeouts.most_common(8)},
            "waits_timed_out": self.timed_out, "wait_other_results": dict(self.results),
            "sleep_thread": {"classes": dict(self.sleeps),
                             "values_ns_top": {str(k): v for k, v in self.sleep_values.most_common(8)}},
            "info_types": {k: dict(v.most_common()) for k, v in self.info_types.items()},
        }


def scan_svc(path: str, symbols: dict[str, str] | None = None, ipc: "tracefmt.IpcTrace | None" = None,
             frame_marker: str = DEFAULT_FRAME_MARKER) -> SvcCensus:
    census = SvcCensus(symbols, ipc, frame_marker)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            census.feed(line)
    return census


def ipc_report(trace: "tracefmt.IpcTrace") -> dict:
    """Per service: each request header with its count and result codes; and the APT CPU-time limit."""
    services: dict[str, dict[str, dict]] = {}
    limit: list[dict] = []
    for c in trace.calls:
        service = tracefmt.instance_free(c.session)
        entry = services.setdefault(service, {}).setdefault(c.cmd or "-", {"count": 0, "results": Counter()})
        entry["count"] += 1
        entry["results"][c.result or "no-reply"] += 1
        if service.startswith("APT") and c.cmd == APT_SET_CPU_TIME_LIMIT and c.words_in and len(c.words_in) >= 3:
            limit.append({"thread": c.thread, "seq": c.seq, "word1": c.words_in[1], "percent": int(c.words_in[2], 16),
                          "result": c.result})
    out = {svc: {cmd: {"count": v["count"], "results": dict(v["results"].most_common())}
                 for cmd, v in sorted(cmds.items())} for svc, cmds in sorted(services.items())}
    get_limit = sum(v["count"] for s, cmds in services.items() if s.startswith("APT")
                    for k, v in cmds.items() if k == APT_GET_CPU_TIME_LIMIT)
    return {"header": trace.header, "requests": len(trace.calls), "services": out,
            "apt_set_cpu_time_limit": limit, "apt_get_cpu_time_limit_calls": get_limit}


# ── command line, markdown ──

def split_label(arg: str) -> tuple[str, str]:
    if "=" in arg and not os.path.exists(arg):
        label, _, path = arg.partition("=")
        return label, path
    base = os.path.basename(arg)
    stem = os.path.splitext(base)[0]
    parent = os.path.basename(os.path.dirname(os.path.abspath(arg)))
    return (parent if stem in ("svc", "ipc") and parent else stem), arg


def _table(rows: Iterable[Iterable], header: list[str]) -> list[str]:
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(str(c) for c in row) + " |")
    return lines + [""]


def render_markdown(report: dict) -> str:
    out = ["# Kernel census", "",
           "Counts, addresses and names only. Generated by `tools/native/kernel_census.py`.", ""]
    for label, r in report["svc_traces"].items():
        out += [f"## {label}: {r['calls']:,} SVC calls, {r['frames']:,} frames (marker `{r['frame_marker']}`)", ""]
        out += ["### Threads", ""]
        rows = []
        for t in r["threads"]:
            sym = f" ({t['entry_symbol']})" if t["entry_symbol"] else ""
            rows.append([t["name"], f"`{t['entry']}`{sym}" if t["entry"] else "-", t["priority"],
                         f"{t['core_requested']}/{t['core_resolved']}", t["parent"] or "-", t["calls"],
                         f"{t['share'] * 100:.2f}%", "exit" if t["exited"] else "alive"])
        out += _table(rows, ["thread", "entry", "prio", "core req/res", "parent", "calls", "share", "end"])
        out += ["### What each thread waits on (top 6)", ""]
        out += _table([[t["name"], "; ".join(f"{k} x{v:,}" for k, v in list(t["waits"].items())[:6]) or "-"]
                       for t in r["threads"]], ["thread", "waits"])
        out += ["### SVC numbers", ""]
        s = r["svcs"]
        out += _table([[f"0x{k}", v["name"], f"{v['calls']:,}", s["in_code"].get(k, {}).get("sites", "-")]
                       for k, v in s["in_trace"].items()], ["svc", "name", "calls", "sites in code"])
        out += [f"In the code but not in this trace: {', '.join('0x' + k for k in s['in_code_not_in_trace']) or '-'}. "
                f"In the trace but not in the code: {', '.join('0x' + k for k in s['in_trace_not_in_code']) or '-'}.", ""]
        cm = r["control_memory"]
        out += ["### ControlMemory, resets, waits, sleeps", ""]
        out += _table([[k, v, f"{cm['bytes_by_operation'].get(k, 0):#x}"] for k, v in cm["operations"].items()],
                      ["operation", "calls", "bytes"])
        out += [f"Permissions (r4): {cm['permissions']}. Distinct sizes: {cm['sizes_distinct']}.", ""]
        out += [f"Reset types: {r['reset_types']}. Mutex initial: {r['mutex_initial']}. Semaphores: {r['semaphores']}.", ""]
        out += _table([[k, v] for k, v in r["arbitrate_address"].items()], ["ArbitrateAddress", "calls"])
        out += [f"WaitSynchronization1 timeouts: {r['wait_synchronization1_timeouts']}. "
                f"WaitSynchronizationN: {r['wait_synchronization_n']}. Waits that timed out: {r['waits_timed_out']}.", "",
                f"SleepThread: {r['sleep_thread']['classes']}. Info types: {r['info_types']}.", ""]
        out += ["### GetSystemTick", ""]
        out += _table([[t["name"], g["calls"], g["per_frame_median"], g["per_frame_max"], g["longest_run"],
                        g["calls_in_runs_of_at_least"]["8"]]
                       for t in r["threads"] if (g := t["get_system_tick"])],
                      ["thread", "calls", "per frame median", "per frame max", "longest run", "calls in runs of 8+"])
    if report.get("ipc"):
        for label, r in report["ipc"].items():
            out += [f"## IPC {label}: {r['requests']:,} requests", ""]
            out += [f"APT SetAppCpuTimeLimit ({APT_SET_CPU_TIME_LIMIT}): {r['apt_set_cpu_time_limit'] or 'not seen'}", ""]
            for service, cmds in r["services"].items():
                out += [f"### {service}", ""]
                out += _table([[f"`{cmd}`", v["count"], ", ".join(f"{k} x{n}" for k, n in v["results"].items())]
                               for cmd, v in cmds.items()], ["header", "count", "results"])
    return "\n".join(out)


def build_report(svc_args: list[str], ipc_args: list[str], services: str | None, symbols: str | None,
                 frame_marker: str) -> dict:
    syms = load_symbols(symbols)
    code = load_code_svcs(services)
    ipc_traces = {}
    for arg in ipc_args:
        label, path = split_label(arg)
        ipc_traces[label] = tracefmt.parse_ipc(path)
    report: dict = {"svc_traces": {}, "ipc": {}}
    for arg in svc_args:
        label, path = split_label(arg)
        census = scan_svc(path, syms, ipc_traces.get(label), frame_marker)
        report["svc_traces"][label] = census.report(code)
    for label, trace in ipc_traces.items():
        report["ipc"][label] = ipc_report(trace)
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--svc", nargs="+", default=[], metavar="TRACE")
    ap.add_argument("--ipc", nargs="+", default=[], metavar="TRACE")
    ap.add_argument("--services", metavar="TSV", default="build/services.tsv")
    ap.add_argument("--symbols", metavar="FILE", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                      "..", "..", "config", "symbols.txt"))
    ap.add_argument("--frame-marker", default=DEFAULT_FRAME_MARKER, metavar="SVC:PC:LR")
    ap.add_argument("--out", metavar="DIR", help="write census.json and census.md here")
    args = ap.parse_args(argv)
    if not args.svc and not args.ipc:
        ap.error("give at least one --svc or --ipc trace")
    for arg in args.svc + args.ipc:
        path = split_label(arg)[1]
        if not os.path.exists(path):
            print(f"kernel_census: no such file: {path}", file=sys.stderr)
            return 2
    try:
        report = build_report(args.svc, args.ipc, args.services, args.symbols, args.frame_marker)
    except ValueError as e:
        print(f"kernel_census: {e}", file=sys.stderr)
        return 2
    text = render_markdown(report)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "census.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(report, f, indent=1)
            f.write("\n")
        with open(os.path.join(args.out, "census.md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
