#!/usr/bin/env python3
"""Reader, checker, summariser and writer for MGPU GPU captures (decision D23).

The byte-level format is defined in docs/native/mgpu-format.md; this module
implements it for format version 1.  Standard library only.

    python3 tools/native/gcap.py summary FILE [--json]
    python3 tools/native/gcap.py check FILE

Library use::

    for rec in gcap.read(path): ...
    w = gcap.Writer(path); w.init(); w.mem(0x20000000, data); w.close()
    mem = gcap.memory_at(records, index); regs = gcap.registers_at(records, index)

Captures hold game data (textures, shaders, framebuffers): they live under
build/ only and never in the repository.  This tool prints counts, ids and
statistics, never buffer contents.
"""
from __future__ import annotations

import argparse
import io
import json
import statistics
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from typing import BinaryIO, Iterable, Iterator, Sequence

MAGIC = b"MGPU"
VERSION = 1
SUPPORTED_VERSIONS = (1,)

FILE_HEADER = struct.Struct("<4sI")
REC_HEADER = struct.Struct("<IIQI")  # type, frame, ticks, size

PAGE_SIZE = 4096

# Record types.
INIT, MEM, CPU_PAGES, GX, PICA_REG, GPU_PAGES, MMIO, SWAP, FILL, IRQ, FLUSH, FRAME = range(1, 13)
TYPE_NAMES = {
    INIT: "INIT", MEM: "MEM", CPU_PAGES: "CPU_PAGES", GX: "GX", PICA_REG: "PICA_REG",
    GPU_PAGES: "GPU_PAGES", MMIO: "MMIO", SWAP: "SWAP", FILL: "FILL", IRQ: "IRQ",
    FLUSH: "FLUSH", FRAME: "FRAME",
}

# INIT block kinds.
(K_GPU_REGS, K_LCD_REGS, K_PICA_REGS, K_VS_SETUP, K_GS_SETUP, K_DEFAULT_ATTRS,
 K_LIGHTING_LUT, K_FOG_LUT, K_PROCTEX, K_IMMEDIATE, K_GEOMETRY_PIPELINE) = range(1, 12)

_SETUP_SIZE = 12 + 4096 * 4 + 4096 * 4 + 96 * 16 + 16 + 16 + 20
INIT_BLOCK_SIZES = {
    K_GPU_REGS: 0x432 * 4,
    K_LCD_REGS: 0x400 * 4,
    K_PICA_REGS: 0x300 * 4,
    K_VS_SETUP: _SETUP_SIZE,
    K_GS_SETUP: _SETUP_SIZE,
    K_DEFAULT_ATTRS: 16 * 16,
    K_LIGHTING_LUT: 24 * 256 * 4,
    K_FOG_LUT: 128 * 4,
    K_PROCTEX: (128 * 3 + 256 * 2) * 4,
    K_IMMEDIATE: 16 * 16 + 8 + 20,
    K_GEOMETRY_PIPELINE: 8 * 4 + 16 * 16,
}
INIT_BLOCK_NAMES = {
    K_GPU_REGS: "GPU_REGS", K_LCD_REGS: "LCD_REGS", K_PICA_REGS: "PICA_REGS",
    K_VS_SETUP: "VS_SETUP", K_GS_SETUP: "GS_SETUP", K_DEFAULT_ATTRS: "DEFAULT_ATTRS",
    K_LIGHTING_LUT: "LIGHTING_LUT", K_FOG_LUT: "FOG_LUT", K_PROCTEX: "PROCTEX",
    K_IMMEDIATE: "IMMEDIATE", K_GEOMETRY_PIPELINE: "GEOMETRY_PIPELINE",
}
NUM_PICA_REGS = 0x300

# GX command ids (Service::GSP::CommandId) and FLUSH kinds.
GX_NAMES = {0: "REQUEST_DMA", 1: "SUBMIT_CMDLIST", 2: "MEMORY_FILL", 3: "DISPLAY_TRANSFER",
            4: "TEXTURE_COPY", 5: "CACHE_FLUSH"}
GX_SUBMIT_CMDLIST = 1
FLUSH_NAMES = {1: "FlushDataCache", 2: "InvalidateDataCache", 3: "StoreDataCache",
               4: "CacheFlushRegion"}
