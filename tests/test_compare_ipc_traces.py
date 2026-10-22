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
        t=0, reply=True, data=(), out_params=()):
    """One IPC trace line. `params`: the normal request parameters; `in_extra`/`out_extra`: the
    translate words (descriptors and what follows them); `desc`/`outdesc`: ready-made entries;
    `out_params`: normal reply parameters after the result (a Read's number of bytes read)."""
    n_in = len(in_extra)
    words_in = [cmd | (len(params) << 6) | n_in] + list(params) + list(in_extra)
    text = (f"{thread} seq={seq} session={session} cmd={w(words_in[0])} in={','.join(w(x) for x in words_in)} "
            f"desc={','.join(desc) or '-'} ")
    if reply:
        # reply header: the command id, the result and `out_params` as normal parameters, len(out_extra) translate words
        words_out = ([(cmd & 0xFFFF0000) | ((1 + len(out_params)) << 6) | len(out_extra), result]
                     + list(out_params) + list(out_extra))
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


# ── across timing models: the fs: Reads as multisets and the masked Writes (N0-32, D52) ──────────────

import zlib  # noqa: E402

import pytest  # noqa: E402

FILE = "fs:USER/file#1"
MULTISET = ("--fs-reads-multiset",)


def fs_read(thread, seq, offset, size, crc, session=FILE, result=0, reply=True, bytes_read=None):
    """A Read of a file session: offset (two words), size, the writable buffer; the reply says how many bytes it
    read (`bytes_read`, the size by default; none for a failed Read) and wrote data of CRC `crc`."""
    got = size if bytes_read is None else bytes_read
    return req(thread, seq, session, 0x08020000, result=result, params=(offset & 0xFFFFFFFF, offset >> 32, size),
               in_extra=((size << 4) | 0xC, 0x0FFFDAF8), out_extra=((size << 4) | 0xC, 0x0FFFDAF8),
               out_params=(got,) if result == 0 else (),
               desc=(f"mapped-w:4:{size}:-",), outdesc=(f"mapped-w:4:{size}:0x{crc:08x}",) if reply else (),
               reply=reply)


def fs_write(thread, seq, offset, data: bytes, session="fs:USER/file#2", with_data=True, crc=None, ptr=0x32EF8DF0):
    size = len(data)
    crc = zlib.crc32(data) if crc is None else crc
    return req(thread, seq, session, 0x08030000, params=(offset & 0xFFFFFFFF, offset >> 32, size, 0x00010001),
               in_extra=((size << 4) | 0xA, ptr), desc=(f"mapped-r:5:{size}:0x{crc:08x}",),
               data=((5, data.hex()),) if with_data else ())


def reads(thread, *items, first_seq=1):
    """Reads of (offset, size, crc) in the order given."""
    return [fs_read(thread, first_seq + i, *item) for i, item in enumerate(items)]


def mask_file(tmp_path, *lines, name="masks.txt"):
    p = tmp_path / name
    p.write_text("# a mask file\n" + "\n".join(lines) + "\n")
    return str(p)


STREAM = [(i * 2048, 2048, 0x1000 + i) for i in range(30)]


def test_a_reader_that_stops_five_reads_early_passes_the_multiset_rule_and_fails_the_plain_one(tmp_path):
    full, short = trace(*reads(5, *STREAM)), trace(*reads(5, *STREAM[:-5]))
    assert run(tmp_path, full, short) == 1                                        # the plain comparison: a missing request
    assert run(tmp_path, full, short, *MULTISET) == 0
    assert run(tmp_path, short, full, *MULTISET) == 0                              # either side may be the shorter one


def test_two_reads_in_another_order_pass_the_multiset_rule_and_fail_the_plain_one(tmp_path):
    a = trace(*reads(5, *STREAM[:6]))
    swapped = list(STREAM[:6])
    swapped[2], swapped[5] = swapped[5], swapped[2]
    b = trace(*reads(5, *swapped))
    assert run(tmp_path, a, b) == 1
    assert run(tmp_path, a, b, *MULTISET) == 0


def test_the_surplus_is_bounded_by_max_surplus(tmp_path, capsys):
    full = trace(*reads(5, *STREAM + [(99999 + i, 2048, 7000 + i) for i in range(3)]))
    base = trace(*reads(5, *STREAM))
    assert run(tmp_path, full[:-1], base, *MULTISET) == 0                          # 2 reads more
    sixteen = trace(*reads(5, *STREAM + [(70000 + i, 8, i) for i in range(16)]))
    assert run(tmp_path, sixteen, base, *MULTISET) == 0                            # exactly 16: allowed (the default)
    seventeen = trace(*reads(5, *STREAM + [(70000 + i, 8, i) for i in range(17)]))
    capsys.readouterr()
    assert run(tmp_path, seventeen, base, *MULTISET) == 1                          # 17: fails
    out = capsys.readouterr().out
    assert "surplus 17" in out and "at most 16" in out
    assert run(tmp_path, seventeen, base, *MULTISET, "--max-surplus", "17") == 0
    assert run(tmp_path, sixteen, base, *MULTISET, "--max-surplus", "15") == 1
    assert run(tmp_path, seventeen, base, *MULTISET, "--max-surplus", "0") == 1


@pytest.mark.parametrize("what,changed", [
    ("the CRC of the data", (0, 2048, 0xDEAD)), ("the offset", (4, 2048, 0x1000)), ("the size", (0, 1024, 0x1000)),
])
def test_a_read_that_differs_in_offset_size_or_data_fails(tmp_path, capsys, what, changed):
    a = trace(*reads(5, *STREAM[:4]))
    b = trace(*reads(5, changed, *STREAM[1:4]))
    capsys.readouterr()
    assert run(tmp_path, a, b, *MULTISET) == 1, what
    out = capsys.readouterr().out
    assert "differ as multisets" in out and "not on the other side" in out
    # the report names offsets, sizes and CRCs, never data
    assert "0x0000dead" not in out or what == "the CRC of the data"


def test_a_read_with_another_result_fails(tmp_path):
    a = trace(*reads(5, *STREAM[:3]))
    b = trace(fs_read(5, 1, *STREAM[0], result=0xC8804464), *reads(5, *STREAM[1:3], first_seq=2))
    assert run(tmp_path, a, b, *MULTISET) == 1


