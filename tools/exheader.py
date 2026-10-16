#!/usr/bin/env python3
# Derived in part from Azahar src/core/file_sys/ncch_container.h and src/core/hle/kernel/process.cpp
# at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.h says so, process.cpp
# says GPL-2.0-or-later): the layout of the ARM11 local capabilities (parse_local_caps) and
# Process::ParseKernelCaps (parse_kernel_caps), translated to Python. See native/THIRD_PARTY.md.
# Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.h)
# Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp)
"""Parser for the 3DS NCCH extended header (exheader) — system control info part.

Layout of the first 0x40 bytes (SCI), little-endian:
    0x00  char[8]  application title
    0x08  u8[5]    reserved
    0x0D  u8       flags (bit 0: ExeFS .code is compressed, bit 1: SD application)
    0x0E  u16      remaster version
    0x10  CodeSetInfo text   (u32 address, u32 size in pages, u32 size in bytes)
    0x1C  u32      stack size
    0x20  CodeSetInfo ro
    0x2C  u32      reserved
    0x30  CodeSetInfo data
    0x3C  u32      bss size
The program ID / title ID of the ACI lives at 0x200 (u64).

ARM11 local system capabilities (ACI at 0x200, 0x170 bytes):
    0x200  u64     program id
    0x208  u32     core version
    0x20C  u8      bit 0 enable L2 cache, bit 1 enable 804 MHz CPU (New 3DS)
    0x20D  u8      New 3DS system mode (0 legacy, 1 prod, 2 dev1)
    0x20E  u8      bits 0-1 ideal processor, 2-3 affinity mask, 4-7 system mode (Old 3DS)
    0x20F  u8      main-thread priority
    0x210  u16     bits 0-6 maximum CPU (core 1), bit 7 schedule mode (0 single, 1 multi)
    0x212  ...     resource limit descriptors, storage info, service access control
    0x36F  u8      resource limit category (0 application, 1 sys applet, 2 lib applet, 3 other)
ARM11 kernel capabilities (0x370, 28 descriptors of u32 and 0x10 reserved bytes): the descriptor
type is told by its leading one bits, as Azahar's Process::ParseKernelCaps does.
Reference: https://www.3dbrew.org/wiki/NCCH/Extended_Header
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass

PAGE_SIZE = 0x1000
SCI_SIZE = 0x40
ACI_PROGRAM_ID_OFFSET = 0x200
LOCAL_CAPS_OFFSET = 0x200
LOCAL_CAPS_SIZE = 0x170
KERNEL_CAPS_OFFSET = LOCAL_CAPS_OFFSET + LOCAL_CAPS_SIZE
KERNEL_CAPS_COUNT = 28
KERNEL_CAPS_END = KERNEL_CAPS_OFFSET + 4 * KERNEL_CAPS_COUNT

MEMORY_REGION_NAMES = {0: "invalid", 1: "application", 2: "system", 3: "base"}
RESOURCE_LIMIT_CATEGORY_NAMES = {0: "application", 1: "sys applet", 2: "lib applet", 3: "other"}


@dataclass(frozen=True)
class CodeSegment:
    address: int
    num_pages: int
    size: int

    @property
    def padded_size(self) -> int:
        return self.num_pages * PAGE_SIZE


@dataclass(frozen=True)
class LocalCaps:
    """ARM11 local system capabilities (the fields the loader uses)."""
    program_id: int
    core_version: int
    enable_l2_cache: bool
    enable_804mhz_cpu: bool
    n3ds_mode: int
    ideal_processor: int
    affinity_mask: int
    system_mode: int
    priority: int
    max_cpu: int
    schedule_mode: int
    resource_limit_category: int


@dataclass(frozen=True)
class AddressMapping:
    address: int
    size: int
    read_only: bool
    unk_flag: bool


@dataclass(frozen=True)
class KernelCaps:
    """ARM11 kernel capabilities, parsed as Azahar's Process::ParseKernelCaps does."""
    kernel_version: int | None
    handle_table_size: int | None
    flags: int | None               # the "misc flags" descriptor
    svc_mask: frozenset[int]        # allowed SVC numbers
    address_mappings: tuple[AddressMapping, ...]

    @property
    def memory_region(self) -> int:
        """Default allocation region from the misc flags (bits 8-11); APPLICATION (1) when the
        descriptor is absent, as Azahar's CreateProcess sets it."""
        return 1 if self.flags is None else (self.flags >> 8) & 0xF


