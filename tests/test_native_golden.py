"""tools/native/golden.py: session files, manifests, record and check, with synthetic data and a fake runner.

The fake runner is a Python script behind a launcher the test creates per OS (a .cmd on Windows, a
shell script elsewhere), so golden.py starts it the way it starts the real mh4u_runner.
"""
import json
import os
import stat
import struct
import sys
import textwrap
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools" / "native"))

import golden  # noqa: E402

FAKE = '''
import json, os, struct, sys
sys.path.insert(0, {tools!r})
import capfmt, gcap

argv = sys.argv[1:]
mode = os.environ.get("FAKE_MODE", "")


def opt(name, default=None):
    return argv[argv.index(name) + 1] if name in argv else default


with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(json.dumps({{"argv": argv, "tz": os.environ.get("TZ"), "cwd": os.getcwd()}}) + "\\n")
counter = os.environ["FAKE_LOG"] + ".count"
n = int(open(counter).read()) if os.path.exists(counter) else 0
open(counter, "w").write(str(n + 1))
if mode == "fail":
    sys.exit(3)
frames = int(opt("--frames"))
user = opt("--user")
with open(os.path.join(user, "ran.txt"), "a") as f:
    f.write("run\\n")
change = {{"late_scene": 12, "very_late": 16}}.get(mode, 10)
if opt("--trace-svc"):
    svc = ["# svctrace v1 2026-01-01 game=0004000000155400 cpu=recomp new3ds=off",
           "1 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=10",
           "1 2B pc=001091E0 lr=001078BC 0FFFFFE8 FFFF8001 00000014 00000001 t=20"]
    if mode == "svc_diff":
        svc[2] = svc[2].replace("00000014", "00000015")
    open(opt("--trace-svc"), "w").write("\\n".join(svc) + "\\n")
if opt("--trace-ipc"):
    ipc = ["# ipctrace v1 2026-01-01 game=0004000000155400 cpu=recomp new3ds=off",
           "1 seq=1 session=srv:USER cmd=0x00010040 in=0x00010040 desc=- out=0x00010040,0x00000000 outdesc=- t=5",
           "1 seq=2 session=srv:USER cmd=0x00020080 in=0x00020080 desc=- out=0x00020080,%s outdesc=- t=6"
           % ("0xC8804464" if mode == "ipc_diff" else "0x00000000")]
    open(opt("--trace-ipc"), "w").write("\\n".join(ipc) + "\\n")
if opt("--state-trace"):
    lines = ["# statetrace v1 run %d" % n]
    for fr in range(1, frames + 1):
        scene = 0 if fr < change else 2
        extra = (n if mode == "vary_state" else 0)
        lines.append("f=%d t=%d scene=0x%02x frame_counter=0x%08x smain_delta=- quest_id=%d stage_loader_state=-"
                     % (fr, fr * 4470000, scene, fr, extra))
    if mode != "no_state":
        open(opt("--state-trace"), "w").write("\\n".join(lines) + "\\n")
if opt("--capture-hid"):
    with capfmt.Writer(opt("--capture-hid"), "hid") as w:
        for fr in range(1, frames + 1):
            w.hid(fr, fr * 4470000, capfmt.make_hid(buttons=1 if fr == (13 if mode == "hid_diff" else 12) else 0))
for flag, kind in (("--capture-dsp", "dsp"), ("--capture-gpu", "gpu")):
    if flag in argv:
        start, count, path = opt(flag).split(":", 2)
        start, count = int(start), int(count)
        if kind == "dsp":
            with capfmt.Writer(path, "dsp") as w:
                for i in range(count):
                    r = capfmt.make_region(i, {{0: capfmt.make_status(enabled=1)}} if mode == "dsp_diff" else None)
                    w.dsp_regions(start + i, i, r, r)
                    w.dsp_pcm(start + i, i, capfmt.make_pcm([(i, 0)] * 160))
        else:
            with open(path, "wb") as fh:
                g = gcap.Writer(fh)
                g.init()
                for i in range(count):
                    g.record(gcap.FRAME, struct.pack("<I", start + i), frame=start + i)
            os.makedirs(path + ".frames", exist_ok=True)
            open(os.path.join(path + ".frames", "%d_top.ppm" % start), "w").write("P3 1 1 255 0 0 0\\n")
if opt("--count-unmapped"):
    open(opt("--count-unmapped"), "w").write("count=1 addr=00000000 size=4 rw=read lr=00000000\\n")
if opt("--shots"):
    os.makedirs(opt("--shots"), exist_ok=True)
    px = "1 1 1" if mode == "pixels" else "0 0 0"
    for ms in (1000, 2000):
        open(os.path.join(opt("--shots"), "%d_top.ppm" % ms), "w").write("P3 1 1 255 " + px + "\\n")
'''

