"""N0-15: tools/native/perf_report.py on synthetic CSV, and port/src/perf.{h,cpp} (the timers) built
standalone with a fake clock. No game data: every number here is made up."""
from __future__ import annotations

import random
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
    # a frame rate of 0 is a bad budget, not "use the default"; neither is a budget that is not finite
    for bad in (["--fps", "0"], ["--fps", "-60"], ["--budget-ms", "inf"], ["--budget-ms", "nan"], ["--fps", "inf"]):
        assert perf_report.main([str(path), *bad]) == 2, bad
    capsys.readouterr()
    assert perf_report.main([str(path)]) == 0
    assert "budget of 8.333 ms" in capsys.readouterr().out


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


# -- the native runner's log (N1-23) -----------------------------------------------------------------------------

NATIVE_HEADER = ("# perf v1 2026-01-01 game=0x0000000000000000 cpu=native new3ds=off clock={clock} mode={mode}\n"
                 "frame,present_us,main_us,guest_us,svc_us,service_us,idle_us,switches,preempts\n")


def native_csv(rows, clock="host", first_frame=1):
    """rows of (present, main, guest, svc, service, idle) in microseconds; frames count from first_frame."""
    mode = "realtime" if clock == "host" else "deterministic"
    body = "".join(f"{first_frame + i},{p:.3f},{m:.3f},{g:.3f},{s:.3f},{v:.3f},{d:.3f},3,1\n"
                   for i, (p, m, g, s, v, d) in enumerate(rows))
    return NATIVE_HEADER.format(clock=clock, mode=mode) + body + f"# frames={len(rows)} dropped=0\n"


def state_trace(scenes, first_frame=1):
    """A --state-trace with one record per frame; `scenes` are scene values."""
    lines = ["# statetrace v1 2026-01-01 game=0x0000000000000000 cpu=native new3ds=off\n"]
    for i, scene in enumerate(scenes):
        lines.append(f"f={first_frame + i} t={i * 1000} frame_counter=0x{first_frame + i:08x} scene=0x{scene:08x}\n")
    return "".join(lines)


HUNT = 0x00E655C8
DEMO = 0x00E64D5C  # aDemo, the intro movies: a scene that is not the hunt


def test_native_columns_get_busy_and_game_thread_time():
    # 100 frames: main 1..100 ms / 25, svc and service fixed, other threads' guest time on top
    rows = [(8333.0, i * 40.0, i * 40.0 + 500.0, 300.0, 100.0, 5000.0) for i in range(1, 101)]
    columns, parsed, _ = perf_report.parse(native_csv(rows))
    data = perf_report.columns_of(columns, parsed)
    assert data["busy_us"][0] == 40.0 + 500.0 + 300.0 + 100.0
    assert data["game_us"][0] == 40.0 + 300.0 + 100.0
    assert "other_us" not in data  # no gpu_us in this log: nothing to derive it from
    text = perf_report.report(native_csv(rows))
    assert "| game (ms) | 2.400 | 4.200 | 4.360 | 4.400 |" in text  # main + 400 us of calls and services
    assert "| main (ms) | 2.000 | 3.800 | 3.960 | 4.000 |" in text
    assert "| switches | 3.000 | 3.000 | 3.000 | 3.000 |" in text


