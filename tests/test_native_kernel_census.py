"""Tests for tools/native/kernel_census.py (N0-12), on synthetic traces."""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "native"))

import kernel_census as kc  # noqa: E402
import tracefmt  # noqa: E402

HEADER = "# svctrace v1 2026-01-01 game=0x0004000000000000 cpu=recomp new3ds=off"
MARKER = "32:00001000:00002000"


def call(tid, svc, r=("0", "0", "0", "0"), in4=("0", "0", "0"), out=("0", "0", "0", "0"), pc="00001234", lr="00005678",
         t=100):
    regs = " ".join(f"{int(x, 16):08X}" for x in r)
    o = "-" if out is None else ",".join(f"{int(x, 16):08X}" for x in out)
    return f"{tid} {svc} pc={pc} lr={lr} {regs} t={t} in4={','.join(f'{int(x, 16):08X}' for x in in4)} out={o}"


def census(lines, ipc=None, symbols=None):
    c = kc.SvcCensus(symbols, ipc, MARKER)
    for line in [HEADER] + lines:
        c.feed(line)
    return c.report()


def thread_line(tid, parent, entry, prio, core):
    return f"# thread {tid} parent={parent} entry={entry} arg=00000000 stack=10000000 prio={prio} core={core}"


def test_threads_named_by_creation_order_and_symbols():
    lines = [
        thread_line(1, 0, "00100000", 48, "0/0"),
        call(1, "08", ("18", "10D8D4", "0", "0"), ("FFFFFFFE", "0", "0"), ("0", "28009", "0", "0")),
        thread_line(7, 1, "0010D8D4", 24, "-2/1"),
        thread_line(3, 1, "0010D8D4", 47, "-2/0"),
        call(7, "28"), call(3, "28"), call(7, "28"),
    ]
    r = census(lines, symbols={"0010D8D4": "thread_entry"})
    names = [t["name"] for t in r["threads"]]
    assert names == ["main", "0010D8D4#0", "0010D8D4#1"]
    t0 = r["threads"][1]
    assert (t0["id"], t0["priority"], t0["core_requested"], t0["core_resolved"], t0["parent"]) == ("7", 24, -2, 1, "main")
    assert t0["entry_symbol"] == "thread_entry"
    assert r["threads"][0]["share"] == 0.25 and r["threads"][1]["calls"] == 2


def test_svc_counts_and_code_comparison(tmp_path):
    tsv = tmp_path / "services.tsv"
    tsv.write_text("function\tmode\tservice\theader\tcommand\tcallers\tsite\thandle\tnote\n"
                   "0x00100000\tarm\tsvc\t0x28\tGetSystemTick\t1\t0x00100004\t\tinline\n"
                   "0x00100010\tarm\tsvc\t0x3c\tBreak\t1\t0x00100014\t\tinline\n"
                   "0x00100020\tarm\tAPT:U?\t0x004f0080\tX\t1\t0x0\t\t\n", encoding="utf-8")
    code = kc.load_code_svcs(str(tsv))
    assert set(code) == {"28", "3C"}
    c = kc.SvcCensus(None, None, MARKER)
    for line in [HEADER, call(1, "28"), call(1, "28"), call(1, "0A")]:
        c.feed(line)
    r = c.report(code)
    assert r["svcs"]["in_trace"]["28"]["calls"] == 2
    assert r["svcs"]["in_code_not_in_trace"] == ["3C"] and r["svcs"]["in_trace_not_in_code"] == ["0A"]


