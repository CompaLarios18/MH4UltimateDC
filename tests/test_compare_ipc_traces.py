"""tools/compare_ipc_traces.py: IPC trace comparison (synthetic traces only: made-up services,
made-up words and CRCs; nothing here comes from the game)."""
import os
import sys

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import compare_ipc_traces as cmp  # noqa: E402
import tracefmt  # noqa: E402

HEADER = "# ipctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off"


def w(v: int) -> str:
    return f"0x{v:08x}"


def thread_line(tid, parent, entry):
    return f"# thread {tid} parent={parent} entry={entry:08X} arg=00000000 stack=0FFFF000 prio=48 core=0/0"


def req(thread, seq, session, cmd, result=0, params=(), desc=(), outdesc=(), out_extra=(), in_extra=(),
        t=0, reply=True, data=()):
    """One IPC trace line. `params`: the normal request parameters; `in_extra`/`out_extra`: the
    translate words (descriptors and what follows them); `desc`/`outdesc`: ready-made entries."""
    n_in = len(in_extra)
    words_in = [cmd | (len(params) << 6) | n_in] + list(params) + list(in_extra)
    text = (f"{thread} seq={seq} session={session} cmd={w(words_in[0])} in={','.join(w(x) for x in words_in)} "
            f"desc={','.join(desc) or '-'} ")
    if reply:
        # reply header: the command id, one normal parameter (the result), len(out_extra) translate words
        words_out = [(cmd & 0xFFFF0000) | (1 << 6) | len(out_extra), result] + list(out_extra)
        text += f"out={','.join(w(x) for x in words_out)} outdesc={','.join(outdesc) or '-'} "
    else:
        text += "out=- outdesc=- "
    text += f"t={t}"
    for index, hexbytes in data:
        text += f" data={index}:{hexbytes}"
    return text


def trace(*lines):
    return [HEADER, thread_line(1, 0, 0x100000), thread_line(5, 1, 0x200000), *lines]


def write(tmp_path, name, lines):
    p = tmp_path / name
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def run(tmp_path, ref, cand, *args):
    return cmp.main([write(tmp_path, "ref.txt", ref), write(tmp_path, "cand.txt", cand), *args])


BASE = trace(
    req(1, 1, "srv:", 0x00010000, params=(0x7,), t=10),
    req(1, 2, "fs:USER", 0x00020000, desc=("static:1:16:0x11223344",), t=20),
    req(5, 1, "gsp::Gpu", 0x00030000, t=30),
    req(5, 2, "gsp::Gpu", 0x00040000, outdesc=("static:1:8:0xaabbccdd",), t=40),
    req(1, 3, "fs:USER", 0x00050000, t=50),
)


def test_identical_traces_match(tmp_path, capsys):
    assert run(tmp_path, BASE, BASE) == 0
    out = capsys.readouterr().out
    assert "all threads match" in out and "main: 3 vs 3" in out


def test_a_different_result_does_not_match_and_the_report_has_no_values(tmp_path, capsys):
    cand = list(BASE)
    cand[3 + 1] = req(1, 2, "fs:USER", 0x00020000, result=0xC8804464, desc=("static:1:16:0x11223344",), t=20)
    assert run(tmp_path, BASE, cand) == 1
    out = capsys.readouterr().out
    assert "thread main differs at request 1" in out and "differs in: result" in out
    assert "result=0x00000000" in out and "result=0xc8804464" in out
    assert "0x00000007" not in out                  # request words are never printed


def test_a_different_interleaving_matches(tmp_path):
    cand = trace(BASE[3], BASE[5], BASE[4], BASE[6], BASE[7])   # same threads, other file order
    assert cmp.main([write(tmp_path, "a.txt", BASE), write(tmp_path, "b.txt", cand)]) == 0


def test_renumbered_threads_match(tmp_path):
    cand = [HEADER, thread_line(2, 0, 0x100000), thread_line(9, 2, 0x200000)] + [
        line.replace("1 seq=", "2 seq=", 1) if line.startswith("1 ") else line.replace("5 seq=", "9 seq=", 1)
        for line in BASE[3:]]
    assert cmp.main([write(tmp_path, "a.txt", BASE), write(tmp_path, "b.txt", cand)]) == 0