def test_a_smaller_side_must_be_contained_in_the_larger_even_inside_the_surplus(tmp_path):
    a = trace(*reads(5, *STREAM[:10]))
    b = trace(*reads(5, *STREAM[:8], (123456, 2048, 1), (123457, 2048, 2), (123458, 2048, 3)))      # 10 against 11
    assert run(tmp_path, a, b, *MULTISET) == 1


def test_equal_numbers_of_reads_must_be_equal_multisets(tmp_path):
    a = trace(*reads(5, *STREAM[:4]))
    b = trace(*reads(5, *STREAM[:3], (555, 2048, 5)))
    assert run(tmp_path, a, b, *MULTISET) == 1
    # a read repeated on one side and not on the other is not contained either
    c = trace(*reads(5, STREAM[0], STREAM[0], STREAM[1]))
    d = trace(*reads(5, STREAM[0], STREAM[1], STREAM[1]))
    assert run(tmp_path, c, d, *MULTISET) == 1


def test_the_reads_of_each_thread_are_a_multiset_of_their_own(tmp_path):
    a = trace(*reads(1, *STREAM[:3]), *reads(5, *STREAM[3:6], first_seq=1))
    b = trace(*reads(5, *STREAM[:3]), *reads(1, *STREAM[3:6], first_seq=1))        # the same reads on the other thread
    assert run(tmp_path, a, b, *MULTISET) == 1


def test_a_thread_that_reads_on_one_side_only_is_a_difference_beyond_the_surplus(tmp_path):
    a = trace(*reads(5, *STREAM))
    b = trace(req(5, 1, "srv:", 0x00010000))
    assert run(tmp_path, a, b, *MULTISET) == 1
    assert run(tmp_path, trace(*reads(5, *STREAM[:3])), b, *MULTISET) == 1         # 3 reads: inside the surplus, not contained


def test_reads_of_other_sessions_and_commands_are_compared_in_order(tmp_path):
    # a Read-looking request on a directory session, or a Read of a non-file service, is not a file Read
    a = trace(fs_read(5, 1, 0, 8, 1, session="fs:USER/dir#1"), fs_read(5, 2, 8, 8, 2, session="fs:USER/dir#1"))
    b = trace(fs_read(5, 1, 8, 8, 2, session="fs:USER/dir#1"), fs_read(5, 2, 0, 8, 1, session="fs:USER/dir#1"))
    assert run(tmp_path, a, b, *MULTISET) == 1
    c = trace(fs_read(5, 1, 0, 8, 1, session="xx:USER/file#1"), fs_read(5, 2, 8, 8, 2, session="xx:USER/file#1"))
    d = trace(fs_read(5, 1, 8, 8, 2, session="xx:USER/file#1"), fs_read(5, 2, 0, 8, 1, session="xx:USER/file#1"))
    assert run(tmp_path, c, d, *MULTISET) == 1


def test_the_file_session_number_does_not_matter_for_a_read(tmp_path):
    a = trace(fs_read(5, 1, 0, 8, 1, session="fs:USER/file#1"))
    b = trace(fs_read(5, 1, 0, 8, 1, session="fs:USER/file#9"))
    assert run(tmp_path, a, b, *MULTISET) == 0


def fs_request(thread, seq, session, cmd, params=(), **kw):
    return req(thread, seq, session, cmd, params=params, in_extra=(0x00000002, 0x12345678), **kw)


def test_the_other_fs_requests_are_compared_in_order_and_in_full(tmp_path, capsys):
    # an Open with its normal parameters (a path size here), then a Close
    a = trace(fs_request(5, 1, "fs:USER", 0x08010000, params=(0, 0x10, 3, 0x2A)),
              fs_request(5, 2, FILE, 0x08040000))
    assert run(tmp_path, a, a, *MULTISET) == 0
    b = trace(fs_request(5, 1, "fs:USER", 0x08010000, params=(0, 0x10, 3, 0x2B)), fs_request(5, 2, FILE, 0x08040000))
    assert run(tmp_path, a, b) == 0                                            # the plain comparison reads no words
    capsys.readouterr()
    assert run(tmp_path, a, b, *MULTISET) == 1                                 # in full: a normal parameter differs
    assert "in parameter word 4" in capsys.readouterr().out
    # a different order
    c = trace(fs_request(5, 1, FILE, 0x08040000), fs_request(5, 2, "fs:USER", 0x08010000, params=(0, 0x10, 3, 0x2A)))
    assert run(tmp_path, a, c, *MULTISET) == 1
    # a buffer's address (a translate word) is not a parameter of the command
    d = trace(fs_request(5, 1, "fs:USER", 0x08010000, params=(0, 0x10, 3, 0x2A)).replace("0x12345678", "0x87654321"),
              fs_request(5, 2, FILE, 0x08040000))
    assert run(tmp_path, a, d, *MULTISET) == 0


def test_requests_of_other_services_are_not_compared_word_by_word_with_the_multiset_option(tmp_path):
    a = trace(req(1, 1, "gsp::Gpu", 0x00030000, params=(1,)), req(5, 1, "gsp::Gpu", 0x00040000, params=(2,)))
    b = trace(req(1, 1, "gsp::Gpu", 0x00030000, params=(9,)), req(5, 1, "gsp::Gpu", 0x00040000, params=(2,)))
    assert run(tmp_path, a, b, *MULTISET) == 0


def test_a_write_that_differs_fails_without_a_mask(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0, bytes(range(16))))
    b = trace(fs_write(5, 1, 0, bytes(range(1, 17))))
    capsys.readouterr()
    assert run(tmp_path, a, b, *MULTISET) == 1
    out = capsys.readouterr().out
    assert "differs in: desc entry 0" in out and "000102030405" not in out        # CRCs, never the bytes
    assert run(tmp_path, a, a, *MULTISET) == 0


