"""tools/native/cross_model_report.py (N0-28): synthetic runs only, with made-up frames, threads and counts."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "native" / "cross_model_report.py"
sys.path.insert(0, str(ROOT / "tools" / "native"))

import cross_model_report as cmr  # noqa: E402

SCENE_A, SCENE_B, SCENE_C = 0x00AA0100, 0x00AA0200, 0x00AA0300

STATE_HEADER = "# statetrace v1 2026-01-01 game=0x0000000000000000 cpu={cpu} new3ds=off\n"
SVC_HEADER = "# svctrace v1 2026-01-01 game=0x0000000000000000 cpu={cpu} new3ds=off\n"
IPC_HEADER = "# ipctrace v1 2026-01-01 game=0x0000000000000000 cpu={cpu} new3ds=off\n"

OBSERVABLE = "[[0x00001000]+0x30] 4 scene eq\n0x00002000 4 frame_counter eq\n"


def state_trace(changes: list[tuple[int, int]], frames: int, cpu: str = "jit", unreadable_until: int = 0) -> str:
    """A state trace of `frames` frames: scene `-` up to `unreadable_until`, then the first scene of `changes`,
    switching to each later scene at its frame."""
    out = STATE_HEADER.format(cpu=cpu)
    for frame in range(frames + 1):
        scene = None
        if frame >= unreadable_until:
            for at, value in changes:
                if frame >= at:
                    scene = value
        text = "-" if scene is None else f"0x{scene:08x}"
        out += f"f={frame} t={frame * 4470000} scene={text} frame_counter=0x{frame:08x}\n"
    return out


def thread_lines(entries: list[tuple[int, int, str]]) -> str:
    """(id, parent, entry) -> `# thread` lines."""
    return "".join(f"# thread {i} parent={p} entry={e} arg=00000000 stack=00000000 prio=48 core=0/0\n"
                   for i, p, e in entries)


def svc_call(thread: int, svc: str) -> str:
    return (f"{thread} {svc} pc=00100000 lr=00100004 00000000 00000000 00000000 00000000 t=1 "
            f"in4=00000000,00000000,00000000 out=00000000,00000000,00000000,00000000\n")


def svc_trace(threads: list[tuple[int, int, str]], calls: dict[tuple[int, str], int], cpu: str = "jit") -> str:
    out = SVC_HEADER.format(cpu=cpu) + thread_lines(threads)
    for (thread, svc), n in calls.items():
        out += svc_call(thread, svc) * n
    return out


def ipc_call(thread: int, seq: int, session: str, cmd: int) -> str:
    return (f"{thread} seq={seq} session={session} cmd=0x{cmd:08x} in=0x{cmd:08x} desc=- out=0x{cmd:08x},0x00000000 "
            f"outdesc=- t=1\n")


def ipc_trace(threads: list[tuple[int, int, str]], requests: dict[tuple[int, str, int], int], cpu: str = "jit") -> str:
    out = IPC_HEADER.format(cpu=cpu) + thread_lines(threads)
    seq = 0
    for (thread, session, cmd), n in requests.items():
        for _ in range(n):
            seq += 1
            out += ipc_call(thread, seq, session, cmd)
    return out


def write_run(folder: Path, state: str, svc: str | None = None, ipc: str | None = None) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "state.txt").write_text(state, encoding="utf-8")
    if svc is not None:
        (folder / "svc.txt").write_text(svc, encoding="utf-8")
    if ipc is not None:
        (folder / "ipc.txt").write_text(ipc, encoding="utf-8")
    return folder


def run_tool(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, check=False)


THREADS = [(1, 0, "00100000"), (2, 1, "0010D8D4"), (3, 1, "0010D8D4"), (4, 2, "0010D8D4")]


# ── scene changes ────────────────────────────────────────────────────────────

def test_a_scene_change_shifted_by_one_frame(tmp_path: Path) -> None:
    ref = write_run(tmp_path / "ref", state_trace([(2, SCENE_A), (36, SCENE_B)], 60, "recomp"))
    cand = write_run(tmp_path / "cand", state_trace([(2, SCENE_A), (37, SCENE_B)], 60, "jit"))
    changes_ref = cmr.read_scene_changes(ref / "state.txt")
    changes_cand = cmr.read_scene_changes(cand / "state.txt")
    assert [(c.scene, c.frame) for c in changes_ref] == [(SCENE_B, 36)]
    assert [(c.scene, c.frame) for c in changes_cand] == [(SCENE_B, 37)]
    lines, problems = cmr.scene_table(changes_ref, changes_cand, 2)
    assert problems == []
    assert lines[-1] == f"| 1 | 0x{SCENE_B:08x} | 0x{SCENE_B:08x} | 36 | 37 | +1 | yes |"


def test_a_dash_start_is_not_a_real_scene_change(tmp_path: Path) -> None:
    # the scene is `-` up to frame 4, then SCENE_A (not a change), then SCENE_B at 30 (the only real one)
    path = tmp_path / "state.txt"
    path.write_text(state_trace([(5, SCENE_A), (30, SCENE_B)], 40, unreadable_until=5), encoding="utf-8")
    assert [(c.scene, c.frame) for c in cmr.read_scene_changes(path)] == [(SCENE_B, 30)]
    only_start = tmp_path / "start.txt"
    only_start.write_text(state_trace([(5, SCENE_A)], 40, unreadable_until=5), encoding="utf-8")
    assert cmr.read_scene_changes(only_start) == []


def test_changes_more_than_the_tolerance_apart_are_reported(tmp_path: Path) -> None:
    ref = [cmr.Change(SCENE_B, 36), cmr.Change(SCENE_C, 100)]
    cand = [cmr.Change(SCENE_B, 38), cmr.Change(SCENE_C, 103)]
    lines, problems = cmr.scene_table(ref, cand, 2)
    assert lines[-2].endswith("| +2 | yes |")
    assert lines[-1].endswith("| +3 | no |")
    assert problems == ["real scene change #2: 3 frames apart, more than 2"]
    # a wider tolerance is the caller's choice: the same changes pass with 3
    assert cmr.scene_table(ref, cand, 3)[1] == []


def test_another_scene_or_a_missing_change_is_a_problem() -> None:
    _, problems = cmr.scene_table([cmr.Change(SCENE_B, 36)], [cmr.Change(SCENE_C, 36)], 2)
    assert problems == ["real scene change #1: another scene on each side"]
    lines, problems = cmr.scene_table([cmr.Change(SCENE_B, 36), cmr.Change(SCENE_C, 90)], [cmr.Change(SCENE_B, 36)], 2)
    assert problems == ["real scene change #2 exists on the reference only"]
    assert lines[-1].endswith("| no: only the reference has it |")


# ── input events ────────────────────────────────────────────────────────────

SESSION = """\
# name: GX
# needs: none
# frames: 400
# checkpoints:
# captures:
# a comment line
f10:A:f6,f50:A:f6
f200:TOUCH:45:115:f6
f300:CIRCLE:0.0:1.0:f60
"""


def test_events_of_a_session_script() -> None:
    events = cmr.parse_events(SESSION)
    assert [(e.frame, e.label) for e in events] == [(10, "A"), (50, "A"), (200, "TOUCH"), (300, "CIRCLE")]
    with pytest.raises(cmr.ReportError):
        cmr.parse_events("f10:A:f6,garbage\n")


def test_an_input_events_margin_on_both_sides() -> None:
    events = cmr.parse_events(SESSION)
    ref = [cmr.Change(SCENE_B, 36), cmr.Change(SCENE_C, 250)]
    cand = [cmr.Change(SCENE_B, 38), cmr.Change(SCENE_C, 251)]
    rows = cmr.event_table(events, ref, cand)[2:]
    # the event at frame 10 is before the first real change on both sides; at 50 the change at 36 / 38 is the last
    assert rows[0] == "| 10 | A | - | - | - | - | - |"
    assert rows[1] == "| 50 | A | 36 | 38 | 14 | 12 | -2 |"
    assert rows[2] == "| 200 | TOUCH | 36 | 38 | 164 | 162 | -2 |"
    # at frame 300 the second change (250 against 251) is the last on each side
    assert rows[3] == "| 300 | CIRCLE | 250 | 251 | 50 | 49 | -1 |"
    summary = cmr.margin_summary(events, ref, cand)
    assert summary[0] == "- smallest margin on the reference: 14 frames (the event at frame 50)"
    assert summary[1] == "- smallest margin on the candidate: 12 frames (the event at frame 50)"


def test_an_event_on_the_frame_of_a_change_has_margin_zero() -> None:
    rows = cmr.event_table([cmr.Event(36, "A")], [cmr.Change(SCENE_B, 36)], [cmr.Change(SCENE_B, 37)])[2:]
    assert rows == ["| 36 | A | 36 | - | 0 | - | - |"]


# ── per-thread SVC counts ────────────────────────────────────────────────────────────

def test_per_thread_svc_counts_use_cross_model_names(tmp_path: Path) -> None:
    ref = write_run(tmp_path / "ref", state_trace([(2, SCENE_A)], 5),
                    svc_trace(THREADS, {(1, "28"): 10, (1, "0A"): 3, (2, "32"): 4, (3, "0A"): 2, (4, "22"): 5}, "recomp"))
    # the candidate numbers its threads in another order (3 before 2): the names follow the order of each parent's
    # own creations and its entries, never the numbers
    swapped = [(1, 0, "00100000"), (3, 1, "0010D8D4"), (2, 1, "0010D8D4"), (4, 3, "0010D8D4")]
    cand = write_run(tmp_path / "cand", state_trace([(2, SCENE_A)], 5),
                     svc_trace(swapped, {(1, "28"): 7, (1, "0A"): 3, (3, "32"): 4, (2, "0A"): 5, (4, "22"): 5}))
    r, c = cmr.count_svc(ref / "svc.txt"), cmr.count_svc(cand / "svc.txt")
    child = "main/0010D8D4#0/0010D8D4#0"
    assert dict(r.per_thread()) == {"main": 13, "main/0010D8D4#0": 4, "main/0010D8D4#1": 2, child: 5}
    assert dict(c.per_thread()) == {"main": 10, "main/0010D8D4#0": 4, "main/0010D8D4#1": 5, child: 5}
    table = cmr.svc_table(r, c)
    assert "| main | 13 | 10 | -3 | 0x28 GetSystemTick 10/7 |" in table
    assert "| main/0010D8D4#0 | 4 | 4 | +0 | - |" in table
    assert "| main/0010D8D4#1 | 2 | 5 | +3 | 0x0A SleepThread 2/5 |" in table
    assert f"| {child} | 5 | 5 | +0 | - |" in table
    assert table[-1] == "| **all 4 threads** | 24 | 24 | +0 | 2 of 4 threads differ |"


def test_svc_counts_of_a_trace_without_calls_are_refused(tmp_path: Path) -> None:
    path = tmp_path / "svc.txt"
    path.write_text(SVC_HEADER.format(cpu="jit") + thread_lines(THREADS), encoding="utf-8")
    with pytest.raises(cmr.ReportError):
        cmr.count_svc(path)
    with pytest.raises(cmr.ReportError):
        cmr.count_svc(tmp_path / "missing.txt")


# ── IPC counts ────────────────────────────────────────────────────────────

def test_ipc_counts_name_the_commands_whose_counts_differ(tmp_path: Path) -> None:
    ref = write_run(tmp_path / "ref", state_trace([(2, SCENE_A)], 5), ipc=ipc_trace(
        THREADS, {(1, "gsp::Gpu", 0x000C0000): 9, (1, "y2r:u", 0x00010040): 5, (2, "fs:USER/file#1", 0x080200C2): 7}))
    cand = write_run(tmp_path / "cand", state_trace([(2, SCENE_A)], 5), ipc=ipc_trace(
        THREADS, {(1, "gsp::Gpu", 0x000C0000): 11, (1, "y2r:u", 0x00010040): 5, (2, "fs:USER/file#3", 0x080200C2): 6}))
    table = cmr.ipc_table(cmr.count_ipc(ref / "ipc.txt"), cmr.count_ipc(cand / "ipc.txt"))
    assert table[0] == "21 requests on the reference, 22 on the candidate, 3 distinct commands; 2 of them with a different count on at least one thread."
    assert "| gsp::Gpu | 0x000c0000 | 9 | 11 | +2 | main 9/11 |" in table
    # the instance number of a file session is left out, as the comparator does
    assert "| fs:USER/file | 0x080200c2 | 7 | 6 | -1 | main/0010D8D4#0 7/6 |" in table
    assert not any("y2r" in line for line in table)


def test_ipc_counts_that_agree_have_no_table(tmp_path: Path) -> None:
    same = ipc_trace(THREADS, {(1, "gsp::Gpu", 0x000C0000): 3})
    ref = write_run(tmp_path / "ref", state_trace([(2, SCENE_A)], 5), ipc=same)
    cand = write_run(tmp_path / "cand", state_trace([(2, SCENE_A)], 5), ipc=same)
    table = cmr.ipc_table(cmr.count_ipc(ref / "ipc.txt"), cmr.count_ipc(cand / "ipc.txt"))
    assert table == ["3 requests on the reference, 3 on the candidate, 1 distinct commands; 0 of them with a different "
                     "count on at least one thread.", ""]


def test_an_ipc_trace_without_its_header_is_refused(tmp_path: Path) -> None:
    path = tmp_path / "ipc.txt"
    path.write_text(svc_trace(THREADS, {(1, "28"): 1}), encoding="utf-8")
    with pytest.raises(cmr.ReportError):
        cmr.count_ipc(path)


# ── the command line ────────────────────────────────────────────────────────────

# Four inputs: the first before the first real change (it has no margin), the others 74, 164 and 264 frames after
# the change at frame 36 of the reference, so all at least D50's 60 frames (with room for a shift of up to 14).
SESSION_FAR = """\
# name: GX
f10:A:f6,f110:A:f6
f200:TOUCH:45:115:f6
f300:CIRCLE:0.0:1.0:f60
"""


def _runs(tmp_path: Path, shift: int, session_text: str = SESSION_FAR) -> tuple[Path, Path, Path]:
    tmp_path.mkdir(parents=True, exist_ok=True)
    session = tmp_path / "GX.txt"
    session.write_text(session_text, encoding="utf-8")
    ref = write_run(tmp_path / "ref", state_trace([(2, SCENE_A), (36, SCENE_B)], 80, "recomp"),
                    svc_trace(THREADS, {(1, "28"): 10, (2, "0A"): 3}, "recomp"),
                    ipc_trace(THREADS, {(1, "gsp::Gpu", 0x000C0000): 4}, "recomp"))
    cand = write_run(tmp_path / "cand", state_trace([(2, SCENE_A), (36 + shift, SCENE_B)], 80, "jit"),
                     svc_trace(THREADS, {(1, "28"): 6, (2, "0A"): 3}),
                     ipc_trace(THREADS, {(1, "gsp::Gpu", 0x000C0000): 5}))
    return session, ref, cand


def test_the_command_line_prints_the_report(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path, 1)
    obs = tmp_path / "observable.txt"
    obs.write_text(OBSERVABLE, encoding="utf-8")
    done = run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand),
                    "--observable", str(obs))
    assert done.returncode == 0, done.stderr
    out = done.stdout
    assert out.startswith("## GX: jit (candidate) against recomp (reference)\n")
    assert f"| 1 | 0x{SCENE_B:08x} | 0x{SCENE_B:08x} | 36 | 37 | +1 | yes |" in out
    assert "The largest difference is 1 frame." in out
    assert "### Input events (4; minimum margin 60 frames)" in out
    assert "| main | 10 | 6 | -4 | 0x28 GetSystemTick 10/6 |" in out
    assert "| gsp::Gpu | 0x000c0000 | 4 | 5 | +1 | main 4/5 |" in out


def _cli(session: Path, ref: Path, cand: Path, *extra: str) -> subprocess.CompletedProcess:
    return run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand), *extra)


def test_the_defaults_are_d50s_numbers() -> None:
    assert cmr.DEFAULT_TOLERANCE == 4
    assert cmr.DEFAULT_MIN_MARGIN == 60
    help_text = run_tool("--help").stdout
    assert "default 4" in help_text and "default 60" in help_text


def test_a_shift_of_4_frames_passes_and_5_fails_by_default(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path, 4)
    done = _cli(session, ref, cand)
    assert done.returncode == 0, done.stderr
    assert "| 36 | 40 | +4 | yes |" in done.stdout
    assert "The largest difference is 4 frames." in done.stdout
    session, ref, cand = _runs(tmp_path / "five", 5)
    done = _cli(session, ref, cand)
    # the report is still written; the problem is named on stderr
    assert done.returncode == 1
    assert "| 36 | 41 | +5 | no |" in done.stdout
    assert "real scene change #1: 5 frames apart, more than 4" in done.stderr
    # the threshold is the caller's to give, never silently widened
    assert _cli(session, ref, cand, "--tolerance", "5").returncode == 0
    assert _cli(session, ref, cand, "--tolerance", "4").returncode == 1


def test_the_unit_check_of_the_tolerance_is_4_frames() -> None:
    ref = [cmr.Change(SCENE_B, 36), cmr.Change(SCENE_C, 100)]
    assert cmr.scene_table(ref, [cmr.Change(SCENE_B, 40), cmr.Change(SCENE_C, 104)], cmr.DEFAULT_TOLERANCE)[1] == []
    _, problems = cmr.scene_table(ref, [cmr.Change(SCENE_B, 40), cmr.Change(SCENE_C, 105)], cmr.DEFAULT_TOLERANCE)
    assert problems == ["real scene change #2: 5 frames apart, more than 4"]


def test_a_margin_of_60_frames_passes_and_59_fails() -> None:
    ref, cand = [cmr.Change(SCENE_B, 36)], [cmr.Change(SCENE_B, 36)]
    assert cmr.margin_problems([cmr.Event(96, "A")], ref, cand, cmr.DEFAULT_MIN_MARGIN) == []
    problems = cmr.margin_problems([cmr.Event(95, "TOUCH")], ref, cand, cmr.DEFAULT_MIN_MARGIN)
    # the event is named, with its frame, its kind and the side, on each side that is too close
    assert problems == ["input event at frame 95 (TOUCH): margin 59 frames on the reference, less than 60",
                        "input event at frame 95 (TOUCH): margin 59 frames on the candidate, less than 60"]
    # an event before the first real scene change has no margin; one on the frame of a change has margin 0
    assert cmr.margin_problems([cmr.Event(10, "A")], ref, cand, 60) == []
    assert len(cmr.margin_problems([cmr.Event(36, "A")], ref, cand, 60)) == 2
    # the margin of each side is its own: a change that comes later on the candidate leaves it less
    problems = cmr.margin_problems([cmr.Event(97, "A")], ref, [cmr.Change(SCENE_B, 38)], 60)
    assert problems == ["input event at frame 97 (A): margin 59 frames on the candidate, less than 60"]
    # the last real change before the event counts, not the first
    later = [cmr.Change(SCENE_B, 36), cmr.Change(SCENE_C, 150)]
    assert cmr.margin_problems([cmr.Event(200, "A")], later, later, 60) == [
        "input event at frame 200 (A): margin 50 frames on the reference, less than 60",
        "input event at frame 200 (A): margin 50 frames on the candidate, less than 60"]
    # a smaller or larger minimum is the caller's
    assert cmr.margin_problems([cmr.Event(95, "A")], ref, cand, 59) == []


def test_the_command_line_checks_the_margin_of_each_side(tmp_path: Path) -> None:
    # ref change at 36, candidate change at 38 (a shift of 2): the margins of an input at frame 98 are 62 and 60
    session, ref, cand = _runs(tmp_path / "pass", 2, "f98:A:f6\n")
    done = _cli(session, ref, cand)
    assert done.returncode == 0, done.stderr
    assert "| 98 | A | 36 | 38 | 62 | 60 | -2 |" in done.stdout
    # at frame 97 the margins are 61 and 59: the candidate's is too small, the event is named, the report is written
    session, ref, cand = _runs(tmp_path / "fail", 2, "f97:A:f6\n")
    done = _cli(session, ref, cand)
    assert done.returncode == 1
    assert "| 97 | A | 36 | 38 | 61 | 59 | -2 |" in done.stdout
    assert "input event at frame 97 (A): margin 59 frames on the candidate, less than 60" in done.stderr
    assert "margin 61" not in done.stderr
    # the minimum is the caller's to give
    assert _cli(session, ref, cand, "--min-margin", "59").returncode == 0
    assert _cli(session, ref, cand, "--min-margin", "60").returncode == 1
    # both limits at once: a shift of 5 and a margin of 59 give both problems
    session, ref, cand = _runs(tmp_path / "both", 5, "f95:A:f6\n")
    done = _cli(session, ref, cand)
    assert done.returncode == 1
    assert "5 frames apart, more than 4" in done.stderr and "input event at frame 95 (A)" in done.stderr


def test_the_report_states_the_margin_verdict(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path / "ok", 1)
    assert "every input event is at least 60 frames after the last real scene change on both sides" in _cli(
        session, ref, cand).stdout
    session, ref, cand = _runs(tmp_path / "bad", 1, "f90:A:f6,f300:A:f6\n")
    out = _cli(session, ref, cand).stdout
    assert "1 input event is less than 60 frames after the last real scene change on one side or both" in out


def test_a_negative_minimum_margin_is_bad_input(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path, 0)
    assert _cli(session, ref, cand, "--min-margin", "-1").returncode == 2


def test_the_old_scene_tolerance_of_2_is_still_available(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path, 3)
    assert _cli(session, ref, cand).returncode == 0
    done = _cli(session, ref, cand, "--tolerance", "2")
    assert done.returncode == 1
    assert "| 36 | 39 | +3 | no |" in done.stdout
    assert "3 frames apart, more than 2" in done.stderr


def test_the_report_leaves_out_what_it_is_told_to(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path, 0)
    done = run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand), "--no-svc", "--no-ipc")
    assert done.returncode == 0, done.stderr
    assert "SVC calls" not in done.stdout and "IPC requests" not in done.stdout
    (cand / "svc.txt").unlink()
    (cand / "ipc.txt").unlink()
    assert run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand), "--no-svc",
                    "--no-ipc").returncode == 0
    # without --no-svc a missing SVC trace is bad input, not an empty table
    bad = run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand))
    assert bad.returncode == 2 and "svc.txt" in bad.stderr


def test_bad_input_is_exit_status_2(tmp_path: Path) -> None:
    session, ref, cand = _runs(tmp_path, 0)
    missing = run_tool("--session", str(session), "--reference", str(tmp_path / "nowhere"), "--candidate", str(cand))
    assert missing.returncode == 2 and "state.txt" in missing.stderr
    no_scene = tmp_path / "no_scene.txt"
    no_scene.write_text("0x00002000 4 frame_counter eq\n", encoding="utf-8")
    assert run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand),
                    "--observable", str(no_scene)).returncode == 2
    (cand / "state.txt").write_text(STATE_HEADER.format(cpu="jit") + "f=1 scene=0x1\n", encoding="utf-8")
    assert run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand)).returncode == 2
    assert run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand),
                    "--tolerance", "-1").returncode == 2


def test_the_report_holds_no_value_of_the_traces(tmp_path: Path) -> None:
    """Frames, counts, thread names, service names and headers only: a data buffer written into a trace never
    reaches the report."""
    session, ref, cand = _runs(tmp_path, 1)
    secret = "5eC0ffEEdeadbeef"
    with open(ref / "ipc.txt", "a", encoding="utf-8") as f:
        f.write(f"1 seq=99 session=fs:USER/file#2 cmd=0x080200c2 in=0x080200c2 desc=- out=0x080200c2,0x00000000 "
                f"outdesc=- t=1 data=0:{secret}\n")
    done = run_tool("--session", str(session), "--reference", str(ref), "--candidate", str(cand))
    assert done.returncode == 0, done.stderr
    assert secret not in done.stdout + done.stderr


# ── images (N2b-17) ──────────────────────────────────────────────────────────

def _ppm(width: int = 32, height: int = 32, base: int = 10, edits: dict | None = None) -> bytes:
    body = bytearray()
    for y in range(height):
        for x in range(width):
            v = (edits or {}).get((x, y), base)
            body += bytes((v, v, v))
    return f"P6\n{width} {height}\n255\n".encode() + bytes(body)


def _block(x0: int, y0: int, value: int) -> dict:
    return {(x, y): value for x in range(x0, x0 + 16) for y in range(y0, y0 + 16)}


def _shots(folder: Path, frames: list[int], bad: dict[tuple[int, str], dict] | None = None,
           size: int = 32, missing: tuple = ()) -> None:
    """Screenshots at `frames`: a flat grey, or the edits of `bad[(frame, screen)]`."""
    shots = folder / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    for frame in frames:
        for screen in ("top", "bottom"):
            if (frame, screen) not in missing:
                (shots / f"f{frame:07d}_{screen}.ppm").write_bytes(
                    _ppm(size, size, edits=(bad or {}).get((frame, screen))))


FRAMES = list(range(300, 1000, 60))           # 300, 360, ... 960: 12 screenshots
IMG_CHANGES = [(2, SCENE_A), (700, SCENE_B)]    # scene 1: frames 0 to 699, scene 2: 700 to 1000


def _image_runs(tmp_path: Path, bad=None, missing=(), gas: str | None = None,
                size: int = 32) -> tuple[Path, Path, Path]:
    tmp_path.mkdir(parents=True, exist_ok=True)
    session = tmp_path / "G9.txt"
    session.write_text("# name: G9\nf400:A\n", encoding="utf-8")
    ref = write_run(tmp_path / "ref", state_trace(IMG_CHANGES, 1000, "recomp"))
    cand = write_run(tmp_path / "cand", state_trace(IMG_CHANGES, 1000, "jit"))
    _shots(ref, FRAMES, size=size)
    _shots(cand, FRAMES, bad, size=size, missing=missing)
    if gas is not None:
        (cand / "shots" / "gas.txt").write_text(gas, encoding="utf-8")
    return session, ref, cand


def _images(session: Path, ref: Path, cand: Path, *extra: str) -> subprocess.CompletedProcess:
    return run_tool("--images", "--session", str(session), "--reference", str(ref), "--candidate", str(cand), *extra)


def test_images_lists_every_frame_and_proposes_those_where_both_screens_pass(tmp_path: Path) -> None:
    bad = {(420, "top"): _block(0, 0, 40),           # a block off by 30: fails the block rule
           (780, "bottom"): _block(16, 16, 30)}      # the other screen, in the second scene
    session, ref, cand = _image_runs(tmp_path, bad)
    proposal = tmp_path / "frames.txt"
    done = _images(session, ref, cand, "--proposal", str(proposal))
    assert done.returncode == 0, done.stderr
    expected = [f for f in FRAMES if f not in (420, 780)]
    assert proposal.read_text(encoding="utf-8").splitlines() == [f"G9 {f}" for f in expected]
    out = done.stdout
    assert "10 of 12 frames are proposed** (10 pass the plain rule)" in out
    assert "| 420 | 1 |" in out and "| 780 | 2 |" in out
    assert "### Frames that do not pass the plain rule (2)" in out and "420, 780" in out
    assert "| 1 | 0x00aa0100 | 0 to 699 | 700 |" in out         # the scenes with their lengths
    assert "met" in out and "inf | 0.0" in out


def test_images_proposal_replaces_only_the_sessions_own_lines(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path)
    proposal = tmp_path / "frames.txt"
    proposal.write_text("G1 300\nG9 999\nG2 360\n", encoding="utf-8")
    assert _images(session, ref, cand, "--proposal", str(proposal)).returncode == 0
    lines = proposal.read_text(encoding="utf-8").splitlines()
    assert lines[:2] == ["G1 300", "G2 360"] and "G9 999" not in lines and lines[2:] == [f"G9 {f}" for f in FRAMES]


def test_images_a_long_scene_without_a_frame_is_a_shortfall(tmp_path: Path) -> None:
    # every screenshot of the second scene (700 to 1000, 301 frames) is wrong on the top screen
    bad = {(f, "top"): _block(0, 0, 90) for f in FRAMES if f >= 700}
    session, ref, cand = _image_runs(tmp_path, bad)
    proposal = tmp_path / "frames.txt"
    done = _images(session, ref, cand, "--proposal", str(proposal))
    assert done.returncode == 1 and "scene #2" in done.stderr and "no image frame" in done.stderr
    assert proposal.read_text(encoding="utf-8").splitlines() == [f"G9 {f}" for f in FRAMES if f < 700]   # still written
    assert "scene #2" in done.stdout
    assert _images(session, ref, cand, "--long-scene", "400").returncode == 0     # the scene is no longer "long"


def test_images_fewer_than_three_frames_is_a_shortfall(tmp_path: Path) -> None:
    bad = {(f, "bottom"): _block(0, 0, 90) for f in FRAMES if f not in (300, 360)}
    session, ref, cand = _image_runs(tmp_path, bad)
    done = _images(session, ref, cand, "--long-scene", "10000")
    assert done.returncode == 1 and "only 2 image frames, at least 3 needed" in done.stderr
    assert _images(session, ref, cand, "--long-scene", "10000", "--min-frames", "2").returncode == 0


def test_images_a_frame_in_gas_txt_or_missing_is_never_proposed(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path, missing=((540, "bottom"),), gas="360\n")
    proposal = tmp_path / "frames.txt"
    done = _images(session, ref, cand, "--proposal", str(proposal))
    assert done.returncode == 0, done.stderr
    assert proposal.read_text(encoding="utf-8").splitlines() == [f"G9 {f}" for f in FRAMES if f not in (360, 540)]
    assert "| gas |" in done.stdout and "the bottom screenshot is missing on the candidate" in done.stdout


def test_images_screens_of_another_size_are_a_problem_of_the_frame(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path)
    (cand / "shots" / "f0000300_top.ppm").write_bytes(_ppm(48, 32))
    done = _images(session, ref, cand, "--proposal", str(tmp_path / "p.txt"))
    assert done.returncode == 0 and "different sizes" in done.stdout
    assert "G9 300" not in (tmp_path / "p.txt").read_text(encoding="utf-8")


def test_images_bad_input_is_exit_status_2(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path)
    shutil.rmtree(cand / "shots")
    assert _images(session, ref, cand).returncode == 2
    session, ref, cand = _image_runs(tmp_path / "b")
    (cand / "shots" / "gas.txt").write_text("three\n", encoding="utf-8")
    assert _images(session, ref, cand).returncode == 2
    session, ref, cand = _image_runs(tmp_path / "c")
    assert _images(session, ref, cand, "--min-frames", "-1").returncode == 2
    assert _images(session, tmp_path / "nowhere", cand).returncode == 2


# ── the margin of D58 ────────────────────────────────────────────────────────

def _noisy(k: int) -> dict:
    """A 32x32 screen whose pixels are all 2 off (base 10 -> 12), the first `k` pixels 3 off: k=21 is a PSNR of
    42.0004 dB, k=22 one of 41.995 dB; the block error stays near 2."""
    return {(x, y): 13 if y * 32 + x < k else 12 for y in range(32) for x in range(32)}


def _blocky(extra_pixels: int = 0, diff: int = 7, size: int = 64) -> dict:
    """The 16x16 block at 0,0 off by `diff` (base 10), `extra_pixels` of its pixels one more: a block error of
    exactly `diff` when there are none; the PSNR stays above 43 dB on a 64x64 screen."""
    edits = _block(0, 0, 10 + diff)
    for i in range(extra_pixels):
        edits[(i % 16, i // 16)] = 10 + diff + 1
    return edits


def _proposed(tmp_path: Path, edits: dict, *extra: str, size: int = 32, frame: int = 420) -> bool:
    session, ref, cand = _image_runs(tmp_path, {(frame, "top"): edits}, size=size)
    proposal = tmp_path / "p.txt"
    done = _images(session, ref, cand, "--proposal", str(proposal), *extra)
    assert done.returncode == 0, done.stderr
    return f"G9 {frame}" in proposal.read_text(encoding="utf-8").splitlines()


def test_the_margin_premises_of_the_synthetic_frames() -> None:
    import imgdiff
    ref = imgdiff.parse_ppm(_ppm(32, 32))
    over = imgdiff.compare(ref, imgdiff.parse_ppm(_ppm(32, 32, edits=_noisy(21))))
    under = imgdiff.compare(ref, imgdiff.parse_ppm(_ppm(32, 32, edits=_noisy(22))))
    assert 42.0 <= over.psnr < 42.01 and 41.9 < under.psnr < 42.0 and over.worst_block < 3 and under.worst_block < 3
    ref64 = imgdiff.parse_ppm(_ppm(64, 64))
    for extra, expect in ((0, 7.0), (26, 7.1016)):
        m = imgdiff.compare(ref64, imgdiff.parse_ppm(_ppm(64, 64, edits=_blocky(extra))))
        assert m.psnr > 43 and abs(m.worst_block - expect) < 1e-3


def test_the_defaults_of_the_margin_are_2_db_and_1_of_255() -> None:
    assert cmr.IMAGE_MARGIN_DB == 2.0 and cmr.IMAGE_MARGIN_BLOCK == 1.0
    help_text = run_tool("--help").stdout
    assert "default 2" in help_text and "default 1" in help_text


def test_a_frame_just_under_42_db_is_not_proposed_and_one_just_over_is(tmp_path: Path) -> None:
    assert _proposed(tmp_path / "over", _noisy(21)) is True
    assert _proposed(tmp_path / "under", _noisy(22)) is False


def test_a_block_of_7_1_is_not_proposed_and_one_of_exactly_7_is(tmp_path: Path) -> None:
    assert _proposed(tmp_path / "seven", _blocky(0), size=64) is True
    assert _proposed(tmp_path / "over", _blocky(26), size=64) is False


def test_margins_of_zero_give_the_plain_rule(tmp_path: Path) -> None:
    plain = ("--margin-db", "0", "--margin-block", "0")
    # 40.5 dB: passes the gate's 40 dB, not the 42 dB of the margin
    edits = _noisy(368)
    assert _proposed(tmp_path / "a", edits) is False
    assert _proposed(tmp_path / "b", edits, *plain) is True
    # a block of 7.5: passes 8/255, not 7.0
    block = _blocky(128, 7)
    assert _proposed(tmp_path / "c", block, size=64) is False
    assert _proposed(tmp_path / "d", block, *plain, size=64) is True
    # the plain rule still refuses a PSNR under 40 dB (all pixels 4 off: 36 dB) and a block above 8
    assert _proposed(tmp_path / "e", _blocky(0, 9), *plain, size=64) is False
    assert _proposed(tmp_path / "f", {(x, y): 14 for x in range(32) for y in range(32)}, *plain) is False


def test_each_margin_is_a_dimension_of_its_own(tmp_path: Path) -> None:
    only_db = ("--margin-db", "0")
    only_block = ("--margin-block", "0")
    # 41.99 dB: the block margin alone does not drop it, the dB margin does
    assert _proposed(tmp_path / "a", _noisy(22), *only_db) is True
    assert _proposed(tmp_path / "b", _noisy(22), *only_block) is False
    # a block of 7.1: the dB margin alone does not drop it, the block margin does
    assert _proposed(tmp_path / "c", _blocky(26), *only_block, size=64) is True
    assert _proposed(tmp_path / "d", _blocky(26), *only_db, size=64) is False


def test_a_negative_margin_is_exit_status_2(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path)
    assert _images(session, ref, cand, "--margin-db", "-0.5").returncode == 2
    assert _images(session, ref, cand, "--margin-block", "-1").returncode == 2


def test_a_narrow_frame_is_reported_as_not_proposed(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path, {(420, "top"): _noisy(22), (480, "bottom"): _noisy(21)})
    done = _images(session, ref, cand)
    assert done.returncode == 0, done.stderr
    out = done.stdout
    assert "| 420 | 1 |" in out and "narrow |" in out and out.count("narrow |") == 1
    assert "### Frames that pass the plain rule but are not proposed: narrow (1)" in out
    assert "11 of 12 frames are proposed** (12 pass the plain rule)" in out
    assert "### Frames that do not pass the plain rule (0)" in out
    plain = _images(session, ref, cand, "--margin-db", "0", "--margin-block", "0").stdout
    assert "12 of 12 frames are proposed" in plain and "narrow (0)" in plain


def test_coverage_is_computed_on_the_margin_set(tmp_path: Path) -> None:
    # every top screen of the second scene (700 to 1000) is narrow: it passes 40 dB, not 42 dB
    bad = {(f, "top"): _noisy(22) for f in FRAMES if f >= 700}
    session, ref, cand = _image_runs(tmp_path, bad)
    done = _images(session, ref, cand)
    assert done.returncode == 1 and "scene #2" in done.stderr and "no image frame" in done.stderr
    assert _images(session, ref, cand, "--margin-db", "0", "--margin-block", "0").returncode == 0


def test_a_scene_named_not_judged_needs_no_frame(tmp_path: Path) -> None:
    bad = {(f, "top"): _noisy(22) for f in FRAMES if f >= 700}
    session, ref, cand = _image_runs(tmp_path, bad)
    done = _images(session, ref, cand, "--not-judged", "700-1000")
    assert done.returncode == 0, done.stderr
    assert "not judged by the jit-calibrated gate" in done.stdout
    # the range names the scene by where it starts; another range, or another shortfall, is still a shortfall
    assert _images(session, ref, cand, "--not-judged", "701-1000").returncode == 1
    assert _images(session, ref, cand, "--not-judged", "0-1").returncode == 1
    two = {(f, "top"): _noisy(22) for f in FRAMES}                      # no frame anywhere: fewer than --min-frames
    session, ref, cand = _image_runs(tmp_path / "b", two)
    done = _images(session, ref, cand, "--not-judged", "0-1000")
    assert done.returncode == 1 and "only 0 image frames" in done.stderr
    # repeatable, and bad specs are bad input
    assert _images(session, ref, cand, "--not-judged", "0-1", "--not-judged", "700-1000").returncode == 1
    for spec in ("700", "a-b", "900-800", "700-", "\u0667-\u0668"):
        assert _images(session, ref, cand, "--not-judged", spec).returncode == 2, spec


def test_a_shot_number_in_gas_txt_with_a_non_ascii_digit_is_bad_input(tmp_path: Path) -> None:
    session, ref, cand = _image_runs(tmp_path)
    (cand / "shots" / "gas.txt").write_text("3\u00b2\n", encoding="utf-8")
    assert _images(session, ref, cand).returncode == 2