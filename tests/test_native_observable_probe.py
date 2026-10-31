"""Tests of tools/native/observable_probe.py (N3-21): synthetic sessions, state traces and memory dumps only."""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "native"))
import observable_probe as op  # noqa: E402
import state_diff  # noqa: E402

TOOL = ROOT / "tools" / "native" / "observable_probe.py"

SESSION = """\
# name: S
# needs: none
# frames: 1000
# checkpoints: 100:0x00e64d5c, 600:0x00e655c8
# captures: gpu:700:10
# init-time: 1767268800.5
# a comment about frames 100 to 200
f100:A:f6,f150:TOUCH:290:195:f6
f300:CIRCLE:0.5:-0.5:f100,f700:CIRCLE:1:0:f100   # walk
f900:B
f950:A:250
"""


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def run(*args: object) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)


# -- scale ---------------------------------------------------------------------------------------------------

def test_scale_multiplies_frame_keys_holds_checkpoints_and_total():
    out = op.scale_session(SESSION, 2)
    lines = out.splitlines()
    assert "# name: S-x2" in lines
    assert "# frames: 2000" in lines
    assert "# checkpoints: 200:0x00e64d5c, 1200:0x00e655c8" in lines
    assert "f200:A:f12,f300:TOUCH:290:195:f12" in lines          # coordinates are pixels, not frames
    assert "f600:CIRCLE:0.5:-0.5:f200,f1400:CIRCLE:1:0:f200 # walk" in lines
    assert "f1800:B" in lines
    assert "f1900:A:250" in lines                                   # a hold in milliseconds stays
    assert "# captures: gpu:700:10" in lines                        # not a frame key of the script
    assert "# init-time: 1767268800.5" in lines
    assert "# a comment about frames 100 to 200" in lines
    assert out.endswith("\n") and "\r" not in out


def test_scale_factor_one_changes_only_the_name_and_rejects_zero():
    out = op.scale_session(SESSION, 1)
    assert out.replace("-x1", "") == SESSION.replace("f300:CIRCLE:0.5:-0.5:f100,f700:CIRCLE:1:0:f100   # walk",
                                                      "f300:CIRCLE:0.5:-0.5:f100,f700:CIRCLE:1:0:f100 # walk")
    with pytest.raises(op.UsageError):
        op.scale_session(SESSION, 0)


def test_scale_also_scales_the_hit_marks_and_composes():
    text = SESSION + "# hit: 120, 640\n"
    twice = op.scale_session(op.scale_session(text, 2), 2)
    assert "# hit: 480, 2560" in twice.splitlines()
    assert "# name: S-x2-x2" in twice.splitlines()
    assert "# frames: 4000" in twice.splitlines()


def test_scale_command_writes_the_file(tmp_path):
    src, dst = tmp_path / "S.txt", tmp_path / "S-x2.txt"
    src.write_text(SESSION)
    done = run("scale", src, "--factor", 2, "-o", dst)
    assert done.returncode == 0, done.stderr
    assert dst.read_text() == op.scale_session(SESSION, 2)
    assert run("scale", src, "--factor", 0, "-o", dst).returncode == 2


def test_scaled_session_parses_with_twice_the_frames():
    base = op.parse_session(SESSION)
    scaled = op.parse_session(op.scale_session(SESSION, 2))
    assert scaled.factor == 2 and scaled.frames == 2 * base.frames
    assert [(p.kind, p.start * 1, p.hold) for p in base.presses if p.kind == "CIRCLE"] == [("CIRCLE", 300, 100), ("CIRCLE", 700, 100)]
    assert [(p.start, p.hold) for p in scaled.presses if p.kind == "CIRCLE"] == [(600, 200), (1400, 200)]


# -- traces --------------------------------------------------------------------------------------------------

HUNT_START, HUNT_END = 200, 900
WALKS = ((300, 400), (700, 800))                    # [start, end) of the circle pad held, as in SESSION