IRQ_NAMES = {0: "PSC0", 1: "PSC1", 2: "PDC0", 3: "PDC1", 4: "PPF", 5: "P3D", 6: "DMA"}

REG_FOG_MODE = 0x0E0  # texturing.fog_mode, bits 0-2
FOG_NAMES = {0: "none", 5: "fog", 7: "gas"}


class GcapError(Exception):
    """A capture is malformed, truncated or of an unknown version."""


@dataclass(frozen=True)
class Record:
    type: int
    frame: int
    ticks: int
    payload: bytes

    @property
    def name(self) -> str:
        return TYPE_NAMES.get(self.type, f"type{self.type}")


# --------------------------------------------------------------------------
# Reading

def _read_exact(f: BinaryIO, n: int, what: str) -> bytes:
    data = f.read(n)
    if len(data) != n:
        raise GcapError(f"truncated: {what} needs {n} bytes, got {len(data)}")
    return data


def read_stream(f: BinaryIO) -> Iterator[Record]:
    """Yield the records of an open capture; raise GcapError on any damage.

    Errors are raised when the damaged record is reached, so the records
    before it have already been yielded.
    """
    head = f.read(FILE_HEADER.size)
    if len(head) < FILE_HEADER.size:
        raise GcapError("truncated: file header")
    magic, version = FILE_HEADER.unpack(head)
    if magic != MAGIC:
        raise GcapError(f"wrong magic {magic!r}, expected {MAGIC!r}")
    if version not in SUPPORTED_VERSIONS:
        raise GcapError(f"unsupported format version {version} (known: {SUPPORTED_VERSIONS})")
    index = 0
    while True:
        h = f.read(REC_HEADER.size)
        if not h:
            return
        if len(h) < REC_HEADER.size:
            raise GcapError(f"truncated: record {index} header")
        rtype, frame, ticks, size = REC_HEADER.unpack(h)
        payload = _read_exact(f, size, f"record {index} payload")
        yield Record(rtype, frame, ticks, payload)
        index += 1


def read(path: str) -> Iterator[Record]:
    """Iterate over the records of the capture at `path`."""
    with open(path, "rb") as f:
        yield from read_stream(f)


def read_all(path: str) -> list[Record]:
    return list(read(path))


# --------------------------------------------------------------------------
# Payload decoding

def parse_init(payload: bytes) -> list[tuple[int, bytes]]:
    """Split an INIT payload into (kind, bytes) blocks."""
    if len(payload) < 4:
        raise GcapError("INIT: payload shorter than the block count")
    (count,) = struct.unpack_from("<I", payload, 0)
    pos, blocks = 4, []
    for i in range(count):
        if pos + 8 > len(payload):
            raise GcapError(f"INIT: block {i} header past the end")
        kind, size = struct.unpack_from("<II", payload, pos)
        pos += 8
        if pos + size > len(payload):
            raise GcapError(f"INIT: block {i} (kind {kind}) past the end")
        blocks.append((kind, payload[pos:pos + size]))
        pos += size
    if pos != len(payload):
        raise GcapError(f"INIT: {len(payload) - pos} trailing bytes")
    return blocks


def parse_mem(payload: bytes) -> tuple[int, bytes]:
    if len(payload) < 8:
        raise GcapError("MEM: payload shorter than its header")
    addr, size = struct.unpack_from("<II", payload, 0)
    if len(payload) != 8 + size:
        raise GcapError(f"MEM: size field {size} does not match payload {len(payload) - 8}")
    return addr, payload[8:]


def parse_pages(payload: bytes, name: str = "CPU_PAGES") -> list[tuple[int, bytes]]:
    if len(payload) < 4:
        raise GcapError(f"{name}: payload shorter than the count")
    (count,) = struct.unpack_from("<I", payload, 0)
    if len(payload) != 4 + count * (4 + PAGE_SIZE):
        raise GcapError(f"{name}: payload size does not match count {count}")
    pages, pos = [], 4
    for _ in range(count):
        (addr,) = struct.unpack_from("<I", payload, pos)
        if addr % PAGE_SIZE:
            raise GcapError(f"{name}: address 0x{addr:08x} not page aligned")
        pages.append((addr, payload[pos + 4:pos + 4 + PAGE_SIZE]))
        pos += 4 + PAGE_SIZE
    return pages


