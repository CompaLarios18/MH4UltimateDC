"""tools/native/cross_model_report.py (N0-28): synthetic runs only, with made-up frames, threads and counts."""
from __future__ import annotations

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
