"""tools/native/state_diff.py (N0-07): synthetic state traces only, with made-up values."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "native"))

import state_diff  # noqa: E402

OBSERVABLE = """\
# a comment
[[0x00001000]+0x30] 1 scene eq
0x00002000 4 frame_counter eq
[[0x00003000]+0x5c] 4 delta ignore
[[0x00004000]+0x10+0x8] 2 quest eq
"""

HEADER = "# statetrace v1 2026-01-01 game=0x0000000000000000 cpu=jit new3ds=off\n"


def trace(rows: list[tuple[int, int, str, int, int]], header: bool = True) -> str:
    """rows of (frame, scene, quest, delta, ...) -> a trace text. quest is a ready value."""
    out = HEADER if header else ""
    for frame, scene, quest, delta, _ in rows:
        out += (f"f={frame} t={frame * 4470000} scene=0x{scene:02x} frame_counter=0x{frame:08x} "
                f"delta=0x{delta:08x} quest={quest}\n")
    return out


def base(shift: int = 0, quest_at_3: str = "0x0001", delta: int = 0x3F800000):
    # scene 1 from frame 1, scene 2 from 100+shift, scene 3 from 300+shift
    rows = []
    for frame in range(1, 400):
        scene = 1 if frame < 100 + shift else 2 if frame < 300 + shift else 3
        quest = quest_at_3 if scene == 3 else "-"
        rows.append((frame, scene, quest, delta, 0))
    return trace(rows)


@pytest.fixture()
def observables():
    return state_diff.parse_observable(OBSERVABLE)


def run(ref: str, cand: str, observables, **kw):
    return state_diff.diff(state_diff.parse_trace(ref), state_diff.parse_trace(cand), observables, **kw)


def test_equal(observables):
    assert run(base(), base(), observables) is None


def test_shift_within_tolerance(observables):
    assert run(base(), base(shift=1), observables) is None
    assert run(base(), base(shift=2), observables) is None


def test_shift_beyond_tolerance_only_with_order_only(observables):
    problem = run(base(), base(shift=5), observables)
    assert problem and "more than 2 apart" in problem
    assert run(base(), base(shift=5), observables, order_only=True) is None
    assert run(base(), base(shift=5), observables, tolerance=5) is None


def test_different_scene_order(observables):
    swapped = base().replace("scene=0x02", "scene=0x09").replace("scene=0x03", "scene=0x02") \
        .replace("scene=0x09", "scene=0x03")
    for order_only in (False, True):
        problem = run(base(), swapped, observables, order_only=order_only)
        assert problem and "scene change #1" in problem and "scene 0x2" in problem


def test_missing_and_extra_scene_change(observables):
    short = "".join(line for line in base().splitlines(True) if "scene=0x03" not in line)
    assert "candidate has no further change" in run(base(), short, observables)
    assert "reference has no further change" in run(short, base(), observables)


def test_null_chain_is_dash_and_compared(observables):
    # the quest id is '-' (null chain) before scene 3 in both: still equal
    assert "quest=-" in base()
    assert run(base(), base(), observables) is None
    # '-' against a value at a scene change is a divergence
    cand = base().replace("quest=0x0001", "quest=-")
    problem = run(base(), cand, observables)
    assert problem and "quest is 0x1 in the reference, - in the candidate" in problem


def test_ignore_value_may_differ(observables):
    assert run(base(), base(delta=0x40000000), observables) is None


def test_eq_value_differs(observables):
    problem = run(base(), base(quest_at_3="0x0002"), observables)
    assert problem and "quest is 0x1" in problem and "0x2 in the candidate" in problem
    # order-only does not look at it
    assert run(base(), base(quest_at_3="0x0002"), observables, order_only=True) is None


def test_trace_without_an_observable_is_an_error(observables):
    stripped = base().replace(" quest=", " other=")
    with pytest.raises(state_diff.FormatError, match="no quest="):
        run(base(), stripped, observables)


def test_parse_skips_comments_and_rejects_garbage():
    records = state_diff.parse_trace("# x\n\nf=0x10 t=5 scene=0x01 quest=-\n")
    assert records[0].frame == 16 and records[0].values == {"scene": 1, "quest": None}
    for bad in ("f=1 scene=0x01\n", "f=1 t=2 scene\n", "f=1 t=2 scene=zz\n"):
        with pytest.raises(state_diff.FormatError):
            state_diff.parse_trace(bad)


def test_observable_file_syntax():
    parsed = state_diff.parse_observable(OBSERVABLE)
    assert [(o.name, o.size, o.compare) for o in parsed] == [
        ("scene", 1, True), ("frame_counter", 4, True), ("delta", 4, False), ("quest", 2, True)]
    for bad in ("0x10 3 a eq", "0x10 4 a maybe", "0x10 4 eq", "[0x10 4 a eq", "[[0x10]+z] 4 a eq",
                "0x10 4 a eq\n0x20 4 a eq"):
        with pytest.raises(state_diff.FormatError):
            state_diff.parse_observable(bad)


def test_the_shipped_observable_file_parses():
    parsed = state_diff.parse_observable((ROOT / "config" / "native" / "observable.txt").read_text())
    names = [o.name for o in parsed]
    assert names[:2] == ["scene", "frame_counter"] and len(set(names)) == len(names)
    by_name = {o.name: o for o in parsed}
    assert by_name["smain_delta"].compare is False and by_name["scene"].size == 1


def test_expression_evaluation():
    memory = {0x1000: 0x5000, 0x2000: 0x77, 0x5030: 0xAB, 0x3000: 0, 0x4000: 0x6000, 0x6018: 0x1234}

    def read(address, size):
        return memory.get(address)

    assert state_diff.evaluate("0x00002000", 4, read) == 0x77
    assert state_diff.evaluate("[[0x00001000]+0x30]", 1, read) == 0xAB
    assert state_diff.evaluate("[[0x00004000]+0x10+0x8]", 2, read) == 0x1234
    assert state_diff.evaluate("[0x00002000]", 4, read) == 0x77  # bare brackets: the same read
    # a null pointer anywhere in the chain
    assert state_diff.evaluate("[[0x00003000]+0x5c]", 4, read) is None
    # an unreadable pointer
    assert state_diff.evaluate("[[0x00009000]+0x5c]", 4, read) is None


def test_command_line(tmp_path, capsys):
    obs = tmp_path / "obs.txt"
    obs.write_text(OBSERVABLE)
    ref, same, shifted, bad = (tmp_path / n for n in ("ref", "same", "shifted", "bad"))
    ref.write_text(base())
    same.write_text(base(shift=1))
    shifted.write_text(base(shift=5))
    bad.write_text("garbage\n")
    args = ["--observable", str(obs)]
    assert state_diff.main([str(ref), str(same)] + args) == 0
    assert "traces agree" in capsys.readouterr().out
    assert state_diff.main([str(ref), str(shifted)] + args) == 1
    assert "DIVERGENCE" in capsys.readouterr().out
    assert state_diff.main([str(ref), str(shifted), "--order-only"] + args) == 0
    assert state_diff.main([str(ref), str(shifted), "--tolerance", "5"] + args) == 0
    assert state_diff.main([str(ref), str(bad)] + args) == 2
    assert state_diff.main([str(ref), str(tmp_path / "missing")] + args) == 2


# ── --min-changes: nothing compared is not a pass (N0-25, D34) ───────────────────────

def scene_trace(*scenes: object) -> str:
    """One record per frame from f=0: a scene is an int, or '-' for a null chain."""
    out = HEADER
    for frame, scene in enumerate(scenes):
        text = "-" if scene == "-" else f"0x{scene:02x}"
        out += f"f={frame} t={frame * 4470000} scene={text} frame_counter=0x{frame:08x} delta=0x00000000 quest=-\n"
    return out


def real_changes(*scenes: object) -> int:
    return len(state_diff.real_scene_changes(state_diff.parse_trace(scene_trace(*scenes))))


def test_a_real_scene_change_is_between_two_readable_scenes():
    assert real_changes("-", 1, 1, 1) == 0                # the first value after a null chain is not a change
    assert real_changes(1, 1, 1) == 0                     # nor is the first record
    assert real_changes("-", 1, 2) == 1
    assert real_changes(1, 2, 3) == 2
    assert real_changes(1, "-", 2) == 1                   # the scene before the gap is the one compared
    assert real_changes(1, "-", 1) == 0                   # the same scene after a gap is no change
    assert real_changes(1, 2, "-") == 1                   # a change to '-' is not a real one
    assert real_changes("-", "-") == 0
    # the records returned are the ones that changed
    (change,) = state_diff.real_scene_changes(state_diff.parse_trace(scene_trace(5, 5, 7, 7)))
    assert change.frame == 2 and change.values["scene"] == 7


def min_changes(tmp_path, observables_file, ref, cand, n, capsys, *extra):
    r, c = tmp_path / "r.txt", tmp_path / "c.txt"
    r.write_text(ref)
    c.write_text(cand)
    capsys.readouterr()
    code = state_diff.main([str(r), str(c), "--observable", str(observables_file), "--min-changes", str(n), *extra])
    return code, capsys.readouterr().out


def test_min_changes_exits_3_when_too_few_real_changes_were_aligned(tmp_path, capsys):
    obs = tmp_path / "obs.txt"
    obs.write_text(OBSERVABLE)
    constant = scene_trace("-", 1, 1, 1, 1)
    code, out = min_changes(tmp_path, obs, constant, constant, 1, capsys)
    assert code == 3 and "0 real scene change" in out and "--min-changes 1" in out
    ab = scene_trace(1, 1, 2, 2)
    assert min_changes(tmp_path, obs, ab, ab, 1, capsys)[0] == 0
    code, out = min_changes(tmp_path, obs, ab, ab, 2, capsys)
    assert code == 3 and "1 real scene change" in out
    abc = scene_trace(1, 2, 2, 3)
    assert min_changes(tmp_path, obs, abc, abc, 2, capsys)[0] == 0
    assert min_changes(tmp_path, obs, abc, abc, 3, capsys)[0] == 3


def test_without_min_changes_a_constant_trace_still_passes(tmp_path, capsys):
    obs = tmp_path / "obs.txt"
    obs.write_text(OBSERVABLE)
    for flags in ([], ["--min-changes", "0"]):
        r = tmp_path / "r.txt"
        r.write_text(scene_trace("-", 1, 1, 1))
        assert state_diff.main([str(r), str(r), "--observable", str(obs), *flags]) == 0
    assert "traces agree" in capsys.readouterr().out


def test_a_difference_wins_over_too_few_changes(tmp_path, capsys):
    obs = tmp_path / "obs.txt"
    obs.write_text(OBSERVABLE)
    code, out = min_changes(tmp_path, obs, scene_trace(1, 2), scene_trace(1, 3), 5, capsys)
    assert code == 1 and "DIVERGENCE" in out


def test_the_last_line_prints_the_real_changes(tmp_path, capsys):
    obs = tmp_path / "obs.txt"
    obs.write_text(OBSERVABLE)
    code, out = min_changes(tmp_path, obs, scene_trace("-", 1, 2, 3), scene_trace("-", 1, 2, 3), 1, capsys)
    assert code == 0
    assert out.strip().splitlines()[-1].endswith("; 2 real scene changes")
    code, out = min_changes(tmp_path, obs, scene_trace("-", 1, 2, 3), scene_trace("-", 1, 2, 3), 0, capsys, "--order-only")
    assert out.strip().splitlines()[-1].endswith("; 2 real scene changes")


def test_a_negative_min_changes_is_a_usage_error(tmp_path):
    r = tmp_path / "r.txt"
    r.write_text(scene_trace(1, 2))
    with pytest.raises(SystemExit) as stop:
        state_diff.main([str(r), str(r), "--min-changes", "-1"])
    assert stop.value.code == 2
