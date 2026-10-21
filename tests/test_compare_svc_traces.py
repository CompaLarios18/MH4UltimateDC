"""tools/compare_svc_traces.py on synthetic traces."""
import os
import sys

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

LINES = [
    "1 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=10",
    "2 24 pc=00200000 lr=00200100 00000001 00000000 00000000 00000000 t=15",
    "1 2B pc=001091E0 lr=001078BC 0FFFFFE8 FFFF8001 00000014 00000001 t=20",
    "1 32 pc=00106B6C lr=00106A00 00000005 00000000 00000000 00000000 t=30",
]


def write(tmp_path, name, lines):
    p = tmp_path / name
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def test_interleaving_and_ticks_dont_matter(tmp_path):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", LINES)
    # other thread order and other tick counts
    cand_lines = [LINES[0].replace("t=10", "t=11"), LINES[2], LINES[1], LINES[3].replace("t=30", "t=99")]
    cand = write(tmp_path, "cand.txt", cand_lines)
    assert c.compare(c.parse(ref), c.parse(cand)) == []
    assert c.main([ref, cand]) == 0


def test_first_difference_per_thread(tmp_path):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", LINES)
    bad = list(LINES)
    bad[2] = bad[2].replace("00000014", "00000015")             # thread 1, call 1: other argument
    cand = write(tmp_path, "cand.txt", bad[:3])                  # and thread 1 stops early
    diffs = c.compare(c.parse(ref), c.parse(cand))
    assert [(d["thread"], d["index"]) for d in diffs] == [("1", 1)]
    assert diffs[0]["ref"].args[2] == "00000014" and diffs[0]["cand"].args[2] == "00000015"
    assert c.compare(c.parse(ref), c.parse(cand), with_args=False) != []          # still shorter
    assert c.compare(c.parse(ref), c.parse(cand), with_args=False, prefix=True) == []
    assert c.main([ref, cand]) == 1


# ── N0-04: results, thread lines, normalisation ─────────────────────────────────────

HEADER = "# svctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off"
MAIN, WORKER = 0x00100000, 0x00200000


def h(v):
    return f"{v:08X}"


def line(thread, svc, args=(0, 0, 0, 0), out=None, pc=0x00100100, lr=0x00100104, t=0, in4=(0, 0, 0)):
    text = (f"{thread} {svc} pc={h(pc)} lr={h(lr)} {' '.join(h(a) for a in args)} t={t}"
            f" in4={','.join(h(a) for a in in4)}")
    if out == "-":
        return text + " out=-"
    return text + (f" out={','.join(h(a) for a in out)}" if out is not None else "")


def thread_line(tid, parent, entry):
    return f"# thread {tid} parent={parent} entry={h(entry)} arg={h(0)} stack={h(0x0FFFF000)} prio=48 core=0/0"


def session(main, worker, event, thread_handle, blocked=True):
    """The same program as run by a runtime that numbers threads `main`, `worker` and handles
    `event`, `thread_handle`. In one runtime the worker's wait blocks (its line comes after the
    main thread's later calls), in the other the event is signalled first and it doesn't."""
    wait = line(worker, "24", (event, 0, 0xFFFFFFFF, 0xFFFFFFFF), out=(0, 0, 0, 0), pc=0x00200100, lr=0x00200104)
    signal = line(main, "18", (event, 0, 0, 0), out=(0, 0, 0, 0), pc=0x00100300)
    after = line(main, "32", (0x0000DEAD + main, 0, 0, 0), out=(0, 0, 0, 0), pc=0x00100400)
    worker_end = line(worker, "0A", (1000, 0, 0, 0), out=(0, 0, 0, 0), pc=0x00200200)
    return [
        HEADER,
        thread_line(main, 0, MAIN),
        line(main, "17", (1, 0, 0, 0), out=(0, event, 0, 0), pc=0x00100200),
        line(main, "08", (0x30, WORKER, 0, 0x0F000000), out=(0, thread_handle, 0, 0), in4=(0, 0, 0), pc=0x00100250),
        thread_line(worker, main, WORKER),
        *([signal, after, wait, worker_end] if blocked else [wait, signal, after, worker_end]),
    ]


def test_header_thread_lines_and_new_fields_are_skipped_by_the_old_comparison(tmp_path):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", session(1, 2, 0x8001, 0x8002))
    # same ids and values: matches without any option, although lines carry in4= and out=
    cand = write(tmp_path, "cand.txt", session(1, 2, 0x8001, 0x8002))
    assert c.main([ref, cand]) == 0
    assert c.main([ref, cand, "--normalize", "--outputs", "--in4"]) == 0
    parsed = c.parse(ref)
    assert sorted(parsed) == ["1", "2"] and len(parsed["1"]) == 4 and len(parsed["2"]) == 2


def test_other_interleaving_and_a_blocking_call_written_late_match(tmp_path):
    import compare_svc_traces as c
    blocked = write(tmp_path, "blocked.txt", session(1, 2, 0x8001, 0x8002, blocked=True))
    quick = write(tmp_path, "quick.txt", session(1, 2, 0x8001, 0x8002, blocked=False))
    lines = open(blocked).read().splitlines()
    assert lines.index(next(l for l in lines if l.startswith("2 24"))) > lines.index(
        next(l for l in lines if l.startswith("1 32")))            # the wait's line comes after the main thread's
    for flags in ([], ["--normalize"], ["--normalize", "--outputs"]):
        assert c.main([blocked, quick] + flags) == 0, flags


