"""tools/native/tracefmt.py: SVC trace parsing and normalisation (synthetic traces only)."""
import os
import sys

import pytest

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
    t = parse(["1 21 pc=00100500 lr=00100504 00300000 00000000 00000000 00000000 t=10"])
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
    old = parse(["1 21 pc=00100500 lr=00100504 00300000 00000000 00000000 00000000 t=10"]).calls[0]
    new = parse([call(1, "21", (0x00300000, 0, 0, 0), pc=0x100500, lr=0x100504, out=(0, 1, 2, 3))]).calls[0]
    assert old.matches(new, with_outputs=True, with_in4=True)
    other = parse([call(1, "21", (0x00300000, 0, 0, 0), pc=0x100500, lr=0x100504, out=(0, 1, 2, 4))]).calls[0]
    assert new.matches(other) and not new.matches(other, with_outputs=True)


def test_cli_prints_normalised_calls(tmp_path, capsys):
    p = tmp_path / "t.txt"
    p.write_text("\n".join([thread_line(1, 0, 0x100000), call(1, "17", out=(0, 0x8001, 0, 0), t=5)]) + "\n")
    assert tracefmt.main([str(p), "--normalize"]) == 0
    line = capsys.readouterr().out.strip()
    assert line.startswith("main 17 pc=00100000") and line.endswith("out=00000000,event#0,00000000,00000000")
    assert " t=0 " in line


# ── names across timing models (N0-25, D31) ─────────────────────────────────────────

def test_cross_model_thread_names_are_per_parent():
    """`main`, then `<parent name>/<entry>#<k>`, k counting the threads that parent created with that entry."""
    lines = [
        thread_line(1, 0, 0x100000),
        thread_line(4, 1, 0x200000),
        thread_line(5, 1, 0x300000),
        thread_line(9, 4, 0x400000),               # created by thread 4
        thread_line(10, 5, 0x400000),              # same entry, created by thread 5
        thread_line(11, 4, 0x400000),              # thread 4's second thread with that entry
    ]
    names = tracefmt.thread_names(parse(lines), per_parent=True)
    assert names == {"1": "main", "4": "main/00200000#0", "5": "main/00300000#0",
                     "9": "main/00200000#0/00400000#0", "10": "main/00300000#0/00400000#0",
                     "11": "main/00200000#0/00400000#1"}
    # the global names are unchanged without the flag
    assert tracefmt.thread_names(parse(lines))["10"] == "00400000#1"


def test_cross_model_names_do_not_depend_on_the_order_of_creations_by_different_parents():
    def prog(order):
        lines = [thread_line(1, 0, 0x100000), thread_line(2, 1, 0x200000), thread_line(3, 1, 0x300000)]
        children = {"a": thread_line(7, 2, 0x400000), "b": thread_line(8, 3, 0x400000)}
        return lines + [children[k] for k in order]
    names_ab = tracefmt.thread_names(parse(prog("ab")), per_parent=True)
    names_ba = tracefmt.thread_names(parse(prog("ba")), per_parent=True)
    assert names_ab["7"] == names_ba["7"] == "main/00200000#0/00400000#0"
    assert names_ab["8"] == names_ba["8"] == "main/00300000#0/00400000#0"
    # the global names swap when the creations happen in the other order
    g_ab, g_ba = tracefmt.thread_names(parse(prog("ab"))), tracefmt.thread_names(parse(prog("ba")))
    assert g_ab["7"] == "00400000#0" and g_ba["7"] == "00400000#1"


def test_cross_model_handles_are_named_per_creating_thread():
    def prog(order):
        creations = {"m": call(1, "17", (0, 0, 0, 0), out=(0, 0x8001 if order == "mw" else 0x8002, 0, 0)),
                     "w": call(2, "17", (0, 0, 0, 0), out=(0, 0x8002 if order == "mw" else 0x8001, 0, 0))}
        return [thread_line(1, 0, 0x100000), thread_line(2, 1, 0x200000)] + [creations[k] for k in order]
    for order in ("mw", "wm"):
        t = tracefmt.normalise(parse(prog(order)), cross_model=True)
        by_thread = {c.thread: c.out[1] for c in t.calls}
        assert by_thread == {"main": "event#main.0", "main/00200000#0": "event#main/00200000#0.0"}, order
    # the plain names depend on the interleaving
    plain = {c.thread: c.out[1] for c in tracefmt.normalise(parse(prog("wm"))).calls}
    assert plain == {"main": "event#1", "00200000#0": "event#0"}


