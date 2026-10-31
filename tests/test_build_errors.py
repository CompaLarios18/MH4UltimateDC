"""tools/recomp/build.py and tools/build_game.py when a step goes wrong, with a stand-in for the
compiler (subprocess.run replaced: no compiler needed). A compiler the system stops (it runs out
of memory with every core compiling) is tried again on its own; a compile or a link that fails
without a word still stops the build and says how the compiler ended; the install puts the library
in place in one step."""
import os
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

from recomp import build as rbuild  # noqa: E402


class Compiler:
    """subprocess.run as the build calls it: a compile writes its object, a link its library (and
    finds every object it is given), unless `results` says otherwise. `results` maps a source's
    name, or "link", to how its runs end, one (status, what it says) per run, or (0, "", False)
    for a status of 0 with nothing written; runs past the list work. `runs` lists what was run,
    in order."""

    def __init__(self, results=None):
        self.results = {key: list(ends) for key, ends in (results or {}).items()}
        self.runs = []

    def __call__(self, cmd, **kwargs):
        out = cmd[cmd.index("-o") + 1]
        link = "-shared" in cmd
        key = "link" if link else os.path.basename(cmd[cmd.index("-o") - 1])
        self.runs.append(key)
        ends = self.results.get(key)
        code, said, *writes = ends.pop(0) if ends else (0, "")
        if code == 0 and (writes or [True])[0]:
            if link:
                rsp = next(arg[1:] for arg in cmd if arg.startswith("@"))
                with open(rsp) as f:
                    objects = [line.strip().strip('"') for line in f if line.strip()]
                missing = [o for o in objects if not os.path.isfile(o)]
                assert not missing, f"linked without {missing}"
            with open(out, "w") as f:
                f.write("built")
        return subprocess.CompletedProcess(cmd, code, "", said)


def sources(tmp_path, *names):
    src = tmp_path / "src"
    src.mkdir()
    for name in names:
        (src / name).write_text("int f(void) { return 0; }\n")
    return str(tmp_path)


def test_a_compiler_the_system_stopped_is_tried_again_alone(tmp_path, monkeypatch, capsys):
    out = sources(tmp_path, "a.c", "b.c", "c.c")
    cc = Compiler({"b.c": [(-9, "")]})                     # SIGKILL: what the system's OOM killer sends
    monkeypatch.setattr(rbuild.subprocess, "run", cc)
    lib = rbuild.build(out, 3, "O0", [], "lib.so", [])
    assert os.path.isfile(lib)
    assert cc.runs.count("b.c") == 2 and cc.runs.count("a.c") == 1 and cc.runs[-1] == "link"
    text = capsys.readouterr().out
    assert "1 file(s) didn't compile: again, one at a time" in text and "compiling b.c again" in text
    assert "linked " in text


def test_stopped_again_on_its_own_the_build_stops_and_says_why(tmp_path, monkeypatch, capsys):
    out = sources(tmp_path, "a.c", "b.c")
    cc = Compiler({"b.c": [(-9, ""), (-9, "")]})
    monkeypatch.setattr(rbuild.subprocess, "run", cc)
    with pytest.raises(SystemExit) as e:
        rbuild.build(out, 2, "O0", [], "lib.so", [])
    assert e.value.code == 1
    err = capsys.readouterr().err
    assert "b.c" in err and "was stopped by signal 9" in err and "ran out of memory" in err
    assert "link" not in cc.runs                           # never linked without its objects


def test_a_compile_that_fails_without_a_word_still_fails(tmp_path, monkeypatch, capsys):
    """(it used to count as compiled: the link then failed at the end, for want of its object)"""
    out = sources(tmp_path, "a.c", "b.c")
    cc = Compiler({"b.c": [(1, "")]})
    monkeypatch.setattr(rbuild.subprocess, "run", cc)
    with pytest.raises(SystemExit):
        rbuild.build(out, 1, "O0", [], "lib.so", [])        # one job: nothing to try again
    err = capsys.readouterr().err
    assert "error in" in err and "b.c" in err and "ended with status 1" in err
    assert "link" not in cc.runs


def test_real_errors_are_tried_again_once(tmp_path, monkeypatch, capsys):
    """A file that fails again on its own has a real error: the others aren't tried again, and all
    of them are reported with what the compiler said."""
    out = sources(tmp_path, "a.c", "b.c", "c.c")
    cc = Compiler({"a.c": [(1, "a.c:1:1: error: one")] * 2, "b.c": [(1, "b.c:1:1: error: two")]})
    monkeypatch.setattr(rbuild.subprocess, "run", cc)
    with pytest.raises(SystemExit):
        rbuild.build(out, 3, "O0", [], "lib.so", [])
    assert cc.runs.count("a.c") == 2 and cc.runs.count("b.c") == 1 and "link" not in cc.runs
    err = capsys.readouterr().err
    assert "a.c:1:1: error: one" in err and "b.c:1:1: error: two" in err


def test_a_failed_link_says_how_the_linker_ended(tmp_path, monkeypatch):
    out = sources(tmp_path, "a.c")
    for end, words in (((1, ""), "ended with status 1"), ((-9, ""), "was stopped by signal 9"),
                       ((1, "ld: cannot find -lm\n"), "ended with status 1\nld: cannot find -lm")):
        monkeypatch.setattr(rbuild.subprocess, "run", Compiler({"link": [end]}))
        with pytest.raises(SystemExit) as e:
            rbuild.build(out, 1, "O0", [], "lib.so", [])
        message = str(e.value.code)
        assert message.startswith("linking lib.so failed: ") and words in message, message