def test_a_write_that_differs_only_in_a_masked_range_passes(tmp_path):
    data = bytearray(range(32))
    other = bytearray(data)
    other[4:8] = b"\xff\xff\xff\xff"
    other[20] ^= 0x55
    a = trace(fs_write(5, 1, 0x40, bytes(data)))
    b = trace(fs_write(5, 1, 0x40, bytes(other)))
    masks = mask_file(tmp_path, "S1 32 64 4 4   the first range: derived from the clock",
                      "S1 32 64 20 1   the second range: also from the clock")
    assert run(tmp_path, a, b, *MULTISET) == 1
    assert run(tmp_path, a, b, *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 0
    # the plain comparison does not read the options
    assert run(tmp_path, a, b, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 0


def test_a_write_that_differs_outside_the_masked_ranges_fails(tmp_path, capsys):
    data = bytearray(range(32))
    for where in (0, 3, 8, 19, 21, 31):                    # just outside the two ranges above, and the ends
        other = bytearray(data)
        other[4:8] = b"\xff\xff\xff\xff"                   # the masked bytes differ as well
        other[20] ^= 0x55
        other[where] ^= 0x01
        masks = mask_file(tmp_path, "S1 32 64 4 4   reason", "S1 32 64 20 1   reason")
        capsys.readouterr()
        code = run(tmp_path, trace(fs_write(5, 1, 0x40, bytes(data))), trace(fs_write(5, 1, 0x40, bytes(other))),
                   *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "S1")
        assert code == 1, where
        assert "desc entry 0" in capsys.readouterr().out
    # a mask one byte short does not cover the range that differs
    other = bytearray(data)
    other[4:8] = b"\xff\xff\xff\xff"
    short = mask_file(tmp_path, "S1 32 64 4 3   reason")
    assert run(tmp_path, trace(fs_write(5, 1, 0x40, bytes(data))), trace(fs_write(5, 1, 0x40, bytes(other))),
               *MULTISET, "--fs-write-mask", short, "--fs-write-mask-session", "S1") == 1


def test_a_mask_applies_to_the_write_of_its_size_and_offset_only(tmp_path):
    base = bytes(range(32))
    changed = bytearray(base)
    changed[4] = 0xEE
    masks = mask_file(tmp_path, "S1 32 64 4 4   reason")
    options = (*MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "S1")
    anchor = fs_write(1, 1, 64, base, session="fs:USER/file#1")     # a Write the line applies to, equal on both sides
    # the same bytes differ in a Write of another offset, or of another size: the line does not hide them
    for size, offset in ((32, 128), (40, 64)):
        pad = bytes(size - 32)
        a = trace(anchor, fs_write(5, 1, offset, base + pad))
        b = trace(anchor, fs_write(5, 1, offset, bytes(changed) + pad))
        assert run(tmp_path, a, b, *options) == 1, (size, offset)
    # at its own size and offset it does
    a = trace(anchor, fs_write(5, 1, 64, base))
    b = trace(anchor, fs_write(5, 1, 64, bytes(changed)))
    assert run(tmp_path, a, b, *options) == 0

def test_the_masks_of_other_sessions_are_not_looked_at(tmp_path):
    data, other = bytearray(range(32)), bytearray(range(32))
    other[4] = 0xEE
    a, b = trace(fs_write(5, 1, 0x40, bytes(data))), trace(fs_write(5, 1, 0x40, bytes(other)))
    masks = mask_file(tmp_path, "S2 32 64 4 4   the same range, for another session", "S1 99 0 0 1   a Write of S1")
    # session S2's mask would hide the difference; asked for S1 it is not used (and S1's line matches no Write)
    assert run(tmp_path, a, b, *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "S2") == 0
    assert run(tmp_path, a, b, *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 2


def test_a_session_with_no_line_in_the_mask_file_is_compared_in_full(tmp_path):
    data, other = bytearray(range(32)), bytearray(range(32))
    other[4] = 0xEE
    a, b = trace(fs_write(5, 1, 0x40, bytes(data))), trace(fs_write(5, 1, 0x40, bytes(other)))
    masks = mask_file(tmp_path, "S2 32 64 4 4   for another session")
    assert run(tmp_path, a, b, *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "G9") == 1
    assert run(tmp_path, a, a, *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "G9") == 0


def test_a_mask_that_matches_no_write_is_an_error(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32))))
    masks = mask_file(tmp_path, "S1 32 64 4 4   applies", "S1 32 0 4 4   a Write that is not in the traces")
    capsys.readouterr()
    assert run(tmp_path, a, a, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 2
    err = capsys.readouterr().err
    assert "masks.txt:3" in err and "matches no Write" in err


def test_a_mask_on_a_trace_without_write_data_is_an_error(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32)), with_data=False))
    masks = mask_file(tmp_path, "S1 32 64 4 4   reason")
    capsys.readouterr()
    assert run(tmp_path, a, a, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 2
    assert "--trace-ipc-full" in capsys.readouterr().err


def test_write_data_that_does_not_match_its_crc_is_an_error(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32)), crc=0x12345678))
    masks = mask_file(tmp_path, "S1 32 64 4 4   reason")
    capsys.readouterr()
    assert run(tmp_path, a, a, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 2
    assert "does not match its CRC-32" in capsys.readouterr().err


@pytest.mark.parametrize("line,message", [
    ("S1 32 64 4 4", "a reason is required"),
    ("S1 32 64 4", "expected"),
    ("S1 32 64 x 4   reason", "not a number"),
    ("S1 32 64 -4 4   reason", "negative"),
    ("S1 32 64 4 0   reason", "empty or outside"),
    ("S1 32 64 30 4   reason", "empty or outside"),
])
def test_a_malformed_mask_file_is_an_error(tmp_path, capsys, line, message):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32))))
    capsys.readouterr()
    assert run(tmp_path, a, a, "--fs-write-mask", mask_file(tmp_path, line), "--fs-write-mask-session", "S1") == 2
    assert message in capsys.readouterr().err


def test_a_missing_or_empty_mask_file_is_an_error(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32))))
    assert run(tmp_path, a, a, "--fs-write-mask", str(tmp_path / "nothing.txt"), "--fs-write-mask-session", "S1") == 2
    assert "cannot read the write-mask file" in capsys.readouterr().err
    assert run(tmp_path, a, a, "--fs-write-mask", mask_file(tmp_path), "--fs-write-mask-session", "S1") == 2
    assert "holds no line" in capsys.readouterr().err


def test_a_mask_file_that_is_not_text_is_an_error(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32))))
    bad = tmp_path / "bad.txt"
    bad.write_bytes(b"\xff\xfe\x00G2 1 0 0 1 x\n")
    assert run(tmp_path, a, a, "--fs-write-mask", str(bad), "--fs-write-mask-session", "S1") == 2
    assert "not UTF-8" in capsys.readouterr().err