def test_renumbered_threads_and_handles_need_normalize(tmp_path):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", session(1, 2, 0x8001, 0x8002))
    cand = write(tmp_path, "cand.txt", session(5, 9, 0x1000A, 0x1000C, blocked=False))
    # the thread numbers differ, so the plain comparison sees no common thread
    assert c.main([ref, cand]) == 1
    assert c.main([ref, cand, "--normalize"]) == 0
    # and the handle values are in the arguments: with normalisation they are names
    assert c.main([ref, cand, "--normalize", "--outputs", "--in4"]) == 0
    assert c.compare(c.parse(ref, True), c.parse(cand, True), with_outputs=True) == []
    assert sorted(c.parse(ref, True)) == ["00200000#0", "main"]


def test_same_thread_numbers_but_other_handle_values_differ_without_normalize(tmp_path):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", session(1, 2, 0x8001, 0x8002))
    cand = write(tmp_path, "cand.txt", session(1, 2, 0x8011, 0x8012))
    assert c.main([ref, cand]) == 1
    assert c.main([ref, cand, "--normalize"]) == 0


def test_a_different_result_is_found_only_with_outputs(tmp_path):
    import compare_svc_traces as c
    good = session(1, 2, 0x8001, 0x8002)
    bad = [l.replace("out=00000000,00000000,00000000,00000000", "out=09401BFE,00000000,00000000,00000000")
           if l.startswith("2 24") else l for l in good]
    ref, cand = write(tmp_path, "ref.txt", good), write(tmp_path, "cand.txt", bad)
    assert c.main([ref, cand, "--normalize"]) == 0
    assert c.main([ref, cand, "--normalize", "--outputs"]) == 1
    (d,) = c.compare(c.parse(ref, True), c.parse(cand, True), with_outputs=True)
    assert d["thread"] == "00200000#0" and d["index"] == 0
    assert d["ref"].out[0] == "00000000" and d["cand"].out[0] == "09401BFE"


def test_a_different_handle_in_the_results_is_found(tmp_path):
    import compare_svc_traces as c
    good = session(1, 2, 0x8001, 0x8002)
    # the candidate's main thread signals a handle that is not the event it created
    bad = [l.replace(f"pc={h(0x00100300)} lr={h(0x00100104)} {h(0x8001)}", f"pc={h(0x00100300)} lr={h(0x00100104)} {h(0x8002)}")
           if l.startswith("1 18") else l for l in good]
    ref, cand = write(tmp_path, "ref.txt", good), write(tmp_path, "cand.txt", bad)
    assert c.main([ref, cand]) == 1
    assert c.main([ref, cand, "--normalize"]) == 1
    (d,) = c.compare(c.parse(ref, True), c.parse(cand, True))
    assert d["ref"].args[0] == "event#0" and d["cand"].args[0] == "thread#0"


def test_time_dependent_values_and_unset_outputs_dont_matter_when_normalized(tmp_path):
    import compare_svc_traces as c
    tick = lambda lo: [HEADER, line(1, "28", (0, 0, 0, 0), out=(lo, 7, 0, 0), t=lo),
                       line(1, "25", (0, 0x1000, 2, 0), out=(0x09401BFE, lo, lo, lo), t=lo + 5)]
    ref, cand = write(tmp_path, "ref.txt", tick(100)), write(tmp_path, "cand.txt", tick(900))
    assert c.main([ref, cand, "--normalize", "--outputs"]) == 0
    assert c.main([ref, cand, "--outputs"]) == 1          # without normalisation the times differ


def test_in4_is_compared_only_when_asked(tmp_path):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", [line(1, "22", (0x8001, 0x1000, 0, 5), in4=(0x100, 0, 0))])
    cand = write(tmp_path, "cand.txt", [line(1, "22", (0x8001, 0x1000, 0, 5), in4=(0x200, 0, 0))])
    assert c.main([ref, cand]) == 0
    assert c.main([ref, cand, "--in4"]) == 1


def test_old_format_trace_against_the_new_one(tmp_path):
    import compare_svc_traces as c
    old = write(tmp_path, "old.txt", ["1 21 pc=00100100 lr=00100104 0FFFFFE0 00000000 00000000 00000000 t=10"])
    new = write(tmp_path, "new.txt", [HEADER, thread_line(1, 0, MAIN),
                                      line(1, "21", (0x0FFFFFE0, 0, 0, 0), out=(0, 5, 6, 7), t=99)])
    assert c.main([old, new]) == 0
    assert c.main([old, new, "--outputs", "--in4"]) == 0     # nothing to compare on the old side
    # an old trace has no thread lines to name its threads by, so --normalize leaves its numbers
    assert sorted(c.parse(old, True)) == ["1"] and sorted(c.parse(new, True)) == ["main"]


def test_a_call_still_waiting_when_the_run_ended_is_not_a_difference(tmp_path):
    import compare_svc_traces as c
    done = [thread_line(1, 0, MAIN), line(1, "24", (0x8001, 0, 5, 0), out=(0x09401BFE, 0, 0, 0))]
    cut = [thread_line(1, 0, MAIN), line(1, "24", (0x8001, 0, 5, 0), out="-")]
    ref, cand = write(tmp_path, "ref.txt", done), write(tmp_path, "cand.txt", cut)
    assert c.main([ref, cand, "--normalize", "--outputs"]) == 0
    assert c.parse(cand)["1"][0].out is None


