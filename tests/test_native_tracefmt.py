"""tools/native/tracefmt.py: SVC trace parsing and normalisation (synthetic traces only)."""
import os
import sys

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import tracefmt  # noqa: E402

HEADER = "# svctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off"


def h(v: int) -> str:
    return f"{v:08X}"


def call(thread, svc, args=(0, 0, 0, 0), out=None, pc=0x100000, lr=0x100004, t=0, in4=(0, 0, 0)):
    """One trace line, in the format of N0-04. args/out are ints (or ready-made hex strings)."""
    def fmt(v):
        return v if isinstance(v, str) else h(v)
    line = (f"{thread} {svc} pc={h(pc)} lr={h(lr)} {' '.join(fmt(a) for a in args)} t={t}"
            f" in4={','.join(fmt(a) for a in in4)}")
    if out == "-":
        line += " out=-"
    elif out is not None:
        line += f" out={','.join(fmt(a) for a in out)}"
    return line


def thread_line(tid, parent, entry, arg=0, stack=0x0FFFF000, prio=48, core="0/0"):
    return f"# thread {tid} parent={parent} entry={h(entry)} arg={h(arg)} stack={h(stack)} prio={prio} core={core}"


def parse(lines):
    return tracefmt.parse_lines(lines)


def test_parse_header_threads_and_calls():
    t = parse([
        HEADER,
        thread_line(1, 0, 0x100000),
        thread_line(7, 1, 0x200000, arg=0x55, prio=30, core="-2/0"),
        "# a note that every parser skips",
        "",
        call(1, "17", (1, 0, 0, 0), out=(0, 0x100, 0, 0), t=12, in4=(4, 5, 6)),
        call(7, "24", (0x100, 0, 0xFFFFFFFF, 0xFFFFFFFF), out="-"),
    ])
    assert t.header == {"tool": "svctrace", "version": "1", "date": "2026-10-16",
                        "game": "0x0004000000000000", "cpu": "jit", "new3ds": "off"}
    assert list(t.threads) == ["1", "7"]
    seven = t.threads["7"]
    assert (seven.parent, seven.entry, seven.arg, seven.prio) == ("1", "00200000", "00000055", 30)
    assert (seven.core_requested, seven.core_resolved) == (-2, 0)
    first, second = t.calls
    assert (first.thread, first.svc, first.pc, first.lr) == ("1", "17", "00100000", "00100004")
    assert first.args == ("00000001",) + ("00000000",) * 3
    assert first.ticks == 12 and first.in4 == ("00000004", "00000005", "00000006")
    assert first.out == ("00000000", "00000100", "00000000", "00000000")
    assert second.out is None                      # out=-: the call had not completed
    assert [c.line for c in t.calls] == [6, 7]     # line numbers count every line


def test_old_format_still_parses():
    t = parse(["1 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=10"])
    (c,) = t.calls
    assert c.thread == "1" and c.in4 is None and c.out is None and c.ticks == 10
    assert t.header is None and not t.threads


def test_thread_lines_are_not_headers():
    t = parse([thread_line(1, 0, 0x100000)])
    assert t.header is None and list(t.threads) == ["1"]


def test_thread_names():
    t = parse([
        thread_line(1, 0, 0x100000),
        thread_line(4, 1, 0x200000),
        thread_line(5, 1, 0x300000),
        thread_line(9, 4, 0x200000),               # a second thread with entry 200000
    ])
    assert tracefmt.thread_names(t) == {"1": "main", "4": "00200000#0", "5": "00300000#0", "9": "00200000#1"}


def test_thread_without_a_thread_line_keeps_its_number():
    t = tracefmt.normalise(parse([call(3, "0A")]))
    assert t.calls[0].thread == "3"