def test_the_new_options_have_their_usage_errors(tmp_path, capsys):
    a = trace(fs_write(5, 1, 0x40, bytes(range(32))))
    masks = mask_file(tmp_path, "S1 32 64 4 4   reason")
    for extra in (["--max-surplus", "3"], ["--fs-reads-multiset", "--max-surplus", "-1"],
                  ["--fs-write-mask", masks], ["--fs-write-mask-session", "S1"]):
        with pytest.raises(SystemExit) as stop:
            run(tmp_path, a, a, *extra)
        assert stop.value.code == 2, extra
    capsys.readouterr()


def test_the_report_of_the_multiset_rule_says_how_many_reads_and_masks(tmp_path, capsys):
    a = trace(*reads(5, *STREAM), fs_write(1, 1, 0x40, bytes(range(32))))
    b = trace(*reads(5, *STREAM[:-5]), fs_write(1, 1, 0x40, bytes(range(32))))
    masks = mask_file(tmp_path, "S1 32 64 4 4   reason")
    capsys.readouterr()
    assert run(tmp_path, a, b, *MULTISET, "--fs-write-mask", masks, "--fs-write-mask-session", "S1") == 0
    out = capsys.readouterr().out
    assert "fs reads: 1 thread(s) read; 30 on the reference and 25 on the candidate; the largest difference is 5" in out
    assert "fs write masks: 1 line(s) of session S1, applied to 1 Write(s) of the reference and 1 of the candidate" in out


def test_the_docstring_documents_the_cross_model_options():
    for word in ("--fs-reads-multiset", "--max-surplus", "--fs-write-mask", "--fs-write-mask-session", "D52",
                 "multiset"):
        assert word in cmp.__doc__

# ── touching Reads merged before the multiset is built (N0-33, D76) ───────────────────────────────

COALESCE = (*MULTISET, "--fs-reads-coalesce")
FILE_DATA = bytes((i * 13 + i // 97) & 0xFF for i in range(70000))


def spans(thread, first_seq, items, session=FILE):
    """Reads of (offset, size) of the file bytes FILE_DATA, with the CRC-32 of the bytes read."""
    return [fs_read(thread, first_seq + i, off, size, zlib.crc32(FILE_DATA[off:off + size]), session=session)
            for i, (off, size) in enumerate(items)]


def split(offset, pieces):
    """Consecutive (offset, size) spans of the given sizes from `offset`."""
    out = []
    for n in pieces:
        out.append((offset, n))
        offset += n
    return out


# 28672 bytes as the reference's reader reads them (4096- and 2048-byte pieces)
PIECES = split(0, [4096, 2048, 4096, 4096, 2048, 2048, 4096, 2048, 2048, 2048])
WHOLE = [(0, 28672)]


def test_one_28672_byte_read_equals_the_pieces_that_cover_the_same_range(tmp_path):
    big, small = trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, PIECES))
    assert sum(n for _, n in PIECES) == 28672
    assert run(tmp_path, big, small, *MULTISET) == 1                        # as multisets alone they differ
    assert run(tmp_path, big, small, *COALESCE) == 0
    assert run(tmp_path, small, big, *COALESCE) == 0                        # either side may be the one that splits
    # the pieces come in another order: still the same range (the order of the Reads is not compared, D52)
    shuffled = list(PIECES)
    shuffled[1], shuffled[2] = shuffled[2], shuffled[1]
    shuffled.reverse()
    assert run(tmp_path, big, trace(*spans(5, 1, shuffled)), *COALESCE) == 0
    assert run(tmp_path, small, trace(*spans(5, 1, shuffled)), *COALESCE) == 0
    assert run(tmp_path, small, trace(*spans(5, 1, shuffled)), *MULTISET) == 0


def test_two_ways_of_splitting_the_same_range_match(tmp_path):
    a = trace(*spans(5, 1, split(4096, [4096, 2048, 8192])))
    b = trace(*spans(5, 1, split(4096, [2048, 4096, 2048, 2048, 4096])))
    assert 4096 + 2048 + 8192 == 2048 + 4096 + 2048 + 2048 + 4096
    assert run(tmp_path, a, b, *COALESCE) == 0


def test_a_gap_in_the_pieces_fails(tmp_path, capsys):
    gap = list(PIECES)
    del gap[4]                                                              # 2048 bytes are never read
    capsys.readouterr()
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, gap)), *COALESCE) == 1
    out = capsys.readouterr().out
    assert "differ as multisets" in out and "not on the other side" in out
    assert run(tmp_path, trace(*spans(5, 1, gap)), trace(*spans(5, 1, WHOLE)), *COALESCE) == 1
    # a read short at the end: the pieces cover less than the whole
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, PIECES[:-1])), *COALESCE) == 1
    # a piece read twice is one read more than the range needs: the whole range is there, and the repeated piece
    # is a surplus run of its own (inside the allowance, never a way to hide a missing piece)
    twice = PIECES[:3] + PIECES[2:3] + PIECES[3:]
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, twice)), *COALESCE) == 0
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, twice)), *COALESCE, "--max-surplus", "0") == 1
    twice_gap = [p for i, p in enumerate(twice) if i != 7]                 # and a piece missing as well
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, twice_gap)), *COALESCE) == 1


def test_pieces_with_another_crc_fail(tmp_path):
    good = spans(5, 1, PIECES)
    bad = list(good)
    bad[3] = fs_read(5, 4, PIECES[3][0], PIECES[3][1], zlib.crc32(FILE_DATA[PIECES[3][0]:PIECES[3][0] + PIECES[3][1]]) ^ 1)
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), trace(*bad), *COALESCE) == 1
    # and a whole read whose CRC is not that of the bytes the pieces read
    other = [fs_read(5, 1, 0, 28672, zlib.crc32(FILE_DATA[1:28673]))]
    assert run(tmp_path, trace(*other), trace(*good), *COALESCE) == 1


def test_a_run_longer_on_one_side_is_a_range_the_other_never_read(tmp_path):
    # the reader on one side went on for 2048 more bytes: its run is a different range
    longer = trace(*spans(5, 1, PIECES + [(28672, 2048)]))
    assert run(tmp_path, trace(*spans(5, 1, WHOLE)), longer, *COALESCE) == 1
    assert run(tmp_path, longer, longer, *COALESCE) == 0