SESSION = """\
# name: S1
# needs: none
# frames: 30
# checkpoints: 5:0, 20:2
# captures: gpu:10:2, dsp:10:3
f12:A
f15:TOUCH:160:120:f3, f20:START
"""


@pytest.fixture
def env(tmp_path, monkeypatch):
    """A fake runner behind a per-OS launcher, a sessions folder, a user folder and fake game files."""
    script = tmp_path / "fake_runner.py"
    script.write_text(FAKE.format(tools=str(ROOT / "tools" / "native")), encoding="utf-8")
    if os.name == "nt":
        launcher = tmp_path / "fake_runner.cmd"
        launcher.write_text(f'@"{sys.executable}" "{script}" %*\r\n', encoding="ascii")
    else:
        launcher = tmp_path / "fake_runner"
        launcher.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="ascii")
        launcher.chmod(launcher.stat().st_mode | stat.S_IXUSR)
    sessions = tmp_path / "sessions"
    sessions.mkdir()
    (sessions / "S1.txt").write_text(SESSION, encoding="utf-8")
    user = tmp_path / "user"
    (user / "sdmc").mkdir(parents=True)
    (user / "sdmc" / "a.txt").write_text("x")
    game, update, lib = tmp_path / "game.cxi", tmp_path / "update.cia", tmp_path / "code.dll"
    for f in (game, update, lib):
        f.write_bytes(b"synthetic")
    log = tmp_path / "fake.log"
    monkeypatch.setenv("FAKE_LOG", str(log))
    monkeypatch.delenv("FAKE_MODE", raising=False)

    class E:
        pass

    e = E()
    e.tmp, e.script, e.launcher, e.sessions, e.user = tmp_path, script, launcher, sessions, user
    e.game, e.update, e.lib, e.log, e.out = game, update, lib, log, tmp_path / "golden"
    e.calls = lambda: [json.loads(x) for x in log.read_text().splitlines()]
    e.common = ["--out", str(e.out), "--sessions-dir", str(sessions)]
    e.ref = ["--runner", str(launcher), "--game", str(game), "--update", str(update), "--code", str(lib)]
    e.record = lambda *extra: golden.main(["record", "S1", *e.common, *e.ref, "--user", str(user), *extra])
    e.check = lambda *extra: golden.main(["check", "S1", *e.common, *e.ref, *extra])
    return e


# ---- session files

def write_session(tmp_path, text, name="S1"):
    p = tmp_path / f"{name}.txt"
    p.write_text(text, encoding="utf-8")
    return p


def test_parse_session(tmp_path):
    s = golden.parse_session(write_session(tmp_path, SESSION))
    assert (s.name, s.needs, s.frames, s.events, s.draft) == ("S1", "none", 30, 3, False)
    assert s.checkpoints == [(5, 0), (20, 2)]
    assert [(c.kind, c.start, c.frames, c.filename) for c in s.captures] == [
        ("gpu", 10, 2, "gpu-10.mgpu"), ("dsp", 10, 3, "dsp-10.mdsp")]


def test_hex_scene_and_draft(tmp_path):
    text = SESSION.replace("5:0, 20:2", "").replace("# captures", "# draft: yes\n# captures")
    s = golden.parse_session(write_session(tmp_path, text))
    assert s.draft and s.checkpoints == []
    s = golden.parse_session(write_session(tmp_path, SESSION.replace("20:2", "20:0x1f")))
    assert s.checkpoints[1] == (20, 31)