def test_the_verdict_names_both_bounds():
    # game_us = main + 400; the p99 of main is 3.960 ms: upper 4.360 against 4 ms, lower 3.960 below it;
    # the other threads add 500 us of guest code, so busy_us = main + 900 (p99 4.860) is not game_us
    rows = [(8333.0, i * 40.0, i * 40.0 + 500.0, 300.0, 100.0, 0.0) for i in range(1, 101)]
    text = perf_report.report(native_csv(rows))
    assert ("Game thread, p99 against its budget of 4.000 ms: game_us (upper bound) 4.360 ms, "
            "main_us (lower bound) 3.960 ms: UNDECIDED") in text
    assert "Every guest thread together (busy_us, every guest thread's counted work), p99: 4.860 ms" in text
    # the rows of a real log are in frame order, not sorted: the verdict sorts before it takes the p99
    shuffled = list(rows)
    random.Random(1).shuffle(shuffled)
    text = perf_report.report(native_csv(shuffled))
    assert "game_us (upper bound) 4.360 ms, main_us (lower bound) 3.960 ms: UNDECIDED" in text
    assert "Every guest thread together (busy_us, every guest thread's counted work), p99: 4.860 ms" in text
    # within: the upper bound is under the budget
    low = [(8333.0, 1000.0, 1500.0, 300.0, 100.0, 0.0)] * 20
    assert ": WITHIN" in perf_report.report(native_csv(low))
    # over: even the main thread alone is over
    high = [(8333.0, 4500.0, 5000.0, 300.0, 100.0, 0.0)] * 20
    assert ": OVER" in perf_report.report(native_csv(high))
    # the budget is an option; exactly on it is within
    on = [(8333.0, 1000.0, 1000.0, 1500.0, 1500.0, 0.0)] * 5
    assert ": WITHIN" in perf_report.report(native_csv(on), thread_budget_ms=4.0)
    assert ": OVER" not in perf_report.report(native_csv(on), thread_budget_ms=4.0)
    assert "budget of 3.000 ms" in perf_report.report(native_csv(on), thread_budget_ms=3.0)
    # the main thread exactly on the budget is not over it: the lower bound may still fit
    edge = [(8333.0, 4000.0, 4000.0, 300.0, 100.0, 0.0)] * 5
    assert "main_us (lower bound) 4.000 ms: UNDECIDED" in perf_report.report(native_csv(edge))
    # a reference runner's log has no game-thread verdict (its columns are N0-15's)
    assert "Game thread" not in perf_report.report(make_csv([(8000.0, 1.0, 1.0, 1.0)] * 3))


def test_hunt_frames_read_the_scene_of_a_state_trace():
    trace = state_trace([DEMO, DEMO, HUNT, HUNT, DEMO], first_frame=10)
    assert perf_report.hunt_frames(trace) == {12, 13}
    assert perf_report.hunt_frames(trace, DEMO) == {10, 11, 14}
    assert perf_report.hunt_frames("# statetrace v1\nnot a record\n") == set()
    # a name that only contains `scene` does not count
    assert perf_report.hunt_frames("f=1 t=0 old_scene=0x00e655c8\n") == set()


def test_only_the_hunt_counts_and_skip_applies_to_it():
    rows = [(8333.0, 100.0, 100.0, 0.0, 0.0, 0.0)] * 10 + [(8333.0, 3000.0, 3000.0, 0.0, 0.0, 0.0)] * 20
    csv = native_csv(rows, first_frame=5001)  # frames 5001..5030; the hunt is frames 5011..5030
    trace = state_trace([DEMO] * 15 + [HUNT] * 20, first_frame=4996)  # the trace starts before the log
    frames = perf_report.hunt_frames(trace)
    text = perf_report.report(csv, frames=frames, min_frames=5)
    assert "Frames: 20" in text and "Hunt: 20 rows" in text
    assert "| main (ms) | 3.000 | 3.000 | 3.000 | 3.000 |" in text
    text = perf_report.report(csv, skip=5, frames=frames, min_frames=5)
    assert "Frames: 15 (the first 5 skipped)" in text
    # not enough of it left after the skip
    with pytest.raises(perf_report.NotCounted, match="20 rows, 15 after skipping 5: fewer than 16"):
        perf_report.report(csv, skip=5, frames=frames, min_frames=16)
    # the default asks for 20 s at 120 fps
    with pytest.raises(perf_report.NotCounted, match="fewer than 2400"):
        perf_report.report(csv, frames=frames)
    # a frame the poll missed: the row is not there, the hunt has one row fewer
    gap = "".join(line + "\n" for line in csv.splitlines() if not line.startswith("5020,"))
    assert "Hunt: 19 rows" in perf_report.report(gap, frames=frames, min_frames=5)