def test_the_surplus_counts_runs_after_the_merge(tmp_path, capsys):
    base = spans(5, 1, [(0, 100), (1000, 100), (2000, 100)])
    def isolated(n):
        return spans(5, 10, [(10000 + 10 * i, 5) for i in range(n)])        # n reads that touch nothing
    ref = trace(*base)
    assert run(tmp_path, trace(*base, *isolated(16)), ref, *COALESCE) == 0
    capsys.readouterr()
    assert run(tmp_path, trace(*base, *isolated(17)), ref, *COALESCE) == 1
    out = capsys.readouterr().out
    assert "surplus 17" in out and "at most 16" in out
    assert run(tmp_path, trace(*base, *isolated(17)), ref, *COALESCE, "--max-surplus", "17") == 0
    # 40 extra reads that run on from the last one are one run: the surplus is 0 runs
    on = spans(5, 10, split(2100, [50] * 40))
    assert run(tmp_path, trace(*base[:2], *spans(5, 3, [(2000, 100)]), *on),
               trace(*base[:2], *spans(5, 3, [(2000, 2100)])), *COALESCE) == 0


def test_runs_are_per_thread_and_per_file_session(tmp_path):
    a = trace(*spans(1, 1, PIECES[:2], session="fs:USER/file#1"), *spans(5, 1, PIECES[2:4], session="fs:USER/file#1"))
    # the first two pieces on thread 1, the next two on thread 5: no thread reads a touching range alone
    b = trace(*spans(1, 1, [(0, 6144)], session="fs:USER/file#1"), *spans(5, 1, [(6144, 8192)], session="fs:USER/file#1"))
    assert run(tmp_path, a, b, *COALESCE) == 0
    c = trace(*spans(1, 1, [(0, 14336)], session="fs:USER/file#1"))
    assert run(tmp_path, a, c, *COALESCE) == 1                                # the threads' reads are not pooled
    # two files read in turn: each file's pieces merge, whatever is read between them
    mixed = [(0, 4096, "fs:USER/file#1"), (9000, 100, "fs:USER/file#2"), (4096, 2048, "fs:USER/file#1"),
             (9100, 100, "fs:USER/file#2")]
    lines = [spans(5, 1 + i, [(off, n)], session=s)[0] for i, (off, n, s) in enumerate(mixed)]
    merged = [spans(5, 1, [(0, 6144)], session="fs:USER/file#7")[0],
              spans(5, 2, [(9000, 200)], session="fs:USER/file#8")[0]]
    assert run(tmp_path, trace(*lines), trace(*merged), *COALESCE) == 0


def test_reads_that_cannot_be_merged_are_compared_on_their_own(tmp_path):
    short = fs_read(5, 2, 100, 100, zlib.crc32(FILE_DATA[100:200]), bytes_read=60)       # read 60 of 100 bytes
    a = trace(*spans(5, 1, [(0, 100)]), short, *spans(5, 3, [(200, 100)]))
    assert run(tmp_path, a, a, *COALESCE) == 0
    # the same range in one piece is not the same thing as a short read between two reads
    b = trace(*spans(5, 1, [(0, 300)]))
    assert run(tmp_path, a, b, *COALESCE) == 1
    failed = fs_read(5, 2, 100, 100, 0, result=0xC8804464)
    c = trace(*spans(5, 1, [(0, 100)]), failed, *spans(5, 3, [(200, 100)]))
    assert run(tmp_path, c, b, *COALESCE) == 1
    assert run(tmp_path, c, c, *COALESCE) == 0


def test_without_the_coalesce_option_nothing_is_merged_and_it_needs_the_multiset_rule(tmp_path, capsys):
    big, small = trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, PIECES))
    assert run(tmp_path, big, small, *MULTISET) == 1
    capsys.readouterr()
    with pytest.raises(SystemExit) as stop:
        run(tmp_path, big, small, "--fs-reads-coalesce")
    assert stop.value.code == 2
    assert "--fs-reads-coalesce is a rule of --fs-reads-multiset" in capsys.readouterr().err


def test_the_report_says_the_reads_were_merged(tmp_path, capsys):
    big, small = trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, PIECES))
    capsys.readouterr()
    assert run(tmp_path, big, small, *COALESCE) == 0
    out = capsys.readouterr().out
    assert "fs reads: 1 thread(s) read; 1 on the reference and 10 on the candidate" in out
    assert "after merging touching reads: 1 and 1" in out
    # a failure names how many runs each side has, and the reads they were merged from
    capsys.readouterr()
    gap = list(PIECES)
    del gap[4]
    assert run(tmp_path, big, trace(*spans(5, 1, gap)), *COALESCE) == 1
    out = capsys.readouterr().out
    assert "1 on the reference, 2 on the candidate" in out
    assert "reads merged into runs: from 1 and 9 reads" in out


def test_the_docstring_documents_the_coalesce_option():
    for word in ("--fs-reads-coalesce", "D76", "crc32_combine", "touch"):
        assert word in cmp.__doc__


# ── a stream that ends further on one side: the shorter run is a verified prefix of the longer (N0-33) ───────────

PREFIX = (*COALESCE, "--fs-reads-prefix")
TWO_K = split(0, [2048] * 20)                                  # 40960 bytes in 2048-byte pieces
BIG_THEN_TWO_K = split(0, [28672] + [2048] * 10)               # 49152 bytes: a boundary at 40960, 8192 bytes further on


def test_a_run_that_is_a_prefix_of_the_other_sides_run_passes_with_the_prefix_rule_only(tmp_path, capsys):
    short, long_ = trace(*spans(5, 1, TWO_K)), trace(*spans(5, 1, BIG_THEN_TWO_K))
    assert sum(n for _, n in BIG_THEN_TWO_K[:7]) == 40960
    assert run(tmp_path, short, long_, *COALESCE) == 1                      # two runs of other lengths
    capsys.readouterr()
    assert run(tmp_path, short, long_, *PREFIX) == 0
    out = capsys.readouterr().out
    assert "prefix of a longer run: 1 run(s), 4 read(s) beyond it" in out
    assert run(tmp_path, long_, short, *PREFIX) == 0                        # either side may run further