def test_renumbered_handles_match_even_when_strict(tmp_path):
    def with_handle(raw, session_name):
        # the reply hands out one session (words: header, result, descriptor, handle); a later request
        # passes it back (words: header, descriptor, handle)
        return trace(
            req(1, 1, "fs:USER", 0x00080000, out_extra=(0x00000000, raw), outdesc=(f"copy:2:1:{session_name}",)),
            req(1, 2, f"{session_name}", 0x00090000, in_extra=(0x00000000, raw), desc=(f"copy:1:1:{session_name}",)),
        )
    a = write(tmp_path, "a.txt", with_handle(0x8001, "fs:USER/file#0"))
    b = write(tmp_path, "b.txt", with_handle(0x4F2C, "fs:USER/file#0"))
    assert cmp.main([a, b, "--strict"]) == 0


def test_strict_compares_words_and_names_only_their_indices(tmp_path, capsys):
    a = trace(req(1, 1, "srv:", 0x00010000, params=(0x1234,)))
    b = trace(req(1, 1, "srv:", 0x00010000, params=(0x4321,)))
    assert run(tmp_path, a, b) == 0                       # same command, same result, same descriptors
    assert run(tmp_path, a, b, "--strict") == 1
    out = capsys.readouterr().out
    assert "in word 1" in out and "0x00001234" not in out and "0x00004321" not in out


def test_a_different_output_crc_does_not_match(tmp_path, capsys):
    cand = list(BASE)
    cand[3 + 3] = req(5, 2, "gsp::Gpu", 0x00040000, outdesc=("static:1:8:0xaabbccde",), t=40)
    assert run(tmp_path, BASE, cand) == 1
    out = capsys.readouterr().out
    assert "thread 00200000#0 differs at request 1" in out and "outdesc entry 0" in out
    assert "0xaabbccdd" in out and "0xaabbccde" in out    # CRCs are shown


def test_a_different_input_crc_size_or_index_does_not_match(tmp_path):
    for entry in ("static:1:16:0x11223345", "static:1:17:0x11223344", "static:2:16:0x11223344", "mapped-r:1:16:0x11223344"):
        cand = list(BASE)
        cand[3 + 1] = req(1, 2, "fs:USER", 0x00020000, desc=(entry,), t=20)
        assert run(tmp_path, BASE, cand) == 1, entry


def test_a_deferred_reply_after_later_requests_of_other_threads(tmp_path):
    # thread 1's second request is answered late: its line comes after thread 5's later requests
    deferred = trace(
        req(1, 1, "srv:", 0x00010000, params=(0x7,), t=10),
        req(5, 1, "gsp::Gpu", 0x00030000, t=30),
        req(5, 2, "gsp::Gpu", 0x00040000, outdesc=("static:1:8:0xaabbccdd",), t=40),
        req(1, 2, "fs:USER", 0x00020000, desc=("static:1:16:0x11223344",), t=900),
        req(1, 3, "fs:USER", 0x00050000, t=950),
    )
    assert cmp.main([write(tmp_path, "a.txt", BASE), write(tmp_path, "b.txt", deferred)]) == 0
    parsed = tracefmt.parse_ipc_lines(deferred)
    assert [c.seq for c in parsed.by_thread()["1"]] == [1, 2, 3]       # sent order, not written order


def test_the_requests_of_a_thread_are_compared_in_the_order_they_were_sent(tmp_path):
    shuffled = trace(BASE[3], BASE[4], BASE[5], BASE[6], BASE[7])
    # a thread whose lines are written out of order (a deferred reply) is sorted by seq
    swapped = trace(BASE[3], BASE[7], BASE[4], BASE[5], BASE[6])
    assert cmp.main([write(tmp_path, "a.txt", shuffled), write(tmp_path, "b.txt", swapped)]) == 0