def test_threads_with_the_same_entry_are_told_apart_by_creation_order(tmp_path):
    import compare_svc_traces as c
    def prog(a, b):
        return [thread_line(1, 0, MAIN), thread_line(a, 1, WORKER), thread_line(b, 1, WORKER),
                line(a, "0A", (111, 0, 0, 0), pc=0x00200100), line(b, "0A", (222, 0, 0, 0), pc=0x00200100)]
    ref = write(tmp_path, "ref.txt", prog(2, 3))
    same = write(tmp_path, "same.txt", prog(7, 4))        # created in the same order, other numbers
    swapped = write(tmp_path, "swapped.txt", prog(2, 3)[:3] + [prog(2, 3)[4], prog(2, 3)[3]])
    assert c.main([ref, same, "--normalize"]) == 0
    assert c.main([ref, swapped, "--normalize"]) == 0       # only the lines' order differs
    other = write(tmp_path, "other.txt", [l.replace("0000006F", "000000DE") for l in prog(2, 3)])
    assert c.main([ref, other, "--normalize"]) == 1         # the first worker slept for another time


def test_thread_tokens_that_are_not_plain_digits_do_not_crash_the_comparison(tmp_path, capsys):
    import compare_svc_traces as c
    odd = ["² 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=10",
           "9" * 5000 + " 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=10", LINES[0]]
    path = write(tmp_path, "odd.txt", odd)
    c.main([path, path])
    assert "Traceback" not in capsys.readouterr().err


def test_svc_traces_are_read_as_utf8_whatever_the_locale(tmp_path):
    import subprocess
    path = write(tmp_path, "t.txt", [LINES[0] + "  # café", *LINES[1:]])
    env = {"PATH": os.environ.get("PATH", ""), "LC_ALL": "C", "PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0"}
    tool = os.path.join(HERE, "..", "tools", "compare_svc_traces.py")
    r = subprocess.run([sys.executable, tool, path, path], env=env, capture_output=True, text=True)
    assert "Traceback" not in r.stderr, r.stderr


# ── --ignore-regs: registers a call does not define ─────────────────────────────────

def _release(handle, junk, out_junk=None, thread=1):
    """ReleaseMutex (svc 0x14): the handle in r0, a leftover in r1; r0 = result, r1 = leftover."""
    return line(thread, "14", (handle, junk, 0, 0), out=(0, out_junk if out_junk is not None else junk, 0, 0),
                pc=0x00100500)


def _write(tmp_path, name, lines):
    p = tmp_path / name
    p.write_text("\n".join(lines) + "\n")
    return str(p)


RULES = "0x14 in=r1,r2,r3 out=r1,r2,r3  # ReleaseMutex defines r0 only\n"


def test_ignore_regs_leaves_out_only_the_registers_named(tmp_path, capsys):
    import compare_svc_traces as c
    rules = tmp_path / "rules.txt"
    rules.write_text(RULES)
    ref = _write(tmp_path, "ref.txt", [HEADER, _release(7, 0x111)])
    junk = _write(tmp_path, "junk.txt", [HEADER, _release(7, 0x999)])           # r1 in and out differ
    other_handle = _write(tmp_path, "h.txt", [HEADER, _release(8, 0x111)])      # r0 differs
    assert c.main([ref, junk, "--outputs"]) == 1                                 # without the rules
    assert c.main([ref, junk, "--outputs", "--ignore-regs", str(rules)]) == 0
    assert c.main([ref, other_handle, "--outputs", "--ignore-regs", str(rules)]) == 1
    # another svc is not touched by the rule
    a = _write(tmp_path, "a.txt", [HEADER, line(1, "13", (7, 0x111, 0, 0), out=(0, 0x111, 0, 0), pc=0x00100500)])
    b = _write(tmp_path, "b.txt", [HEADER, line(1, "13", (7, 0x999, 0, 0), out=(0, 0x999, 0, 0), pc=0x00100500)])
    assert c.main([a, b, "--outputs", "--ignore-regs", str(rules)]) == 1
    # only the inputs, or only the outputs
    ins = tmp_path / "in.txt"
    ins.write_text("14 in=r1\n")
    outs_differ = _write(tmp_path, "o.txt", [HEADER, _release(7, 0x111, out_junk=0x222)])
    assert c.main([ref, junk, "--ignore-regs", str(ins)]) == 0                   # args only
    assert c.main([ref, outs_differ, "--outputs", "--ignore-regs", str(ins)]) == 1


def test_ignore_regs_rules_with_a_bad_line_or_file_exit_2(tmp_path, capsys):
    import compare_svc_traces as c
    ref = _write(tmp_path, "ref.txt", [HEADER, _release(7, 1)])
    for text in ("zz in=r1\n", "14\n", "14 in=r4\n", "14 foo=r1\n", "14 in=\n", "100 in=r1\n", "-1 in=r1\n"):
        bad = tmp_path / "bad.txt"
        bad.write_text(text)
        assert c.main([ref, ref, "--ignore-regs", str(bad)]) == 2, text
        assert "error:" in capsys.readouterr().err
    assert c.main([ref, ref, "--ignore-regs", str(tmp_path / "missing.txt")]) == 2
    binary = tmp_path / "bin.txt"
    binary.write_bytes(b"\xff\xfe\x00")
    assert c.main([ref, ref, "--ignore-regs", str(binary)]) == 2


def test_a_rule_for_a_single_digit_svc_matches_the_traces_two_digit_number(tmp_path):
    import compare_svc_traces as c
    rules = tmp_path / "rules.txt"
    for spelling in ("0x0A", "0a", "A"):
        rules.write_text(f"{spelling} in=r1 out=r1\n")
        ref = _write(tmp_path, "ref.txt", [HEADER, line(1, "0A", (5, 0x111, 0, 0), out=(0, 0x111, 0, 0), pc=0x00100500)])
        other = _write(tmp_path, "o.txt", [HEADER, line(1, "0A", (5, 0x999, 0, 0), out=(0, 0x999, 0, 0), pc=0x00100500)])
        assert c.main([ref, other, "--outputs", "--ignore-regs", str(rules)]) == 0, spelling
    assert c.main([ref, other, "--outputs"]) == 1


