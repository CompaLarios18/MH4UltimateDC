"""tools/native/timeline_check.py (N0-39): the scheduler's timeline against the recording's timing.txt.  Synthetic
files only."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "native"))

import timeline_check as tc  # noqa: E402

HEADER = "# timingtrace v1 2026-01-01 game=0x0004000000126300 cpu=recomp new3ds=off\n"

# (tick, core, late, name)
BASE = [(0, 0, 0, "SharedPage::UpdateTimeCallback"), (497376, 0, 0, "ThreadWakeupCallback_0"),
        (520461, 0, 2, "ThreadWakeupCallback_0"), (536762, 1, 0, "UDS::handle_async_event_signals_event"),
        (556108, 0, 0, "ThreadWakeupCallback_0"), (572409, 0, 4, "HID::UpdatePadCallback"),
        (600000, 1, 0, "ThreadWakeupCallback_1"), (610000, 0, 0, "ThreadWakeupCallback_0")]


def ref_text(events=BASE) -> str:
    return HEADER + "".join(f"tick={t} event={n} core={c} late={l} from=emu\n" for t, c, l, n in events)


def cand_text(events=BASE, stretches=True) -> str:
    out = []
    for t, c, l, _ in events:
        out.append(f"tick={t} core={c} late={l}\n")
        if stretches:
            out.append(f"stretch core={c} thread=1 start={t} slice=1000 t0={t} t1={t}\n")
            out.append(f"ended core={c} ran=10 end={t + 10}\n")
    return "".join(out)


def files(tmp_path, ref=None, cand=None):
    r, c = tmp_path / "timing.txt", tmp_path / "events.txt"
    r.write_text(ref if ref is not None else ref_text(), encoding="utf-8")
    c.write_text(cand if cand is not None else cand_text(), encoding="utf-8")
    return r, c


def run(tmp_path, capsys, ref=None, cand=None, allow=None, extra=()):
    r, c = files(tmp_path, ref, cand)
    argv = ["--reference", str(r), "--candidate", str(c), *extra]
    if allow is not None:
        a = tmp_path / "allow.txt"
        a.write_text(allow, encoding="utf-8")
        argv += ["--allow", str(a)]
    code = tc.main(argv)
    out = capsys.readouterr()
    return code, out.out, out.err


# ---- the cases of the spec

def test_equal_timelines(tmp_path, capsys):
    code, out, _ = run(tmp_path, capsys)
    assert code == 0
    assert out.splitlines()[0] == "8 of 8 equal"
    assert "difference" not in out


def test_one_event_late(tmp_path, capsys):
    events = list(BASE)
    events[3] = (536768, 1, 0, BASE[3][3])
    code, out, _ = run(tmp_path, capsys, cand=cand_text(events))
    assert code == 1
    assert out.splitlines()[0] == "7 of 8 equal; 1 difference(s), 0 allowed, 1 not allowed"
    assert "difference 1 (differs, NOT ALLOWED) at event 3" in out
    # the event, its two neighbours (the one before and the one after), on both sides
    assert "tick=520461 core=0 late=2 event=ThreadWakeupCallback_0" in out          # before, reference
    assert "tick=536762 core=1 late=0 event=UDS::handle_async_event_signals_event" in out      # the reference's event
    assert "tick=536768 core=1 late=0" in out                                       # the candidate's
    assert "tick=556108 core=0 late=0" in out                                       # after


def test_a_different_lateness_core_or_name_free_tick_is_a_difference(tmp_path, capsys):
    for k, changed in ((2, (520461, 0, 3, "x")), (1, (497376, 1, 0, "x"))):
        events = list(BASE)
        events[k] = changed
        code, out, _ = run(tmp_path, capsys, cand=cand_text(events))
        assert code == 1 and "1 difference(s)" in out.splitlines()[0]


def test_one_missing_event(tmp_path, capsys):
    events = BASE[:3] + BASE[4:]
    code, out, _ = run(tmp_path, capsys, cand=cand_text(events))
    assert code == 1
    assert out.splitlines()[0] == "7 of 8 equal (the candidate has 7 events); 1 difference(s), 0 allowed, 1 not allowed"
    assert "(missing, NOT ALLOWED) at event 3" in out
    assert "cand (none)" in out


def test_one_extra_event(tmp_path, capsys):
    events = BASE[:3] + [(540000, 0, 0, "x")] + BASE[3:]
    code, out, _ = run(tmp_path, capsys, cand=cand_text(events))
    assert code == 1
    assert out.splitlines()[0] == "8 of 8 equal (the candidate has 9 events); 1 difference(s), 0 allowed, 1 not allowed"
    assert "(extra, NOT ALLOWED) at event 3" in out
    assert "tick=540000 core=0 late=0" in out


def test_a_missing_event_at_the_end_and_an_extra_one_at_the_end(tmp_path, capsys):
    code, out, _ = run(tmp_path, capsys, cand=cand_text(BASE[:-1]))
    assert code == 1 and "(missing, NOT ALLOWED) at event 7" in out
    code, out, _ = run(tmp_path, capsys, cand=cand_text(BASE + [(700000, 0, 0, "x")]))
    assert code == 1 and "(extra, NOT ALLOWED) at event 8" in out


def test_the_first_event_missing(tmp_path, capsys):
    code, out, _ = run(tmp_path, capsys, cand=cand_text(BASE[1:]))
    assert code == 1 and "(missing, NOT ALLOWED) at event 0" in out
    assert "before:\n    ref  (none)" in out


def test_an_allowed_difference_passes(tmp_path, capsys):
    events = list(BASE)
    events[3] = (536756, 1, 0, BASE[3][3])
    allow = ("# the known residual\n\n"
             "ref=536762:1:0:UDS::handle_async_event_signals_event cand=536756:1:0   # nwm async events: 6 ticks early\n")
    code, out, _ = run(tmp_path, capsys, cand=cand_text(events), allow=allow)
    assert code == 0
    assert out.splitlines()[0] == "7 of 8 equal; 1 difference(s), 1 allowed, 0 not allowed"
    assert "(differs, allowed)" in out


def test_an_allow_line_names_the_difference_exactly(tmp_path, capsys):
    events = list(BASE)
    events[3] = (536756, 1, 0, BASE[3][3])
    for allow in (
            "ref=536762:1:0:UDS::handle_async_event_signals_event cand=536757:1:0   # one tick off\n",
            "ref=536762:1:0:Other cand=536756:1:0   # another event kind\n",
            "ref=536762:1:1:UDS::handle_async_event_signals_event cand=536756:1:0   # another lateness\n",
            "ref=536762:0:0:UDS::handle_async_event_signals_event cand=536756:1:0   # another core\n"):
        code, out, _ = run(tmp_path, capsys, cand=cand_text(events), allow=allow)
        assert code == 1, allow
        assert "matched no difference" in out


def test_an_allow_line_covers_one_occurrence(tmp_path, capsys):
    ref = [(0, 0, 0, "a"), (10, 1, 0, "u"), (10, 1, 0, "u"), (20, 0, 0, "b")]
    cand = [(0, 0, 0, "a"), (4, 1, 0, "u"), (4, 1, 0, "u"), (20, 0, 0, "b")]
    line = "ref=10:1:0:u cand=4:1:0   # early\n"
    code, out, _ = run(tmp_path, capsys, ref=ref_text(ref), cand=cand_text(cand), allow=line)
    assert code == 1 and "1 difference(s) not allowed" not in out and "2 difference(s), 1 allowed, 1 not allowed" in out
    code, out, _ = run(tmp_path, capsys, ref=ref_text(ref), cand=cand_text(cand), allow=line * 2)
    assert code == 0 and "2 difference(s), 2 allowed, 0 not allowed" in out


def test_missing_and_extra_events_can_be_allowed(tmp_path, capsys):
    code, out, _ = run(tmp_path, capsys, cand=cand_text(BASE[:3] + BASE[4:]),
                       allow="ref=536762:1:0:UDS::handle_async_event_signals_event cand=-   # not modelled\n")
    assert code == 0
    extra = BASE[:3] + [(540000, 0, 0, "x")] + BASE[3:]
    code, out, _ = run(tmp_path, capsys, cand=cand_text(extra), allow="ref=- cand=540000:0:0   # a beacon\n")
    assert code == 0


def test_unused_allow_lines_are_noted_and_not_an_error(tmp_path, capsys):
    code, out, _ = run(tmp_path, capsys, allow="ref=1:0:0:x cand=2:0:0   # stale\n")
    assert code == 0 and "note: allow line ref=1:0:0:x cand=2:0:0 matched no difference" in out


def test_the_candidate_without_stretch_lines_and_with_event_names_is_fine(tmp_path, capsys):
    code, out, _ = run(tmp_path, capsys, cand=cand_text(stretches=False))
    assert code == 0
    named = "".join(f"tick={t} event={n} core={c} late={l}\n" for t, c, l, n in BASE)
    code, out, _ = run(tmp_path, capsys, cand=named)
    assert code == 0


def test_differences_that_cascade_are_not_taken_for_a_missing_event(tmp_path, capsys):
    # three events in a row differ (the lateness): three differences, not a missing event and an extra one
    events = list(BASE)
    for k in (2, 3, 4):
        events[k] = (events[k][0], events[k][1], events[k][2] + 1, events[k][3])
    code, out, _ = run(tmp_path, capsys, cand=cand_text(events))
    assert code == 1
    assert out.splitlines()[0] == "5 of 8 equal; 3 difference(s), 0 allowed, 3 not allowed"
    assert "missing" not in out and "extra" not in out


def test_only_the_first_differences_are_printed(tmp_path, capsys):
    ref = [(i * 10, 0, 0, "e") for i in range(10)]
    cand = [(i * 10 + 1, 0, 0, "e") for i in range(10)]
    code, out, _ = run(tmp_path, capsys, ref=ref_text(ref), cand=cand_text(cand), extra=("--show", "2"))
    assert code == 1
    assert out.splitlines()[0] == "0 of 10 equal; 10 difference(s), 0 allowed, 10 not allowed"
    assert out.count("\ndifference ") == 2 and "and 8 more difference(s) not printed" in out


# ---- malformed input: exit 2

@pytest.mark.parametrize("bad", [
    "tick=5 core=0\n",                      # no lateness
    "tick=x core=0 late=0\n",
    "garbage\n",
])
def test_a_malformed_candidate_line_is_exit_2(tmp_path, capsys, bad):
    code, out, err = run(tmp_path, capsys, cand=cand_text() + bad)
    assert code == 2 and "events.txt:" in err and "not an event line" in err and out == ""


def test_a_malformed_reference_line_is_exit_2(tmp_path, capsys):
    code, out, err = run(tmp_path, capsys, ref=ref_text() + "tick=5 event=x core=0\n")
    assert code == 2 and "timing.txt:10:" in err       # the header, 8 events, then the line


def test_the_candidate_s_unknown_prefixes_are_not_skipped(tmp_path, capsys):
    code, _, err = run(tmp_path, capsys, cand=cand_text() + "thread core=0\n")
    assert code == 2 and "not an event line" in err


def test_missing_and_empty_files_are_exit_2(tmp_path, capsys):
    r, c = files(tmp_path)
    assert tc.main(["--reference", str(tmp_path / "no.txt"), "--candidate", str(c)]) == 2
    assert "cannot read" in capsys.readouterr().err
    assert tc.main(["--reference", str(r), "--candidate", str(tmp_path / "no.txt")]) == 2
    code, _, err = run(tmp_path, capsys, cand="stretch core=0 thread=1 start=0 slice=1 t0=0 t1=0\n")
    assert code == 2 and "no events" in err and "MH4N_TRACE_EVENTS" in err
    code, _, err = run(tmp_path, capsys, ref=HEADER)
    assert code == 2 and "no events" in err


@pytest.mark.parametrize("allow, message", [
    ("ref=1:0:0:x cand=2:0:0\n", "a reason"),
    ("ref=1:0:0:x cand=2:0:0 #   \n", "a reason"),
    ("ref=1:0:0 cand=2:0:0  # r\n", "bad ref"),
    ("ref=1:0:0:x cand=2:0  # r\n", "bad cand"),
    ("ref=- cand=-  # r\n", "both '-'"),
    ("1 0 0 x  # r\n", "expected"),
])
def test_a_malformed_allow_file_is_exit_2(tmp_path, capsys, allow, message):
    code, out, err = run(tmp_path, capsys, allow=allow)
    assert code == 2 and message in err and "allow.txt:1:" in err


def test_an_unreadable_allow_file_is_exit_2(tmp_path, capsys):
    r, c = files(tmp_path)
    assert tc.main(["--reference", str(r), "--candidate", str(c), "--allow", str(tmp_path / "none.txt")]) == 2


# ---- streaming

def test_the_files_are_streamed(tmp_path):
    """An event is read when it is needed: a malformed line far into the file is found without the lines before
    it being held, and read_events is a generator."""
    p = tmp_path / "t.txt"
    p.write_text("".join(f"tick={i} core=0 late=0\n" for i in range(50000)) + "garbage\n", encoding="utf-8")
    gen = tc.read_events(p)
    assert iter(gen) is gen
    assert next(gen).tick == 0
    stream = tc.Stream(tc.read_events(p))
    assert stream.peek(2).tick == 2 and stream.read == 3        # only what the look-ahead needed
    with pytest.raises(tc.TimelineError):
        for _ in tc.read_events(p):
            pass


def test_a_long_timeline_with_a_few_differences(tmp_path, capsys):
    n = 30000
    ref = "".join(f"tick={i * 7} event=e core={i % 2} late=0 from=emu\n" for i in range(n))
    cand = "".join(f"tick={i * 7 + (3 if i in (100, 20000) else 0)} core={i % 2} late=0\n" for i in range(n))
    code, out, _ = run(tmp_path, capsys, ref=HEADER + ref, cand=cand)
    assert code == 1 and out.splitlines()[0] == f"{n - 2} of {n} equal; 2 difference(s), 0 allowed, 2 not allowed"