@pytest.mark.parametrize("old,new,message", [
    ("# name: S1", "# name: S2", "differs from the file name"),
    ("# needs: none", "# needs: maybe", "needs must be"),
    ("# frames: 30", "# frames: 0", "positive"),
    ("5:0, 20:2", "20:2, 5:0", "increasing"),
    ("5:0, 20:2", "5:0, 99:2", "after the last frame"),
    ("5:0, 20:2", "", "no checkpoints"),
    ("5:0, 20:2", "five:0", "bad checkpoint"),
    ("gpu:10:2", "gpu:10:31", "frames must be"),
    ("gpu:10:2", "gpu:10:2, gpu:10:3", "twice"),
    ("gpu:10:2", "vid:10:2", "bad capture"),
    ("gpu:10:2", "gpu:99:2", "after the last frame"),
    ("f12:A", "12:A", "must be frames"),
    ("f12:A", "f12:JUMP", "unknown button"),
    ("f12:A", "f99:A", "after the end"),
    ("f12:A", "f12:TOUCH:1", "wrong number"),
    ("f12:A", "f12:A:soon", "bad hold"),
    ("# needs: none\n", "", "missing header"),
])
def test_bad_sessions(tmp_path, old, new, message):
    with pytest.raises(golden.GoldenError, match=message):
        golden.parse_session(write_session(tmp_path, SESSION.replace(old, new)))


def test_shipped_sessions_parse():
    names = sorted(p.stem for p in golden.SESSIONS_DIR.glob("*.txt"))
    assert names == ["G1", "G2", "G6"]
    for n in names:
        s = golden.parse_session(golden.SESSIONS_DIR / f"{n}.txt")
        assert s.needs == "none"
    g1 = golden.parse_session(golden.SESSIONS_DIR / "G1.txt")
    assert {(c.kind, c.frames) for c in g1.captures} == {("gpu", 3), ("dsp", 300)}
    g2 = golden.parse_session(golden.SESSIONS_DIR / "G2.txt")
    assert [(c.kind, c.frames) for c in g2.captures] == [("gpu", 3)]


def test_shipped_sessions_event_counts():
    # golden.py's own parser: the number of input events of each shipped session
    counts = {n: golden.parse_session(golden.SESSIONS_DIR / f"{n}.txt").events for n in ("G1", "G2", "G6")}
    assert counts == {"G1": 8, "G2": 52, "G6": 23}


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="reference runner not set (MH4U_RUNNER)")
@pytest.mark.parametrize("name", ["G1", "G2", "G6"])
def test_reference_runner_accepts_the_shipped_sessions(name):
    import subprocess
    done = subprocess.run([os.environ["MH4U_RUNNER"], "--input-file", str(golden.SESSIONS_DIR / f"{name}.txt"),
                           "--print-schedule"], capture_output=True, timeout=120, check=False)
    assert done.returncode == 0, done.stderr.decode("utf-8", "replace")[-300:]


# ---- record

def test_record(env):
    assert env.record() == 0
    out = env.out / "S1"
    for name in ("svc.txt", "ipc.txt", "state.txt", "unmapped.txt", "hid.mhid", "hid.mhid.json", "dsp-10.mdsp",
                 "gpu-10.mgpu", "manifest.json", "runner.log"):
        assert (out / name).is_file(), name
    assert (out / "shots" / "1000_top.ppm").is_file()
    assert (out / "gpu-10.mgpu.frames" / "10_top.ppm").is_file()
    assert (out / "user-before" / "sdmc" / "a.txt").is_file()
    assert not (out / "user-before" / "ran.txt").exists()          # the run used a working copy
    assert (out / "user-after" / "ran.txt").read_text() == "run\n"   # and left it as user-after/
    assert not (out / "work").exists()
    m = json.loads((out / "manifest.json").read_text())
    assert m["tz"] == "UTC0" and m["runner"]["exit"] == 0 and m["runner"]["new_3ds"] == "off"
    assert m["runner"]["cpu"] == "recomp" and m["code_library"]["sha256"] == golden.sha256_file(env.lib)
    assert [c["result"] for c in m["checkpoint_results"]] == ["ok", "ok"]
    assert m["capture_problems"] == [] and "manifest.json" not in m["files"]
    assert m["files"]["svc.txt"]["sha256"] == golden.sha256_file(out / "svc.txt")
    assert not any(k.startswith(("user-before", "user-after")) for k in m["files"])
    call = env.calls()[0]
    assert call["tz"] == "UTC0"
    a = call["argv"]
    assert a[0] == str(env.game)
    for pair in (["--new-3ds", "off"], ["--cpu", "recomp"], ["--code", str(env.lib)], ["--install", str(env.update)],
                 ["--frames", "30"], ["--input-file", str(env.sessions / "S1.txt")]):
        assert a[a.index(pair[0]) + 1] == pair[1]
    assert a[a.index("--user") + 1] == str(out / "work")
    assert str(out / "gpu-10.mgpu") in "".join(a) and "--trace-ipc-full" in a and "--count-unmapped" in a