def test_the_repository_rule_file_is_well_formed_and_names_releasemutex():
    import compare_svc_traces as c
    root = os.path.join(HERE, "..")
    rules = c.load_register_rules([os.path.join(root, "config", "native", "svc_unused_registers.txt")])
    assert rules["14"] == {"in": {1, 2, 3}, "out": {1, 2, 3}}
    assert set(rules) == {"14"}                  # a new line is a decision: review it against svc.cpp


# ── unreadable or empty traces exit 2, not 1 ("differs") or 0 ("matches") ────────────

def test_a_missing_trace_exits_2_with_one_line(tmp_path, capsys):
    import compare_svc_traces as c
    ok = write(tmp_path, "ok.txt", LINES)
    for args in ([str(tmp_path / "nope.txt"), ok], [ok, str(tmp_path / "nope.txt")], [ok, str(tmp_path)]):
        capsys.readouterr()
        assert c.main(args) == 2
        err = capsys.readouterr().err
        assert err.startswith("error: cannot read ") and err.count("\n") == 1 and "Traceback" not in err


def test_a_trace_that_is_not_utf8_exits_2(tmp_path, capsys):
    import compare_svc_traces as c
    ok = write(tmp_path, "ok.txt", LINES)
    bad = tmp_path / "bad.bin"
    bad.write_bytes(bytes(range(128, 256)) * 3)
    assert c.main([ok, str(bad)]) == 2
    err = capsys.readouterr().err
    assert "not UTF-8 text" in err and str(bad) in err and err.count("\n") == 1


def test_empty_and_garbage_traces_are_refused(tmp_path, capsys):
    import compare_svc_traces as c
    ok = write(tmp_path, "ok.txt", LINES)
    empty = write(tmp_path, "empty.txt", [])
    only_header = write(tmp_path, "header.txt", [HEADER, "# thread 1 parent=0 entry=00100000"])
    garbage = write(tmp_path, "garbage.txt", ["hello world", "this is not a trace", "1 2 3"])
    for bad in (empty, only_header, garbage):
        for args in ([bad, bad], [bad, ok], [ok, bad]):
            capsys.readouterr()
            assert c.main(args) == 2, (bad, args)
            err = capsys.readouterr().err
            assert "holds no system call" in err and bad in err
    assert c.main([ok, ok]) == 0


def test_the_exit_codes_hold_for_the_script_itself(tmp_path):
    import subprocess
    ok = write(tmp_path, "ok.txt", LINES)
    tool = os.path.join(HERE, "..", "tools", "compare_svc_traces.py")
    r = subprocess.run([sys.executable, tool, str(tmp_path / "nope.txt"), ok], capture_output=True, text=True)
    assert r.returncode == 2 and "Traceback" not in r.stderr


# ── --collapse-polling: runs of a polling call (N0-25, D30's fallback) ───────────────────

ROOT = os.path.join(HERE, "..")
POLLING_FILE = os.path.join(ROOT, "config", "native", "svc_polling.txt")
CROSS_FILE = os.path.join(ROOT, "config", "native", "svc_cross_model.txt")
TICK_PC, TICK_LR = 0x00100800, 0x00100804
SLEEP_PC = 0x00100700


def sleep_call(ms=2, thread=1, junk=0):
    """SleepThread (0x0A): the duration in r0/r1 (ns), whatever the game's loop left in r6 (in4[2])."""
    return line(thread, "0A", (ms * 1_000_000, 0, 0, 0), out=(0, 0, 0, 0), pc=SLEEP_PC, in4=(0, 0, junk))


def tick_calls(n, thread=1, first=None, pc=TICK_PC, lr=TICK_LR):
    """n GetSystemTick calls (0x28) from one place; `first` gives the first one other registers."""
    calls = []
    for i in range(n):
        args, in4, out = (i, i + 1, i + 2, i + 3), (i, i, i), (0x1000 + i, 0x5, 0, 0)
        if i == 0 and first is not None:
            args, in4, out = first
        calls.append(line(thread, "28", args, out=out, pc=pc, lr=lr, in4=in4, t=100 + i))
    return calls


def other_call(thread=1, svc="18", handle=0x8001, out=(0, 0, 0, 0), pc=0x00100900):
    return line(thread, svc, (handle, 0, 0, 0), out=out, pc=pc)


def polling_prog(*parts):
    return [HEADER, thread_line(1, 0, MAIN), *[x for part in parts for x in (part if isinstance(part, list) else [part])]]


def polling_main(tmp_path, ref, cand, *flags):
    import compare_svc_traces as c
    return c.main([write(tmp_path, "ref.txt", ref), write(tmp_path, "cand.txt", cand),
                   "--normalize", "--outputs", "--collapse-polling", POLLING_FILE, *flags])


def test_polling_runs_of_3_and_of_1000_calls_match(tmp_path, capsys):
    ref = polling_prog(sleep_call(), tick_calls(3), other_call())
    cand = polling_prog(sleep_call(), tick_calls(1000), other_call())
    assert polling_main(tmp_path, ref, cand) == 0
    out = capsys.readouterr().out
    assert "all threads match" in out and "5 vs 1002 calls" in out and "longest 3 vs 1000 calls" in out   # the lengths are printed
    assert polling_main(tmp_path, cand, ref) == 0
    # a run of one call matches too
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(), tick_calls(1), other_call())) == 0
    # without the flag the two traces differ
    import compare_svc_traces as c
    assert c.main([str(tmp_path / "ref.txt"), str(tmp_path / "cand.txt"), "--normalize", "--outputs"]) == 1


