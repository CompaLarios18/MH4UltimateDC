#!/usr/bin/env python3
# Derived from Azahar src/audio_core/hle/shared_memory.h, hle.cpp, common.h, audio_types.h and
# src/core/hle/service/hid/hid.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (all say
# "Licensed under GPLv2 or any later version", so GPL-2.0-or-later): the offsets of the DSP
# shared-memory regions and of the source status blocks, the number of sources (common.h), the sample
# rate and samples per frame (audio_types.h), the ticks of an audio frame and the rule that picks
# the region the DSP reads (hle.cpp, CurrentRegionIndex), the layout of the HID pad and touch
# entries.  Also read for the IR header: Azahar src/core/hle/service/ir/ir_user.cpp (GPLv2 or any
# later version).
# Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.h)
# Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (ir_user.cpp)
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
# Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
# Copyright 2017 Citra Emulator Project (audio_types.h)
# Copyright 2016 Citra Emulator Project (common.h)
"""Reader, summariser, comparator and writer for the DSP (MDSP) and HID (MHID) captures.

Written by ``mh4u_runner --capture-dsp START:FRAMES:FILE`` and ``--capture-hid FILE`` (N0-06) and,
later, by the native runtime (N1-16, N1-18); this tool compares the two.  Standard library only.

    python3 tools/native/capfmt.py summary FILE [--json]
    python3 tools/native/capfmt.py check FILE
    python3 tools/native/capfmt.py pcm-to-wav FILE OUT.wav
    python3 tools/native/capfmt.py diff-hid REF CAND
    python3 tools/native/capfmt.py diff-dsp REF CAND [--frames N]

Exit status: 0 equal / fine, 1 different (or a broken capture for ``check``), 2 a file that is not a
capture of the right kind or cannot be read.

Format (little-endian), the common binary format of docs/native/wp/N0.md: magic (``MDSP`` or
``MHID``), u32 version (1), then records ``{u32 type, u32 frame, u64 ticks, u32 size, payload}``.
``frame`` is the game's frame counter (the u32 at 0x010571d4) when the record was written; ``ticks``
are ARM11 ticks since the run began.  A sibling ``<file>.json`` holds the manifest (tool, version,
game, frames, record counts).

MDSP records (one audio frame = 160 stereo samples = 1,310,720 ARM11 ticks, at 32,728 Hz):
  1  the two DSP shared-memory regions (0x8000 bytes each, region 0 then region 1) as the HLE DSP
     sees them at the start of the audio frame; the u16 at 0x7ffe of each is the DSP's own frame
     counter, which the comparison is keyed on
  2  the 160 stereo s16 samples of the frame's output (640 bytes), taken before the sink
  3  sample data the DSP read: u32 physical address, u32 size, u32 CRC-32 of the data, then the
     bytes (only the first time a given (address, size, CRC) appears; later records stop after the
     CRC)
MHID records (one set per game frame):
  1  the 0x2b0 bytes of HID shared memory (pad at 0x00, touch at 0xa8, accelerometer 0x108,
     gyroscope 0x158, debug pad 0x238)
  2  the IR (Circle Pad Pro) state: the first bytes (at most 0x200) of the ir:USER shared memory,
     16-byte header first (latest receive error, latest send error, connection status, trying to
     connect status, connection role, machine id, connected, network id, initialized, unknown) and
     the receive buffer's info after it; absent while no game has set ir:USER up

Captures hold game data (the game's audio, its input): they live under build/ only and never in
the repository.  This tool prints counts, frame numbers, buttons and statuses, never samples or
sample data.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import wave
import zlib
from dataclasses import dataclass
from typing import BinaryIO, Iterator, Sequence

MAGIC_DSP = b"MDSP"
MAGIC_HID = b"MHID"
VERSION = 1
FILE_HEADER = struct.Struct("<4sI")
REC_HEADER = struct.Struct("<IIQI")  # type, frame, ticks, size

KINDS = {MAGIC_DSP: "dsp", MAGIC_HID: "hid"}
MAGICS = {v: k for k, v in KINDS.items()}

# DSP records.
DSP_REGIONS, DSP_PCM, DSP_SAMPLES = 1, 2, 3
# HID records.
HID_SHARED, HID_IR = 1, 2

SAMPLE_RATE = 32728
SAMPLES_PER_FRAME = 160
DSP_FRAME_TICKS = 1310720
REGION_SIZE = 0x8000
REGIONS_SIZE = 2 * REGION_SIZE
PCM_SIZE = SAMPLES_PER_FRAME * 2 * 2
NUM_SOURCES = 24
STATUS_OFFSET = 0xD00  # SharedMemory::source_statuses
STATUS_SIZE = 12
COUNTER_OFFSET = 0x7FFE  # SharedMemory::frame_counter
SAMPLE_READ_HEAD = 12

HID_SIZE = 0x2B0
HID_PAD = 0x00
HID_TOUCH = 0xA8
IR_MAX = 0x200

# (name, offset, size) inside one source status block.
STATUS_FIELDS = (("enabled", 0, 1), ("dirty", 1, 1), ("sync_count", 2, 2),
                 ("buffer_position", 4, 4), ("current_buffer_id", 8, 2), ("last_buffer_id", 10, 2))

BUTTONS = ("A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y", "DEBUG", "GPIO14")
BUTTON_BITS = {name: 1 << i for i, name in enumerate(BUTTONS)}
BUTTON_BITS.update({"CIRCLE_RIGHT": 1 << 28, "CIRCLE_LEFT": 1 << 29, "CIRCLE_UP": 1 << 30, "CIRCLE_DOWN": 1 << 31})

TYPE_NAMES = {
    "dsp": {DSP_REGIONS: "REGIONS", DSP_PCM: "PCM", DSP_SAMPLES: "SAMPLES"},
    "hid": {HID_SHARED: "HID", HID_IR: "IR"},
}


class CapError(Exception):
    """A capture that is not valid, or not of the kind the command wants."""


@dataclass
class Record:
    type: int
    frame: int
    ticks: int
    payload: bytes


# ── reading ───────────────────────────────────────────────────────────────────────────


def _read_exact(f: BinaryIO, n: int, what: str) -> bytes:
    data = f.read(n)
    if len(data) != n:
        raise CapError(f"truncated capture: {what} needs {n} bytes, {len(data)} left")
    return data


def read_stream(f: BinaryIO) -> Iterator[Record]:
    """Records of an open capture; the first item is checked (magic, version) before any record."""
    head = f.read(FILE_HEADER.size)
    if len(head) < FILE_HEADER.size:
        raise CapError("not a capture: shorter than the file header")
    magic, version = FILE_HEADER.unpack(head)
    if magic not in KINDS:
        raise CapError(f"not a capture: magic {magic!r} is neither MDSP nor MHID")
    if version != VERSION:
        raise CapError(f"unsupported capture version {version} (this tool reads {VERSION})")
    kind = KINDS[magic]
    while True:
        first = f.read(1)
        if not first:
            return
        rest = _read_exact(f, REC_HEADER.size - 1, "a record header")
        rtype, frame, ticks, size = REC_HEADER.unpack(first + rest)
        payload = _read_exact(f, size, f"a {size}-byte payload")
        rec = Record(rtype, frame, ticks, payload)
        check_record(kind, rec)
        yield rec


def file_kind(path: str) -> str:
    """'dsp' or 'hid', from the magic."""
    try:
        with open(path, "rb") as f:
            head = f.read(FILE_HEADER.size)
    except OSError as e:
        raise CapError(f"cannot read {path}: {e.strerror}") from e
    if len(head) < FILE_HEADER.size:
        raise CapError(f"{path}: not a capture")
    magic, version = FILE_HEADER.unpack(head)
    if magic not in KINDS:
        raise CapError(f"{path}: not a capture (magic {magic!r})")
    if version != VERSION:
        raise CapError(f"{path}: unsupported capture version {version}")
    return KINDS[magic]


def read(path: str) -> Iterator[Record]:
    """Records of the capture at PATH, validated one by one (CapError on the first bad one)."""
    try:
        f = open(path, "rb")
    except OSError as e:
        raise CapError(f"cannot read {path}: {e.strerror}") from e
    with f:
        yield from read_stream(f)


def read_all(path: str) -> list[Record]:
    return list(read(path))


def check_record(kind: str, rec: Record) -> None:
    """Structure of one record (sizes, the CRC of sample data); CapError otherwise."""
    n = len(rec.payload)
    name = TYPE_NAMES[kind].get(rec.type)
    if name is None:
        raise CapError(f"unknown {kind} record type {rec.type} (frame {rec.frame})")
    if kind == "dsp":
        if rec.type == DSP_REGIONS and n != REGIONS_SIZE:
            raise CapError(f"REGIONS record has {n} bytes, wants {REGIONS_SIZE} (frame {rec.frame})")
        if rec.type == DSP_PCM and n != PCM_SIZE:
            raise CapError(f"PCM record has {n} bytes, wants {PCM_SIZE} (frame {rec.frame})")
        if rec.type == DSP_SAMPLES:
            if n < SAMPLE_READ_HEAD:
                raise CapError(f"SAMPLES record has {n} bytes, wants at least {SAMPLE_READ_HEAD}")
            _, size, crc = struct.unpack_from("<III", rec.payload)
            if n != SAMPLE_READ_HEAD and n != SAMPLE_READ_HEAD + size:
                raise CapError(f"SAMPLES record has {n} bytes for a read of {size} (frame {rec.frame})")
            if n != SAMPLE_READ_HEAD and zlib.crc32(rec.payload[SAMPLE_READ_HEAD:]) != crc:
                raise CapError(f"SAMPLES record at frame {rec.frame}: the CRC-32 does not match the bytes")
    else:
        if rec.type == HID_SHARED and n != HID_SIZE:
            raise CapError(f"HID record has {n} bytes, wants {HID_SIZE} (frame {rec.frame})")
        if rec.type == HID_IR and not 16 <= n <= IR_MAX:
            raise CapError(f"IR record has {n} bytes, wants 16 to {IR_MAX} (frame {rec.frame})")


# ── decoding ──────────────────────────────────────────────────────────────────────────


def region_counters(payload: bytes) -> tuple[int, int]:
    """The DSP frame counters of region 0 and region 1 of a REGIONS payload."""
    c0 = struct.unpack_from("<H", payload, COUNTER_OFFSET)[0]
    c1 = struct.unpack_from("<H", payload, REGION_SIZE + COUNTER_OFFSET)[0]
    return c0, c1


def current_region(c0: int, c1: int) -> int:
    """Which region the HLE DSP reads (hle.cpp CurrentRegionIndex): the higher counter, with wraparound."""
    if c0 == 0xFFFF and c1 != 0xFFFE:
        return 1
    if c1 == 0xFFFF and c0 != 0xFFFE:
        return 0
    return 0 if c0 > c1 else 1


def dsp_counter(payload: bytes) -> int:
    """The DSP's own frame counter for a REGIONS record: the counter of the region it reads."""
    counters = region_counters(payload)
    return counters[current_region(*counters)]