def test_record_refusals(env, capsys):
    assert env.record() == 0
    assert env.record() == 2 and "not empty" in capsys.readouterr().err
    assert env.record("--force") == 0
    (env.sessions / "D.txt").write_text(SESSION.replace("S1", "D").replace("5:0, 20:2", "")
                                        .replace("# captures", "# draft: yes\n# captures"))
    assert golden.main(["record", "D", *env.common, *env.ref, "--user", str(env.user)]) == 2
    assert "draft" in capsys.readouterr().err
    (env.sessions / "N.txt").write_text(SESSION.replace("S1", "N").replace("none", "save"))
    assert golden.main(["record", "N", *env.common, *env.ref, "--user", str(env.user)]) == 2
    assert "needs a save" in capsys.readouterr().err
    assert golden.main(["record", "S1", *env.common, "--runner", str(env.launcher), "--game", str(env.game),
                        "--user", str(env.user), "--force"]) == 2        # recomp with no --code
    assert "needs --code" in capsys.readouterr().err


def test_record_save_is_merged(env):
    save = env.tmp / "save"
    (save / "sdmc").mkdir(parents=True)
    (save / "sdmc" / "save.bin").write_text("s")
    assert env.record("--save", str(save)) == 0
    assert (env.out / "S1" / "user-before" / "sdmc" / "save.bin").is_file()


def test_record_fails_when_the_runner_fails_or_misses_a_checkpoint(env, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "fail")
    assert env.record() == 1
    monkeypatch.setenv("FAKE_MODE", "late_scene")      # scene changes at frame 12, checkpoint 20:2 still ok
    assert env.record("--force") == 0
    (env.sessions / "S1.txt").write_text(SESSION.replace("5:0, 20:2", "5:0, 11:2"))
    assert env.record("--force") == 1                   # at frame 11 the scene is still 0
    m = json.loads((env.out / "S1" / "manifest.json").read_text())
    assert [c["result"] for c in m["checkpoint_results"]] == ["ok", "mismatch"]


# ---- check

def test_check_same_runner_passes(env, capsys):
    assert env.record() == 0
    assert env.check() == 0
    text = capsys.readouterr().out
    for name in ("svc", "ipc", "state", "checkpoints", "hid", "dsp-10", "images"):
        assert f"S1: {name}: PASS" in text
    assert "0 failed, 0 skipped" in text
    calls = env.calls()
    assert len(calls) == 2 and calls[1]["tz"] == "UTC0"
    a = calls[1]["argv"]
    assert "--capture-gpu" not in a and "--new-3ds" in a
    # it ran on a copy of user-before/, so the recording is untouched
    assert not (env.out / "S1" / "user-before" / "ran.txt").exists()


@pytest.mark.parametrize("mode,failing", [("svc_diff", "svc"), ("vary_state", "state"), ("pixels", "images"),
                                          ("very_late", "state")])
def test_check_detects_differences(env, capsys, monkeypatch, mode, failing):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", mode)
    capsys.readouterr()
    assert env.check() == 1
    assert f"S1: {failing}: FAIL" in capsys.readouterr().out