@dataclass(frozen=True)
class ExHeader:
    title: str
    flags: int
    remaster_version: int
    text: CodeSegment
    stack_size: int
    ro: CodeSegment
    data: CodeSegment
    bss_size: int
    program_id: int | None
    local_caps: LocalCaps | None = None
    kernel_caps: KernelCaps | None = None

    @property
    def code_compressed(self) -> bool:
        return bool(self.flags & 0x1)

    @property
    def bss_address(self) -> int:
        return self.data.address + self.data.size


def _segment(buf: bytes, offset: int) -> CodeSegment:
    return CodeSegment(*struct.unpack_from("<III", buf, offset))


def parse_local_caps(buf: bytes) -> LocalCaps:
    o = LOCAL_CAPS_OFFSET
    program_id, core_version = struct.unpack_from("<QI", buf, o)
    cpu_flags, n3ds_mode, flags0, priority = struct.unpack_from("<4B", buf, o + 0x0C)
    (core1,) = struct.unpack_from("<H", buf, o + 0x10)
    return LocalCaps(
        program_id=program_id,
        core_version=core_version,
        enable_l2_cache=bool(cpu_flags & 1),
        enable_804mhz_cpu=bool(cpu_flags & 2),
        n3ds_mode=n3ds_mode,
        ideal_processor=flags0 & 3,
        affinity_mask=(flags0 >> 2) & 3,
        system_mode=flags0 >> 4,
        priority=priority,
        max_cpu=core1 & 0x7F,
        schedule_mode=(core1 >> 7) & 1,
        resource_limit_category=buf[o + LOCAL_CAPS_SIZE - 1],
    )


def parse_kernel_caps(descriptors: list[int]) -> KernelCaps:
    kernel_version = handle_table_size = flags = None
    svc: set[int] = set()
    mappings: list[AddressMapping] = []
    i = 0
    n = len(descriptors)
    while i < n:
        d = descriptors[i]
        t = d >> 20
        if d == 0xFFFFFFFF:
            pass                                    # unused entry
        elif (t & 0xF00) == 0xE00:
            pass                                    # allowed interrupts, ignored
        elif (t & 0xF80) == 0xF00:                  # allowed SVCs: 24 per descriptor
            index = ((d >> 24) & 7) * 24
            bits = d & 0xFFFFFF
            while bits and index < 0x80:
                if bits & 1:
                    svc.add(index)
                index += 1
                bits >>= 1
        elif (t & 0xFF0) == 0xFE0:
            handle_table_size = d & 0x3FF
        elif (t & 0xFF8) == 0xFF0:
            flags = d & 0xFFFF
        elif (t & 0xFFE) == 0xFF8:                  # mapped range: two descriptors
            if i + 1 >= n or ((descriptors[i + 1] >> 20) & 0xFFE) != 0xFF8:
                i += 1
                continue                            # incomplete, ignored
            end_desc = descriptors[i + 1]
            i += 1
            address = (d << 12) & 0xFFFFFFFF
            end = (end_desc << 12) & 0xFFFFFFFF
            mappings.append(AddressMapping(address, end - address if address < end else 0,
                                           bool(d & (1 << 20)), bool(end_desc & (1 << 20))))
        elif (t & 0xFFF) == 0xFFE:                  # one mapped page
            mappings.append(AddressMapping((d << 12) & 0xFFFFFFFF, PAGE_SIZE, False, False))
        elif (t & 0xFE0) == 0xFC0:
            kernel_version = d & 0xFFFF
        i += 1
    return KernelCaps(kernel_version, handle_table_size, flags, frozenset(svc), tuple(mappings))


