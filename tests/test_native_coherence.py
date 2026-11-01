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


def make_capture(path: str, build) -> None:
    """A capture with 16 pages of FCRAM (page i filled with 0x10+i) and 4 of VRAM, then whatever BUILD writes."""
    with gcap.Writer(path) as w:
        w.init()
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
    w.mmio(0x1EF00018, 1)
    w.gpu_pages([(FCRAM + 8 * PAGE, page(0x78))])
    cpu_change(w, {FCRAM + 7 * PAGE: page(0x17, {3: 3})})
    w.swap(0, [0, VRAM_V, 0, 720, 0, 0, 0])
    w.fill(0x01FF0000)                                              # nothing changed before it
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


def test_a_flush_after_the_event_does_not_cover_it_but_the_next_one(tmp_path):
    def build(w):
        w.submit_cmdlist(LINEAR, 4)                                  # the first event, nothing changed
        w.pica_reg([])
        cpu_change(w, {FCRAM + PAGE: page(0x11, {0: 1})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # event 2: uncovered
        w.flush(1, LINEAR + PAGE, PAGE)                              # after it: too late for event 2
        cpu_change(w, {FCRAM + 2 * PAGE: page(0x12, {0: 1})})
        w.gx([2, LINEAR, 0, LINEAR + PAGE, 0, 0, 0, 0])              # event 3: the same flush is before it, too wide?
    path = str(tmp_path / "late.mgpu")
    make_capture(path, build)
    row = coh.analyse_capture(gcap.read(path))["rows"]["MEMORY_FILL"]
    assert row["events"] == 2 and row["pages"] == 2
    assert row["covered_bytes"]["any"] == 0 and row["pages_none_any"] == 2   # the flush is of page 1, not 2


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
        c.WriteRegBegin(0x1EF00018, 1);
        c.WriteRegEnd();
        // a command inside a command is not a second event
        c.ExecuteBegin(dma);
        c.WriteRegBegin(0x1EF00018, 2);
        fcram[6 * 4096] = 0x66;
        c.WriteRegEnd();
        c.Access(kLinear + 13 * 4096, 4, BlockRead, 1);              // still inside the DMA: the GPU's
        c.ExecuteEnd();
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
    assert len(cpu_loads) == 2                                    # page 4 and the late read of page 10


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
    assert rep["summary"]["pending"] == 5 and rep["complete"]    # pages 3, 4, 6, 11, 12 were never touched


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
    assert "CPU loads 2" in md.read_text()
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