def test_a_missing_request_is_reported_and_prefix_accepts_a_shorter_candidate(tmp_path, capsys):
    short = trace(*BASE[3:6])
    assert run(tmp_path, BASE, short) == 1
    assert "(no more requests)" in capsys.readouterr().out
    assert run(tmp_path, BASE, short, "--prefix") == 0
    assert run(tmp_path, short, BASE, "--prefix") == 1        # a longer candidate is not a prefix


def test_prefix_still_reports_a_difference_inside_the_common_part(tmp_path):
    cand = trace(BASE[3], req(1, 2, "fs:USER", 0x00020000, result=1, desc=("static:1:16:0x11223344",)))
    assert run(tmp_path, BASE, cand, "--prefix") == 1


def test_a_thread_missing_in_the_candidate(tmp_path):
    assert run(tmp_path, BASE, trace(BASE[3], BASE[4], BASE[7])) == 1
    assert run(tmp_path, BASE, trace(BASE[3], BASE[4], BASE[7]), "--prefix") == 0


def test_ignore_drops_commands_from_both_traces_with_two_files(tmp_path):
    # the candidate differs in two commands whose replies depend on time
    cand = list(BASE)
    cand[3 + 1] = req(1, 2, "fs:USER", 0x00020000, result=0x55, desc=("static:1:16:0x11223344",))
    cand[3 + 3] = req(5, 2, "gsp::Gpu", 0x00040000, outdesc=("static:1:8:0x99999999",))
    one = tmp_path / "one.txt"
    one.write_text("# time dependent commands\nfs:USER 0x00020000   # a tick count in the reply\n\n")
    two = tmp_path / "two.txt"
    two.write_text("gsp::Gpu 00040000  # a timestamp buffer\n")
    a, b = write(tmp_path, "a.txt", BASE), write(tmp_path, "b.txt", cand)
    assert cmp.main([a, b]) == 1
    assert cmp.main([a, b, "--ignore", str(one)]) == 1             # the second difference is still there
    assert cmp.main([a, b, "--ignore", str(two)]) == 1
    assert cmp.main([a, b, "--ignore", str(one), "--ignore", str(two)]) == 0
    # the commands are dropped from both traces, so a request only one of them has is dropped too
    assert cmp.main([a, write(tmp_path, "c.txt", trace(BASE[3], BASE[5], BASE[6], BASE[7])),
                     "--ignore", str(one), "--ignore", str(two)]) == 0


def test_ignore_matches_the_service_kind_of_a_handed_out_session(tmp_path):
    keep = req(1, 3, "srv:", 0x00010000)                          # something must be left to compare
    a = trace(req(1, 1, "fs:USER/file#0", 0x080200C2, result=0), req(1, 2, "fs:USER/file#1", 0x080200C2, result=0), keep)
    b = trace(req(1, 1, "fs:USER/file#0", 0x080200C2, result=9), req(1, 2, "fs:USER/file#1", 0x080200C2, result=9), keep)
    ignore = tmp_path / "i.txt"
    ignore.write_text("fs:USER/file 080200c2  # reads depend on the clock\n")
    assert run(tmp_path, a, b) == 1
    assert run(tmp_path, a, b, "--ignore", str(ignore)) == 0
    other = tmp_path / "o.txt"
    other.write_text("fs:USER 080200c2\n")                         # the service itself, not its files
    assert run(tmp_path, a, b, "--ignore", str(other)) == 1


def test_bad_ignore_file_and_missing_trace_exit_2(tmp_path, capsys):
    bad = tmp_path / "bad.txt"
    bad.write_text("fs:USER\n")
    a = write(tmp_path, "a.txt", BASE)
    assert cmp.main([a, a, "--ignore", str(bad)]) == 2
    assert cmp.main([a, str(tmp_path / "nothing.txt")]) == 2
    assert "error" in capsys.readouterr().err


