#!/usr/bin/env python3
# Derived from Azahar src/video_core/gpu.cpp, src/core/memory.h, src/core/hle/service/gsp/gsp_command.h,
# src/video_core/pica/regs_external.h, regs_internal.h, regs_texturing.h, regs_pipeline.h and pica_core.cpp,
# src/video_core/rasterizer_accelerated.cpp, src/video_core/renderer_opengl/gl_rasterizer.cpp and
# src/video_core/rasterizer_cache/pixel_format.h, utils.cpp, surface_params.cpp and rasterizer_cache.h at
# 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: every one of those files says "Licensed under GPLv2 or
# any later version"): the fixed virtual-to-physical mapping of GPU::VirtualToPhysicalAddress and the constants of
# the windows it uses (memory.h), the arguments of the GX commands (gsp_command.h), the pixel formats and the input
# size fields of a display transfer (regs_external.h), the command buffer registers a SubmitCmdList sets (gpu.cpp),
# the positions and fields of the PICA registers a draw or a command buffer jump reads (regs_internal.h,
# regs_texturing.h, regs_pipeline.h) and what those triggers do (pica_core.cpp), the vertex span of a draw
# (rasterizer_accelerated.cpp), the texture units a draw binds (gl_rasterizer.cpp, rasterizer_cache.h), and the bits
# per pixel and the mip levels of a texture (pixel_format.h, utils.cpp, surface_params.cpp).
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h, pica_core.cpp,
#   rasterizer_accelerated.cpp, pixel_format.h)
# Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h)
# Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h)
# Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
# Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_pipeline.h)
# Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
# Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (gl_rasterizer.cpp)
# Copyright 2023 Citra Emulator Project (utils.cpp)
# Copyright 2022 Citra Emulator Project (surface_params.cpp)
"""Flat-memory coherence report (N0-14): does the game depend on cache coherence between CPU and GPU?

Principle 2 of the native port (docs/native/ROADMAP.md) says the game's memory can be one flat block that the
GPU reads and writes in place, with no copy and no cache flush.  Two questions test it
(docs/research/flat-memory-check.md has the method, the commands and the results):

(a) from an MGPU capture, how often does the GPU use data the CPU changed with no cache flush covering it?

        python3 tools/native/coherence.py capture FILE.mgpu [FILE2.mgpu ...] [--alias-log LOG] [--json OUT]
                                                  [--md OUT] [--top N]

    Two answers.  The same-interval rule: for every GX command (and the other events a CPU_PAGES record
    precedes: MMIO, SWAP, FILL), the pages the CPU changed since the previous command, how many bytes of them
    changed, and how many of those bytes a FLUSH record in that same interval covers, per kind of flush.  The
    read-based answer (key `read_coverage`): when the GPU reads bytes the CPU changed, was a flush recorded
    between the change and the read?  The reads come from the GX commands' words and from a replay of the PICA
    registers (textures, vertex arrays, index arrays, chained command buffers).  LOG, the log of a
    `mh4u_runner --check-coherence` run, gives the alias runs that split FCRAM into regions and that map a DMA
    source or a flush outside the GPU's windows.

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
FCRAM_N3DS_SIZE = 0x10000000    # the largest FCRAM (New 3DS)
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

# regs_internal.h, regs_texturing.h and regs_pipeline.h: the PICA registers a draw or a command buffer jump reads.
REG_TEX_CONFIG = 0x80           # texturing.main_config: bit n enables texture unit n (n = 0, 1, 2)
# texture0, texture1 and texture2: the registers of (height and width, type, lod, address, format) of each unit.
# The size is height (bits 0-10) and width (bits 16-26); the type (bits 28-30) counts for unit 0 only; lod.max_level
# is bits 16-19; the address (bits 0-27) is the physical address / 8; the format is bits 0-3.
TEX_UNIT_REGS = ((0x82, 0x83, 0x84, 0x85, 0x8E), (0x92, 0x93, 0x94, 0x95, 0x96), (0x9A, 0x9B, 0x9C, 0x9D, 0x9E))
REG_CUBE_ADDR = 0x86            # texturing.cube_address[5]: bits 0-21 of the address of cube faces 1 to 5
TEX_CUBE, TEX_SHADOW_CUBE = 1, 4    # TextureConfig::TextureType: the two types with six faces
REG_ATTR_BASE = 0x200           # pipeline.vertex_attributes.base_address: bits 1-28, the physical address / 16
REG_LOADER = 0x203              # attribute_loaders[12], 3 words each: data_offset (bits 0-27); -; byte_count
LOADERS = 12                    #   (bits 16-23) and component_count (bits 28-31) in the third word
REG_INDEX = 0x227               # pipeline.index_array: offset from the base (bits 0-27), 16-bit indices (bit 31)
REG_NUM_VERTICES = 0x228
REG_VERTEX_OFFSET = 0x22A
REG_DRAW, REG_DRAW_INDEXED = 0x22E, 0x22F    # writing either one draws (pica_core.cpp)
# pipeline.command_buffer: size[2] (bits 0-19, bytes / 8), addr[2] (bits 0-27, physical address / 8) and
# trigger[2]: writing trigger[n] jumps to the command buffer of channel n (pica_core.cpp).
REG_CMDBUF_SIZE, REG_CMDBUF_ADDR, REG_CMDBUF_JUMP = 0x238, 0x23A, 0x23C
# rasterizer_cache/pixel_format.h (FORMAT_MAP): bits per pixel of texture formats 0 (RGBA8) to 13 (ETC1A4).
TEX_BITS = (32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8)

# What each GX command's own words say it reads (named_input), as a kind of read.
NAMED_READ = {0: "dma_source", 1: "cmdlist", 3: "transfer_input", 4: "transfer_input"}
NAMED_KINDS = ("cmdlist", "dma_source", "transfer_input")
GX_CACHE_FLUSH = 5             # gsp_command.h CommandId::CacheFlush
READ_KINDS = ("cmdlist", "dma_source", "transfer_input", "texture", "vertex", "index", "chained_list")
CLASSES = ("certain", "same_interval", "not_covered")
# Where each changed byte ends, per region: every byte of every change lands in exactly one of the keys from
# read_by_gpu to not_read_never_in_read_range (the *_flushed keys are parts of the two not_read keys).
REGION_KEYS = ("changed_bytes", "page_changes", "flushed_by_end", "read_by_gpu", "replaced", "overwritten_by_gpu",
               "not_read_in_read_range", "not_read_in_read_range_flushed", "not_read_never_in_read_range",
               "not_read_never_in_read_range_flushed")
COUNT_KEYS = ("draws", "draws_indexed", "texture_reads", "chained_list_jumps", "submit_cmdlist",
              "submit_cmdlist_do_flush", "cache_flush", "cache_flush_equals_next_buffer", "flushes_through_alias_runs",
              "flushes_with_two_mappings")


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


def passed_through_range(vaddr: int, size: int) -> tuple[int, int] | None:
    """The physical byte range of an address outside the windows, which GPU::VirtualToPhysicalAddress passes
    through unchanged: [vaddr, vaddr + size) clipped to VRAM or FCRAM; None if it starts in neither."""
    for lo, wsize in ((VRAM_PADDR, VRAM_SIZE), (FCRAM_PADDR, FCRAM_N3DS_SIZE)):
        if lo <= vaddr < lo + wsize:
            return vaddr, min(vaddr + size, lo + wsize)
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


def _one_place(virtual: list[tuple[int, int]], ranges: list[tuple[int, int]]) -> tuple[list[tuple[int, int]], bool]:
    """The physical ranges of the parts of VIRTUAL (RANGES are their physical ranges, pair by pair) that every run
    holding them maps to the same memory, and whether some part was mapped to two different places."""
    parts = [(lo, hi, plo - lo) for (lo, hi), (plo, _) in zip(virtual, ranges)]
    cuts = sorted({x for lo, hi, _ in parts for x in (lo, hi)})
    out: list[tuple[int, int]] = []
    two = False
    for a, b in zip(cuts, cuts[1:]):
        deltas = {d for lo, hi, d in parts if lo <= a and b <= hi}
        if len(deltas) == 1:
            d = deltas.pop()
            out.append((a + d, b + d))
        elif deltas:
            two = True
    return _merge(out), two


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


def analyse_capture(records: Iterable[gcap.Record], top: int = 10,
                    alias_runs: Sequence[tuple[int, int, int]] | None = None) -> dict:
    """Stream a capture and report the CPU changes before each event against the flushes.

    The rows (the same-interval rule): a CPU_PAGES record is taken before the next GPU event, so a page in it
    changed at some point after the previous diff (the end of the previous command).  It counts as covered when a
    FLUSH recorded after that point and before the event covers it (for a CacheFlush command, its own regions count
    too).  A change made after the flush within the same interval is counted as covered: the capture cannot tell.
    The interval before the first event follows MEM, with no earlier flush visible, and is reported as a row of its
    own.  `read_coverage` is the read-based answer of _ReadCoverage, from the same pass over the records;
    ALIAS_RUNS (load_alias_runs) split FCRAM into regions there and map DMA sources and flushes outside the GPU's
    windows.
    """
    reads = _ReadCoverage(alias_runs)
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
        reads.feed(rec)
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
        "read_coverage": reads.report(top),
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


# --------------------------------------------------------------------------
# (a) the read-based answer: when the GPU reads bytes the CPU changed, was a flush recorded in between?
#
# The bytes of a page are tracked as byte masks: an int with 0x01 in each byte of the page that is in the set.
# The bitwise operators combine them and int.bit_count() counts the bytes.

_NONZERO = bytes([0]) + bytes([1]) * 255      # a bytes.translate table: 0 stays 0, anything else becomes 1
_ONES = int.from_bytes(b"\x01" * PAGE, "little")
_BYTE_MASKS = [gcap.mask_expand(m) for m in range(16)]


def _span(a: int, b: int) -> int:
    """The byte mask of bytes [a, b) of a page (0 <= a < b <= PAGE)."""
    return (_ONES >> (8 * (PAGE - b + a))) << (8 * a)


def _range_mask(page: int, lo: int, hi: int) -> int:
    """The byte mask of the part of the physical range [lo, hi) inside the page at PAGE (0 if none)."""
    a, b = max(lo, page) - page, min(hi, page + PAGE) - page
    return _span(a, b) if b > a else 0


def _changed(old: bytes, new: bytes) -> int:
    """The byte mask of the bytes that differ between two versions of a page."""
    x = int.from_bytes(old, "little") ^ int.from_bytes(new, "little")
    return int.from_bytes(x.to_bytes(PAGE, "little").translate(_NONZERO), "little") if x else 0


def _pages_in(lo: int, hi: int, pages) -> list[int]:
    """The pages of PAGES (a set or dict of page addresses) that the physical range [lo, hi) touches, ascending."""
    first = lo & ~(PAGE - 1)
    if (hi - first) // PAGE > len(pages):
        return sorted(p for p in pages if first <= p < hi)
    return [p for p in range(first, hi, PAGE) if p in pages]


def mip_levels(width: int, height: int, max_level: int) -> int:
    """rasterizer_cache/utils.cpp MipLevels: the levels of a texture Azahar loads, at most max_level + 1."""
    levels = 1
    while width > 8 and height > 8:
        levels += 1
        width >>= 1
        height >>= 1
    return min(levels, max_level + 1)


def texture_size(width: int, height: int, fmt: int, max_level: int) -> int:
    """The bytes of a texture with its mip chain (surface_params.cpp CalculateSurfaceSize); a format that
    pixel_format.h does not list counts as 32 bits per pixel (an upper bound)."""
    bits = TEX_BITS[fmt] if fmt < len(TEX_BITS) else 32
    return sum((width >> lv) * (height >> lv) * bits // 8 for lv in range(mip_levels(width, height, max_level)))


def cube_face_size(width: int, fmt: int, max_level: int) -> int:
    """The bytes of one face of a TextureCube: gl_rasterizer.cpp BindTextureCube loads max_level + 1 levels of
    width x width, with no MipLevels limit (rasterizer_cache.h GetTextureCube), and surface_params.cpp
    CalculateSurfaceSize sums them; a format that pixel_format.h does not list counts as 32 bits per pixel."""
    bits = TEX_BITS[fmt] if fmt < len(TEX_BITS) else 32
    return sum((width >> lv) * (width >> lv) * bits // 8 for lv in range(max_level + 1))


def load_alias_runs(lines: Iterable[str]) -> list[tuple[int, int, int]]:
    """The union of the ALIAS runs of a --check-coherence log: [(paddr, pages, vaddr)], in the order of the log.
    A run seen again with the same vaddr and paddr (a run that grew or shrank between scans) is kept once, with
    its largest page count."""
    runs: list[tuple[int, int, int]] = []
    for tag, f in read_log(lines):
        if tag == "ALIAS" and "vaddr" in f:
            paddr, pages, vaddr = _int(f["paddr"]), _int(f["pages"]), _int(f["vaddr"])
            for i, (p, n, v) in enumerate(runs):
                if (p, v) == (paddr, vaddr):
                    runs[i] = (p, max(n, pages), v)
                    break
            else:
                runs.append((paddr, pages, vaddr))
    return runs


class _Regions:
    """The region of a physical page: VRAM; FCRAM in an alias run, named by the run's virtual address (memory the
    game reaches through a non-linear address: its code, heap, stack, .data and the like); or the rest of FCRAM,
    which the game reaches through the linear windows.  With no alias map (or one with no runs), FCRAM is one
    region."""

    def __init__(self, runs: Sequence[tuple[int, int, int]] | None) -> None:
        self.given = bool(runs)
        self.runs = len(runs or ())
        self.rest = "FCRAM linear" if self.given else "FCRAM"   # the FCRAM no run holds
        self.alias: dict[int, str] = {}
        for paddr, pages, vaddr in runs or ():
            for page in range(paddr & ~(PAGE - 1), paddr + pages * PAGE, PAGE):
                self.alias.setdefault(page, f"FCRAM alias 0x{vaddr:08x}")   # a page in two runs: the first names it

    def __call__(self, page: int) -> str:
        if VRAM_PADDR <= page < VRAM_PADDR + VRAM_SIZE:
            return "VRAM"
        if FCRAM_PADDR <= page < FCRAM_PADDR + FCRAM_N3DS_SIZE:
            return self.alias.get(page, self.rest)
        return "other"

    @staticmethod
    def order(label: str) -> tuple[int, str]:
        rank = {"VRAM": 0, "FCRAM": 1, "FCRAM linear": 1, "other": 3}.get(label, 2)
        return rank, label


class _Pica:
    """The PICA register file, replayed from the INIT record and the PICA_REG records, and the memory each draw
    and each command buffer jump reads.  The ranges are upper bounds: every enabled texture unit, even if no
    stage samples it, with its whole mip chain; the full span [vmin, vmax] of each attribute loader."""

    def __init__(self, mem: gcap.Memory) -> None:
        self.mem = mem
        self.regs: list[int] | None = None
        self.counts: Counter = Counter()

    def init(self, payload: bytes) -> None:
        for kind, data in gcap.parse_init(payload):
            if kind == gcap.K_PICA_REGS:
                if len(data) != gcap.NUM_PICA_REGS * 4:
                    raise gcap.GcapError("INIT: PICA_REGS block has the wrong size")
                self.regs = list(struct.unpack(f"<{gcap.NUM_PICA_REGS}I", data))

    def _registers(self) -> list[int]:
        if self.regs is None:
            raise gcap.GcapError("a command list before an INIT with a PICA_REGS block")
        return self.regs

    def submit(self, vaddr: int, size: int) -> None:
        """GPU::Execute (gpu.cpp) sets channel 0 of the command buffer itself for a SubmitCmdList: no PICA_REG."""
        regs = self._registers()
        paddr = virtual_to_physical(vaddr)
        paddr = vaddr if paddr is None else paddr         # GPU::VirtualToPhysicalAddress passes the rest through
        regs[REG_CMDBUF_ADDR] = (regs[REG_CMDBUF_ADDR] & ~0x0FFFFFFF) | ((paddr >> 3) & 0x0FFFFFFF)
        regs[REG_CMDBUF_SIZE] = (regs[REG_CMDBUF_SIZE] & ~0xFFFFF) | ((size >> 3) & 0xFFFFF)

    def write(self, writes: Iterable[tuple[int, int, int]], out: list[tuple[str, int, int]]) -> None:
        """Apply register writes (register, byte mask, value) in order; add what each trigger reads to OUT."""
        regs = self._registers()
        for reg, mask, value in writes:
            m = _BYTE_MASKS[mask]
            regs[reg] = (regs[reg] & ~m) | (value & m)
            if reg == REG_DRAW or reg == REG_DRAW_INDEXED:
                self._draw(regs, reg == REG_DRAW_INDEXED, out)
            elif reg == REG_CMDBUF_JUMP or reg == REG_CMDBUF_JUMP + 1:
                channel = reg - REG_CMDBUF_JUMP
                addr = (regs[REG_CMDBUF_ADDR + channel] & 0x0FFFFFFF) * 8
                size = (regs[REG_CMDBUF_SIZE + channel] & 0xFFFFF) * 8
                self.counts["chained_list_jumps"] += 1
                if size:
                    out.append(("chained_list", addr, addr + size))

    def _draw(self, regs: list[int], indexed: bool, out: list[tuple[str, int, int]]) -> None:
        """rasterizer_accelerated.cpp AnalyzeVertexArray: an indexed draw reads its index array, whose smallest and
        largest index are vmin and vmax; any other draw has vmin = vertex_offset, vmax = vmin + num_vertices - 1.
        Each loader with components reads (vmax - vmin + 1) x byte_count bytes from base + data_offset +
        vmin x byte_count.  A draw of no vertices reads nothing."""
        self.counts["draws_indexed" if indexed else "draws"] += 1
        count = regs[REG_NUM_VERTICES]
        if count == 0:
            return
        base = ((regs[REG_ATTR_BASE] >> 1) & 0x0FFFFFFF) * 16
        if indexed:
            wide = regs[REG_INDEX] >> 31
            addr = base + (regs[REG_INDEX] & 0x0FFFFFFF)
            size = count * (2 if wide else 1)
            out.append(("index", addr, addr + size))
            data = self.mem.read(addr, size)
            values = struct.unpack(f"<{count}H", data) if wide else data
            first, last = min(values), max(values)
        else:
            first = regs[REG_VERTEX_OFFSET]
            last = first + count - 1
        for i in range(LOADERS):
            config = regs[REG_LOADER + 3 * i + 2]
            stride = (config >> 16) & 0xFF
            if config >> 28 and stride:
                lo = base + (regs[REG_LOADER + 3 * i] & 0x0FFFFFFF) + first * stride
                out.append(("vertex", lo, lo + (last - first + 1) * stride))
        self._textures(regs, out)

    def _textures(self, regs: list[int], out: list[tuple[str, int, int]]) -> None:
        """Each enabled texture unit (gl_rasterizer.cpp SyncTextureUnits).  Unit 0 with type TextureCube reads six
        faces of width x width with max_level + 1 levels (BindTextureCube, cube_face_size); unit 0 with type
        ShadowCube six faces of width x height with the mip chain of a 2D texture (BindShadowCube loads one level:
        an upper bound); any other unit or type one texture of width x height of its format with its mip chain
        (Azahar binds even the Disabled type of unit 0 as a 2D texture).  Address 0 is no texture
        (rasterizer_cache.h GetTextureSurface)."""
        for unit, (r_size, r_type, r_lod, r_addr, r_format) in enumerate(TEX_UNIT_REGS):
            if not (regs[REG_TEX_CONFIG] >> unit) & 1:
                continue
            height, width = regs[r_size] & 0x7FF, (regs[r_size] >> 16) & 0x7FF
            fmt, max_level = regs[r_format] & 0xF, (regs[r_lod] >> 16) & 0xF
            kind = (regs[r_type] >> 28) & 7 if unit == 0 else None
            if kind == TEX_CUBE:
                size = cube_face_size(width, fmt, max_level)
            else:
                size = texture_size(width, height, fmt, max_level)
            address = regs[r_addr] & 0x0FFFFFFF
            faces = [address]
            if kind in (TEX_CUBE, TEX_SHADOW_CUBE):
                # regs_texturing.h GetCubePhysicalAddress: faces 1 to 5 take bits 22-27 from the address of face 0
                faces += [(address & ~0x3FFFFF) | (regs[REG_CUBE_ADDR + f] & 0x3FFFFF) for f in range(5)]
            for face in faces:
                if face and size:
                    out.append(("texture", face * 8, face * 8 + size))
                    self.counts["texture_reads"] += 1


class _Change:
    """Bytes of one page that the CPU changed in interval K and no GPU read has taken yet (PEND, a byte mask),
    and, of them, those a flush covered in a later interval (CERTAIN) or in interval K itself (SAME)."""
    __slots__ = ("k", "pend", "certain", "same")

    def __init__(self, k: int, pend: int, same: int) -> None:
        self.k, self.pend, self.certain, self.same = k, pend, 0, same


@dataclass
class _Event:
    """A GX/MMIO/SWAP/FILL event whose reads and own flush regions are still being gathered."""
    index: int
    gx: int | None = None                                              # the GX command id
    reads: list[tuple[str, int, int]] = field(default_factory=list)    # (kind of read, lo, hi), physical
    own: list[tuple[int, int]] = field(default_factory=list)           # the kind-4 FLUSH regions right after its GX
    tail: bool = False                                                 # nothing else since the GX record yet


class _ReadCoverage:
    """The read-based answer of part (a), from one pass over the records of a capture.

    Interval k is the time before event k (the GX, MMIO, SWAP and FILL records, numbered from 0).  The CPU_PAGES
    record just before event k holds the CPU's changes of interval k, byte by byte; a later change to a byte
    replaces the earlier one.  A FLUSH record (kinds 1 to 4, physical ranges as for the rows, or through the alias
    runs outside the windows: _flush_ranges) covers the pending changes in its range: with certain order when it
    is recorded in a later interval than the change, only in the same interval when it is recorded in the change's
    own interval (the capture cannot order the store and the flush).  The kind-4 records right after a GX record (a
    CacheFlush command's regions) are at that command's position: certain for the changes of earlier intervals,
    same interval for those of its own CPU_PAGES.  At each event the GPU reads what the command's words name
    (_named) and, for a SubmitCmdList, what its register writes point at (_Pica).  The first read of a changed byte
    takes the change: a later read of the same unchanged byte is not a new pair.  An event's reads come before the
    flushes and the GPU writes recorded after it.  A GPU write to a byte with a pending change ends that change (the
    CPU's value is gone from memory).  Not modelled: the colour and depth buffers a draw loads (regs_framebuffer.h)
    and the screen the display reads at SWAP.
    """

    def __init__(self, alias_runs: Sequence[tuple[int, int, int]] | None = None) -> None:
        self.mem = gcap.Memory()
        self.pica = _Pica(self.mem)
        self.alias_runs = list(alias_runs or ())
        self.regions = _Regions(alias_runs)
        self.outside = {kind: Counter() for kind in NAMED_KINDS}   # named reads outside the GPU's windows
        self.events = 0                                       # events started so far: the current interval
        self.open: _Event | None = None
        self.pending: dict[int, list[_Change]] = {}           # page -> its pending changes
        self.interval_flushes: list[tuple[int, int]] = []     # flush ranges recorded in the current interval
        self.flush_log: list[tuple[int, int, int]] = []       # (position, lo, hi) of every flush
        self.change_log: list[tuple[int, int, int]] = []      # (interval, page, byte mask) of every change
        self.all_reads: list[tuple[int, int]] = []            # every read range of the capture, merged
        self.read_pages: dict[str, set[int]] = defaultdict(set)
        self.stats = {kind: {"bytes": [0, 0, 0], "pairs": [0, 0, 0], "page_class": {}, "first_interval": 0}
                      for kind in READ_KINDS + ("total",)}
        self.by_region: dict[str, Counter] = defaultdict(Counter)
        for label in ("VRAM", self.regions.rest):              # always reported, even with no change
            self.by_region[label] = Counter()
        self.region_pages: dict[str, set[int]] = defaultdict(set)
        self.counts: Counter = Counter()
        self.waiting_cache_flushes: list[list[tuple[int, int]]] = []

    # ── the records ──

    def feed(self, rec: gcap.Record) -> None:
        t = rec.type
        ev = self.open
        if ev is not None:
            if t == gcap.FLUSH and ev.tail and struct.unpack_from("<I", rec.payload)[0] == 4:
                ev.own += self._flush_ranges(*struct.unpack_from("<II", rec.payload, 4))
                return
            if t == gcap.PICA_REG and ev.gx == gcap.GX_SUBMIT_CMDLIST:
                ev.tail = False
                self.pica.write(gcap.parse_pica_reg(rec.payload), ev.reads)
                return
            self._close(ev)
        if t == gcap.INIT:
            self.pica.init(rec.payload)
        elif t == gcap.MEM:
            addr, data = gcap.parse_mem(rec.payload)
            self.mem.write(addr, data)
        elif t == gcap.CPU_PAGES:
            for addr, data in gcap.parse_pages(rec.payload):
                mask = _changed(self.mem.read(addr, PAGE), data)
                self.mem.write(addr, data)
                if mask:
                    self._change(addr, mask)
        elif t == gcap.GPU_PAGES:
            for addr, data in gcap.parse_pages(rec.payload, rec.name):
                if addr in self.pending:
                    mask = _changed(self.mem.read(addr, PAGE), data)
                    self.by_region[self.regions(addr)]["overwritten_by_gpu"] += self._take(addr, mask)
                self.mem.write(addr, data)
        elif t == gcap.FLUSH:
            kind, vaddr, size = struct.unpack("<III", rec.payload)
            if kind in FLUSH_KINDS:
                for lo, hi in self._flush_ranges(vaddr, size):
                    self.interval_flushes.append((lo, hi))
                    self._flush(lo, hi, self.events)
        elif t == gcap.PICA_REG:
            self.pica.write(gcap.parse_pica_reg(rec.payload), [])    # no command list open: replayed only
        elif t in EVENT_ROWS:
            self._start(t, rec.payload)

    def _start(self, t: int, payload: bytes) -> None:
        ev = _Event(self.events)
        self.events += 1
        self.interval_flushes = []
        if t == gcap.GX:
            cid, words = gcap.parse_gx(payload)
            ev.gx, ev.tail = cid, True
            named = named_input(words)
            ranges = []
            if named and named[1] > 0 and cid in NAMED_READ:
                ranges = self._named(NAMED_READ[cid], *named)
                ev.reads += [(NAMED_READ[cid], lo, hi) for lo, hi in ranges]
            if cid == gcap.GX_SUBMIT_CMDLIST:
                self.counts["submit_cmdlist"] += 1
                if words[7]:     # do_flush (gsp_command.h SubmitCmdListCommand): counted, not credited as a flush
                    self.counts["submit_cmdlist_do_flush"] += 1
                self.pica.submit(words[1], words[2])
                self.counts["cache_flush_equals_next_buffer"] += sum(
                    1 for regions in self.waiting_cache_flushes if ranges and regions == ranges)
                self.waiting_cache_flushes = []
            elif cid == GX_CACHE_FLUSH:
                self.counts["cache_flush"] += 1
        self.open = ev

    def _close(self, ev: _Event) -> None:
        self.open = None
        self._read(ev)
        for lo, hi in ev.own:
            self._flush(lo, hi, ev.index)
        if ev.gx == GX_CACHE_FLUSH:
            self.waiting_cache_flushes.append(_merge(ev.own))

    def _named(self, kind: str, vaddr: int, size: int) -> list[tuple[int, int]]:
        """The physical ranges of a read a GX command's words name.  An address in one of the GPU's windows maps
        through it.  Outside them, Azahar passes a command list or transfer address through as a physical one
        (gpu.cpp VirtualToPhysicalAddress, as _Pica.submit does), and copies a DMA source through the game's page
        table (gpu.cpp RequestDma: CopyBlock), which the alias runs of --alias-log give.  Those reads are counted
        in self.outside; their bytes that map to no memory the capture holds are not modelled."""
        span = physical_range(vaddr, size)
        if span is not None:
            return [span]
        out = self.outside[kind]
        out["reads"] += 1
        out["bytes"] += size
        if kind == "dma_source":
            virtual, ranges = self._alias_ranges(vaddr, size)
            out["bytes_modelled"] += sum(hi - lo for lo, hi in _merge(virtual))
            return ranges
        span = passed_through_range(vaddr, size)
        if span is None:
            return []
        out["bytes_modelled"] += span[1] - span[0]
        return [span]

    def _alias_ranges(self, vaddr: int, size: int) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
        """The parts of [vaddr, vaddr + size) that the alias runs hold: their virtual and their physical ranges."""
        virtual, ranges = [], []
        for paddr, pages, run in self.alias_runs:
            lo, hi = max(vaddr, run), min(vaddr + size, run + pages * PAGE)
            if hi > lo:
                virtual.append((lo, hi))
                ranges.append((lo - run + paddr, hi - run + paddr))
        return virtual, ranges

    def _flush_ranges(self, vaddr: int, size: int) -> list[tuple[int, int]]:
        """The physical ranges a FLUSH record covers here.  An address in one of the GPU's windows maps through it,
        as for the rows.  Outside them the game flushes by the address it uses, and the cache is cleaned for the
        memory behind it, so the alias runs map it, as they map a DMA source (_named); without an alias map such a
        flush covers nothing.  The alias map is the union of the log's ALIAS sets, so two runs can map one address to
        different memory (a remap between two sets): a flush covers no byte of such an address (_one_place), while a
        DMA source reads from both, which can only add reads.  The same-interval rows credit only the windows."""
        span = physical_range(vaddr, size)
        if span is not None:
            return [span]
        ranges, two = _one_place(*self._alias_ranges(vaddr, size))
        if ranges:
            self.counts["flushes_through_alias_runs"] += 1
        if two:
            self.counts["flushes_with_two_mappings"] += 1
        return ranges

    # ── changes, flushes and reads ──

    def _take(self, page: int, mask: int) -> int:
        """Remove the bytes of MASK from the page's pending changes; return how many of them were pending."""
        taken = 0
        keep = []
        for g in self.pending.get(page, ()):
            hit = g.pend & mask
            if hit:
                taken += hit.bit_count()
                g.pend &= ~mask
                g.certain &= ~mask
                g.same &= ~mask
            if g.pend:
                keep.append(g)
        if keep:
            self.pending[page] = keep
        else:
            self.pending.pop(page, None)
        return taken

    def _change(self, page: int, mask: int) -> None:
        k = self.events
        label = self.regions(page)
        region = self.by_region[label]
        region["changed_bytes"] += mask.bit_count()
        region["page_changes"] += 1
        self.region_pages[label].add(page)
        self.change_log.append((k, page, mask))
        region["replaced"] += self._take(page, mask)
        same = 0
        for lo, hi in self.interval_flushes:        # recorded earlier in this interval: order unknown
            same |= _range_mask(page, lo, hi)
        self.pending.setdefault(page, []).append(_Change(k, mask, same & mask))

    def _flush(self, lo: int, hi: int, pos: int) -> None:
        """A flush at position POS (the interval it was recorded in, or the event it belongs to)."""
        self.flush_log.append((pos, lo, hi))
        for page in _pages_in(lo, hi, self.pending):
            mask = _range_mask(page, lo, hi)
            for g in self.pending[page]:
                if g.k < pos:
                    g.certain |= g.pend & mask
                elif g.k == pos:
                    g.same |= g.pend & mask

    def _read(self, ev: _Event) -> None:
        by_kind: dict[str, list[tuple[int, int]]] = defaultdict(list)
        for kind, lo, hi in ev.reads:
            if hi > lo:
                by_kind[kind].append((lo, hi))
        if not by_kind:
            return
        union: dict[int, int] = {}
        ranges: list[tuple[int, int]] = []
        for kind in READ_KINDS:
            if kind not in by_kind:
                continue
            merged = _merge(by_kind[kind])
            ranges += merged
            hits: dict[int, int] = {}
            for lo, hi in merged:
                self.read_pages[kind].update(range(lo & ~(PAGE - 1), hi, PAGE))
                for page in _pages_in(lo, hi, self.pending):
                    hits[page] = hits.get(page, 0) | _range_mask(page, lo, hi)
            self._classify(kind, hits)
            for page, mask in hits.items():
                union[page] = union.get(page, 0) | mask
        for lo, hi in ranges:
            self.read_pages["total"].update(range(lo & ~(PAGE - 1), hi, PAGE))
        self._classify("total", union)
        for page, mask in union.items():
            self.by_region[self.regions(page)]["read_by_gpu"] += self._take(page, mask)
        self.all_reads = _merge(self.all_reads + ranges)

    def _classify(self, kind: str, hits: dict[int, int]) -> None:
        """Count the pending bytes one kind of read (or all of them) takes at this event, per class and pair."""
        st = self.stats[kind]
        for page, mask in hits.items():
            per_k: dict[int, list[int]] = {}
            for g in self.pending[page]:
                hit = g.pend & mask
                if hit:
                    n = per_k.setdefault(g.k, [0, 0, 0])
                    n[0] += (hit & g.certain).bit_count()
                    n[1] += (hit & g.same & ~g.certain).bit_count()
                    n[2] += (hit & ~(g.certain | g.same)).bit_count()
            worst = -1
            for k, n in per_k.items():
                for c in range(3):
                    st["bytes"][c] += n[c]
                cls = 2 if n[2] else 1 if n[1] else 0
                st["pairs"][cls] += 1
                worst = max(worst, cls)
                if k == 0:
                    st["first_interval"] += n[2]
            if worst >= 0:
                st["page_class"][page] = max(st["page_class"].get(page, -1), worst)

    # ── the report ──

    def report(self, top: int = 10) -> dict:
        if self.open is not None:
            self._close(self.open)
        ledger = self.by_region
        starts = [lo for lo, _ in self.all_reads]
        for page, groups in self.pending.items():                # the changes no read took
            in_range = 0
            i = max(bisect.bisect_right(starts, page) - 1, 0)
            while i < len(starts) and starts[i] < page + PAGE:
                in_range |= _range_mask(page, *self.all_reads[i])
                i += 1
            region = ledger[self.regions(page)]
            for g in groups:
                flushed = g.certain | g.same
                for key, part in (("in_read_range", g.pend & in_range), ("never_in_read_range", g.pend & ~in_range)):
                    region[f"not_read_{key}"] += part.bit_count()
                    region[f"not_read_{key}_flushed"] += (part & flushed).bit_count()
        changed = {page for _, page, _ in self.change_log}       # flushed by the end: any flush from the change on
        flushes: dict[int, list[tuple[int, int]]] = defaultdict(list)
        for pos, lo, hi in self.flush_log:
            for page in _pages_in(lo, hi, changed):
                flushes[page].append((pos, _range_mask(page, lo, hi)))
        for k, page, mask in self.change_log:
            cover = 0
            for pos, fmask in flushes.get(page, ()):
                if pos >= k:
                    cover |= fmask
            ledger[self.regions(page)]["flushed_by_end"] += (mask & cover).bit_count()

        reads = {}
        for kind, st in self.stats.items():
            classes = st["page_class"]
            uncovered = sorted(p for p, c in classes.items() if c == 2)
            grouped = group_pages(uncovered)
            reads[kind] = {
                "bytes": dict(zip(CLASSES, st["bytes"])),
                "pages": {name: sum(1 for c in classes.values() if c == i) for i, name in enumerate(CLASSES)},
                "pairs": dict(zip(CLASSES, st["pairs"])),
                "not_covered_first_interval_bytes": st["first_interval"],
                "read_pages": len(self.read_pages[kind]),
                "uncovered": {"pages": len(uncovered), "ranges": len(grouped),
                              "largest": [{"paddr": lo, "pages": n} for lo, n in
                                          sorted(grouped, key=lambda r: -r[1])[:top]]},
            }
        by_region = {}
        for label in sorted(ledger, key=_Regions.order):
            by_region[label] = {key: ledger[label][key] for key in REGION_KEYS}
            by_region[label]["pages"] = len(self.region_pages[label])
        total = {key: sum(r[key] for r in by_region.values()) for key in REGION_KEYS + ("pages",)}
        counts = self.counts + self.pica.counts
        return {
            "events": self.events,
            "alias_map": {"given": self.regions.given, "runs": self.regions.runs},
            "named_outside_windows": {kind: {key: self.outside[kind][key] for key in ("reads", "bytes",
                                                                                       "bytes_modelled")}
                                      for kind in NAMED_KINDS},
            "reads": reads,
            "regions": by_region,
            "changes": total,
            "counts": {key: counts[key] for key in COUNT_KEYS},
        }


def pct(part: int, whole: int) -> str:
    return f"{100.0 * part / whole:.1f}%" if whole else "-"


def format_capture(label: str, rep: dict) -> str:
    out = [f"## {label}", ""]
    out.append(f"events: {rep['events']} (the first is a {rep['first_event']}); flush records: " + ", ".join(
        f"{k} {v['count']} ({v['maps_nowhere']} map nowhere)" for k, v in rep["flushes"].items()))
    out += ["", "### Same-interval rule", "",
            "A flush counts here only when it is recorded in the same interval as the CPU change (for a CACHE_FLUSH, "
            "its own regions count for its own CPU pages), and every changed byte counts, read by the GPU or not. "
            "The read-based section below asks instead whether a flush came between a change and the GPU's read of it.",
            ""]
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
    if "read_coverage" in rep:
        out += format_read_coverage(rep["read_coverage"])
    return "\n".join(out)


def format_read_coverage(rc: dict) -> list[str]:
    """The markdown lines of the read-based section of one capture."""
    out = ["", "### Read-based coverage: was a flush recorded between the CPU's change and the GPU's read?", "",
           "Every byte that differs in a CPU_PAGES record is a change, made in the interval before the event that the "
           "record precedes; a later change to the same byte replaces it. The first GPU read of a changed byte takes "
           "the change: later reads of the same unchanged byte are not counted again. A read byte is covered with "
           "certain order when a flush recorded in a later interval than the change, and before the read, covers it; "
           "covered in the same interval when the only such flush was recorded in the change's own interval (the "
           "capture cannot tell whether the store or the flush came first); not covered otherwise. The regions of a "
           "CACHE_FLUSH are at that command's own place. A pair is one page changed in one interval and read by one "
           "event; a page and a pair are counted in the worst class of their bytes. The changes of the first interval "
           "can have no earlier flush in the file (a flush before the capture started is not recorded). The modelled "
           "reads are the addresses a GX command's words name and what a command list's register writes point at; "
           "they are upper bounds: every enabled texture unit (even if no stage samples it) with its whole mip chain, "
           "and the full vertex span of each attribute loader. Not modelled: the colour and depth buffers a draw loads "
           "from memory, and the screen the display reads at SWAP; a change that only those read counts as not read. "
           "'pages read' counts every page a read of that kind touched.",
           "",
           "| read | bytes: certain / same interval / not covered | pages: certain / same interval / not covered | "
           "pairs: certain / same interval / not covered | not covered, changed in the first interval (bytes) | "
           "pages read |",
           "|---|---|---|---|---|---|"]
    for kind in READ_KINDS + ("total",):
        r = rc["reads"][kind]
        cells = [" / ".join(str(r[part][c]) for c in CLASSES) for part in ("bytes", "pages", "pairs")]
        out.append(f"| {kind} | " + " | ".join(cells) + f" | {r['not_covered_first_interval_bytes']} | {r['read_pages']} |")
    bad = [(kind, rc["reads"][kind]["uncovered"]) for kind in ("total",) + READ_KINDS
           if rc["reads"][kind]["uncovered"]["pages"]]
    if not bad:
        out += ["", "No byte a modelled read took after a change is uncovered."]
    for kind, info in bad:
        out += ["", f"pages with read bytes not covered ({kind}): {info['pages']} pages in {info['ranges']} ranges; "
                "largest:"]
        out += [f"- 0x{r['paddr']:08x} + {r['pages']} pages" for r in info["largest"]]
    c = rc["counts"]
    out += ["", f"Draws: {c['draws'] + c['draws_indexed']} ({c['draws']} not indexed, {c['draws_indexed']} indexed); "
            f"texture reads (units and cube faces): {c['texture_reads']}; chained command buffer jumps: "
            f"{c['chained_list_jumps']}. SUBMIT_CMDLIST: {c['submit_cmdlist']}, of which {c['submit_cmdlist_do_flush']} "
            "have do_flush (word 7) nonzero: counted only, not credited as a flush, because Azahar ignores it and its "
            f"meaning on hardware is not checked here. CACHE_FLUSH: {c['cache_flush']}, of which "
            f"{c['cache_flush_equals_next_buffer']} flush exactly the buffer of the next SUBMIT_CMDLIST."]
    am = rc["alias_map"]
    o = rc["named_outside_windows"]
    out += ["", "GX commands whose named address is outside the GPU's windows: " + ", ".join(
        f"{kind} {o[kind]['reads']} ({o[kind]['bytes_modelled']} of {o[kind]['bytes']} bytes modelled)"
        for kind in NAMED_KINDS) + ". Azahar passes a command list or transfer address outside the windows through "
        "as a physical address, and so does this tool; it copies a DMA through the game's page table, which this "
        "tool follows through the alias runs " + ("of the alias map" if am["given"] else
                                                   "of an alias map (none was given, so such a DMA is not modelled)")
        + ". The bytes of such a read that map to no memory the capture holds are not modelled. A flush by an address "
        "outside the windows is mapped through the alias runs, if any: "
        f"{c['flushes_through_alias_runs']} flushes covered memory this way. Such a flush covers no byte whose "
        "address two runs map to different memory (the alias map is the union of the log's ALIAS sets): "
        f"{c['flushes_with_two_mappings']} flushes named such an address, whether or not they covered other bytes. "
        "The same-interval rule above credits only the windows."]
    if am["given"]:
        out += ["", f"Changes by region. Alias map: {am['runs']} runs (the union of the ALIAS sets of the log; a run "
                "seen in several sets counts once). An alias region is FCRAM the game reaches through a non-linear "
                "address (code, heap, stack, .data and the like), named by that address; 'FCRAM linear' is the rest "
                "of FCRAM, which the game reaches through the linear windows."]
    else:
        out += ["", "Changes by region. No alias map was given (--alias-log, with at least one ALIAS run): FCRAM is "
                "one region."]
    out += ["", "'page changes' counts each page with a changed byte in each CPU_PAGES record; 'pages' counts "
            "distinct pages. VRAM and the rest of FCRAM are always listed, even with no change."]
    out += ["", "| region | changed bytes | page changes | pages | flushed by the end | read by the GPU | "
            "replaced before a read | overwritten by the GPU | not read: in a read range (i) | "
            "not read: never in a read range (ii) |", "|---|---|---|---|---|---|---|---|---|---|"]
    for label, row in list(rc["regions"].items()) + [("total", rc["changes"])]:
        out.append(f"| {label} | {row['changed_bytes']} | {row['page_changes']} | {row['pages']} | "
                   f"{row['flushed_by_end']} | {row['read_by_gpu']} | {row['replaced']} | {row['overwritten_by_gpu']} | "
                   f"{row['not_read_in_read_range']} | {row['not_read_never_in_read_range']} |")
    t = rc["changes"]
    out += ["", "'Flushed by the end' counts the changed bytes any flush covered from the change's own interval to the "
            "end of the capture (an upper bound). Each changed byte ends in one of the last five columns: a GPU read "
            "took it, a later CPU change replaced it, a GPU write overwrote it, or it was still pending at the end. "
            "The table below splits the last kind; the replaced and overwritten changes were not read either.", "",
            "| changes still pending at the end (no read took them, no later CPU change or GPU write replaced them) "
            "| bytes | flushed by the end |", "|---|---|---|",
            f"| (i) in a read range of the capture, changed after its last read (data for a read after the capture) | "
            f"{t['not_read_in_read_range']} | {t['not_read_in_read_range_flushed']} |",
            f"| (ii) never in a read range of the capture | {t['not_read_never_in_read_range']} | "
            f"{t['not_read_never_in_read_range_flushed']} |"]
    return out


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
        if name == "capture":
            p.add_argument("--alias-log", metavar="LOG", help="a --check-coherence log: its ALIAS runs split FCRAM "
                                                              "into regions in the read-based section and map a DMA "
                                                              "source or a flush outside the GPU's windows")
    args = ap.parse_args(argv)
    reports: dict[str, dict] = {}
    try:
        alias_runs = None
        if args.cmd == "capture" and args.alias_log:
            with open(args.alias_log, encoding="utf-8") as f:
                alias_runs = load_alias_runs(f)
        for path in args.files:
            if args.cmd == "capture":
                reports[path] = analyse_capture(gcap.read(path), args.top, alias_runs)
            else:
                with open(path, encoding="utf-8") as f:
                    reports[path] = analyse_log(f, args.top)
    except (gcap.GcapError, OSError, ValueError, KeyError) as e:
        print(f"coherence: {e}", file=sys.stderr)
        return 1
    return _emit(args, reports, format_capture if args.cmd == "capture" else format_runlog)


if __name__ == "__main__":
    sys.exit(main())