def parse_gx(payload: bytes) -> tuple[int, tuple[int, ...]]:
    """Return (command id, the 8 words)."""
    if len(payload) != 32:
        raise GcapError(f"GX: payload is {len(payload)} bytes, expected 32")
    words = struct.unpack("<8I", payload)
    return words[0] & 0xFF, words


def parse_pica_reg(payload: bytes) -> list[tuple[int, int, int]]:
    """Return [(register, byte mask, value)]."""
    if len(payload) < 4:
        raise GcapError("PICA_REG: payload shorter than the count")
    (count,) = struct.unpack_from("<I", payload, 0)
    if len(payload) != 4 + 8 * count:
        raise GcapError(f"PICA_REG: payload size does not match count {count}")
    out = []
    for i in range(count):
        reg, mask, zero, value = struct.unpack_from("<HBBI", payload, 4 + 8 * i)
        if zero != 0:
            raise GcapError(f"PICA_REG: entry {i} has a nonzero pad byte")
        if mask > 0xF:
            raise GcapError(f"PICA_REG: entry {i} has byte mask 0x{mask:02x}")
        if reg >= NUM_PICA_REGS:
            raise GcapError(f"PICA_REG: entry {i} register 0x{reg:03x} out of range")
        out.append((reg, mask, value))
    return out


def parse_irq(payload: bytes) -> tuple[int, int]:
    if len(payload) != 12:
        raise GcapError(f"IRQ: payload is {len(payload)} bytes, expected 12")
    return struct.unpack("<IQ", payload)


# Fixed-size payloads: type -> size.
_FIXED_SIZES = {GX: 32, MMIO: 8, SWAP: 32, FILL: 4, IRQ: 12, FLUSH: 12, FRAME: 4}


def check_record(rec: Record) -> None:
    """Validate one record's payload on its own; raise GcapError."""
    t = rec.type
    if t not in TYPE_NAMES:
        raise GcapError(f"unknown record type {t}")
    p = rec.payload
    if t in _FIXED_SIZES:
        if len(p) != _FIXED_SIZES[t]:
            raise GcapError(f"{rec.name}: payload is {len(p)} bytes, expected {_FIXED_SIZES[t]}")
    if t == INIT:
        prev_kind = 0
        for kind, data in parse_init(p):
            if kind not in INIT_BLOCK_SIZES:
                raise GcapError(f"INIT: unknown block kind {kind}")
            if kind <= prev_kind:
                raise GcapError(f"INIT: block kind {kind} after kind {prev_kind}; "
                                f"kinds must appear once, in increasing order")
            prev_kind = kind
            if len(data) != INIT_BLOCK_SIZES[kind]:
                raise GcapError(f"INIT: block {INIT_BLOCK_NAMES[kind]} is {len(data)} bytes, "
                                f"expected {INIT_BLOCK_SIZES[kind]}")
        missing = sorted(set(INIT_BLOCK_SIZES) - {k for k, _ in parse_init(p)})
        if missing:
            raise GcapError("INIT: missing block kinds "
                            + ", ".join(f"{k} ({INIT_BLOCK_NAMES[k]})" for k in missing))
    elif t == MEM:
        parse_mem(p)
    elif t in (CPU_PAGES, GPU_PAGES):
        parse_pages(p, rec.name)
    elif t == PICA_REG:
        parse_pica_reg(p)
    elif t == FLUSH:
        kind = struct.unpack_from("<I", p, 0)[0]
        if kind not in FLUSH_NAMES:
            raise GcapError(f"FLUSH: unknown kind {kind}")
    elif t == FRAME:
        if struct.unpack("<I", p)[0] != rec.frame:
            raise GcapError(f"FRAME: payload {struct.unpack('<I', p)[0]} differs from "
                            f"the header frame {rec.frame}")