def test_control_memory_and_resets():
    r = census([
        call(1, "01", ("10003", "8000000", "0", "1000"), ("3", "0", "0")),
        call(1, "01", ("10003", "8000000", "0", "1000"), ("3", "0", "0")),
        call(1, "01", ("3", "0", "0", "2000"), ("1", "0", "0")),
        call(1, "17", ("0", "1", "0", "0"), out=("0", "50001", "0", "0")),
        call(1, "1A", ("0", "2", "0", "0"), out=("0", "50002", "0", "0")),
        call(1, "13", ("0", "1", "0", "0"), out=("0", "50003", "0", "0")),
        call(1, "15", ("0", "0", "4", "0"), out=("0", "50004", "0", "0")),
    ])
    cm = r["control_memory"]
    assert cm["operations"] == {"op=0x03 linear=yes": 2, "op=0x03 linear=no": 1}
    assert cm["permissions"] == {"0x3": 2, "0x1": 1}
    assert cm["bytes_by_operation"]["op=0x03 linear=yes"] == 0x2000
    assert r["reset_types"] == {"event": {"sticky": 1}, "timer": {"pulse": 1}}
    assert r["mutex_initial"] == {"locked": 1} and r["semaphores"] == {"initial=0 max=4": 1}


def test_arbitrate_waits_sleeps_and_info_types():
    inf = ("FFFFFFFF", "FFFFFFFF")
    r = census([
        call(1, "22", ("1", "8000000", "0", "1"), ("0", "0", "0")),                  # Signal
        call(1, "22", ("1", "8000000", "1", "1"), ("0", "0", "0")),                  # WaitIfLess, no timeout field
        call(1, "22", ("1", "8000000", "3", "1"), ("0", "0", "0")),                  # with timeout 0
        call(1, "22", ("1", "8000000", "4", "1"), ("1000", "0", "0")),               # finite
        call(1, "22", ("1", "8000000", "3", "1"), ("FFFFFFFF", "FFFFFFFF", "0")),    # infinite
        call(1, "24", ("50001", "0", "0", "0")),                                     # poll
        call(1, "24", ("50001", "0", "3B9ACA00", "0")),                              # finite
        call(1, "24", ("50001", "0") + inf),                                         # infinite
        call(1, "25", ("FFFFFFFF", "100", "3", "0"), ("FFFFFFFF", "0", "0")),        # N, infinite
        call(1, "0A", ("0", "0")), call(1, "0A", ("3E8", "0")), call(1, "0A", ("5F5E100", "0")),
        call(1, "2A", ("0", "0", "0", "0")), call(1, "2B", ("0", "FFFF8001", "14", "0")),
        call(1, "2C", ("0", "FFFF8000", "10000", "0")),
        call(1, "29", ("0", "50001", "1", "0")),
    ])
    assert r["info_types"]["GetHandleInfo"] == {"type=0x1": 1}
    a = r["arbitrate_address"]
    assert a["0 Signal timeout=n/a"] == 1 and a["1 WaitIfLess timeout=n/a"] == 1
    assert a["3 WaitIfLessTimeout timeout=0"] == 1 and a["3 WaitIfLessTimeout timeout=infinite"] == 1
    assert a["4 DecrementAndWaitIfLessTimeout timeout=finite"] == 1
    assert r["wait_synchronization1_timeouts"] == {"0": 1, "finite": 1, "infinite": 1}
    assert list(r["wait_synchronization_n"]) == ["timeout=infinite count=3 wait_all=0"]
    assert r["sleep_thread"]["classes"] == {"0": 1, "under 1 ms": 1, "1 ms or more": 1}
    assert r["info_types"]["GetSystemInfo"] == {"type=0x0 param=0x0": 1}
    assert r["info_types"]["GetProcessInfo"] == {"type=0x14": 1}
    assert r["info_types"]["GetThreadInfo"] == {"type=0x10000": 1}


def test_wait_kind_from_created_handles_and_ipc():
    ipc = tracefmt.parse_ipc_lines([
        "# ipctrace v1 2026-01-01 game=0x0004000000000000 cpu=recomp new3ds=off",
        "1 seq=1 session=APT:A cmd=0x00010040 in=0x00010040,0x0 desc=- out=0x000100c2,0x0,0x0,0x0,0x0,0x0003800b "
        "outdesc=copy:4:1:APT:A/mutex#0 t=5",
    ])
    r = census([
        thread_line(1, 0, "00100000", 48, "0/0"),
        call(1, "17", ("0", "0", "0", "0"), out=("0", "50001", "0", "0")),
        call(1, "32", ("8002", "0", "0", "0"), out=("0", "0", "0", "0")),
        call(1, "24", ("50001", "0", "FFFFFFFF", "FFFFFFFF")),
        call(1, "24", ("3800B", "0", "FFFFFFFF", "FFFFFFFF")),
        call(1, "24", ("9999", "0", "FFFFFFFF", "FFFFFFFF")),
    ], ipc=ipc)
    waits = r["threads"][0]["waits"]
    assert waits["WaitSynchronization1 event timeout=infinite"] == 1
    assert waits["WaitSynchronization1 mutex timeout=infinite"] == 1
    assert waits["WaitSynchronization1 unknown timeout=infinite"] == 1
    assert waits["SendSyncRequest APT:A"] == 1