def test_the_prefix_must_end_at_a_part_boundary_of_the_longer_run_and_have_its_crc(tmp_path, capsys):
    short = trace(*spans(5, 1, TWO_K))
    # no part of the longer run ends at 40960: its data there is not known
    no_boundary = split(0, [28672, 8192, 8192, 4096])
    capsys.readouterr()
    assert run(tmp_path, short, trace(*spans(5, 1, no_boundary)), *PREFIX) == 1
    assert "not on the other side" in capsys.readouterr().out
    # a boundary, but another CRC-32 on one of the shorter run's pieces
    wrong = list(spans(5, 1, TWO_K))
    wrong[7] = fs_read(5, 8, 7 * 2048, 2048, zlib.crc32(FILE_DATA[7 * 2048:8 * 2048]) ^ 1)
    assert run(tmp_path, trace(*wrong), trace(*spans(5, 1, BIG_THEN_TWO_K)), *PREFIX) == 1
    # a longer run with another start is not a run that goes further
    shifted = split(2048, [28672] + [2048] * 10)
    assert run(tmp_path, short, trace(*spans(5, 1, shifted)), *PREFIX) == 1
    # nor is a different result
    failed = trace(*spans(5, 1, TWO_K[:5]), fs_read(5, 6, 5 * 2048, 2048, 0, result=0xC8804464))
    assert run(tmp_path, failed, trace(*spans(5, 1, BIG_THEN_TWO_K)), *PREFIX) == 1


def test_the_reads_beyond_the_prefix_count_toward_the_surplus(tmp_path, capsys):
    ref = trace(*spans(5, 1, TWO_K))
    for tail, code in ((16, 0), (17, 1)):
        cand = trace(*spans(5, 1, split(0, [40960] + [2048] * tail)))
        capsys.readouterr()
        assert run(tmp_path, ref, cand, *PREFIX) == code, tail
    out = capsys.readouterr().out
    assert "surplus 17" in out and "at most 16" in out
    cand = trace(*spans(5, 1, split(0, [40960] + [2048] * 17)))
    assert run(tmp_path, ref, cand, *PREFIX, "--max-surplus", "17") == 0


def test_the_prefix_rule_needs_the_coalesce_rule_and_leaves_exact_matches_alone(tmp_path, capsys):
    a = trace(*spans(5, 1, TWO_K))
    capsys.readouterr()
    with pytest.raises(SystemExit) as stop:
        run(tmp_path, a, a, *MULTISET, "--fs-reads-prefix")
    assert stop.value.code == 2
    assert "--fs-reads-prefix is a rule of --fs-reads-coalesce" in capsys.readouterr().err
    assert run(tmp_path, a, a, *PREFIX) == 0
    assert "prefix of a longer run" not in capsys.readouterr().out         # nothing was a prefix


def test_a_single_read_can_be_the_prefix_of_a_run(tmp_path):
    ref = trace(*spans(5, 1, [(0, 4096)]))
    cand = trace(*spans(5, 1, [(0, 4096), (4096, 4096), (8192, 2048)]))
    assert run(tmp_path, ref, cand, *COALESCE) == 1
    assert run(tmp_path, ref, cand, *PREFIX) == 0


def test_each_run_serves_one_prefix_only(tmp_path):
    # three runs of the same 4096 bytes on one side, one run that goes on from there and a run of something else
    # on the other: the longer run takes one of the three, the other two are not contained anywhere
    ref = trace(*spans(5, 1, [(0, 4096), (0, 4096), (0, 4096)]))
    cand = trace(*spans(5, 1, [(0, 4096), (4096, 4096), (50000, 100)]))
    assert run(tmp_path, ref, cand, *PREFIX) == 1                           # (50000, 100) is on the reference nowhere
    # the same, with the other thing absent: the two remaining runs of the reference are its surplus
    assert run(tmp_path, trace(*spans(5, 1, [(0, 4096), (0, 4096)])), trace(*spans(5, 1, [(0, 4096), (4096, 4096)])),
               *PREFIX) == 0
    # the same bytes under a run of another start are no prefix
    assert run(tmp_path, trace(*spans(5, 1, [(2048, 4096)])), trace(*spans(5, 1, [(0, 8192)])), *PREFIX) == 1


def test_the_docstring_documents_the_prefix_option():
    for word in ("--fs-reads-prefix", "part boundary", "prefix"):
        assert word in cmp.__doc__

# ── the bytes only one side covers: information, never a rule (N0-36) ─────────────────────────────


def coverage(ref_lines, cand_lines, tmp_path):
    """read_coverage on two synthetic traces, by (thread, session)."""
    ref = cmp.load(write(tmp_path, "cref.txt", ref_lines), set())
    cand = cmp.load(write(tmp_path, "ccand.txt", cand_lines), set())
    return {(r.thread, r.session): r for r in cmp.read_coverage(ref, cand)}


def test_the_coverage_is_the_union_of_the_ranges_whatever_the_pieces(tmp_path):
    rows = coverage(trace(*spans(5, 1, WHOLE)), trace(*spans(5, 1, PIECES)), tmp_path)
    (row,) = rows.values()
    assert (row.ref_bytes, row.cand_bytes, row.ref_only, row.cand_only) == (28672, 28672, 0, 0)
    # the same range read twice covers it once
    twice = coverage(trace(*spans(5, 1, [(0, 4096), (0, 4096), (4096, 4096)])), trace(*spans(5, 1, [(0, 8192)])),
                     tmp_path)
    assert [(r.ref_bytes, r.cand_bytes, r.ref_only, r.cand_only) for r in twice.values()] == [(8192, 8192, 0, 0)]


def test_the_bytes_only_one_side_covers_are_counted_in_bytes_and_ranges(tmp_path):
    ref = trace(*spans(5, 1, [(0, 8192), (8192, 8192), (40000, 4096), (50000, 100)]))
    cand = trace(*spans(5, 1, [(0, 8192), (60000, 1000)]))
    (row,) = coverage(ref, cand, tmp_path).values()
    assert row.ref_bytes == 8192 + 8192 + 4096 + 100 and row.cand_bytes == 8192 + 1000
    assert (row.ref_only, row.ref_only_ranges) == (8192 + 4096 + 100, 3)
    assert (row.cand_only, row.cand_only_ranges) == (1000, 1)
    # a range that overlaps the other side's only partly counts for the part outside it
    (part,) = coverage(trace(*spans(5, 1, [(0, 10000)])), trace(*spans(5, 1, [(4000, 10000)])), tmp_path).values()
    assert (part.ref_only, part.cand_only) == (4000, 4000)