def test_cross_model_handle_counts_run_per_thread_and_kind():
    t = tracefmt.normalise(parse([
        thread_line(1, 0, 0x100000),
        call(1, "17", out=(0, 0x8001, 0, 0)), call(1, "13", out=(0, 0x8002, 0, 0)), call(1, "17", out=(0, 0x8003, 0, 0)),
        call(1, "27", (0, 0x8003, 0, 0), out=(0, 0x8004, 0, 0)),         # a duplicate is the same object
        call(1, "18", (0x8004, 0, 0, 0), out=(0, 0, 0, 0)),
    ]), cross_model=True)
    assert [c.out[1] for c in t.calls[:3]] == ["event#main.0", "mutex#main.0", "event#main.1"]
    assert t.calls[3].out[1] == "event#main.1" and t.calls[4].args[0] == "event#main.1"


def test_cross_model_normalised_threads_keep_their_parents_names():
    t = tracefmt.normalise(parse([
        thread_line(1, 0, 0x100000), thread_line(6, 1, 0x200000), thread_line(8, 6, 0x300000),
        call(8, "0A", (1, 0, 0, 0)),
    ]), cross_model=True)
    assert t.calls[0].thread == "main/00200000#0/00300000#0"
    assert t.threads["main/00200000#0/00300000#0"].parent == "main/00200000#0"


def test_cli_cross_model_flag_names_per_parent(tmp_path, capsys):
    p = tmp_path / "t.txt"
    p.write_text("\n".join([thread_line(1, 0, 0x100000), thread_line(2, 1, 0x200000),
                            call(2, "17", out=(0, 0x8001, 0, 0))]) + "\n")
    assert tracefmt.main([str(p), "--cross-model"]) == 0
    line = capsys.readouterr().out.strip()
    assert line.startswith("main/00200000#0 17 ") and "event#main/00200000#0.0" in line


# ── IPC traces (N0-21) ──────────────────────────────────────────────────────────────

IPC_HEADER = "# ipctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off"


def ipc_line(thread, seq, session, words_in, words_out, desc="-", outdesc="-", t=0, data=(), cmd=None):
    """One IPC trace line (ints for the words)."""
    def fmt(words):
        return "-" if words is None else ",".join(f"0x{x:08x}" for x in words)
    text = (f"{thread} seq={seq} session={session} cmd=0x{(words_in[0] if cmd is None else cmd):08x} "
            f"in={fmt(words_in)} desc={desc} out={fmt(words_out)} outdesc={outdesc} t={t}")
    return text + "".join(f" data={i}:{b}" for i, b in data)


def test_parse_ipc_header_threads_and_requests():
    t = tracefmt.parse_ipc_lines([
        IPC_HEADER,
        thread_line(1, 0, 0x100000),
        "# a note",
        ipc_line(1, 1, "fs:USER", (0x00010042, 5, 6), (0x00010040, 0), desc="static:2:16:0x0badcafe,move:1:1:a:b/c#0",
                 outdesc="copy:2:2:x+0", t=77, data=((2, "00ff"),)),
        ipc_line(1, 2, "srv:", (0x00020000,), None),
    ])
    assert t.header["tool"] == "ipctrace" and t.header["cpu"] == "jit"
    assert list(t.threads) == ["1"]
    first, second = t.calls
    assert (first.thread, first.seq, first.session, first.cmd) == ("1", 1, "fs:USER", "0x00010042")
    assert first.words_in == ("0x00010042", "0x00000005", "0x00000006")
    assert [str(d) for d in first.desc] == ["static:2:16:0x0badcafe", "move:1:1:a:b/c#0"]   # ':' inside a token
    assert first.desc[1].token == "a:b/c#0" and first.outdesc[0].token == "x+0"
    assert first.ticks == 77 and first.data == ((2, "00ff"),) and first.result == "0x00000000"
    assert second.words_out is None and second.result is None and second.desc == ()   # never answered