def test_an_input_that_is_not_an_ipc_trace_is_refused(tmp_path, capsys):
    svc = ["# svctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off",
           "main 00000001 svc=0x32 in=00000000 out=00000000"]
    cases = (([], "no '# ipctrace' header"), ([""], "no '# ipctrace' header"),
             (svc, "not an IPC trace"), ([HEADER], "holds no request"), (trace(), "holds no request"))
    for bad, message in cases:                               # empty, SVC trace, header only, no request
        for ref, cand in ((bad, BASE), (BASE, bad), (bad, bad)):
            for extra in ((), ("--prefix",)):
                assert run(tmp_path, ref, cand, *extra) == 2
                err = capsys.readouterr()
                assert "all threads match" not in err.out
                assert "error:" in err.err and message in err.err


def test_the_header_is_checked_on_its_own_even_when_requests_follow(tmp_path, capsys):
    requests = BASE[3:]                                     # BASE's thread lines and request lines
    for header in ("# svctrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off",
                   "# ipctrace2 v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off",
                   "# IPCtrace v1 2026-10-16 game=0x0004000000000000 cpu=jit new3ds=off"):
        bad = [header, *BASE[1:]]
        assert run(tmp_path, bad, BASE) == 2, header
        assert "not an IPC trace" in capsys.readouterr().err
        assert run(tmp_path, BASE, bad, "--prefix") == 2
        capsys.readouterr()
    no_header = BASE[1:]
    assert run(tmp_path, BASE, no_header) == 2
    assert "no '# ipctrace' header" in capsys.readouterr().err
    assert requests                                          # the fixtures do have requests


def test_a_file_that_is_not_utf8_text_is_refused_not_a_traceback(tmp_path, capsys):
    good = write(tmp_path, "good.txt", BASE)
    binary = tmp_path / "bin.bin"
    binary.write_bytes(bytes(range(256)) * 4)
    utf16 = tmp_path / "utf16.txt"
    utf16.write_bytes(("\n".join(BASE) + "\n").encode("utf-16"))
    for bad in (binary, utf16):
        assert cmp.main([good, str(bad)]) == 2
        assert cmp.main([str(bad), good]) == 2
        assert "not UTF-8 text" in capsys.readouterr().err


def test_an_ignore_file_that_is_not_utf8_text_is_refused_not_a_traceback(tmp_path, capsys):
    a = write(tmp_path, "a.txt", BASE)
    for name, data in (("latin1.txt", "srv: 0x00010040  # caf\xe9\n".encode("latin-1")),
                       ("utf16.txt", "srv: 0x00010040\n".encode("utf-16")),
                       ("ff.txt", b"srv: 0x00010040  # \xff\xfe\n")):
        bad = tmp_path / name
        bad.write_bytes(data)
        assert cmp.main([a, a, "--ignore", str(bad)]) == 2, name
        assert "not UTF-8 text" in capsys.readouterr().err


def test_digits_that_are_not_plain_decimal_do_not_crash_the_parser(tmp_path, capsys):
    odd = [HEADER, thread_line(1, 0, 0x100000),
           "1 seq=\u00b2 session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0",         # superscript two
           "1 seq=" + "9" * 5000 + " session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0",
           "\u00b2 seq=1 session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0",     # odd thread token
           "1 seq=2 session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0 data=\u00b2:aa"]
    assert run(tmp_path, odd, odd) == 0
    assert "Traceback" not in capsys.readouterr().err
    assert run(tmp_path, odd[:2] + odd[2:4], BASE) == 2      # only malformed request lines: nothing to compare


def test_descriptors_with_impossible_numbers_do_not_crash_or_hang(tmp_path, capsys):
    base = "1 seq=1 session=srv: cmd=0x00010000 in=0x00010000 desc=- out=0x00010040,0x00000000 outdesc={} t=5"
    for outdesc in ("copy:-100:1:foo", "move:0:-1:foo", "copy:0:99999999999999999:foo", "copy:99999999:1:foo",
                    "static:-5:4:0x11223344"):
        line = [HEADER, thread_line(1, 0, 0x100000), base.format(outdesc)]
        assert run(tmp_path, line, line) in (0, 2), outdesc      # a result, never a traceback or a hang
        assert "Traceback" not in capsys.readouterr().err