def test_a_failed_or_short_read_covers_only_the_bytes_it_returned(tmp_path):
    ref = trace(fs_read(5, 1, 0, 4096, 0x11), fs_read(5, 2, 4096, 4096, 0x22, result=0xC8804464, reply=True),
                fs_read(5, 3, 8192, 4096, 0x33, bytes_read=100), fs_read(5, 4, 20000, 4096, 0x44, reply=False))
    (row,) = coverage(ref, trace(fs_read(5, 1, 0, 4096, 0x11)), tmp_path).values()
    assert (row.ref_bytes, row.ref_only, row.cand_only) == (4096 + 100, 100, 0)


def test_a_thread_or_a_file_session_that_one_side_never_reads_is_all_one_sided(tmp_path):
    ref = trace(*spans(5, 1, [(0, 4096), (4096, 4096)]), *spans(1, 1, [(0, 2048)], session="fs:USER/file#3"))
    cand = trace(*spans(5, 1, [(0, 4096), (4096, 4096)]))
    rows = coverage(ref, cand, tmp_path)
    assert len(rows) == 2
    other = [r for r in rows.values() if r.session == "fs:USER/file#3"][0]
    assert (other.ref_bytes, other.cand_bytes, other.ref_only, other.cand_only) == (2048, 0, 2048, 0)


def test_the_report_has_a_coverage_line_with_the_multiset_option_only(tmp_path, capsys):
    a, b = trace(*reads(5, *STREAM)), trace(*reads(5, *STREAM[:-5]))
    capsys.readouterr()
    assert run(tmp_path, a, b, *MULTISET) == 0
    out = capsys.readouterr().out
    line = [ln for ln in out.splitlines() if ln.startswith("fs coverage:")]
    assert len(line) == 1 and out.splitlines().index(line[0]) > out.splitlines().index(
        next(ln for ln in out.splitlines() if ln.startswith("fs reads:")))
    assert ("1 thread/file session(s) read; bytes only the reference covers: 10240 (in 1, 1 range(s)), only the "
            "candidate covers: 0 (in 0, 0 range(s)); the most per thread and file session: reference 10240 on thread"
            ) in line[0]
    assert "(information, not a rule)" in line[0]
    assert run(tmp_path, a, a, *MULTISET) == 0
    assert ("only the reference covers: 0 (in 0, 0 range(s)), only the candidate covers: 0 (in 0, 0 range(s)) "
            "(information") in capsys.readouterr().out
    assert run(tmp_path, a, b, "--prefix") in (0, 1)
    assert "fs coverage:" not in capsys.readouterr().out


def test_the_coverage_is_information_it_changes_no_result(tmp_path, capsys):
    base = trace(*reads(5, *STREAM))
    # 4 MiB that only the candidate reads, in one read: the multiset rule accepts one read more, as it did before
    big = trace(*reads(5, *STREAM), fs_read(5, 99, 10_000_000, 4 << 20, 0xABCD))
    capsys.readouterr()
    assert run(tmp_path, base, big, *MULTISET) == 0
    assert "only the candidate covers: 4194304 (in 1, 1 range(s))" in capsys.readouterr().out
    # a CRC that differs on an identical (offset, size) fails, with or without a coverage difference
    changed = list(STREAM)
    changed[3] = (changed[3][0], changed[3][1], 0xDEAD)
    assert run(tmp_path, base, trace(*reads(5, *changed)), *MULTISET) == 1
    out = capsys.readouterr().out
    assert "only the reference covers: 0 (in 0, 0 range(s)), only the candidate covers: 0 (in 0, 0 range(s))" in out
    # (same ranges, other data)


def test_merge_ranges_is_the_union_of_unsorted_contained_and_touching_ranges():
    assert cmp.merge_ranges([]) == []
    assert cmp.merge_ranges([(100, 200), (0, 50)]) == [(0, 50), (100, 200)]                  # any order
    assert cmp.merge_ranges([(0, 8192), (100, 200)]) == [(0, 8192)]                           # one inside an earlier, longer one
    assert cmp.merge_ranges([(0, 8192), (100, 200), (8000, 9000)]) == [(0, 9000)]
    assert cmp.merge_ranges([(0, 10), (10, 20)]) == [(0, 20)]                                 # touching ranges are one
    assert cmp.merge_ranges([(0, 10), (11, 20)]) == [(0, 10), (11, 20)]                       # one byte apart are two
    assert cmp.merge_ranges([(5, 15), (0, 10)]) == [(0, 15)]


def test_subtract_ranges_keeps_what_the_other_side_does_not_cover():
    sub = cmp.subtract_ranges
    assert sub([], [(0, 10)]) == [] and sub([(0, 10)], []) == [(0, 10)]
    assert sub([(0, 100)], [(0, 100)]) == []
    assert sub([(0, 100)], [(0, 50)]) == [(50, 100)] and sub([(0, 100)], [(50, 100)]) == [(0, 50)]
    assert sub([(0, 100)], [(40, 60)]) == [(0, 40), (60, 100)]
    assert sub([(0, 100)], [(0, 1)]) == [(1, 100)] and sub([(0, 100)], [(99, 100)]) == [(0, 99)]
    assert sub([(0, 50)], [(10, 20), (30, 40)]) == [(0, 10), (20, 30), (40, 50)]
    # ranges that only touch cover nothing of the other
    assert sub([(100, 200)], [(0, 100)]) == [(100, 200)] and sub([(100, 200)], [(200, 300)]) == [(100, 200)]
    assert sub([(0, 10), (20, 30)], [(5, 25)]) == [(0, 5), (25, 30)]
    assert sub([(0, 100)], [(-50, 200)]) == []


def read_extent(tmp_path, line):
    (calls,) = cmp.load(write(tmp_path, "extent.txt", trace(line)), set()).values()
    return cmp.fs_read_extent(calls[0])