def source_statuses(payload: bytes, region: int) -> list[bytes]:
    """The 24 status blocks (12 bytes each) of REGION (0 or 1)."""
    base = region * REGION_SIZE + STATUS_OFFSET
    return [payload[base + i * STATUS_SIZE: base + (i + 1) * STATUS_SIZE] for i in range(NUM_SOURCES)]


def parse_sample_read(payload: bytes) -> tuple[int, int, int, bytes | None]:
    """(physical address, size, CRC-32, bytes or None when they were written by an earlier record)."""
    paddr, size, crc = struct.unpack_from("<III", payload)
    data = payload[SAMPLE_READ_HEAD:] if len(payload) > SAMPLE_READ_HEAD else None
    return paddr, size, crc, data


@dataclass
class HidState:
    buttons: int
    circle_x: int
    circle_y: int
    touch_x: int
    touch_y: int
    touch_valid: int

    def key(self) -> tuple[int, int, int, int, int, int]:
        return (self.buttons, self.circle_x, self.circle_y, self.touch_x, self.touch_y, self.touch_valid)

    def describe(self) -> str:
        names = [n for n, bit in BUTTON_BITS.items() if self.buttons & bit]
        return (f"buttons=0x{self.buttons:08x}[{','.join(names)}] circle=({self.circle_x},{self.circle_y}) "
                f"touch=({self.touch_x},{self.touch_y},valid={self.touch_valid})")


