"""tools/native/scene_probe.py (N0-27): synthetic dumps and state traces only, with made-up values."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "native"))

import scene_probe  # noqa: E402

OBSERVABLE = """\
# synthetic
[[[0x00001000]+0x24]] 4 scene eq
[[0x00002000]+0x30] 1 applet_state eq
0x00003000 4 frame_counter eq
"""

HEADER = "# statetrace v1 2026-01-01 game=0x0000000000000000 cpu={cpu} new3ds=off\n"

A, B, C, D = 0x00E00010, 0x00E00020, 0x00E00030, 0x00E00040


def trace(cpu: str, changes: list[tuple[int, int]], total: int = 1000, *, first_frame: int = 0,
          field: str = "scene", gap: tuple[int, int] | None = None) -> str:
    """A state trace of `total` frames. `changes` is [(frame the value starts at, value)]; frames
    before the first entry read `-`. `gap` makes the field `-` for frames in [lo, hi)."""
    out = HEADER.format(cpu=cpu)
    for frame in range(first_frame, total + 1):
        value = None
        for start, v in changes:
            if frame >= start:
                value = v
        if gap and gap[0] <= frame < gap[1]:
            value = None
        text = "-" if value is None else f"0x{value:08x}"
        out += f"f={frame} t={frame * 4470000} {field}={text} frame_counter=0x{frame:08x}\n"
    return out


G2_SEQ = [(1, A), (36, B), (300, C), (600, D)]
G1_SEQ = [(1, A), (36, B)]


def write(tmp: Path, name: str, text: str) -> str:
    path = tmp / name
    path.write_text(text)
    return str(path)


@pytest.fixture()
def observable(tmp_path):
    return write(tmp_path, "observable.txt", OBSERVABLE)


def exit_code(argv: list[str]) -> int:
    try:
        return scene_probe.main(argv)
    except SystemExit as error:          # argparse's own usage errors
        return int(error.code)


def run_validate(observable: str, traces: list[str], *extra: str) -> int:
    return exit_code(["validate", "--observable", observable, *extra, *traces])


# ── dumps ──────────────────────────────────────────────────────────────────────────

def test_dumps_prints_one_dump_option_per_object_and_step(tmp_path, capsys):
    session = write(tmp_path, "S.txt", "# name: S\n# frames: 200\nf10:A:f6\n")
    assert scene_probe.main(["dumps", "--session", session, "--object", "0x081cce60:0x3840",
                             "--object", "0x08e44300:0x400", "--every", "50"]) == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines == [
        "--dump f50:0x081cce60:0x3840", "--dump f50:0x08e44300:0x400",
        "--dump f100:0x081cce60:0x3840", "--dump f100:0x08e44300:0x400",
        "--dump f150:0x081cce60:0x3840", "--dump f150:0x08e44300:0x400",
        "--dump f200:0x081cce60:0x3840", "--dump f200:0x08e44300:0x400",
    ]


def test_dumps_default_step_is_50(tmp_path, capsys):
    session = write(tmp_path, "S.txt", "# frames: 120\n")
    assert scene_probe.main(["dumps", "--session", session, "--object", "0x1000:16"]) == 0
    assert capsys.readouterr().out.splitlines() == ["--dump f50:0x00001000:0x10", "--dump f100:0x00001000:0x10"]


def test_dumps_first_and_last_narrow_the_range(tmp_path, capsys):
    session = write(tmp_path, "S.txt", "# frames: 5000\n")
    assert scene_probe.main(["dumps", "--session", session, "--object", "0x1000:16", "--every", "10",
                             "--first", "4900", "--last", "4930"]) == 0
    assert capsys.readouterr().out.splitlines() == [f"--dump f{f}:0x00001000:0x10" for f in (4900, 4910, 4920, 4930)]
    # --last beyond the session stops at the session's total; --first after --last is a usage problem
    assert scene_probe.main(["dumps", "--session", session, "--object", "0x1000:16", "--every", "10",
                             "--first", "4990", "--last", "9999"]) == 0
    assert capsys.readouterr().out.splitlines() == [f"--dump f{f}:0x00001000:0x10" for f in (4990, 5000)]
    assert exit_code(["dumps", "--session", session, "--object", "0x1000:16", "--first", "300", "--last", "200"]) == 2


@pytest.mark.parametrize("text,args", [
    ("# name: S\n", ["--object", "0x1000:16"]),                      # no '# frames:' line
    ("# frames: 100\n", []),                                          # no object
    ("# frames: 100\n", ["--object", "0x1000"]),                      # no size
    ("# frames: 100\n", ["--object", "0x1000:0"]),                    # empty object
    ("# frames: 100\n", ["--object", "0x1000:16", "--every", "0"]),
])
def test_dumps_usage_problems_exit_2(tmp_path, text, args):
    session = write(tmp_path, "S.txt", text)
    assert exit_code(["dumps", "--session", session, *args]) == 2


def test_dumps_missing_session_exits_2(tmp_path, capsys):
    assert scene_probe.main(["dumps", "--session", str(tmp_path / "none.txt"), "--object", "0x1000:16"]) == 2


# ── rank ───────────────────────────────────────────────────────────────────────────

def make_dumps(tmp_path: Path, frames: list[int], addr: int = 0x081CCE60, size: int = 0x40) -> Path:
    """Dumps in which: u32 +0x10 changes right after the phases 300 and 600 (a scene field), +0x20 is
    a counter, +0x30 is constant, +0x34 is a flag that toggles at random-looking frames."""
    folder = tmp_path / "dumps"
    folder.mkdir()
    for frame in frames:
        data = bytearray(size)
        scene = 1 + (frame > 300 + 20) + (frame > 600 + 20)
        struct.pack_into("<I", data, 0x10, 0x00E00000 + scene)
        struct.pack_into("<I", data, 0x20, frame)
        struct.pack_into("<I", data, 0x30, 0xCAFEBABE)
        struct.pack_into("<I", data, 0x34, 1 if frame in (150, 450, 850) else 0)
        (folder / f"mem_f{frame:07d}_{addr:08x}.bin").write_bytes(bytes(data))
    return folder


def test_rank_puts_a_planted_phase_field_above_a_counter_and_a_constant(tmp_path, capsys):
    folder = make_dumps(tmp_path, list(range(50, 1001, 50)))
    assert scene_probe.main(["rank", str(folder), "--phases", "300,600", "--top", "500", "--all"]) == 0
    lines = [l for l in capsys.readouterr().out.splitlines() if l.startswith("0x")]
    position = {}
    for index, line in enumerate(lines):
        position.setdefault(" ".join(line.split()[:2]), index)
    planted, counter, toggle, constant = (position[f"0x081cce60+0x{o:02x} u32"] for o in (0x10, 0x20, 0x34, 0x30))
    assert planted == 0                       # candidates first
    assert planted < counter < toggle < constant


def test_rank_reports_counts_and_frames(tmp_path, capsys):
    folder = make_dumps(tmp_path, list(range(50, 1001, 50)))
    assert scene_probe.main(["rank", str(folder), "--phases", "300,600"]) == 0
    first = capsys.readouterr().out.splitlines()
    row = next(l for l in first if l.startswith("0x081cce60+0x10 u32"))
    assert "distinct=3" in row and "changes=2" in row and "near=2" in row
    assert "350" in row and "650" in row     # the dump frames where the change shows


def test_rank_lists_u8_u16_and_u32_offsets(tmp_path, capsys):
    folder = make_dumps(tmp_path, list(range(50, 1001, 50)))
    scene_probe.main(["rank", str(folder), "--phases", "300", "--top", "1000", "--all"])
    out = capsys.readouterr().out
    assert "0x081cce60+0x10 u8" in out and "0x081cce60+0x10 u16" in out and "0x081cce60+0x10 u32" in out
    assert "0x081cce60+0x11 u8" in out and "0x081cce60+0x12 u16" in out
    assert "0x081cce60+0x11 u16" not in out and "0x081cce60+0x12 u32" not in out   # aligned only


def test_rank_separates_objects_by_address(tmp_path, capsys):
    folder = make_dumps(tmp_path, [50, 100, 150])
    for frame in (50, 100, 150):
        (folder / f"mem_f{frame:07d}_08e44300.bin").write_bytes(bytes([frame // 50]) * 8)
    scene_probe.main(["rank", str(folder), "--phases", "100", "--top", "1000", "--all"])
    out = capsys.readouterr().out
    assert "0x08e44300+0x00 u8" in out and "0x081cce60+0x10 u8" in out


def test_rank_without_dumps_or_with_bad_phases_is_a_usage_problem(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    assert scene_probe.main(["rank", str(empty), "--phases", "300"]) == 2
    folder = make_dumps(tmp_path, [50, 100])
    assert exit_code(["rank", str(folder), "--phases", "x,y"]) == 2
    assert exit_code(["rank", str(folder), "--phases", ","]) == 2
    assert exit_code(["rank", str(tmp_path / "missing"), "--phases", "100"]) == 2


# ── validate ───────────────────────────────────────────────────────────────────────

def good_set(tmp_path: Path) -> list[str]:
    return [
        write(tmp_path, "G2-recomp-1.txt", trace("recomp", G2_SEQ)),
        write(tmp_path, "G2-recomp-2.txt", trace("recomp", [(1, A), (37, B), (301, C), (599, D)])),
        write(tmp_path, "G2-jit-1.txt", trace("jit", [(1, A), (40, B), (350, C), (700, D)])),
        write(tmp_path, "G1-recomp-1.txt", trace("recomp", G1_SEQ)),
        write(tmp_path, "G6-recomp-1.txt", trace("recomp", [(1, A), (36, B), (300, C)])),
    ]


def test_validate_passes_on_a_good_field_with_different_sequences_per_session(tmp_path, observable, capsys):
    traces = good_set(tmp_path)
    # the leading '-' (frame 0) is not a scene; G1, G2 and G6 sequences differ and are never compared
    assert run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G2:100,400,800") == 0
    out = capsys.readouterr().out
    assert "largest" in out and "G2" in out


def test_validate_prints_the_largest_delta_per_session(tmp_path, observable, capsys):
    traces = good_set(tmp_path)
    run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G2:100,400,800")
    out = capsys.readouterr().out
    assert any(l.startswith("G2") and "largest" in l and "2" in l.split("largest")[1] for l in out.splitlines())


def test_validate_fails_on_a_constant_field(tmp_path, observable, capsys):
    traces = [write(tmp_path, "G2-recomp-1.txt", trace("recomp", [(1, A)])),
              write(tmp_path, "G2-recomp-2.txt", trace("recomp", [(1, A)]))]
    assert run_validate(observable, traces) == 1
    assert "distinct" in capsys.readouterr().out


def test_validate_fails_when_two_runs_of_a_session_differ_in_sequence(tmp_path, observable, capsys):
    traces = good_set(tmp_path)
    traces[1] = write(tmp_path, "G2-recomp-2.txt", trace("recomp", [(1, A), (36, B), (300, D), (600, C)]))
    assert run_validate(observable, traces, "--tolerance", "2") == 1
    assert "sequence" in capsys.readouterr().out


def test_validate_fails_when_cross_backend_sequences_differ(tmp_path, observable):
    traces = good_set(tmp_path)
    traces[2] = write(tmp_path, "G2-jit-1.txt", trace("jit", [(1, A), (40, B), (350, C)]))
    assert run_validate(observable, traces, "--tolerance", "2") == 1


def test_validate_fails_when_two_runs_of_a_backend_are_more_than_n_frames_apart(tmp_path, observable, capsys):
    traces = good_set(tmp_path)
    traces[1] = write(tmp_path, "G2-recomp-2.txt", trace("recomp", [(1, A), (36, B), (310, C), (600, D)]))
    assert run_validate(observable, traces, "--tolerance", "2") == 1
    assert "apart" in capsys.readouterr().out
    assert run_validate(observable, traces, "--tolerance", "10") == 0
    assert run_validate(observable, traces) == 1                       # default tolerance 0


def test_validate_does_not_gate_frames_across_backends(tmp_path, observable):
    # the jit trace of good_set is up to 100 frames away from the recomp ones: only the order counts
    assert run_validate(observable, good_set(tmp_path), "--tolerance", "2") == 0


def test_validate_fails_when_the_scene_goes_unreadable_after_its_first_value(tmp_path, observable, capsys):
    traces = good_set(tmp_path)
    traces[0] = write(tmp_path, "G2-recomp-1.txt", trace("recomp", G2_SEQ, gap=(100, 110)))
    assert run_validate(observable, traces, "--tolerance", "2") == 1
    assert "unreadable" in capsys.readouterr().out


def test_validate_fails_when_a_g2_trace_has_fewer_than_three_values(tmp_path, observable, capsys):
    traces = [write(tmp_path, "G2-recomp-1.txt", trace("recomp", G1_SEQ)),
              write(tmp_path, "G2-recomp-2.txt", trace("recomp", G1_SEQ))]
    assert run_validate(observable, traces) == 1
    assert "at least 3" in capsys.readouterr().out
    # the same two values are fine in G1: only G2 needs 3
    g1 = [write(tmp_path, "G1-recomp-1.txt", trace("recomp", G1_SEQ)),
          write(tmp_path, "G1-recomp-2.txt", trace("recomp", G1_SEQ))]
    assert run_validate(observable, g1) == 0


def test_validate_expect_distinct_needs_pairwise_distinct_values(tmp_path, observable, capsys):
    traces = good_set(tmp_path)
    assert run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G2:100,400,800") == 0
    assert run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G2:100,200") == 1
    assert "same scene" in capsys.readouterr().out
    # a frame the trace does not reach is a failure, not a pass
    assert run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G2:100,5000") == 1
    # frames of a session without the option are not checked; only the named session is
    assert run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G6:20,100") == 0


def test_validate_expect_distinct_value_at_a_frame_before_the_first_readable_value_fails(tmp_path, observable):
    traces = good_set(tmp_path)
    assert run_validate(observable, traces, "--tolerance", "2", "--expect-distinct", "G2:0,400") == 1


def test_validate_exit_3_when_a_trace_has_no_scene_field(tmp_path, observable):
    traces = good_set(tmp_path)
    traces[3] = write(tmp_path, "G1-recomp-1.txt", trace("recomp", G1_SEQ, field="other"))
    assert run_validate(observable, traces, "--tolerance", "2") == 3


def test_validate_exit_2_for_bad_trace_names_and_mismatched_backends(tmp_path, observable):
    good = write(tmp_path, "G2-recomp-1.txt", trace("recomp", G2_SEQ))
    assert run_validate(observable, [write(tmp_path, "state.txt", trace("recomp", G2_SEQ))]) == 2
    assert run_validate(observable, [write(tmp_path, "G2-recomp.txt", trace("recomp", G2_SEQ))]) == 2
    assert run_validate(observable, [write(tmp_path, "G2-other-1.txt", trace("recomp", G2_SEQ))]) == 2
    # the name says jit, the header recomp
    assert run_validate(observable, [write(tmp_path, "G2-jit-1.txt", trace("recomp", G2_SEQ))]) == 2
    assert run_validate(observable, [good, str(tmp_path / "missing" / "G2-recomp-2.txt")]) == 2


def test_validate_exit_2_for_usage_problems(tmp_path, observable):
    good = write(tmp_path, "G2-recomp-1.txt", trace("recomp", G2_SEQ))
    assert run_validate(str(tmp_path / "none.txt"), [good]) == 2                  # no observable file
    assert run_validate(write(tmp_path, "noscene.txt", "0x1000 4 other eq\n"), [good]) == 2
    assert run_validate(observable, [good], "--expect-distinct", "G2") == 2
    assert run_validate(observable, [good], "--expect-distinct", "G2:x") == 2
    assert run_validate(observable, [good], "--tolerance", "-1") == 2
    assert exit_code(["validate", "--observable", observable]) == 2               # no trace


def test_validate_exit_2_for_an_empty_or_malformed_trace(tmp_path, observable):
    assert run_validate(observable, [write(tmp_path, "G2-recomp-1.txt", HEADER.format(cpu="recomp"))]) == 2
    assert run_validate(observable, [write(tmp_path, "G2-recomp-1.txt",
                                           HEADER.format(cpu="recomp") + "f=1 scene=0x1\n")]) == 2


def test_validate_exit_1_when_a_trace_never_has_a_readable_scene(tmp_path, observable):
    only_dash = trace("recomp", [])
    assert run_validate(observable, [write(tmp_path, "G1-recomp-1.txt", only_dash)]) == 1


def test_validate_one_trace_alone_still_checks_the_rest(tmp_path, observable, capsys):
    one = write(tmp_path, "G2-recomp-1.txt", trace("recomp", G2_SEQ))
    assert run_validate(observable, [one], "--expect-distinct", "G2:100,400,800") == 0
    assert "no two traces" in capsys.readouterr().out


def test_trace_file_name_parsing():
    assert scene_probe.parse_trace_name("G2-recomp-1.txt") == ("G2", "recomp", 1)
    assert scene_probe.parse_trace_name("/x/y/G6-jit-12.txt") == ("G6", "jit", 12)
    for bad in ("G2-recomp.txt", "G2-recomp-1.log", "-recomp-1.txt", "G2-fast-1.txt", "state.txt"):
        assert scene_probe.parse_trace_name(bad) is None
