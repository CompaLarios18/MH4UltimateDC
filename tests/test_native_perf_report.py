"""N0-15: tools/native/perf_report.py on synthetic CSV, and port/src/perf.{h,cpp} (the timers) built
standalone with a fake clock. No game data: every number here is made up."""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "native"))

import perf_report  # noqa: E402

HEADER = "# perf v1 2026-01-01 game=0x0000000000000000 cpu=recomp new3ds=off\nframe,present_us,recomp_us,svc_us,gpu_us\n"


def make_csv(rows):
    """rows of (present, recomp, svc, gpu) in microseconds."""
    body = "".join(f"{i},{p:.3f},{r:.3f},{s:.3f},{g:.3f}\n" for i, (p, r, s, g) in enumerate(rows))
    return HEADER + body + f"# frames={len(rows)} dropped=0\n"


# ── perf_report.py ──────────────────────────────────────────────────────────────────

def test_percentiles_are_nearest_rank():
    v = [float(i) for i in range(1, 101)]
    assert perf_report.percentile(v, 50) == 50
    assert perf_report.percentile(v, 95) == 95
    assert perf_report.percentile(v, 99) == 99
    assert perf_report.percentile(v, 100) == 100
    assert perf_report.percentile([7.0], 99) == 7.0


def test_report_columns_percentiles_and_derived_other():
    # 100 frames: present 1000..100000 us in steps of 1000, parts 10 % + 20 % + 30 % of it
    rows = [(i * 1000.0, i * 100.0, i * 200.0, i * 300.0) for i in range(1, 101)]
    text = perf_report.report(make_csv(rows), budget_ms=50.0)
    assert "| present (ms) | 50.000 | 95.000 | 99.000 | 100.000 |" in text
    assert "| recomp (ms) | 5.000 | 9.500 | 9.900 | 10.000 |" in text
    assert "| svc (ms) | 10.000 | 19.000 | 19.800 | 20.000 |" in text
    assert "| gpu (ms) | 15.000 | 28.500 | 29.700 | 30.000 |" in text
    assert "| other (ms) | 20.000 | 38.000 | 39.600 | 40.000 |" in text  # present - the three parts
    assert "Frames: 100" in text
    assert "Log: `perf v1 2026-01-01" in text


def test_frames_over_budget_and_stutters():
    rows = [(8000.0, 1.0, 1.0, 1.0)] * 90 + [(9000.0, 1.0, 1.0, 1.0)] * 8 + [(60000.0, 1.0, 1.0, 1.0)] * 2
    text = perf_report.report(make_csv(rows), budget_ms=1000.0 / 120)
    assert "over the budget of 8.333 ms (present_us): 10 of 100 (10.00%)" in text
    assert "over 50 ms (present_us): 2 of 100 (2.00%)" in text
    # the budget applies to the column asked for
    text = perf_report.report(make_csv(rows), budget_ms=0.0005, column="svc_us")
    assert "(svc_us): 100 of 100 (100.00%)" in text


def test_exactly_on_budget_is_not_over():
    text = perf_report.report(make_csv([(8000.0, 1, 1, 1)] * 4), budget_ms=8.0)
    assert ": 0 of 4 (0.00%)" in text.splitlines()[-2]


def test_skip_drops_warmup_frames():
    rows = [(500000.0, 1, 1, 1)] * 3 + [(8000.0, 1, 1, 1)] * 7
    text = perf_report.report(make_csv(rows), skip=3)
    assert "Frames: 7 (the first 3 skipped)" in text
    assert "| present (ms) | 8.000 | 8.000 | 8.000 | 8.000 |" in text


def test_other_is_never_negative():
    text = perf_report.report(make_csv([(1000.0, 900.0, 900.0, 900.0)] * 3))
    assert "| other (ms) | 0.000 | 0.000 | 0.000 | 0.000 |" in text


def test_extra_columns_are_reported_as_they_are():
    csv = "frame,present_us,vram_mb\n0,8000,100\n1,9000,200\n"
    text = perf_report.report(csv)
    assert "| vram_mb | 100.000 | 200.000 | 200.000 | 200.000 |" in text
    assert "other" not in text  # the parts are missing: nothing to derive


@pytest.mark.parametrize("csv,fragment", [
    ("", "no header"),
    ("0,1,2\n", "expected the header"),
    ("frame,foo\n0,1\n", "no present_us"),
    ("frame,present_us\n0,1,2\n", "3 fields"),
    ("frame,present_us\n0,abc\n", "not numbers"),
    ("frame,present_us\n0,-5\n", "negative"),
    ("frame,present_us\n0,nan\n", "negative or non-finite"),
    ("frame,present_us,present_us\n0,1,2\n", "expected the header"),
    ("frame,present_us\n", "no frames"),
])
def test_bad_input_is_refused(csv, fragment):
    with pytest.raises(perf_report.FormatError, match=fragment):
        perf_report.report(csv)


def test_command_line(tmp_path, capsys):
    path = tmp_path / "p.csv"
    path.write_text(make_csv([(8000.0, 2000.0, 1000.0, 500.0)] * 10))
    assert perf_report.main([str(path), "--fps", "60"]) == 0
    out = capsys.readouterr().out
    assert "budget of 16.667 ms" in out and out.startswith("## Performance report: p.csv")
    assert perf_report.main([str(path), "--skip", "10"]) == 2
    assert perf_report.main([str(tmp_path / "missing.csv")]) == 2
    (tmp_path / "bad.csv").write_text("hello\n")
    assert perf_report.main([str(tmp_path / "bad.csv")]) == 2
    assert perf_report.main([str(path), "--budget-ms", "0"]) == 2