def check_records(records: Iterable[Record]) -> list[str]:
    """Structural problems of a record sequence (empty list: valid)."""
    problems: list[str] = []
    last_frame = None
    prev_type = None
    prev_was_submit = False
    n = 0
    for i, rec in enumerate(records):
        n += 1
        try:
            check_record(rec)
        except GcapError as e:
            problems.append(f"record {i}: {e}")
        if i == 0 and rec.type != INIT:
            problems.append("record 0: the first record must be INIT")
        if i > 0 and rec.type == INIT:
            problems.append(f"record {i}: INIT only allowed as the first record")
        if last_frame is not None and rec.frame < last_frame:
            problems.append(f"record {i}: frame {rec.frame} goes back from {last_frame}")
        last_frame = rec.frame
        if prev_was_submit and rec.type != PICA_REG:
            problems.append(f"record {i}: command-list GX not followed by PICA_REG "
                            f"(got {rec.name})")
        if rec.type == PICA_REG and not prev_was_submit:
            problems.append(f"record {i}: PICA_REG does not follow a command-list GX "
                            f"(follows {TYPE_NAMES.get(prev_type, prev_type)})")
        prev_was_submit = False
        if rec.type == GX and len(rec.payload) == 32:
            prev_was_submit = (rec.payload[0] == GX_SUBMIT_CMDLIST)
        prev_type = rec.type
    if n == 0:
        problems.append("no records")
    elif prev_was_submit:
        problems.append("last record: command-list GX not followed by PICA_REG")
    return problems


# --------------------------------------------------------------------------
# State reconstruction

class Memory:
    """Sparse physical memory: 4096-byte pages, unwritten bytes read as zero."""

    def __init__(self) -> None:
        self.pages: dict[int, bytearray] = {}

    def write(self, addr: int, data: bytes) -> None:
        pos = 0
        while pos < len(data):
            page = (addr + pos) & ~(PAGE_SIZE - 1)
            off = (addr + pos) - page
            n = min(PAGE_SIZE - off, len(data) - pos)
            buf = self.pages.get(page)
            if buf is None:
                buf = self.pages[page] = bytearray(PAGE_SIZE)
            buf[off:off + n] = data[pos:pos + n]
            pos += n

    def read(self, addr: int, size: int) -> bytes:
        out = bytearray()
        pos = 0
        while pos < size:
            page = (addr + pos) & ~(PAGE_SIZE - 1)
            off = (addr + pos) - page
            n = min(PAGE_SIZE - off, size - pos)
            buf = self.pages.get(page)
            out += buf[off:off + n] if buf is not None else bytes(n)
            pos += n
        return bytes(out)

    def has_page(self, addr: int) -> bool:
        return (addr & ~(PAGE_SIZE - 1)) in self.pages


def memory_at(records: Sequence[Record], index: int) -> Memory:
    """The memory before record `index`: MEM, CPU_PAGES and GPU_PAGES applied in order."""
    mem = Memory()
    for rec in records[:index]:
        if rec.type == MEM:
            addr, data = parse_mem(rec.payload)
            mem.write(addr, data)
        elif rec.type in (CPU_PAGES, GPU_PAGES):
            for addr, data in parse_pages(rec.payload, rec.name):
                mem.write(addr, data)
    return mem


def mask_expand(mask: int) -> int:
    """Byte-enable mask (bit n = byte n) to a 32-bit bit mask."""
    return sum(0xFF << (8 * n) for n in range(4) if mask & (1 << n))


def registers_at(records: Sequence[Record], index: int) -> list[int]:
    """The PICA internal register file before record `index`.

    Starts from the INIT record's PICA_REGS block and replays the PICA_REG
    writes before `index`, honouring byte masks.
    """
    regs: list[int] | None = None
    for rec in records[:index]:
        if rec.type == INIT:
            for kind, data in parse_init(rec.payload):
                if kind == K_PICA_REGS:
                    if len(data) != NUM_PICA_REGS * 4:
                        raise GcapError("INIT: PICA_REGS block has the wrong size")
                    regs = list(struct.unpack(f"<{NUM_PICA_REGS}I", data))
        elif rec.type == PICA_REG:
            if regs is None:
                raise GcapError("PICA_REG before an INIT with a PICA_REGS block")
            for reg, mask, value in parse_pica_reg(rec.payload):
                m = mask_expand(mask)
                regs[reg] = (regs[reg] & ~m & 0xFFFFFFFF) | (value & m)
    if regs is None:
        raise GcapError("no INIT with a PICA_REGS block before the requested record")
    return regs


