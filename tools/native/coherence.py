#!/usr/bin/env python3
# Derived from Azahar src/video_core/gpu.cpp, src/core/memory.h, src/core/hle/service/gsp/gsp_command.h and
# src/video_core/pica/regs_external.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: every one of
# those files says "Licensed under GPLv2 or any later version"): the fixed virtual-to-physical mapping of
# GPU::VirtualToPhysicalAddress and the constants of the windows it uses (memory.h), the arguments of the GX commands
# (gsp_command.h), and the pixel formats and the input size fields of a display transfer (regs_external.h).
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h)
# Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
# Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
"""Flat-memory coherence report (N0-14): does the game depend on cache coherence between CPU and GPU?

Principle 2 of the native port (docs/native/ROADMAP.md) says the game's memory can be one flat block that the
GPU reads and writes in place, with no copy and no cache flush.  Two questions test it
(docs/research/flat-memory-check.md has the method, the commands and the results):

(a) from an MGPU capture, how often does the GPU use data the CPU changed with no cache flush covering it?

        python3 tools/native/coherence.py capture FILE.mgpu [FILE2.mgpu ...] [--json OUT] [--md OUT] [--top N]

    For every GX command (and the other events a CPU_PAGES record precedes: MMIO, SWAP, FILL): the pages the
    CPU changed since the previous command, how many bytes of them changed, and how many of those bytes a FLUSH
    record covers, per kind of flush.

(b) from the log of `mh4u_runner --check-coherence FILE`, does the CPU ever read what the GPU wrote?

        python3 tools/native/coherence.py runlog FILE [--json OUT] [--md OUT] [--top N]

Captures and logs are game data and live under build/; the reports hold counts and addresses, never contents.
Standard library only.
"""
from __future__ import annotations

import argparse
import bisect
import json
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Iterable, Iterator, Sequence

import gcap

PAGE = gcap.PAGE_SIZE

# Memory.h: the windows of virtual memory the GPU's fixed mapping knows (GPU::VirtualToPhysicalAddress).
VRAM_PADDR = 0x18000000
VRAM_SIZE = 0x00600000
FCRAM_PADDR = 0x20000000
VRAM_VADDR = 0x1F000000
LINEAR_HEAP_VADDR = 0x14000000
LINEAR_HEAP_SIZE = 0x08000000
NEW_LINEAR_HEAP_VADDR = 0x30000000
NEW_LINEAR_HEAP_SIZE = 0x10000000
WINDOWS = ((VRAM_VADDR, VRAM_SIZE, VRAM_PADDR),
           (LINEAR_HEAP_VADDR, LINEAR_HEAP_SIZE, FCRAM_PADDR),
           (NEW_LINEAR_HEAP_VADDR, NEW_LINEAR_HEAP_SIZE, FCRAM_PADDR))

# regs_external.h: PixelFormat of a display transfer and its bytes per pixel.
BYTES_PER_PIXEL = {0: 4, 1: 3, 2: 2, 3: 2, 4: 2}  # RGBA8, RGB8, RGB565, RGB5A1, RGBA4

FLUSH_KINDS = (1, 2, 3, 4)
# Principle 2 names FlushDataCache and FlushCacheRegions (the regions of a CacheFlush GX command).
P2_KINDS = (1, 4)
EVENT_ROWS = {gcap.GX: None, gcap.MMIO: "MMIO", gcap.SWAP: "SWAP", gcap.FILL: "FILL"}
FIRST_ROW = "(first interval)"

LOAD_KINDS = ("Read", "BlockRead", "CopySource")
STORE_KINDS = ("Write", "WriteExclusive", "BlockWrite", "ZeroBlock")


def virtual_to_physical(vaddr: int) -> int | None:
    """GPU::VirtualToPhysicalAddress for the addresses it converts; None for the ones it passes through."""
    if vaddr == 0:
        return None
    for base, size, paddr in WINDOWS:
        if base <= vaddr <= base + size:  # Azahar's tests are inclusive of the end
            return vaddr - base + paddr
    return None


def physical_range(vaddr: int, size: int) -> tuple[int, int] | None:
    """The physical byte range [lo, hi) of a virtual one, clipped to its window; None if it maps nowhere."""
    for base, wsize, paddr in WINDOWS:
        if base <= vaddr <= base + wsize:
            end = min(vaddr + size, base + wsize)
            if end <= vaddr:
                return None
            return vaddr - base + paddr, end - base + paddr
    return None