def test_tick_polling_per_frame_and_runs():
    pc, lr = "00001000", "00002000"
    marker = call(1, "32", pc=pc, lr=lr)
    lines = [thread_line(1, 0, "00100000", 48, "0/0"), thread_line(2, 1, "00200000", 30, "-2/1")]
    for n in (3, 1, 5):                 # frames with 3, 1, 5 ticks on the main thread
        lines += [call(1, "28")] * n + [call(2, "28"), marker]
    lines += [call(1, "28"), call(1, "28"), call(1, "0A"), call(1, "28")]
    r = census(lines)
    assert r["frames"] == 3
    main = r["threads"][0]["get_system_tick"]
    assert main["per_frame_median"] == 3 and main["per_frame_max"] == 5
    assert main["longest_run"] == 5 and main["calls"] == 12
    # runs on the main thread: 3, 1, 5, 2, 1 (the marker and the sleep end a run; thread 2's ticks do not)
    assert main["calls_in_runs_of_at_least"]["2"] == 10
    other = r["threads"][1]["get_system_tick"]
    assert other["per_frame_median"] == 1 and other["longest_run"] == 3    # its three ticks follow each other
    assert main["top_sites"][0]["pc"] == "00001234"


def test_tick_runs_are_per_thread_and_split_by_other_calls():
    lines = [thread_line(1, 0, "00100000", 48, "0/0"), thread_line(2, 1, "00200000", 30, "0/0"),
             call(1, "28"), call(2, "28"), call(1, "28"), call(2, "0A"), call(2, "28")]
    r = census(lines)
    assert r["threads"][0]["get_system_tick"]["longest_run"] == 2     # thread 2's calls do not break it
    assert r["threads"][1]["get_system_tick"]["longest_run"] == 1


def test_get_thread_id_handle_classes_and_call_sites():
    r = census([
        thread_line(1, 0, "00100000", 48, "0/0"),
        call(1, "27", ("0", "FFFF8000", "0", "0"), out=("0", "10006", "0", "0")),        # pseudo -> real handle
        call(1, "08", ("18", "10D8D4", "0", "0"), out=("0", "28009", "0", "0")),
        call(1, "37", ("0", "FFFF8000", "0", "0"), lr="00BFA858"),
        call(1, "37", ("0", "FFFF8000", "0", "0"), lr="00BFA858"),
        call(1, "37", ("0", "10006", "0", "0"), lr="00BFA858"),
        call(1, "37", ("0", "28009", "0", "0"), lr="00BFA874"),
        call(1, "37", ("0", "7777", "0", "0"), lr="00BFA874"),
        call(1, "23", ("10006", "0", "0", "0")),                                          # closed: no longer a duplicate
        call(1, "37", ("0", "10006", "0", "0"), lr="00BFA874"),
    ])
    assert r["get_thread_id_handles"] == {"pseudo handle FFFF8000": 2, "other handle": 2,
                                          "handle made by DuplicateHandle": 1, "handle made by CreateThread": 1}
    assert r["duplicate_handle_sources"] == {"pseudo handle FFFF8000": 1}
    top = r["top_call_sites"]["GetThreadId (37)"]
    assert top["calls"] == 6 and top["callers_distinct"] == 2 and top["top_share"] == 1.0
    assert top["top"] == [{"lr": "00BFA858", "calls": 3}, {"lr": "00BFA874", "calls": 3}]