# --------------------------------------------------------------------------
# Summary

def summarize(records: Sequence[Record]) -> dict:
    counts: Counter[str] = Counter()
    gx: Counter[str] = Counter()
    fog: Counter[str] = Counter()
    irq: dict[int, list[int]] = {}
    frames: list[int] = []
    for rec in records:
        check_record(rec)
        counts[rec.name] += 1
        if rec.type == GX:
            gid, _ = parse_gx(rec.payload)
            gx[GX_NAMES.get(gid, f"id{gid}")] += 1
        elif rec.type == PICA_REG:
            for reg, mask, value in parse_pica_reg(rec.payload):
                if reg == REG_FOG_MODE and mask & 1:
                    mode = value & 7
                    fog[FOG_NAMES.get(mode, f"other({mode})")] += 1
        elif rec.type == IRQ:
            iid, delay = parse_irq(rec.payload)
            irq.setdefault(iid, []).append(delay)
        elif rec.type == FRAME:
            frames.append(struct.unpack("<I", rec.payload)[0])
    irq_stats = {}
    for iid, delays in sorted(irq.items()):
        med = statistics.median(delays)
        irq_stats[IRQ_NAMES.get(iid, f"id{iid}")] = {
            "id": iid, "count": len(delays), "min_ns": min(delays),
            "median_ns": int(med) if med == int(med) else med, "max_ns": max(delays),
        }
    return {
        "records": len(records),
        "counts": dict(sorted(counts.items(), key=lambda kv: next(
            (t for t, n in TYPE_NAMES.items() if n == kv[0]), 99))),
        "gx": dict(sorted(gx.items())),
        "frames": {"count": len(frames), "first": frames[0] if frames else None,
                   "last": frames[-1] if frames else None},
        "fog_modes": dict(sorted(fog.items())),
        "irq": irq_stats,
    }


def format_summary(s: dict) -> str:
    lines = [f"records: {s['records']}", "per type:"]
    lines += [f"  {k}: {v}" for k, v in s["counts"].items()]
    lines.append("GX commands:")
    lines += [f"  {k}: {v}" for k, v in s["gx"].items()] or ["  (none)"]
    fr = s["frames"]
    lines.append(f"frames: {fr['count']} (first {fr['first']}, last {fr['last']})")
    lines.append("fog modes set:")
    lines += [f"  {k}: {v}" for k, v in s["fog_modes"].items()] or ["  (none)"]
    lines.append("IRQ delays (ns):")
    for name, st in s["irq"].items():
        lines.append(f"  {name} (id {st['id']}): count={st['count']} min={st['min_ns']} "
                     f"median={st['median_ns']} max={st['max_ns']}")
    if not s["irq"]:
        lines.append("  (none)")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Writing