def test_the_first_call_of_a_run_may_carry_other_registers(tmp_path):
    """The polling calls after a SleepThread carry that call's leftover registers: they are not compared."""
    leftover = ((5_000_000, 0, 0, 0), (0, 0, 0xABCDEF), (0, 0, 0, 0))
    ref = polling_prog(sleep_call(), tick_calls(3), other_call())
    cand = polling_prog(sleep_call(), tick_calls(10, first=leftover), other_call())
    assert polling_main(tmp_path, ref, cand) == 0
    assert polling_main(tmp_path, cand, ref) == 0
    assert polling_main(tmp_path, cand, ref, "--in4") == 0       # in4 of a polling call is not compared either


def test_a_run_against_no_run_differs(tmp_path, capsys):
    ref = polling_prog(sleep_call(), tick_calls(3), other_call())
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(), other_call())) == 1
    out = capsys.readouterr().out
    assert "run of 3 polling calls" in out and "svc 18" in out
    assert polling_main(tmp_path, polling_prog(sleep_call(), other_call()), ref) == 1
    # a run at the end of one trace and nothing in the other
    assert polling_main(tmp_path, polling_prog(other_call(), tick_calls(2)), polling_prog(other_call())) == 1


def test_a_non_polling_call_missing_between_two_runs_differs(tmp_path):
    ref = polling_prog(tick_calls(3), other_call(), tick_calls(3), other_call(handle=0x8002))
    merged = polling_prog(tick_calls(3), tick_calls(3), other_call(handle=0x8002))
    assert polling_main(tmp_path, ref, merged) == 1
    assert polling_main(tmp_path, ref, polling_prog(tick_calls(7), other_call(), tick_calls(1), other_call(handle=0x8002))) == 0


def test_another_output_of_a_non_polling_call_differs(tmp_path):
    ref = polling_prog(tick_calls(3), other_call(svc="17", out=(0, 0x8001, 0, 0)))
    bad = polling_prog(tick_calls(5), other_call(svc="17", out=(0xC8601801, 0x8001, 0, 0)))
    assert polling_main(tmp_path, ref, bad) == 1
    import compare_svc_traces as c
    # the outputs are compared only with --outputs, as without the collapse
    assert c.main([str(tmp_path / "ref.txt"), str(tmp_path / "cand.txt"), "--normalize", "--collapse-polling", POLLING_FILE]) == 0


def test_another_argument_or_place_of_a_non_polling_call_differs(tmp_path):
    ref = polling_prog(tick_calls(3), other_call(svc="0A", handle=1000))
    assert polling_main(tmp_path, ref, polling_prog(tick_calls(3), other_call(svc="0A", handle=2000))) == 1
    assert polling_main(tmp_path, ref, polling_prog(tick_calls(3), other_call(svc="0A", handle=1000, pc=0x00100A00))) == 1
    assert polling_main(tmp_path, ref, polling_prog(tick_calls(3), other_call(svc="0A", handle=1000))) == 0


def test_a_run_from_another_place_differs(tmp_path):
    ref = polling_prog(sleep_call(), tick_calls(3), other_call())
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(), tick_calls(3, pc=0x00100C00), other_call())) == 1
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(), tick_calls(3, lr=0x00100C00), other_call())) == 1


def test_a_longer_run_may_pass_places_the_shorter_one_never_reached(tmp_path, capsys):
    """A polling loop that runs longer passes more of its own code: the shorter run's places must all be places of
    the longer one, and both start at the same place. Runs of one length must have the same places."""
    short = polling_prog(sleep_call(), tick_calls(2), other_call())
    longer = polling_prog(sleep_call(), tick_calls(2), tick_calls(2, pc=0x00100C00), other_call())
    assert polling_main(tmp_path, short, longer) == 0
    assert polling_main(tmp_path, longer, short) == 0
    # the shorter run calls from a place the longer one never does
    other = polling_prog(sleep_call(), tick_calls(1), tick_calls(1, pc=0x00100D00), other_call())
    bigger = polling_prog(sleep_call(), tick_calls(9), other_call())
    assert polling_main(tmp_path, other, bigger) == 1
    assert polling_main(tmp_path, bigger, other) == 1
    # a longer run that starts somewhere else
    assert polling_main(tmp_path, short, polling_prog(sleep_call(), tick_calls(1, pc=0x00100C00), tick_calls(5),
                                                       other_call())) == 1
    # two runs of one length from different places
    assert polling_main(tmp_path, polling_prog(tick_calls(1), tick_calls(1, pc=0x00100C00), other_call()),
                        polling_prog(tick_calls(1), tick_calls(1, pc=0x00100D00), other_call())) == 1


def test_the_report_names_what_makes_two_polling_records_differ(tmp_path, capsys):
    ref = polling_prog(sleep_call(), tick_calls(2), other_call())
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(), tick_calls(2, pc=0x00100C00), other_call())) == 1
    out = capsys.readouterr().out
    assert "1 of 3 records differ at the same position" in out
    assert "svc 28 differs in start, places" in out


def test_the_sleep_that_precedes_a_run_is_compared_with_its_duration_but_not_its_r6(tmp_path, capsys):
    """in4 stays out of the collapse: r6 of a SleepThread holds the high word of the last GetSystemTick."""
    ref = polling_prog(sleep_call(2, junk=0x1), tick_calls(3), other_call())
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(2, junk=0x2), tick_calls(5), other_call())) == 0
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(2, junk=0x2), tick_calls(5), other_call()), "--in4") == 1
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(1, junk=0x1), tick_calls(3), other_call())) == 1
    assert "svc 0A" in capsys.readouterr().out