def test_a_thread_token_of_thousands_of_digits_does_not_crash(tmp_path, capsys):
    odd = [HEADER, "9" * 5000 + " seq=1 session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0"]
    assert run(tmp_path, odd, odd) == 0                          # an opaque thread token, compared as one
    assert "Traceback" not in capsys.readouterr().err
    both = [HEADER, thread_line(1, 0, 0x100000),
            "9" * 5000 + " seq=1 session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0",
            "1 seq=1 session=srv: cmd=0x00010040 in=0x00010040 desc=- out=- outdesc=- t=0"]
    assert run(tmp_path, both, both) in (0, 1)
    assert "Traceback" not in capsys.readouterr().err


def test_traces_are_read_as_utf8_whatever_the_locale(tmp_path):
    import subprocess
    lines = [HEADER + "  # caf\u00e9", *BASE[1:]]
    a = write(tmp_path, "a.txt", lines)
    env = {"PATH": os.environ.get("PATH", ""), "LC_ALL": "C", "PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0"}
    r = subprocess.run([sys.executable, cmp.__file__, a, a], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "Traceback" not in r.stderr


def test_ignore_that_removes_every_request_is_refused(tmp_path, capsys):
    ignore = tmp_path / "ignore.txt"
    ignore.write_text("srv: 0x00010040\nfs:USER 0x00020000\nfs:USER 0x00050000\ngsp::Gpu 0x00030000\ngsp::Gpu 0x00040000\n")
    a = write(tmp_path, "a.txt", BASE)
    assert cmp.main([a, a, "--ignore", str(ignore)]) == 2
    err = capsys.readouterr()
    assert "--ignore removed every request" in err.err and "all threads match" not in err.out
    partial = tmp_path / "partial.txt"
    partial.write_text("fs:USER 0x00020000\n")
    assert cmp.main([a, a, "--ignore", str(partial)]) == 0


def test_prefix_notes_a_thread_the_candidate_never_used(tmp_path, capsys):
    assert run(tmp_path, BASE, trace(BASE[3], BASE[4], BASE[7]), "--prefix") == 0
    assert "no request on thread(s) 00200000#0" in capsys.readouterr().out
    assert run(tmp_path, BASE, BASE, "--prefix") == 0
    assert "note:" not in capsys.readouterr().out


def test_the_docstring_warns_that_strict_is_noisy_on_the_oracle():
    assert "stale bytes" in cmp.__doc__
    assert "will list" in cmp.__doc__


def test_context_shows_the_requests_before_the_difference(tmp_path, capsys):
    ref = trace(*[req(1, i, "srv:", 0x00010000 + i * 0x10000) for i in range(1, 8)])
    cand = trace(*([req(1, i, "srv:", 0x00010000 + i * 0x10000) for i in range(1, 7)] +
                   [req(1, 7, "srv:", 0x00010000 + 7 * 0x10000, result=1)]))
    assert run(tmp_path, ref, cand, "--context", "2") == 1
    out = capsys.readouterr().out
    assert "differs at request 6" in out
    assert "    4: seq=5" in out and "    5: seq=6" in out and "    3: seq=4" not in out


def test_an_unfinished_request_matches_another_unfinished_one_only(tmp_path):
    a = trace(req(1, 1, "srv:", 0x00010000, reply=False))
    assert run(tmp_path, a, a) == 0
    assert run(tmp_path, a, trace(req(1, 1, "srv:", 0x00010000))) == 1


def test_time_and_data_do_not_matter(tmp_path):
    a = trace(req(1, 1, "fs:USER", 0x00020000, desc=("static:1:2:0x11223344",), t=10, data=((1, "aabb"),)))
    b = trace(req(1, 1, "fs:USER", 0x00020000, desc=("static:1:2:0x11223344",), t=99999))
    assert run(tmp_path, a, b) == 0


def test_the_docstring_documents_the_format():
    for word in ("seq=", "session=", "outdesc=", "--strict", "--ignore", "--prefix", "--context", "data=",
                 "ipctrace"):
        assert word in cmp.__doc__
