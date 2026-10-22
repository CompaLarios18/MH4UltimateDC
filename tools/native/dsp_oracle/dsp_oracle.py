#!/usr/bin/env python3
# Derived from Azahar src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the byte offsets and bit positions of SourceConfiguration (and its u32_dsp middle-endian values), which the commands write.
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
"""The oracle of the native HLE DSP (N3-02 the sources, N3-03 the mixers, the effects and the frame):
what Azahar's own code does.

    dsp_oracle.py --azahar ~/azahar --cases native/tests/dsp_hle_cases.txt --out native/tests/dsp_hle_expected.inc

copies Azahar's codec.cpp, interpolate.cpp, filter.cpp, source.cpp, mixers.cpp and effects.cpp (with their
headers) into a temporary folder, compiles them with clang against the stand-ins of standin/ (our own
headers, each saying what it replaces; bit_field.h is copied as it is), plays every case of the cases file
through driver.cpp and driver_frame.cpp and writes, per case and frame, four CRC-32, as the C++ table
native/tests/dsp_hle_expected.inc. A source case (the default) plays one Azahar Source: the three quad
mixes and the status block. A frame case (`mode=frame`) plays Azahar's whole frame (GenerateCurrentFrame,
copied into driver_frame.cpp): the final samples, the statuses, the aux buses' samples and the
configuration (see driver_frame.cpp). Nothing in the table is computed or written by hand:
native/src/dsp_hle/ and its test are compared with it.

Cases file (one case after another; `#` starts a comment, blank lines are free):

    case <name> [source=<n>] [frames=<n>] [mode=frame] [fc=<n>] [sweep <var>=<a>,<b>... <var>=...]
    mem <address> <kind> seed=<n> bytes=<n>
    <frame> <command> <arguments>

`sweep` repeats the case for every combination of the lists; `${var}` in the name and in the lines is
replaced (the name's other characters than letters, digits and `_` become `_`). The default is source 0
and 200 frames. Memory is FCRAM (`mem` addresses are in the first 4 MiB of it); what is not placed reads
as zeros. A kind is how the bytes come from a seeded generator (the LCG below, which the C++ test
repeats and checks against the CRC-32 the table holds): noise (any byte), walk16 (a random walk of s16
samples), walk8 (of unsigned 8-bit samples), adpcm (frames of 8 bytes: a header with predictor 0-7 and
scale 0-8, then 7 bytes of noise).

The commands write the source's configuration as the application does and set its dirty bits, at the
start of the given frame, before the Source's Tick (the configuration keeps its bytes from frame to
frame, as the shared memory does; the Source clears the dirty bits):

    enable <0|1>              sync <n>               rate <float>        interp <poly|linear|none|n>
    format <pcm8|pcm16|adpcm|n> <mono|stereo|n>      playpos <n>         loop_related <n>
    embedded addr=<a> len=<n> id=<n> [loop] [pos=<n>] [adirty] [ps=<n>] [yn1=<n>] [yn2=<n>]
    queue <slot 0-3> addr=<a> len=<n> id=<n> [loop] [adirty] [ps=<n>] [yn1=<n>] [yn2=<n>]
    partial len=<n> [loop]    gain <mix 0-2> <g0> <g1> <g2> <g3>
    filters <simple 0|1> <biquad 0|1>        simple <b0> <a1>    biquad <a2> <a1> <b2> <b1> <b0>
    coeffs <16 s16 values>    reset    partial_reset    sleep    wakeup
    poke <address> <hex bytes>      writes the bytes into the memory (a game rewriting a buffer)

A number is decimal or 0x hex; `rand:<seed>[:<max>]` in place of the values of `simple`, `biquad` and
`coeffs` (all of them) gives random ones in -max..max (the default max is 32767).

A frame case (`mode=frame`, N3-03) drives all 24 sources and the mixers. Its commands are the source
commands above for a source (`<frame> s<k> <source command>`: the byte writes go into source k's
configuration in the region the DSP reads in that frame) and those of the mixers:

    master <vol>              auxreturn <0|1> <vol>        outfmt <mono|stereo|surround|n>
    auxbus <0|1> <0|1>        headphones <0|1>             dspdirty <bits>      dspwrite <offset> <hex>
    delay <0|1> [enable=<0|1>] [frames=<n>] [g=<s16>] [a=<s16>] [b=<s16>] [work=<a>] [outputs=<n>]
                              [dirty=<0|1>] [nomain]       (g, a, b in Q7; `dirty` is the effect's own dirty
                              word, `nomain` leaves the main dirty bit unset)
    reverb <0|1> rand:<seed>  (52 bytes of noise, and its dirty bit)
    auxmix <1|2> seed=<n> [amp=<n>]     the application's processed samples of an aux bus, from a generator
    sleep    wakeup           the whole DSP (the pipe's Sleep and Wakeup)    poke <address> <hex bytes>

The driver plays the application: before the frame k it writes the commands of that frame into the region
the DSP reads next, which is the one it wrote in the frame before (region (k + 1) % 2), and sets that
region's frame counter to `fc + k + 1` (`fc` is the case's `fc=`, 0 by default; the counters wrap, which
is how the region rule is exercised). A frame in which the DSP is asleep prints zeros.

Driver input, one case after another:
    case <name> <frames> <source>
    mem <address hex> <data hex>
    ev <frame> <c|d|b|a|m|S|W> <offset> <hex bytes or ->    (c: bytes into the configuration, d: OR of
                                                             the dirty word, b: OR of buffers_dirty, a: bytes
                                                             into the ADPCM coefficient table, m: bytes into
                                                             memory at the address `offset`, S: Source::Sleep,
                                                             W: Source::Wakeup)
    end
  or a frame case (events: r bytes at an offset of the region the frame reads, o OR a u32 there, p OR a u16
  there, g `s32` samples from the generator there (seed, byte size, amplitude and the CRC-32 of the output,
  4 bytes each), m, S and W as above for the whole DSP):
    fcase <name> <frames> <fc> <the offsets in a region of: source configurations, ADPCM coefficients, DSP
          configuration, intermediate mix samples, source statuses, final samples, DSP status>
    mem ... / ev <frame> <r|o|p|g|m|S|W> <offset> <hex bytes or -> / end
Driver output, per frame:
    frame <name> <k> <crc mix 0> <crc mix 1> <crc mix 2> <crc status>      (8 hex digits each)
    frame <name> <k> <crc final samples> <crc statuses> <crc mix buses> <crc configuration>   (a frame case)
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import itertools
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_CASES = os.path.join(ROOT, "native", "tests", "dsp_hle_cases.txt")
PINNED = os.path.join(ROOT, "port", "azahar", "COMMIT")

FCRAM = 0x20000000
WINDOW = 0x400000            # the part of FCRAM a case can place data in
DEFAULT_FRAMES = 200
MAX_FRAMES = 2000

# Azahar files copied as they are (relative to src/)
COPIED = (
    "audio_core/audio_types.h",
    "audio_core/codec.cpp",
    "audio_core/codec.h",
    "audio_core/interpolate.cpp",
    "audio_core/interpolate.h",
    "audio_core/hle/common.h",
    "audio_core/hle/effects.cpp",
    "audio_core/hle/effects.h",
    "audio_core/hle/filter.cpp",
    "audio_core/hle/filter.h",
    "audio_core/hle/mixers.cpp",
    "audio_core/hle/mixers.h",
    "audio_core/hle/shared_memory.h",
    "audio_core/hle/source.cpp",
    "audio_core/hle/source.h",
    "common/bit_field.h",
)
SOURCES = (
    "audio_core/codec.cpp",
    "audio_core/interpolate.cpp",
    "audio_core/hle/effects.cpp",
    "audio_core/hle/filter.cpp",
    "audio_core/hle/mixers.cpp",
    "audio_core/hle/source.cpp",
)
DRIVERS = ("driver.cpp", "driver_frame.cpp")

KINDS = {"noise": 0, "walk16": 1, "walk8": 2, "adpcm": 3}

# SourceConfiguration::Configuration (192 bytes): the offsets (shared_memory.h; native/src/audio/dsp_layout.h)
OFF_GAIN = 4
OFF_RATE = 52
OFF_INTERP = 56
OFF_FILTERS_ENABLED = 58
OFF_SIMPLE = 60              # {b0, a1}
OFF_BIQUAD = 64              # {a2, a1, b2, b1, b0}
OFF_BUFFERS = 76             # 4 * 20 bytes
OFF_LOOP_RELATED = 156
OFF_ENABLE = 160
OFF_SYNC = 162
OFF_PLAY_POSITION = 164
OFF_EMB_ADDRESS = 172
OFF_EMB_LENGTH = 176
OFF_FLAGS1 = 180
OFF_EMB_PS = 182
OFF_EMB_YN = 184
OFF_FLAGS2 = 188
OFF_EMB_ID = 190
CONFIG_SIZE = 192

# The shared memory of one region (shared_memory.h; native/src/audio/dsp_layout.h): the offsets of the
# structures the frame cases address. The frame driver checks them against Azahar's own offsetof.
REGION_SIZE = 0x8000
SHM_DSP_STATUS = 0x400 * 2
SHM_DSP_DEBUG = SHM_DSP_STATUS + 32
SHM_FINAL_SAMPLES = SHM_DSP_DEBUG + 0x130 * 2
SHM_SOURCE_STATUSES = SHM_FINAL_SAMPLES + 640
SHM_COMPRESSOR = SHM_SOURCE_STATUSES + 24 * 12
SHM_DSP_CONFIGURATION = SHM_COMPRESSOR + 0xD20 * 2
SHM_INTERMEDIATE_MIX = SHM_DSP_CONFIGURATION + 196
SHM_SOURCE_CONFIGURATIONS = SHM_INTERMEDIATE_MIX + 5120
SHM_ADPCM_COEFFICIENTS = SHM_SOURCE_CONFIGURATIONS + 24 * CONFIG_SIZE
MIX_BUS_SIZE = 2560                  # one aux bus: 4 channels * 160 * s32
OFF_BUFFERS_DIRTY = 74

# DspConfiguration (196 bytes): the offsets and the dirty bits
DC_MASTER_VOLUME = 4
DC_AUX_RETURN_VOLUME = 8             # float[2]
DC_OUTPUT_FORMAT = 22                # u16
DC_HEADPHONES = 26                   # u16
DC_AUX_BUS_ENABLE = 40               # u16[2]
DC_DELAY_EFFECT = 44                 # 2 * 20 bytes: dirty, enable, pad, outputs, work address (u32_dsp),
DC_DELAY_SIZE = 20                   # frame count, g, a, b
DC_REVERB_EFFECT = 84                # 2 * 52 bytes
DC_REVERB_SIZE = 52
DC_AUX_BUS_DIRTY = (1 << 8, 1 << 9)
DC_DELAY_DIRTY = (1 << 10, 1 << 11)
DC_REVERB_DIRTY = (1 << 12, 1 << 13)
DC_MASTER_VOLUME_DIRTY = 1 << 16
DC_AUX_RETURN_DIRTY = (1 << 24, 1 << 25)
DC_OUTPUT_FORMAT_DIRTY = 1 << 26
DC_HEADPHONES_DIRTY = 1 << 28

D_FORMAT = 1 << 0
D_MONO_OR_STEREO = 1 << 1
D_ADPCM_COEFFICIENTS = 1 << 2
D_PARTIAL_EMBEDDED = 1 << 3
D_PARTIAL_RESET = 1 << 4
D_ENABLE = 1 << 16
D_INTERPOLATION = 1 << 17
D_RATE = 1 << 18
D_BUFFER_QUEUE = 1 << 19
D_LOOP_RELATED = 1 << 20
D_PLAY_POSITION = 1 << 21
D_FILTERS_ENABLED = 1 << 22
D_SIMPLE = 1 << 23
D_BIQUAD = 1 << 24
D_GAIN = (1 << 25, 1 << 26, 1 << 27)
D_SYNC = 1 << 28
D_RESET = 1 << 29
D_EMBEDDED = 1 << 30


class CaseError(ValueError):
    pass


class Lcg:
    """The generator of the synthetic data (the C++ test repeats it): a 32-bit LCG, 16 bits out."""

    def __init__(self, seed: int):
        self.state = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state >> 16


def generate(kind: str, seed: int, size: int) -> bytes:
    rng = Lcg(seed)
    out = bytearray()
    if kind == "noise":
        for _ in range(size):
            out.append(rng.next() & 0xFF)
    elif kind == "walk16":
        value = 0
        for _ in range(size // 2):
            value = max(-30000, min(30000, value + rng.next() % 4001 - 2000))
            out += struct.pack("<h", value)
    elif kind == "walk8":
        value = 128
        for _ in range(size):
            value = (value + rng.next() % 41 - 20) & 0xFF
            out.append(value)
    elif kind == "adpcm":
        while len(out) < size:
            predictor = rng.next() % 8
            scale = rng.next() % 9
            out.append((predictor << 4) | scale)
            for _ in range(7):
                out.append(rng.next() & 0xFF)
        del out[size:]
    else:
        raise CaseError(f"unknown kind {kind}")
    if kind == "walk16" and size % 2:
        out.append(0)
    return bytes(out)


def generate_pcm32(seed: int, size: int, amplitude: int) -> bytes:
    """The samples an application leaves in an aux bus (the frame driver repeats it): `s32` little-endian
    in -amplitude..amplitude, each from two draws of the LCG; a size that is not a multiple of 4 ends in
    zero bytes."""
    rng = Lcg(seed)
    out = bytearray()
    for _ in range(size // 4):
        raw = (rng.next() << 16) | rng.next()
        out += struct.pack("<i", raw % (2 * amplitude + 1) - amplitude)
    out += bytes(size - len(out))
    return bytes(out)


@dataclasses.dataclass(frozen=True)
class Fill:
    address: int
    kind: str
    seed: int
    size: int

    def data(self) -> bytes:
        return generate(self.kind, self.seed, self.size)


@dataclasses.dataclass(frozen=True)
class Event:
    frame: int
    op: str          # c d b a m S W
    offset: int
    data: bytes


@dataclasses.dataclass
class Case:
    name: str
    source: int = 0
    frames: int = DEFAULT_FRAMES
    fills: list[Fill] = dataclasses.field(default_factory=list)
    events: list[Event] = dataclasses.field(default_factory=list)
    mode: str = "source"             # "source" (one Source) or "frame" (the whole frame)
    first_counter: int = 0           # a frame case: the frame counter before frame 0


# ---- numbers --------------------------------------------------------------------------------------------------

def integer(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise CaseError(f"bad number {text!r}") from None


def real(text: str) -> float:
    try:
        return float(text)
    except ValueError:
        raise CaseError(f"bad number {text!r}") from None


def dsp32(value: int) -> bytes:
    """A u32_dsp: the 16-bit halves swapped."""
    value &= 0xFFFFFFFF
    return struct.pack("<I", ((value << 16) | (value >> 16)) & 0xFFFFFFFF)


def u16(value: int) -> bytes:
    return struct.pack("<H", value & 0xFFFF)


def values(words: list[str], count: int, what: str) -> list[int]:
    """`count` s16 values, or `rand:<seed>[:<max>]`."""
    if len(words) == 1 and words[0].startswith("rand:"):
        parts = words[0].split(":")
        seed = integer(parts[1])
        limit = integer(parts[2]) if len(parts) > 2 else 32767
        if not 0 < limit <= 32767:
            raise CaseError(f"{what}: rand max must be 1..32767")
        rng = Lcg(seed)
        return [rng.next() % (2 * limit + 1) - limit for _ in range(count)]
    if len(words) != count:
        raise CaseError(f"{what} needs {count} numbers (or rand:<seed>), got {len(words)}")
    out = [integer(w) for w in words]
    for v in out:
        if not -32768 <= v <= 65535:
            raise CaseError(f"{what}: {v} does not fit 16 bits")
    return out


def keyword_arguments(words: list[str], allowed: set[str], flags: set[str], what: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for word in words:
        if word in flags:
            out[word] = 1
        elif "=" in word:
            key, value = word.split("=", 1)
            if key not in allowed:
                raise CaseError(f"{what}: unknown argument {key}=")
            out[key] = integer(value)
        else:
            raise CaseError(f"{what}: unexpected {word!r}")
    return out


# ---- the commands ---------------------------------------------------------------------------------------------

class Writer:
    """Turns the commands of one frame into events, keeping the configuration bytes the shared memory
    would hold (the commands compose fields that share a word)."""

    def __init__(self, source: int):
        self.shadow = bytearray(CONFIG_SIZE)
        self.source = source
        self.events: list[Event] = []

    # Where the writes go: the single source's configuration here; RegionWriter puts them in a region.
    def put_bytes(self, frame: int, offset: int, data: bytes) -> None:
        self.events.append(Event(frame, "c", offset, data))

    def put_dirty(self, frame: int, bits: int) -> None:
        self.events.append(Event(frame, "d", 0, struct.pack("<I", bits)))

    def put_buffers_dirty(self, frame: int, bits: int) -> None:
        self.events.append(Event(frame, "b", 0, u16(bits)))

    def put_coefficients(self, frame: int, data: bytes) -> None:
        self.events.append(Event(frame, "a", 32 * self.source, data))

    def write(self, frame: int, offset: int, data: bytes) -> None:
        self.shadow[offset:offset + len(data)] = data
        self.put_bytes(frame, offset, data)

    def dirty(self, frame: int, bits: int) -> None:
        self.put_dirty(frame, bits)

    def read16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.shadow, offset)[0]

    def command(self, frame: int, words: list[str]) -> None:
        name, args = words[0], words[1:]
        handler = getattr(self, "cmd_" + name, None)
        if handler is None:
            raise CaseError(f"unknown command {name!r}")
        handler(frame, args)

    def need(self, args: list[str], count: int, name: str) -> None:
        if len(args) != count:
            raise CaseError(f"{name} needs {count} argument(s), got {len(args)}")

    def cmd_enable(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "enable")
        self.write(frame, OFF_ENABLE, bytes([integer(args[0]) & 0xFF]))
        self.dirty(frame, D_ENABLE)

    def cmd_sync(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "sync")
        self.write(frame, OFF_SYNC, u16(integer(args[0])))
        self.dirty(frame, D_SYNC)

    def cmd_rate(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "rate")
        self.write(frame, OFF_RATE, struct.pack("<f", real(args[0])))
        self.dirty(frame, D_RATE)

    def cmd_interp(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "interp")
        modes = {"poly": 0, "linear": 1, "none": 2}
        mode = modes[args[0]] if args[0] in modes else integer(args[0])
        self.write(frame, OFF_INTERP, bytes([mode & 0xFF]))
        self.dirty(frame, D_INTERPOLATION)

    def cmd_format(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "format")
        formats = {"pcm8": 0, "pcm16": 1, "adpcm": 2}
        channels = {"mono": 1, "stereo": 2}
        fmt = formats[args[0]] if args[0] in formats else integer(args[0])
        chan = channels[args[1]] if args[1] in channels else integer(args[1])
        if not (0 <= fmt <= 3 and 0 <= chan <= 3):
            raise CaseError("format: the format and the channels are 2-bit values")
        self.write(frame, OFF_FLAGS1, u16((self.read16(OFF_FLAGS1) & ~0xF) | chan | (fmt << 2)))
        self.dirty(frame, D_FORMAT | D_MONO_OR_STEREO)

    def cmd_playpos(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "playpos")
        self.write(frame, OFF_PLAY_POSITION, dsp32(integer(args[0])))
        self.dirty(frame, D_PLAY_POSITION)

    def cmd_loop_related(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "loop_related")
        self.write(frame, OFF_LOOP_RELATED, dsp32(integer(args[0])))
        self.dirty(frame, D_LOOP_RELATED)

    def cmd_embedded(self, frame: int, args: list[str]) -> None:
        kw = keyword_arguments(args, {"addr", "len", "id", "pos", "ps", "yn1", "yn2"}, {"loop", "adirty"}, "embedded")
        for key in ("addr", "len", "id"):
            if key not in kw:
                raise CaseError(f"embedded needs {key}=")
        self.write(frame, OFF_EMB_ADDRESS, dsp32(kw["addr"]))
        self.write(frame, OFF_EMB_LENGTH, dsp32(kw["len"]))
        if "ps" in kw:
            self.write(frame, OFF_EMB_PS, u16(kw["ps"]))
        if "yn1" in kw:
            self.write(frame, OFF_EMB_YN, u16(kw["yn1"]))
        if "yn2" in kw:
            self.write(frame, OFF_EMB_YN + 2, u16(kw["yn2"]))
        self.write(frame, OFF_FLAGS2, u16(kw.get("adirty", 0) | (kw.get("loop", 0) << 1)))
        self.write(frame, OFF_EMB_ID, u16(kw["id"]))
        bits = D_EMBEDDED
        if "pos" in kw:
            self.write(frame, OFF_PLAY_POSITION, dsp32(kw["pos"]))
            bits |= D_PLAY_POSITION
        self.dirty(frame, bits)

    def cmd_queue(self, frame: int, args: list[str]) -> None:
        if not args:
            raise CaseError("queue needs a slot")
        slot = integer(args[0])
        if not 0 <= slot < 4:
            raise CaseError("queue: the slot is 0 to 3")
        kw = keyword_arguments(args[1:], {"addr", "len", "id", "ps", "yn1", "yn2"}, {"loop", "adirty"}, "queue")
        for key in ("addr", "len", "id"):
            if key not in kw:
                raise CaseError(f"queue needs {key}=")
        base = OFF_BUFFERS + 20 * slot
        self.write(frame, base + 0, dsp32(kw["addr"]))
        self.write(frame, base + 4, dsp32(kw["len"]))
        self.write(frame, base + 8, u16(kw.get("ps", 0)))
        self.write(frame, base + 10, u16(kw.get("yn1", 0)))
        self.write(frame, base + 12, u16(kw.get("yn2", 0)))
        self.write(frame, base + 14, bytes([kw.get("adirty", 0)]))
        self.write(frame, base + 15, bytes([kw.get("loop", 0)]))
        self.write(frame, base + 16, u16(kw["id"]))
        self.put_buffers_dirty(frame, 1 << slot)
        self.dirty(frame, D_BUFFER_QUEUE)

    def cmd_partial(self, frame: int, args: list[str]) -> None:
        kw = keyword_arguments(args, {"len"}, {"loop"}, "partial")
        if "len" not in kw:
            raise CaseError("partial needs len=")
        self.write(frame, OFF_EMB_LENGTH, dsp32(kw["len"]))
        self.write(frame, OFF_FLAGS2, u16((self.read16(OFF_FLAGS2) & ~2) | (kw.get("loop", 0) << 1)))
        self.dirty(frame, D_PARTIAL_EMBEDDED)

    def cmd_gain(self, frame: int, args: list[str]) -> None:
        self.need(args, 5, "gain")
        mix = integer(args[0])
        if not 0 <= mix < 3:
            raise CaseError("gain: the mix is 0 to 2")
        self.write(frame, OFF_GAIN + 16 * mix, struct.pack("<4f", *(real(a) for a in args[1:])))
        self.dirty(frame, D_GAIN[mix])

    def cmd_filters(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "filters")
        self.write(frame, OFF_FILTERS_ENABLED, u16((integer(args[0]) & 1) | ((integer(args[1]) & 1) << 1)))
        self.dirty(frame, D_FILTERS_ENABLED)

    def cmd_simple(self, frame: int, args: list[str]) -> None:
        self.write(frame, OFF_SIMPLE, b"".join(u16(v) for v in values(args, 2, "simple")))
        self.dirty(frame, D_SIMPLE)

    def cmd_biquad(self, frame: int, args: list[str]) -> None:
        self.write(frame, OFF_BIQUAD, b"".join(u16(v) for v in values(args, 5, "biquad")))
        self.dirty(frame, D_BIQUAD)

    def cmd_coeffs(self, frame: int, args: list[str]) -> None:
        data = b"".join(u16(v) for v in values(args, 16, "coeffs"))
        self.put_coefficients(frame, data)
        self.dirty(frame, D_ADPCM_COEFFICIENTS)

    def cmd_reset(self, frame: int, args: list[str]) -> None:
        self.need(args, 0, "reset")
        self.dirty(frame, D_RESET)

    def cmd_partial_reset(self, frame: int, args: list[str]) -> None:
        self.need(args, 0, "partial_reset")
        self.dirty(frame, D_PARTIAL_RESET)

    def cmd_poke(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "poke")
        address, data = integer(args[0]), bytes.fromhex(args[1])
        if not data or not FCRAM <= address or address + len(data) > FCRAM + WINDOW:
            raise CaseError("poke: bytes in the first 4 MiB of FCRAM")
        self.events.append(Event(frame, "m", address, data))

    def cmd_sleep(self, frame: int, args: list[str]) -> None:
        self.need(args, 0, "sleep")
        self.events.append(Event(frame, "S", 0, b""))

    def cmd_wakeup(self, frame: int, args: list[str]) -> None:
        self.need(args, 0, "wakeup")
        self.events.append(Event(frame, "W", 0, b""))


class RegionWriter(Writer):
    """The commands of one source in a frame case: the same byte writes, but into that source's
    configuration (and ADPCM coefficient row) in the region the DSP reads."""

    def __init__(self, source: int, events: list[Event]):
        super().__init__(source)
        self.events = events
        self.base = SHM_SOURCE_CONFIGURATIONS + CONFIG_SIZE * source

    def put_bytes(self, frame: int, offset: int, data: bytes) -> None:
        self.events.append(Event(frame, "r", self.base + offset, data))

    def put_dirty(self, frame: int, bits: int) -> None:
        self.events.append(Event(frame, "o", self.base, struct.pack("<I", bits)))

    def put_buffers_dirty(self, frame: int, bits: int) -> None:
        self.events.append(Event(frame, "p", self.base + OFF_BUFFERS_DIRTY, u16(bits)))

    def put_coefficients(self, frame: int, data: bytes) -> None:
        self.events.append(Event(frame, "r", SHM_ADPCM_COEFFICIENTS + 32 * self.source, data))


class FrameWriter:
    """The commands of a frame case: a source's (`s<k> <command>`) and the mixers'."""

    FORMATS = {"mono": 0, "stereo": 1, "surround": 2}

    def __init__(self):
        self.events: list[Event] = []
        self.sources = [RegionWriter(k, self.events) for k in range(24)]

    def command(self, frame: int, words: list[str]) -> None:
        name, args = words[0], words[1:]
        match = re.fullmatch(r"s(\d+)", name)
        if match:
            index = int(match.group(1))
            if index >= 24 or not args:
                raise CaseError(f"{name}: a source is s0 to s23, followed by a command")
            if args[0] in ("sleep", "wakeup", "poke"):
                raise CaseError(f"{args[0]} is the whole DSP's: write it without {name}")
            self.sources[index].command(frame, args)
            return
        handler = getattr(self, "cmd_" + name, None)
        if handler is None:
            raise CaseError(f"unknown command {name!r}")
        handler(frame, args)

    def need(self, args: list[str], count: int, name: str) -> None:
        if len(args) != count:
            raise CaseError(f"{name} needs {count} argument(s), got {len(args)}")

    def config(self, frame: int, offset: int, data: bytes, dirty: int) -> None:
        if data:
            self.events.append(Event(frame, "r", SHM_DSP_CONFIGURATION + offset, data))
        if dirty:
            self.events.append(Event(frame, "o", SHM_DSP_CONFIGURATION, struct.pack("<I", dirty)))

    def index(self, text: str, name: str) -> int:
        value = integer(text)
        if value not in (0, 1):
            raise CaseError(f"{name}: the index is 0 or 1")
        return value

    def cmd_master(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "master")
        self.config(frame, DC_MASTER_VOLUME, struct.pack("<f", real(args[0])), DC_MASTER_VOLUME_DIRTY)

    def cmd_auxreturn(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "auxreturn")
        i = self.index(args[0], "auxreturn")
        self.config(frame, DC_AUX_RETURN_VOLUME + 4 * i, struct.pack("<f", real(args[1])), DC_AUX_RETURN_DIRTY[i])

    def cmd_outfmt(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "outfmt")
        value = self.FORMATS[args[0]] if args[0] in self.FORMATS else integer(args[0])
        self.config(frame, DC_OUTPUT_FORMAT, u16(value), DC_OUTPUT_FORMAT_DIRTY)

    def cmd_auxbus(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "auxbus")
        i = self.index(args[0], "auxbus")
        self.config(frame, DC_AUX_BUS_ENABLE + 2 * i, u16(integer(args[1])), DC_AUX_BUS_DIRTY[i])

    def cmd_headphones(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "headphones")
        self.config(frame, DC_HEADPHONES, u16(integer(args[0])), DC_HEADPHONES_DIRTY)

    def cmd_dspdirty(self, frame: int, args: list[str]) -> None:
        self.need(args, 1, "dspdirty")
        self.config(frame, 0, b"", integer(args[0]) & 0xFFFFFFFF)

    def cmd_dspwrite(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "dspwrite")
        offset, data = integer(args[0]), bytes.fromhex(args[1])
        if not data or offset < 4 or offset + len(data) > 196:
            raise CaseError("dspwrite: bytes inside the DSP configuration, after its dirty word")
        self.config(frame, offset, data, 0)

    def cmd_delay(self, frame: int, args: list[str]) -> None:
        if not args:
            raise CaseError("delay needs an effect (0 or 1)")
        i = self.index(args[0], "delay")
        kw = keyword_arguments(args[1:], {"enable", "frames", "g", "a", "b", "work", "outputs", "dirty"},
                               {"nomain"}, "delay")
        base = DC_DELAY_EFFECT + DC_DELAY_SIZE * i
        block = (u16(kw.get("dirty", 1)) + u16(kw.get("enable", 1)) + u16(0) + u16(kw.get("outputs", 0)) +
                 dsp32(kw.get("work", 0)) + u16(kw.get("frames", 1)) + u16(kw.get("g", 0)) +
                 u16(kw.get("a", 0)) + u16(kw.get("b", 0)))
        self.config(frame, base, block, 0 if "nomain" in kw else DC_DELAY_DIRTY[i])

    def cmd_reverb(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "reverb")
        i = self.index(args[0], "reverb")
        if not args[1].startswith("rand:"):
            raise CaseError("reverb <0|1> rand:<seed>")
        rng = Lcg(integer(args[1].split(":")[1]))
        block = b"".join(u16(rng.next()) for _ in range(DC_REVERB_SIZE // 2))
        self.config(frame, DC_REVERB_EFFECT + DC_REVERB_SIZE * i, block, DC_REVERB_DIRTY[i])

    def cmd_auxmix(self, frame: int, args: list[str]) -> None:
        if not args:
            raise CaseError("auxmix needs a bus (1 or 2)")
        bus = integer(args[0])
        if bus not in (1, 2):
            raise CaseError("auxmix: the bus is 1 or 2")
        kw = keyword_arguments(args[1:], {"seed", "amp"}, set(), "auxmix")
        if "seed" not in kw:
            raise CaseError("auxmix needs seed=")
        amplitude = kw.get("amp", 1000)
        if not 0 <= amplitude <= 0x7FFFFFFF:
            raise CaseError("auxmix: amp is 0 to 2^31-1")
        data = struct.pack("<IIII", kw["seed"] & 0xFFFFFFFF, MIX_BUS_SIZE, amplitude,
                           zlib.crc32(generate_pcm32(kw["seed"] & 0xFFFFFFFF, MIX_BUS_SIZE, amplitude)))
        self.events.append(Event(frame, "g", SHM_INTERMEDIATE_MIX + MIX_BUS_SIZE * (bus - 1), data))

    def cmd_sleep(self, frame: int, args: list[str]) -> None:
        self.need(args, 0, "sleep")
        self.events.append(Event(frame, "S", 0, b""))

    def cmd_wakeup(self, frame: int, args: list[str]) -> None:
        self.need(args, 0, "wakeup")
        self.events.append(Event(frame, "W", 0, b""))

    def cmd_poke(self, frame: int, args: list[str]) -> None:
        self.need(args, 2, "poke")
        address, data = integer(args[0]), bytes.fromhex(args[1])
        if not data or not FCRAM <= address or address + len(data) > FCRAM + WINDOW:
            raise CaseError("poke: bytes in the first 4 MiB of FCRAM")
        self.events.append(Event(frame, "m", address, data))


# ---- the cases file -------------------------------------------------------------------------------------------

def expand_blocks(text: str) -> list[tuple[int, list[tuple[int, str]], dict[str, list[str]]]]:
    """The blocks of the file: (line of `case`, its words, the lines of the block), sweeps not yet applied."""
    blocks: list = []
    current = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.split()[0] == "case":
            current = (number, line, [])
            blocks.append(current)
        elif current is None:
            raise CaseError(f"line {number}: a line before the first `case`")
        else:
            current[2].append((number, line))
    return blocks


def parse_cases(text: str) -> list[Case]:
    cases: list[Case] = []
    seen: set[str] = set()
    for number, header, lines in expand_blocks(text):
        words = header.split()
        if len(words) < 2:
            raise CaseError(f"line {number}: `case` needs a name")
        name_template = words[1]
        if not re.fullmatch(r"[A-Za-z0-9_${}]+", name_template):
            raise CaseError(f"line {number}: bad name {name_template!r}")
        source, frames = 0, DEFAULT_FRAMES
        mode, first_counter = "source", 0
        axes: list[tuple[str, list[str]]] = []
        sweeping = False
        for word in words[2:]:
            if word == "sweep":
                sweeping = True
            elif "=" not in word:
                raise CaseError(f"line {number}: expected key=value, got {word!r}")
            else:
                key, value = word.split("=", 1)
                if sweeping:
                    axes.append((key, value.split(",")))
                elif key == "source":
                    source = integer(value)
                elif key == "frames":
                    frames = integer(value)
                elif key == "mode":
                    if value not in ("source", "frame"):
                        raise CaseError(f"line {number}: mode is source or frame")
                    mode = value
                elif key == "fc":
                    first_counter = integer(value)
                else:
                    raise CaseError(f"line {number}: unknown key {key}=")
        if not 0 <= source < 24:
            raise CaseError(f"line {number}: source {source} (0 to 23)")
        if not 1 <= frames <= MAX_FRAMES:
            raise CaseError(f"line {number}: frames {frames} (1 to {MAX_FRAMES})")
        if mode == "frame" and source != 0:
            raise CaseError(f"line {number}: a frame case has no source=")
        if mode == "source" and first_counter:
            raise CaseError(f"line {number}: fc= is for frame cases")
        if not 0 <= first_counter <= 0xFFFF:
            raise CaseError(f"line {number}: fc {first_counter} (0 to 65535)")
        combos = list(itertools.product(*(vals for _, vals in axes))) if axes else [()]
        for combo in combos:
            subst = {key: v for (key, _), v in zip(axes, combo)}

            def apply(s: str) -> str:
                for key, v in subst.items():
                    s = s.replace("${" + key + "}", v)
                return s

            name = re.sub(r"[^A-Za-z0-9_]", "_", apply(name_template))
            if name in seen:
                raise CaseError(f"line {number}: two cases named {name}")
            seen.add(name)
            case = Case(name, source, frames, mode=mode, first_counter=first_counter)
            writer = FrameWriter() if mode == "frame" else Writer(source)
            for line_number, line in lines:
                try:
                    parse_line(case, writer, apply(line))
                except CaseError as error:
                    raise CaseError(f"line {line_number}: {error}") from None
            case.events = writer.events
            cases.append(case)
    return cases


def parse_line(case: Case, writer: "Writer | FrameWriter", line: str) -> None:
    words = line.split()
    if words[0] == "mem":
        if len(words) != 5:
            raise CaseError("mem <address> <kind> seed=<n> bytes=<n>")
        address, kind = integer(words[1]), words[2]
        if kind not in KINDS:
            raise CaseError(f"mem: kind {kind} (one of {', '.join(KINDS)})")
        kw = keyword_arguments(words[3:], {"seed", "bytes"}, set(), "mem")
        if "seed" not in kw or "bytes" not in kw:
            raise CaseError("mem needs seed= and bytes=")
        size = kw["bytes"]
        if size <= 0 or not FCRAM <= address or address + size > FCRAM + WINDOW:
            raise CaseError(f"mem: {size} bytes at {address:#x} is outside the first 4 MiB of FCRAM")
        if kind == "walk16" and size % 2:
            raise CaseError("mem: walk16 takes an even number of bytes")
        case.fills.append(Fill(address, kind, kw["seed"], size))
        return
    try:
        frame = int(words[0])
    except ValueError:
        raise CaseError(f"expected mem or a frame number, got {words[0]!r}") from None
    if not 0 <= frame < case.frames:
        raise CaseError(f"frame {frame} is outside 0..{case.frames - 1}")
    if len(words) < 2:
        raise CaseError("a frame number needs a command")
    writer.command(frame, words[1:])


# ---- the driver ----------------------------------------------------------------------------------------------

def hexbytes(data: bytes) -> str:
    return data.hex() if data else "-"


def driver_input(cases: list[Case]) -> str:
    out = []
    for case in cases:
        if case.mode == "frame":
            offsets = (SHM_SOURCE_CONFIGURATIONS, SHM_ADPCM_COEFFICIENTS, SHM_DSP_CONFIGURATION,
                       SHM_INTERMEDIATE_MIX, SHM_SOURCE_STATUSES, SHM_FINAL_SAMPLES, SHM_DSP_STATUS)
            out.append(f"fcase {case.name} {case.frames} {case.first_counter} " + " ".join(map(str, offsets)))
        else:
            out.append(f"case {case.name} {case.frames} {case.source}")
        for fill in case.fills:
            out.append(f"mem {fill.address:x} {fill.data().hex()}")
        for e in case.events:
            out.append(f"ev {e.frame} {e.op} {e.offset} {hexbytes(e.data)}")
        out.append("end")
    return "\n".join(out) + "\n"


def parse_driver_output(text: str, cases: list[Case]) -> dict[str, str]:
    """The expected string of each case: per frame, four 8-digit CRCs."""
    rows: dict[str, list[str]] = {c.name: [] for c in cases}
    for raw in text.splitlines():
        words = raw.split()
        if len(words) != 7 or words[0] != "frame" or words[1] not in rows:
            raise CaseError(f"unexpected driver output: {raw!r}")
        if int(words[2]) != len(rows[words[1]]):
            raise CaseError(f"{words[1]}: frame {words[2]} out of order")
        if not all(re.fullmatch(r"[0-9a-f]{8}", w) for w in words[3:]):
            raise CaseError(f"bad CRCs in {raw!r}")
        rows[words[1]].append("".join(words[3:]))
    for case in cases:
        if len(rows[case.name]) != case.frames:
            raise CaseError(f"{case.name}: {len(rows[case.name])} frames answered, {case.frames} asked")
    return {name: "".join(frames) for name, frames in rows.items()}


def render_inc(cases: list[Case], results: dict[str, str], commit: str, cases_sha: str) -> str:
    out = [
        "// Generated by tools/native/dsp_oracle/dsp_oracle.py. DO NOT EDIT.",
        f"// Azahar {commit}; cases file sha256 {cases_sha}.",
        "// {name, frames, source, fills{{address, bytes, kind (0 noise, 1 walk16, 2 walk8, 3 adpcm), seed, crc32 of the bytes}...},",
        "//  events{{frame, op, offset, \"hex\"}...}, \"per frame, 8 hex digits each: crc32 of mix 0, mix 1, mix 2, the status\" (in lines of 8 frames)}",
        "// A frame case ends with `, 1`, its third number is the first frame counter, and its four CRCs are of the final samples, the statuses, the aux buses' samples and the configuration.",
    ]
    for case in cases:
        fills = ", ".join(
            f"{{{f.address:#x}, {f.size}, {KINDS[f.kind]}, {f.seed}, {zlib.crc32(f.data()):#x}}}" for f in case.fills)
        events = ", ".join(f'{{{e.frame}, \'{e.op}\', {e.offset}, "{e.data.hex()}"}}' for e in case.events)
        # the CRCs in lines of 8 frames (adjacent string literals), so that no line is a dense blob
        crcs = results[case.name]
        chunks = "\n".join(f' "{crcs[i:i + 256]}"' for i in range(0, len(crcs), 256))
        third = case.first_counter if case.mode == "frame" else case.source
        out.append(f'{{"{case.name}", {case.frames}, {third}, {{{fills}}},\n'
                   f' {{{events}}},\n'
                   f'{chunks}{", 1" if case.mode == "frame" else ""}}},')
    return "\n".join(out) + "\n"


def pinned_commit() -> str:
    with open(PINNED, encoding="utf-8") as f:
        return f.read().strip()


def azahar_commit(azahar: str) -> str:
    return subprocess.run(["git", "-C", azahar, "rev-parse", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()


def prepare_tree(azahar: str, tree: str) -> None:
    """Copy Azahar's files into tree/src and the stand-ins over the headers they include."""
    for rel in COPIED:
        dst = os.path.join(tree, "src", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(azahar, "src", rel), dst)
    standin = os.path.join(HERE, "standin")
    for folder, _, files in os.walk(standin):
        for name in files:
            dst = os.path.join(tree, "src", os.path.relpath(os.path.join(folder, name), standin))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(folder, name), dst)