def named_input(words: Sequence[int]) -> tuple[int, int] | None:
    """The bytes a GX command reads that its own words name, as a virtual (address, size); None if none.

    SubmitCmdList reads the command list buffer; RequestDma its source; TextureCopy `size` bytes from its input
    (an upper bound: the line gaps are ignored); DisplayTransfer width x height x bytes per pixel from its
    input.  A command list also reads the textures, vertices and shaders its register writes point at, and a
    memory fill reads nothing: neither is named by the words, so they are not in this subset (an approximation).
    """
    cid = words[0] & 0xFF
    if cid == 0:
        return words[1], words[3]
    if cid == 1:
        return words[1], words[2]
    if cid == 3:
        width, height = words[3] & 0xFFFF, words[3] >> 16
        bpp = BYTES_PER_PIXEL.get((words[5] >> 8) & 7, 4)
        return words[1], width * height * bpp
    if cid == 4:
        return words[1], words[3]
    return None


# --------------------------------------------------------------------------
# (a) from a capture

def _merge(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return out


class _Coverage:
    """The flush ranges of one interval, merged per set of flush kinds, for page lookups."""

    SETS = {"flush": (1,), "invalidate": (2,), "store": (3,), "region": (4,), "any": FLUSH_KINDS, "p2": P2_KINDS}

    def __init__(self, flushes: Sequence[tuple[int, int, int]]) -> None:
        self.ranges: dict[str, list[tuple[int, int]]] = {}
        self.starts: dict[str, list[int]] = {}
        for name, kinds in self.SETS.items():
            merged = _merge([(lo, hi) for kind, lo, hi in flushes if kind in kinds])
            self.ranges[name] = merged
            self.starts[name] = [lo for lo, _ in merged]

    def covered(self, name: str, page: int, changed: bytes) -> int:
        """How many of the changed bytes (nonzero in CHANGED, a page of XOR) the flushes of that set cover."""
        ranges, starts = self.ranges[name], self.starts[name]
        i = max(bisect.bisect_right(starts, page) - 1, 0)
        total = 0
        while i < len(ranges) and ranges[i][0] < page + PAGE:
            lo, hi = max(ranges[i][0], page) - page, min(ranges[i][1], page + PAGE) - page
            if hi > lo:
                total += (hi - lo) - changed.count(0, lo, hi)
            i += 1
        return total


def _new_row() -> dict:
    row = {"events": 0, "events_with_cpu_pages": 0, "pages": 0, "changed_bytes": 0, "unchanged_pages": 0,
           "covered_bytes": {k: 0 for k in _Coverage.SETS}}
    for k in ("any", "p2"):
        row[f"pages_full_{k}"] = row[f"pages_partial_{k}"] = row[f"pages_none_{k}"] = 0
    row["named_pages"] = 0
    row["named_not_covered_any"] = row["named_not_covered_p2"] = 0
    return row


@dataclass
class _Open:
    """A GX/MMIO/SWAP/FILL event whose tail (the kind-4 flushes of a CacheFlush) is still being read."""
    row: str
    pages: list[tuple[int, bytes]]
    before: list[tuple[int, int, int]]
    own: list[tuple[int, int, int]] = field(default_factory=list)
    words: Sequence[int] | None = None


def analyse_capture(records: Iterable[gcap.Record], top: int = 10) -> dict:
    """Stream a capture and report the CPU changes before each event against the flushes.

    A CPU_PAGES record is taken before the next GPU event, so a page in it changed at some point after the
    previous diff (the end of the previous command).  It counts as covered when a FLUSH recorded after that
    point and before the event covers it (for a CacheFlush command, its own regions count too).  A change made
    after the flush within the same interval is counted as covered: the capture cannot tell.  The interval before
    the first event follows MEM, with no earlier flush visible, and is reported as a row of its own.
    """
    mem = gcap.Memory()
    rows: dict[str, dict] = {}
    flush_counts: Counter = Counter()
    flush_unmapped: Counter = Counter()
    interval: list[tuple[int, int, int]] = []   # (kind, lo, hi) physical, since the last diff point
    pending: list[tuple[int, bytes]] | None = None
    pending_before: list[tuple[int, int, int]] = []
    open_event: _Open | None = None
    events_seen = 0
    first_event = None
    not_covered: dict[str, list[int]] = defaultdict(list)
    records_seen = Counter()

    def close() -> None:
        nonlocal open_event, interval
        ev = open_event
        if ev is None:
            return
        open_event = None
        row = rows.setdefault(ev.row, _new_row())
        row["events"] += 1
        if ev.pages:
            row["events_with_cpu_pages"] += 1
        cov = _Coverage(ev.before + ev.own)
        named = None
        if ev.words is not None:
            n = named_input(ev.words)
            named = physical_range(*n) if n and n[1] > 0 else None
        for addr, data in ev.pages:
            prev = mem.read(addr, PAGE)
            diff = (int.from_bytes(prev, "little") ^ int.from_bytes(data, "little")).to_bytes(PAGE, "little")
            changed = PAGE - diff.count(0)
            mem.write(addr, data)
            row["pages"] += 1
            if changed == 0:
                row["unchanged_pages"] += 1
                continue
            row["changed_bytes"] += changed
            full_any = False
            for name in _Coverage.SETS:
                got = cov.covered(name, addr, diff)
                row["covered_bytes"][name] += got
                if name in ("any", "p2"):
                    status = "full" if got == changed else "none" if got == 0 else "partial"
                    row[f"pages_{status}_{name}"] += 1
                    if name == "any":
                        full_any = status == "full"
                        if not full_any:
                            not_covered[ev.row].append(addr)
                    if named and addr < named[1] and addr + PAGE > named[0]:
                        if name == "any":
                            row["named_pages"] += 1
                        if status != "full":
                            row[f"named_not_covered_{name}"] += 1
        interval = []

    for rec in records:
        records_seen[rec.name] += 1
        t = rec.type
        if open_event is not None:
            if t == gcap.FLUSH and open_event.words is not None and struct.unpack_from("<I", rec.payload)[0] == 4:
                kind, vaddr, size = struct.unpack("<III", rec.payload)
                span = physical_range(vaddr, size)
                flush_counts[kind] += 1
                if span is None:
                    flush_unmapped[kind] += 1
                else:
                    open_event.own.append((kind, span[0], span[1]))
                continue
            close()
        if t == gcap.MEM:
            addr, data = gcap.parse_mem(rec.payload)
            mem.write(addr, data)
        elif t == gcap.GPU_PAGES:
            for addr, data in gcap.parse_pages(rec.payload, rec.name):
                mem.write(addr, data)
        elif t == gcap.CPU_PAGES:
            pending = gcap.parse_pages(rec.payload)
            pending_before = list(interval)
        elif t == gcap.FLUSH:
            kind, vaddr, size = struct.unpack("<III", rec.payload)
            span = physical_range(vaddr, size)
            flush_counts[kind] += 1
            if span is None:
                flush_unmapped[kind] += 1
            else:
                interval.append((kind, span[0], span[1]))
        elif t in EVENT_ROWS:
            words = None
            if t == gcap.GX:
                cid, words = gcap.parse_gx(rec.payload)
                name = gcap.GX_NAMES.get(cid, f"id{cid}")
            else:
                name = EVENT_ROWS[t]
            events_seen += 1
            if events_seen == 1:
                first_event = name
                name = FIRST_ROW
            open_event = _Open(name, pending or [], pending_before if pending else list(interval), [], words)
            pending = None
            pending_before = []
    close()

    total = _new_row()
    for row in rows.values():
        for key, value in row.items():
            if key == "covered_bytes":
                for name, n in value.items():
                    total[key][name] += n
            else:
                total[key] += value
    ranges = {}
    for name, pages in not_covered.items():
        grouped = group_pages(sorted(pages))
        ranges[name] = {"pages": len(pages), "ranges": len(grouped),
                        "largest": [{"paddr": lo, "pages": n} for lo, n in
                                    sorted(grouped, key=lambda r: -r[1])[:top]]}
    return {
        "records": dict(records_seen),
        "events": events_seen,
        "first_event": first_event,
        "flushes": {gcap.FLUSH_NAMES[k]: {"count": flush_counts[k], "maps_nowhere": flush_unmapped[k]}
                    for k in FLUSH_KINDS},
        "rows": rows,
        "total": total,
        "not_fully_covered": ranges,
    }


def group_pages(pages: Sequence[int]) -> list[tuple[int, int]]:
    """Ascending page addresses as (first address, page count) runs of consecutive pages."""
    out: list[tuple[int, int]] = []
    for p in pages:
        if out and p == out[-1][0] + out[-1][1] * PAGE:
            out[-1] = (out[-1][0], out[-1][1] + 1)
        else:
            out.append((p, 1))
    return out


def pct(part: int, whole: int) -> str:
    return f"{100.0 * part / whole:.1f}%" if whole else "-"


def format_capture(label: str, rep: dict) -> str:
    out = [f"## {label}", ""]
    out.append(f"events: {rep['events']} (the first is a {rep['first_event']}); flush records: " + ", ".join(
        f"{k} {v['count']} ({v['maps_nowhere']} map nowhere)" for k, v in rep["flushes"].items()))
    out.append("")
    head = ("| row | events | CPU pages | changed bytes | covered (any) | covered (flush+region) | "
            "none / partial / full (any) | none / partial / full (flush+region) | named pages not fully covered |")
    out += [head, "|" + "---|" * 9]
    for name, row in list(rep["rows"].items()) + [("total", rep["total"])]:
        cb = row["covered_bytes"]
        out.append(
            f"| {name} | {row['events']} | {row['pages']} | {row['changed_bytes']} | "
            f"{cb['any']} ({pct(cb['any'], row['changed_bytes'])}) | {cb['p2']} ({pct(cb['p2'], row['changed_bytes'])}) | "
            f"{row['pages_none_any']} / {row['pages_partial_any']} / {row['pages_full_any']} | "
            f"{row['pages_none_p2']} / {row['pages_partial_p2']} / {row['pages_full_p2']} | "
            f"{row['named_not_covered_any']} of {row['named_pages']} |")
    out += ["", "changed bytes covered, per kind of flush alone:", "",
            "| row | FlushDataCache | InvalidateDataCache | StoreDataCache | CacheFlushRegion |", "|---|---|---|---|---|"]
    for name, row in list(rep["rows"].items()) + [("total", rep["total"])]:
        cb = row["covered_bytes"]
        out.append(f"| {name} | {cb['flush']} | {cb['invalidate']} | {cb['store']} | {cb['region']} |")
    for name, info in rep["not_fully_covered"].items():
        out += ["", f"pages not fully covered before {name}: {info['pages']} pages in {info['ranges']} ranges; largest:"]
        out += [f"- 0x{r['paddr']:08x} + {r['pages']} pages" for r in info["largest"]]
    return "\n".join(out)


# --------------------------------------------------------------------------
# (b) from the runner's log

def parse_line(line: str) -> tuple[str, dict[str, str]] | None:
    """`TAG key=value ...` -> (TAG, fields); comment and empty lines -> None."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = line.split()
    fields = {}
    for part in parts[1:]:
        key, sep, value = part.partition("=")
        if sep:
            fields[key] = value
    return parts[0], fields


def _int(value: str) -> int:
    return int(value, 0)


def read_log(lines: Iterable[str]) -> Iterator[tuple[str, dict[str, str]]]:
    header = None
    for line in lines:
        if header is None:
            header = line
            if not line.startswith("# coherence v1 "):
                raise ValueError(f"not a coherence log (first line {line.strip()!r})")
            continue
        parsed = parse_line(line)
        if parsed:
            yield parsed


def analyse_log(lines: Iterable[str], top: int = 10) -> dict:
    """Group the accesses of a --check-coherence log by side, kind, writing event and call site."""
    summary: dict[str, int] = {}
    events: dict[str, dict[str, int]] = {}
    unwatched: list[dict] = []
    aliases: list[dict] = []
    by_side_kind: Counter = Counter()
    by_event: dict[tuple[str, str], Counter] = defaultdict(Counter)
    sites: dict[tuple[str, str], dict] = {}
    delays: list[int] = []
    frame_delays: list[int] = []
    sizes: Counter = Counter()
    n_access = 0
    header_seen = False
    for tag, f in read_log(lines):
        header_seen = True
        if tag == "ACCESS":
            n_access += 1
            side, kind = f["side"], f["kind"]
            cls = "load" if kind in LOAD_KINDS else "store" if kind in STORE_KINDS else "pointer"
            by_side_kind[(side, kind)] += 1
            by_event[(side, cls)][f["mark_ev"]] += 1
            if side == "cpu":
                key = (f["lr"], cls)
                site = sites.setdefault(key, {"lr": f["lr"], "class": cls, "count": 0, "bytes": 0,
                                              "kinds": Counter(), "mark_ev": Counter(), "guest": f["guest"] == "1"})
                site["count"] += 1
                site["bytes"] += _int(f["size"])
                site["kinds"][kind] += 1
                site["mark_ev"][f["mark_ev"]] += 1
            if side == "cpu" and cls == "load":
                delays.append(_int(f["t"]) - _int(f["mark_t"]))
                frame_delays.append(_int(f["f"]) - _int(f["mark_f"]))
                sizes[_int(f["size"])] += 1
        elif tag == "SUMMARY":
            summary = {k: _int(v) for k, v in f.items()}
        elif tag == "EVENT":
            events[f["ev"]] = {k: _int(v) for k, v in f.items() if k != "ev"}
        elif tag == "UNWATCHED":
            unwatched.append({"kind": f["kind"], "paddr": _int(f["paddr"]), "pages": _int(f["pages"]),
                              "marks": _int(f["marks"])})
        elif tag == "ALIAS" and "vaddr" in f:
            aliases.append({"f": _int(f["f"]), "vaddr": _int(f["vaddr"]), "paddr": _int(f["paddr"]),
                            "pages": _int(f["pages"])})
    if not header_seen and not summary:
        raise ValueError("the log has no records")
    ranked = sorted(sites.values(), key=lambda s: (-s["count"], s["lr"]))
    out_sites = [{"lr": s["lr"], "class": s["class"], "count": s["count"], "bytes": s["bytes"],
                  "kinds": dict(s["kinds"]), "mark_ev": dict(s["mark_ev"])} for s in ranked]
    cpu_loads = sum(v for (side, kind), v in by_side_kind.items() if side == "cpu" and kind in LOAD_KINDS)
    cpu_stores = sum(v for (side, kind), v in by_side_kind.items() if side == "cpu" and kind in STORE_KINDS)
    return {
        "complete": bool(summary),
        "summary": summary,
        "events": events,
        "accesses": n_access,
        "by_side_kind": {f"{s}/{k}": v for (s, k), v in sorted(by_side_kind.items())},
        "cpu_loads": cpu_loads,
        "cpu_stores": cpu_stores,
        "cpu_pointers": by_side_kind[("cpu", "Pointer")],
        "gpu_side": sum(v for (side, _), v in by_side_kind.items() if side == "gpu"),
        "by_writing_event": {f"{s}/{c}": dict(v) for (s, c), v in sorted(by_event.items())},
        "cpu_load_sites": [s for s in out_sites if s["class"] == "load"][:top],
        "cpu_store_sites": [s for s in out_sites if s["class"] == "store"][:top],
        "cpu_pointer_sites": [s for s in out_sites if s["class"] == "pointer"][:top],
        "cpu_load_sites_total": sum(1 for s in out_sites if s["class"] == "load"),
        "load_delay_ticks": _stats(delays),
        "load_delay_frames": _stats(frame_delays),
        "load_sizes": {str(k): v for k, v in sorted(sizes.items())},
        "unwatched": unwatched,
        "aliases": aliases,
    }


def _stats(values: list[int]) -> dict:
    if not values:
        return {}
    values = sorted(values)
    return {"min": values[0], "median": values[len(values) // 2], "p99": values[min(len(values) - 1, len(values) * 99 // 100)],
            "max": values[-1]}


def format_runlog(label: str, rep: dict) -> str:
    s = rep["summary"]
    out = [f"## {label}", ""]
    if not rep["complete"]:
        out += ["The log has no SUMMARY line: the run did not finish; the counts below are partial.", ""]
    if s:
        out += [f"frames {s.get('first_frame', 0)} to {s.get('last_frame', 0)}; {s.get('events', 0)} GPU events changed "
                f"{s.get('gpu_pages', 0)} pages ({s.get('marked', 0)} marked, {s.get('rewritten', 0)} rewritten while "
                f"still marked); {s.get('pending', 0)} still marked at the end.", ""]
    out += ["| GPU event | events | pages changed | marked | rewritten | no window | aliased |", "|---|---|---|---|---|---|---|"]
    for name, e in rep["events"].items():
        out.append(f"| {name} | {e['n']} | {e['gpu_pages']} | {e['marked']} | {e['rewritten']} | {e['unwatched']} | {e['aliased']} |")
    out += ["", f"accesses to marked pages: {rep['accesses']} (CPU {s.get('cpu', 0)}, GPU-side {s.get('gpu', 0)}); "
            f"CPU loads {rep['cpu_loads']}, CPU stores {rep['cpu_stores']}, raw pointers {rep['cpu_pointers']}.", ""]
    out += ["| side / kind | accesses |", "|---|---|"]
    out += [f"| {k} | {v} |" for k, v in rep["by_side_kind"].items()]
    out += ["", "first access to a page, by the GPU event that wrote it:", "", "| side / class | by event |", "|---|---|"]
    out += [f"| {k} | " + ", ".join(f"{e} {n}" for e, n in sorted(v.items())) + " |" for k, v in rep["by_writing_event"].items()]
    for title, key in (("CPU load call sites (LR)", "cpu_load_sites"), ("CPU store call sites (LR)", "cpu_store_sites"),
                       ("raw pointer call sites (LR)", "cpu_pointer_sites")):
        out += ["", f"{title}, most frequent first:", "", "| LR | accesses | bytes | kinds | writing events |", "|---|---|---|---|---|"]
        for site in rep[key]:
            out.append(f"| {site['lr']} | {site['count']} | {site['bytes']} | "
                       + ", ".join(f"{k} {v}" for k, v in sorted(site["kinds"].items())) + " | "
                       + ", ".join(f"{k} {v}" for k, v in sorted(site["mark_ev"].items())) + " |")
        if not rep[key]:
            out.append("| (none) | | | | |")
    if rep["load_delay_ticks"]:
        d, fr = rep["load_delay_ticks"], rep["load_delay_frames"]
        out += ["", f"CPU loads: ticks from the GPU write to the first load min {d['min']} / median {d['median']} / "
                f"p99 {d['p99']} / max {d['max']}; in frames min {fr['min']} / median {fr['median']} / max {fr['max']}."]
    out += ["", "coverage (pages the checker could not watch):", ""]
    if rep["unwatched"]:
        out += ["| kind | first page | pages | marks |", "|---|---|---|---|"]
        out += [f"| {u['kind']} | 0x{u['paddr']:08x} | {u['pages']} | {u['marks']} |" for u in rep["unwatched"]]
    else:
        out.append("none: every page the GPU changed was reachable by the game only through a watched window.")
    out += ["", f"ignored accesses: outside the emulation loop {s.get('outside_run', 0)}, other host threads "
            f"{s.get('other_thread', 0)}; to a page not marked by the checker {s.get('foreign', 0)}; to an address "
            f"outside the watched windows {s.get('no_window', 0)}."]
    return "\n".join(out)


# --------------------------------------------------------------------------
# Command line

def _emit(args: argparse.Namespace, reports: dict[str, dict], formatter) -> int:
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(reports, f, indent=2, sort_keys=True)
    text = "\n\n".join(formatter(label, rep) for label, rep in reports.items())
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if not args.json and not args.md:
        print(text)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, helptext in (("capture", "(a) the CPU's changes against the flushes, from MGPU captures"),
                           ("runlog", "(b) the report of a --check-coherence log")):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("files", nargs="+", metavar="FILE")
        p.add_argument("--json", metavar="OUT", help="write the report as JSON to OUT")
        p.add_argument("--md", metavar="OUT", help="write the report as markdown to OUT (default: print it)")
        p.add_argument("--top", type=int, default=10, help="how many ranges / call sites to list (default 10)")
    args = ap.parse_args(argv)
    reports: dict[str, dict] = {}
    try:
        for path in args.files:
            if args.cmd == "capture":
                reports[path] = analyse_capture(gcap.read(path), args.top)
            else:
                with open(path, encoding="utf-8") as f:
                    reports[path] = analyse_log(f, args.top)
    except (gcap.GcapError, OSError, ValueError, KeyError) as e:
        print(f"coherence: {e}", file=sys.stderr)
        return 1
    return _emit(args, reports, format_capture if args.cmd == "capture" else format_runlog)


if __name__ == "__main__":
    sys.exit(main())
