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
