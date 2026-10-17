"""The GPU capture recorder of port/src/runner.cpp (N0-05), without Azahar.

runner.cpp keeps the recording logic of --capture-gpu between the markers `BEGIN gpu-capture-core`
and `END gpu-capture-core`, free of Azahar types. These tests compile that region on its own with a
scripted game (synthetic memory, a made-up frame counter), make real captures with it, and check
them with tools/native/gcap.py. They do not run Azahar: the hooks of azahar.patch and the part of the
runner that reads the PICA core can only be exercised by the CI runner job and by a run with the game.
The patch is checked here only for applying to the pinned Azahar sources, when they are around.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))

import gcap  # noqa: E402

RUNNER = os.path.join(ROOT, "port", "src", "runner.cpp")
PATCH = os.path.join(ROOT, "port", "azahar", "azahar.patch")
PINNED = os.path.join(ROOT, "port", "azahar", "COMMIT")
PAGE = gcap.PAGE_SIZE
FCRAM, VRAM = 0x20000000, 0x18000000


def region_text() -> str:
    text = open(RUNNER, encoding="utf-8").read()
    start = text.index("// BEGIN gpu-capture-core")
    end = text.index("// END gpu-capture-core")
    return text[start:end]


DRIVER = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#include "region.inc"

struct FakeHost : GpuCaptureHost {
    u32 frame = 100;
    u64 ticks = 1000;
    std::vector<u32> images;
    u32 Frame() override { return frame; }
    u64 Ticks() override { return ticks += 10; }
    void FrameImages(u32 f) override { images.push_back(f); }
};

static std::vector<GpuInitBlock> InitBlocks() {
    std::vector<GpuInitBlock> blocks;
    for (u32 i = 0; i < 11; ++i) {
        GpuInitBlock b;
        b.kind = i + 1;
        b.data.assign(kGcapInitSizes[i], static_cast<u8>(i));
        blocks.push_back(std::move(b));
    }
    return blocks;
}

static void Summary(FakeHost& host, GpuRecorder& rec) {
    std::printf("images");
    for (u32 f : host.images) std::printf(" %u", f);
    std::printf("\nframes %u first %u last %u finished %d started %d dropped %u bytes %llu\n", rec.FramesWritten(),
                rec.FirstFrame(), rec.LastFrame(), rec.Finished(), rec.Started(), rec.DroppedRegWrites(),
                static_cast<unsigned long long>(rec.Bytes()));
    for (u32 t = 1; t <= 12; ++t) std::printf("count %u %u\n", t, rec.Count(t));
}

int main(int, char** argv) {
    const std::string scenario = argv[2];
    FakeHost host;
    if (scenario == "basic") {
        std::vector<u8> fcram(8 * 4096), vram(4 * 4096);
        for (u32 i = 0; i < 8; ++i) std::fill_n(fcram.begin() + i * 4096, 4096, static_cast<u8>(0x10 + i));
        for (u32 i = 0; i < 4; ++i) std::fill_n(vram.begin() + i * 4096, 4096, static_cast<u8>(0x80 + i));
        GpuRecorder rec(host, 2);
        if (!rec.Open(argv[1])) return 2;
        std::vector<GpuMemoryRegion> regions(2);
        regions[0].paddr = 0x20000000; regions[0].size = fcram.size(); regions[0].live = fcram.data();
        regions[1].paddr = 0x18000000; regions[1].size = vram.size(); regions[1].live = vram.data();
        // hooks before the start do nothing
        rec.Fill(1);
        rec.Interrupt(2, 5);
        rec.Start(InitBlocks(), regions);

        // frame 100: the CPU changes page 1, a command list runs, the GPU changes page 5 and VRAM page 0
        fcram[4096 + 5] = 0xAA;
        const u32 submit[8] = {1, 0x20001000, 0x100, 0, 0, 0, 0, 0};
        rec.ExecuteBegin(submit);
        rec.RegWrite(0x010, 0xF, 0x11111111);
        rec.RegWrite(0x0E0, 0x1, 5);
        rec.RegWrite(0x300, 0xF, 99);       // outside the PICA file: not recorded
        rec.Interrupt(5, 1234);             // waits for the end of the command
        fcram[5 * 4096] = 0xBB;
        vram[0] = 0xCC;
        rec.ExecuteEnd();

        // the GPU's own writes are not CPU changes: this GX has no CPU_PAGES
        const u32 transfer[8] = {3, 0x1f000000, 0x20005000, 0x100, 0x100, 0, 0, 0};
        rec.ExecuteBegin(transfer);
        rec.Interrupt(4, 77);
        rec.ExecuteEnd();

        // a register write that starts a command list: its PICA writes cannot be recorded
        rec.WriteRegBegin(0x1EF00000 + 0x18, 1);
        rec.RegWrite(0x020, 0xF, 1);
        rec.WriteRegEnd();

        const u32 info[7] = {1, 0x1f000000, 0, 720, 0, 0, 0};
        fcram[7 * 4096 + 9] = 0x5A;
        rec.Swap(0, info);
        rec.Fill(0x01FF0000);
        rec.Flush(1, 0x14000000, 0x100);
        const u32 cache[8] = {5, 0x14000000, 0x40, 0, 0, 0x14100000, 0x80, 0};
        rec.ExecuteBegin(cache);
        rec.ExecuteEnd();
        rec.Interrupt(2, 0);                // a VBlank interrupt outside any command

        // frame 101: the FRAME record comes before the event that shows it
        host.frame = 101;
        rec.Swap(1, info);
        // frame 102 is more than wanted: nothing from here on
        host.frame = 102;
        rec.Fill(0x01000000);
        rec.ExecuteBegin(submit);
        rec.RegWrite(0x010, 0xF, 1);
        rec.ExecuteEnd();
        rec.Poll();
        rec.Close();
        Summary(host, rec);
    } else if (scenario == "chunks") {
        // a region of 20 MiB is recorded in two MEM records
        std::vector<u8> big(20u << 20);
        for (size_t i = 0; i < big.size(); i += 4096) big[i] = static_cast<u8>(i >> 12);
        GpuRecorder rec(host, 1);
        if (!rec.Open(argv[1])) return 2;
        std::vector<GpuMemoryRegion> regions(1);
        regions[0].paddr = 0x20000000; regions[0].size = big.size(); regions[0].live = big.data();
        host.frame = 500;
        rec.Start(InitBlocks(), regions);
        host.frame = 501;
        rec.Poll();
        rec.Close();
        Summary(host, rec);
    } else if (scenario == "burst") {
        // many pages change at once, and the same page changes twice in a row
        std::vector<u8> mem(64 * 4096);
        GpuRecorder rec(host, 5);
        if (!rec.Open(argv[1])) return 2;
        std::vector<GpuMemoryRegion> regions(1);
        regions[0].paddr = 0x20000000; regions[0].size = mem.size(); regions[0].live = mem.data();
        rec.Start(InitBlocks(), regions);
        for (u32 round = 0; round < 3; ++round) {
            for (u32 p = 0; p < 64; p += 3) mem[p * 4096 + round] = static_cast<u8>(round + 1);
            const u32 fill[8] = {2, 0x1f000000, 0, 0x1f001000, 0, 0, 0, 0};
            rec.ExecuteBegin(fill);
            rec.ExecuteEnd();
        }
        rec.Close();
        Summary(host, rec);
    } else {
        return 3;
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler")
    work = tmp_path_factory.mktemp("recorder")
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
    out = str(tmp_path / f"{scenario}.mgpu")
    done = subprocess.run([driver, out, scenario], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    info: dict = {"count": {}}
    for line in done.stdout.splitlines():
        parts = line.split()
        if parts[0] == "count":
            info["count"][int(parts[1])] = int(parts[2])
        elif parts[0] == "images":
            info["images"] = [int(x) for x in parts[1:]]
        else:
            info.update({parts[i]: int(parts[i + 1]) for i in range(0, len(parts), 2)})
    return out, info


def names(records):
    return [r.name for r in records]


def test_the_constants_are_the_formats(driver):
    text = region_text()
    ids = {name: int(value) for name, value in re.findall(r"\b(kGcap[A-Za-z]+) = (\d+)", text.split("kGcapPage")[0])}
    assert ids == {"kGcapInit": gcap.INIT, "kGcapMem": gcap.MEM, "kGcapCpuPages": gcap.CPU_PAGES, "kGcapGx": gcap.GX,
                   "kGcapPicaReg": gcap.PICA_REG, "kGcapGpuPages": gcap.GPU_PAGES, "kGcapMmio": gcap.MMIO,
                   "kGcapSwap": gcap.SWAP, "kGcapFill": gcap.FILL, "kGcapIrq": gcap.IRQ, "kGcapFlush": gcap.FLUSH,
                   "kGcapFrame": gcap.FRAME}
    sizes = re.search(r"kGcapInitSizes\[11\] = \{([^}]*)\}", text).group(1)
    assert [int(x) for x in sizes.split(",")] == [gcap.INIT_BLOCK_SIZES[k] for k in range(1, 12)]
    assert int(re.search(r"kGcapPage = (\d+)", text).group(1)) == gcap.PAGE_SIZE
    assert int(re.search(r"kGcapGxSubmitCmdList = (\d+)", text).group(1)) == gcap.GX_SUBMIT_CMDLIST
    assert int(re.search(r"kGcapMaxPicaReg = (0x[0-9a-f]+)", text).group(1), 16) == gcap.NUM_PICA_REGS


def test_a_capture_the_recorder_makes_passes_the_checker(driver, tmp_path):
    out, info = run(driver, "basic", tmp_path)
    assert gcap.check_file(out) == []
    assert gcap.main(["check", out]) == 0
    recs = gcap.read_all(out)
    assert recs[0].type == gcap.INIT and sum(r.type == gcap.INIT for r in recs) == 1
    assert [k for k, _ in gcap.parse_init(recs[0].payload)] == list(range(1, 12))
    assert info["count"][gcap.FRAME] == 2 == info["frames"]
    assert info["images"] == [100, 101]


def test_the_records_come_in_the_intended_order(driver, tmp_path):
    out, info = run(driver, "basic", tmp_path)
    recs = gcap.read_all(out)
    kinds = names(recs)
    assert kinds == [
        "INIT", "MEM", "MEM", "FRAME",
        # the command list: CPU change before it, its register writes, what the GPU changed, then the interrupt
        "CPU_PAGES", "GX", "PICA_REG", "GPU_PAGES", "IRQ",
        # the transfer: no CPU change (the GPU's own writes were taken into the shadow), PPF after
        "GX", "IRQ",
        # a register write that starts a list: no PICA_REG
        "MMIO",
        "CPU_PAGES", "SWAP", "FILL", "FLUSH",
        "GX", "FLUSH", "FLUSH", "IRQ",
        # the next frame: FRAME first
        "FRAME", "SWAP",
    ], kinds
    assert info["dropped"] == 1
    assert info["finished"] == 1


def test_pages_registers_and_interrupts_carry_the_right_data(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    recs = gcap.read_all(out)
    by = lambda name: [r for r in recs if r.name == name]
    cpu = gcap.parse_pages(by("CPU_PAGES")[0].payload)
    assert [a for a, _ in cpu] == [FCRAM + PAGE] and cpu[0][1][5] == 0xAA
    gpu = gcap.parse_pages(by("GPU_PAGES")[0].payload)
    assert sorted(a for a, _ in gpu) == [VRAM, FCRAM + 5 * PAGE]
    assert dict(gpu)[FCRAM + 5 * PAGE][0] == 0xBB and dict(gpu)[VRAM][0] == 0xCC
    assert gcap.parse_pica_reg(by("PICA_REG")[0].payload) == [(0x010, 0xF, 0x11111111), (0x0E0, 0x1, 5)]
    assert [gcap.parse_irq(r.payload) for r in by("IRQ")] == [(5, 1234), (4, 77), (2, 0)]
    cpu2 = gcap.parse_pages(by("CPU_PAGES")[1].payload)
    assert [a for a, _ in cpu2] == [FCRAM + 7 * PAGE]
    flush = [struct.unpack("<III", r.payload) for r in by("FLUSH")]
    assert flush == [(1, 0x14000000, 0x100), (4, 0x14000000, 0x40), (4, 0x14100000, 0x80)]
    gx = [gcap.parse_gx(r.payload) for r in by("GX")]
    assert [g[0] for g in gx] == [1, 3, 5]
    assert gcap.parse_gx(by("GX")[0].payload)[1][1:3] == (0x20001000, 0x100)


def test_memory_and_registers_replay_from_the_capture(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    recs = gcap.read_all(out)
    mem = gcap.memory_at(recs, len(recs))
    assert mem.read(FCRAM + PAGE + 5, 1) == b"\xaa"
    assert mem.read(FCRAM + 5 * PAGE, 1) == b"\xbb"
    assert mem.read(VRAM, 1) == b"\xcc"
    assert mem.read(FCRAM + 7 * PAGE + 9, 1) == b"\x5a"
    assert mem.read(FCRAM + 2 * PAGE, 1) == bytes([0x12])           # untouched, from MEM
    regs = gcap.registers_at(recs, len(recs))
    assert regs[0x010] == 0x11111111
    assert regs[0x0E0] & 0xFF == 5
    assert regs[0x020] == 0x02020202                                 # the dropped write is not replayed


def test_headers_carry_the_frame_and_increasing_ticks(driver, tmp_path):
    out, _ = run(driver, "basic", tmp_path)
    recs = gcap.read_all(out)
    assert [r.frame for r in recs[:4]] == [100, 100, 100, 100]
    assert recs[-2].type == gcap.FRAME and recs[-2].frame == 101 and recs[-1].frame == 101
    ticks = [r.ticks for r in recs]
    assert ticks == sorted(ticks) and len(set(ticks)) == len(ticks)


def test_a_large_region_is_split_into_mem_records(driver, tmp_path):
    out, info = run(driver, "chunks", tmp_path)
    assert gcap.check_file(out) == []
    recs = gcap.read_all(out)
    mems = [gcap.parse_mem(r.payload) for r in recs if r.type == gcap.MEM]
    assert [(a, len(d)) for a, d in mems] == [(FCRAM, 16 << 20), (FCRAM + (16 << 20), 4 << 20)]
    mem = gcap.memory_at(recs, len(recs))
    assert mem.read(FCRAM + (17 << 20), 1) == bytes([(17 << 20 >> 12) & 0xFF])
    assert info["frames"] == 1 and info["finished"] == 1 and info["images"] == [500]


def test_pages_that_change_in_a_burst_are_each_recorded_once(driver, tmp_path):
    out, info = run(driver, "burst", tmp_path)
    assert gcap.check_file(out) == []
    recs = gcap.read_all(out)
    cpu = [gcap.parse_pages(r.payload) for r in recs if r.type == gcap.CPU_PAGES]
    assert len(cpu) == 3 and all(len(p) == 22 for p in cpu)        # pages 0, 3, ... 63 change in each round
    assert not any(r.type == gcap.GPU_PAGES for r in recs)           # the fake GPU changed nothing
    assert info["finished"] == 0 and info["frames"] == 1


def pinned_sources():
    for candidate in (os.environ.get("AZAHAR_SRC"), os.path.expanduser("~/azahar")):
        if candidate and os.path.isdir(os.path.join(candidate, "src", "video_core")):
            return candidate
    return None


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


def test_the_patch_has_every_observer_the_runner_sets():
    patch = open(PATCH, encoding="utf-8").read()
    runner = open(RUNNER, encoding="utf-8").read()
    hooks = re.findall(r"hooks\.(\w+) = ", runner.split("bool StartGpuCapture")[1].split("return true;")[0])
    assert sorted(hooks) == sorted(["execute_begin", "execute_end", "write_reg_begin", "write_reg_end",
                                    "buffer_swap", "color_fill", "interrupt", "reg_write"])
    for hook in hooks:
        assert re.search(r"(?m)^\+\s+std::function<[^\n]*> " + hook + ";", patch), hook    # declared
        assert re.search(r"(?m)^\+.*g_capture_hooks\." + hook + r"\(", patch), hook         # called
    # the register-write observer stands at all four places the command processor writes registers
    assert len(re.findall(r"(?m)^\+.*g_capture_hooks\.reg_write\(", patch)) == 4