def hid_state(payload: bytes) -> HidState:
    """The newest pad entry (buttons, circle pad) and touch entry of a HID record.

    Pad: index at +0x10, entries at +0x28, 0x10 bytes each (state, additions, removals, s16 x, s16 y).
    Touch (at 0xa8): index at +0x10, entries at +0x20, 8 bytes each (u16 x, u16 y, u32 valid bits).
    """
    pad_index = struct.unpack_from("<I", payload, HID_PAD + 0x10)[0] % 8
    at = HID_PAD + 0x28 + 0x10 * pad_index
    buttons = struct.unpack_from("<I", payload, at)[0]
    circle_x, circle_y = struct.unpack_from("<hh", payload, at + 12)
    touch_index = struct.unpack_from("<I", payload, HID_TOUCH + 0x10)[0] % 8
    at = HID_TOUCH + 0x20 + 8 * touch_index
    tx, ty, valid = struct.unpack_from("<HHI", payload, at)
    return HidState(buttons, circle_x, circle_y, tx, ty, valid & 0x7F)


def ir_header(payload: bytes) -> dict[str, int]:
    """The named bytes of the ir:USER shared-memory header at the start of an IR record."""
    recv_err, send_err = struct.unpack_from("<II", payload)
    names = ("connection_status", "trying_to_connect_status", "connection_role", "machine_id",
             "connected", "network_id", "initialized", "unknown")
    out = {"latest_receive_error": recv_err, "latest_send_error": send_err}
    out.update({n: payload[8 + i] for i, n in enumerate(names)})
    return out