@pytest.mark.parametrize("mode,failing", [("ipc_diff", "ipc"), ("dsp_diff", "dsp-10"), ("hid_diff", "hid")])
def test_check_detects_ipc_dsp_and_hid_differences(env, capsys, monkeypatch, mode, failing):
    monkeypatch.setattr(golden, "IPC_TIME_DEPENDENT", env.tmp / "no_such_file.txt")
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", mode)
    capsys.readouterr()
    assert env.check() == 1
    out = capsys.readouterr().out
    assert f"S1: {failing}: FAIL" in out
    for other in {"ipc", "dsp-10", "hid"} - {failing}:
        assert f"S1: {other}: PASS" in out


def test_ignore_option_and_the_time_dependent_list(env, capsys, monkeypatch):
    absent = env.tmp / "no_such_file.txt"
    monkeypatch.setattr(golden, "IPC_TIME_DEPENDENT", absent)
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "ipc_diff")
    capsys.readouterr()
    assert env.check() == 1 and "S1: ipc: FAIL" in capsys.readouterr().out
    # --ignore drops the command whose reply differs
    ignore = env.tmp / "ignore.txt"
    ignore.write_text("srv:USER 00020080   # test\n", encoding="utf-8")
    assert env.check("--ignore", str(ignore)) == 0
    assert "S1: ipc: PASS" in capsys.readouterr().out
    # an ignore file for another command does not help
    other = env.tmp / "other.txt"
    other.write_text("srv:USER 00010040\n", encoding="utf-8")
    assert env.check("--ignore", str(other)) == 1
    capsys.readouterr()
    # the repository's list is honoured when it exists
    monkeypatch.setattr(golden, "IPC_TIME_DEPENDENT", ignore)
    assert env.check() == 0
    assert "S1: ipc: PASS" in capsys.readouterr().out


def test_candidate_without_a_state_trace_skips_state_and_checkpoints(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "no_state")
    capsys.readouterr()
    assert env.check("--candidate-runner", str(env.script)) == 0
    out = capsys.readouterr().out
    assert "S1: state: SKIPPED" in out and "S1: checkpoints: SKIPPED" in out
    assert "S1: svc: PASS" in out and "0 failed, 3 skipped" in out        # state, checkpoints, images
    # the reference runner not writing it is a failure, for both
    assert env.check() == 1
    out = capsys.readouterr().out
    assert "S1: state: FAIL" in out and "S1: checkpoints: FAIL" in out


@pytest.mark.parametrize("text,expected", [
    ("--code C:\\a\\b.dll --x 1", ["--code", "C:\\a\\b.dll", "--x", "1"]),
    ('--code "C:\\Program Files\\b.dll"', ["--code", "C:\\Program Files\\b.dll"]),
] if os.name == "nt" else [
    ("--code /a/b.so --x 1", ["--code", "/a/b.so", "--x", "1"]),
    ("--code '/a b/c.so'", ["--code", "/a b/c.so"]),
])
def test_split_args_keeps_backslash_paths_on_windows(text, expected):
    assert golden.split_args(text) == expected
    assert golden.split_args(None) == [] and golden.split_args("") == []


@pytest.mark.skipif(os.name != "nt", reason="backslash paths")
def test_candidate_args_with_a_windows_path_reach_the_runner_intact(env):
    assert env.record() == 0
    path = "C:\\no\\such\\dir\\native.dll"
    assert env.check("--candidate-runner", str(env.script), f"--candidate-args=--native {path}") == 0
    assert env.calls()[1]["argv"][-2:] == ["--native", path]


def test_check_runner_failure(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "fail")
    assert env.check() == 1
    assert "runner: FAIL" in capsys.readouterr().out


def test_check_needs_a_recording_and_notices_tampering(env, capsys):
    assert env.check() == 2 and "not recorded" in capsys.readouterr().err
    assert env.record() == 0
    with open(env.out / "S1" / "svc.txt", "a") as f:
        f.write("# edited\n")
    assert env.check() == 2 and "changed since" in capsys.readouterr().err