def test_result_needs_a_normal_parameter():
    t = tracefmt.parse_ipc_lines([ipc_line(1, 1, "srv:", (0x00010000,), (0x00010000, 0xC8804464))])
    assert t.calls[0].result is None                      # header says 0 normal parameters
    t = tracefmt.parse_ipc_lines([ipc_line(1, 1, "srv:", (0x00010000,), (0x00010040, 0xC8804464))])
    assert t.calls[0].result == "0xc8804464"


def test_lines_without_seq_and_other_comments_are_skipped_and_words_are_lowercased():
    t = tracefmt.parse_ipc_lines(["1 21 pc=00100500 lr=00100504 00300000 00000000 00000000 00000000 t=10",
                                  "#thread-ish", "1 seq=x session=a", "1 seq=3 session=a cmd=0X00AB00CD in=0X00AB00CD "
                                  "desc=- out=- outdesc=- t=1"])
    (c,) = t.calls
    assert c.cmd == "0x00ab00cd" and c.words_in == ("0x00ab00cd",)


def test_ipc_by_thread_orders_by_seq():
    t = tracefmt.parse_ipc_lines([ipc_line(1, 2, "a", (0x20000,), (0x20040, 0)), ipc_line(2, 1, "b", (0x10000,), (0x10040, 0)),
                                  ipc_line(1, 1, "a", (0x10000,), (0x10040, 0))])
    assert [c.seq for c in t.by_thread()["1"]] == [1, 2]


def test_normalise_ipc_names_threads_handles_and_drops_times():
    t = tracefmt.normalise_ipc(tracefmt.parse_ipc_lines([
        thread_line(1, 0, 0x100000), thread_line(6, 1, 0x200000),
        # reply: header, result, descriptor (copy 2 handles), two handles, then a null handle's descriptor and word
        ipc_line(6, 1, "fs:USER", (0x00010000,), (0x00010046, 0, 0x04000000, 0x8001, 0x8002, 0x10, 0),
                 outdesc="copy:2:2:fs:USER/file#0+fs:USER/dir#0,move:5:1:0", t=5),
        ipc_line(1, 1, "srv:", (0x00020042, 0, 0, 0x8001, 0x9999), (0x00020040, 0),
                 desc="copy:2:2:fs:USER/file#0+-", t=9),
    ]))
    by = t.by_thread()
    reply = by["00200000#0"][0]
    assert reply.words_out[3:5] == ("fs:USER/file#0", "fs:USER/dir#0") and reply.words_out[6] == "0x00000000"
    assert reply.words_out[:3] == ("0x00010046", "0x00000000", "0x04000000")
    assert reply.ticks == 0
    request = by["main"][0]
    assert request.words_in[3:5] == ("fs:USER/file#0", "?")              # no name: unknown


def test_normalise_ipc_does_not_change_the_input():
    t = tracefmt.parse_ipc_lines([ipc_line(1, 1, "a", (0x10000, 0, 0x8001), (0x10040, 0), desc="copy:1:1:x#0", t=3)])
    tracefmt.normalise_ipc(t)
    assert t.calls[0].words_in[2] == "0x00008001" and t.calls[0].ticks == 3


def test_cli_prints_an_ipc_trace(tmp_path, capsys):
    p = tmp_path / "ipc.txt"
    p.write_text("\n".join([IPC_HEADER, thread_line(1, 0, 0x100000),
                            ipc_line(1, 1, "srv:", (0x00010042, 5, 0, 0x8001), (0x00010040, 0), desc="copy:2:1:x#0", t=5)]) + "\n")
    assert tracefmt.main([str(p), "--normalize"]) == 0
    line = capsys.readouterr().out.strip()
    assert line.startswith("main seq=1 session=srv: cmd=0x00010042") and "x#0" in line and line.endswith("t=0")
    assert tracefmt.main([str(p)]) == 0
    assert "0x00008001" in capsys.readouterr().out and tracefmt.is_ipc_trace(str(p))


def ipc_of(*lines):
    return tracefmt.parse_ipc_lines([IPC_HEADER, *lines])