def test_finite_waits_counted_by_thread_priority():
    ms5 = ("50001", "0", "4C4B3E", "0")           # 4,999,998 ns
    lines = [thread_line(1, 0, "00100000", 48, "0/0"), thread_line(2, 1, "0010D8D4", 42, "-2/0"),
             thread_line(3, 1, "0010D8D4", 42, "-2/0"), thread_line(4, 1, "0010D8D4", 26, "-2/0"),
             call(2, "24", ms5), call(2, "24", ms5), call(3, "24", ms5), call(4, "24", ("50001", "0", "FFFFFFFF", "FFFFFFFF"))]
    r = census(lines)
    assert r["finite_wait_threads"] == {"4999998": {"threads": 2, "calls": 3, "by_priority": {"42": 2}}}


def test_unfinished_calls_and_exit():
    r = census([thread_line(1, 0, "00100000", 48, "0/0"), call(1, "24", out=None), call(1, "09")])
    t = r["threads"][0]
    assert t["unfinished_calls"] == 1 and t["exited"] is True


def test_ipc_report_per_service_and_apt_limit():
    trace = tracefmt.parse_ipc_lines([
        "# ipctrace v1 2026-01-01 game=0x0004000000000000 cpu=recomp new3ds=off",
        "1 seq=1 session=APT:A cmd=0x004f0080 in=0x004f0080,0x00000001,0x00000019 desc=- out=0x004f0040,0x00000000 "
        "outdesc=- t=1",
        "1 seq=2 session=fs:USER/file#3 cmd=0x080200c2 in=0x080200c2,0x0,0x0,0x10 desc=- out=0x080200c2,0xc8804464 "
        "outdesc=- t=2",
        "2 seq=3 session=fs:USER/file#4 cmd=0x080200c2 in=0x080200c2,0x0,0x0,0x10 desc=- out=0x080200c2,0x00000000 "
        "outdesc=- t=3",
        "2 seq=4 session=gsp::Gpu cmd=0x000c0000 in=0x000c0000 desc=- out=- outdesc=- t=4",
    ])
    r = kc.ipc_report(trace)
    assert r["apt_set_cpu_time_limit"] == [{"thread": "1", "seq": 1, "word1": "0x00000001", "percent": 25,
                                            "result": "0x00000000"}]
    fs = r["services"]["fs:USER/file"]["0x080200c2"]
    assert fs["count"] == 2 and fs["results"] == {"0xc8804464": 1, "0x00000000": 1}
    assert r["services"]["gsp::Gpu"]["0x000c0000"]["results"] == {"no-reply": 1}


def test_command_line_writes_json_and_markdown(tmp_path):
    d = tmp_path / "G0"
    d.mkdir()
    (d / "svc.txt").write_text("\n".join([HEADER, thread_line(1, 0, "00100000", 48, "0/0"), call(1, "28")]) + "\n",
                               encoding="utf-8")
    (d / "ipc.txt").write_text("# ipctrace v1 2026-01-01 game=0x0004000000000000 cpu=recomp new3ds=off\n"
                               "1 seq=1 session=srv: cmd=0x00010002 in=0x00010002,0x20 desc=- out=0x00010040,0x0 "
                               "outdesc=- t=1\n", encoding="utf-8")
    out = tmp_path / "out"
    rc = kc.main(["--svc", str(d / "svc.txt"), "--ipc", str(d / "ipc.txt"), "--services", str(tmp_path / "none.tsv"),
                  "--out", str(out)])
    assert rc == 0
    data = json.loads((out / "census.json").read_text(encoding="utf-8"))
    assert list(data["svc_traces"]) == ["G0"] and data["svc_traces"]["G0"]["calls"] == 1
    assert data["ipc"]["G0"]["services"]["srv:"]["0x00010002"]["count"] == 1
    md = (out / "census.md").read_text(encoding="utf-8")
    assert "## G0: 1 SVC calls" in md and "### srv:" in md
    assert kc.main(["--svc", str(tmp_path / "missing.txt")]) == 2