def test_handles_are_named_by_kind_and_creation_order():
    t = tracefmt.normalise(parse([
        thread_line(1, 0, 0x100000),
        call(1, "17", (0, 0, 0, 0), out=(0, 0x8001, 0, 0)),          # CreateEvent
        call(1, "13", (0, 0, 0, 0), out=(0, 0x8002, 0, 0)),          # CreateMutex
        call(1, "17", (1, 0, 0, 0), out=(0, 0x8003, 0, 0)),          # CreateEvent
        call(1, "18", (0x8003, 0, 0, 0), out=(0, 0, 0, 0)),          # SignalEvent
        call(1, "24", (0x8001, 0, 0xFFFFFFFF, 0xFFFFFFFF), out=(0x09401BFE, 0, 0, 0)),   # timed out
        call(1, "14", (0x8002, 0, 0, 0), out=(0, 0, 0, 0)),          # ReleaseMutex
    ]))
    c = t.calls
    assert c[0].out[1] == "event#0" and c[1].out[1] == "mutex#0" and c[2].out[1] == "event#1"
    assert c[3].args[0] == "event#1"
    assert c[4].args[0] == "event#0"
    assert c[5].args[0] == "mutex#0"
    assert all(x.ticks == 0 for x in c)


def test_each_creating_call_has_its_kind():
    kinds = {"08": "thread", "13": "mutex", "15": "semaphore", "17": "event", "1A": "timer",
             "1E": "memblock", "21": "arbiter", "2D": "port"}
    lines = [call(1, svc, out=(0, 0x8000 + i, 0, 0)) for i, svc in enumerate(kinds)]
    t = tracefmt.normalise(parse(lines))
    assert [c.out[1] for c in t.calls] == [f"{k}#0" for k in kinds.values()]


def test_create_port_names_both_handles_and_sessions_are_unknown():
    t = tracefmt.normalise(parse([
        call(1, "47", out=(0, 0x8001, 0x8002, 0)),                  # CreatePort: server, client
        call(1, "49", out=(0, 0x8003, 0x8004, 0)),                  # CreateSession
        call(1, "32", (0x8004, 0, 0, 0), out=(0, 0, 0, 0)),         # SendSyncRequest on a session
        call(1, "32", (0x8002, 0, 0, 0), out=(0, 0, 0, 0)),         # ... and on the client port
    ]))
    assert t.calls[0].out[1:3] == ("port#0", "port#1")
    assert t.calls[1].out[1:3] == ("?", "?")
    assert t.calls[2].args[0] == "?"
    assert t.calls[3].args[0] == "port#1"


def test_unknown_pseudo_and_zero_handles():
    t = tracefmt.normalise(parse([
        call(1, "32", (0x1234, 0, 0, 0), out=(0, 0, 0, 0)),         # a handle from an IPC reply
        call(1, "0B", (0, 0xFFFF8000, 0, 0), out=(0, 16, 0, 0)),    # GetThreadPriority(current)
        call(1, "4F", (0, 0, 1, 0), out=(0, 0, 0, 0)),              # ReplyAndReceive, no reply target
        call(1, "03", (0x1234, 0, 0, 0)),                           # not a handle SVC: r0 stays
    ]))
    assert t.calls[0].args[0] == "?"
    assert t.calls[1].args[1] == "FFFF8000" and t.calls[1].out[1] == "00000010"
    assert t.calls[2].args[3] == "00000000"
    assert t.calls[3].args[0] == "00001234"


def test_closed_handle_is_forgotten_and_its_value_can_come_back():
    t = tracefmt.normalise(parse([
        call(1, "17", out=(0, 0x8001, 0, 0)),
        call(1, "23", (0x8001, 0, 0, 0), out=(0, 0, 0, 0)),         # CloseHandle
        call(1, "24", (0x8001, 0, 0, 0), out=(0xD8E007F7, 0, 0, 0)),   # a stale handle
        call(1, "13", out=(0, 0x8001, 0, 0)),                       # the value is reused
        call(1, "14", (0x8001, 0, 0, 0), out=(0, 0, 0, 0)),
    ]))
    assert t.calls[1].args[0] == "event#0"       # the close itself still names it
    assert t.calls[2].args[0] == "?"
    assert t.calls[3].out[1] == "mutex#0"
    assert t.calls[4].args[0] == "mutex#0"


def test_duplicate_handle_is_the_same_object():
    t = tracefmt.normalise(parse([
        call(1, "17", out=(0, 0x8001, 0, 0)),
        call(1, "27", (0, 0x8001, 0, 0), out=(0, 0x8002, 0, 0)),    # DuplicateHandle
        call(1, "18", (0x8002, 0, 0, 0), out=(0, 0, 0, 0)),
    ]))
    assert t.calls[1].args[1] == "event#0" and t.calls[1].out[1] == "event#0"
    assert t.calls[2].args[0] == "event#0"