def test_svc_handles_from_ipc_replies_are_named_from_the_ipc_trace():
    svc = parse([
        thread_line(1, 0, 0x100000),
        call(1, "17", out=(0, 0x8001, 0, 0)),                                   # CreateEvent
        call(1, "32", (0x7000, 0, 0, 0), out=(0, 0, 0, 0)),                     # SendSyncRequest to srv: (handle from before)
        call(1, "32", (0x7001, 0, 0, 0), out=(0, 0, 0, 0)),                     # ... on the session srv: gave out
        call(1, "18", (0x8001, 0, 0, 0), out=(0, 0, 0, 0)),
        call(1, "23", (0x7001, 0, 0, 0), out=(0, 0, 0, 0)),                     # CloseHandle of it
        call(1, "32", (0x7001, 0, 0, 0), out=(0xD8E007F7, 0, 0, 0)),            # closed: no IPC record for it
    ])
    ipc = ipc_of(
        ipc_line(1, 1, "srv:", (0x00050100,), (0x00050042, 0, 0x04000000, 0x7001), outdesc="copy:2:1:srv:/session#0"),
        ipc_line(1, 2, "fs:USER", (0x00010000,), (0x00010040, 0)),
    )
    t = tracefmt.normalise(svc, ipc)
    assert t.calls[1].args[0] == "?"                       # the first call used a handle nobody here created
    assert t.calls[2].args[0] == "srv:/session#0"
    assert t.calls[4].args[0] == "srv:/session#0"
    assert t.calls[5].args[0] == "?"                       # forgotten after CloseHandle
    plain = tracefmt.normalise(svc)
    assert plain.calls[2].args[0] == "?"                   # without the IPC trace it stays unknown


def test_svc_pairing_follows_each_threads_requests_and_skips_invalid_handle_calls():
    svc = parse([
        thread_line(1, 0, 0x100000), thread_line(2, 1, 0x200000),
        call(2, "32", (0x1111, 0, 0, 0), out=(0xD8E007F7, 0, 0, 0)),            # never reached the recorder
        call(2, "32", (0x6000, 0, 0, 0), out=(0, 0, 0, 0)),                     # thread 2's first request
        call(1, "32", (0x6000, 0, 0, 0), out=(0, 0, 0, 0)),                     # thread 1's first request
        call(1, "32", (0x7001, 0, 0, 0), out=(0, 0, 0, 0)),
        call(2, "32", (0x7002, 0, 0, 0), out=(0, 0, 0, 0)),
    ])
    ipc = ipc_of(
        ipc_line(1, 1, "srv:", (0x00050100,), (0x00050042, 0, 0x04000000, 0x7001), outdesc="copy:2:1:a/session#0"),
        ipc_line(2, 1, "srv:", (0x00050100,), (0x00050042, 0, 0x04000000, 0x7002), outdesc="copy:2:1:b/session#0"),
    )
    t = tracefmt.normalise(svc, ipc)
    assert [c.args[0] for c in t.calls[3:]] == ["a/session#0", "b/session#0"]


def test_handles_a_request_moved_are_forgotten():
    svc = parse([
        call(1, "32", (0x6000, 0, 0, 0), out=(0, 0, 0, 0)),
        call(1, "32", (0x7001, 0, 0, 0), out=(0, 0, 0, 0)),                     # the request moves 0x7001
        call(1, "24", (0x7001, 0, 0, 0), out=(0xD8E007F7, 0, 0, 0)),
    ])
    ipc = ipc_of(
        ipc_line(1, 1, "srv:", (0x00050100,), (0x00050042, 0, 0x04000000, 0x7001), outdesc="move:2:1:a/session#0"),
        ipc_line(1, 2, "a/session#0", (0x00010042, 0x10, 0x7001), (0x00010040, 0), desc="move:1:1:a/session#0"),
    )
    t = tracefmt.normalise(svc, ipc)
    assert t.calls[1].args[0] == "a/session#0" and t.calls[2].args[0] == "?"