def test_every_record_that_differs_at_the_same_position_is_listed_with_its_fields(tmp_path, capsys):
    def prog(durations):
        parts = []
        for ms in durations:
            parts += [sleep_call(ms), tick_calls(4), other_call()]
        return polling_prog(*parts)
    ref, cand = prog([7, 5, 4, 3, 2, 9]), prog([6, 5, 4, 3, 1, 9])
    assert polling_main(tmp_path, ref, cand) == 1
    out = capsys.readouterr().out
    assert "2 of 18 records differ at the same position" in out
    assert out.count("svc 0A differs in r0") == 2
    assert "all threads match" not in out and "1 thread(s) differ" in out


def test_a_shorter_thread_is_one_difference_not_a_list(tmp_path, capsys):
    ref = polling_prog(sleep_call(), tick_calls(3), other_call(), other_call(handle=0x8002))
    assert polling_main(tmp_path, ref, polling_prog(sleep_call(), tick_calls(3), other_call())) == 1
    assert "differ at the same position" not in capsys.readouterr().out


def test_polling_file_errors_exit_2(tmp_path, capsys):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", polling_prog(tick_calls(2)))
    for text in ("zz  # nope\n", "0x28 in=r0\n", "100\n", "0x28\n0x28\n", "# only comments\n"):
        bad = tmp_path / "bad.txt"
        bad.write_text(text)
        capsys.readouterr()
        assert c.main([ref, ref, "--normalize", "--collapse-polling", str(bad)]) == 2, text
        assert "error:" in capsys.readouterr().err
    assert c.main([ref, ref, "--collapse-polling", str(tmp_path / "missing.txt")]) == 2
    assert c.main([ref, ref, "--collapse-polling", POLLING_FILE, "--cross-model", CROSS_FILE]) == 2


def test_the_repository_polling_file_lists_only_get_system_tick():
    import compare_svc_traces as c
    assert c.load_polling(POLLING_FILE) == {"28"}


# ── --cross-model: the calls compared between two timing models (N0-25, D31) ────────────

P1, P2, EW = 0x00300000, 0x00400000, 0x00500000


def ctl(thread=1, size=0x1000, perm=3, op=3, addr=0x08000000, hint=0, result=0):
    """ControlMemory (0x01): r0 operation, r1/r2 addresses, r3 size, r4 permissions; out r1 the address."""
    return line(thread, "01", (op, hint, 0, size), out=(result, addr, 0, 0), pc=0x00100200, in4=(perm, 0, 0))


def make(kind, thread, handle, flag=0, pc=0x00100300, result=0):
    """CreateEvent (17) / CreateMutex (13): the flag in r1, the output pointer's slot (r0) a stack address."""
    return line(thread, kind, (0x0FFFF000 + thread, flag, 0, 0), out=(result, handle, 0, 0), pc=pc)


def spawn_call(thread, handle, entry, prio=0x30, arg=0, stack=0x0F000000, core=0xFFFFFFFE, pc=0x00100400):
    return line(thread, "08", (prio, entry, arg, stack), out=(0, handle, 0, 0), pc=pc, in4=(core, 0, 0))


def close(thread, handle, pc=0x00100500):
    return line(thread, "23", (handle, 0, 0, 0), out=(0, 0, 0, 0), pc=pc)


def wait_timeout(thread, handle, pc=0x00100600):
    return line(thread, "24", (handle, 0, 5, 0), out=(0x09401BFE, 0, 0, 0), pc=pc)


def cross_prog(main=1, worker=2, event=0x8001, worker_handle=0x8002, mutex=0x8003, extra=False, **ctl_kw):
    lines = [HEADER, thread_line(main, 0, MAIN), ctl(main, **ctl_kw), make("17", main, event, flag=1)]
    if extra:
        lines += [wait_timeout(main, event), sleep_call(1, main), line(main, "28", out=(7, 7, 0, 0), pc=0x00100610)]
    lines += [spawn_call(main, worker_handle, WORKER, arg=0x0E000000 + main, stack=0x0F000000 + worker),
              thread_line(worker, main, WORKER), make("13", worker, mutex, flag=0, pc=0x00200300)]
    if extra:
        lines += [wait_timeout(worker, mutex, 0x00200600), sleep_call(2, worker)]
    lines += [close(worker, mutex, 0x00200500), line(worker, "09", pc=0x00200700), close(main, event)]
    return lines


def cross_main(tmp_path, ref, cand, *flags, rules=CROSS_FILE):
    import compare_svc_traces as c
    return c.main([write(tmp_path, "ref.txt", ref), write(tmp_path, "cand.txt", cand),
                   "--outputs", "--cross-model", rules, *flags])


def test_extra_waits_sleeps_and_clock_reads_pass_across_models(tmp_path, capsys):
    ref, cand = cross_prog(), cross_prog(main=5, worker=9, event=0x1000A, worker_handle=0x1000C, mutex=0x1000E, extra=True)
    assert cross_main(tmp_path, ref, cand) == 0
    out = capsys.readouterr().out
    # per thread: the compared calls, then each side's total
    assert "thread main: compared calls 4 vs 4 (all calls 4 vs 7)" in out
    assert "all threads match" in out
    # the plain comparison sees the extra calls
    import compare_svc_traces as c
    assert c.main([str(tmp_path / "ref.txt"), str(tmp_path / "cand.txt"), "--normalize", "--outputs"]) == 1