# ── writing (the runner writes the same bytes; tests use this) ─────────────────────────


class Writer:
    """Writes a capture and, on close, its manifest ``<path>.json``."""

    def __init__(self, path: str, kind: str, game: int = 0, tool: str = "mh4u_runner", manifest: bool = True):
        if kind not in MAGICS:
            raise CapError(f"unknown capture kind {kind!r}")
        self.path = path
        self.kind = kind
        self.game = game
        self.tool = tool
        self.manifest = manifest
        self.counts: dict[int, int] = {}
        self._seen: set[tuple[int, int, int]] = set()
        self._frames = 0
        self._f = open(path, "wb")
        self._f.write(FILE_HEADER.pack(MAGICS[kind], VERSION))

    def add(self, rtype: int, frame: int, ticks: int, payload: bytes) -> None:
        check_record(self.kind, Record(rtype, frame, ticks, bytes(payload)))
        self._f.write(REC_HEADER.pack(rtype, frame, ticks, len(payload)) + bytes(payload))
        self.counts[rtype] = self.counts.get(rtype, 0) + 1
        if rtype == 1:
            self._frames += 1

    # DSP
    def dsp_regions(self, frame: int, ticks: int, region0: bytes, region1: bytes) -> None:
        self.add(DSP_REGIONS, frame, ticks, region0 + region1)

    def dsp_pcm(self, frame: int, ticks: int, pcm: bytes) -> None:
        self.add(DSP_PCM, frame, ticks, pcm)

    def dsp_samples(self, frame: int, ticks: int, paddr: int, data: bytes) -> bool:
        """A sample read; the bytes go in only the first time this (address, size, CRC) is seen."""
        crc = zlib.crc32(data)
        key = (paddr, len(data), crc)
        head = struct.pack("<III", paddr, len(data), crc)
        first = key not in self._seen
        self._seen.add(key)
        self.add(DSP_SAMPLES, frame, ticks, head + (data if first else b""))
        return first

    # HID
    def hid(self, frame: int, ticks: int, shared: bytes) -> None:
        self.add(HID_SHARED, frame, ticks, shared)

    def ir(self, frame: int, ticks: int, state: bytes) -> None:
        self.add(HID_IR, frame, ticks, state)

    def close(self) -> None:
        if self._f.closed:
            return
        self._f.close()
        if self.manifest:
            write_manifest(self.path, self.tool, self.kind, self.game, self._frames, self.counts)

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def write_manifest(path: str, tool: str, kind: str, game: int, frames: int, counts: dict[int, int]) -> None:
    """``<path>.json``: tool, format, version, game (title id), frames (audio frames or game frames), record counts."""
    manifest = {
        "tool": tool,
        "format": MAGICS[kind].decode(),
        "version": VERSION,
        "game": f"0x{game:016x}",
        "frames": frames,
        "records": {str(k): counts[k] for k in sorted(counts)},
    }
    with open(path + ".json", "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


def make_status(enabled: int = 0, dirty: int = 0, sync_count: int = 0, buffer_position: int = 0,
                current_buffer_id: int = 0, last_buffer_id: int = 0) -> bytes:
    return struct.pack("<BBHIHH", enabled, dirty, sync_count, buffer_position, current_buffer_id, last_buffer_id)


def make_region(counter: int, statuses: dict[int, bytes] | None = None, fill: int = 0) -> bytes:
    """One 0x8000-byte region with COUNTER, the given status blocks (by source) and a FILL byte elsewhere."""
    region = bytearray([fill]) * REGION_SIZE
    struct.pack_into("<H", region, COUNTER_OFFSET, counter)
    for i in range(NUM_SOURCES):
        region[STATUS_OFFSET + i * STATUS_SIZE: STATUS_OFFSET + (i + 1) * STATUS_SIZE] = make_status()
    for source, block in (statuses or {}).items():
        region[STATUS_OFFSET + source * STATUS_SIZE: STATUS_OFFSET + (source + 1) * STATUS_SIZE] = block
    return bytes(region)


def make_hid(buttons: int = 0, circle: tuple[int, int] = (0, 0), touch: tuple[int, int, int] = (0, 0, 0),
             index: int = 0, touch_index: int = 0) -> bytes:
    """A 0x2b0-byte HID block whose newest pad entry and touch entry hold the given values."""
    data = bytearray(HID_SIZE)
    struct.pack_into("<I", data, HID_PAD + 0x10, index)
    struct.pack_into("<I", data, HID_PAD + 0x28 + 0x10 * index, buttons)
    struct.pack_into("<hh", data, HID_PAD + 0x28 + 0x10 * index + 12, *circle)
    struct.pack_into("<I", data, HID_TOUCH + 0x10, touch_index)
    struct.pack_into("<HHI", data, HID_TOUCH + 0x20 + 8 * touch_index, *touch)
    return bytes(data)


def make_pcm(samples: Sequence[tuple[int, int]]) -> bytes:
    """160 stereo samples; a shorter list is padded with silence."""
    frame = list(samples) + [(0, 0)] * (SAMPLES_PER_FRAME - len(samples))
    return b"".join(struct.pack("<hh", left, right) for left, right in frame[:SAMPLES_PER_FRAME])


# ── commands ──────────────────────────────────────────────────────────────────────────


def summarize(path: str) -> dict:
    kind = file_kind(path)
    records = read_all(path)
    counts: dict[int, int] = {}
    for r in records:
        counts[r.type] = counts.get(r.type, 0) + 1
    s: dict = {"kind": kind, "records": {TYPE_NAMES[kind][t]: counts[t] for t in sorted(counts)}}
    if records:
        s["first_frame"] = min(r.frame for r in records)
        s["last_frame"] = max(r.frame for r in records)
        s["last_ticks"] = max(r.ticks for r in records)
    if kind == "dsp":
        regions = [r for r in records if r.type == DSP_REGIONS]
        pcm = [r for r in records if r.type == DSP_PCM]
        reads = [parse_sample_read(r.payload) for r in records if r.type == DSP_SAMPLES]
        s["dsp_frames"] = len(regions)
        if regions:
            counters = [dsp_counter(r.payload) for r in regions]
            s["dsp_counter_first"] = counters[0]
            s["dsp_counter_last"] = counters[-1]
            s["sources_enabled_max"] = max(
                sum(1 for blk in source_statuses(r.payload, current_region(*region_counters(r.payload))) if blk[0])
                for r in regions)
        s["pcm_frames"] = len(pcm)
        s["pcm_non_silent_frames"] = sum(1 for r in pcm if any(r.payload))
        unique = {(p, n, c) for p, n, c, _ in reads}
        s["sample_reads"] = len(reads)
        s["sample_reads_unique"] = len(unique)
        s["sample_bytes_stored"] = sum(len(d) for _, _, _, d in reads if d is not None)
        s["sample_reads_without_bytes"] = sum(1 for _, _, _, d in reads if d is None)
    else:
        hid = [r for r in records if r.type == HID_SHARED]
        states = [hid_state(r.payload) for r in hid]
        s["hid_records"] = len(hid)
        s["hid_distinct_frames"] = len({r.frame for r in hid})
        s["frames_with_buttons"] = sum(1 for st in states if st.buttons & 0x0FFF)
        s["frames_with_touch"] = sum(1 for st in states if st.touch_valid)
        s["frames_with_circle_pad"] = sum(1 for st in states if st.circle_x or st.circle_y)
        pressed = 0
        for st in states:
            pressed |= st.buttons
        s["buttons_seen"] = [n for n, bit in BUTTON_BITS.items() if pressed & bit]
        ir = [ir_header(r.payload) for r in records if r.type == HID_IR]
        s["ir_records"] = len(ir)
        s["ir_connected_records"] = sum(1 for h in ir if h["connected"])
    return s


def format_summary(s: dict) -> str:
    lines = [f"kind: {s['kind']}"]
    lines.append("records: " + (", ".join(f"{k}={v}" for k, v in s["records"].items()) or "none"))
    if "first_frame" in s:
        lines.append(f"game frames: {s['first_frame']}..{s['last_frame']}, last ticks {s['last_ticks']}")
    for key, value in s.items():
        if key in ("kind", "records", "first_frame", "last_frame", "last_ticks"):
            continue
        lines.append(f"{key}: {', '.join(value) if isinstance(value, list) else value}")
    return "\n".join(lines)


def pcm_to_wav(path: str, out: str) -> int:
    """Writes every PCM record of a DSP capture, in order, as a 16-bit stereo WAV at 32,728 Hz."""
    if file_kind(path) != "dsp":
        raise CapError(f"{path}: not a DSP capture")
    frames = 0
    try:
        with wave.open(out, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            for r in read(path):
                if r.type == DSP_PCM:
                    w.writeframes(r.payload)  # s16 little-endian, as the wave module wants
                    frames += 1
    except OSError as e:
        raise CapError(f"cannot write {out}: {e.strerror}") from e
    return frames


def _load_kind(path: str, kind: str) -> list[Record]:
    if file_kind(path) != kind:
        raise CapError(f"{path}: not a {kind.upper()} capture")
    return read_all(path)


def diff_hid(ref: str, cand: str, limit: int = 10) -> list[str]:
    """Differences in buttons, circle pad and touch per game frame (ticks ignored); [] when equal."""
    def by_frame(path: str) -> dict[int, list[HidState]]:
        out: dict[int, list[HidState]] = {}
        for r in _load_kind(path, "hid"):
            if r.type == HID_SHARED:
                out.setdefault(r.frame, []).append(hid_state(r.payload))
        return out

    a, b = by_frame(ref), by_frame(cand)
    problems: list[str] = []
    for frame in sorted(set(a) | set(b)):
        if frame not in b:
            problems.append(f"frame {frame}: only in the reference")
        elif frame not in a:
            problems.append(f"frame {frame}: only in the candidate")
        elif [s.key() for s in a[frame]] != [s.key() for s in b[frame]]:
            if len(a[frame]) != len(b[frame]):
                problems.append(f"frame {frame}: {len(a[frame])} records in the reference, {len(b[frame])} in the candidate")
            else:
                for x, y in zip(a[frame], b[frame]):
                    if x.key() != y.key():
                        problems.append(f"frame {frame}: ref {x.describe()} cand {y.describe()}")
                        break
    total = len(problems)
    if total > limit:
        problems = problems[:limit] + [f"... and {total - limit} more frames differ"]
    return problems


def _dsp_frames(path: str) -> list[tuple[int, bytes]]:
    return [(dsp_counter(r.payload), r.payload) for r in _load_kind(path, "dsp") if r.type == DSP_REGIONS]


def _describe_status(block: bytes) -> str:
    return " ".join(f"{name}=0x{int.from_bytes(block[o:o + n], 'little'):0{2 * n}x}" for name, o, n in STATUS_FIELDS)


def diff_dsp(ref: str, cand: str, frames: int | None = None, limit: int = 10) -> list[str]:
    """Differences in the DSP frame counters and every source's status block, keyed on the DSP's own
    frame counter (not on game frames).  When one capture starts at a later DSP frame, the earlier
    frames of the other are skipped (and said); the first FRAMES DSP frames of what remains are
    compared.  [] when equal."""
    a, b = _dsp_frames(ref), _dsp_frames(cand)
    notes: list[str] = []
    if a and b:
        keys_a = [k for k, _ in a]
        keys_b = [k for k, _ in b]
        if keys_a[0] != keys_b[0]:
            if keys_b[0] in keys_a:
                skip = keys_a.index(keys_b[0])
                a = a[skip:]
                notes.append(f"the reference starts {skip} DSP frames earlier; they are skipped")
            elif keys_a[0] in keys_b:
                skip = keys_b.index(keys_a[0])
                b = b[skip:]
                notes.append(f"the candidate starts {skip} DSP frames earlier; they are skipped")
    if frames is not None:
        a, b = a[:frames], b[:frames]
    problems: list[str] = []
    for i in range(max(len(a), len(b))):
        if i >= len(b):
            problems.append(f"DSP frame #{i} (counter {a[i][0]}): only in the reference")
            break
        if i >= len(a):
            problems.append(f"DSP frame #{i} (counter {b[i][0]}): only in the candidate")
            break
        (ka, pa), (kb, pb) = a[i], b[i]
        if ka != kb:
            problems.append(f"DSP frame #{i}: counter {ka} in the reference, {kb} in the candidate")
            continue
        if region_counters(pa) != region_counters(pb):
            problems.append(f"DSP frame {ka}: region counters {region_counters(pa)} (ref) vs {region_counters(pb)} (cand)")
        for region in (0, 1):
            for source, (x, y) in enumerate(zip(source_statuses(pa, region), source_statuses(pb, region))):
                if x != y:
                    problems.append(f"DSP frame {ka} region {region} source {source}: ref {_describe_status(x)} "
                                    f"cand {_describe_status(y)}")
    total = len(problems)
    if total > limit:
        problems = problems[:limit] + [f"... and {total - limit} more differences"]
    return notes + problems if problems else []


# ── command line ──────────────────────────────────────────────────────────────────────


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="DSP (MDSP) and HID (MHID) capture tool")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary", help="record counts and what the capture holds (never its bytes)")
    p.add_argument("file")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("check", help="structural validity; exit 1 if broken")
    p.add_argument("file")
    p = sub.add_parser("pcm-to-wav", help="write the PCM records of a DSP capture as a WAV file")
    p.add_argument("file")
    p.add_argument("out")
    p = sub.add_parser("diff-hid", help="buttons, circle pad and touch per game frame; exit 0 when equal")
    p.add_argument("ref")
    p.add_argument("cand")
    p = sub.add_parser("diff-dsp", help="source statuses and DSP frame counters keyed on the DSP frame counter")
    p.add_argument("ref")
    p.add_argument("cand")
    p.add_argument("--frames", type=int, default=None, help="compare only the first N DSP frames")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "summary":
            s = summarize(args.file)
            print(json.dumps(s, indent=2) if args.json else format_summary(s))
            return 0
        if args.cmd == "check":
            n = len(read_all(args.file))
            print(f"{args.file}: ok, {n} records")
            return 0
        if args.cmd == "pcm-to-wav":
            n = pcm_to_wav(args.file, args.out)
            print(f"{args.out}: {n} audio frames, {n * SAMPLES_PER_FRAME} samples at {SAMPLE_RATE} Hz")
            return 0
        if args.cmd == "diff-hid":
            problems = diff_hid(args.ref, args.cand)
        else:
            if args.frames is not None and args.frames < 0:
                ap.error("--frames wants a count")
            problems = diff_dsp(args.ref, args.cand, args.frames)
    except CapError as e:
        print(f"capfmt: {e}", file=sys.stderr)
        return 1 if args.cmd == "check" and not str(e).startswith("cannot read") else 2
    if problems:
        print("\n".join(problems))
        return 1
    print("equal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