def test_a_state_trace_needs_the_native_log():
    # the reference runner's frame column is a vertical-blank index, not the f= of a state trace
    with pytest.raises(perf_report.FormatError, match="native"):
        perf_report.report(make_csv([(8000.0, 1, 1, 1)] * 30), frames={1, 2}, min_frames=1)


def test_a_native_log_without_its_clock_does_not_count():
    csv = native_csv([(8333.0, 100.0, 100.0, 0.0, 0.0, 0.0)] * 5)
    csv = "".join(line + "\n" for line in csv.splitlines() if not line.startswith("# perf"))
    with pytest.raises(perf_report.NotCounted, match="clock"):
        perf_report.report(csv)


def test_a_run_that_never_reached_the_hunt_does_not_count():
    csv = native_csv([(8333.0, 100.0, 100.0, 0.0, 0.0, 0.0)] * 20)
    frames = perf_report.hunt_frames(state_trace([DEMO] * 20))
    with pytest.raises(perf_report.NotCounted, match="never shows the hunt"):
        perf_report.report(csv, frames=frames, min_frames=1)
    # without a state trace every row counts: the report does not know the scene, and prints no Hunt line
    assert "Frames: 20" in perf_report.report(csv)
    assert "Hunt:" not in perf_report.report(csv)


def test_a_log_in_virtual_time_does_not_count():
    csv = native_csv([(8333.0, 100.0, 100.0, 0.0, 0.0, 0.0)] * 5, clock="virtual")
    with pytest.raises(perf_report.NotCounted, match="virtual time"):
        perf_report.report(csv)


def test_native_command_line(tmp_path, capsys):
    rows = [(8333.0, 100.0, 100.0, 0.0, 0.0, 0.0)] * 6 + [(8333.0, 1500.0, 2000.0, 400.0, 100.0, 4000.0)] * 14
    csv = tmp_path / "run.csv"
    csv.write_text(native_csv(rows))
    trace = tmp_path / "state.txt"
    trace.write_text(state_trace([DEMO] * 6 + [HUNT] * 14))
    args = [str(csv), "--state-trace", str(trace), "--skip", "4", "--min-frames", "10"]
    assert perf_report.main(args) == 0
    out = capsys.readouterr().out
    assert "Frames: 10 (the first 4 skipped)" in out
    assert "game_us (upper bound) 2.000 ms, main_us (lower bound) 1.500 ms: WITHIN" in out
    # too few rows left: the exit status is 1 and the table is not printed
    assert perf_report.main([*args[:-1], "11"]) == 1
    out = capsys.readouterr().out
    assert out.startswith("NOT COUNTED: ") and "| column |" not in out
    # the scene can be named (in hex or decimal); naming the demo leaves 2 rows after --skip 4: fewer than 10
    assert perf_report.main([*args, "--hunt-scene", "0x00e64d5c"]) == 1
    assert "fewer than 10" in capsys.readouterr().out
    assert perf_report.main([*args, "--hunt-scene", "15095240"]) == 0  # 0x00e655c8, the hunt, in decimal
    capsys.readouterr()
    assert perf_report.main([*args, "--thread-budget-ms", "inf"]) == 2
    capsys.readouterr()
    assert perf_report.main([*args, "--hunt-scene", "banana"]) == 2
    assert perf_report.main([*args[:-1], "0"]) == 2
    assert perf_report.main([*args, "--thread-budget-ms", "0"]) == 2
    assert perf_report.main([str(csv), "--state-trace", str(tmp_path / "missing.txt")]) == 2
    capsys.readouterr()
    # a virtual-time log: exit 1 as well
    virtual = tmp_path / "det.csv"
    virtual.write_text(native_csv(rows, clock="virtual"))
    assert perf_report.main([str(virtual)]) == 1
    assert "virtual time" in capsys.readouterr().out
    # the budget option moves the verdict
    assert perf_report.main([*args, "--thread-budget-ms", "1.9"]) == 0
    assert "budget of 1.900 ms" in capsys.readouterr().out