def test_the_extent_of_a_read_is_what_its_reply_says_it_read(tmp_path):
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 4096, 0x11)) == (1000, 5096)
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 4096, 0x11, bytes_read=100)) == (1000, 1100)
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 4096, 0x11, bytes_read=9999)) == (1000, 5096)      # at most the size asked
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 4096, 0x11, bytes_read=0)) is None                # read nothing
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 4096, 0x11, reply=False)) is None                 # no reply
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 4096, 0x11, result=0xC8804464)) is None            # failed
    assert read_extent(tmp_path, fs_read(5, 1, 1000, 0, 0x11)) is None                                  # asked for nothing
    high = (1 << 32) + 100
    assert read_extent(tmp_path, fs_read(5, 1, high, 4096, 0x11)) == (high, high + 4096)               # the high word counts
    # a successful Read whose reply has no byte count covers the whole size asked for
    no_count = req(5, 1, FILE, 0x08020000, params=(1000, 0, 4096), in_extra=((4096 << 4) | 0xC, 0x0FFFDAF8),
                   out_extra=((4096 << 4) | 0xC, 0x0FFFDAF8), desc=("mapped-w:4:4096:-",),
                   outdesc=("mapped-w:4:4096:0x00000011",))
    assert read_extent(tmp_path, no_count) == (1000, 5096)


def test_only_reads_cover_bytes(tmp_path):
    # a Write and another request of the same file session on the same thread add no bytes to either side
    other = req(5, 3, FILE, 0x08040000)
    ref = trace(*spans(5, 1, [(0, 4096)]), fs_write(5, 2, 100000, bytes(range(256)) * 16, session=FILE), other)
    cand = trace(*spans(5, 1, [(0, 4096)]))
    (row,) = coverage(ref, cand, tmp_path).values()
    assert (row.ref_bytes, row.cand_bytes, row.ref_only, row.cand_only) == (4096, 4096, 0, 0)
    # a file session the thread only writes is no row at all
    (only,) = coverage(trace(*spans(5, 1, [(0, 4096)]), fs_write(5, 2, 0, b"abcd" * 100, session="fs:USER/file#2")),
                       cand, tmp_path).values()
    assert only.session == FILE


def test_a_thread_or_file_session_that_only_the_candidate_reads_is_all_one_sided(tmp_path):
    ref = trace(*spans(5, 1, [(0, 4096)]))
    cand = trace(*spans(5, 1, [(0, 4096)]), *spans(1, 1, [(0, 2048)], session="fs:USER/file#3"),
                 *spans(1, 3, [(0, 100)], session=FILE))
    rows = coverage(ref, cand, tmp_path)
    assert len(rows) == 3
    new = sorted((r.ref_bytes, r.cand_bytes, r.ref_only, r.cand_only, r.ref_only_ranges, r.cand_only_ranges)
                 for r in rows.values() if r.thread == "main")
    assert new == [(0, 100, 0, 100, 0, 1), (0, 2048, 0, 2048, 0, 1)]
    assert len({r.thread for r in rows.values()}) == 2
    # a thread that the reference has no request on at all
    (alone,) = coverage(trace(req(1, 1, "srv:", 0x00050100)), trace(*spans(5, 1, [(0, 4096)])), tmp_path).values()
    assert (alone.ref_bytes, alone.cand_bytes, alone.ref_only, alone.cand_only) == (0, 4096, 0, 4096)


def test_a_file_session_whose_reads_all_failed_is_still_a_row(tmp_path):
    failed = trace(fs_read(5, 1, 0, 4096, 0, result=0xC8804464))
    (row,) = coverage(failed, trace(*spans(5, 1, [(0, 4096)])), tmp_path).values()
    assert (row.ref_bytes, row.cand_bytes, row.ref_only, row.cand_only) == (0, 4096, 0, 4096)
    (both,) = coverage(failed, failed, tmp_path).values()
    assert (both.ref_bytes, both.cand_bytes, both.ref_only_ranges, both.cand_only_ranges) == (0, 0, 0, 0)


def test_the_ranges_are_the_number_of_separate_pieces(tmp_path):
    ref = trace(*spans(5, 1, [(0, 100), (200, 100), (400, 100)]))
    cand = trace(*spans(5, 1, [(250, 300)]))
    (row,) = coverage(ref, cand, tmp_path).values()
    # the reference covers [0,100) [200,300) [400,500), the candidate [250,550): only the reference [0,100) and
    # [200,250); only the candidate [300,400) and [500,550)
    assert (row.ref_only, row.ref_only_ranges) == (100 + 50, 2)
    assert (row.cand_only, row.cand_only_ranges) == (100 + 50, 2)


def test_the_coverage_line_names_the_largest_one_sided_row_of_each_side():
    C = cmp.Coverage
    rows = [C("#3", FILE, 100, 50, 60, 10, 1, 1), C("#5", FILE, 100, 50, 70, 30, 2, 1), C("#12", FILE, 10, 15, 0, 5, 0, 1)]
    assert cmp.coverage_line(rows) == (
        "fs coverage: 3 thread/file session(s) read; bytes only the reference covers: 130 (in 2, 3 range(s)), only the "
        "candidate covers: 45 (in 3, 3 range(s)); the most per thread and file session: reference 70 on thread #5, "
        "candidate 30 on thread #5 (information, not a rule)")
    # the largest per side, not the smallest, and a tie goes to the later thread
    rows = [C("#3", FILE, 0, 0, 40, 5, 1, 1), C("#5", FILE, 0, 0, 40, 9, 1, 1), C("#7", FILE, 0, 0, 10, 2, 1, 1)]
    assert "reference 40 on thread #5, candidate 9 on thread #5 (information" in cmp.coverage_line(rows)
    # a side with nothing one-sided has no part in the line
    only_candidate = cmp.coverage_line([C("#5", FILE, 0, 30, 0, 30, 0, 1)])
    assert only_candidate == ("fs coverage: 1 thread/file session(s) read; bytes only the reference covers: 0 (in 0, 0 "
                              "range(s)), only the candidate covers: 30 (in 1, 1 range(s)); the most per thread and "
                              "file session: candidate 30 on thread #5 (information, not a rule)")
    only_reference = cmp.coverage_line([C("#5", FILE, 30, 0, 30, 0, 1, 0)])
    assert "the most per thread and file session: reference 30 on thread #5 (information" in only_reference
    assert "candidate 30" not in only_reference
    assert cmp.coverage_line([]) == ("fs coverage: 0 thread/file session(s) read; bytes only the reference covers: 0 "
                                     "(in 0, 0 range(s)), only the candidate covers: 0 (in 0, 0 range(s)) "
                                     "(information, not a rule)")


def test_the_docstring_documents_the_coverage_line():
    for word in ("fs coverage:", "N0-36", "comparators.md section 11", "information"):
        assert word in cmp.__doc__