def compile_driver(tree: str, cxx: str) -> str:
    exe = os.path.join(tree, "dsp_driver")
    src = os.path.join(tree, "src")
    # -fwrapv: the 32-bit sums of the filters and of ADPCM overflow in Azahar's C++ with extreme
    # coefficients, which is undefined there; the compiled code wraps, and so does the native code.
    # -ffp-contract=off: the gain ramp, the downmix and the delay are floating point, and the native code is
    # built the same way.
    cmd = [cxx, "-std=c++20", "-O1", "-ffp-contract=off", "-fwrapv", "-Wall", "-Wno-unused", "-I", src,
           "-I", os.path.join(src, "common"), "-I", HERE, *(os.path.join(src, s) for s in SOURCES),
           *(os.path.join(HERE, d) for d in DRIVERS), "-o", exe]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError("compiling Azahar's source code failed:\n" + done.stderr[-4000:])
    return exe


def render_from_run(azahar: str, cases: list[Case], cxx: str, keep: str | None, commit: str, cases_sha: str) -> str:
    tmp = keep or tempfile.mkdtemp(prefix="dsp_oracle_")
    try:
        os.makedirs(tmp, exist_ok=True)
        prepare_tree(azahar, tmp)
        exe = compile_driver(tmp, cxx)
        done = subprocess.run([exe], input=driver_input(cases), capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(f"the driver failed ({done.returncode}):\n{done.stderr[-2000:]}")
        return render_inc(cases, parse_driver_output(done.stdout, cases), commit, cases_sha)
    finally:
        if keep is None:
            shutil.rmtree(tmp, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True, help="Azahar's sources at the pinned commit (~/azahar)")
    parser.add_argument("--out", required=True, help="the table to write (native/tests/dsp_hle_expected.inc)")
    parser.add_argument("--cases", default=DEFAULT_CASES)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--keep", help="build in this folder and keep it")
    args = parser.parse_args(argv)
    try:
        with open(args.cases, encoding="utf-8") as f:
            text = f.read()
        cases = parse_cases(text)
        azahar = os.path.abspath(os.path.expanduser(args.azahar))
        commit = azahar_commit(azahar)
        if commit != pinned_commit():
            raise RuntimeError(f"{azahar} is at {commit}, the pinned commit is {pinned_commit()}")
        table = render_from_run(azahar, cases, args.cxx, args.keep, commit,
                                hashlib.sha256(text.encode("utf-8")).hexdigest())
    except (CaseError, RuntimeError, subprocess.CalledProcessError, OSError) as error:
        detail = getattr(error, "stderr", "") or ""
        print(f"dsp_oracle: {error}\n{detail}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(table)
    print(f"dsp_oracle: {len(cases)} cases -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