class Writer:
    """Writes an MGPU capture.  `frame` and `ticks` stamp the records that follow."""

    def __init__(self, target: str | BinaryIO, version: int = VERSION) -> None:
        self._own = isinstance(target, str)
        self.f: BinaryIO = open(target, "wb") if isinstance(target, str) else target
        self.frame = 0
        self.ticks = 0
        self.f.write(FILE_HEADER.pack(MAGIC, version))

    def record(self, rtype: int, payload: bytes = b"", frame: int | None = None,
               ticks: int | None = None) -> None:
        self.f.write(REC_HEADER.pack(rtype, self.frame if frame is None else frame,
                                     self.ticks if ticks is None else ticks, len(payload)))
        self.f.write(payload)

    def init(self, blocks: dict[int, bytes] | None = None, **kw) -> None:
        """INIT with zero-filled blocks of every kind, overridden by `blocks`."""
        full = {k: bytes(n) for k, n in INIT_BLOCK_SIZES.items()}
        full.update(blocks or {})
        self.record(INIT, build_init(full), **kw)

    def mem(self, addr: int, data: bytes, **kw) -> None:
        self.record(MEM, struct.pack("<II", addr, len(data)) + data, **kw)

    def cpu_pages(self, pages: Sequence[tuple[int, bytes]], **kw) -> None:
        self.record(CPU_PAGES, build_pages(pages), **kw)

    def gpu_pages(self, pages: Sequence[tuple[int, bytes]], **kw) -> None:
        self.record(GPU_PAGES, build_pages(pages), **kw)

    def gx(self, words: Sequence[int], **kw) -> None:
        self.record(GX, struct.pack("<8I", *words), **kw)

    def submit_cmdlist(self, addr: int, size: int, flags: int = 0, do_flush: int = 0,
                       **kw) -> None:
        self.gx([GX_SUBMIT_CMDLIST, addr, size, flags, 0, 0, 0, do_flush], **kw)

    def pica_reg(self, writes: Sequence[tuple[int, int, int]], **kw) -> None:
        """writes: (register, byte mask, value)."""
        body = struct.pack("<I", len(writes))
        for reg, mask, value in writes:
            body += struct.pack("<HBBI", reg, mask, 0, value)
        self.record(PICA_REG, body, **kw)

    def mmio(self, addr: int, value: int, **kw) -> None:
        self.record(MMIO, struct.pack("<II", addr, value), **kw)

    def swap(self, screen: int, info: Sequence[int], **kw) -> None:
        self.record(SWAP, struct.pack("<8I", screen, *info), **kw)

    def fill(self, word: int, **kw) -> None:
        self.record(FILL, struct.pack("<I", word), **kw)

    def irq(self, irq_id: int, delay_ns: int, **kw) -> None:
        self.record(IRQ, struct.pack("<IQ", irq_id, delay_ns), **kw)

    def flush(self, kind: int, vaddr: int, size: int, **kw) -> None:
        self.record(FLUSH, struct.pack("<III", kind, vaddr, size), **kw)

    def frame_marker(self, number: int, **kw) -> None:
        """A FRAME record; also sets the frame stamp of the records that follow."""
        self.frame = number
        self.record(FRAME, struct.pack("<I", number), **kw)

    def close(self) -> None:
        if self._own:
            self.f.close()

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def build_init(blocks: dict[int, bytes]) -> bytes:
    out = struct.pack("<I", len(blocks))
    for kind in sorted(blocks):
        out += struct.pack("<II", kind, len(blocks[kind])) + blocks[kind]
    return out


def build_pages(pages: Sequence[tuple[int, bytes]]) -> bytes:
    out = struct.pack("<I", len(pages))
    for addr, data in pages:
        if len(data) != PAGE_SIZE:
            raise ValueError("a page is exactly 4096 bytes")
        out += struct.pack("<I", addr) + data
    return out


def pica_regs_block(values: dict[int, int] | None = None) -> bytes:
    """A PICA_REGS INIT block, zero except for `values` {register: word}."""
    regs = [0] * NUM_PICA_REGS
    for r, v in (values or {}).items():
        regs[r] = v
    return struct.pack(f"<{NUM_PICA_REGS}I", *regs)


def to_bytes(records: Iterable[Record], version: int = VERSION) -> bytes:
    """Serialise records (testing helper)."""
    buf = io.BytesIO()
    w = Writer(buf, version)
    for r in records:
        w.record(r.type, r.payload, frame=r.frame, ticks=r.ticks)
    return buf.getvalue()


# --------------------------------------------------------------------------
# Command line

def check_file(path: str) -> list[str]:
    """All problems of the capture at `path`, including read errors."""
    records: list[Record] = []
    try:
        for rec in read(path):
            records.append(rec)
    except GcapError as e:
        return check_records(records) + [str(e)]
    return check_records(records)


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="MGPU capture tool")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary", help="counts, GX kinds, frames, fog modes, IRQ delays")
    s.add_argument("file")
    s.add_argument("--json", action="store_true")
    c = sub.add_parser("check", help="structural validity; non-zero exit if broken")
    c.add_argument("file")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "summary":
            s_ = summarize(read_all(args.file))
            print(json.dumps(s_, indent=2) if args.json else format_summary(s_))
            return 0
        problems = check_file(args.file)
    except (GcapError, OSError) as e:
        print(f"gcap: {e}", file=sys.stderr)
        return 1
    if problems:
        for p in problems:
            print(f"gcap: {p}", file=sys.stderr)
        return 1
    print("gcap: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