def test_cli_names_svc_handles_with_ipc(tmp_path, capsys):
    s = tmp_path / "svc.txt"
    s.write_text("\n".join([thread_line(1, 0, 0x100000), call(1, "32", (0x6000, 0, 0, 0), out=(0, 0, 0, 0)),
                            call(1, "32", (0x7001, 0, 0, 0), out=(0, 0, 0, 0))]) + "\n")
    i = tmp_path / "ipc.txt"
    i.write_text("\n".join([IPC_HEADER, ipc_line(1, 1, "srv:", (0x00050100,), (0x00050042, 0, 0x04000000, 0x7001),
                                                 outdesc="copy:2:1:srv:/session#0")]) + "\n")
    assert tracefmt.main([str(s), "--ipc", str(i)]) == 0
    lines = capsys.readouterr().out.strip().splitlines()
    assert "srv:/session#0" in lines[1] and "srv:/session#0" not in lines[0]


def test_descriptors_with_an_index_or_size_that_cannot_exist_are_dropped():
    good = tracefmt._descs("static:1:16:0x11223344,copy:0:2:a+b,move:63:64:x")
    assert [(d.kind, d.index, d.size) for d in good] == [("static", 1, 16), ("copy", 0, 2), ("move", 63, 64)]
    # an index is a word of the 64-word command buffer; a handle descriptor holds at most 64 handles
    for text in ("copy:-100:1:foo", "static:-1:4:0x1", "copy:64:1:foo", "move:0:-1:foo", "static:0:-4:0x1",
                 "copy:0:65:foo", "move:0:99999999999999999:foo"):
        assert tracefmt._descs(text) == (), text
    # a buffer's size is in bytes and may be large
    assert [d.size for d in tracefmt._descs("static:1:99999999:0x11223344")] == [99999999]


# ── `fs:` file requests (N0-32, D52) ────────────────────────────────────────────────

def read_call(session="fs:USER/file#3", offset=0x1_0000_0010, size=0x800, crc="0x1c853882", result=0, reply=True):
    words_in = (0x080200C2, offset & 0xFFFFFFFF, offset >> 32, size, (size << 4) | 0xC, 0x0FFFDAF8)
    words_out = (0x08020082, result, size, (size << 4) | 0xC, 0x0FFFDAF8) if reply else None
    return tracefmt.parse_ipc_lines([ipc_line(1, 1, session, words_in, words_out, desc=f"mapped-w:4:{size}:-",
                                              outdesc=f"mapped-w:4:{size}:{crc}" if reply else "-")]).calls[0]


def write_call(offset=0, size=16, flags=0x00010001, data=None):
    words_in = (0x08030102, offset & 0xFFFFFFFF, offset >> 32, size, flags, (size << 4) | 0xA, 0x32EF8DF0)
    return tracefmt.parse_ipc_lines([ipc_line(1, 2, "fs:USER/file#4", words_in, (0x08030082, 0, size, 0, 0),
                                              desc=f"mapped-r:5:{size}:0x8ddaee24",
                                              data=() if data is None else ((5, data),))]).calls[0]


def test_instance_free_and_the_command_kinds():
    assert tracefmt.instance_free("fs:USER/file#12") == "fs:USER/file"
    assert tracefmt.instance_free("fs:USER") == "fs:USER"
    assert tracefmt.fs_file_command(read_call()) == "read"
    assert tracefmt.fs_file_command(write_call()) == "write"
    assert tracefmt.fs_file_command(read_call(session="fs:USER/dir#1")) is None       # not a file session
    other = tracefmt.parse_ipc_lines([ipc_line(1, 1, "fs:USER/file#1", (0x08040000,), (0x08040040, 0))]).calls[0]
    assert tracefmt.fs_file_command(other) is None                                    # another command (Close)


def test_a_file_request_has_a_64_bit_offset_and_a_size():
    assert tracefmt.fs_file_request(read_call(offset=0x1_0000_0010, size=0x800)) == (0x1_0000_0010, 0x800)
    assert tracefmt.fs_file_request(write_call(offset=0x40, size=16)) == (0x40, 16)
    short = tracefmt.parse_ipc_lines([ipc_line(1, 1, "fs:USER/file#1", (0x080200C2, 1), None)]).calls[0]
    assert tracefmt.fs_file_request(short) is None                                    # a truncated request
    assert tracefmt.fs_file_request(tracefmt.parse_ipc_lines(
        [ipc_line(1, 1, "srv:", (0x00010000,), None)]).calls[0]) is None