def test_cross_model_implies_normalize(tmp_path):
    ref, cand = cross_prog(), cross_prog(main=5, worker=9, event=0x1000A, worker_handle=0x1000C, mutex=0x1000E)
    assert cross_main(tmp_path, ref, cand) == 0           # other thread numbers and handle values, no --normalize


def test_creations_interleaved_differently_across_two_threads_pass(tmp_path):
    def prog(order):
        creations = {"m": make("17", 1, 0x8001 if order == "mw" else 0x8002),
                     "w": make("17", 2, 0x8002 if order == "mw" else 0x8001, pc=0x00200300)}
        closes = [close(1, 0x8001 if order == "mw" else 0x8002), close(2, 0x8002 if order == "mw" else 0x8001, 0x00200500)]
        return [HEADER, thread_line(1, 0, MAIN), spawn_call(1, 0x8010, WORKER), thread_line(2, 1, WORKER),
                *[creations[k] for k in order], *closes]
    assert cross_main(tmp_path, prog("mw"), prog("wm")) == 0
    assert cross_main(tmp_path, prog("wm"), prog("mw")) == 0
    # with the plain names the handles of the two events swap and the closes differ
    import compare_svc_traces as c
    assert c.main([str(tmp_path / "ref.txt"), str(tmp_path / "cand.txt"), "--normalize", "--outputs"]) == 1


def test_two_threads_with_one_entry_created_by_different_parents_in_the_opposite_order_pass(tmp_path):
    def prog(order, flags=None):
        """P1 and P2 each create a thread with entry EW; `order` says whose child is created first. The child
        of P1 creates a mutex with flag 1, the child of P2 one with flag 0."""
        flags = flags or {"1": 1, "2": 0}
        kids = {"1": (2, 0x00300400), "2": (3, 0x00400400)}        # parent thread, pc of its CreateThread
        lines = [HEADER, thread_line(1, 0, MAIN), spawn_call(1, 0x8010, P1), thread_line(2, 1, P1),
                 spawn_call(1, 0x8011, P2, pc=0x00100410), thread_line(3, 1, P2)]
        for n, key in enumerate(order):
            parent, pc = kids[key]
            child = 10 + n
            lines += [spawn_call(parent, 0x8020 + n, EW, pc=pc), thread_line(child, parent, EW),
                      make("13", child, 0x8030 + n, flag=flags[key], pc=0x00500300)]
        return lines
    assert cross_main(tmp_path, prog("12"), prog("21")) == 0
    assert cross_main(tmp_path, prog("21"), prog("12")) == 0
    # the global names `<entry>#k` swap the two children, so the plain comparison fails
    import compare_svc_traces as c
    assert c.main([str(tmp_path / "ref.txt"), str(tmp_path / "cand.txt"), "--normalize", "--outputs"]) == 1
    # and a real difference between the children is still found
    assert cross_main(tmp_path, prog("12"), prog("21", {"1": 0, "2": 0})) == 1


def test_a_control_memory_that_returns_another_address_passes(tmp_path):
    assert cross_main(tmp_path, cross_prog(), cross_prog(addr=0x08123000)) == 0
    # the addresses it takes as inputs (a hint, a block freed or mapped) are kernel-chosen too
    assert cross_main(tmp_path, cross_prog(), cross_prog(hint=0x08444000)) == 0


def test_a_control_memory_with_another_size_permission_operation_or_result_fails(tmp_path):
    for kw in ({"size": 0x2000}, {"perm": 1}, {"op": 1}, {"result": 0xE0E01BF5}):
        assert cross_main(tmp_path, cross_prog(), cross_prog(**kw)) == 1, kw


def test_a_missing_creation_or_close_fails(tmp_path):
    ref = cross_prog()
    no_event = [l for l in ref if not l.startswith("1 17 ")]
    assert len(no_event) == len(ref) - 1
    assert cross_main(tmp_path, ref, no_event) == 1
    assert cross_main(tmp_path, no_event, ref) == 1
    no_close = [l for l in ref if not l.startswith("1 23 ")]
    assert cross_main(tmp_path, ref, no_close) == 1
    other_kind = [l.replace(" 17 ", " 13 ") if l.startswith("1 17 ") else l for l in ref]
    assert cross_main(tmp_path, ref, other_kind) == 1


def test_a_missing_thread_fails(tmp_path, capsys):
    ref = cross_prog()
    no_worker = [l for l in ref if not l.startswith("2 ") and not l.startswith("# thread 2 ")]
    assert cross_main(tmp_path, ref, no_worker) == 1
    out = capsys.readouterr().out
    assert "thread main/00200000#0" in out and "differ" in out
    assert cross_main(tmp_path, no_worker, ref) == 1
    # a thread that ends later in one trace: both sides' sequences are compared whole
    no_exit = [l for l in ref if not l.startswith("2 09 ")]
    assert cross_main(tmp_path, ref, no_exit) == 1


def test_other_arguments_of_a_creation_fail_but_heap_addresses_do_not(tmp_path):
    ref = cross_prog()
    # CreateThread: the argument and the stack top are heap addresses
    heap = [l.replace(h(0x0E000001), h(0x0E0000AA)).replace(h(0x0F000002), h(0x0F0000BB)) if l.startswith("1 08 ") else l
            for l in ref]
    assert heap != ref and cross_main(tmp_path, ref, heap) == 0
    prio = [l.replace(f"{h(0x30)} {h(WORKER)}", f"{h(0x31)} {h(WORKER)}") if l.startswith("1 08 ") else l for l in ref]
    assert prio != ref and cross_main(tmp_path, ref, prio) == 1
    entry = [l.replace(h(WORKER), h(WORKER + 4), 1) if l.startswith("1 08 ") else l for l in ref]
    assert cross_main(tmp_path, ref, entry) == 1
    # CreateEvent: the reset type counts, the output pointer's slot does not
    reset = [l.replace(f"{h(0x0FFFF001)} {h(1)}", f"{h(0x0FFFF999)} {h(2)}") if l.startswith("1 17 ") else l for l in ref]
    assert reset != ref and cross_main(tmp_path, ref, reset) == 1
    slot = [l.replace(h(0x0FFFF001), h(0x0FFFF999)) if l.startswith("1 17 ") else l for l in ref]
    assert slot != ref and cross_main(tmp_path, ref, slot) == 0