def test_a_status_of_0_with_nothing_written_is_a_failure(tmp_path, monkeypatch):
    """What a Python that can't wait for its children (SIGCHLD ignored) sees: every status 0."""
    out = sources(tmp_path, "a.c", "b.c")
    cc = Compiler({"b.c": [(0, "", False)]})
    monkeypatch.setattr(rbuild.subprocess, "run", cc)
    with pytest.raises(SystemExit, match="1 object.* missing after compiling, b-O0-.*status 0 without writing"):
        rbuild.build(out, 2, "O0", [], "lib.so", [])
    assert "link" not in cc.runs
    monkeypatch.setattr(rbuild.subprocess, "run", Compiler({"link": [(0, "", False)]}))
    with pytest.raises(SystemExit, match="linking lib.so failed: .* status 0 without writing it"):
        rbuild.build(out, 2, "O0", [], "lib.so", [])


def test_how_it_ended():
    assert rbuild.how_it_ended(1) == "ended with status 1"
    assert rbuild.how_it_ended(-9).startswith("was stopped by signal 9")
    assert "out of memory" in rbuild.how_it_ended(-9)
    assert "signal 11" in rbuild.how_it_ended(-11) and "memory" not in rbuild.how_it_ended(-11)
    # a Windows crash: its NTSTATUS in hexadecimal
    assert rbuild.how_it_ended(0xC0000005) == "ended with status 3221225477 (0xC0000005)"


def test_install_puts_the_library_in_place_in_one_step(tmp_path):
    """A game that runs on the old library keeps it as it was (here a second name for the old
    file): the new one takes its name, it isn't copied over it."""
    import build_game
    new = tmp_path / "new.so"
    new.write_bytes(b"new library")
    dest = tmp_path / "recomp" / "0004000000126300.so"
    dest.parent.mkdir()
    dest.write_bytes(b"old library")
    running = tmp_path / "running.so"
    try:
        os.link(dest, running)
    except (OSError, NotImplementedError):
        pytest.skip("no hard links here")
    build_game.install_library(str(new), str(dest))
    assert dest.read_bytes() == b"new library"
    assert running.read_bytes() == b"old library"
    assert os.listdir(dest.parent) == [dest.name]          # nothing left beside it


def test_a_failed_install_leaves_the_old_library(tmp_path, monkeypatch):
    import build_game
    new = tmp_path / "new.so"
    new.write_bytes(b"new library")
    dest = tmp_path / "recomp" / "0004000000126300.so"
    dest.parent.mkdir()
    dest.write_bytes(b"old library")

    def disk_full(src, dst):
        with open(dst, "wb") as f:
            f.write(b"new li")
        raise OSError(28, "No space left on device")
    monkeypatch.setattr(build_game.shutil, "copy2", disk_full)
    with pytest.raises(SystemExit, match="couldn't put the code library at .*No space left on device"):
        build_game.install_library(str(new), str(dest))
    assert dest.read_bytes() == b"old library"
    assert os.listdir(dest.parent) == [dest.name]


def synthetic_cxi(tmp_path, program_id=0x0004000000ABCD00):
    import make_cxi
    text = struct.pack("<4I", 0xE3A0002A, 0xE12FFF1E, 0xE0800001, 0xE12FFF1E).ljust(0x1000, b"\0")
    exheader = bytearray(0x800)
    exheader[0:8] = b"SYNTH\0\0\0"
    struct.pack_into("<III", exheader, 0x10, 0x00100000, 1, len(text))
    struct.pack_into("<III", exheader, 0x20, 0x00101000, 1, 0x1000)
    struct.pack_into("<III", exheader, 0x30, 0x00102000, 1, 0x1000)
    struct.pack_into("<Q", exheader, 0x200, program_id)
    code = text + bytes(0x2000)
    exefs = bytearray(0x200)
    struct.pack_into("<8sII", exefs, 0, b".code", 0, len(code))
    cxi = tmp_path / "synthetic.cxi"
    cxi.write_bytes(make_cxi.make_cxi(bytes(exheader), bytes(exefs) + code + bytes(-len(code) % 0x200)))
    functions = tmp_path / "functions.tsv"
    functions.write_text("address\tsize\tmode\tkind\tname\tstatus\towner\tnotes\n"
                         "0x00100000\t0x8\tarm\tunknown\t\ttodo\t\t\n")
    return cxi, functions


def test_build_game_names_its_compiler_and_installs(tmp_path, monkeypatch, capsys):
    """build_game.py from the dump to the install, the compile itself left out: the compiler's
    line names the program found on the PATH (for a report), and the library is installed."""
    import build_game
    cxi, functions = synthetic_cxi(tmp_path)

    def fake_build(outdir, jobs, opt, defines, lib_name, extra_sources, cc="clang", **kwargs):
        lib = os.path.join(outdir, lib_name)
        with open(lib, "w") as f:
            f.write(f"built with {cc}")
        return lib
    monkeypatch.setattr(build_game.rbuild, "build", fake_build)
    monkeypatch.setattr(build_game.shutil, "which", lambda name: "/usr/bin/clang" if name == "clang" else None)
    install = tmp_path / "azahar"
    assert build_game.main(["--dump", str(cxi), "--out", str(tmp_path / "out"), "--opt", "O0", "--any-version",
                            "--functions", str(functions), "--extra", "", "--hooks", "", "--tunables", "",
                            "--cc", "clang", "--install", str(install), "--no-mods", "-j", "2"]) == 0
    text = capsys.readouterr().out
    assert "compiling with /usr/bin/clang -O0 (2 jobs;" in text
    lib = install / "recomp" / build_game.library_name(0x0004000000ABCD00)
    assert lib.read_text() == "built with clang"
    assert f"installed to {lib}" in text
    assert sorted(os.listdir(lib.parent)) == sorted([lib.name, "0004000000ABCD00.build.json"])