def test_the_crc_of_the_data_a_read_returned_comes_from_the_reply_buffer():
    assert tracefmt.fs_read_crc(read_call(crc="0x1c853882")) == "0x1c853882"
    assert tracefmt.fs_read_crc(read_call(reply=False)) == "-"                         # a Read that never got a reply


def test_command_words_stop_before_the_translate_parameters():
    # header 0x08030102: 4 normal parameters (offset lo, offset hi, size, flags), 2 translate words
    words = tracefmt.parse_ipc_lines([ipc_line(1, 1, "fs:USER/file#4", (0x08030102, 1, 2, 3, 4, 0xA, 0x32EF8DF0), None)]
                                     ).calls[0].words_in
    assert tracefmt.command_words(words) == words[:5]
    assert tracefmt.command_words(None) is None and tracefmt.command_words(()) is None
    assert tracefmt.command_words(("garbage", "0x1")) is None


def test_ipc_data_is_found_by_descriptor_index():
    call = write_call(size=3, data="00ff10")
    assert tracefmt.ipc_data(call, 5) == b"\x00\xff\x10"
    assert tracefmt.ipc_data(call, 4) is None and tracefmt.ipc_data(write_call(), 5) is None
    with pytest.raises(ValueError):
        tracefmt.ipc_data(write_call(size=3, data="zz"), 5)


# ── touching Reads and the CRC-32 of their concatenation (N0-33, D55) ────────────────────

import random  # noqa: E402
import zlib  # noqa: E402


def test_crc32_combine_equals_zlib_on_random_splits():
    rng = random.Random(33)
    for _ in range(300):
        total = rng.choice([0, 1, 2, 3, 7, 64, 4096, 6144, 28672]) + rng.randrange(0, 300)
        data = bytes(rng.randrange(256) for _ in range(total))
        cut = rng.randrange(0, total + 1)
        a, b = data[:cut], data[cut:]
        assert tracefmt.crc32_combine(zlib.crc32(a), zlib.crc32(b), len(b)) == zlib.crc32(data), (total, cut)


def test_crc32_combine_edges_and_chains():
    rng = random.Random(7)
    data = bytes(rng.randrange(256) for _ in range(28672))
    whole = zlib.crc32(data)
    # nothing appended: the first CRC; nothing before: the second
    assert tracefmt.crc32_combine(zlib.crc32(data), 0, 0) == whole
    assert tracefmt.crc32_combine(0, whole, len(data)) == whole
    # the pieces of a 28672-byte range: 4096-byte and 2048-byte reads, folded left to right
    sizes, crc, at = [4096, 2048, 4096, 4096, 2048, 2048, 4096, 2048, 2048, 2048], 0, 0
    assert sum(sizes) == len(data)
    for n in sizes:
        crc = tracefmt.crc32_combine(crc, zlib.crc32(data[at:at + n]), n)
        at += n
    assert crc == whole
    # the combination is a function of the CRCs and the length only, and works for a CRC value of 0xFFFFFFFF too
    assert tracefmt.crc32_combine(0xFFFFFFFF, 0xFFFFFFFF, 1) == tracefmt.crc32_combine(0xFFFFFFFF, 0xFFFFFFFF, 1)
    big = tracefmt.crc32_combine(zlib.crc32(b"ab"), zlib.crc32(bytes(1 << 20)), 1 << 20)
    assert big == zlib.crc32(b"ab" + bytes(1 << 20))


def fs_read_call(seq, offset, data, session="fs:USER/file#3", result=0, bytes_read=None, crc=None, reply=True):
    """A Read of `len(data)` bytes at `offset`; the reply wrote `data` (its CRC-32 is the token)."""
    size = len(data)
    crc = zlib.crc32(data) if crc is None else crc
    words_in = (0x080200C2, offset & 0xFFFFFFFF, offset >> 32, size, (size << 4) | 0xC, 0x0FFFDAF8)
    got = size if bytes_read is None else bytes_read
    words_out = (0x08020082, result, got, (size << 4) | 0xC, 0x0FFFDAF8) if reply else None
    token = "-" if crc is False else f"0x{crc:08x}"
    return tracefmt.parse_ipc_lines([ipc_line(1, seq, session, words_in, words_out, desc=f"mapped-w:4:{size}:-",
                                              outdesc=f"mapped-w:4:{size}:{token}" if reply else "-")]).calls[0]


