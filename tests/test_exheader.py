"""tools/exheader.py: the SCI, the ARM11 local capabilities and the kernel capabilities, from
synthetic exheaders built here (3dbrew NCCH/Extended Header layout; checked against Azahar's
ExHeader_ARM11_SystemLocalCaps and Process::ParseKernelCaps)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import exheader as ex  # noqa: E402

UNUSED = 0xFFFFFFFF


def svc_desc(group: int, bits: int) -> int:
    return 0xF0000000 | (group << 24) | bits


def make_exheader(*, title=b"SYNTH", text=(0x00100000, 0x10, 0xF123), ro=(0x00200000, 4, 0x3100),
                  data=(0x00300000, 8, 0x7000), bss=0x2345, stack=0x4000, compressed=True,
                  program_id=0x0004000000ABCD00, core_version=2, cpu_flags=0, n3ds_mode=0,
                  ideal=1, affinity=2, system_mode=2, priority=0x30, max_cpu=0x59, schedule=1,
                  category=0, kernel_caps=None) -> bytes:
    b = bytearray(0x800)
    b[0:len(title)] = title
    b[0x0D] = 1 if compressed else 0
    struct.pack_into("<III", b, 0x10, *text)
    struct.pack_into("<I", b, 0x1C, stack)
    struct.pack_into("<III", b, 0x20, *ro)
    struct.pack_into("<III", b, 0x30, *data)
    struct.pack_into("<I", b, 0x3C, bss)
    struct.pack_into("<QI", b, 0x200, program_id, core_version)
    b[0x20C] = cpu_flags
    b[0x20D] = n3ds_mode
    b[0x20E] = ideal | (affinity << 2) | (system_mode << 4)
    b[0x20F] = priority
    struct.pack_into("<H", b, 0x210, max_cpu | (schedule << 7))
    b[0x36F] = category
    caps = list(kernel_caps or [])
    caps += [UNUSED] * (28 - len(caps))
    struct.pack_into("<28I", b, 0x370, *caps)
    return bytes(b)


def test_sci_fields_are_unchanged():
    h = ex.parse(make_exheader())
    assert h.title == "SYNTH"
    assert h.code_compressed
    assert (h.text.address, h.text.num_pages, h.text.size) == (0x00100000, 0x10, 0xF123)
    assert h.stack_size == 0x4000
    assert h.bss_size == 0x2345 and h.bss_address == 0x00300000 + 0x7000
    assert h.program_id == 0x0004000000ABCD00


def test_local_caps_fields():
    h = ex.parse(make_exheader(cpu_flags=3, n3ds_mode=2, category=3))
    lc = h.local_caps
    assert lc is not None
    assert lc.program_id == 0x0004000000ABCD00 and lc.core_version == 2
    assert lc.enable_l2_cache and lc.enable_804mhz_cpu and lc.n3ds_mode == 2
    assert (lc.ideal_processor, lc.affinity_mask, lc.system_mode) == (1, 2, 2)
    assert lc.priority == 0x30
    assert (lc.max_cpu, lc.schedule_mode) == (0x59, 1)
    assert lc.resource_limit_category == 3


def test_local_caps_bit_boundaries():
    h = ex.parse(make_exheader(cpu_flags=1, ideal=3, affinity=0, system_mode=15, max_cpu=0x7F,
                               schedule=0))
    lc = h.local_caps
    assert lc.enable_l2_cache and not lc.enable_804mhz_cpu
    assert (lc.ideal_processor, lc.affinity_mask, lc.system_mode) == (3, 0, 15)
    assert (lc.max_cpu, lc.schedule_mode) == (0x7F, 0)


def test_kernel_caps_descriptors():
    caps = [
        0xE2000000 | 0x1F,                       # allowed interrupts: ignored
        svc_desc(0, 0x00FFFF),                   # SVCs 0..15
        svc_desc(1, 0x000005),                   # SVCs 24 and 26
        0xFE000000 | 0x200,                      # handle table size 0x200
        0xFF000000 | (2 << 8) | 0x4,             # misc flags: region 2 (system), bit 2
        0xFC000000 | 0x22C,                      # kernel version 2.44
        0xFF800000 | 0x1FF50,                    # range start (read only: bit 20 set below)
        0xFF800000 | 0x1FF58,                    # range end
        0xFFE00000 | 0x1EC40,                    # single page
        UNUSED,
    ]
    # Bit 20 of the first descriptor of a range marks it read only.
    caps[6] |= 1 << 20
    h = ex.parse(make_exheader(kernel_caps=caps))
    kc = h.kernel_caps
    assert kc is not None
    assert kc.kernel_version == 0x22C
    assert kc.handle_table_size == 0x200
    assert kc.flags == (2 << 8) | 4 and kc.memory_region == 2
    assert kc.svc_mask == frozenset(list(range(16)) + [24, 26])
    assert len(kc.address_mappings) == 2
    r, p = kc.address_mappings
    assert (r.address, r.size, r.read_only, r.unk_flag) == (0x1FF50000, 0x8000, True, False)
    assert (p.address, p.size, p.read_only) == (0x1EC40000, 0x1000, False)


def test_kernel_caps_incomplete_range_and_unknown_are_skipped():
    caps = [0xFF800000 | 0x1000, 0xFFE00000 | 0x1234, 0xFC000000 | 0x200]
    kc = ex.parse(make_exheader(kernel_caps=caps)).kernel_caps
    # The first descriptor wants a second range descriptor and gets a page one: ignored, and the
    # page descriptor is then handled on its own, as in Azahar.
    assert [(m.address, m.size) for m in kc.address_mappings] == [(0x1234000, 0x1000)]
    assert kc.kernel_version == 0x200
    assert kc.handle_table_size is None and kc.flags is None and kc.memory_region == 1


def test_range_with_end_before_start_has_size_zero():
    caps = [0xFF800000 | 0x2000, 0xFF800000 | 0x1000]
    kc = ex.parse(make_exheader(kernel_caps=caps)).kernel_caps
    assert [(m.address, m.size) for m in kc.address_mappings] == [(0x2000000, 0)]


def test_short_exheader_has_no_caps():
    h = ex.parse(make_exheader()[:0x208])
    assert h.local_caps is None and h.kernel_caps is None
    assert h.program_id == 0x0004000000ABCD00


def test_describe_lists_the_new_fields():
    text = ex.describe(ex.parse(make_exheader(kernel_caps=[0xFC000000 | 0x23A])))
    assert "main thread priority : 48" in text
    assert "kernel version       : 0x23A" in text
    assert "core 1 schedule mode : 1 (multi), max cpu 89" in text