def test_the_result_code_is_compared_without_outputs(tmp_path):
    import compare_svc_traces as c
    ref = cross_prog()
    bad = [l.replace("out=00000000,00008001", "out=C8601801,00008001") if l.startswith("1 17 ") else l for l in ref]
    assert bad != ref
    a, b = write(tmp_path, "a.txt", ref), write(tmp_path, "b.txt", bad)
    assert c.main([a, b, "--cross-model", CROSS_FILE]) == 1


def test_an_empty_filter_exits_3(tmp_path, capsys):
    import compare_svc_traces as c
    only_waits = [HEADER, thread_line(1, 0, MAIN), wait_timeout(1, 0x8001), sleep_call(1)]
    assert cross_main(tmp_path, only_waits, only_waits) == 3
    out = capsys.readouterr().out
    assert "NOTHING COMPARED" in out
    # an empty list of calls compares nothing either
    empty = tmp_path / "empty.txt"
    empty.write_text("# no call listed\n")
    assert cross_main(tmp_path, cross_prog(), cross_prog(), rules=str(empty)) == 3
    # a difference wins over 3: one side has a listed call and the other has none
    assert cross_main(tmp_path, only_waits, cross_prog()) == 1


def test_cross_model_file_errors_exit_2(tmp_path, capsys):
    import compare_svc_traces as c
    ref = write(tmp_path, "ref.txt", cross_prog())
    for text in ("zz in=r1\n", "100 in=r1\n", "0x13 in=r7\n", "0x13 out=r4\n", "0x13 foo=r1\n", "0x13 in=\n",
                 "0x13 in=r1\n0x13 in=r1\n", "0x13 in=r1 in=r2\n", "0x13 r1\n"):
        bad = tmp_path / "bad.txt"
        bad.write_text(text)
        capsys.readouterr()
        assert c.main([ref, ref, "--cross-model", str(bad)]) == 2, text
        assert "error:" in capsys.readouterr().err
    assert c.main([ref, ref, "--cross-model", str(tmp_path / "missing.txt")]) == 2
    for flag in ("--in4", "--ignore-args"):
        assert c.main([ref, ref, "--cross-model", CROSS_FILE, flag]) == 2, flag


def test_the_repository_cross_model_file_follows_the_criterion():
    import compare_svc_traces as c
    rules = c.load_cross_model_rules(CROSS_FILE)
    assert set(rules) == {"01", "08", "09", "13", "15", "17", "1A", "1E", "1F", "20", "21", "23", "27", "2D"}
    # waits, sleeps, signals, arbitration, the clock and SendSyncRequest are never compared
    assert not set(rules) & {"0A", "18", "19", "22", "24", "25", "28", "32", "14", "16"}
    # no address the kernel chooses: ControlMemory's r1 and r2 inputs and its r1 output
    assert rules["01"].inputs == {0, 3, 4} and rules["01"].outputs == {0}
    # the other placements: a memory block's address, a thread's stack top and argument, a port name
    assert 1 not in rules["1E"].inputs and 1 not in rules["1F"].inputs and 1 not in rules["20"].inputs
    assert not rules["08"].inputs & {2, 3} and 1 not in rules["2D"].inputs
    # a handle that is created is compared by name
    for svc in ("08", "13", "15", "17", "1A", "1E", "21", "27", "2D"):
        assert {0, 1} <= rules[svc].outputs, svc
    # every line says why
    for n, text in enumerate(open(CROSS_FILE, encoding="utf-8"), 1):
        body = text.strip()
        if body and not body.startswith("#"):
            assert "#" in body and body.split("#", 1)[1].strip(), f"line {n} has no reason"


def test_the_load_of_a_cross_model_file_keeps_the_registers(tmp_path):
    import compare_svc_traces as c
    path = tmp_path / "rules.txt"
    path.write_text("0x1F in=r0,r4,r6 out=r0,r3  # r4 to r6 are the extra inputs\n0x09  # no registers\n")
    rules = c.load_cross_model_rules(str(path))
    assert rules["1F"].inputs == {0, 4, 6} and rules["1F"].outputs == {0, 3}
    assert rules["09"].inputs == set() and rules["09"].outputs == set()


def test_the_cross_model_comparison_uses_in4_for_r4_to_r6(tmp_path):
    path = tmp_path / "rules.txt"
    path.write_text("0x22 in=r4,r6  # arbitrate, for the test only\n")
    def prog(r4, r5, r6):
        return [HEADER, thread_line(1, 0, MAIN), line(1, "22", (0x8001, 0, 0, 0), out=(0, 0, 0, 0), in4=(r4, r5, r6))]
    assert cross_main(tmp_path, prog(1, 2, 3), prog(1, 99, 3), rules=str(path)) == 0      # r5 is not listed
    assert cross_main(tmp_path, prog(1, 2, 3), prog(2, 2, 3), rules=str(path)) == 1
    assert cross_main(tmp_path, prog(1, 2, 3), prog(1, 2, 4), rules=str(path)) == 1