FILE_BYTES = bytes((i * 7 + i // 251) & 0xFF for i in range(40000))


def piece(seq, offset, size, **kw):
    return fs_read_call(seq, offset, FILE_BYTES[offset:offset + size], **kw)


def test_touching_reads_of_one_file_form_a_run_whose_crc_is_that_of_the_whole_range():
    calls = [piece(1, 0, 4096), piece(2, 4096, 2048), piece(3, 6144, 4096)]
    runs = tracefmt.coalesce_fs_reads(calls)
    assert [[c.seq for c in run] for run in runs] == [[1, 2, 3]]
    assert tracefmt.merged_fs_read(runs[0]) == (0, 10240, f"0x{zlib.crc32(FILE_BYTES[:10240]):08x}")
    # a run of one read is that read
    one = tracefmt.coalesce_fs_reads([piece(1, 100, 64)])
    assert tracefmt.merged_fs_read(one[0]) == (100, 64, f"0x{zlib.crc32(FILE_BYTES[100:164]):08x}")


def test_a_read_that_touches_no_other_starts_a_run_of_its_own():
    gap = [piece(1, 0, 4096), piece(2, 4097, 2048)]                    # one byte missing
    overlap = [piece(1, 0, 4096), piece(2, 4095, 2048)]                # one byte read twice
    again = [piece(1, 0, 4096), piece(2, 0, 4096)]                     # the same range twice
    for calls in (gap, overlap, again):
        runs = tracefmt.coalesce_fs_reads(calls)
        assert [[c.seq for c in run] for run in runs] == [[1], [2]]
    runs = tracefmt.coalesce_fs_reads([piece(1, 0, 4096), piece(2, 9000, 100), piece(3, 9100, 100), piece(4, 4096, 10)])
    assert [[c.seq for c in run] for run in runs] == [[1, 4], [2, 3]]


def test_the_order_in_which_the_reads_were_sent_does_not_matter():
    # D52 ignores the order of the Reads: two readers that interleave differently send the same Reads in another order
    in_order = [piece(1, 0, 4096), piece(2, 4096, 2048), piece(3, 6144, 4096), piece(4, 10240, 2048)]
    for order in ([3, 0, 2, 1], [3, 2, 1, 0], [1, 0, 3, 2]):
        calls = [in_order[i] for i in order]
        runs = tracefmt.coalesce_fs_reads(calls)
        assert len(runs) == 1
        assert [c.seq for c in runs[0]] == [1, 2, 3, 4]                # a run is in offset order
        assert tracefmt.merged_fs_read(runs[0]) == (0, 12288, f"0x{zlib.crc32(FILE_BYTES[:12288]):08x}")
    # the same Reads sent twice give two runs, whichever way they are interleaved
    twice = [piece(1, 0, 100), piece(2, 100, 100), piece(3, 0, 100), piece(4, 100, 100)]
    for order in ([0, 1, 2, 3], [0, 2, 1, 3], [3, 2, 1, 0]):
        sizes = sorted(len(r) for r in tracefmt.coalesce_fs_reads([twice[i] for i in order]))
        assert sizes == [2, 2]
    # one more Read of the first chunk: the second chunk joins the older run, and the run left over holds the odd Read
    odd = [piece(1, 0, 100), piece(2, 0, 100), piece(3, 100, 100)]
    runs = tracefmt.coalesce_fs_reads(odd)
    assert sorted(len(r) for r in runs) == [1, 2]
    again = tracefmt.coalesce_fs_reads([odd[2], odd[1], odd[0]])
    assert sorted(len(r) for r in again) == [1, 2]
    key = lambda rs: sorted(tracefmt.merged_fs_read(r) for r in rs)           # noqa: E731
    assert key(runs) == key(again)                                             # the same units, whatever the order


def test_runs_are_kept_per_file_session_and_other_requests_between_reads_do_not_break_them():
    a, b = "fs:USER/file#1", "fs:USER/file#2"
    calls = [piece(1, 0, 100, session=a), piece(2, 5000, 100, session=b), piece(3, 100, 100, session=a),
             piece(4, 5100, 100, session=b), piece(5, 200, 100, session=a)]
    runs = tracefmt.coalesce_fs_reads(calls)
    assert sorted([c.seq for c in run] for run in runs) == [[1, 3, 5], [2, 4]]
    # a file of another session name (not an fs:USER file), a Write and a Close are not Reads: they are left out
    other = [piece(1, 0, 100), write_call(), piece(2, 100, 100, session="fs:USER/dir#1"), piece(3, 100, 100)]
    runs = tracefmt.coalesce_fs_reads(other)
    assert [[c.seq for c in run] for run in runs] == [[1, 3]]


def test_a_read_that_cannot_be_merged_stands_alone():
    cases = {
        "a failed read": piece(2, 4096, 100, result=0xC8804464, bytes_read=0),
        "a short read": piece(2, 4096, 100, bytes_read=60),
        "a read that never got a reply": piece(2, 4096, 100, reply=False),
        "a read without the CRC-32 of its data": piece(2, 4096, 100, crc=False),
    }
    for what, bad in cases.items():
        runs = tracefmt.coalesce_fs_reads([piece(1, 0, 4096), bad, piece(3, 4196, 100)])
        assert sorted([c.seq for c in run] for run in runs) == [[1], [2], [3]], what
        # it is no part of any run, and it does not stop the Reads on either side of it from forming one
        runs = tracefmt.coalesce_fs_reads([piece(1, 0, 4096), bad, piece(3, 4096, 100)])
        assert sorted([c.seq for c in run] for run in runs) == [[1, 3], [2]], what
    # a reply that does not say how many bytes it read cannot be trusted to have filled the buffer
    words_out = (0x08020042, 0, (100 << 4) | 0xC, 0x0FFFDAF8)           # one normal parameter only
    bare = tracefmt.parse_ipc_lines([ipc_line(1, 2, "fs:USER/file#3", (0x080200C2, 4096, 0, 100, 0x64C, 0x0FFFDAF8),
                                              words_out, desc="mapped-w:4:100:-",
                                              outdesc="mapped-w:4:100:0x00000001")]).calls[0]
    runs = tracefmt.coalesce_fs_reads([piece(1, 0, 4096), bare])
    assert sorted([c.seq for c in run] for run in runs) == [[1], [2]]
    # nor does a Read of no bytes
    runs = tracefmt.coalesce_fs_reads([piece(1, 0, 4096), fs_read_call(2, 4096, b""), piece(3, 4096, 100)])
    assert sorted([c.seq for c in run] for run in runs) == [[1, 3], [2]]


def test_a_64_bit_offset_touches_across_the_32_bit_boundary():
    low = fs_read_call(1, 0xFFFFF000, FILE_BYTES[:4096])
    high = fs_read_call(2, 0x1_0000_0000, FILE_BYTES[4096:6144])
    assert [[c.seq for c in run] for run in tracefmt.coalesce_fs_reads([low, high])] == [[1, 2]]

def test_the_prefixes_of_a_run_are_the_crc_of_the_bytes_so_far():
    calls = [piece(1, 1000, 4096), piece(2, 5096, 2048), piece(3, 7144, 100)]
    runs = tracefmt.coalesce_fs_reads(calls)
    assert len(runs) == 1
    prefixes = tracefmt.fs_run_prefixes(runs[0])
    assert [(n, size) for n, size, _ in prefixes] == [(1, 4096), (2, 6144), (3, 6244)]
    for n, size, token in prefixes:
        assert token == f"0x{zlib.crc32(FILE_BYTES[1000:1000 + size]):08x}"
    assert tracefmt.merged_fs_read(runs[0]) == (1000, 6244, prefixes[-1][2])
    # a Read that cannot be merged has none, and cannot be in a run of several
    bad = piece(4, 0, 100, bytes_read=50)
    assert tracefmt.fs_run_prefixes([bad]) == []
    with pytest.raises(ValueError):
        tracefmt.fs_run_prefixes([piece(5, 0, 100), bad])
    assert tracefmt.merged_fs_read([bad]) == (0, 100, tracefmt.fs_read_crc(bad))
    with pytest.raises(ValueError):
        tracefmt.merged_fs_read([tracefmt.parse_ipc_lines([ipc_line(1, 1, "fs:USER/file#1", (0x080200C2, 1), None)]).calls[0]])