def test_candidate_gets_its_own_program_library_and_shared_options_only(env, capsys):
    assert env.record() == 0
    prog, lib2 = env.tmp / "prog", env.tmp / "native.so"
    prog.mkdir()
    lib2.write_bytes(b"x")
    cand_runner = env.tmp / "fake_runner.py"        # a .py runner is started with the Python interpreter
    assert env.check("--candidate-runner", str(cand_runner), "--candidate-program", str(prog),
                     "--candidate-code", str(lib2), "--candidate-args=--native --extra 1",
                     "--set", "5000:fps=60") == 0
    a = env.calls()[1]["argv"]
    assert a[0] == str(prog) and a[a.index("--code") + 1] == str(lib2)
    assert a[-3:] == ["--native", "--extra", "1"]
    assert a[a.index("--set") + 1] == "5000:fps=60"
    for not_shared in ("--new-3ds", "--install", "--cpu", "--capture-gpu", "--count-unmapped", "--shots",
                       "--shot-every"):
        assert not_shared not in a
    for shared in ("--user", "--frames", "--input-file", "--trace-svc", "--trace-ipc", "--trace-ipc-full",
                   "--state-trace", "--capture-hid", "--capture-dsp"):
        assert shared in a
    out = capsys.readouterr().out
    assert "S1: images: SKIPPED" in out and "S1: svc: PASS" in out
    assert "0 failed, 1 skipped" in out


def test_candidate_skipped_comparisons_are_not_passes(env, capsys):
    assert env.record() == 0
    # a candidate that writes nothing comparable: every comparison is skipped, none passes
    quiet = env.tmp / "quiet.py"
    quiet.write_text("import sys, os\n"
                     "u = sys.argv[sys.argv.index('--user') + 1]\n"
                     "open(os.path.join(u, 'ran.txt'), 'a').write('x')\n")
    capsys.readouterr()
    assert env.check("--candidate-runner", str(quiet)) == 0
    out = capsys.readouterr().out
    assert "PASS" not in out and "SKIPPED" in out and "0 passed" in out
    # but the reference runner missing a file is a failure
    broken = env.tmp / "broken.py"
    broken.write_text(quiet.read_text())
    assert env.check("--runner", str(broken)) == 1


def test_repeat(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--repeat", "3") == 0
    out = capsys.readouterr().out
    assert "repeat: PASS" in out and "run3: state: PASS" in out
    assert len(env.calls()) == 4
    monkeypatch.setenv("FAKE_MODE", "vary_state")     # state differs from run to run
    assert env.check("--repeat", "2") == 1
    out = capsys.readouterr().out
    assert "repeat: FAIL" in out
    assert env.check("--repeat", "0") == 2


# ---- list, helpers

def test_list(env, capsys):
    assert golden.main(["list", *env.common]) == 0
    assert "S1: needs=none frames=30 events=3 checkpoints=2 captures=gpu:10:2,dsp:10:3 not recorded" in capsys.readouterr().out
    assert env.record() == 0
    capsys.readouterr()
    golden.main(["list", *env.common])
    assert capsys.readouterr().out.strip().endswith(" recorded")
    (env.sessions / "Bad.txt").write_text("# name: Bad\n")
    golden.main(["list", *env.common])
    assert "Bad: INVALID" in capsys.readouterr().out


def test_missing_runner_and_session(env, capsys):
    assert golden.main(["record", "S1", *env.common, "--runner", str(env.tmp / "nope.exe"), "--game", str(env.game),
                        "--user", str(env.user), "--cpu", "jit"]) == 2
    assert "runner not found" in capsys.readouterr().err
    assert golden.main(["record", "S9", *env.common, "--runner", "x", "--game", "y", "--user", "z"]) == 2
    assert golden.main(["record", "../S1", *env.common, "--runner", "x", "--game", "y", "--user", "z"]) == 2


def test_runner_argv_uses_the_interpreter_for_scripts(env):
    assert golden.runner_argv(env.script) == [sys.executable, str(env.script)]
    assert golden.runner_argv(env.launcher) == [str(env.launcher)]


def test_child_env_sets_utc(monkeypatch):
    monkeypatch.setenv("TZ", "PST8PDT")
    assert golden.child_env()["TZ"] == "UTC0"
    assert os.environ["TZ"] == "PST8PDT"