def position_at(frame: int, still_noise: bool = False) -> tuple[float, float, float]:
    x = 100.0
    for s, e in WALKS:
        x += 2.5 * max(0, min(frame, e) - s)
    return (x, 5.0, -40.0 + (0.001 * frame if still_noise else 0.0))


def trace_text(position=position_at, quest=None, scale: int = 1, scene_changes=((1, 0x10), (HUNT_START, 0x20))) -> str:
    lines = ["# statetrace v1 2026-10-30 game=0x0004000000126300 cpu=recomp new3ds=off"]
    for f in range(0, HUNT_END * scale + 1):
        scene = [s for at, s in scene_changes if at * scale <= f]
        row = f"f={f} t={f * 4_470_000} scene={'-' if not scene else hex(scene[-1])}"
        x, y, z = position(f // scale)
        row += f" hunter_x=0x{bits(x):08x} hunter_y=0x{bits(y):08x} hunter_z=0x{bits(z):08x}"
        if quest is not None:
            row += f" quest_time=0x{quest(f, scale):08x}"
        lines.append(row)
    return "\n".join(lines) + "\n"


def set_value(text: str, frame: int, name: str, value: str) -> str:
    out = []
    for line in text.splitlines():
        if line.startswith(f"f={frame} "):
            line = " ".join(f"{name}={value}" if w.startswith(name + "=") else w for w in line.split())
        out.append(line)
    return "\n".join(out) + "\n"


def records(text: str):
    return state_diff.parse_trace(text)


def check_position(text: str, session: str = SESSION, fps: int = 60, **kw):
    return op.validate_position(records(text), op.parse_session(session, fps), op.DEFAULT_POSITION, fps, **kw)


def test_position_valid_when_constant_between_walks_and_changing_during_them():
    ok, why, details = check_position(trace_text())
    assert ok and why == "valid", why
    assert any("hunt frames 200 to 900" in d for d in details)


def test_position_valid_on_the_scaled_session_at_120_fps():
    scaled = op.scale_session(SESSION, 2)
    ok, why, _ = check_position(trace_text(scale=2), scaled, 120)
    assert ok, why


def test_position_invalid_when_it_moves_without_input():
    def drifting(frame):
        x, y, z = position_at(frame)
        return (x + (0.5 if frame >= 250 else 0.0), y, z)

    ok, why, _ = check_position(trace_text(drifting))
    assert not ok and "without movement input" in why and "stretch 200..299" in why


def test_position_invalid_when_one_value_wobbles_in_a_still_stretch():
    ok, why, _ = check_position(trace_text(lambda f: position_at(f, still_noise=True)))
    assert not ok and "without movement input" in why


def test_position_invalid_when_it_ignores_the_circle_pad():
    ok, why, _ = check_position(trace_text(lambda f: (100.0, 5.0, -40.0)))
    assert not ok and "does not change while the circle pad is held" in why


def test_position_a_change_in_one_component_is_enough_while_walking():
    ok, why, _ = check_position(trace_text(lambda f: (100.0, 5.0, -40.0 + position_at(f)[0])))
    assert ok, why


def test_position_invalid_on_magnitude_nan_and_unreadable():
    ok, why, _ = check_position(trace_text(lambda f: (position_at(f)[0] * 1000.0, 5.0, -40.0)))
    assert not ok and "under 100000" in why
    ok, why, _ = check_position(set_value(trace_text(), 500, "hunter_y", "0x7fc00000"))
    assert not ok and "hunter_y" in why and "frame 500" in why
    ok, why, _ = check_position(set_value(trace_text(), 500, "hunter_x", "-"))
    assert not ok and "unreadable at frame 500" in why


def test_position_hunt_starts_where_the_values_become_readable():
    lines = trace_text().splitlines()
    for i, line in enumerate(lines):
        if line.startswith("f=") and 200 <= int(line.split()[0][2:]) < 240:
            lines[i] = " ".join(w if not w.startswith("hunter") else w.split("=")[0] + "=-" for w in line.split())
    ok, why, details = check_position("\n".join(lines))
    assert ok, why
    assert any("hunt frames 240 to 900" in d for d in details)


def test_position_nothing_compared_is_not_valid():
    no_walk = "# name: S\n# frames: 1000\nf100:A:f6\n"
    ok, why, _ = check_position(trace_text(lambda f: (1.0, 2.0, 3.0)), no_walk)
    assert not ok and "no stretch with the circle pad held" in why
    short = "# name: S\n# frames: 1000\nf300:CIRCLE:1:0:f100\nf400:CIRCLE:1:0:f100\nf500:CIRCLE:1:0:f100\n" \
            "f600:CIRCLE:1:0:f100\nf700:CIRCLE:1:0:f100\nf800:CIRCLE:1:0:f100\nf200:CIRCLE:1:0:f100\n"
    ok, why, _ = check_position(trace_text(), short)
    assert not ok and "no stretch of 60 frames" in why


def test_position_short_stretches_are_not_compared():
    # a 30-frame stretch between two walks may change (a settling hunter); stretches under 60 frames are skipped
    session = "# name: S\n# frames: 1000\nf300:CIRCLE:1:0:f100\nf430:CIRCLE:1:0:f100\nf700:CIRCLE:1:0:f100\n"

    def settling(frame):
        x = 100.0 + 2.5 * max(0, min(frame, 400) - 300) + 2.5 * max(0, min(frame, 530) - 430)
        x += 2.5 * max(0, min(frame, 800) - 700)
        return (x + (0.1 * (frame - 400) if 400 <= frame < 430 else 0.0), 5.0, -40.0)

    ok, why, _ = check_position(trace_text(settling), session)
    assert ok, why


def test_position_hit_marks_leave_a_stretch_out():
    drifting = lambda f: (position_at(f)[0] + (3.0 if f >= 850 else 0.0), 5.0, -40.0)   # noqa: E731
    ok, why, _ = check_position(trace_text(drifting))
    assert not ok and "stretch 800..900" in why
    ok, why, details = check_position(trace_text(drifting), SESSION + "# hit: 880\n")
    assert ok, why
    assert any("left out (a hit taken)" in d for d in details)


def test_position_settle_skips_the_start_of_each_stretch():
    def sliding(frame):
        x, y, z = position_at(frame)
        return (x + (1.0 if 300 + 100 <= frame < 405 else 0.0) * (frame - 400), y, z)

    ok, _, _ = check_position(trace_text(sliding))
    assert not ok
    ok, why, _ = check_position(trace_text(sliding), settle=6)
    assert ok, why


def test_position_needs_three_names_and_the_columns():
    with pytest.raises(op.UsageError):
        op.validate_position(records(trace_text()), op.parse_session(SESSION), ("a", "b"), 60)
    with pytest.raises(op.UsageError):
        op.validate_position(records(trace_text()), op.parse_session(SESSION), ("p", "q", "r"), 60)


# an attack press (A at f820, after the second walk) makes the hunter move by itself from f823 on: not circle-pad input
ATTACK_SESSION = SESSION + "f820:A:f6\n"
ATTACK_FRAME, FIRST_MOVE = 820, 823


def swinging(frame):
    x, y, z = position_at(frame)
    return (x + (0.5 * min(frame - FIRST_MOVE + 1, 40) if frame >= FIRST_MOVE else 0.0), y, z)


def test_position_restricted_to_the_part_before_the_first_attack_press():
    text = trace_text(swinging)
    ok, why, _ = check_position(text, ATTACK_SESSION)                  # the whole hunt: the swing is a move without input
    assert not ok and "without movement input" in why and f"first at frame {FIRST_MOVE}" in why
    ok, why, details = check_position(text, ATTACK_SESSION, end=FIRST_MOVE - 1)
    assert ok, why
    assert any("hunt frames 200 to 822" in d for d in details)


def test_position_command_prints_the_restriction_it_used(tmp_path):
    session, trace = tmp_path / "S.txt", tmp_path / "t.txt"
    session.write_text(ATTACK_SESSION)
    trace.write_text(trace_text(swinging))
    done = run("validate", "--trace", trace, "--session", session, "--rule", "position")
    assert done.returncode == 1 and done.stdout.startswith("invalid: the position changes inside the stretch")
    assert "restricted" not in done.stdout                              # nothing was restricted
    done = run("validate", "--trace", trace, "--session", session, "--rule", "position", "--to", FIRST_MOVE - 1)
    assert done.returncode == 0 and done.stdout == f"valid (hunt restricted by --to {FIRST_MOVE - 1})\n", done.stdout + done.stderr
    done = run("validate", "--trace", trace, "--session", session, "--rule", "position", "--from", 210, "--to", 822)
    assert done.stdout == "valid (hunt restricted by --from 210 --to 822)\n", done.stdout + done.stderr
    # the restriction is named in a refusal too
    done = run("validate", "--trace", trace, "--session", session, "--rule", "position", "--to", 880)
    assert done.returncode == 1 and done.stdout.rstrip().endswith("(hunt restricted by --to 880)"), done.stdout


def test_position_scaled_session_restricted_at_twice_the_frame():
    scaled = op.scale_session(ATTACK_SESSION, 2)

    text = trace_text(swinging, scale=2)                               # the position follows the 60 fps frame, f // scale
    ok, why, _ = check_position(text, scaled, 120)
    assert not ok and "without movement input" in why
    ok, why, _ = check_position(text, scaled, 120, end=2 * FIRST_MOVE - 1)
    assert ok, why


# -- quest_time ----------------------------------------------------------------------------------------------

def counter(rate_per_second: float, offset: int = 1000):
    return lambda f, scale: offset + int(rate_per_second * (f / (60 * scale)))


def check_quest(text: str, fps: int = 60, **kw):
    return op.validate_quest_time(records(text), op.DEFAULT_QUEST_TIME, fps, **kw)


def test_quest_time_valid_for_a_linear_counter_at_both_rates():
    ok, why, details = check_quest(trace_text(quest=counter(30)))
    assert ok and why == "valid (running stretches 1, pauses 0)", why
    assert any("rate 30" in d for d in details)
    ok, why, details = check_quest(trace_text(quest=counter(30), scale=2), 120)
    assert ok, why
    assert any("rate 30" in d for d in details)


def countdown(runs):
    """Counts down 60 per second of game time, but only inside the (start s, end s) ranges of `runs`."""
    def value(f, scale):
        t = f / (60.0 * scale)
        return 100000 - int(60 * sum(max(0.0, min(t, b) - a) for a, b in runs))
    return value


# game seconds of the synthetic hunt: 200/60 = 3.33 to 900/60 = 15
PAUSING = countdown([(0.0, 4.0), (7.0, 13.0)])         # runs 3.33..4 (a blip), pauses, runs 7..13, pauses to the end


def test_quest_time_a_countdown_with_pauses_is_valid_and_the_pauses_are_reported():
    for scale, fps in ((1, 60), (2, 120)):
        ok, why, details = check_quest(trace_text(quest=PAUSING, scale=scale), fps)
        assert ok and why == "valid (running stretches 1, pauses 2)", (scale, why, details)
        assert sum(1 for d in details if d.startswith("pause frames")) == 2
        running = [d for d in details if d.startswith("running frames")]
        assert len(running) == 1 and "rate -60" in running[0], running      # the rate is printed per stretch
        assert any("1 running stretch(es) shorter than 5 s ignored" in d for d in details)


def test_quest_time_does_not_assert_the_rate_between_frame_rates():
    # a counter that adds one per frame is 60 per second at 60 fps and 120 per second at 120 fps: both are reported,
    # and the comparison between them is the audit's
    per_frame = lambda f, scale: 1000 + f   # noqa: E731
    ok60, _, d60 = check_quest(trace_text(quest=per_frame))
    ok120, _, d120 = check_quest(trace_text(quest=per_frame, scale=2), 120)
    assert ok60 and ok120
    assert any("rate 60" in d for d in d60) and any("rate 120" in d for d in d120)


def test_quest_time_a_stretch_that_curves_by_more_than_one_percent_is_invalid():
    # 60 per second for 3.5 s and 90 per second for 3.5 s inside one running stretch: 10 % off its straight line
    def curved(f, scale):
        t = f / (60.0 * scale)
        return 100000 - int(60 * max(0.0, min(t, 7.0) - 4.0) + 90 * max(0.0, min(t, 10.5) - 7.0)) if t >= 4.0 else 100000

    for scale, fps in ((1, 60), (2, 120)):
        ok, why, details = check_quest(trace_text(quest=curved, scale=scale), fps)
        assert not ok and "leaves the straight line of its running stretch" in why and "the limit is 1 %" in why, (why, details)


def test_quest_time_a_curve_inside_a_stretch_shorter_than_the_minimum_is_ignored():
    # 1.7 s of a curved blip (60 then 120 per second), a pause, then a straight 7 s run
    def blip_then_line(f, scale):
        t = f / (60.0 * scale)
        blip = 60 * max(0.0, min(t, 4.2) - 3.3) + 120 * max(0.0, min(t, 5.0) - 4.2)
        line = 60 * max(0.0, min(t, 14.0) - 7.0)
        return 100000 - int(blip + line)

    ok, why, details = check_quest(trace_text(quest=blip_then_line))
    assert ok, (why, details)
    assert any("shorter than 5 s ignored" in d for d in details)


def test_quest_time_with_no_stretch_of_the_minimum_length_is_invalid():
    ok, why, _ = check_quest(trace_text(quest=countdown([(0.0, 7.0)])))     # runs 3.33..7: 3.7 s only
    assert not ok and "no running stretch of 5 s" in why
    ok, why, _ = check_quest(trace_text(quest=countdown([(0.0, 8.4)])))     # 5.1 s: long enough
    assert ok, why
    assert op.MIN_RUNNING_SECONDS == 5.0 and op.RATE_TOLERANCE == 0.01 and op.MIN_IDLE_SECONDS == 1.0
    assert op.MAGNITUDE_LIMIT == 100000.0


def test_quest_time_two_pauses_separated_by_a_blip_are_one_pause():
    stretches = op.quest_stretches(
        [op.Segment(False, 0, 100, 1.7, 0.0, 0.0), op.Segment(True, 100, 160, 1.0, -60.0, 0.0),
         op.Segment(False, 160, 400, 4.0, 0.0, 0.0), op.Segment(True, 400, 1000, 10.0, -60.0, 0.0),
         op.Segment(False, 1000, 1200, 3.3, 0.0, 0.0)], 60)
    running, pauses, ignored = stretches
    assert [(p.first, p.last) for p in pauses] == [(0, 400), (1000, 1200)]
    assert [(s.first, s.last) for s in running] == [(400, 1000)] and len(ignored) == 1


def test_quest_time_invalid_when_constant_unreadable_or_too_coarse():
    ok, why, _ = check_quest(trace_text(quest=lambda f, s: 77))
    assert not ok and "no running stretch" in why
    ok, why, _ = check_quest(set_value(trace_text(quest=counter(30)), 600, "quest_time", "-"))
    assert not ok and "unreadable" in why
    ok, why, _ = check_quest(trace_text(quest=counter(0.1)))      # whole steps about every 10 s: it never runs for a second
    assert not ok and "no running stretch" in why


def test_quest_time_float_counter():
    def seconds(f, scale):
        return bits(2.5 + f / (60.0 * scale))

    ok, why, details = check_quest(trace_text(quest=seconds), as_float=True)
    assert ok, why
    assert any("rate 1" in d for d in details)
    ok, why, _ = check_quest(trace_text(quest=lambda f, s: 0x7F800000), as_float=True)
    assert not ok


def pausing_counter(f, scale):
    """Counts down 60 per second of game time, except between game seconds 4 and 7 and from 12 on."""
    t = f / (60.0 * scale)
    running = min(t, 4) + max(0.0, min(t, 12) - 7)
    return 100000 - int(60 * running)


def test_segments_cut_a_pausing_counter_into_running_and_paused_stretches():
    for scale, fps in ((1, 60), (2, 120)):
        recs = records(trace_text(quest=pausing_counter, scale=scale))
        segs = op.quest_segments(recs, "quest_time", fps)
        # the hunt starts at game second 200/60 = 3.33: running to 4, paused 4 to 7, running 7 to 12, paused to 15
        assert [s.running for s in segs] == [True, False, True, False], segs
        assert abs(segs[0].first - 200 * scale) <= 2 and abs(segs[0].seconds - 0.67) < 0.04
        assert abs(segs[2].seconds - 5.0) < 0.01 and abs(segs[2].rate - (-60.0)) < 0.5
        assert segs[2].deviation <= 1.5 / 300           # the counter is an integer: one unit of 300
        assert segs[1].rate == 0.0


def test_segments_command(tmp_path):
    session, trace = tmp_path / "S.txt", tmp_path / "t.txt"
    session.write_text(SESSION)
    trace.write_text(trace_text(quest=pausing_counter))
    done = run("segments", "--trace", trace, "--session", session)
    assert done.returncode == 0, done.stderr
    lines = done.stdout.splitlines()
    assert [ln.split()[0] for ln in lines] == ["running", "paused", "running", "paused"]
    assert "rate -60" in lines[2]
    assert run("segments", "--trace", trace, "--session", session, "--name", "nope").returncode == 2


# -- the command line ----------------------------------------------------------------------------------------

def test_validate_command_prints_valid_or_why_and_derives_fps_from_the_name(tmp_path):
    session = tmp_path / "S.txt"
    trace = tmp_path / "t.txt"
    session.write_text(SESSION)
    trace.write_text(trace_text())
    done = run("validate", "--trace", trace, "--session", session, "--rule", "position")
    assert done.returncode == 0 and done.stdout == "valid\n", done.stdout + done.stderr
    bad = tmp_path / "bad.txt"
    bad.write_text(trace_text(lambda f: (100.0, 5.0, -40.0)))
    done = run("validate", "--trace", bad, "--session", session, "--rule", "position")
    assert done.returncode == 1 and done.stdout.startswith("invalid: the position does not change")
    scaled = tmp_path / "S-x2.txt"
    scaled.write_text(op.scale_session(SESSION, 2))
    trace.write_text(trace_text(quest=counter(30), scale=2))
    done = run("validate", "--trace", trace, "--session", scaled, "--rule", "quest_time")
    assert done.returncode == 0 and "rate 30" in done.stderr, done.stdout + done.stderr
    done = run("validate", "--trace", trace, "--session", session, "--rule", "quest_time")
    assert done.returncode == 0 and "rate 15" in done.stderr         # read as 60 fps the counter runs at 15 per second: printed, not judged
    done = run("validate", "--trace", trace, "--session", session, "--rule", "quest_time", "--expect-rate", 30)
    assert done.returncode == 2                                     # the rate is not asserted by the tool any more
    done = run("validate", "--trace", tmp_path / "missing.txt", "--session", session, "--rule", "position")
    assert done.returncode == 2
    done = run("validate", "--trace", trace, "--session", session, "--rule", "position", "--names", "a,b,c")
    assert done.returncode == 2 and "no a= value" in done.stderr


# -- memory dumps ---------------------------------------------------------------------------------------------

BASE = 0x08000000
WORDS = 4096          # one block


def write_dump(folder: Path, frame: int, patches: dict[int, int], base: int = BASE) -> None:
    data = bytearray(WORDS * 4 * 2)
    for index, value in patches.items():
        struct.pack_into("<I", data, index * 4, value)
    (folder / f"mem_f{frame:08d}_{base:08x}.bin").write_bytes(bytes(data))


def make_walk_dumps(folder: Path) -> None:
    still_a, still_b, walking = (100, 200), (900, 950), (300, 400, 500, 600)
    decoys = {1000: bits(1.0), 1001: bits(2.0), 1002: bits(3.0)}                    # never moves
    for f in still_a + still_b + walking:
        x = 10.0 + 2.0 * (min(max(f, 250), 700) - 250)
        patches = dict(decoys)
        patches.update({50: bits(x), 51: bits(7.5), 52: bits(-3.25)})              # the hunter: y is flat ground
        patches.update({70: bits(x + (f % 7)), 71: bits(1.0), 72: bits(2.0)})        # moves but jitters when still
        patches.update({90: bits(x * 1e7), 91: bits(1.0), 92: bits(2.0)})            # moves, out of range
        patches.update({110: bits(x * (1.0 + 0.5 * (f == 400))), 111: bits(0.0), 112: bits(0.0)})   # uneven speed
        patches[3000] = 5000 + f                                                  # a frame counter
        patches[3001] = 100 + (f - 100) // 2 if f < 700 else 100 + 300      # stops at 700
        patches[3002] = bits(f / 60.0)                                            # a timer in seconds
        write_dump(folder, f, patches)


def test_find_position_finds_the_flat_ground_triple_and_ranks_by_steady_speed(tmp_path, capsys):
    make_walk_dumps(tmp_path)
    done = run("find-position", tmp_path, "--base", hex(BASE), "--still", "100,200;900,950", "--moving", "300,400,500,600")
    assert done.returncode == 0, done.stderr
    lines = done.stdout.splitlines()
    assert lines[0] == "6 candidate triples", done.stdout           # the three windows over words 48..52 and over 110..112
    hunter = [ln for ln in lines if ln.startswith(f"0x{BASE + 50 * 4:08x}")]
    assert hunter and "spread 0.0000" in hunter[0] and "first (110.000, 7.500, -3.250)" in hunter[0]
    assert "last (710.000, 7.500, -3.250)" in hunter[0]
    assert not any(ln.startswith(f"0x{BASE + a * 4:08x}") for a in (70, 90) for ln in lines)   # jitter, out of range
    uneven = [ln for ln in lines if ln.startswith(f"0x{BASE + 110 * 4:08x}")]
    assert uneven and "spread 0.7750" in uneven[0]                   # the speed changes by a lot between two dumps


def test_find_position_uneven_speed_ranks_below_steady_and_still_group_is_required(tmp_path):
    make_walk_dumps(tmp_path)
    dumps = op.load_dumps(str(tmp_path), BASE, [100, 200, 900, 950, 300, 400, 500, 600])
    try:
        # without the still groups' words the jittering triple has the same spread as the steady one is irrelevant:
        # it is dropped by the still check
        found = op.find_position(dumps, [[100, 200]], [300, 400, 500, 600])
        starts = {j for j, _, _ in found}
        assert 50 in starts and 70 not in starts and 90 not in starts
        found = op.find_position(dumps, [], [300, 400, 500, 600])
        by_start = {j: spread for j, _, spread in found}
        assert by_start[50] < by_start.get(110, 99) or 110 not in by_start
        assert 70 in by_start                                           # no still groups: the jitter passes
    finally:
        for d in dumps.values():
            d.close()


def test_find_position_needs_two_moving_dumps(tmp_path):
    make_walk_dumps(tmp_path)
    done = run("find-position", tmp_path, "--base", hex(BASE), "--still", "100,200", "--moving", "300")
    assert done.returncode == 2 and "at least two" in done.stderr
    done = run("find-position", tmp_path, "--base", hex(BASE), "--still", "100,200", "--moving", "300,301")
    assert done.returncode == 2 and "frame(s) 301" in done.stderr


def test_find_counter_lists_only_linear_words(tmp_path):
    make_walk_dumps(tmp_path)
    frames = "100,200,300,400,500,600"
    done = run("find-counter", tmp_path, "--base", hex(BASE), "--frames", frames)
    assert done.returncode == 0, done.stderr
    assert f"0x{BASE + 3000 * 4:08x}" in done.stdout and "per frame 1" in done.stdout
    assert f"0x{BASE + 3001 * 4:08x}" in done.stdout              # linear up to 600
    assert f"0x{BASE + 50 * 4:08x}" not in done.stdout            # the position stops moving before the end
    done = run("find-counter", tmp_path, "--base", hex(BASE), "--frames", "100,200,300,400,500,600,900", "--tolerance", "0.001")
    assert f"0x{BASE + 3001 * 4:08x}" not in done.stdout          # it stopped at 700
    done = run("find-counter", tmp_path, "--base", hex(BASE), "--frames", frames, "--float")
    assert f"0x{BASE + 3002 * 4:08x}" in done.stdout and "per frame 0.0166667" in done.stdout


def test_find_monotone_finds_a_countdown_that_pauses(tmp_path):
    frames = list(range(100, 1100, 100))
    for f in frames:
        write_dump(tmp_path, f, {20: 108000 - (max(0, min(f, 500) - 200) + max(0, f - 700)),     # pauses 500..700
                                 21: 7 * (f // 300),                                              # moves in 3 gaps
                                 22: f * 3 % 1000,                                                # turns back
                                 23: 5 + (f > 400)})                                              # moves once
    arg = ",".join(map(str, frames))
    done = run("find-monotone", tmp_path, "--base", hex(BASE), "--frames", arg, "--min-moving", 5)
    assert done.returncode == 0, done.stderr
    assert done.stdout.splitlines()[0] == "1 monotone words"
    assert f"0x{BASE + 20 * 4:08x}  down  moves in 6 of 9 gaps  first 108000  last 107400" in done.stdout, done.stdout
    both = run("find-monotone", tmp_path, "--base", hex(BASE), "--frames", arg, "--min-moving", 3)
    assert both.stdout.splitlines()[0] == "2 monotone words"            # the counter that moves in 3 gaps as well
    only_down = run("find-monotone", tmp_path, "--base", hex(BASE), "--frames", arg, "--min-moving", 3, "--down")
    assert only_down.stdout.splitlines()[0] == "1 monotone words"
    assert run("find-monotone", tmp_path, "--base", hex(BASE), "--frames", arg, "--min-moving", 50).returncode == 1


def test_words_command_prints_the_values_of_each_dump(tmp_path):
    make_walk_dumps(tmp_path)
    done = run("words", tmp_path, "--base", hex(BASE), "--address", f"0x{BASE + 51 * 4:08x},0x{BASE + 3000 * 4:08x}")
    assert done.returncode == 0, done.stderr
    assert "f300: 0x40f00000 1089470464 7.5 | 0x00001388" not in done.stdout    # f300 is 5300
    assert "f300: 0x40f00000 1089470464 7.5 | 0x000014b4 5300" in done.stdout
    done = run("words", tmp_path, "--base", hex(BASE), "--address", "0x10")
    assert done.returncode == 2


def test_changed_words_skips_equal_blocks(tmp_path):
    write_dump(tmp_path, 1, {10: 1, 5000: 2})
    write_dump(tmp_path, 2, {10: 1, 5000: 3, 7000: 4})
    dumps = op.load_dumps(str(tmp_path), BASE, [1, 2])
    try:
        assert op.changed_words(dumps[1], dumps[2]) == [5000, 7000]
    finally:
        for d in dumps.values():
            d.close()