def parse(buf: bytes) -> ExHeader:
    if len(buf) < SCI_SIZE:
        raise ValueError(f"exheader too small (0x{len(buf):X} bytes)")
    title = buf[0:8].split(b"\0", 1)[0].decode("ascii", errors="replace")
    flags = buf[0x0D]
    (remaster,) = struct.unpack_from("<H", buf, 0x0E)
    (stack,) = struct.unpack_from("<I", buf, 0x1C)
    (bss,) = struct.unpack_from("<I", buf, 0x3C)
    program_id = None
    if len(buf) >= ACI_PROGRAM_ID_OFFSET + 8:
        (program_id,) = struct.unpack_from("<Q", buf, ACI_PROGRAM_ID_OFFSET)
    local_caps = kernel_caps = None
    if len(buf) >= KERNEL_CAPS_END:
        local_caps = parse_local_caps(buf)
        kernel_caps = parse_kernel_caps(
            list(struct.unpack_from(f"<{KERNEL_CAPS_COUNT}I", buf, KERNEL_CAPS_OFFSET)))
    return ExHeader(
        title=title,
        flags=flags,
        remaster_version=remaster,
        text=_segment(buf, 0x10),
        stack_size=stack,
        ro=_segment(buf, 0x20),
        data=_segment(buf, 0x30),
        bss_size=bss,
        program_id=program_id,
        local_caps=local_caps,
        kernel_caps=kernel_caps,
    )


def describe(h: ExHeader) -> str:
    def seg(name: str, s: CodeSegment) -> str:
        return (f"  {name:<5} addr=0x{s.address:08X} size=0x{s.size:08X} "
                f"pages=0x{s.num_pages:X} (end=0x{s.address + s.size:08X})")
    lines = [
        f"title        : {h.title}",
        f"program id   : {h.program_id:016X}" if h.program_id is not None else "program id   : (exheader truncated)",
        f"remaster ver : {h.remaster_version}",
        f"code compressed: {h.code_compressed}",
        f"stack size   : 0x{h.stack_size:X}",
        "segments:",
        seg("text", h.text),
        seg("ro", h.ro),
        seg("data", h.data),
        f"  bss   addr=0x{h.bss_address:08X} size=0x{h.bss_size:08X}",
    ]
    lc, kc = h.local_caps, h.kernel_caps
    if lc is not None:
        lines += [
            "ARM11 local capabilities:",
            f"  main thread priority : {lc.priority}",
            f"  ideal processor      : {lc.ideal_processor} (affinity mask {lc.affinity_mask})",
            f"  system mode          : {lc.system_mode}",
            f"  New 3DS mode         : {lc.n3ds_mode} (L2 cache {int(lc.enable_l2_cache)}, "
            f"804 MHz {int(lc.enable_804mhz_cpu)})",
            f"  core 1 schedule mode : {lc.schedule_mode} ({'multi' if lc.schedule_mode else 'single'}),"
            f" max cpu {lc.max_cpu}",
            f"  resource limit cat.  : {lc.resource_limit_category} "
            f"({RESOURCE_LIMIT_CATEGORY_NAMES.get(lc.resource_limit_category, '?')})",
            f"  core version         : 0x{lc.core_version:X}",
        ]
    if kc is not None:
        lines += ["ARM11 kernel capabilities:"]
        lines.append("  kernel version       : " + (
            f"0x{kc.kernel_version:X}" if kc.kernel_version is not None else "(none)"))
        lines.append("  handle table size    : " + (
            str(kc.handle_table_size) if kc.handle_table_size is not None else "(none)"))
        lines.append(f"  memory region        : {kc.memory_region} "
                     f"({MEMORY_REGION_NAMES.get(kc.memory_region, '?')})")
        lines.append(f"  allowed SVCs         : {len(kc.svc_mask)}")
        lines.append(f"  address mappings     : {len(kc.address_mappings)}")
    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <exheader.bin>", file=sys.stderr)
        sys.exit(2)
    with open(sys.argv[1], "rb") as f:
        print(describe(parse(f.read())))