def test_failed_calls_drop_their_unset_outputs_and_failed_creates_name_nothing():
    t = tracefmt.normalise(parse([
        call(1, "17", out=(0xC8601801, 0xDEADBEEF, 0x12345678, 0x9ABCDEF0)),     # CreateEvent failed
        call(1, "17", out=(0, 0x8001, 0, 0)),
        call(1, "18", (0x8001, 0, 0, 0), out=(0, 0, 0, 0)),
    ]))
    assert t.calls[0].out == ("C8601801", "~", "~", "~")
    assert t.calls[1].out[1] == "event#0"        # the failed call did not use up a number
    assert t.calls[2].args[0] == "event#0"


def test_timed_out_wait_drops_the_index_it_never_set():
    t = tracefmt.normalise(parse([call(1, "25", (0, 0x1000, 2, 0), out=(0x09401BFE, 0x7FFF0000, 0, 0))]))
    assert t.calls[0].out == ("09401BFE", "~", "~", "~")
    ok = tracefmt.normalise(parse([call(1, "25", (0, 0x1000, 2, 0), out=(0, 1, 0, 0))]))
    assert ok.calls[0].out == ("00000000", "00000001", "00000000", "00000000")


def test_time_dependent_values_are_dropped():
    t = tracefmt.normalise(parse([call(1, "28", out=(0x1234, 0x5678, 0, 0), t=99)]))
    assert t.calls[0].out == ("~", "~", "~", "~") and t.calls[0].ticks == 0
    # a call that did not complete has no outputs to drop
    assert tracefmt.normalise(parse([call(1, "28", out="-")])).calls[0].out is None


def test_normalise_does_not_change_the_input():
    t = parse([thread_line(1, 0, 0x100000), call(1, "17", out=(0, 0x8001, 0, 0), t=5)])
    tracefmt.normalise(t)
    assert t.calls[0].thread == "1" and t.calls[0].out[1] == "00008001" and t.calls[0].ticks == 5


def test_normalised_threads_use_entry_and_creation_order():
    t = tracefmt.normalise(parse([
        thread_line(1, 0, 0x100000),
        thread_line(6, 1, 0x200000),
        thread_line(8, 1, 0x200000),
        call(6, "0A", (1, 0, 0, 0)), call(8, "0A", (2, 0, 0, 0)), call(1, "0A", (3, 0, 0, 0)),
    ]))
    assert [c.thread for c in t.calls] == ["00200000#0", "00200000#1", "main"]
    assert list(t.threads) == ["main", "00200000#0", "00200000#1"]
    assert t.threads["00200000#0"].parent == "main"


def test_by_thread_keeps_each_threads_order():
    t = parse([call(1, "0A", (1, 0, 0, 0)), call(2, "0A", (2, 0, 0, 0)), call(1, "0A", (3, 0, 0, 0))])
    grouped = t.by_thread()
    assert [c.args[0] for c in grouped["1"]] == ["00000001", "00000003"]


def test_matches_skips_what_a_trace_does_not_have():
    old = parse(["1 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=10"]).calls[0]
    new = parse([call(1, "21", (0x0FFFFFE0, 0, 0, 0), pc=0x107328, lr=0x106790, out=(0, 1, 2, 3))]).calls[0]
    assert old.matches(new, with_outputs=True, with_in4=True)
    other = parse([call(1, "21", (0x0FFFFFE0, 0, 0, 0), pc=0x107328, lr=0x106790, out=(0, 1, 2, 4))]).calls[0]
    assert new.matches(other) and not new.matches(other, with_outputs=True)


def test_cli_prints_normalised_calls(tmp_path, capsys):
    p = tmp_path / "t.txt"
    p.write_text("\n".join([thread_line(1, 0, 0x100000), call(1, "17", out=(0, 0x8001, 0, 0), t=5)]) + "\n")
    assert tracefmt.main([str(p), "--normalize"]) == 0
    line = capsys.readouterr().out.strip()
    assert line.startswith("main 17 pc=00100000") and line.endswith("out=00000000,event#0,00000000,00000000")
    assert " t=0 " in line
