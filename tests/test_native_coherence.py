# Derived from Azahar src/core/memory.h and memory.cpp, src/core/hle/service/gsp/gsp_command.h,
# src/video_core/gpu.cpp, src/video_core/pica/regs_external.h, regs_internal.h, regs_texturing.h, regs_pipeline.h and
# pica_core.cpp, src/video_core/rasterizer_accelerated.cpp, src/video_core/renderer_opengl/gl_rasterizer.cpp and
# src/video_core/rasterizer_cache/rasterizer_cache.h, pixel_format.h, pixel_format.cpp, utils.cpp and
# surface_params.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; each says "Licensed under GPLv2
# or any later version"): the synthetic captures and the scripted game use the addresses of the windows of memory.h
# (the linear heap, the new linear heap, VRAM and the physical bases of FCRAM and VRAM) and of its application heap
# and process image (not windows), the argument layouts of the GX commands of gsp_command.h (RequestDma,
# SubmitCmdList, MemoryFill, DisplayTransfer, TextureCopy, CacheFlush), the input size fields, the position of the
# input pixel format in the flags and the bytes per pixel of a display transfer (regs_external.h), and the positions
# and fields of the texture registers (regs_internal.h, regs_texturing.h) and the fields of the vertex attribute,
# index and command buffer registers (regs_pipeline.h) that the synthetic register writes set; the expected values
# follow gpu.cpp (a SubmitCmdList sets channel 0 of the command buffer and ignores do_flush, a DMA reads its source
# through the game's page table, and an address outside the windows is passed through), pica_core.cpp (how a masked
# write lands, and that a command buffer trigger jumps whatever its byte mask), rasterizer_accelerated.cpp and
# gl_rasterizer.cpp (the vertices a draw spans and the bytes each attribute loader reads for them), gl_rasterizer.cpp
# and rasterizer_cache.h (the texture units a draw binds and the size of a cube face), and pixel_format.h,
# pixel_format.cpp, utils.cpp and surface_params.cpp (the bits per pixel of each known texture format, and a
# texture's mip levels and size); the fake marker follows memory.cpp (RasterizerMarkRegionCached: marking a marked
# page or unmarking an unmarked one is UNREACHABLE()), and the patch test expects a hook at every rasterizer-cached
# case of memory.cpp.
# Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (memory.h, memory.cpp)
# Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gpu.cpp, regs_external.h, pica_core.cpp,
#   rasterizer_accelerated.cpp, pixel_format.h)
# Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
# Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_pipeline.h)
# Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (gl_rasterizer.cpp)
# Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h)
# Copyright 2023 Citra Emulator Project (pixel_format.cpp, utils.cpp)
# Copyright 2022 Citra Emulator Project (surface_params.cpp)
"""tools/native/coherence.py and the coherence check of port/src/runner.cpp (N0-14), without Azahar or the game.

(a) The report from a capture is tested with synthetic captures made with gcap.Writer; (b) the report from the
runner's log with synthetic logs, and with the logs the runner's checking logic writes: runner.cpp keeps that
logic between the markers `BEGIN coherence-core` and `END coherence-core`, free of Azahar types, and these tests
compile it together with `gpu-capture-core` (it reuses the capture's page diff) and drive it with a scripted game
and a fake marker that, like RasterizerMarkRegionCached, aborts when a page already marked is marked again or a
page that is not marked is unmarked.  The hooks of azahar.patch and the runner's glue (page table, frame counter,
GPU hooks) can only be exercised by the CI runner job and by a run with the game; the patch is checked here for
applying to the pinned Azahar sources when they are around, and for having a hook at every rasterizer-cached case.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))

import coherence as coh  # noqa: E402
import gcap  # noqa: E402

RUNNER = os.path.join(ROOT, "port", "src", "runner.cpp")
PATCH = os.path.join(ROOT, "port", "azahar", "azahar.patch")
PINNED = os.path.join(ROOT, "port", "azahar", "COMMIT")
PAGE = gcap.PAGE_SIZE
FCRAM, VRAM = coh.FCRAM_PADDR, coh.VRAM_PADDR
LINEAR, VRAM_V = coh.LINEAR_HEAP_VADDR, coh.VRAM_VADDR


# ── (a) captures ──────────────────────────────────────────────────────────────────────

def page(fill: int, patch: dict[int, int] | None = None) -> bytes:
    data = bytearray([fill]) * PAGE
    for off, value in (patch or {}).items():
        data[off] = value
    return bytes(data)


def make_capture(path: str, build, init_blocks=None) -> None:
    """A capture with 16 pages of FCRAM (page i filled with 0x10+i) and 4 of VRAM, then whatever BUILD writes.
    INIT_BLOCKS replace the zero-filled blocks of the INIT record."""
    with gcap.Writer(path) as w:
        w.init(init_blocks)
        w.mem(FCRAM, b"".join(page(0x10 + i) for i in range(16)))
        w.mem(VRAM, b"".join(page(0x80 + i) for i in range(4)))
        build(w)


def cpu_change(w: gcap.Writer, changes: dict[int, bytes]) -> None:
    w.cpu_pages(sorted(changes.items()))


def test_virtual_to_physical_is_the_fixed_mapping_of_the_gpu():
    assert coh.virtual_to_physical(LINEAR + 0x1234) == FCRAM + 0x1234
    assert coh.virtual_to_physical(coh.NEW_LINEAR_HEAP_VADDR + 0x4000) == FCRAM + 0x4000
    assert coh.virtual_to_physical(VRAM_V + 0x10) == VRAM + 0x10
    assert coh.virtual_to_physical(0) is None                      # the GPU passes 0 through
    assert coh.virtual_to_physical(0x08000000) is None             # the application heap is not a window
    assert coh.virtual_to_physical(0x00100000) is None
    assert coh.physical_range(LINEAR + 0x100, 0x40) == (FCRAM + 0x100, FCRAM + 0x140)
    assert coh.physical_range(VRAM_V + coh.VRAM_SIZE - 0x10, 0x100) == (VRAM + coh.VRAM_SIZE - 0x10, VRAM + coh.VRAM_SIZE)
    assert coh.physical_range(0x08000000, 0x100) is None


def test_named_input_ranges_follow_the_words_of_each_command():
    assert coh.named_input([0, 0x14000000, 0x14100000, 0x300, 0, 0, 0, 0]) == (0x14000000, 0x300)       # DMA source
    assert coh.named_input([1, 0x14002000, 0x80, 0, 0, 0, 0, 0]) == (0x14002000, 0x80)                  # command list
    assert coh.named_input([2, 0x14000000, 0, 0x14001000, 0, 0, 0, 0]) is None                         # fill
    assert coh.named_input([4, 0x14003000, 0x14004000, 0x200, 0, 0, 0, 0]) == (0x14003000, 0x200)       # texture copy
    rgb8 = 1 << 8
    assert coh.named_input([3, VRAM_V, 0x14000000, (8 << 16) | 16, 0, rgb8, 0, 0]) == (VRAM_V, 16 * 8 * 3)
    assert coh.named_input([3, VRAM_V, 0x14000000, (8 << 16) | 16, 0, 0, 0, 0]) == (VRAM_V, 16 * 8 * 4)
    assert coh.named_input([5, 0x14000000, 0x10, 0, 0, 0, 0, 0]) is None                                # cache flush


def basic_capture(w: gcap.Writer) -> None:
    # event 1 (a command list): the first interval, page 1 changed in 3 bytes; a FlushDataCache covers the page
    w.flush(1, LINEAR + PAGE, PAGE)
    cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 1, 1: 2, 2: 3})})
    w.submit_cmdlist(LINEAR + PAGE, 0x80)
    w.pica_reg([])
    w.gpu_pages([(FCRAM + 8 * PAGE, page(0x77))])                   # the GPU writes page 8
    # event 2 (a display transfer): page 2 changed in 4 bytes, no flush at all; the transfer reads it
    cpu_change(w, {FCRAM + 2 * PAGE: page(0x12, {10: 0, 11: 0, 12: 0, 13: 0})})
    w.gx([3, LINEAR + 2 * PAGE, LINEAR + 9 * PAGE, (4 << 16) | 8, 0, 2 << 8, 0, 0])
    # event 3 (a command list): page 3 changed in 2 bytes, the flush covers one of them only
    w.flush(1, LINEAR + 3 * PAGE, 1)
    cpu_change(w, {FCRAM + 3 * PAGE: page(0x13, {0: 9, 100: 9})})
    w.submit_cmdlist(LINEAR + 5 * PAGE, 0x40)
    w.pica_reg([])
    # event 4 (a cache flush): page 4 changed, covered by this command's own region; page 5 only invalidated
    w.flush(2, LINEAR + 5 * PAGE, PAGE)
    cpu_change(w, {FCRAM + 4 * PAGE: page(0x14, {7: 7}), FCRAM + 5 * PAGE: page(0x15, {8: 8})})
    w.gx([5, LINEAR + 4 * PAGE, PAGE, 0, 0, 0, 0, 0])
    w.flush(4, LINEAR + 4 * PAGE, PAGE)
    # a flush of a heap address that maps nowhere, a store, then a register write, a swap and a fill
    w.flush(3, 0x08000000, 0x1000)
    cpu_change(w, {FCRAM + 6 * PAGE: page(0x16, {1: 1}), VRAM: page(0x80, {2: 2})})
    w.mmio(0, 0)
    w.gpu_pages([(FCRAM + 8 * PAGE, page(0x78))])
    cpu_change(w, {FCRAM + 7 * PAGE: page(0x17, {3: 3})})
    w.swap(0, [0] * 7)
    w.fill(0)                                                       # nothing changed before it
    # event: the CPU changes the page the GPU wrote (page 8): the change is measured against the GPU's data
    w.flush(4, LINEAR + 8 * PAGE, 16)
    cpu_change(w, {FCRAM + 8 * PAGE: page(0x78, {0: 1, 1: 1})})
    w.submit_cmdlist(LINEAR + 8 * PAGE, 0x10)
    w.pica_reg([])


@pytest.fixture()
def basic_report(tmp_path):
    path = str(tmp_path / "basic.mgpu")
    make_capture(path, basic_capture)
    assert gcap.check_file(path) == []
    return coh.analyse_capture(gcap.read(path)), path


def test_a_capture_is_counted_per_event_kind(basic_report):
    rep, _ = basic_report
    assert rep["events"] == 8 and rep["first_event"] == "SUBMIT_CMDLIST"
    rows = rep["rows"]
    assert sorted(rows) == sorted([coh.FIRST_ROW, "DISPLAY_TRANSFER", "SUBMIT_CMDLIST", "CACHE_FLUSH", "MMIO",
                                   "SWAP", "FILL"])
    assert rows[coh.FIRST_ROW]["events"] == 1 and rows["SUBMIT_CMDLIST"]["events"] == 2
    assert rows["FILL"]["events"] == 1 and rows["FILL"]["pages"] == 0
    assert rep["total"]["events"] == 8


def test_changed_bytes_are_covered_by_the_flushes_recorded_in_the_interval(basic_report):
    rows = basic_report[0]["rows"]
    first = rows[coh.FIRST_ROW]                                      # page 1: 3 bytes changed, flushed
    assert (first["pages"], first["changed_bytes"]) == (1, 3)
    assert first["covered_bytes"]["any"] == 3 and first["covered_bytes"]["flush"] == 3
    assert first["pages_full_any"] == 1 and first["pages_full_p2"] == 1
    xfer = rows["DISPLAY_TRANSFER"]                                  # page 2: 4 bytes changed, no flush
    assert (xfer["pages"], xfer["changed_bytes"], xfer["pages_none_any"]) == (1, 4, 1)
    assert xfer["covered_bytes"]["any"] == 0
    # the transfer reads 4 x 8 x 2 = 64 bytes from page 2: the page is named by the command and not covered
    assert xfer["named_pages"] == 1 and xfer["named_not_covered_any"] == 1 and xfer["named_not_covered_p2"] == 1
    cmd = rows["SUBMIT_CMDLIST"]                                     # page 3 (1 of 2 bytes flushed) and page 8
    assert cmd["pages"] == 2 and cmd["changed_bytes"] == 2 + 2
    assert cmd["pages_partial_any"] == 1 and cmd["covered_bytes"]["any"] == 1 + 2
    # the command list buffer is page 5 for event 3: page 3 is not in it; page 8 is the buffer of the last one
    assert cmd["named_pages"] == 1 and cmd["named_not_covered_any"] == 0


def test_flush_kinds_are_kept_apart_and_principle_two_counts_flush_and_region_only(basic_report):
    rows = basic_report[0]["rows"]
    cache = rows["CACHE_FLUSH"]          # page 4 covered by the command's own region, page 5 only invalidated
    assert cache["pages"] == 2 and cache["changed_bytes"] == 2
    assert cache["covered_bytes"]["region"] == 1 and cache["covered_bytes"]["invalidate"] == 1
    assert cache["covered_bytes"]["any"] == 2 and cache["covered_bytes"]["p2"] == 1
    assert cache["pages_full_any"] == 2 and cache["pages_full_p2"] == 1 and cache["pages_none_p2"] == 1
    flushes = basic_report[0]["flushes"]
    assert flushes["FlushDataCache"]["count"] == 2 and flushes["InvalidateDataCache"]["count"] == 1
    assert flushes["StoreDataCache"] == {"count": 1, "maps_nowhere": 1}
    assert flushes["CacheFlushRegion"]["count"] == 2


def test_cpu_changes_before_mmio_swap_and_fill_are_their_own_rows(basic_report):
    rows = basic_report[0]["rows"]
    assert rows["MMIO"]["pages"] == 2 and rows["MMIO"]["changed_bytes"] == 2   # a store flush that maps nowhere covers nothing
    assert rows["MMIO"]["pages_none_any"] == 2
    assert rows["SWAP"]["pages"] == 1 and rows["SWAP"]["changed_bytes"] == 1


def test_a_page_the_gpu_wrote_is_compared_with_the_gpu_data(basic_report):
    rows = basic_report[0]["rows"]
    # page 8 is 0x78 after the GPU wrote it; the CPU changed 2 bytes, both flushed by the region: the 16 flushed bytes
    cmd = rows["SUBMIT_CMDLIST"]
    assert cmd["covered_bytes"]["region"] == 2


def test_uncovered_ranges_are_grouped(tmp_path):
    def build(w):
        w.submit_cmdlist(LINEAR, 4)
        w.pica_reg([])
        cpu_change(w, {FCRAM + i * PAGE: page(0x10 + i, {0: 0xEE}) for i in (1, 2, 3, 7)})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])
    path = str(tmp_path / "ranges.mgpu")
    make_capture(path, build)
    rep = coh.analyse_capture(gcap.read(path))
    info = rep["not_fully_covered"]["MEMORY_FILL"]
    assert info["pages"] == 4 and info["ranges"] == 2
    assert info["largest"][0] == {"paddr": FCRAM + PAGE, "pages": 3}
    assert coh.group_pages([FCRAM, FCRAM + PAGE, FCRAM + 3 * PAGE]) == [(FCRAM, 2), (FCRAM + 3 * PAGE, 1)]


def test_a_flush_after_an_event_does_not_cover_that_event_but_covers_the_next_one(tmp_path):
    def build(w):
        w.submit_cmdlist(LINEAR, 4)                                  # the first event, nothing changed
        w.pica_reg([])
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 1})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # event 2: page 1 changed, no flush yet
        w.flush(1, LINEAR + PAGE, PAGE)                              # too late for event 2
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 2})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # event 3: the same page again, now flushed before it
    path = str(tmp_path / "late.mgpu")
    make_capture(path, build)
    row = coh.analyse_capture(gcap.read(path))["rows"]["MEMORY_FILL"]
    assert row["events"] == 2 and row["pages"] == 2
    assert (row["pages_none_any"], row["pages_full_any"]) == (1, 1)
    assert row["covered_bytes"]["any"] == 1 and row["changed_bytes"] == 2


def test_a_flush_covers_only_the_changes_since_the_previous_diff_point(tmp_path):
    def build(w):
        w.submit_cmdlist(LINEAR, 4)                                  # the first event: its interval is the special one
        w.pica_reg([])
        w.flush(1, LINEAR + PAGE, PAGE)                              # flush page 1 ...
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 1})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # ... event 2: the CPU changed it: covered
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 2})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # event 3: changed again, no new flush: uncovered
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 3})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # event 4: and again
    path = str(tmp_path / "interval.mgpu")
    make_capture(path, build)
    row = coh.analyse_capture(gcap.read(path))["rows"]["MEMORY_FILL"]
    assert row["events"] == 3 and row["pages"] == 3
    assert (row["pages_full_any"], row["pages_none_any"]) == (1, 2)
    assert row["covered_bytes"]["any"] == 1 and row["changed_bytes"] == 3


def test_unchanged_pages_and_an_empty_interval_are_not_counted_as_changes(tmp_path):
    def build(w):
        w.cpu_pages([(FCRAM + PAGE, page(0x11))])                    # the same bytes as MEM: no change
        w.submit_cmdlist(LINEAR, 4)
        w.pica_reg([])
    path = str(tmp_path / "same.mgpu")
    make_capture(path, build)
    first = coh.analyse_capture(gcap.read(path))["rows"][coh.FIRST_ROW]
    assert first["pages"] == 1 and first["unchanged_pages"] == 1 and first["changed_bytes"] == 0


def test_the_markdown_and_json_reports_and_the_command_line(basic_report, tmp_path, capsys):
    _, path = basic_report
    out_json, out_md = str(tmp_path / "r.json"), str(tmp_path / "r.md")
    assert coh.main(["capture", path, "--json", out_json, "--md", out_md]) == 0
    data = json.load(open(out_json))
    assert data[path]["events"] == 8
    text = open(out_md).read()
    assert "SUBMIT_CMDLIST" in text and "CacheFlushRegion" in text and "(first interval)" in text
    assert coh.main(["capture", path]) == 0
    assert "events: 8" in capsys.readouterr().out


def test_a_truncated_capture_is_an_error_not_a_report(tmp_path, capsys):
    path = str(tmp_path / "cut.mgpu")
    make_capture(path, basic_capture)
    data = open(path, "rb").read()
    open(path, "wb").write(data[:-10])
    assert coh.main(["capture", path]) == 1
    assert "truncated" in capsys.readouterr().err
    assert coh.main(["capture", str(tmp_path / "missing.mgpu")]) == 1


# ── (a) the read-based answer ─────────────────────────────────────────────────────────

def report_of(tmp_path, build, alias_runs=None, init_blocks=None, top=10):
    path = str(tmp_path / "reads.mgpu")
    make_capture(path, build, init_blocks)
    assert gcap.check_file(path) == []
    return coh.analyse_capture(gcap.read(path), top, alias_runs)


def classes(rc, kind, part="bytes"):
    """(certain, same interval, not covered) of one kind of read."""
    return tuple(rc["reads"][kind][part][c] for c in coh.CLASSES)


def dma(src, size):
    return [0, src, LINEAR + 15 * PAGE, size, 0, 0, 0, 0]


def cache_flush(w, vaddr, size):
    """A CacheFlush GX command with one region, and its FLUSH record, as the recorder writes them."""
    w.gx([5, vaddr, size, 0, 0, 0, 0, 0])
    w.flush(4, vaddr, size)


def test_a_cache_flush_in_a_later_interval_covers_the_list_with_certain_order(tmp_path):
    def build(w):
        w.fill(0)                                                       # event 0: nothing changed before it
        cpu_change(w, {FCRAM + 2 * PAGE: page(0x12, {0x10: 1, 0x11: 2})})   # interval 1: the game writes its list
        w.swap(0, [0] * 7)                                              # event 1
        cpu_change(w, {FCRAM + 3 * PAGE: page(0x13, {0x20: 3})})        # interval 2: more of it, in the flush's interval
        cache_flush(w, LINEAR + 2 * PAGE, 2 * PAGE)                     # event 2 flushes pages 2 and 3
        w.submit_cmdlist(LINEAR + 2 * PAGE, 2 * PAGE)                   # event 3 reads them
        w.pica_reg([])
    rep = report_of(tmp_path, build)
    rc = rep["read_coverage"]
    assert classes(rc, "cmdlist") == (2, 1, 0)                          # page 2 certain, page 3 same interval
    assert classes(rc, "cmdlist", "pairs") == (1, 1, 0) and classes(rc, "cmdlist", "pages") == (1, 1, 0)
    assert classes(rc, "total") == (2, 1, 0) and rc["reads"]["total"]["uncovered"]["pages"] == 0
    assert rc["counts"]["cache_flush"] == 1 and rc["counts"]["cache_flush_equals_next_buffer"] == 1
    # the same-interval rule calls the change before SWAP uncovered: no flush in its own interval
    swap = rep["rows"]["SWAP"]
    assert swap["changed_bytes"] == 2 and swap["covered_bytes"]["any"] == 0 and swap["pages_none_any"] == 1


def test_a_flush_in_the_same_interval_as_the_change_covers_it_in_the_same_interval_only(tmp_path):
    def build(w):
        w.fill(0)
        w.flush(1, LINEAR + 3 * PAGE, PAGE)                             # interval 1: a FlushDataCache ...
        cpu_change(w, {FCRAM + 3 * PAGE: page(0x13, {5: 0, 6: 0})})     # ... and the change: their order is unknown
        w.gx(dma(LINEAR + 3 * PAGE, 0x10))                              # event 1: a DMA reads 16 bytes of page 3
    rep = report_of(tmp_path, build)
    rc = rep["read_coverage"]
    assert classes(rc, "dma_source") == (0, 2, 0) and classes(rc, "dma_source", "pairs") == (0, 1, 0)
    assert classes(rc, "total", "pages") == (0, 1, 0)
    assert rep["rows"]["REQUEST_DMA"]["covered_bytes"]["any"] == 2      # the rows agree: same interval
    # the upper bound by region counts a flush of the change's own interval
    assert rc["regions"]["FCRAM"]["flushed_by_end"] == 2 and rc["changes"]["flushed_by_end"] == 2


def test_a_flush_in_an_earlier_interval_does_not_cover_a_later_change(tmp_path):
    def build(w):
        w.fill(0)
        w.flush(1, LINEAR + 6 * PAGE, PAGE)                             # interval 1: a flush, nothing changed yet
        w.swap(0, [0] * 7)                                              # event 1
        cpu_change(w, {FCRAM + 6 * PAGE: page(0x16, {1: 1})})           # interval 2: the change
        w.gx(dma(LINEAR + 6 * PAGE, 0x10))                              # event 2 reads it
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "dma_source") == (0, 0, 1)
    assert rc["regions"]["FCRAM"]["flushed_by_end"] == 0 and rc["changes"]["flushed_by_end"] == 0


def test_pairs_take_the_worst_class_of_their_bytes_and_certain_order_wins(tmp_path):
    def build(w):
        w.fill(0)
        w.flush(1, LINEAR + 2 * PAGE, 1)                                # interval 1: byte 0 of page 2, order unknown
        cpu_change(w, {FCRAM + 2 * PAGE: page(0x12, {0: 1}),            # flushed here and again later: certain
                       FCRAM + 3 * PAGE: page(0x13, {0: 1, 1: 1}),      # byte 0 flushed later, byte 1 never
                       FCRAM + 4 * PAGE: page(0x14, {0: 1})})           # page 4 byte 0 ...
        w.swap(0, [0] * 7)                                              # event 1
        w.flush(1, LINEAR + 2 * PAGE, 1)                                # interval 2
        w.flush(1, LINEAR + 3 * PAGE, 1)
        cpu_change(w, {FCRAM + 4 * PAGE: page(0x14, {0: 1, 1: 1})})     # ... and byte 1, a change of interval 2
        w.gx(dma(LINEAR + 2 * PAGE, 3 * PAGE))                          # event 2 reads pages 2 to 4
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "dma_source") == (2, 0, 3)
    # page 3 is one pair in its worst class; page 4 is two pairs, one per interval
    assert classes(rc, "dma_source", "pairs") == (1, 0, 3)
    assert classes(rc, "dma_source", "pages") == (1, 0, 2)


def test_invalidate_and_store_flushes_cover_reads_too(tmp_path):
    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 2 * PAGE: page(0x12, {0: 1}), FCRAM + 3 * PAGE: page(0x13, {0: 1})})
        w.swap(0, [0] * 7)
        w.flush(2, LINEAR + 2 * PAGE, PAGE)                             # InvalidateDataCache
        w.flush(3, LINEAR + 3 * PAGE, PAGE)                             # StoreDataCache
        w.gx(dma(LINEAR + 2 * PAGE, 2 * PAGE))
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "dma_source") == (2, 0, 0)


def test_a_read_with_no_flush_before_it_is_not_covered_and_listed(tmp_path):
    def build(w):
        w.fill(0)
        w.flush(1, 0x08000000, 16 * PAGE)                               # with no alias map a heap address covers nothing
        cpu_change(w, {FCRAM + i * PAGE: page(0x10 + i, {0: 0xEE}) for i in (4, 5, 7)})
        w.gx([4, LINEAR + 4 * PAGE, LINEAR + 12 * PAGE, 4 * PAGE, 0, 0, 0, 0])   # a texture copy of pages 4 to 7
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "transfer_input") == (0, 0, 3) and classes(rc, "transfer_input", "pages") == (0, 0, 3)
    for kind in ("transfer_input", "total"):
        info = rc["reads"][kind]["uncovered"]
        assert (info["pages"], info["ranges"]) == (3, 2)
        assert info["largest"] == [{"paddr": FCRAM + 4 * PAGE, "pages": 2}, {"paddr": FCRAM + 7 * PAGE, "pages": 1}]
    assert rc["reads"]["cmdlist"]["uncovered"] == {"pages": 0, "ranges": 0, "largest": []}
    top = report_of(tmp_path, build, top=1)["read_coverage"]["reads"]["total"]["uncovered"]   # --top 1
    assert (top["ranges"], top["largest"]) == (2, [{"paddr": FCRAM + 4 * PAGE, "pages": 2}])


def test_a_flush_after_the_read_does_not_cover_that_read(tmp_path):
    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 6 * PAGE: page(0x16, {1: 1})})
        w.gx(dma(LINEAR + 6 * PAGE, 0x10))                              # event 1 reads the change
        w.flush(1, LINEAR + 6 * PAGE, PAGE)                             # interval 2: too late for event 1
        w.gx(dma(LINEAR + 6 * PAGE, 0x10))                              # event 2 reads the unchanged byte again
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "dma_source") == (0, 0, 1) and classes(rc, "dma_source", "pairs") == (0, 0, 1)
    region = rc["regions"]["FCRAM"]
    assert region["read_by_gpu"] == 1
    assert region["flushed_by_end"] == 1           # the upper bound by region counts the late flush; the read does not


def test_a_second_read_is_not_a_new_pair_and_a_change_after_a_read_is(tmp_path):
    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 6 * PAGE: page(0x16, {0: 1})})           # interval 1
        w.gx(dma(LINEAR + 6 * PAGE, 0x10))                              # event 1: a pair, not covered
        w.gx(dma(LINEAR + 6 * PAGE, 0x10))                              # event 2: the same unchanged byte: no pair
        cpu_change(w, {FCRAM + 6 * PAGE: page(0x16, {0: 2})})           # interval 3: changed again
        w.swap(0, [0] * 7)                                              # event 3
        w.flush(1, LINEAR + 6 * PAGE, PAGE)                             # interval 4
        w.gx(dma(LINEAR + 6 * PAGE, 0x10))                              # event 4: a new pair, certain
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "dma_source") == (1, 0, 1) and classes(rc, "dma_source", "pairs") == (1, 0, 1)
    assert classes(rc, "dma_source", "pages") == (0, 0, 1)             # a page takes the worst class of its reads
    assert rc["changes"]["changed_bytes"] == 2 and rc["changes"]["read_by_gpu"] == 2


CUBE_FACES = FCRAM + 0x01000000     # faces 1 to 5 of the cube: bit 21 of their address / 8 is set, not face 0's
ETC = FCRAM + 0x01100000


def texture_writes():
    """Draw 1: unit 0 a cube map, RGBA8, 16 wide and 8 high (a face is width x width) with max_level 2, face 0 at
    page 8 and faces 1 to 5 at CUBE_FACES + pages 9 to 13; unit 1 a 16x16 I8 texture with 2 levels at page 14, its
    type set to cube (only unit 0 has faces); unit 2 configured at page 15 but disabled.  Draw 2: the same, but unit 1
    is an ETC1A4 texture of 1024x8 at ETC.  Each draws 3 vertices with no attribute loader."""
    regs = {coh.REG_TEX_CONFIG: 0b011,
            0x82: (16 << 16) | 8, 0x83: 1 << 28, 0x84: 2 << 16, 0x85: (FCRAM + 8 * PAGE) >> 3, 0x8E: 0,
            0x92: (16 << 16) | 16, 0x93: 1 << 28, 0x94: 1 << 16, 0x95: (FCRAM + 14 * PAGE) >> 3, 0x96: 7,
            0x9A: (8 << 16) | 8, 0x9D: (FCRAM + 15 * PAGE) >> 3, 0x9E: 0,
            coh.REG_NUM_VERTICES: 3}
    for face in range(5):
        regs[coh.REG_CUBE_ADDR + face] = ((CUBE_FACES + (9 + face) * PAGE) >> 3) & 0x3FFFFF
    etc = {0x92: (1024 << 16) | 8, 0x93: 0, 0x95: ETC >> 3, 0x96: 13}
    return ([(reg, 0xF, value) for reg, value in regs.items()] + [(coh.REG_DRAW, 0xF, 1)]
            + [(reg, 0xF, value) for reg, value in etc.items()] + [(coh.REG_DRAW, 0xF, 1)])


def test_textures_are_read_from_the_register_replay(tmp_path):
    def build(w):
        w.fill(0)
        changes = {CUBE_FACES + p * PAGE: page(0, {0x10: 0xEE}) for p in range(9, 14)}   # a byte of faces 1 to 5
        changes[FCRAM + 8 * PAGE] = page(0x18, {0x10: 0xEE, 1343: 0xEE, 1344: 0xEE})   # face 0 is 1344 bytes
        changes[FCRAM + 14 * PAGE] = page(0x1E, {319: 0xEE, 320: 0xEE})     # level 1 ends at 16x16 + 8x8 = 320 bytes
        changes[FCRAM + 15 * PAGE] = page(0x1F, {0: 0xEE})                  # the disabled unit
        changes[ETC + PAGE] = page(0, {PAGE - 1: 0xEE})                     # the last of the 8192 bytes at ETC ...
        changes[ETC + 2 * PAGE] = page(0, {0: 0xEE})                        # ... and the first byte past them
        cpu_change(w, changes)
        w.submit_cmdlist(LINEAR, 0x10)
        w.pica_reg(texture_writes())
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "texture") == (0, 0, 9) and classes(rc, "texture", "pages") == (0, 0, 8)
    assert rc["reads"]["texture"]["uncovered"]["largest"] == [
        {"paddr": CUBE_FACES + 9 * PAGE, "pages": 5}, {"paddr": FCRAM + 8 * PAGE, "pages": 1},
        {"paddr": FCRAM + 14 * PAGE, "pages": 1}, {"paddr": ETC + PAGE, "pages": 1}]
    assert rc["counts"]["draws"] == 2 and rc["counts"]["texture_reads"] == 14    # six faces and unit 1, twice
    assert rc["changes"]["not_read_never_in_read_range"] == 4
    assert coh.cube_face_size(16, 0, 2) == (16 * 16 + 8 * 8 + 4 * 4) * 4 and coh.texture_size(16, 16, 0, 2) == 1280
    assert coh.texture_size(16, 16, 7, 1) == 320 and coh.texture_size(8, 8, 0, 0) == 256
    assert coh.texture_size(8, 8, 1, 0) == 192 and coh.texture_size(1024, 8, 13, 0) == 8192   # RGB8; ETC1A4
    assert coh.mip_levels(8, 8, 3) == 1 and coh.mip_levels(64, 32, 15) == 3 and coh.mip_levels(64, 64, 0) == 1
    assert coh.texture_size(8, 8, 12, 0) == 32 and coh.texture_size(8, 8, 15, 0) == 256   # ETC1; Invalid: 32 bits


def test_vertex_spans_come_from_the_index_data_or_from_offset_and_count(tmp_path):
    base = FCRAM + 2 * PAGE
    regs = [(coh.REG_ATTR_BASE, 0xF, (base >> 4) << 1),
            (coh.REG_LOADER, 0xF, 0), (coh.REG_LOADER + 2, 0xF, (1 << 28) | (8 << 16)),   # loader 0: 8 bytes a vertex
            (coh.REG_LOADER + 3, 0xF, 0x800), (coh.REG_LOADER + 5, 0xF, 4 << 16),         # loader 1: no components
            (coh.REG_VERTEX_OFFSET, 0xF, 2), (coh.REG_NUM_VERTICES, 0xF, 3),
            (coh.REG_DRAW, 0xF, 1),                                        # vertices 2 to 4: bytes 16 to 39
            (coh.REG_INDEX, 0xF, (1 << 31) | PAGE),                        # 16-bit indices at base + 0x1000 (page 3)
            (coh.REG_DRAW_INDEXED, 0xF, 1)]                                # indices 9, 6, 7: bytes 48 to 79

    def build(w):
        w.fill(0)
        outside = {**{b: 0xEE for b in range(8)}, 40: 0xEE, 80: 0xEE}    # all of vertex 0; vertices 5 and 10
        inside = {b: 0xEE for b in (16, 39, 48, 79)}
        cpu_change(w, {base: page(0x12, {**outside, **inside, 0x808: 0xEE}),   # 0x808: loader 1's vertex 2
                       base + PAGE: page(0x13, dict(enumerate(struct.pack("<3H", 9, 6, 7))))})
        w.submit_cmdlist(LINEAR, 0x10)
        w.pica_reg(regs)
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "vertex") == (0, 0, 4)                           # bytes 16, 39, 48 and 79
    assert classes(rc, "index") == (0, 0, 6)                            # the three indices
    assert classes(rc, "total") == (0, 0, 10) and classes(rc, "total", "pairs") == (0, 0, 2)
    assert (rc["counts"]["draws"], rc["counts"]["draws_indexed"]) == (1, 1)
    # vertex 0, vertices 5 and 10, and byte 0x808, which loader 1 would read if it had components
    assert rc["changes"]["not_read_never_in_read_range"] == 8 + 2 + 1


def test_an_indexed_draw_with_8_bit_indices(tmp_path):
    base = FCRAM + 4 * PAGE
    regs = [(coh.REG_ATTR_BASE, 0xF, (base >> 4) << 1),
            (coh.REG_LOADER + 2, 0xF, (1 << 28) | (8 << 16)), (coh.REG_NUM_VERTICES, 0xF, 3),
            (coh.REG_INDEX, 0xF, PAGE),                                    # bit 31 clear: 8-bit indices at page 5
            (coh.REG_DRAW_INDEXED, 0xF, 1)]                                # indices 3, 1, 2: bytes 8 to 31

    def build(w):
        w.fill(0)
        cpu_change(w, {base: page(0x14, {7: 0xEE, 8: 0xEE, 31: 0xEE, 32: 0xEE}),
                       base + PAGE: page(0x15, {0: 3, 1: 1, 2: 2, 3: 0xEE})})     # byte 3: past the 3 indices
        w.submit_cmdlist(LINEAR, 0x10)
        w.pica_reg(regs)
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "index") == (0, 0, 3) and classes(rc, "vertex") == (0, 0, 2)
    assert rc["changes"]["not_read_never_in_read_range"] == 3          # vertex bytes 7 and 32, index byte 3


def test_a_command_buffer_jump_reads_the_chained_buffer(tmp_path):
    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 5 * PAGE: page(0x15, {0x3F: 1, 0x40: 1}), FCRAM + 6 * PAGE: page(0x16, {0x1F: 1}),
                       FCRAM + 8 * PAGE: page(0x18, {0x1F: 1, 0x20: 1})})
        w.submit_cmdlist(LINEAR + 4 * PAGE, 0x20)                       # event 1 jumps to 0x40 bytes of page 5
        w.pica_reg([(coh.REG_CMDBUF_SIZE, 0xF, 0x40 >> 3), (coh.REG_CMDBUF_ADDR, 0xF, (FCRAM + 5 * PAGE) >> 3),
                    (coh.REG_CMDBUF_JUMP, 0xF, 1)])
        w.submit_cmdlist(LINEAR + 6 * PAGE, 0x20)                       # event 2: channel 0 is its own buffer now
        w.pica_reg([(coh.REG_CMDBUF_JUMP, 0xF, 1)])
        w.submit_cmdlist(LINEAR + 7 * PAGE, 0x20)                       # event 3 jumps through channel 1 to page 8
        w.pica_reg([(coh.REG_CMDBUF_SIZE + 1, 0xF, 0x20 >> 3),
                    (coh.REG_CMDBUF_ADDR + 1, 0xF, (FCRAM + 8 * PAGE) >> 3),
                    (coh.REG_CMDBUF_JUMP + 1, 0, 0)])                   # a trigger jumps whatever its byte mask
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "chained_list") == (0, 0, 3)                     # byte 0x3F of page 5, 0x1F of pages 6 and 8
    assert classes(rc, "cmdlist") == (0, 0, 1)                          # page 6 byte 0x1F, as the list itself
    assert classes(rc, "total") == (0, 0, 3)                            # read by both at once: counted once
    assert rc["counts"]["chained_list_jumps"] == 3
    assert rc["changes"]["not_read_never_in_read_range"] == 2          # page 5 byte 0x40, page 8 byte 0x20


def test_the_register_replay_starts_from_the_init_block_and_honours_byte_masks(tmp_path):
    init = {coh.REG_TEX_CONFIG: 1, 0x82: (8 << 16) | 8, 0x85: (FCRAM + 3 * PAGE) >> 3,   # unit 0: 8x8 RGBA8, page 3
            coh.REG_NUM_VERTICES: 3}

    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 3 * PAGE: page(0x13, {0: 1}), FCRAM + 4 * PAGE: page(0x14, {0: 1})})
        w.submit_cmdlist(LINEAR, 0x10)
        w.pica_reg([(coh.REG_TEX_CONFIG, 0x4, 0),                       # byte 2 only: unit 0 stays enabled
                    (0x85, 0x3, ((FCRAM + 4 * PAGE) >> 3) & 0xFFFF),    # bytes 0 and 1 only: the address is page 4
                    (coh.REG_DRAW, 0xF, 1)])
    rc = report_of(tmp_path, build, init_blocks={gcap.K_PICA_REGS: gcap.pica_regs_block(init)})["read_coverage"]
    assert classes(rc, "texture") == (0, 0, 1)
    assert rc["reads"]["texture"]["uncovered"]["largest"] == [{"paddr": FCRAM + 4 * PAGE, "pages": 1}]
    assert rc["changes"]["not_read_never_in_read_range"] == 1          # page 3: the address before the write


def test_named_reads_outside_the_windows_are_counted_and_mapped_as_azahar_does(tmp_path):
    heap = 0x08000000                                                   # not a window of the GPU

    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 3 * PAGE: page(0x13, {0: 1}), FCRAM + 10 * PAGE: page(0x1A, {0xFF0: 1}),
                       FCRAM + 11 * PAGE: page(0x1B, {0: 1}), FCRAM + 12 * PAGE: page(0x1C, {0: 1})})
        w.gx(dma(heap + PAGE - 0x10, 0x20))                             # event 1: across heap pages 0 and 1, in the run
        w.gx(dma(heap + 2 * PAGE - 0x10, 0x20))                         # event 2: half of it past the run, which ends
                                                                        # just before FCRAM page 12
        w.submit_cmdlist(FCRAM + 3 * PAGE, 0x10)                        # event 3: a physical address, passed through
        w.pica_reg([])
        w.gx([4, heap, LINEAR + 12 * PAGE, 0x20, 0, 0, 0, 0])           # event 4: passed through to no memory
    runs = [(FCRAM + 10 * PAGE, 2, heap)]                               # heap pages 0 and 1 are FCRAM pages 10 and 11
    rc = report_of(tmp_path, build, runs)["read_coverage"]
    o = rc["named_outside_windows"]
    assert o["dma_source"] == {"reads": 2, "bytes": 0x40, "bytes_modelled": 0x30}
    assert o["cmdlist"] == {"reads": 1, "bytes": 0x10, "bytes_modelled": 0x10}
    assert o["transfer_input"] == {"reads": 1, "bytes": 0x20, "bytes_modelled": 0}
    assert classes(rc, "dma_source") == (0, 0, 2) and classes(rc, "cmdlist") == (0, 0, 1)
    assert rc["changes"]["not_read_never_in_read_range"] == 1          # FCRAM page 12
    twice = report_of(tmp_path, build, runs + [(FCRAM + 10 * PAGE, 1, heap)])["read_coverage"]   # overlapping runs
    assert twice["named_outside_windows"]["dma_source"]["bytes_modelled"] == 0x30
    assert coh.passed_through_range(VRAM + coh.VRAM_SIZE - 0x10, 0x20) == (VRAM + coh.VRAM_SIZE - 0x10,
                                                                           VRAM + coh.VRAM_SIZE)   # clipped to VRAM
    assert coh.passed_through_range(heap, 0x10) is None
    plain = report_of(tmp_path, build)["read_coverage"]                 # no alias map: the DMA is not modelled
    assert plain["named_outside_windows"]["dma_source"]["bytes_modelled"] == 0
    assert classes(plain, "dma_source") == (0, 0, 0) and plain["changes"]["not_read_never_in_read_range"] == 3
    md = str(tmp_path / "o.md")
    assert coh.main(["capture", str(tmp_path / "reads.mgpu"), "--md", md]) == 0
    assert "dma_source 2 (0 of 64 bytes modelled)" in open(md).read()


def test_a_flush_by_an_address_of_an_alias_run_covers_the_memory_behind_it(tmp_path):
    heap = 0x08000000
    runs = [(FCRAM + 10 * PAGE, 2, heap), (FCRAM + 13 * PAGE, 1, heap + 3 * PAGE)]   # heap pages 0, 1 and 3

    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + p * PAGE: page(0x10 + p, {0: 1}) for p in (9, 10, 11, 12, 13)})   # interval 1
        w.swap(0, [0] * 7)                                              # event 1
        w.flush(1, heap - PAGE, 2 * PAGE)                               # interval 2: from the page below the run: page 10
        w.flush(1, LINEAR + 14 * PAGE, PAGE)                            # a flush by a window: not counted
        cache_flush(w, heap + PAGE, 3 * PAGE)                           # event 2: pages 11 and 13, through two runs
                                                                        # and the gap between them
        w.gx(dma(heap, 4 * PAGE))                                       # event 3 reads pages 10, 11 and 13
        w.gx(dma(LINEAR + 9 * PAGE, 0x10))                              # event 4: page 9, below the run: not flushed
        w.gx(dma(LINEAR + 12 * PAGE, 0x10))                             # event 5: page 12, which no run holds
    rep = report_of(tmp_path, build, runs)
    rc = rep["read_coverage"]
    assert classes(rc, "dma_source") == (3, 0, 2)
    assert (rc["counts"]["flushes_through_alias_runs"], rc["counts"]["flushes_with_two_mappings"]) == (2, 0)  # once each
    regions = rc["regions"]
    assert (regions["FCRAM alias 0x08000000"]["flushed_by_end"], regions["FCRAM alias 0x08003000"]["flushed_by_end"],
            regions["FCRAM linear"]["flushed_by_end"]) == (2, 1, 0)
    # the rows map a flush through the windows only
    assert rep["flushes"]["FlushDataCache"]["maps_nowhere"] == 1 and rep["flushes"]["CacheFlushRegion"]["maps_nowhere"] == 1
    log = tmp_path / "alias.log"
    log.write_text("# coherence v1 2026-11-01 game=0x0004000000126300 cpu=recomp new3ds=off\n"
                   "ALIAS f=0 vaddr=0x08000000 paddr=0x2000a000 pages=2\n"
                   "ALIAS f=0 vaddr=0x08003000 paddr=0x2000d000 pages=1\n")
    md = str(tmp_path / "a.md")
    assert coh.main(["capture", str(tmp_path / "reads.mgpu"), "--alias-log", str(log), "--md", md]) == 0
    text = open(md).read()
    assert "2 flushes covered memory this way" in text and "0 flushes named such an address" in text
    plain = report_of(tmp_path, build)["read_coverage"]                 # no alias map: neither flush nor DMA mapped
    assert classes(plain, "dma_source") == (0, 0, 2) and plain["counts"]["flushes_through_alias_runs"] == 0


def test_alias_flushes_keep_the_order_rules_and_an_address_no_window_or_run_holds_covers_nothing(tmp_path):
    heap = 0x08000000

    def build(w):
        w.fill(0)                                                       # event 0
        w.flush(1, heap, PAGE)                                          # interval 1: page 10 through the run, and a
        w.flush(1, FCRAM + 13 * PAGE, PAGE)                             # physical address, neither a window nor a run
        cpu_change(w, {FCRAM + 10 * PAGE: page(0x1A, {0: 1}), FCRAM + 13 * PAGE: page(0x1D, {0: 1})})
        cache_flush(w, heap + PAGE, PAGE)                               # event 1: page 11, at the command's position
        cpu_change(w, {FCRAM + 10 * PAGE: page(0x1A, {0: 1, 1: 1}),    # interval 2: after both flushes
                       FCRAM + 11 * PAGE: page(0x1B, {0: 1})})
        w.gx(dma(heap, 2 * PAGE))                                       # event 2 reads pages 10 and 11
        w.gx(dma(LINEAR + 13 * PAGE, 0x10))                             # event 3 reads page 13
    rc = report_of(tmp_path, build, [(FCRAM + 10 * PAGE, 2, heap)])["read_coverage"]
    assert classes(rc, "dma_source") == (0, 1, 3)                       # page 10 byte 0: in the same interval only
    assert rc["counts"]["flushes_through_alias_runs"] == 2
    # neither flush covers a change of a later interval, and the physical address covers nothing
    assert rc["regions"]["FCRAM alias 0x08000000"]["flushed_by_end"] == 1
    assert rc["regions"]["FCRAM linear"]["flushed_by_end"] == 0


def test_a_flush_covers_no_address_two_alias_runs_map_to_different_memory(tmp_path):
    heap = 0x08000000

    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + p * PAGE: page(0x10 + p, {0: 1}) for p in (5, 10, 11)})   # interval 1
        w.swap(0, [0] * 7)                                              # event 1
        w.flush(1, heap, 2 * PAGE)                                      # interval 2: two places, then one
        w.flush(1, heap, PAGE)                                          # two places only: covers nothing
        w.gx(dma(heap, 0x10))                                           # event 2: a DMA source reads from both places
        w.gx(dma(LINEAR + 11 * PAGE, 0x10))                             # event 3 reads page 11 by the window
    # heap page 0 is FCRAM page 10 in one ALIAS set and page 5 in another; heap page 1 is page 11 in both
    rc = report_of(tmp_path, build, [(FCRAM + 10 * PAGE, 2, heap), (FCRAM + 5 * PAGE, 1, heap)])["read_coverage"]
    assert classes(rc, "dma_source") == (1, 0, 2)                       # pages 10 and 5 read, only page 11 flushed
    assert (rc["counts"]["flushes_through_alias_runs"], rc["counts"]["flushes_with_two_mappings"]) == (1, 2)
    once = report_of(tmp_path, build, [(FCRAM + 10 * PAGE, 2, heap), (FCRAM + 10 * PAGE, 1, heap)])["read_coverage"]
    assert classes(once, "dma_source") == (2, 0, 0) and once["counts"]["flushes_with_two_mappings"] == 0   # one place
    assert coh._one_place([(0, 8), (4, 12)], [(100, 108), (204, 212)]) == ([(100, 104), (208, 212)], True)


def test_a_shadow_cube_reads_six_faces_and_address_0_or_no_vertices_read_nothing(tmp_path):
    regs = {coh.REG_TEX_CONFIG: 0b011,                                  # unit 0 a shadow cube; unit 1 at address 0
            0x82: (8 << 16) | 8, 0x83: coh.TEX_SHADOW_CUBE << 28, 0x85: (FCRAM + 8 * PAGE) >> 3,
            0x92: (8 << 16) | 8, 0x95: 0,
            coh.REG_NUM_VERTICES: 3}
    for face in range(5):
        regs[coh.REG_CUBE_ADDR + face] = ((CUBE_FACES + (9 + face) * PAGE) >> 3) & 0x3FFFFF
    empty = [(coh.REG_ATTR_BASE, 0xF, (FCRAM >> 4) << 1), (coh.REG_INDEX, 0xF, (1 << 31) | 20 * PAGE),
             (coh.REG_NUM_VERTICES, 0xF, 0), (coh.REG_DRAW_INDEXED, 0xF, 1)]   # an indexed draw of no vertices

    def build(w):
        w.fill(0)
        changes = {CUBE_FACES + p * PAGE: page(0, {0: 0xEE}) for p in range(9, 14)}
        changes[FCRAM + 8 * PAGE] = page(0x18, {0: 0xEE})
        changes[FCRAM + 20 * PAGE] = page(0, {0: 0xEE})                  # the index data of the empty draw
        cpu_change(w, changes)
        w.submit_cmdlist(LINEAR, 0x10)
        w.pica_reg([(reg, 0xF, value) for reg, value in regs.items()] + [(coh.REG_DRAW, 0xF, 1)] + empty)
    rc = report_of(tmp_path, build)["read_coverage"]
    assert classes(rc, "texture") == (0, 0, 6) and rc["counts"]["texture_reads"] == 6
    assert classes(rc, "index") == (0, 0, 0) and classes(rc, "vertex") == (0, 0, 0)
    assert (rc["counts"]["draws"], rc["counts"]["draws_indexed"]) == (1, 1)
    assert rc["changes"]["not_read_never_in_read_range"] == 1          # the index data


ALIAS_LOG = """\
# coherence v1 2026-11-01 game=0x0004000000126300 cpu=recomp new3ds=off
ALIAS f=0 runs=1
ALIAS f=0 vaddr=0x08000000 paddr=0x2000a000 pages=1
ALIAS f=600 runs=2
ALIAS f=600 vaddr=0x08000000 paddr=0x2000a000 pages=2
ALIAS f=600 vaddr=0x0ffc0000 paddr=0x2000c000 pages=1
"""


def test_an_alias_log_splits_fcram_into_regions(tmp_path):
    def build(w):
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 1}), FCRAM + 10 * PAGE: page(0x1A, {0: 1, 1: 1}),
                       FCRAM + 11 * PAGE: page(0x1B, {0: 1}), FCRAM + 12 * PAGE: page(0x1C, {0: 1}),
                       VRAM: page(0x80, {0: 1})})
        w.fill(0)
    runs = coh.load_alias_runs(ALIAS_LOG.splitlines(True))
    # the union of the two sets: the run at 0x08000000 grew from 1 page to 2 and is kept once
    assert runs == [(0x2000A000, 2, 0x08000000), (0x2000C000, 1, 0x0FFC0000)]
    rc = report_of(tmp_path, build, runs)["read_coverage"]
    assert rc["alias_map"] == {"given": True, "runs": 2}
    regions = rc["regions"]
    assert list(regions) == ["VRAM", "FCRAM linear", "FCRAM alias 0x08000000", "FCRAM alias 0x0ffc0000"]
    assert (regions["FCRAM alias 0x08000000"]["changed_bytes"], regions["FCRAM alias 0x08000000"]["pages"]) == (3, 2)
    assert regions["FCRAM alias 0x0ffc0000"]["changed_bytes"] == 1 and regions["FCRAM linear"]["changed_bytes"] == 1
    assert rc["changes"]["changed_bytes"] == 6 and rc["changes"]["pages"] == 5
    plain = report_of(tmp_path, build)["read_coverage"]                 # no alias map: FCRAM is one region
    assert plain["alias_map"] == {"given": False, "runs": 0}
    assert list(plain["regions"]) == ["VRAM", "FCRAM"] and plain["regions"]["FCRAM"]["changed_bytes"] == 5
    path = str(tmp_path / "t.md")
    assert coh.main(["capture", str(tmp_path / "reads.mgpu"), "--md", path]) == 0
    assert "No alias map was given" in open(path).read()
    shared = [(FCRAM + 12 * PAGE, 1, 0x08000000), (FCRAM + 12 * PAGE, 1, 0x0FF00000)]   # one page in two runs
    assert list(report_of(tmp_path, build, shared)["read_coverage"]["regions"]) == [
        "VRAM", "FCRAM linear", "FCRAM alias 0x08000000"]                # the first run names it
    empty = coh.load_alias_runs(ALIAS_LOG.splitlines(True)[:1])         # a log with no ALIAS run: no alias map
    assert empty == [] and report_of(tmp_path, build, empty)["read_coverage"]["alias_map"] == {"given": False,
                                                                                               "runs": 0}


def test_changes_no_read_took_are_split_and_every_changed_byte_is_accounted_for(tmp_path):
    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 4 * PAGE: page(0x14, {0: 1}), FCRAM + 9 * PAGE: page(0x19, {0: 1, 1: 1}),
                       FCRAM + 10 * PAGE: page(0x1A, {0: 1}), FCRAM + 11 * PAGE: page(0x1B, {0: 1})})   # interval 1
        w.gx(dma(LINEAR + 4 * PAGE, 0x10))                              # event 1 reads page 4
        w.flush(1, LINEAR + 9 * PAGE, 1)                                # interval 2: byte 0 of page 9
        cpu_change(w, {FCRAM + 4 * PAGE: page(0x14, {0: 2}),            # after its read: for a read after the capture
                       FCRAM + 10 * PAGE: page(0x1A, {0: 2})})          # replaces the change no read took
        w.submit_cmdlist(LINEAR, 0x10)                                  # event 2: the GPU writes page 11 byte 0
        w.pica_reg([])
        w.gpu_pages([(FCRAM + 11 * PAGE, page(0x1B, {0: 0x55}))])
    rc = report_of(tmp_path, build)["read_coverage"]
    assert list(rc["regions"]) == ["VRAM", "FCRAM"] and rc["regions"]["VRAM"]["changed_bytes"] == 0   # listed anyway
    t = rc["changes"]
    assert t["changed_bytes"] == 7 and t["page_changes"] == 6
    assert (t["read_by_gpu"], t["replaced"], t["overwritten_by_gpu"]) == (1, 1, 1)
    assert (t["not_read_in_read_range"], t["not_read_in_read_range_flushed"]) == (1, 0)        # (i): page 4
    assert (t["not_read_never_in_read_range"], t["not_read_never_in_read_range_flushed"]) == (3, 1)   # (ii)
    assert t["changed_bytes"] == sum(t[k] for k in ("read_by_gpu", "replaced", "overwritten_by_gpu",
                                                    "not_read_in_read_range", "not_read_never_in_read_range"))
    assert t["flushed_by_end"] == 1


def test_do_flush_is_counted_not_credited_and_cache_flushes_are_matched_with_the_next_list(tmp_path):
    def build(w):
        w.fill(0)
        cpu_change(w, {FCRAM + 2 * PAGE: page(0x12, {0: 1})})
        w.submit_cmdlist(LINEAR + 2 * PAGE, 0x20, do_flush=1)          # event 1: do_flush is not a flush
        w.pica_reg([])
        cache_flush(w, LINEAR + 3 * PAGE, 0x40)                         # exactly the next list's buffer
        w.submit_cmdlist(LINEAR + 3 * PAGE, 0x40)
        w.pica_reg([])
        cache_flush(w, LINEAR + 4 * PAGE, 0x40)                         # not the next list's buffer
        w.submit_cmdlist(LINEAR + 5 * PAGE, 0x40)
        w.pica_reg([])
        cache_flush(w, LINEAR + 6 * PAGE, 0x20)                         # its start, but not its size
        w.submit_cmdlist(LINEAR + 6 * PAGE, 0x40)
        w.pica_reg([])
        w.submit_cmdlist(LINEAR + 4 * PAGE, 0x40)                       # an earlier flush's buffer, but not the next list
        w.pica_reg([])
        cache_flush(w, LINEAR + 8 * PAGE, 0x40)                         # another buffer, the next list's, another one:
        cache_flush(w, LINEAR + 7 * PAGE, 0x40)                         # every CacheFlush since the last list is matched
        cache_flush(w, LINEAR + 11 * PAGE, 0x40)
        w.submit_cmdlist(LINEAR + 7 * PAGE, 0x40)
        w.pica_reg([])
        cache_flush(w, LINEAR + 9 * PAGE, 0x40)
        w.swap(0, [0] * 7)                                              # an event in between changes nothing
        w.submit_cmdlist(LINEAR + 9 * PAGE, 0x40)
        w.pica_reg([])
        w.gx([5, LINEAR + 10 * PAGE, 0x40, LINEAR + 12 * PAGE, 0x40, 0, 0, 0])   # the buffer and one more region:
        w.flush(4, LINEAR + 10 * PAGE, 0x40)                            # not exactly the buffer
        w.flush(4, LINEAR + 12 * PAGE, 0x40)
        w.submit_cmdlist(LINEAR + 10 * PAGE, 0x40)
        w.pica_reg([])
    rc = report_of(tmp_path, build)["read_coverage"]
    c = rc["counts"]
    assert (c["submit_cmdlist"], c["submit_cmdlist_do_flush"]) == (8, 1)
    assert (c["cache_flush"], c["cache_flush_equals_next_buffer"]) == (8, 3)
    assert classes(rc, "cmdlist") == (0, 0, 1)


def test_the_changes_of_the_first_interval_count_like_the_others_and_are_shown_apart(tmp_path):
    def build(w):
        w.flush(1, LINEAR + PAGE, PAGE)                                 # interval 0, before the first event
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 1, 1: 1}), FCRAM + 2 * PAGE: page(0x12, {0: 1})})
        w.gx(dma(LINEAR + PAGE, 2 * PAGE))                              # event 0 reads pages 1 and 2
    rep = report_of(tmp_path, build)
    rc = rep["read_coverage"]
    assert classes(rc, "dma_source") == (0, 2, 1)
    assert rc["reads"]["dma_source"]["not_covered_first_interval_bytes"] == 1
    assert rc["reads"]["total"]["not_covered_first_interval_bytes"] == 1
    assert rep["rows"][coh.FIRST_ROW]["changed_bytes"] == 3


def test_the_read_based_report_on_the_command_line(basic_report, tmp_path, capsys):
    _, path = basic_report
    log = tmp_path / "alias.log"
    log.write_text("# coherence v1 2026-11-01 game=0x0004000000126300 cpu=recomp new3ds=off\n"
                   "ALIAS f=0 vaddr=0x08000000 paddr=0x20006000 pages=1\n")
    out_json, out_md = str(tmp_path / "r.json"), str(tmp_path / "r.md")
    assert coh.main(["capture", path, "--alias-log", str(log), "--json", out_json, "--md", out_md, "--top", "3"]) == 0
    data = json.load(open(out_json))[path]
    assert data["events"] == 8 and "rows" in data and "not_fully_covered" in data        # the old keys are kept
    rc = data["read_coverage"]
    assert rc["alias_map"] == {"given": True, "runs": 1}
    # the lists of events 1 and 8 were flushed in their own interval; the display transfer reads 4 changed bytes
    assert rc["reads"]["cmdlist"]["bytes"] == {"certain": 0, "same_interval": 5, "not_covered": 0}
    assert rc["reads"]["transfer_input"]["bytes"] == {"certain": 0, "same_interval": 0, "not_covered": 4}
    assert rc["regions"]["FCRAM alias 0x08000000"]["changed_bytes"] == 1                  # page 6
    text = open(out_md).read()
    assert "### Same-interval rule" in text and "### Read-based coverage" in text
    assert "| transfer_input | 0 / 0 / 4 |" in text and "FCRAM alias 0x08000000" in text
    assert "pages with read bytes not covered (total): 1 pages in 1 ranges" in text
    assert "outside the GPU's windows: cmdlist 0 (0 of 0 bytes modelled)" in text and "Not modelled:" in text
    assert "| changes still pending at the end" in text and "| VRAM |" in text
    bad = tmp_path / "bad.log"
    bad.write_text("# perf v1\n")
    assert coh.main(["capture", path, "--alias-log", str(bad)]) == 1
    assert "not a coherence log" in capsys.readouterr().err


# ── (b) the runner's log ──────────────────────────────────────────────────────────────

LOG = """\
# coherence v1 2026-10-30 game=0x0004000000126300 cpu=recomp new3ds=off
ALIAS f=10 runs=1
ALIAS f=10 vaddr=0x08000000 paddr=0x2000b000 pages=1
ACCESS n=1 f=11 t=2000 side=cpu kind=Read paddr=0x20004008 vaddr=0x14004008 size=4 guest=1 lr=0x00100020 in=- mark=1 mark_ev=SubmitCmdList mark_f=10 mark_t=1000 watched=1 aliased=0
ACCESS n=2 f=11 t=2100 side=cpu kind=Read paddr=0x20005000 vaddr=0x14005000 size=1 guest=1 lr=0x00100020 in=- mark=1 mark_ev=SubmitCmdList mark_f=10 mark_t=1000 watched=1 aliased=0
ACCESS n=3 f=12 t=3000 side=cpu kind=BlockRead paddr=0x20006000 vaddr=0x14006000 size=256 guest=1 lr=0x00200010 in=- mark=2 mark_ev=DisplayTransfer mark_f=11 mark_t=2500 watched=1 aliased=0
ACCESS n=4 f=12 t=3100 side=cpu kind=Write paddr=0x20007000 vaddr=0x14007000 size=4 guest=1 lr=0x00300000 in=- mark=2 mark_ev=DisplayTransfer mark_f=11 mark_t=2500 watched=1 aliased=0
ACCESS n=5 f=12 t=3200 side=cpu kind=Pointer paddr=0x20008000 vaddr=0x14008000 size=0 guest=0 lr=- in=- mark=3 mark_ev=MemoryFill mark_f=12 mark_t=3000 watched=1 aliased=0
ACCESS n=6 f=13 t=4000 side=gpu kind=CopySource paddr=0x18000000 vaddr=0x1f000000 size=4096 guest=1 lr=0x00400000 in=RequestDma mark=4 mark_ev=SubmitCmdList mark_f=12 mark_t=3500 watched=1 aliased=0
EVENT ev=SubmitCmdList n=2 gpu_pages=7 marked=5 rewritten=1 unwatched=1 aliased=0
EVENT ev=DisplayTransfer n=1 gpu_pages=3 marked=3 rewritten=0 unwatched=0 aliased=1
UNWATCHED kind=nowindow paddr=0x2000a000 pages=1 marks=1
UNWATCHED kind=aliased paddr=0x2000b000 pages=1 marks=1
PENDING pages=5
SUMMARY first_frame=10 last_frame=13 events=3 gpu_pages=10 marked=8 rewritten=1 unwatched=1 aliased=1 accesses=6 cpu=5 gpu=1 pending=5 cpu_pages_changed=4 foreign=2 no_window=1 outside_run=7 other_thread=0
"""


def test_the_log_is_grouped_by_side_kind_event_and_call_site():
    rep = coh.analyse_log(LOG.splitlines(True))
    assert rep["complete"] and rep["accesses"] == 6
    assert (rep["cpu_loads"], rep["cpu_stores"], rep["cpu_pointers"], rep["gpu_side"]) == (3, 1, 1, 1)
    assert rep["by_side_kind"]["cpu/Read"] == 2 and rep["by_side_kind"]["gpu/CopySource"] == 1
    assert rep["by_writing_event"]["cpu/load"] == {"SubmitCmdList": 2, "DisplayTransfer": 1}
    top = rep["cpu_load_sites"][0]
    assert top["lr"] == "0x00100020" and top["count"] == 2 and top["bytes"] == 5 and top["kinds"] == {"Read": 2}
    assert [s["lr"] for s in rep["cpu_load_sites"]] == ["0x00100020", "0x00200010"]
    assert rep["cpu_store_sites"][0]["lr"] == "0x00300000"
    assert rep["cpu_pointer_sites"][0]["lr"] == "-"                  # no guest thread was running
    assert rep["load_delay_frames"] == {"min": 1, "median": 1, "p99": 1, "max": 1}
    assert rep["load_delay_ticks"]["min"] == 500 and rep["load_delay_ticks"]["max"] == 1100
    assert rep["load_sizes"] == {"1": 1, "4": 1, "256": 1}
    assert [(u["kind"], u["paddr"], u["pages"]) for u in rep["unwatched"]] == [("nowindow", 0x2000A000, 1),
                                                                               ("aliased", 0x2000B000, 1)]
    assert rep["aliases"] == [{"f": 10, "vaddr": 0x08000000, "paddr": 0x2000B000, "pages": 1}]
    assert rep["summary"]["outside_run"] == 7 and rep["events"]["SubmitCmdList"]["rewritten"] == 1


def test_a_log_without_a_summary_is_reported_as_partial(tmp_path):
    cut = "".join(LOG.splitlines(True)[:6])
    rep = coh.analyse_log(cut.splitlines(True))
    assert not rep["complete"] and rep["accesses"] == 3
    path = tmp_path / "cut.log"
    path.write_text(cut)
    md = tmp_path / "cut.md"
    assert coh.main(["runlog", str(path), "--md", str(md)]) == 0
    assert "no SUMMARY line" in md.read_text()


def test_a_log_with_a_foreign_header_is_refused(tmp_path, capsys):
    path = tmp_path / "x.log"
    path.write_text("# perf v1 2026-10-30\nSUMMARY events=1\n")
    assert coh.main(["runlog", str(path)]) == 1
    assert "not a coherence log" in capsys.readouterr().err
    empty = tmp_path / "empty.log"
    empty.write_text("")
    assert coh.main(["runlog", str(empty)]) == 1


def test_the_log_report_and_its_json(tmp_path, capsys):
    path = tmp_path / "ok.log"
    path.write_text(LOG)
    out_json = str(tmp_path / "l.json")
    assert coh.main(["runlog", str(path), "--json", out_json]) == 0
    assert json.load(open(out_json))[str(path)]["cpu_loads"] == 3
    assert coh.main(["runlog", str(path)]) == 0
    text = capsys.readouterr().out
    assert "0x00100020" in text and "CPU loads 3" in text and "nowindow" in text and "aliased" in text


# ── the checking logic of the runner ──────────────────────────────────────────────────

def region_text() -> str:
    text = open(RUNNER, encoding="utf-8").read()
    capture = text[text.index("// BEGIN gpu-capture-core"):text.index("// END gpu-capture-core")]
    check = text[text.index("// BEGIN coherence-core"):text.index("// END coherence-core")]
    return capture + "\n" + check


DRIVER = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#include "region.inc"

// The marker of Azahar: marking a marked page or unmarking an unmarked one is UNREACHABLE().
struct FakeHost : CoherenceHost {
    u32 frame = 100;
    u64 ticks = 1000;
    bool guest = true;
    std::set<u32> marked;     // physical pages Azahar has marked
    std::set<u32> nowindow;   // pages no virtual window of the process reaches
    u32 mark_calls = 0, mark_pages = 0;
    u32 Frame() override { return frame; }
    u64 Ticks() override { return ticks += 10; }
    bool Watched(u32 paddr) override { return nowindow.count(paddr) == 0; }
    bool GuestRunning() override { return guest; }
    void Mark(u32 paddr, u32 pages, bool cached) override {
        ++mark_calls;
        mark_pages += pages;
        for (u32 i = 0; i < pages; ++i) {
            const u32 p = paddr + i * 4096;
            if (cached && !marked.insert(p).second) { std::fprintf(stderr, "UNREACHABLE: page 0x%08x marked twice\n", p); std::exit(99); }
            if (!cached && marked.erase(p) == 0) { std::fprintf(stderr, "UNREACHABLE: page 0x%08x unmarked but not marked\n", p); std::exit(98); }
        }
    }
};

enum { Read, Write, WriteExclusive, BlockRead, BlockWrite, ZeroBlock, CopySource, Pointer };
static const u32 kLinear = 0x14000000, kVramV = 0x1F000000, kFcram = 0x20000000;

int main(int, char** argv) {
    const std::string scenario = argv[2];
    FakeHost host;
    std::vector<u8> fcram(16 * 4096), vram(4 * 4096);
    for (u32 i = 0; i < 16; ++i) std::fill_n(fcram.begin() + i * 4096, 4096, static_cast<u8>(0x10 + i));
    for (u32 i = 0; i < 4; ++i) std::fill_n(vram.begin() + i * 4096, 4096, static_cast<u8>(0x80 + i));
    std::vector<GpuMemoryRegion> regions(2);
    regions[0].paddr = kFcram; regions[0].size = fcram.size(); regions[0].live = fcram.data();
    regions[1].paddr = 0x18000000; regions[1].size = vram.size(); regions[1].live = vram.data();
    CoherenceChecker c(host);
    if (!c.Open(argv[1], "coherence v1 2026-10-30 game=0x0004000000126300 cpu=recomp new3ds=off")) return 2;
    const u32 submit[8] = {1, kLinear, 0x100, 0, 0, 0, 0, 0};
    const u32 transfer[8] = {3, kVramV, kLinear, 0x100, 0x100, 0, 0, 0};
    const u32 dma[8] = {0, kLinear, kLinear + 0x10000, 0x1000, 0, 0, 0, 0};

    // before Start nothing is watched or counted
    c.ExecuteBegin(submit); c.ExecuteEnd();
    c.Access(kLinear, 4, Read, 1);
    c.Start(regions);
    c.SetAliases({{0x08000000, kFcram + 11 * 4096, 1}});
    host.nowindow.insert(kFcram + 10 * 4096);

    if (scenario == "basic") {
        // event 1: a command list; the GPU writes FCRAM pages 3 to 5 and 13 and VRAM page 0
        fcram[2 * 4096] = 0xEE;                                     // the CPU's own change: never marked
        c.ExecuteBegin(submit);
        for (u32 p : {3u, 4u, 5u, 13u}) fcram[p * 4096 + 1] = 0xBB;
        vram[3] = 0xCC;
        c.ExecuteEnd();
        std::printf("marked_after_1 %zu calls %u\n", host.marked.size(), host.mark_calls);
        // the CPU reads page 4 (guest running), writes page 3, reads page 4 again, touches page 9 (never marked)
        host.frame = 101;
        c.Access(kLinear + 4 * 4096 + 8, 4, Read, 0x00100020);
        c.Access(kLinear + 3 * 4096, 4, Write, 0x00100024);
        c.Access(kLinear + 4 * 4096 + 8, 4, Read, 0x00100020);
        c.Access(kLinear + 9 * 4096, 4, Read, 0x00100020);
        c.Access(0x08000000, 4, Read, 0x00100020);                  // the application heap: no window
        host.guest = false;
        c.Access(0x30000000 + 5 * 4096, 0, Pointer, 0xDEAD);         // the new linear window, no guest thread
        host.guest = true;
        // event 2: a display transfer; its own read of VRAM page 0 is the GPU's; it writes pages 3 and 4
        // (unmarked above: new marks) and page 13 (still marked: rewritten, the mark keeps this writer)
        c.ExecuteBegin(transfer);
        c.Access(kVramV, 4096, CopySource, 0x1234);
        fcram[4 * 4096 + 2] = 0xAB;
        fcram[3 * 4096 + 2] = 0xAB;
        fcram[13 * 4096 + 2] = 0xAB;
        c.ExecuteEnd();
        // a register write between events: the CPU changed page 7 before it, the GPU nothing
        fcram[7 * 4096] = 1;
        c.WriteRegBegin(0, 1);
        c.WriteRegEnd();
        // a command inside a command is not a second event
        c.ExecuteBegin(dma);
        c.WriteRegBegin(0, 2);
        fcram[6 * 4096] = 0x66;
        c.WriteRegEnd();
        c.Access(kLinear + 13 * 4096, 4, BlockRead, 1);              // still inside the DMA: the GPU's
        c.Access(kLinear + 13 * 4096, 4, BlockRead, 1);              // the same mark again: not logged again
        c.ExecuteEnd();
        // the GPU's reads did not end the marks: the CPU's read of page 13 is still seen
        c.Access(kLinear + 13 * 4096 + 4, 4, Read, 0x7777);
        // unwatched pages: 10 has no window, 11 is aliased
        c.ExecuteBegin(submit);
        fcram[10 * 4096] = 1;
        fcram[11 * 4096] = 1;
        fcram[12 * 4096] = 1;
        c.ExecuteEnd();
        c.Access(kLinear + 10 * 4096, 4, BlockRead, 0x5555);          // reached through another table
        c.SetIgnored(7, 3);
        c.Finish();
        std::printf("host_marked_left %zu mark_pages %u accesses %llu\n", host.marked.size(), host.mark_pages,
                    static_cast<unsigned long long>(c.Accesses()));
    } else if (scenario == "regpu") {
        c.ExecuteBegin(submit);                                      // the GPU writes page 3: marked
        fcram[3 * 4096] = 1;
        c.ExecuteEnd();
        c.ExecuteBegin(dma);                                         // a DMA reads it (logged), and writes it again
        c.Access(kLinear + 3 * 4096, 4, CopySource, 0x10);
        c.Access(kLinear + 3 * 4096, 4, CopySource, 0x10);           // the same mark: not logged again
        fcram[3 * 4096] = 2;
        c.ExecuteEnd();                                              // rewritten: a new writer
        c.ExecuteBegin(dma);
        c.Access(kLinear + 3 * 4096, 4, CopySource, 0x20);           // logged again
        c.ExecuteEnd();
        c.Finish();
        std::printf("host_marked_left %zu mark_pages %u accesses %llu\n", host.marked.size(), host.mark_pages,
                    static_cast<unsigned long long>(c.Accesses()));
    } else if (scenario == "idle") {
        c.Finish();
        std::printf("host_marked_left %zu mark_pages %u accesses 0\n", host.marked.size(), host.mark_pages);
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler")
    work = tmp_path_factory.mktemp("coherence")
    (work / "region.inc").write_text(region_text(), encoding="utf-8")
    (work / "driver.cpp").write_text(DRIVER, encoding="utf-8")
    exe = str(work / "driver")
    base = [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(work), str(work / "driver.cpp"), "-o", exe]
    done = subprocess.run(base + ["-fsanitize=address,undefined"], capture_output=True, text=True)
    if done.returncode != 0:
        done = subprocess.run(base, capture_output=True, text=True)    # no sanitizer runtime here
    assert done.returncode == 0, done.stderr
    return exe


def run(driver, scenario, tmp_path):
    out = str(tmp_path / f"{scenario}.log")
    done = subprocess.run([driver, out, scenario], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr             # 98 and 99: the fake marker hit an UNREACHABLE rule
    info = {}
    for line in done.stdout.splitlines():
        parts = line.split()
        info.update({parts[i]: int(parts[i + 1]) for i in range(0, len(parts), 2)})
    return out, info


def accesses(path):
    return [f for tag, f in coh.read_log(open(path)) if tag == "ACCESS"]


def test_the_constants_are_the_ones_of_the_analysis_tool():
    text = open(RUNNER, encoding="utf-8").read()
    check = text[text.index("// BEGIN coherence-core"):text.index("// END coherence-core")]
    def const(name):
        return int(re.search(rf"\b{name} = (0x[0-9A-Fa-f]+)", check).group(1), 16)
    assert (const("kCohVramVaddr"), const("kCohVramSize"), const("kCohVramPaddr")) == (coh.VRAM_VADDR, coh.VRAM_SIZE, coh.VRAM_PADDR)
    assert (const("kCohLinearVaddr"), const("kCohLinearSize")) == (coh.LINEAR_HEAP_VADDR, coh.LINEAR_HEAP_SIZE)
    assert (const("kCohNewLinearVaddr"), const("kCohNewLinearSize")) == (coh.NEW_LINEAR_HEAP_VADDR, coh.NEW_LINEAR_HEAP_SIZE)
    assert const("kCohFcramPaddr") == coh.FCRAM_PADDR
    assert int(re.search(r"kCohPage = (\d+)", check).group(1)) == PAGE


def test_the_marker_is_never_asked_to_break_its_rules(driver, tmp_path):
    out, info = run(driver, "basic", tmp_path)                  # exit 98 or 99 would fail here
    assert info["host_marked_left"] == 0                         # Finish unmarks what is left, once each
    assert info["marked_after_1"] == 5 and info["calls"] == 3    # FCRAM pages 3 to 5 as one run, page 13, VRAM page 0


def test_the_first_access_is_logged_with_its_page_kind_caller_and_writer(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    acc = accesses(out)
    first = acc[0]
    assert (first["side"], first["kind"], first["paddr"], first["vaddr"], first["size"]) == (
        "cpu", "Read", "0x20004008", "0x14004008", "4")
    assert (first["guest"], first["lr"], first["in"]) == ("1", "0x00100020", "-")
    assert (first["mark"], first["mark_ev"], first["mark_f"], first["f"]) == ("1", "SubmitCmdList", "100", "101")
    assert int(first["t"]) > int(first["mark_t"])
    assert (first["watched"], first["aliased"]) == ("1", "0")
    write = acc[1]
    assert (write["kind"], write["paddr"]) == ("Write", "0x20003000")
    assert acc[2]["kind"] == "Pointer" and acc[2]["lr"] == "-" and acc[2]["guest"] == "0" and acc[2]["size"] == "0"
    assert acc[2]["paddr"] == "0x20005000"                       # through the new linear heap window


def test_a_page_is_logged_once_per_mark_and_foreign_accesses_are_only_counted(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    log = list(coh.read_log(open(out)))
    summary = dict(next(f for tag, f in log if tag == "SUMMARY"))
    assert summary["foreign"] == "2" and summary["no_window"] == "1"   # page 4 again and page 9; the heap address
    assert summary["outside_run"] == "7" and summary["other_thread"] == "3"
    reads = [a for a in accesses(out) if a["paddr"] == "0x20004008"]
    assert len(reads) == 1                                        # the second read of the unmarked page is not logged


def test_accesses_inside_a_gpu_command_are_the_gpus_own(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    gpu = [a for a in accesses(out) if a["side"] == "gpu"]
    assert [(a["kind"], a["paddr"], a["in"], a["lr"]) for a in gpu] == [
        ("CopySource", "0x18000000", "DisplayTransfer", "0x00001234"),
        ("BlockRead", "0x2000d000", "RequestDma", "0x00000001")]    # after the nested register write ended
    assert gpu[0]["mark_ev"] == "SubmitCmdList"
    assert gpu[1]["mark_ev"] == "DisplayTransfer"                # the latest writer of a page that was written twice
    cpu_loads = [a for a in accesses(out) if a["side"] == "cpu" and a["kind"] in coh.LOAD_KINDS]
    assert [a["paddr"] for a in cpu_loads] == ["0x20004008", "0x2000d004", "0x2000a000"]
    # the GPU's reads left their pages marked: the CPU's later read of page 13 is the one that ended its mark
    assert cpu_loads[1]["mark_ev"] == "DisplayTransfer"


def test_a_mark_is_kept_when_the_gpu_writes_the_page_again(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    ev = coh.analyse_log(open(out))["events"]
    # event 2 changed pages 3 and 4 (both unmarked by the CPU) and 13 (still marked): 2 new marks, 1 rewritten
    assert ev["DisplayTransfer"] == {"n": 1, "gpu_pages": 3, "marked": 2, "rewritten": 1, "unwatched": 0, "aliased": 0}
    assert ev["SubmitCmdList"]["gpu_pages"] == 5 + 3 and ev["SubmitCmdList"]["n"] == 2
    assert ev["Mmio"]["gpu_pages"] == 0                          # the CPU change before it is not the GPU's
    assert ev["RequestDma"] == {"n": 1, "gpu_pages": 1, "marked": 1, "rewritten": 0, "unwatched": 0, "aliased": 0}


def test_pages_the_process_cannot_reach_through_a_window_are_reported_not_hidden(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    rep = coh.analyse_log(open(out))
    kinds = {(u["kind"], u["paddr"], u["pages"]) for u in rep["unwatched"]}
    assert ("nowindow", 0x2000A000, 1) in kinds and ("aliased", 0x2000B000, 1) in kinds
    assert rep["events"]["SubmitCmdList"]["unwatched"] == 1 and rep["events"]["SubmitCmdList"]["aliased"] == 1
    late = [a for a in accesses(out) if a["paddr"] == "0x2000a000"]
    assert len(late) == 1 and late[0]["watched"] == "0"           # an access through another table is still logged
    assert rep["aliases"] == [{"f": 100, "vaddr": 0x08000000, "paddr": 0x2000B000, "pages": 1}]
    # pages 3, 4, 6, 11, 12 were never touched by the CPU, and VRAM page 0 was only read by the GPU
    assert rep["summary"]["pending"] == 6 and rep["complete"]


def test_the_gpu_side_access_of_a_page_the_gpu_wrote_again_is_logged_again(driver, tmp_path):
    out, info = run(driver, "regpu", tmp_path)
    gpu = [a for a in accesses(out) if a["side"] == "gpu"]
    assert [(a["paddr"], a["lr"], a["mark"]) for a in gpu] == [("0x20003000", "0x00000010", "1"),
                                                              ("0x20003000", "0x00000020", "2")]
    assert info["accesses"] == 2 and info["host_marked_left"] == 0
    ev = coh.analyse_log(open(out))["events"]
    assert ev["RequestDma"]["rewritten"] == 1 and ev["SubmitCmdList"]["marked"] == 1


def test_events_before_start_and_an_idle_run_leave_the_marker_alone(driver, tmp_path):
    out, info = run(driver, "idle", tmp_path)
    assert info["mark_pages"] == 0 and info["host_marked_left"] == 0
    rep = coh.analyse_log(open(out))
    assert rep["complete"] and rep["accesses"] == 0 and rep["summary"]["events"] == 0
    assert rep["summary"]["foreign"] == 0                         # the access before Start was not counted


def test_the_log_of_the_checker_is_a_valid_report_input(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    md = tmp_path / "m.md"
    assert coh.main(["runlog", out, "--md", str(md)]) == 0
    assert "CPU loads 3" in md.read_text()
    header = open(out).readline()
    assert re.fullmatch(r"# coherence v1 \d{4}-\d\d-\d\d game=0x[0-9a-f]{16} cpu=\w+ new3ds=(on|off)\n", header)


# ── the Azahar patch and the runner's options ─────────────────────────────────────────

def pinned_sources():
    for candidate in (os.environ.get("AZAHAR_SRC"), os.path.expanduser("~/azahar")):
        if candidate and os.path.isdir(os.path.join(candidate, "src", "video_core")):
            return candidate
    return None


def test_the_patch_hooks_every_rasterizer_cached_case_of_memory_cpp():
    patch = open(PATCH, encoding="utf-8").read()
    start = patch.index("diff --git a/src/core/memory.cpp b/src/core/memory.cpp")
    section = patch[start:patch.index("diff --git", start + 10)]
    added = re.findall(r"(?m)^\+\s+(?:impl->)?NoteCached\(.*CachedAccess::(\w+)\);", section)
    # Read<T> (2 cases), Write<T> (2), WriteExclusive<T> (2), ReadBlockImpl, WriteBlockImpl, ZeroBlock, CopyBlock, GetPointer (2)
    assert sorted(added) == sorted(["Read"] * 2 + ["Write"] * 2 + ["WriteExclusive"] * 2 + ["BlockRead", "BlockWrite",
                                   "ZeroBlock", "CopySource"] + ["Pointer"] * 2)
    assert "void SetCachedAccessObserver(CachedAccessObserver observer);" in patch
    kinds = re.search(r"enum class CachedAccess : u32 \{(.*?)\};", patch, re.S).group(1)
    kinds = re.sub(r"(?m)^\+", "", kinds)
    assert [k.split("=")[0].strip() for k in kinds.split(",") if k.strip()] == [
        "Read", "Write", "WriteExclusive", "BlockRead", "BlockWrite", "ZeroBlock", "CopySource", "Pointer"]


def test_the_access_names_of_the_runner_match_the_enumeration_of_the_patch():
    runner = open(RUNNER, encoding="utf-8").read()
    names = re.search(r"CohAccessName\(u32 kind\) \{\s*static const char\* const names\[kCohAccessKinds\] = \{(.*?)\};",
                      runner, re.S).group(1)
    assert re.findall(r'"(\w+)"', names) == ["Read", "Write", "WriteExclusive", "BlockRead", "BlockWrite", "ZeroBlock",
                                             "CopySource", "Pointer"]
    events = re.search(r"CohEventName\(u32 ev\) \{\s*static const char\* const names\[kCohEventKinds\] = \{(.*?)\};",
                       runner, re.S).group(1)
    assert re.findall(r'"(\w+)"', events)[:6] == ["RequestDma", "SubmitCmdList", "MemoryFill", "DisplayTransfer",
                                                  "TextureCopy", "CacheFlush"]


def test_the_patch_applies_to_the_pinned_azahar_sources(tmp_path):
    root = pinned_sources()
    if not root or not shutil.which("git"):
        pytest.skip("the pinned Azahar sources are not around (AZAHAR_SRC or ~/azahar)")
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True).stdout.strip()
    if head != open(PINNED).read().strip():
        pytest.skip("the Azahar checkout is not at the pinned commit")
    patch = open(PATCH, encoding="utf-8").read()
    for path in re.findall(r"(?m)^diff --git a/(\S+) b/", patch):
        src = os.path.join(root, path)
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(tmp_path / path), exist_ok=True)
            shutil.copy(src, tmp_path / path)
    done = subprocess.run(["git", "apply", "--check", PATCH], cwd=tmp_path, capture_output=True, text=True)
    assert done.returncode == 0, done.stderr


def test_the_runner_refuses_the_options_that_share_the_gpu_hooks():
    runner = open(RUNNER, encoding="utf-8").read()
    assert '"--check-coherence" && i + 1 < argc' in runner
    assert re.search(r"!coherence_file\.empty\(\) && \(!capture_gpu_file\.empty\(\) \|\| !perf_log\.empty\(\)\)", runner)
    # drawing stays on and the right eye is rendered, as for --capture-gpu
    assert "if (!capture_gpu_file.empty() || !coherence_file.empty()) {" in runner
    assert "capture_gpu_file.empty() && coherence_file.empty()" in runner
    readme = open(os.path.join(ROOT, "port", "README.md"), encoding="utf-8").read()
    assert "--check-coherence" in readme and "coherence.py" in readme