# ── the timers, built without Azahar ───────────────────────────────────────────────────

DRIVER = textwrap.dedent(r"""
    #include "perf.h"
    #include <cstdio>
    #include <cstdlib>
    #include <functional>
    #include <string>
    using namespace Port::Perf;

    static std::uint64_t g_now = 1000;
    static std::uint64_t Clock() { return g_now; }
    static void Advance(std::uint64_t us) { g_now += us * 1000; }

    // the members of Pica::CaptureHooks that the timers use
    struct Command { int id; };
    struct Hooks {
        std::function<void(const Command&)> execute_begin;
        std::function<void()> execute_end;
        std::function<void(unsigned, unsigned)> write_reg_begin;
        std::function<void()> write_reg_end;
        std::function<void(int, unsigned long long)> interrupt;
    };

    #define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)

    int main(int, char** argv) {
        std::string error;
        SetClockForTest(Clock);
        CHECK(!Enabled());
        { Scope s(Slot::Svc); Advance(5); }  // off: nothing happens
        CHECK(Start(argv[1], "perf v1 2026-01-01 game=0x0000000000000000 cpu=recomp new3ds=off", error));
        CHECK(!Start(argv[1], "x", error));
        CHECK(Enabled());

        int previous_calls = 0;
        Hooks hooks;
        hooks.execute_begin = [&](const Command&) { ++previous_calls; };
        hooks.interrupt = [&](int, unsigned long long) { ++previous_calls; };
        ChainGpuHooks(hooks, [](int id) { return id == 2; });

        hooks.interrupt(2, 0);  // first vblank: starts frame 0, no row
        // frame 0: 3000 us in the code, a system call of 1000 us with a 400 us GPU command in it,
        // a 250 us GPU command outside of any call, 7000 us nowhere
        {
            Scope code(Slot::Recomp);
            Advance(2000);
            {
                Scope svc(Slot::Svc);
                Advance(300);
                hooks.execute_begin(Command{1});
                Advance(400);
                hooks.execute_end();
                Advance(300);
            }
            Advance(1000);
        }
        hooks.write_reg_begin(0x1000, 1);
        Advance(250);
        hooks.write_reg_end();
        Advance(7000);
        hooks.interrupt(0, 0);  // not a vblank
        hooks.interrupt(2, 0);  // frame 0 ends: 11250 us from the first vblank to this one
        // frame 1: nothing but 8333 us
        Advance(8333);
        hooks.interrupt(2, 0);
        // frame 2: an unbalanced Leave (a scope opened before the log) is ignored
        Leave(Slot::Gpu);
        Advance(100);
        hooks.interrupt(2, 0);
        CHECK(previous_calls == 1 + 5);  // execute_begin once, the old interrupt hook on all 5 interrupts
        Stop();
        Stop();
        CHECK(!Enabled());
        CHECK(DroppedFrames() == 0);
        // after Stop the timers do nothing
        { Scope s(Slot::Svc); Advance(5); }
        FrameBoundary();
        return 0;
    }
""")


@pytest.mark.skipif(shutil.which("clang++") is None and shutil.which("g++") is None, reason="no C++ compiler")
def test_timers_with_a_fake_clock(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    driver = tmp_path / "driver.cpp"
    driver.write_text(DRIVER)
    exe = tmp_path / "driver"
    cmd = [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT / "port" / "src"),
           str(driver), str(ROOT / "port" / "src" / "perf.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr[-3000:]
    log = tmp_path / "perf.csv"
    r = subprocess.run([str(exe), str(log)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    lines = log.read_text().splitlines()
    assert lines[0] == "# perf v1 2026-01-01 game=0x0000000000000000 cpu=recomp new3ds=off"
    assert lines[1] == "frame,present_us,recomp_us,svc_us,gpu_us"
    # frame 0 lasts 4000 + 250 + 7000 us. The code scope lasts 4000, of which the call takes 1000
    # (recomp = 3000); the call's 1000 hold the GPU command's 400 (svc = 600); gpu = 400 + 250
    assert lines[2] == "0,11250.000,3000.000,600.000,650.000"
    assert lines[3] == "1,8333.000,0.000,0.000,0.000"
    assert lines[4] == "2,100.000,0.000,0.000,0.000"
    assert lines[5] == "# frames=3 dropped=0"
    assert len(lines) == 6
    # and the report reads what the timers wrote
    columns, rows, meta = perf_report.parse(log.read_text())
    assert columns == ["frame", "present_us", "recomp_us", "svc_us", "gpu_us"] and len(rows) == 3
    text = perf_report.report(log.read_text(), budget_ms=8.333)
    assert "over the budget of 8.333 ms (present_us): 1 of 3" in text


def test_perf_sources_include_nothing_of_azahar():
    bad = re.compile(r'^\s*#\s*include\s*[<"](?:core|video_core|common|network|audio_core|input_common)/')
    for name in ("perf.h", "perf.cpp"):
        for n, line in enumerate((ROOT / "port" / "src" / name).read_text().splitlines(), 1):
            assert not bad.match(line), f"{name}:{n} includes Azahar"


def test_perf_is_wired_into_the_build_and_the_options():
    cmake = (ROOT / "port" / "CMakeLists.txt").read_text()
    assert "src/perf.cpp" in cmake and "src/perf.h" in cmake
    runner = (ROOT / "port" / "src" / "runner.cpp").read_text()
    assert '"--perf-log"' in runner and "--perf-log FILE" in runner
    assert "Perf::Scope" in (ROOT / "port" / "src" / "arm_recomp.cpp").read_text()
    assert "perf_log" in (ROOT / "port" / "app" / "config.cpp").read_text()
