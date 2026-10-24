"""tools/native/golden.py: session files, manifests, record and check, with synthetic data and a fake runner.

The fake runner is a Python script behind a launcher the test creates per OS (a .cmd on Windows, a
shell script elsewhere), so golden.py starts it the way it starts the real mh4u_runner.  What the fake
writes follows the real runner's formats; FAKE_MODE makes it write something else (a different scene
frame, a capture without sound, ...), and `--cpu jit` makes it behave like another timing model (other
ticks, one more polling call, a time-dependent reply).
"""
import json
import os
import re
import stat
import struct
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools" / "native"))

import golden  # noqa: E402

FAKE = r'''
import json, os, struct, sys, zlib
sys.path.insert(0, @TOOLS@)
import capfmt, gcap

argv = sys.argv[1:]
mode = os.environ.get("FAKE_MODE", "")


def opt(name, default=None):
    return argv[argv.index(name) + 1] if name in argv else default


if argv == ["--help"]:      # golden.py asks a candidate runner what it can do (N2b-17); not a run, not logged
    print("usage: fake_runner PROGRAM [options]\n  --frames N\n  --user DIR")
    if not os.environ.get("FAKE_NO_SHOTS"):
        print("  --shots DIR  --shots-from fN  --shot-every fN")
    sys.exit(0)
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(json.dumps({"argv": argv, "tz": os.environ.get("TZ"), "cwd": os.getcwd(),
                        "mh4u": {k: v for k, v in os.environ.items() if k.startswith("MH4U_")}}) + "\n")
counter = os.environ["FAKE_LOG"] + ".count"
n = int(open(counter).read()) if os.path.exists(counter) else 0
open(counter, "w").write(str(n + 1))
if mode == "fail":
    sys.exit(3)
frames = int(opt("--frames"))
user = opt("--user")
with open(os.path.join(user, "ran.txt"), "a") as f:
    f.write("run\n")
jit = opt("--cpu") == "jit"
cpu = opt("--cpu", "native")
change = {"late_scene": 12, "very_late": 15, "late4": 14, "shift1": 11, "late15": 25, "late30": 40}.get(mode, 10)
second = 999 if mode == "one_change" else {"late15": 40, "late30": 55}.get(mode, 25)
if mode == "no_change":
    change = second = 999


def tick(fr):
    return fr * 4470000 + (7 if jit else 0)


def head(tool):
    return "# %s v1 2026-01-01 game=0x0004000000126300 cpu=%s new3ds=off" % (tool, cpu)


THREAD1 = "# thread 1 parent=0 entry=00100000 arg=00000000 stack=10000000 prio=48 core=0/0"
if opt("--trace-svc"):
    svc = [head("svctrace"), THREAD1]
    if mode == "thread_line":
        svc.append("# thread 2 parent=1 entry=00200000 arg=00000000 stack=10000000 prio=48 core=0/0")
    code = "E0E01BF5" if mode == "svc_diff" else "00000000"
    svc.append("1 21 pc=00107328 lr=00106790 0FFFFFE0 00000000 00000000 00000000 t=%d in4=00000000,00000000,00000000 "
               "out=%s,00000001,00000000,00000000" % (tick(1), code))
    for k in range(4 if jit else 1):      # the polling call: another timing model runs it more often
        svc.append("1 28 pc=00109000 lr=00109004 00000000 00000000 00000000 00000000 t=%d in4=00000000,00000000,00000000 "
                   "out=00000000,%08X,00000000,00000000" % (tick(2) + k, tick(2) + k))
    svc.append("1 2B pc=001091E0 lr=001078BC 0FFFFFE8 FFFF8001 00000014 00000001 t=%d in4=00000000,00000000,00000000 "
               "out=00000000,F0000000,00000000,00000001" % tick(3))
    open(opt("--trace-svc"), "w").write("\n".join(svc) + "\n")
if opt("--trace-ipc"):
    ipc = [head("ipctrace"), THREAD1,
           "1 seq=1 session=srv:USER cmd=0x00010040 in=0x00010040 desc=- out=0x00010040,0x00000000 outdesc=- t=5",
           "1 seq=2 session=srv:USER cmd=0x00020080 in=0x00020080 desc=- out=0x00020080,%s outdesc=- t=6"
           % ("0xC8804464" if (mode == "ipc_diff" or jit) else "0x00000000")]
    # D52: Reads of a file and a Write of 32 bytes.  Another timing model reads in another order and one read
    # fewer, and its Write holds other bytes in the range the mask file masks (4 to 7)
    reads = [(0, 16, 0x11), (16, 16, 0x22), (32, 16, 0x33)]
    if jit:
        reads = reads[:2][::-1]          # the last read is missing, the two others come in the other order
    if mode == "read_crc":
        reads[0] = (reads[0][0], 16, 0x99)
    if mode == "reads_many":
        reads += [(1000 + 16 * i, 16, 0x100 + i) for i in range(17)]
    if mode == "read_far":                             # N0-37: one 8-byte read next to nothing the recording reads
        reads += [(900000, 8, 0x777)]
    if mode == "read_near":                            # N0-37: one 16-byte read that goes on where the stream ends
        reads += [(48, 16, 0x778)]
    if mode == "reads_apart":                          # 17 more reads, none touching another: 17 more runs
        reads += [(1000 + 32 * i, 16, 0x100 + i) for i in range(17)]
    if mode == "pieces":
        # D76: one reader reads 32 bytes of a file in two pieces, the other (jit) in one: the same bytes
        blob = bytes((i * 5 + 1) & 255 for i in range(64))
        reads = [(0, 32, zlib.crc32(blob[:32]))] if jit else [(0, 16, zlib.crc32(blob[:16])), (16, 16, zlib.crc32(blob[16:32]))]
    for k, (off, size, crc) in enumerate(reads):
        ipc.append("1 seq=%d session=fs:USER/file#0 cmd=0x080200c2 in=0x080200c2,0x%08x,0x00000000,0x%08x,0x%08x,"
                   "0x0fffdaf8 desc=mapped-w:4:%d:- out=0x08020082,0x00000000,0x%08x,0x%08x,0x0fffdaf8 "
                   "outdesc=mapped-w:4:%d:0x%08x t=%d" % (10 + k, off, size, (size << 4) | 0xC, size, size,
                                                          (size << 4) | 0xC, size, crc, 10 + k))
    data = bytearray(range(32))
    if jit:
        data[4:8] = b"\xee" * 4
    if mode == "write_outside":
        data[10] ^= 1
    ipc.append("1 seq=40 session=fs:USER/file#1 cmd=0x08030102 in=0x08030102,0x00000040,0x00000000,0x00000020,0x00010001,"
               "0x0000020a,0x32ef8df0 desc=mapped-r:5:32:0x%08x out=0x08030082,0x00000000,0x00000020,0x0000020a,0x32ef8df0 "
               "outdesc=- t=40 data=5:%s" % (zlib.crc32(bytes(data)), bytes(data).hex()))
    if mode == "thread_line":
        ipc.insert(2, "# thread 2 parent=1 entry=00200000 arg=00000000 stack=10000000 prio=48 core=0/0")
    if mode in ("ir_ok", "ir_missing"):
        ipc.append("1 seq=3 session=ir:USER cmd=0x00010182 in=0x00010182 desc=- out=0x00010040,0x00000000 outdesc=- t=%d"
                   % tick(15))
    open(opt("--trace-ipc"), "w").write("\n".join(ipc) + "\n")
if opt("--state-trace"):
    lines = ["# statetrace v1 2026-01-01 run %d" % n]
    for fr in range(1, frames + 1):
        scene = 0 if fr < change else (2 if fr < second else 4)
        for after, value in sorted((int(x.split(":")[0]), int(x.split(":")[1]))     # FAKE_SCENES=frame:scene,...
                                   for x in os.environ.get("FAKE_SCENES", "").split(",") if x):
            if fr >= after:
                scene = value
        lines.append("f=%d t=%d scene=0x%02x applet_state=0x00 frame_counter=0x%08x smain_delta=- quest_id=%s "
                     "stage_loader_state=-" % (fr, tick(fr), scene, fr, n if mode == "vary_state" else "-"))
    if mode != "no_state":
        open(opt("--state-trace"), "w").write("\n".join(lines) + "\n")
if opt("--capture-hid"):
    with capfmt.Writer(opt("--capture-hid"), "hid") as w:
        for fr in range(1, frames + 1):
            w.hid(fr, tick(fr), capfmt.make_hid(buttons=1 if fr == (13 if mode == "hid_diff" else 12) else 0))
            if fr == 16 and mode == "ir_ok":
                w.ir(fr, tick(fr), b"\0" * 32)
    if mode == "bad_hid":
        open(opt("--capture-hid"), "wb").write(b"junk")
for flag, kind in (("--capture-dsp", "dsp"), ("--capture-gpu", "gpu")):
    if flag in argv:
        start, count, path = opt(flag).split(":", 2)
        start, count = int(start), int(count)
        if kind == "dsp":
            with capfmt.Writer(path, "dsp") as w:
                for i in range(count):
                    r = capfmt.make_region(i, {0: capfmt.make_status(enabled=1)} if mode == "dsp_diff" else None)
                    if mode in ("dsp_stream", "dsp_far"):
                        # one audio stream; another timing model has it 7 DSP frames later (its counter at stream
                        # time t is t + 7), a buffer id that advances every 4 frames; "dsp_far": 205 frames later.
                        # As on the console the two regions swap with the counter (the region of its parity is
                        # the one the DSP reads), so an odd shift swaps them
                        t = (400 if mode == "dsp_far" else 100) + i - ((205 if mode == "dsp_far" else 7) if jit else 0)
                        def blk(t):
                            return capfmt.make_status(enabled=1, sync_count=1 + t // 4, buffer_position=(t % 4) * 160,
                                                      current_buffer_id=1 + t // 4, last_buffer_id=t // 4)
                        now = (100 + i) & 1
                        regions = [None, None]
                        regions[now] = capfmt.make_region(100 + i, {0: blk(t)})
                        regions[1 - now] = capfmt.make_region(99 + i, {0: blk(t - 1)})
                        w.dsp_regions(start + i, i, regions[0], regions[1])
                    else:
                        w.dsp_regions(start + i, i, r, r)
                    sound = [] if mode == "silent_dsp" else [(i + 1, 0)] * 160
                    w.dsp_pcm(start + i, i, capfmt.make_pcm(sound))
                    if i == 0 and mode not in ("silent_dsp", "no_samples"):
                        w.dsp_samples(start, i, 0x1000, b"\x01\x02\x03\x04")
        else:
            if mode == "bad_gpu":
                open(path, "wb").write(b"junk")
            else:
                with open(path, "wb") as fh:
                    g = gcap.Writer(fh)
                    g.init()
                    for i in range(count):
                        g.record(gcap.FRAME, struct.pack("<I", start + i + (1 if mode == "gpu_diff" else 0)),
                                 frame=start + i)
                        if mode in ("gas", "fog_only") and i == 0:      # a command list that sets the fog mode
                            g.submit_cmdlist(0x1000, 0x40, frame=start + i)
                            g.pica_reg([(gcap.REG_FOG_MODE, 1, 7 if mode == "gas" else 5)], frame=start + i)
            os.makedirs(path + ".frames", exist_ok=True)
            open(os.path.join(path + ".frames", "%d_top.ppm" % start), "w").write("P3 1 1 255 0 0 0\n")
if opt("--trace-timing"):
    t = [head("timingtrace"), "tick=0 event=SharedPage::UpdateTimeCallback core=0 late=0 from=emu"]
    if mode == "timing_diff":
        t.append("tick=5 event=ThreadWakeupCallback_0 core=0 late=0 from=emu")
    open(opt("--trace-timing"), "w").write("\n".join(t) + "\n")
if opt("--count-unmapped"):
    open(opt("--count-unmapped"), "w").write(
        head("unmapped") + "\ncount=%d addr=00000000 size=4 rw=read lr=00000000\n" % (2 if mode == "unmapped_diff" else 1))
if opt("--shots"):
    os.makedirs(opt("--shots"), exist_ok=True)
    # P6 files of one pixel: "pixels" is 1/255 off, "pixels_far" 200/255; FAKE_PIXELS="frame:value,..." sets
    # single frames; FAKE_SKIP_SHOT="frame_screen,..." leaves screenshots out; FAKE_GAS="frame,..." writes gas.txt
    value = {"pixels": 1, "pixels_far": 200}.get(mode, 0)
    per_frame = dict((int(x.split(":")[0]), int(x.split(":")[1])) for x in os.environ.get("FAKE_PIXELS", "").split(",") if x)
    skip = os.environ.get("FAKE_SKIP_SHOT", "").split(",")
    first = int(opt("--shots-from", "f1")[1:])
    every = int(opt("--shot-every", "f1")[1:])
    for fr in range(first, frames + 1, every):
        for screen in ("top", "bottom"):
            if "%d_%s" % (fr, screen) in skip:
                continue
            v = per_frame.get(fr, value)
            open(os.path.join(opt("--shots"), "f%07d_%s.ppm" % (fr, screen)), "wb").write(b"P6\n1 1\n255\n" + bytes((v, v, v)))
    if "FAKE_GAS" in os.environ:
        open(os.path.join(opt("--shots"), "gas.txt"), "w").write("".join(x + "\n" for x in os.environ["FAKE_GAS"].split(",") if x))
    if "--dump" in argv:
        at, addr, size = opt("--dump").split(":")
        open(os.path.join(opt("--shots"), "mem_f%07d_%s.bin" % (int(at[1:]), addr.lower())), "w").write(
            "page2" if mode == "shared_diff" else "page")
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
# a G2-like session: three distinct scene values among its checkpoints
G2_SESSION = SESSION.replace("S1", "G2").replace("5:0, 20:2", "5:0, 20:2, 28:4")


@pytest.fixture
def env(tmp_path, monkeypatch):
    """A fake runner behind a per-OS launcher, a sessions folder, a user folder and fake game files."""
    script = tmp_path / "fake_runner.py"
    script.write_text(FAKE.replace("@TOOLS@", repr(str(ROOT / "tools" / "native"))), encoding="utf-8")
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
    # the scripts are 30 frames long here: shots and the shared page from frame 10, every 10 frames
    monkeypatch.setattr(golden, "SHOTS_FROM_FRAME", 10)
    monkeypatch.setattr(golden, "DEFAULT_SHOT_EVERY_FRAMES", 10)
    # no repository list unless a test wants one
    monkeypatch.setattr(golden, "IPC_TIME_DEPENDENT", tmp_path / "no_such_file.txt")
    # the cross profile's write masks: the fake runner's Write of 32 bytes at offset 64 (session S1), bytes 4 to 7
    masks = tmp_path / "fs_write_masks.txt"
    masks.write_text("# test masks\nS1 32 64 4 4   test: the fake clock\nS9 32 64 0 4   another session's line\n", encoding="utf-8")
    monkeypatch.setattr(golden, "FS_WRITE_MASKS", masks)
    # the fake sessions are 30 frames long, their inputs a few frames after a scene change: not D50's 60
    monkeypatch.setattr(golden, "CROSS_MIN_MARGIN", 1)

    class E:
        pass

    e = E()
    e.tmp, e.script, e.launcher, e.sessions, e.user = tmp_path, script, launcher, sessions, user
    e.game, e.update, e.lib, e.log, e.out = game, update, lib, log, tmp_path / "golden"
    e.calls = lambda: [json.loads(x) for x in log.read_text().splitlines()]
    e.common = ["--out", str(e.out), "--sessions-dir", str(sessions)]
    e.ref = ["--runner", str(launcher), "--game", str(game), "--update", str(update), "--code", str(lib)]
    e.record = lambda *extra, s="S1": golden.main(["record", s, *e.common, *e.ref, "--user", str(user), *extra])
    e.check = lambda *extra, s="S1": golden.main(["check", s, *e.common, *e.ref, *extra])
    e.manifest = lambda s="S1": json.loads((e.out / s / "manifest.json").read_text())
    e.masks = masks
    e.ignore_file = tmp_path / "ignore.txt"
    e.ignore_file.write_text("srv:USER 00020080   # test: the reply carries a time\n", encoding="utf-8")
    return e


def write_session(tmp_path, text, name="S1"):
    p = tmp_path / f"{name}.txt"
    p.write_text(text, encoding="utf-8")
    return p


def statuses(text, session="S1"):
    """{label: STATUS} from the lines `check` printed."""
    out = {}
    for line in text.splitlines():
        m = re.match(rf"^{re.escape(session)}: (.+?): (PASS|FAIL|SKIPPED|INFO)(?: |$)", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


# ---- session files

def test_parse_session(tmp_path):
    s = golden.parse_session(write_session(tmp_path, SESSION))
    assert (s.name, s.needs, s.frames, s.events, s.draft) == ("S1", "none", 30, 3, False)
    assert s.checkpoints == [(5, 0), (20, 2)]
    assert [(c.kind, c.start, c.frames, c.filename, c.silent) for c in s.captures] == [
        ("gpu", 10, 2, "gpu-10.mgpu", False), ("dsp", 10, 3, "dsp-10.mdsp", False)]


def test_silent_dsp_capture(tmp_path):
    s = golden.parse_session(write_session(tmp_path, SESSION.replace("dsp:10:3", "dsp:10:3:silent")))
    assert [(c.kind, c.silent, c.text()) for c in s.captures] == [("gpu", False, "gpu:10:2"),
                                                                  ("dsp", True, "dsp:10:3:silent")]
    with pytest.raises(golden.GoldenError, match="only a DSP capture"):
        golden.parse_session(write_session(tmp_path, SESSION.replace("gpu:10:2", "gpu:10:2:silent")))


def test_hex_scene_and_draft(tmp_path):
    text = SESSION.replace("5:0, 20:2", "").replace("# captures", "# draft: yes\n# captures")
    s = golden.parse_session(write_session(tmp_path, text))
    assert s.draft and s.checkpoints == []
    s = golden.parse_session(write_session(tmp_path, SESSION.replace("20:2", "20:0x00e64d5c")))
    assert s.checkpoints[1] == (20, 0xE64D5C)


def test_env_headers(tmp_path):
    """`# env:` lines are environment variables of the runner, any number of them, in order (G3-perf)."""
    s = golden.parse_session(write_session(tmp_path, SESSION))
    assert s.env == []
    text = SESSION.replace("# captures", "# env: MH4U_WIDESCREEN=3440:1440\n# env: MH4U_WIDE_RENDER=1\n# captures")
    s = golden.parse_session(write_session(tmp_path, text))
    assert s.env == ["MH4U_WIDESCREEN=3440:1440", "MH4U_WIDE_RENDER=1"]
    assert s.events == 3                      # an env line is a header, not an input event


@pytest.mark.parametrize("value,message", [("MH4U_WIDESCREEN", "bad env"), ("=1", "bad env"), ("1X=2", "bad env"),
                                           ("MH4U_A=1 2", "bad env"), ("MH4U_A=", "bad env"), ("", "bad env"),
                                           ("MH4U_A=1\x002", "bad env"),
                                           ("TZ=PST8PDT", "TZ is fixed"), ("tz=PST8PDT", "TZ is fixed"),
                                           ("PATH=C:/x", "only MH4U_"), ("PYTHONPATH=x", "only MH4U_"),
                                           ("LD_PRELOAD=x", "only MH4U_"), ("mh4u_fps=1", "only MH4U_")])
def test_bad_env_headers(tmp_path, value, message):
    text = SESSION.replace("# captures", f"# env: {value}\n# captures")
    with pytest.raises(golden.GoldenError, match=message):
        golden.parse_session(write_session(tmp_path, text))


def test_an_env_variable_given_twice_is_refused(tmp_path):
    text = SESSION.replace("# captures", "# env: MH4U_FPS=60\n# env: MH4U_WIDE_RENDER=1\n# env: MH4U_FPS=120\n# captures")
    with pytest.raises(golden.GoldenError, match="given twice"):
        golden.parse_session(write_session(tmp_path, text))


INIT_VALID = [("1767268800", 1767268800000000), ("1767268800.5", 1767268800500000),
              ("1767268800.500000", 1767268800500000), ("1767268800.000001", 1767268800000001),
              ("946684800", 946684800000000), ("4102444799.999999", 4102444799999999),
              ("0946684800", 946684800000000)]
INIT_INVALID = ["", "abc", "yesterday", "-1", "+1767268800", "1767268800.", ".5", "1767268800.1234567",
                "1767268800.5.5", "1767268800,5", " 1767268800", "1767268800 ", "1767268800\n", "1e9", "0x6553F100",
                "1767268800.5x", "946684799.999999", "946684799", "4102444800", "99999999999", "\u0661\u0667\u0666\u0667\u0032\u0036\u0038\u0038\u0030\u0030"]


@pytest.mark.parametrize("text,micros", INIT_VALID)
def test_parse_init_time(text, micros):
    canonical, got = golden.parse_init_time(text)
    assert got == micros
    assert canonical == golden.parse_init_time(canonical)[0]


@pytest.mark.parametrize("text,canonical", [("0946684800", "946684800"), ("1767268800.500000", "1767268800.5"),
                                            ("1767268800.000000", "1767268800"), ("1767268800.000001", "1767268800.000001"),
                                            ("1767268800.050", "1767268800.05"), ("4102444799.999999", "4102444799.999999")])
def test_init_time_canonical_form(text, canonical):
    assert golden.parse_init_time(text)[0] == canonical


@pytest.mark.parametrize("text", INIT_INVALID)
def test_parse_init_time_refuses(text):
    with pytest.raises(golden.GoldenError):
        golden.parse_init_time(text)


def with_init_time(value):
    return SESSION.replace("# captures", f"# init-time: {value}\n# captures")


def test_init_time_header(tmp_path):
    assert golden.parse_session(write_session(tmp_path, SESSION)).init_time is None
    s = golden.parse_session(write_session(tmp_path, with_init_time("0946684800.500000")))
    assert s.init_time == "946684800.5" and s.events == 3          # canonical; a header, not an input event
    s = golden.parse_session(write_session(tmp_path, with_init_time("1767268800.000000")))
    assert s.init_time == "1767268800"


@pytest.mark.parametrize("value", ["", "abc", "-1", "1767268800.", ".5", "1e9", "946684799", "4102444800",
                                   "1767268800.1234567"])
def test_bad_init_time_headers(tmp_path, value):
    with pytest.raises(golden.GoldenError, match=r"S1.txt:\d+: .*init time"):
        golden.parse_session(write_session(tmp_path, with_init_time(value)))


def test_init_time_given_twice_is_refused(tmp_path):
    text = SESSION.replace("# captures", "# init-time: 1767268800.5\n# init-time: 1767268800.5\n# captures")
    with pytest.raises(golden.GoldenError, match="'init-time' given twice"):
        golden.parse_session(write_session(tmp_path, text))


def init_values(argv):
    return [argv[i + 1] for i, t in enumerate(argv) if t == "--init-time"]


def test_no_header_no_init_time_anywhere(env):
    assert env.record() == 0
    assert env.check("--repeat", "2") == 0
    env.check("--candidate-args", "--cpu jit", "--profile", "cross")       # only the command lines matter here
    env.check("--candidate-runner", str(env.launcher), "--candidate-program", str(env.game))
    assert all("--init-time" not in c["argv"] for c in env.calls())
    assert env.manifest()["session"]["init_time"] is None


def test_init_time_reaches_every_runner(env):
    (env.sessions / "S1.txt").write_text(with_init_time("1767268800.500000"), encoding="utf-8")
    assert env.record() == 0
    assert env.manifest()["session"]["init_time"] == "1767268800.5"
    assert env.check("--repeat", "2") == 0
    env.check("--candidate-args", "--cpu jit", "--profile", "cross")       # only the command lines matter here
    env.check("--candidate-runner", str(env.launcher), "--candidate-program", str(env.game))
    calls = env.calls()
    assert len(calls) == 1 + 2 + 1 + 1
    assert [init_values(c["argv"]) for c in calls] == [["1767268800.5"]] * len(calls)


def test_equal_candidate_init_time_is_harmless(env):
    (env.sessions / "S1.txt").write_text(with_init_time("1767268800.5"), encoding="utf-8")
    assert env.record() == 0
    env.check("--candidate-args", "--cpu jit --init-time 1767268800.500000", "--profile", "cross")
    assert init_values(env.calls()[-1]["argv"]) == ["1767268800.5"]       # given once


def test_candidate_init_time_conflict(env, capsys):
    (env.sessions / "S1.txt").write_text(with_init_time("1767268800.5"), encoding="utf-8")
    assert env.record() == 0
    runs = len(env.calls())
    for bad in ("--init-time 1767268800", "--init-time=1767268801.5", "--init-time nonsense"):
        assert env.check(f"--candidate-args={bad}") == 2
        assert "--candidate-args" in capsys.readouterr().err
    assert env.check("--candidate-args", "--init-time ") == 2                  # no value
    assert env.check("--runner-args", "--init-time 1767268800") == 2          # the session decides, whoever's option
    assert len(env.calls()) == runs                                       # nothing ran


def test_record_runner_args_init_time(env, capsys):
    (env.sessions / "S1.txt").write_text(with_init_time("1767268800.5"), encoding="utf-8")
    assert env.record("--runner-args=--init-time 1767268801") == 2
    assert "--runner-args" in capsys.readouterr().err
    assert not env.log.exists()                                          # no runner was started
    assert env.record("--force", "--runner-args=--init-time 1767268800.500000") == 0
    assert [init_values(c["argv"]) for c in env.calls()] == [["1767268800.5"]]


def test_candidate_init_time_untouched_without_header(env):
    assert env.record() == 0
    env.check("--candidate-args", "--init-time 1767268801 --cpu jit", "--profile", "cross")
    assert init_values(env.calls()[-1]["argv"]) == ["1767268801"]


def test_init_time_read_back_mismatch(env, capsys):
    path = env.sessions / "S1.txt"
    # header edited after record
    path.write_text(with_init_time("1767268800.5"), encoding="utf-8")
    assert env.record() == 0
    path.write_text(with_init_time("1767268800.25"), encoding="utf-8")
    assert env.check() == 2
    err = capsys.readouterr().err
    assert "1767268800.25" in err and "1767268800.5" in err
    # header added after a recording without one
    path.write_text(SESSION, encoding="utf-8")
    assert env.record("--force") == 0
    path.write_text(with_init_time("1767268800.5"), encoding="utf-8")
    assert env.check() == 2
    err = capsys.readouterr().err
    assert "1767268800.5" in err and "none" in err
    # header removed after a recording with one
    path.write_text(with_init_time("1767268800.5"), encoding="utf-8")
    assert env.record("--force") == 0
    path.write_text(SESSION, encoding="utf-8")
    assert env.check() == 2
    err = capsys.readouterr().err
    assert "none" in err and "1767268800.5" in err
    # another spelling of the same date is the same date
    path.write_text(with_init_time("1767268800.500000"), encoding="utf-8")
    assert env.check() == 0


def test_manifest_without_the_init_time_key(env):
    assert env.record() == 0
    m = env.manifest()
    del m["session"]["init_time"]                      # a manifest from before N0-35
    (env.out / "S1" / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
    assert env.check() == 0


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
    ("dsp:10:3", "dsp:10:3:loud", "bad capture"),
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


A_DEMO, A_NEWGAME, A_VILLAGE, A_QUEST = 0x00E64D5C, 0x00E6C138, 0x00E6C170, 0x00E655C8
A_CONTINUE, A_ARENA = 0x00E6C778, 0x00E650BC       # N0-10: the scenes the sessions with a save pass through
NO_SAVE = ["G1", "G2", "G6"]
WITH_SAVE = ["G3", "G3-perf", "G4", "G8", "G9a", "G9b", "G9c"]
DRAFTS = ["G7"]


def test_shipped_sessions_parse():
    names = sorted(p.stem for p in golden.SESSIONS_DIR.glob("*.txt"))
    assert names == sorted(NO_SAVE + WITH_SAVE + DRAFTS)
    sessions = {n: golden.parse_session(golden.SESSIONS_DIR / f"{n}.txt") for n in names}
    assert all(not sessions[n].draft for n in NO_SAVE + WITH_SAVE)
    assert all(sessions[n].draft for n in DRAFTS)           # N0-10: G7 could not be made (golden.md, G7)
    assert all(sessions[n].needs == "none" for n in NO_SAVE)
    assert all(sessions[n].needs == "save" for n in WITH_SAVE + DRAFTS)
    g1, g2, g6 = sessions["G1"], sessions["G2"], sessions["G6"]
    assert [(c.kind, c.start, c.frames, c.silent) for c in g1.captures] == [("gpu", 1500, 3, False),
                                                                           ("dsp", 1240, 300, False)]
    assert [(c.kind, c.start, c.frames) for c in g2.captures] == [("gpu", 2500, 3)]
    assert g6.captures == []
    # G1: the title, aDemo, is the only scene that lasts more than 100 frames before the title
    assert {scene for _, scene in g1.checkpoints} == {A_DEMO}
    # G2's checkpoints hold at least 3 distinct scene values (and a checkpoint in each long scene)
    assert {scene for _, scene in g2.checkpoints} == {A_DEMO, A_NEWGAME, A_VILLAGE, A_QUEST}
    assert {scene for _, scene in g6.checkpoints} == {A_DEMO, A_NEWGAME}


def test_shipped_g1_dsp_capture_is_inside_the_last_audio_stretch():
    # the audio of G1 is in game frames 300 to 1350, its last stretch 1234 to 1350; 300 DSP frames are about
    # 90 game frames
    g1 = golden.parse_session(golden.SESSIONS_DIR / "G1.txt")
    dsp = next(c for c in g1.captures if c.kind == "dsp")
    assert dsp.start >= 1234 and dsp.start + dsp.frames * 90 // 300 + 1 <= 1350


def test_shipped_sessions_event_counts():
    # golden.py's own parser: the number of input events of each shipped session
    counts = {n: golden.parse_session(golden.SESSIONS_DIR / f"{n}.txt").events
              for n in NO_SAVE + WITH_SAVE + DRAFTS}
    assert counts == {"G1": 8, "G2": 52, "G6": 23, "G3": 111, "G3-perf": 111, "G4": 9, "G7": 72,
                      "G8": 9, "G9a": 49, "G9b": 75, "G9c": 78}


def events_of(name):
    """The input events of a shipped session, as written (`f<frame>:KIND...`), in order."""
    out = []
    for line in (golden.SESSIONS_DIR / f"{name}.txt").read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        out += [x.strip() for x in line.split(",") if x.strip()]
    return out


def test_shipped_g3_is_the_arena_hunt_with_a_save():
    g3 = golden.parse_session(golden.SESSIONS_DIR / "G3.txt")
    assert g3.frames == 18000 and g3.env == []
    # N0-10: a GPU capture of 10 frames in the fight (the Yian Kut-Ku is on screen from about f16740 to f17800),
    # and 3891 frames (about 65 s) of hunt from the load at about f14109
    assert [(c.kind, c.start, c.frames) for c in g3.captures] == [("gpu", 17400, 10)]
    # a checkpoint in every scene that lasts more than 100 frames: the intro movies, the continue screen, the
    # village (the arrival scene, the talk, the hall), the Arena's equipment screen and the hunt
    assert {scene for _, scene in g3.checkpoints} == {A_DEMO, A_CONTINUE, A_VILLAGE, A_ARENA, A_QUEST}
    assert g3.checkpoints[-1] == (18000, A_QUEST) and (13700, A_ARENA) in g3.checkpoints
    events = events_of("G3")
    # the traps of the spec: B is never pressed (B right after the quest is accepted leaves it unposted), and the
    # Arena's "Use this equipment?" has No selected: LEFT, then A
    assert not [e for e in events if e.split(":")[1].upper() == "B"]
    left = events.index("f13850:LEFT:f6")
    assert events[left + 1] == "f13910:A:f6"
    # the quest counter's talk: Yes is selected at "Select this Quest?", so A posts it (f12044) and the next A closes
    # the notice (f12119); a press after that would start the talk again
    talk = [e for e in events if e.endswith(":A:f6") and 11444 <= int(e[1:].split(":")[0]) <= 12200]
    assert [int(e[1:].split(":")[0]) for e in talk][-2:] == [12044, 12119]
    # the village's talk ends with the press at f8460 and nothing presses A again until the counter's
    assert "f8460:A:f6" in events and not [e for e in events if e.endswith(":A:f6") and 8460 < int(e[1:].split(":")[0]) < 11444]
    # the save has the Circle Pad Pro option off, so the session never opens ir:USER and does not touch the options
    assert not [e for e in events if ":TOUCH:" in e and e.split(":")[0] in ("f1070", "f1250", "f1850")]


def test_shipped_g3_perf_is_g3_at_21_9():
    g3, perf = (golden.parse_session(golden.SESSIONS_DIR / f"{n}.txt") for n in ("G3", "G3-perf"))
    # the spec: G3 with the widening options and a GPU capture of 30 frames instead of 10, taken inside the fight
    # (the port reads its options once at start-up: environment variables, which the runner's --set refuses)
    assert perf.env[:2] == ["MH4U_WIDESCREEN=3440:1440", "MH4U_WIDE_RENDER=1"]
    assert perf.env[2] == "MH4U_DEPTH_OF_FIELD=off"            # wide_render crashes the reference runner without it
    assert len(perf.env) == 3
    assert [(c.kind, c.frames) for c in perf.captures] == [("gpu", 30)]
    assert 16740 <= perf.captures[0].start and perf.captures[0].start + 30 <= 17800
    # the render width does not move the frame at which anything happens: the inputs and checkpoints are G3's
    assert events_of("G3-perf") == events_of("G3")
    assert perf.frames == g3.frames and perf.checkpoints == g3.checkpoints


def test_shipped_g4_is_the_arrival_scene_with_gas():
    g4 = golden.parse_session(golden.SESSIONS_DIR / "G4.txt")
    assert g4.frames == 3000 and g4.env == []
    assert [(c.kind, c.start, c.frames) for c in g4.captures] == [("gpu", 2400, 10)]
    assert {scene for _, scene in g4.checkpoints} == {A_DEMO, A_CONTINUE, A_VILLAGE}
    # nothing after the nine boot presses: the scene plays by itself
    assert events_of("G4") == events_of("G3")[:9] and len(events_of("G4")) == 9
    # the recording must show gas (golden.MINIMUMS): a G4 whose capture lost it fails `record`
    assert golden.MINIMUMS["G4"] == {"gas_frames": 1}
    assert golden.MINIMUMS["G3"] == golden.MINIMUMS["G3-perf"] == {"checkpoint_scenes": 5}


def test_shipped_g8_is_g4s_boot_from_the_save_as_it_is():
    """N0-35 Part B, D36/D80: G8 is the session in which the game opens `ir:USER` (the maintainer's save with the
    Circle Pad Pro option on). It is the nine boot presses of G4 and the same start date; it needs no capture."""
    g8 = golden.parse_session(golden.SESSIONS_DIR / "G8.txt")
    assert not g8.draft and g8.needs == "save" and g8.env == []
    assert g8.init_time == "1767268800.5" == golden.parse_session(golden.SESSIONS_DIR / "G4.txt").init_time
    assert g8.captures == [] and 3000 <= g8.frames <= 3500
    # the intro movies, the continue screen and the village: the scenes that last more than 100 frames
    assert {scene for _, scene in g8.checkpoints} == {A_DEMO, A_CONTINUE, A_VILLAGE}
    assert events_of("G8") == events_of("G4") and len(events_of("G8")) == 9
    # the recording's own rule (D36) is in `_ir_problems`: no gas, no scene minimum for this session
    assert "G8" not in golden.MINIMUMS


def test_shipped_g9_are_g3s_route_cut_for_a_census_capture():
    """N0-35 (D90): G9a, G9b and G9c are G3's route cut just after a 3-frame GPU capture each (the reference runner has
    one --capture-gpu). Their events, start date and checkpoints are G3's own up to the cut."""
    g3 = golden.parse_session(golden.SESSIONS_DIR / "G3.txt")
    g3_events = events_of("G3")
    want = {"G9a": (8003, 8000, {A_DEMO, A_CONTINUE, A_VILLAGE}),
            "G9b": (13503, 13500, {A_DEMO, A_CONTINUE, A_VILLAGE, A_ARENA}),
            "G9c": (14153, 14150, {A_DEMO, A_CONTINUE, A_VILLAGE, A_ARENA, A_QUEST})}
    for name, (frames, start, scenes) in want.items():
        s = golden.parse_session(golden.SESSIONS_DIR / f"{name}.txt")
        assert not s.draft and s.needs == "save" and s.env == [] and s.init_time == g3.init_time
        assert s.frames == frames and [(c.kind, c.start, c.frames) for c in s.captures] == [("gpu", start, 3)]
        assert {scene for _, scene in s.checkpoints} == scenes
        # the events are G3's up to the cut, none after it
        cut = [e for e in g3_events if int(e[1:].split(":")[0]) <= frames]
        assert events_of(name) == cut
        # a checkpoint of G3's own where G3 has one at that frame
        assert all(c in g3.checkpoints for c in s.checkpoints if c[0] in dict(g3.checkpoints))
        assert name not in golden.MINIMUMS


def test_shipped_g7_is_a_draft_that_stops_at_the_empty_board():
    """G7 (a downloaded quest) could not be made: the board that lists them says it has none (golden.md). The draft
    keeps the route that gets there, so that the next attempt starts at the board; `record` refuses it."""
    g7 = golden.parse_session(golden.SESSIONS_DIR / "G7.txt")
    assert g7.draft and g7.needs == "save"
    events = events_of("G7")
    # the same village as G3's up to the counter, then the Event Quests touch of the quest board
    assert events[:61] == events_of("G3")[:61]            # the boot, the talk, the map and the walk (to f10894)
    assert "f12700:TOUCH:122:190:f6" in events
    assert not [e for e in events if e.split(":")[1].upper() == "B"]


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="reference runner not set (MH4U_RUNNER)")
@pytest.mark.parametrize("name", ["G1", "G2", "G6"])
def test_reference_runner_accepts_the_shipped_sessions(name):
    import subprocess
    done = subprocess.run([os.environ["MH4U_RUNNER"], "--input-file", str(golden.SESSIONS_DIR / f"{name}.txt"),
                           "--print-schedule"], capture_output=True, timeout=120, check=False)
    assert done.returncode == 0, done.stderr.decode("utf-8", "replace")[-300:]


# ---- record

def test_record(env, capsys):
    assert env.record() == 0
    out = env.out / "S1"
    for name in ("svc.txt", "ipc.txt", "state.txt", "unmapped.txt", "timing.txt", "hid.mhid", "hid.mhid.json",
                 "dsp-10.mdsp", "gpu-10.mgpu", "manifest.json", "runner.log"):
        assert (out / name).is_file(), name
    assert (out / "shots" / "f0000010_top.ppm").is_file() and (out / "shots" / "f0000030_bottom.ppm").is_file()
    assert (out / "shots" / "mem_f0000010_1ff81000.bin").is_file()
    assert (out / "gpu-10.mgpu.frames" / "10_top.ppm").is_file()
    assert (out / "user-before" / "sdmc" / "a.txt").is_file()
    assert not (out / "user-before" / "ran.txt").exists()          # the run used a working copy
    assert (out / "user-after" / "ran.txt").read_text() == "run\n"   # and left it as user-after/
    assert not (out / "work").exists()
    m = env.manifest()
    assert m["tz"] == "UTC0" and m["runner"]["exit"] == 0 and m["runner"]["new_3ds"] == "off"
    assert m["runner"]["cpu"] == "recomp" and m["code_library"]["sha256"] == golden.sha256_file(env.lib)
    assert [c["result"] for c in m["checkpoint_results"]] == ["ok", "ok"]
    assert m["capture_problems"] == [] and "manifest.json" not in m["files"]
    assert m["files"]["svc.txt"]["sha256"] == golden.sha256_file(out / "svc.txt")
    assert not any(k.startswith(("user-before", "user-after")) for k in m["files"])
    assert m["real_scene_changes"] == 2              # 0 to 2 at frame 10, 2 to 4 at frame 25
    assert m["shots"] == {"from_frame": 10, "every_frames": 10}
    assert m["user_config_savegame"] is False
    assert "no config savegame" in capsys.readouterr().out
    call = env.calls()[0]
    assert call["tz"] == "UTC0"
    a = call["argv"]
    assert a[0] == str(env.game)
    for pair in (["--new-3ds", "off"], ["--cpu", "recomp"], ["--code", str(env.lib)], ["--install", str(env.update)],
                 ["--frames", "30"], ["--input-file", str(env.sessions / "S1.txt")],
                 ["--observable", str(golden.OBSERVABLE)], ["--trace-timing", str(out / "timing.txt")],
                 ["--shots", str(out / "shots")], ["--shots-from", "f10"], ["--shot-every", "f10"],
                 ["--dump", "f10:1FF81000:0x1000"]):
        assert a[a.index(pair[0]) + 1] == pair[1], pair
    assert a[a.index("--user") + 1] == str(out / "work")
    assert str(out / "gpu-10.mgpu") in "".join(a) and "--trace-ipc-full" in a and "--count-unmapped" in a
    assert f"10:3:{out / 'dsp-10.mdsp'}" in a


def test_record_notes_the_config_savegame(env, capsys):
    config = env.user / "nand" / "data" / "00000000000000000000000000000000" / "sysdata" / "00010017" / "00000000"
    config.mkdir(parents=True)
    (config / "config").write_bytes(b"cfg")
    assert env.record() == 0
    assert env.manifest()["user_config_savegame"] is True
    assert "no config savegame" not in capsys.readouterr().out


def test_record_without_screenshots(env, capsys):
    assert env.record("--shot-every-frames", "0") == 0
    a = env.calls()[0]["argv"]
    assert not {"--shots", "--shots-from", "--shot-every", "--dump"} & set(a)
    assert not (env.out / "S1" / "shots").exists()
    assert env.manifest()["shots"]["every_frames"] == 0
    capsys.readouterr()
    assert env.check("--require", "svc,ipc,state") == 0          # images: skipped, not required
    assert statuses(capsys.readouterr().out)["images"] == "SKIPPED"
    assert env.check("--require", "images") == 3


def test_record_refusals(env, capsys):
    assert env.record() == 0
    assert env.record() == 2 and "not empty" in capsys.readouterr().err
    assert env.record("--force") == 0
    (env.sessions / "D.txt").write_text(SESSION.replace("S1", "D").replace("5:0, 20:2", "")
                                        .replace("# captures", "# draft: yes\n# captures"))
    assert golden.main(["record", "D", *env.common, *env.ref, "--user", str(env.user)]) == 2
    assert "draft" in capsys.readouterr().err
    runs = len(env.calls())
    assert not (env.out / "D").exists()                       # the refusal ran nothing and made nothing
    assert golden.main(["record", "D", *env.common, *env.ref, "--user", str(env.user), "--allow-draft"]) == 0
    assert len(env.calls()) == runs + 1                       # --allow-draft: the draft is recorded
    assert env.manifest("D")["session"]["checkpoints"] == []
    (env.sessions / "N.txt").write_text(SESSION.replace("S1", "N").replace("none", "save"))
    assert golden.main(["record", "N", *env.common, *env.ref, "--user", str(env.user)]) == 2
    assert "needs a save" in capsys.readouterr().err
    assert golden.main(["record", "S1", *env.common, "--runner", str(env.launcher), "--game", str(env.game),
                        "--user", str(env.user), "--force"]) == 2        # recomp with no --code
    assert "needs --code" in capsys.readouterr().err
    assert env.record("--force", "--shot-every-frames", "-1") == 2


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
    assert [c["result"] for c in env.manifest()["checkpoint_results"]] == ["ok", "mismatch"]


# ---- the capture problems that make record exit 1

def problems(env):
    return env.manifest()["capture_problems"]


def test_a_gpu_capture_the_checker_rejects(env, monkeypatch, capsys):
    monkeypatch.setenv("FAKE_MODE", "bad_gpu")
    assert env.record() == 1
    assert any(p.startswith("gpu-10.mgpu:") for p in problems(env))
    assert "capture: gpu-10.mgpu" in capsys.readouterr().out


def test_a_hid_capture_the_reader_rejects(env, monkeypatch, capsys):
    monkeypatch.setenv("FAKE_MODE", "bad_hid")
    assert env.record() == 1
    assert [p for p in problems(env) if p.startswith("hid.mhid:")]


def test_a_dsp_capture_without_sound_or_samples(env, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "silent_dsp")
    assert env.record() == 1
    text = "\n".join(problems(env))
    assert "no non-silent PCM frame" in text and "no type-3" in text
    monkeypatch.setenv("FAKE_MODE", "no_samples")
    assert env.record("--force") == 1
    text = "\n".join(problems(env))
    assert "no type-3" in text and "no non-silent PCM frame" not in text


def test_a_silent_dsp_capture_is_fine_when_marked(env, monkeypatch):
    (env.sessions / "S1.txt").write_text(SESSION.replace("dsp:10:3", "dsp:10:3:silent"))
    monkeypatch.setenv("FAKE_MODE", "silent_dsp")
    assert env.record() == 0 and problems(env) == []
    monkeypatch.delenv("FAKE_MODE")      # a silent capture that has sound is not a problem either
    assert env.record("--force") == 0


def test_an_ir_request_needs_an_ir_record_in_the_hid_capture(env, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "ir_missing")
    assert env.record() == 1
    assert any("ir:USER" in p and "type-2" in p for p in problems(env))
    monkeypatch.setenv("FAKE_MODE", "ir_ok")       # an IR record at frame 16, the request at frame 15
    assert env.record("--force") == 0 and problems(env) == []
    monkeypatch.delenv("FAKE_MODE")                # no ir:USER request at all: nothing to ask of the HID capture
    assert env.record("--force") == 0


def test_a_g2_recording_needs_two_real_scene_changes_and_three_scenes(env, monkeypatch):
    (env.sessions / "G2.txt").write_text(G2_SESSION)
    assert env.record(s="G2") == 0
    assert env.manifest("G2")["real_scene_changes"] == 2
    monkeypatch.setenv("FAKE_MODE", "one_change")
    assert env.record("--force", s="G2") == 1
    text = "\n".join(env.manifest("G2")["capture_problems"])
    assert "1 real scene change" in text and "needs at least 2" in text
    monkeypatch.delenv("FAKE_MODE")
    (env.sessions / "G2.txt").write_text(G2_SESSION.replace("5:0, 20:2, 28:4", "5:0, 20:2, 21:2"))
    assert env.record("--force", s="G2") == 1
    assert env.manifest("G2")["capture_problems"] == [
        "G2's checkpoints hold 2 distinct scene value(s), at least 3 wanted"]
    # other sessions have no such minimum
    monkeypatch.setenv("FAKE_MODE", "one_change")
    (env.sessions / "S1.txt").write_text(SESSION.replace("5:0, 20:2", "5:0, 20:2, 28:2"))
    assert env.record() == 0


def test_a_session_with_env_headers_runs_with_them_every_time(env, capsys, monkeypatch):
    """N0-10, G3-perf: the start-up options of the session are part of it: `record` and `check` (the reference
    again, and a candidate runner) give the runner its environment variables, whatever the shell has set."""
    wide = {"MH4U_WIDESCREEN": "3440:1440", "MH4U_WIDE_RENDER": "1"}
    (env.sessions / "S1.txt").write_text(SESSION.replace("# captures", "# env: MH4U_WIDESCREEN=3440:1440\n"
                                                         "# env: MH4U_WIDE_RENDER=1\n# captures"))
    monkeypatch.setenv("MH4U_WIDESCREEN", "16:9")            # the session's value wins over the shell's
    monkeypatch.setenv("MH4U_FPS", "120")                     # and the rest of the environment is kept
    assert env.record() == 0
    assert env.calls()[0]["mh4u"] == {**wide, "MH4U_FPS": "120"} and env.calls()[0]["tz"] == "UTC0"
    assert env.manifest()["session"]["env"] == ["MH4U_WIDESCREEN=3440:1440", "MH4U_WIDE_RENDER=1"]
    capsys.readouterr()
    assert env.check() == 0                          # the reference runner again: nothing on the command line
    assert env.calls()[1]["mh4u"] == {**wide, "MH4U_FPS": "120"}
    prog = env.tmp / "prog"
    prog.mkdir()
    env.check("--candidate-runner", str(env.script), "--candidate-program", str(prog))
    assert env.calls()[2]["mh4u"] == {**wide, "MH4U_FPS": "120"}
    # a session without the headers passes none of its own
    (env.sessions / "S1.txt").write_text(SESSION)
    assert env.record("--force") == 0
    assert env.calls()[3]["mh4u"] == {"MH4U_WIDESCREEN": "16:9", "MH4U_FPS": "120"}
    assert env.manifest()["session"]["env"] == []


def test_a_check_refuses_a_session_whose_env_lines_changed_since_the_recording(env, capsys):
    """The environment of a run is part of the recording: a session file edited after `record` is not checked
    under other variables without a word (and an old manifest without the key was made with none)."""
    with_env = SESSION.replace("# captures", "# env: MH4U_WIDE_RENDER=1\n# captures")
    (env.sessions / "S1.txt").write_text(with_env)
    assert env.record() == 0
    assert env.check() == 0                                                     # unchanged: fine
    capsys.readouterr()
    runs = len(env.calls())
    for other in (SESSION, with_env.replace("RENDER=1", "RENDER=0"),
                  with_env.replace("# captures", "# env: MH4U_FPS=60\n# captures")):
        (env.sessions / "S1.txt").write_text(other)
        assert env.check() == 2
        assert "differ from the ones the recording was made with" in capsys.readouterr().err
    assert len(env.calls()) == runs                           # refused before any run
    # the other way round: a recording without the lines, a session that gained one
    (env.sessions / "S1.txt").write_text(SESSION)
    assert env.record("--force") == 0
    (env.sessions / "S1.txt").write_text(with_env)
    assert env.check() == 2 and "differ" in capsys.readouterr().err
    # a manifest from before the key existed has no variables
    manifest = json.loads((env.out / "S1" / "manifest.json").read_text())
    del manifest["session"]["env"]
    (env.out / "S1" / "manifest.json").write_text(json.dumps(manifest))
    (env.sessions / "S1.txt").write_text(SESSION)
    assert env.check() == 0
    (env.sessions / "S1.txt").write_text(with_env)
    assert env.check() == 2


@pytest.mark.parametrize("name", ["G3", "G3-perf"])
def test_g3_needs_a_checkpoint_in_each_of_five_scenes(env, name, monkeypatch):
    """N0-10: the Arena hunt passes through the intro, the continue screen, the village, the Arena's equipment
    screen and the hunt, and a checkpoint is wanted in each: five distinct scene values pass, four do not."""
    five = SESSION.replace("S1", name).replace("5:0, 20:2", "5:0, 11:2, 16:4, 21:6, 26:8")
    monkeypatch.setenv("FAKE_SCENES", "10:2,15:4,20:6,25:8")      # the fake runner walks through five scene values
    (env.sessions / f"{name}.txt").write_text(five)
    assert env.record(s=name) == 0 and env.manifest(name)["capture_problems"] == []
    assert len({c["scene"] for c in env.manifest(name)["checkpoint_results"]}) == 5
    (env.sessions / f"{name}.txt").write_text(five.replace("11:2, 16:4, 21:6, 26:8", "11:2, 16:4, 21:6, 26:6"))
    assert env.record("--force", s=name) == 1
    assert env.manifest(name)["capture_problems"] == [
        f"{name}'s checkpoints hold 4 distinct scene value(s), at least 5 wanted"]
    monkeypatch.delenv("FAKE_SCENES")
    (env.sessions / f"{name}.txt").write_text(SESSION.replace("S1", name))
    assert env.record("--force", s=name) == 1
    assert env.manifest(name)["capture_problems"] == [
        f"{name}'s checkpoints hold 2 distinct scene value(s), at least 5 wanted"]
    # the same session under another name has no such minimum
    assert env.record() == 0


def test_a_recording_that_opens_ir_user_still_needs_its_type_2_records(env, monkeypatch):
    """D36 is a rule of every session: a game that opens ir:USER (a save with the Circle Pad Pro on, and a
    reference runner whose HID capture lets it) must show it in the HID capture; the sessions of N0-10 are recorded
    with the option off (golden.md, `ir:USER`), so none of them meets the rule, and a recording that does open it
    without a type-2 record is a capture problem whatever its name."""
    for name in ("G3", "G4", "S1"):
        (env.sessions / f"{name}.txt").write_text(SESSION.replace("S1", name))
    monkeypatch.setenv("FAKE_MODE", "ir_missing")
    for name in ("S1", "G3", "G4"):
        assert env.record("--force", s=name) == 1
        assert any("ir:USER" in p and "type-2" in p for p in env.manifest(name)["capture_problems"])


def test_g4_needs_a_gpu_capture_that_sets_fog_mode_7(env, monkeypatch):
    """N0-10, G4: a cutscene whose capture `gcap.py summary` shows with fog mode 7 (gas)."""
    (env.sessions / "G4.txt").write_text(SESSION.replace("S1", "G4"))
    assert env.record(s="G4") == 1
    assert any("never set fog mode 7" in p for p in env.manifest("G4")["capture_problems"])
    monkeypatch.setenv("FAKE_MODE", "fog_only")           # plain fog (mode 5) is not gas
    assert env.record("--force", s="G4") == 1
    monkeypatch.setenv("FAKE_MODE", "gas")
    assert env.record("--force", s="G4") == 0 and env.manifest("G4")["capture_problems"] == []
    # a G4 without any GPU capture cannot show gas either
    monkeypatch.delenv("FAKE_MODE")
    (env.sessions / "G4.txt").write_text(SESSION.replace("S1", "G4").replace("gpu:10:2, ", ""))
    assert env.record("--force", s="G4") == 1
    assert any("never set fog mode 7" in p for p in env.manifest("G4")["capture_problems"])


# ---- check, the same profile

def test_check_same_runner_passes_exactly(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--require", "svc,ipc,state,checkpoints,hid,images,unmapped,dsp") == 0
    text = capsys.readouterr().out
    got = statuses(text)
    for name in ("svc", "ipc", "state", "checkpoints", "unmapped", "hid", "dsp-10", "gpu-10", "images",
                 "shared-page"):
        assert got[name] == "PASS", name
    assert got["timing"] == "INFO"
    assert "0 failed, 0 skipped, 1 info (profile same)" in text
    calls = env.calls()
    assert len(calls) == 2 and calls[1]["tz"] == "UTC0"
    a = calls[1]["argv"]
    # the recording's whole argument list: every recorder, the install, the code library
    for flag in ("--capture-gpu", "--new-3ds", "--install", "--trace-timing", "--count-unmapped", "--shots",
                 "--dump", "--observable", "--capture-dsp", "--capture-hid", "--trace-ipc-full"):
        assert flag in a, flag
    assert a[a.index("--cpu") + 1] == "recomp" and a[a.index("--code") + 1] == str(env.lib)
    # it ran on a copy of user-before/, so the recording is untouched
    assert not (env.out / "S1" / "user-before" / "ran.txt").exists()
    assert (env.out / "S1" / "check" / "result.json").is_file()


@pytest.mark.parametrize("mode,failing", [
    ("svc_diff", "svc"), ("vary_state", "state"), ("pixels", "images"), ("very_late", "state"),
    ("shift1", "state"),                      # one frame is a difference between two runs of one backend
    ("ipc_diff", "ipc"), ("dsp_diff", "dsp-10"), ("hid_diff", "hid"), ("gpu_diff", "gpu-10"),
    ("unmapped_diff", "unmapped"), ("shared_diff", "shared-page"),
])
def test_same_profile_detects_every_difference(env, capsys, monkeypatch, mode, failing):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", mode)
    capsys.readouterr()
    assert env.check() == 1
    got = statuses(capsys.readouterr().out)
    assert got[failing] == "FAIL"
    assert all(v != "FAIL" for k, v in got.items() if k not in (failing, "checkpoints", "state", "ir"))


def test_same_profile_never_uses_the_time_dependent_list(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "ipc_diff")
    monkeypatch.setattr(golden, "IPC_TIME_DEPENDENT", env.ignore_file)      # the list would hide the difference
    capsys.readouterr()
    assert env.check() == 1 and statuses(capsys.readouterr().out)["ipc"] == "FAIL"


def test_two_traces_that_differ_only_in_a_thread_line_fail_the_same_profile(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "thread_line")
    capsys.readouterr()
    assert env.check() == 1
    got = statuses(capsys.readouterr().out)
    assert got["svc"] == "FAIL" and got["ipc"] == "FAIL" and got["state"] == "PASS"


def test_the_timing_trace_is_information_only(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "timing_diff")
    capsys.readouterr()
    assert env.check() == 0
    text = capsys.readouterr().out
    assert statuses(text)["timing"] == "INFO" and "differs at line" in text


def test_compare_after_header(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.write_bytes(b"# header one\nx 1\n# note\ny 2\n")
    b.write_bytes(b"# header two, longer\nx 1\n# note\ny 2\n")
    assert golden.compare_after_header(a, b, block=4)[0]
    b.write_bytes(b"# other\nx 1\n# note\ny 3\n")
    ok, detail = golden.compare_after_header(a, b, block=4)
    assert not ok and "line 3 after the header" in detail
    b.write_bytes(b"# other\nx 1\n# note\ny 2\nz 3\n")             # a longer run
    ok, detail = golden.compare_after_header(a, b, block=4)
    assert not ok and "line 4 after the header" in detail
    b.write_bytes(b"# other\n")
    assert not golden.compare_after_header(a, b)[0]


# ---- the choice of the profile

@pytest.mark.parametrize("forced,runner,args_cpu,recording,expected", [
    (None, None, None, "recomp", "same"),
    (None, None, "recomp", "recomp", "same"),          # --cpu given but not changed
    (None, None, "jit", "recomp", "cross"),
    (None, "RUN2", None, "recomp", "cross"),
    (None, "RUN2", "recomp", "recomp", "cross"),
    ("same", "RUN2", None, "recomp", "same"),          # forced
    ("cross", None, None, "recomp", "cross"),
    ("same", None, "jit", "recomp", "same"),
])
def test_resolve_profile(forced, runner, args_cpu, recording, expected):
    assert golden.resolve_profile(forced, runner, args_cpu, recording) == expected


def test_the_profile_in_use_is_printed(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    env.check()
    assert "(profile same)" in capsys.readouterr().out
    env.check("--candidate-args", "--cpu jit", "--profile", "cross", "--ignore", str(env.ignore_file))
    assert "(profile cross)" in capsys.readouterr().out


# ---- the command lines of the candidates

def test_candidate_args_only_runs_the_recording_s_arguments_again(env, capsys):
    assert env.record() == 0
    # the recording's arguments, --cpu jit, and no --code
    env.check("--candidate-args", "--cpu jit")
    a = env.calls()[1]["argv"]
    assert a.count("--cpu") == 1 and a[a.index("--cpu") + 1] == "jit" and "--code" not in a
    for flag in ("--new-3ds", "--install", "--capture-gpu", "--capture-dsp", "--count-unmapped", "--shots",
                 "--dump", "--trace-timing", "--observable", "--trace-ipc-full"):
        assert flag in a, flag
    assert a[0] == str(env.game) and a[a.index("--install") + 1] == str(env.update)
    # --cpu recomp: with --code
    env.check("--candidate-args", "--cpu recomp")
    a = env.calls()[2]["argv"]
    assert a.count("--cpu") == 1 and a[a.index("--cpu") + 1] == "recomp" and a[a.index("--code") + 1] == str(env.lib)
    # no --cpu in the arguments: the recording's (recomp, with --code), the options at the end
    env.check("--candidate-args", "--set 5000:fps=60")
    a = env.calls()[3]["argv"]
    assert a[a.index("--cpu") + 1] == "recomp" and a[a.index("--code") + 1] == str(env.lib)
    assert a[-2:] == ["--set", "5000:fps=60"]


def test_check_without_the_update_the_recording_installed_is_a_usage_error(env, capsys):
    assert env.record() == 0
    assert golden.main(["check", "S1", *env.common, "--runner", str(env.launcher), "--game", str(env.game),
                        "--code", str(env.lib)]) == 2
    assert "pass --update" in capsys.readouterr().err
    # a candidate runner starts without the install, so it needs no update
    assert golden.main(["check", "S1", *env.common, "--runner", str(env.launcher), "--game", str(env.game),
                        "--candidate-runner", str(env.script)]) == 0


def test_candidate_args_recomp_needs_a_library(env, capsys):
    assert env.record() == 0
    assert golden.main(["check", "S1", *env.common, "--runner", str(env.launcher), "--game", str(env.game),
                        "--update", str(env.update), "--candidate-args", "--cpu recomp"]) == 2
    assert "needs --code" in capsys.readouterr().err


def test_candidate_gets_its_own_program_library_and_shared_options_only(env, capsys, monkeypatch):
    monkeypatch.setenv("FAKE_NO_SHOTS", "1")        # a candidate whose --help does not list --shots (N2b-17)
    assert env.record() == 0
    prog, lib2 = env.tmp / "prog", env.tmp / "native.so"
    prog.mkdir()
    lib2.write_bytes(b"x")
    cand_runner = env.tmp / "fake_runner.py"        # a .py runner is started with the Python interpreter
    capsys.readouterr()
    assert env.check("--candidate-runner", str(cand_runner), "--candidate-program", str(prog),
                     "--candidate-code", str(lib2), "--candidate-args=--native --extra 1",
                     "--set", "5000:fps=60") == 0
    a = env.calls()[1]["argv"]
    assert a[0] == str(prog) and a[a.index("--code") + 1] == str(lib2)
    assert a[-3:] == ["--native", "--extra", "1"]
    assert a[a.index("--set") + 1] == "5000:fps=60"
    for not_shared in ("--new-3ds", "--install", "--cpu", "--capture-gpu", "--count-unmapped", "--shots",
                       "--shot-every", "--shots-from", "--dump", "--trace-timing"):
        assert not_shared not in a
    for shared in ("--user", "--frames", "--input-file", "--trace-svc", "--trace-ipc", "--trace-ipc-full",
                   "--state-trace", "--capture-hid", "--capture-dsp", "--observable"):
        assert shared in a
    # N1-21: the native runner reads the same list of observable values as the comparison does
    assert a[a.index("--observable") + 1] == str(golden.OBSERVABLE)
    out = capsys.readouterr().out
    assert "(profile cross)" in out
    got = statuses(out)
    assert got["svc"] == "PASS" and got["state"] == "PASS" and got["hid"] == "PASS" and got["dsp-10"] == "PASS"
    assert got["images"] == "SKIPPED" and "unmapped" not in got   # no screenshots, no listed frames; unmapped: same only


def test_a_native_candidate_check_needs_neither_the_reference_runner_nor_the_game(env, capsys):
    # N1-21: the reference is the recording; only the candidate's runner and program are needed
    assert env.record() == 0
    prog = env.tmp / "prog"
    prog.mkdir()
    capsys.readouterr()
    common = ["--out", str(env.out), "--sessions-dir", str(env.sessions)]
    assert golden.main(["check", "S1", *common, "--candidate-runner", str(env.script),
                        "--candidate-program", str(prog)]) == 0
    assert env.calls()[-1]["argv"][0] == str(prog)
    # without the program there is nothing to start (usage error); without a candidate runner, the
    # reference runner and the game are still required
    assert golden.main(["check", "S1", *common, "--candidate-runner", str(env.script)]) == 2
    assert "--candidate-program" in capsys.readouterr().err
    assert golden.main(["check", "S1", *common]) == 2
    assert "--runner and --game" in capsys.readouterr().err
    # --candidate-program alone is still refused
    assert golden.main(["check", "S1", *common, "--candidate-program", str(prog)]) == 2


def test_a_candidate_program_or_library_needs_its_runner(env, capsys):
    assert env.record() == 0
    assert env.check("--candidate-program", str(env.tmp)) == 2
    assert "need --candidate-runner" in capsys.readouterr().err
    assert env.check("--candidate-code", str(env.lib)) == 2


@pytest.mark.skipif(os.name != "nt", reason="backslash paths")
def test_candidate_args_with_a_windows_path_reach_the_runner_intact(env):
    assert env.record() == 0
    path = "C:\\no\\such\\dir\\native.dll"
    assert env.check("--candidate-runner", str(env.script), f"--candidate-args=--native {path}") == 0
    assert env.calls()[1]["argv"][-2:] == ["--native", path]


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


# ---- check, the cross profile

def test_cross_profile_tolerates_a_clock_but_not_a_difference(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    # another timing model (--cpu jit): other ticks, one more polling call, a reply that carries a time
    assert env.check("--candidate-args", "--cpu jit") == 1
    got = statuses(capsys.readouterr().out)
    assert got["ipc"] == "FAIL"                                   # only the time-dependent list could hide it
    assert got["state"] == "PASS" and got["checkpoints"] == "PASS" and got["svc"] == "PASS"
    assert got["hid"] == "PASS" and got["dsp-10"] == "PASS"
    assert got["svc-full"] == "INFO" and got["images"] == "SKIPPED" and got["images-all"] == "INFO"   # no frames listed
    assert "gpu-10" not in got and "unmapped" not in got and "timing" not in got
    # --ignore gives the list
    assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file)) == 0
    # so does the repository's list
    monkeypatch.setattr(golden, "IPC_TIME_DEPENDENT", env.ignore_file)
    assert env.check("--candidate-args", "--cpu jit") == 0
    capsys.readouterr()
    # the same run is not enough for the same profile: ticks, an extra call
    assert env.check("--candidate-args", "--cpu jit", "--profile", "same") == 1
    got = statuses(capsys.readouterr().out)
    assert got["svc"] == "FAIL" and got["state"] == "FAIL"


def test_cross_profile_gates_what_a_clock_cannot_change(env, capsys, monkeypatch):
    assert env.record() == 0
    for mode, failing in (("svc_diff", "svc"), ("very_late", "state")):
        monkeypatch.setenv("FAKE_MODE", mode)
        capsys.readouterr()
        assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file)) == 1, mode
        assert statuses(capsys.readouterr().out)[failing] == "FAIL", mode
    # a scene change one frame later is inside the default tolerance (2) of the cross profile
    monkeypatch.setenv("FAKE_MODE", "shift1")
    assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file)) == 0
    # but a difference of the hid or dsp captures is not
    for mode, failing in (("hid_diff", "hid"), ("dsp_diff", "dsp-10")):
        monkeypatch.setenv("FAKE_MODE", mode)
        capsys.readouterr()
        assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file)) == 1
        assert statuses(capsys.readouterr().out)[failing] == "FAIL"


def test_cross_state_is_skipped_when_the_recording_has_no_real_scene_change(env, capsys, monkeypatch):
    (env.sessions / "S1.txt").write_text(SESSION.replace("5:0, 20:2", "5:0, 20:0"))
    monkeypatch.setenv("FAKE_MODE", "no_change")
    assert env.record() == 0
    assert env.manifest()["real_scene_changes"] == 0
    capsys.readouterr()
    assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file)) == 0
    got = statuses(capsys.readouterr().out)
    assert got["state"] == "SKIPPED" and got["checkpoints"] == "PASS"
    assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file), "--require", "state") == 3


# ---- check, the cross profile: the rules of D50, D51 and D52

def cross(env, *extra, s="S1"):
    """A check of `--cpu jit` against the recording (the recomp one) in the cross profile, with the test list."""
    return env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file), *extra, s=s)


SESSION3 = """\
# name: S3
# needs: none
# frames: 30
# checkpoints: 5:0, 20:2
# captures: dsp:10:40
f12:A
"""


def test_the_byte_limits_of_the_fs_reads_are_the_tools_defaults_in_one_place():
    # N0-37: the cross and the native profile pass these to compare_ipc_traces.py; the tool's own defaults are the same
    sys.path.insert(0, str(ROOT / "tools"))
    import compare_ipc_traces
    assert golden.CROSS_MAX_ONE_SIDED == compare_ipc_traces.DEFAULT_MAX_ONE_SIDED == 20480
    assert golden.CROSS_MAX_DETACHED == compare_ipc_traces.DEFAULT_MAX_DETACHED == 0


@pytest.mark.parametrize("profile", ["cross", "native"])
def test_the_byte_limits_stop_a_read_next_to_nothing_and_let_a_stream_go_on(env, capsys, monkeypatch, profile):
    assert env.record() == 0
    check = lambda *extra: env.check("--profile", profile, "--ignore", str(env.ignore_file), *extra)   # the same backend
    capsys.readouterr()
    assert check() == 0
    monkeypatch.setenv("FAKE_MODE", "read_far")
    assert check() == 1
    out = capsys.readouterr().out
    assert statuses(out)["ipc"] == "FAIL" and "touch nothing the other side covers: 8 (at most 0)" in out
    monkeypatch.setattr(golden, "CROSS_MAX_DETACHED", 8)                     # the constant is the only place of the number
    assert check() == 0
    monkeypatch.setattr(golden, "CROSS_MAX_DETACHED", 7)
    assert check() == 1
    monkeypatch.setattr(golden, "CROSS_MAX_DETACHED", 0)
    monkeypatch.setenv("FAKE_MODE", "read_near")                             # 16 bytes that continue what the stream read
    assert check() == 0
    monkeypatch.setattr(golden, "CROSS_MAX_ONE_SIDED", 15)
    capsys.readouterr()
    assert check() == 1
    assert "bytes only the candidate covers: 16 (at most 15)" in capsys.readouterr().out


def test_the_same_profile_does_not_get_the_byte_limits(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "read_far")
    capsys.readouterr()
    assert env.check("--profile", "same") == 1                               # it fails, as the exact comparison, not by a limit
    assert "touch nothing" not in capsys.readouterr().out


def test_the_cross_profile_numbers_are_the_decisions():
    assert golden.CROSS_TOLERANCE == 4 and golden.CROSS_MIN_MARGIN == 60       # D50
    assert golden.CROSS_MAX_SHIFT == 48                                        # D51
    assert golden.CROSS_MAX_SURPLUS == 16                                      # D52
    assert golden.FS_WRITE_MASKS == golden.ROOT / "config" / "native" / "fs_write_masks.txt"


def test_cross_profile_applies_the_fs_rules_of_d52(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    # another timing model: the Reads in another order and one fewer, and its Write holds other bytes in the
    # range the mask file masks
    assert cross(env) == 0
    out = capsys.readouterr().out
    assert statuses(out)["ipc"] == "PASS"
    assert "fs reads: 1 thread(s) read; 3 on the reference and 2 on the candidate" in out
    assert "fs write masks: 1 line(s) of session S1, applied to 1 Write(s) of the reference and 1 of the candidate" in out
    # the same differences are failures to the same profile: nothing is loosened there
    assert env.check("--candidate-args", "--cpu jit", "--profile", "same") == 1
    assert statuses(capsys.readouterr().out)["ipc"] == "FAIL"


@pytest.mark.parametrize("mode", ["write_outside", "read_crc"])
def test_cross_profile_fails_an_fs_difference_the_rules_do_not_cover(env, capsys, monkeypatch, mode):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", mode)
    capsys.readouterr()
    assert cross(env) == 1
    got = statuses(capsys.readouterr().out)
    assert got["ipc"] == "FAIL"
    assert all(v != "FAIL" for k, v in got.items() if k != "ipc")


def test_cross_profile_bounds_the_surplus_of_reads(env, capsys, monkeypatch):
    # the 17 reads are at made-up offsets: the limits on the bytes one side covers (N0-37) are lifted here, they have
    # their own tests below
    monkeypatch.setattr(golden, "CROSS_MAX_ONE_SIDED", 10**9)
    monkeypatch.setattr(golden, "CROSS_MAX_DETACHED", 10**9)
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "reads_many")          # 17 more reads than the recording (the same backend)
    capsys.readouterr()
    assert env.check("--profile", "cross", "--ignore", str(env.ignore_file)) == 1
    out = capsys.readouterr().out
    assert statuses(out)["ipc"] == "FAIL" and "surplus 17" in out
    assert env.check("--profile", "cross", "--ignore", str(env.ignore_file), "--max-surplus", "17") == 0
    assert env.check("--profile", "cross", "--ignore", str(env.ignore_file), "--max-surplus", "16") == 1


def test_cross_profile_state_has_d50s_tolerance_and_margin_guard(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    assert cross(env) == 0
    out = capsys.readouterr().out
    got = statuses(out)
    assert got["state"] == "PASS" and got["state-margin"] == "PASS"
    assert "within 4 frames" in out                                            # D50's tolerance, in the state line
    assert "smallest input margin on the reference 2 frames (at least 1)" in out
    # a scene change four frames later is within the tolerance, five is not (the fake's change is at frame 10)
    monkeypatch.setenv("FAKE_MODE", "late4")
    assert cross(env) == 0
    monkeypatch.setenv("FAKE_MODE", "very_late")
    capsys.readouterr()
    assert cross(env) == 1 and statuses(capsys.readouterr().out)["state"] == "FAIL"
    capsys.readouterr()
    assert cross(env, "--tolerance", "5") == 1          # the state passes, but the input at frame 15 now follows the change by 0
    got = statuses(capsys.readouterr().out)
    assert got["state"] == "PASS" and got["state-margin"] == "FAIL"
    assert cross(env, "--tolerance", "5", "--min-margin", "0") == 0
    # the guard: the input at frame 12 comes 2 frames after the scene change at frame 10
    monkeypatch.delenv("FAKE_MODE")
    capsys.readouterr()
    assert cross(env, "--min-margin", "3") == 1
    out = capsys.readouterr().out
    assert statuses(out)["state-margin"] == "FAIL" and statuses(out)["state"] == "PASS"
    assert "input event at frame 12 (A): margin 2 frames on the reference, less than 3" in out
    # D50's own default is 60
    monkeypatch.setattr(golden, "CROSS_MIN_MARGIN", 60)
    assert cross(env) == 1
    # a check that requires the state comparison requires the guard too
    assert cross(env, "--min-margin", "1", "--require", "state") == 0


def test_the_margin_guard_is_skipped_with_the_state_comparison(env, capsys, monkeypatch):
    (env.sessions / "S1.txt").write_text(SESSION.replace("5:0, 20:2", "5:0, 20:0"))
    monkeypatch.setenv("FAKE_MODE", "no_change")
    assert env.record() == 0
    capsys.readouterr()
    assert cross(env) == 0
    got = statuses(capsys.readouterr().out)
    assert got["state"] == "SKIPPED" and "state-margin" not in got


def test_cross_profile_aligns_the_dsp_capture(env, capsys, monkeypatch):
    (env.sessions / "S3.txt").write_text(SESSION3)
    env.masks.write_text(env.masks.read_text() + "S3 32 64 4 4   test: the fake clock\n")
    monkeypatch.setenv("FAKE_MODE", "dsp_stream")      # one stream; jit has it 7 DSP frames later
    assert env.record(s="S3") == 0
    capsys.readouterr()
    assert cross(env, s="S3") == 0
    out = capsys.readouterr().out
    assert statuses(out, "S3")["dsp-10"] == "PASS"
    assert "shift +7 DSP frames" in out                                         # the shift is part of the result
    result = json.loads((env.out / "S3" / "check" / "result.json").read_text())
    assert "shift +7" in next(r for r in result if r["name"] == "dsp-10")["note"]
    # the bound is D51's 48 by default and an option
    assert cross(env, "--max-shift", "7", s="S3") == 0
    capsys.readouterr()
    assert cross(env, "--max-shift", "6", s="S3") == 1
    out = capsys.readouterr().out
    assert statuses(out, "S3")["dsp-10"] == "FAIL" and "more than the 6 allowed" in out
    # the same profile compares the capture exactly: another backend's capture is a difference
    capsys.readouterr()
    assert env.check("--candidate-args", "--cpu jit", "--profile", "same", s="S3") == 1
    assert statuses(capsys.readouterr().out, "S3")["dsp-10"] == "FAIL"


def test_the_cross_rules_are_options_of_the_cross_profile_only(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    for flag in ("--tolerance", "--min-margin", "--max-surplus", "--max-shift"):
        assert env.check(flag, "3") == 2, flag                                  # the same profile is exact
        assert "never loosened" in capsys.readouterr().err
        assert env.check("--profile", "cross", flag, "-1") == 2, flag
        assert "must not be negative" in capsys.readouterr().err
        assert env.check("--fallback", flag, "3") == 2, flag
    # the cross profile needs the mask file (D52)
    monkeypatch.setattr(golden, "FS_WRITE_MASKS", env.tmp / "no_masks.txt")
    assert cross(env) == 2
    assert "write masks" in capsys.readouterr().err
    assert env.check() == 0                                                     # the same profile does not read it

def test_cross_commands(env, monkeypatch):
    """The comparators the cross profile runs, and with what."""
    assert env.record() == 0
    calls = []

    def fake_tool(script, *args):
        calls.append((script.name, list(args)))
        return 0, "ok"

    monkeypatch.setattr(golden, "_tool", fake_tool)
    ref = env.out / "S1"
    session = golden.parse_session(env.sessions / "S1.txt")
    cfg = golden.Compare("cross", ignore=[env.ignore_file], real_changes=2)
    results = golden.compare_outputs(session, ref, ref, cfg)
    svc = [a for n, a in calls if n == "compare_svc_traces.py"]
    assert ["--normalize", "--outputs", "--cross-model", str(golden.SVC_CROSS_MODEL)] == svc[0][:4]
    assert svc[1][:2] == ["--normalize", "--outputs"] and "--cross-model" not in svc[1]      # the full one: info
    ipc = next(a for n, a in calls if n == "compare_ipc_traces.py")
    assert ipc[:2] == ["--ignore", str(env.ignore_file)]
    # D52: the Reads as multisets (at most 16 more), the Writes in full but for this session's masks
    assert "--fs-reads-multiset" in ipc and ipc[ipc.index("--max-surplus") + 1] == "16"
    assert ipc[ipc.index("--fs-write-mask") + 1] == str(env.masks)
    assert ipc[ipc.index("--fs-write-mask-session") + 1] == "S1"
    state = next(a for n, a in calls if n == "state_diff.py")
    assert "--min-changes" in state and state[state.index("--min-changes") + 1] == "2"
    assert state[state.index("--tolerance") + 1] == "4"                                  # D50 (D33 said 2)
    assert {r.name: r.status for r in results}["svc-full"] == "info"
    assert "state-margin" in {r.name for r in results}


# ---- check, the native profile (D76)

def native(env, *extra, s="S1"):
    """A check of `--cpu jit` against the recording in the native profile, with the test list."""
    return env.check("--candidate-args", "--cpu jit", "--profile", "native", "--ignore", str(env.ignore_file), *extra, s=s)


SESSION_LATE = """\
# name: S5
# needs: none
# frames: 80
# checkpoints: 5:0, 60:4
# captures:
f70:A
"""

SESSION_LONG_DSP = """\
# name: S4
# needs: none
# frames: 330
# checkpoints: 5:0, 20:2, 60:4
# captures: dsp:10:300
f70:A
"""


def test_the_native_profile_numbers_are_d55s_and_the_cross_ones_are_unchanged():
    assert golden.NATIVE_TOLERANCE == 24 and golden.NATIVE_MAX_SHIFT == 300                  # D76
    assert golden.NATIVE_FS_READS_COALESCE is True                                           # D76
    assert golden.PROFILES == ("same", "cross", "native")
    assert golden.CROSS_TOLERANCE == 4 and golden.CROSS_MIN_MARGIN == 60 and golden.CROSS_MAX_SHIFT == 48
    assert golden.CROSS_MAX_SURPLUS == 16
    cross_ = golden.Compare("cross")
    nat = golden.Compare("native")
    assert [cross_.rule(n) for n in ("tolerance", "min_margin", "max_surplus", "max_shift")] == [4, 60, 16, 48]
    assert [nat.rule(n) for n in ("tolerance", "min_margin", "max_surplus", "max_shift")] == [24, 60, 16, 300]
    assert golden.Compare("native", tolerance=7).rule("tolerance") == 7                      # the options still apply


def test_the_profile_names_itself_and_is_never_the_default(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    assert native(env) == 0
    assert "(profile native)" in capsys.readouterr().out
    # only --profile native asks for it
    assert golden.resolve_profile("native", None, None, "recomp") == "native"
    assert golden.resolve_profile(None, "RUN2", None, "recomp") == "cross"
    assert golden.resolve_profile(None, None, "jit", "recomp") == "cross"


def test_native_profile_passes_what_cross_passes_and_gates_the_same_things(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    assert cross(env) == 0 and native(env) == 0
    for mode, failing in (("svc_diff", "svc"), ("hid_diff", "hid"), ("write_outside", "ipc"), ("read_crc", "ipc")):
        monkeypatch.setenv("FAKE_MODE", mode)
        capsys.readouterr()
        code = native(env)
        got = statuses(capsys.readouterr().out)
        assert code == 1 and got[failing] == "FAIL", mode
    monkeypatch.setenv("FAKE_MODE", "dsp_diff")
    capsys.readouterr()
    assert native(env) == 1 and statuses(capsys.readouterr().out)["dsp-10"] == "FAIL"
    # D52's masks and the surplus of reads are the cross profile's (the byte limits of N0-37 lifted: made-up offsets)
    monkeypatch.setattr(golden, "CROSS_MAX_ONE_SIDED", 10**9)
    monkeypatch.setattr(golden, "CROSS_MAX_DETACHED", 10**9)
    monkeypatch.setenv("FAKE_MODE", "reads_many")           # 17 more reads that touch each other: one run more
    assert native(env) == 0
    monkeypatch.setenv("FAKE_MODE", "reads_apart")
    capsys.readouterr()
    assert native(env) == 1
    assert "surplus 18" in capsys.readouterr().out      # 17 runs more, and the read the shorter stream stops before
    assert native(env, "--max-surplus", "17") == 1
    assert native(env, "--max-surplus", "18") == 0


def test_native_profile_tolerates_a_scene_change_24_frames_late_and_no_more(env, capsys, monkeypatch):
    (env.sessions / "S5.txt").write_text(SESSION_LATE)
    env.masks.write_text(env.masks.read_text() + "S5 32 64 4 4   test: the fake clock\n")
    assert env.record(s="S5") == 0
    monkeypatch.setenv("FAKE_MODE", "late15")           # the scene changes 15 frames later than in the recording
    capsys.readouterr()
    assert cross(env, s="S5") == 1
    assert statuses(capsys.readouterr().out, "S5")["state"] == "FAIL"
    assert native(env, s="S5") == 0
    out = capsys.readouterr().out
    got = statuses(out, "S5")
    assert got["state"] == "PASS" and got["state-margin"] == "PASS" and got["checkpoints"] == "PASS"
    assert "largest scene-change shift 15 frames (at most 24)" in out
    assert "(profile native)" in out
    assert native(env, "--tolerance", "14", s="S5") == 1               # the option still applies
    assert native(env, "--tolerance", "15", s="S5") == 0
    monkeypatch.setenv("FAKE_MODE", "late30")                          # 30 frames: past D76's 24
    capsys.readouterr()
    assert native(env, s="S5") == 1
    assert statuses(capsys.readouterr().out, "S5")["state"] == "FAIL"
    assert native(env, "--tolerance", "30", s="S5") == 0
    # the same profile is exact
    assert env.check("--candidate-args", "--cpu jit", "--profile", "same", s="S5") == 1


def test_native_profile_keeps_d50s_margin_guard(env, capsys, monkeypatch):
    (env.sessions / "S5.txt").write_text(SESSION_LATE.replace("f70:A", "f45:A"))      # 5 frames after the late change at 40
    env.masks.write_text(env.masks.read_text() + "S5 32 64 4 4   test: the fake clock\n")
    assert env.record(s="S5") == 0
    monkeypatch.setenv("FAKE_MODE", "late30")
    capsys.readouterr()
    assert native(env, "--tolerance", "30", s="S5") == 0
    monkeypatch.setattr(golden, "CROSS_MIN_MARGIN", 60)                  # D50's own number, the same in every profile
    capsys.readouterr()
    assert native(env, "--tolerance", "30", s="S5") == 1
    got = statuses(capsys.readouterr().out, "S5")
    assert got["state"] == "PASS" and got["state-margin"] == "FAIL"


def test_native_profile_aligns_the_dsp_capture_up_to_300_frames_with_odd_shifts(env, capsys, monkeypatch):
    (env.sessions / "S4.txt").write_text(SESSION_LONG_DSP)
    env.masks.write_text(env.masks.read_text() + "S4 32 64 4 4   test: the fake clock\n")
    monkeypatch.setenv("FAKE_MODE", "dsp_far")          # one stream; jit has it 205 DSP frames later
    assert env.record(s="S4") == 0
    capsys.readouterr()
    assert native(env, s="S4") == 0
    out = capsys.readouterr().out
    assert statuses(out, "S4")["dsp-10"] == "PASS" and "shift +205 DSP frames" in out
    result = json.loads((env.out / "S4" / "check" / "result.json").read_text())
    assert "the shift is odd" in next(r for r in result if r["name"] == "dsp-10")["note"]
    # the cross profile's 48 and the exact profile do not take it
    capsys.readouterr()
    assert cross(env, s="S4") == 1
    out = capsys.readouterr().out
    assert statuses(out, "S4")["dsp-10"] == "FAIL" and "more than the 48 allowed" in out
    assert env.check("--candidate-args", "--cpu jit", "--profile", "same", s="S4") == 1
    # the bound is 300 by default and an option
    assert native(env, "--max-shift", "205", s="S4") == 0
    capsys.readouterr()
    assert native(env, "--max-shift", "204", s="S4") == 1
    assert "more than the 204 allowed" in capsys.readouterr().out


def test_native_profile_merges_the_reads_that_touch_and_cross_does_not(env, capsys, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "pieces")
    assert env.record() == 0                      # the recording reads 32 bytes in two pieces
    capsys.readouterr()
    assert cross(env) == 1                        # the candidate (jit) reads them in one
    assert statuses(capsys.readouterr().out)["ipc"] == "FAIL"
    assert native(env) == 0
    out = capsys.readouterr().out
    assert statuses(out)["ipc"] == "PASS" and "after merging touching reads" in out
    result = json.loads((env.out / "S1" / "check" / "result.json").read_text())
    assert "after merging touching reads: 1 and 1" in next(r for r in result if r["name"] == "ipc")["note"]
    assert env.check("--candidate-args", "--cpu jit", "--profile", "same") == 1


def test_native_commands(env, monkeypatch):
    """The comparators the native profile runs, and with what: the cross profile's, and D76's."""
    assert env.record() == 0
    calls = []

    def fake_tool(script, *args):
        calls.append((script.name, list(args)))
        return 0, "ok"

    monkeypatch.setattr(golden, "_tool", fake_tool)
    ref = env.out / "S1"
    session = golden.parse_session(env.sessions / "S1.txt")
    results = {}
    for profile in ("cross", "native"):
        calls.clear()
        cfg = golden.Compare(profile, ignore=[env.ignore_file], real_changes=2)
        results[profile] = {r.name: r.status for r in golden.compare_outputs(session, ref, ref, cfg)}
        ipc = next(a for n, a in calls if n == "compare_ipc_traces.py")
        state = next(a for n, a in calls if n == "state_diff.py")
        assert "--fs-reads-multiset" in ipc and ipc[ipc.index("--max-surplus") + 1] == "16"            # D52, both
        assert ipc[ipc.index("--fs-write-mask") + 1] == str(env.masks)
        assert ipc[ipc.index("--fs-write-mask-session") + 1] == "S1"
        assert ("--fs-reads-coalesce" in ipc) is (profile == "native")
        assert ("--fs-reads-prefix" in ipc) is (profile == "native")
        assert state[state.index("--tolerance") + 1] == ("24" if profile == "native" else "4")
        svc = [a for n, a in calls if n == "compare_svc_traces.py"]
        assert svc[0][:4] == ["--normalize", "--outputs", "--cross-model", str(golden.SVC_CROSS_MODEL)]
    # the same comparisons, the same names; the screenshots: judged at listed frames in cross, information in native
    assert results["native"].pop("images") == "info" and results["native"] == {
        k: v for k, v in results["cross"].items() if not k.startswith("images")}
    calls.clear()
    golden.compare_outputs(session, ref, ref, golden.Compare("same", real_changes=2))
    assert not [a for n, a in calls if n == "compare_ipc_traces.py" and "--fs-reads-coalesce" in a]


def test_native_rules_are_options_of_the_cross_and_native_profiles_only(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    for flag in ("--tolerance", "--min-margin", "--max-surplus", "--max-shift"):
        assert env.check(flag, "3") == 2, flag                                  # the same profile is exact
        assert "never loosened" in capsys.readouterr().err
        assert env.check("--profile", "native", flag, "-1") == 2, flag
        assert "must not be negative" in capsys.readouterr().err
    assert env.check("--ignore", str(env.ignore_file)) == 2
    assert "cross and native profiles only" in capsys.readouterr().err
    assert env.check("--fallback", "--profile", "native") == 2                  # D30's fallback is the same profile's
    capsys.readouterr()
    # the native profile needs the mask file and the cross-model SVC list, as the cross profile does
    monkeypatch.setattr(golden, "FS_WRITE_MASKS", env.tmp / "no_masks.txt")
    assert native(env) == 2
    assert "write masks" in capsys.readouterr().err
    monkeypatch.setattr(golden, "FS_WRITE_MASKS", env.masks)
    monkeypatch.setattr(golden, "SVC_CROSS_MODEL", env.tmp / "no_list.txt")
    assert native(env) == 2
    assert "no_list.txt" in capsys.readouterr().err


def test_the_native_profile_is_in_the_help_and_the_docstring():
    help_text = golden.make_parser().format_help()
    assert "native" in golden.__doc__ and "D76" in golden.__doc__
    parser = golden.make_parser()
    check = next(a for a in parser._subparsers._group_actions[0].choices["check"]._actions if a.dest == "profile")
    assert tuple(check.choices) == ("same", "cross", "native")
    assert help_text

# ---- check, D30's fallback

def test_fallback_commands(env, monkeypatch):
    assert env.record() == 0
    calls = []

    def fake_tool(script, *args):
        calls.append((script.name, list(args)))
        return 0, "ok"

    monkeypatch.setattr(golden, "_tool", fake_tool)
    ref = env.out / "S1"
    session = golden.parse_session(env.sessions / "S1.txt")
    cfg = golden.Compare("same", fallback=True, ignore=[env.ignore_file], real_changes=2)
    results = {r.name: r.status for r in golden.compare_outputs(session, ref, ref, cfg)}
    svc = next(a for n, a in calls if n == "compare_svc_traces.py")
    assert svc[:4] == ["--normalize", "--outputs", "--collapse-polling", str(golden.SVC_POLLING)]
    assert "--in4" not in svc and "--cross-model" not in svc
    ipc = next(a for n, a in calls if n == "compare_ipc_traces.py")
    assert "--ignore" not in ipc                                  # no ignore list in the same profile
    assert not [a for a in ipc if a.startswith("--fs-") or a == "--max-surplus"]       # and none of D52's rules
    state = next(a for n, a in calls if n == "state_diff.py")
    assert state[state.index("--tolerance") + 1] == "0" and state[state.index("--min-changes") + 1] == "2"
    # unmapped, the captures and the screenshots exactly as in `same`
    assert results["unmapped"] == "pass" and results["images"] == "pass" and results["hid"] == "pass"
    assert len([1 for n, _ in calls if n == "compare_svc_traces.py"]) == 1        # no full comparison


def test_fallback_end_to_end_and_its_usage_errors(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--fallback") == 0
    out = capsys.readouterr().out
    assert "D30 fallback" in out and statuses(out)["svc"] == "PASS" and statuses(out)["state"] == "PASS"
    for extra in (["--fallback", "--profile", "cross"], ["--fallback", "--candidate-runner", str(env.script)],
                  ["--fallback", "--ignore", str(env.ignore_file)],
                  ["--fallback", "--candidate-args", "--cpu jit"]):
        assert env.check(*extra) == 2, extra
    assert "fallback" in capsys.readouterr().err


# ---- check, exit statuses

def test_exit_status_function():
    r = golden.Result
    assert golden.exit_status([r("svc", "pass")], []) == 0
    assert golden.exit_status([r("svc", "pass"), r("images", "info")], ["svc"]) == 0
    assert golden.exit_status([r("svc", "pass"), r("ipc", "fail")], []) == 1
    assert golden.exit_status([r("svc", "skipped"), r("ipc", "skipped")], []) == 3
    assert golden.exit_status([r("svc-full", "info")], []) == 3
    assert golden.exit_status([], []) == 3
    assert golden.exit_status([r("svc", "pass"), r("images", "skipped")], ["images"]) == 3
    assert golden.exit_status([r("svc", "pass")], ["images"]) == 3               # it did not run at all
    assert golden.exit_status([r("svc", "pass"), r("images", "skipped")], []) == 0
    assert golden.exit_status([r("dsp-10", "pass"), r("dsp-20", "pass")], ["dsp"]) == 0
    assert golden.exit_status([r("dsp-10", "pass"), r("dsp-20", "skipped")], ["dsp"]) == 3
    assert golden.exit_status([r("svc", "fail"), r("images", "skipped")], ["images"]) == 1    # a failure wins


def test_a_candidate_that_writes_nothing_exits_3(env, capsys):
    assert env.record() == 0
    quiet = env.tmp / "quiet.py"
    quiet.write_text("import sys, os\n"
                     "u = sys.argv[sys.argv.index('--user') + 1]\n"
                     "open(os.path.join(u, 'ran.txt'), 'a').write('x')\n")
    capsys.readouterr()
    for profile in ("same", "cross"):
        assert env.check("--candidate-runner", str(quiet), "--profile", profile) == 3
        out = capsys.readouterr().out
        assert "PASS" not in out and "SKIPPED" in out and "0 passed" in out and "nothing was compared" in out
    # but the reference runner missing a file is a failure
    broken = env.tmp / "broken.py"
    broken.write_text(quiet.read_text())
    assert env.check("--runner", str(broken)) == 1


def test_a_skipped_required_comparison_exits_3(env, capsys, monkeypatch):
    monkeypatch.setenv("FAKE_NO_SHOTS", "1")
    assert env.record() == 0
    capsys.readouterr()
    # a foreign candidate in the same profile writes no screenshots, no unmapped report and no GPU capture
    assert env.check("--candidate-runner", str(env.script), "--profile", "same") == 0
    got = statuses(capsys.readouterr().out)
    assert got["images"] == "SKIPPED" and got["unmapped"] == "SKIPPED" and got["gpu-10"] == "SKIPPED"
    assert got["svc"] == "PASS"
    assert env.check("--candidate-runner", str(env.script), "--profile", "same", "--require", "svc,ipc") == 0
    capsys.readouterr()
    for name in ("images", "unmapped"):
        assert env.check("--candidate-runner", str(env.script), "--profile", "same", "--require", name) == 3
        assert f"--require {name}: skipped or not run (exit 3)" in capsys.readouterr().out
    # a comparison that does not exist for the session is not run, so a requirement of it is not met
    (env.sessions / "S2.txt").write_text(SESSION.replace("S1", "S2").replace(", dsp:10:3", ""))
    assert env.record(s="S2") == 0
    assert env.check("--require", "svc", s="S2") == 0
    assert env.check("--require", "svc,dsp", s="S2") == 3


def test_a_failure_together_with_skips_exits_1(env, capsys, monkeypatch):
    monkeypatch.setenv("FAKE_NO_SHOTS", "1")
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "svc_diff")
    capsys.readouterr()
    assert env.check("--candidate-runner", str(env.script), "--profile", "same", "--require", "images") == 1
    got = statuses(capsys.readouterr().out)
    assert got["svc"] == "FAIL" and got["images"] == "SKIPPED"


def test_usage_errors(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--require", "svc,nonsense") == 2 and "unknown comparison" in capsys.readouterr().err
    assert env.check("--repeat", "0") == 2
    # --ignore with the same profile: forced, or the default one
    assert env.check("--ignore", str(env.ignore_file), "--profile", "same") == 2
    assert "never loosened" in capsys.readouterr().err
    assert env.check("--ignore", str(env.ignore_file)) == 2
    assert env.check("--ignore", str(env.ignore_file), "--candidate-args", "--cpu recomp") == 2
    # nothing ran for any of them
    assert len(env.calls()) == 1
    # with the cross profile it is fine
    assert env.check("--ignore", str(env.ignore_file), "--candidate-args", "--cpu jit") == 0


def test_check_runner_failure(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "fail")
    assert env.check() == 1
    assert "runner: FAIL" in capsys.readouterr().out


def test_candidate_without_a_state_trace_skips_state_and_checkpoints(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_MODE", "no_state")
    capsys.readouterr()
    assert env.check("--candidate-runner", str(env.script)) == 0
    got = statuses(capsys.readouterr().out)
    assert got["state"] == "SKIPPED" and got["checkpoints"] == "SKIPPED" and got["svc"] == "PASS"
    # the reference runner not writing it is a failure, for both
    assert env.check() == 1
    got = statuses(capsys.readouterr().out)
    assert got["state"] == "FAIL" and got["checkpoints"] == "FAIL"


def test_check_needs_a_recording_and_notices_tampering(env, capsys):
    assert env.check() == 2 and "not recorded" in capsys.readouterr().err
    assert env.record() == 0
    with open(env.out / "S1" / "svc.txt", "a") as f:
        f.write("# edited\n")
    assert env.check() == 2 and "changed since" in capsys.readouterr().err


# ---- --repeat

def test_repeat(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--repeat", "3", "--require", "svc,ipc,state,checkpoints,hid,images,unmapped,dsp") == 0
    out = capsys.readouterr().out
    got = statuses(out)
    assert got["run3: state"] == "PASS" and got["run1: svc"] == "PASS"
    # the runs are compared with each other in the same profile as well
    assert got["run2 against run1: state"] == "PASS" and got["run3 against run1: svc"] == "PASS"
    assert "run2 against run1: images" in got and "run3 against run1: dsp-10" in got
    assert len(env.calls()) == 4
    monkeypatch.setenv("FAKE_MODE", "vary_state")     # state differs from run to run
    assert env.check("--repeat", "2") == 1
    got = statuses(capsys.readouterr().out)
    assert got["run2 against run1: state"] == "FAIL" and got["run1: state"] == "FAIL"


def test_repeat_in_the_cross_profile_compares_the_runs_exactly(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    # two runs of the jit model are close to each other (same ticks), but the recording is a different model
    assert env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file), "--repeat", "2") == 0
    got = statuses(capsys.readouterr().out)
    assert got["run2 against run1: svc"] == "PASS" and got["run1: svc"] == "PASS"
    assert got["run1: svc-full"] == "INFO"


def test_repeat_skips_for_a_candidate_that_writes_less(env, capsys, monkeypatch):
    monkeypatch.setenv("FAKE_NO_SHOTS", "1")
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--candidate-runner", str(env.script), "--repeat", "2") == 0
    got = statuses(capsys.readouterr().out)
    assert got["run2 against run1: svc"] == "PASS" and got["run2 against run1: state"] == "PASS"
    assert got["run2 against run1: images"] == "SKIPPED" and got["run2 against run1: unmapped"] == "SKIPPED"


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
    (env.sessions / "S1.txt").write_text(SESSION.replace("dsp:10:3", "dsp:10:3:silent"))
    golden.main(["list", *env.common])
    assert "captures=gpu:10:2,dsp:10:3:silent" in capsys.readouterr().out


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


def test_option_value():
    assert golden.option_value(["--a", "1", "--cpu", "jit"], "--cpu") == "jit"
    assert golden.option_value(["--a", "1", "--cpu"], "--cpu") is None
    assert golden.option_value([], "--cpu") is None


def test_the_shared_page_dump_matches_the_file_the_runner_writes():
    assert golden.shared_page_dump() == "f300:1FF81000:0x1000"
    assert golden.shared_page_file() == "shots/mem_f0000300_1ff81000.bin"


# ---- the image gate of the cross profile (N2b-17, D58)

def frames_file(env, text="S1 10\nS1 30\n"):
    p = env.tmp / "image_frames.txt"
    p.write_text(text, encoding="utf-8")
    return str(p)


def jit_check(env, *extra):
    return env.check("--candidate-args", "--cpu jit", "--ignore", str(env.ignore_file), *extra)


def result_of(env, name):
    result = json.loads((env.out / "S1" / "check" / "result.json").read_text())
    return next(r for r in result if r["name"] == name)


def test_image_frames_file_format(tmp_path):
    parsed = golden.parse_image_frames("# frames\nG1 360\nG1 300  # first\n\nG2 420\nG1 360\n")
    assert parsed == {"G1": [300, 360], "G2": [420]}
    for bad in ("G1\n", "G1 abc\n", "G1 300 301\n", "G1 -5\n", "G1 3²\n", "G1 ٣٠٠\n"):
        with pytest.raises(golden.GoldenError):
            golden.parse_image_frames(bad)
    path = tmp_path / "f.txt"
    path.write_text("G1 300\nG2 420\n", encoding="utf-8")
    assert golden.load_image_frames(path, "G2") == [420] and golden.load_image_frames(path, "G3") == []
    with pytest.raises(golden.GoldenError):
        golden.load_image_frames(tmp_path / "missing.txt", "G1")           # asked for, not there


def test_the_default_image_frames_file_is_the_repository_one(env, monkeypatch):
    assert golden.IMAGE_FRAMES == ROOT / "config" / "native" / "image_frames.txt"
    monkeypatch.setattr(golden, "IMAGE_FRAMES", env.tmp / "no_such_file.txt")
    assert golden.load_image_frames(None, "S1") == []                      # a missing default file lists nothing
    listed = env.tmp / "listed.txt"
    listed.write_text("S1 10\n", encoding="utf-8")
    monkeypatch.setattr(golden, "IMAGE_FRAMES", listed)
    assert golden.load_image_frames(None, "S1") == [10]


def test_images_are_judged_at_the_listed_frames_only(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_PIXELS", "20:200")             # frame 20 of the candidate is far off, the others equal
    capsys.readouterr()
    assert jit_check(env, "--image-frames", frames_file(env), "--require", "images,svc,state") == 0
    out = capsys.readouterr().out
    got = statuses(out)
    assert got["images"] == "PASS" and got["images-all"] == "INFO"
    assert "lowest PSNR inf dB" in out and "differ" in result_of(env, "images-all")["detail"]
    # listing frame 20 fails it, and the failure names the frame and the screen
    assert jit_check(env, "--image-frames", frames_file(env, "S1 10\nS1 20\n")) == 1
    out = capsys.readouterr().out
    assert statuses(out)["images"] == "FAIL" and "frame 20 top" in out
    assert "psnr=" in result_of(env, "images")["detail"] and "2 of 4 screenshots fail" in result_of(env, "images")["note"]


def test_a_small_difference_passes_imgdiffs_rule(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    # "pixels": every candidate pixel is 1/255 off, a PSNR of 48 dB and a block error of 1; byte for byte different
    monkeypatch.setenv("FAKE_MODE", "pixels")
    assert jit_check(env, "--image-frames", frames_file(env)) == 0
    assert statuses(capsys.readouterr().out)["images"] == "PASS"
    assert result_of(env, "images")["note"].startswith("lowest PSNR 48.1 dB, worst block 1.0/255")
    assert "6 of 6 differ" in result_of(env, "images-all")["detail"]

def test_a_far_difference_fails_at_a_listed_frame(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_PIXELS", "10:200,30:200")
    capsys.readouterr()
    assert jit_check(env, "--image-frames", frames_file(env)) == 1
    assert statuses(capsys.readouterr().out)["images"] == "FAIL"


def test_no_listed_frame_skips_images_and_a_requirement_of_it_exits_3(env, capsys):
    assert env.record() == 0
    capsys.readouterr()
    assert jit_check(env, "--image-frames", frames_file(env, "S2 10\n")) == 0     # another session's frames
    assert statuses(capsys.readouterr().out)["images"] == "SKIPPED"
    assert jit_check(env, "--image-frames", frames_file(env, "# none\n"), "--require", "images") == 3
    assert "--require images: skipped or not run (exit 3)" in capsys.readouterr().out


def test_a_frame_in_gas_txt_is_skipped_and_not_judged(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_PIXELS", "20:200")
    monkeypatch.setenv("FAKE_GAS", "20")
    capsys.readouterr()
    assert jit_check(env, "--image-frames", frames_file(env, "S1 10\nS1 20\nS1 30\n"), "--require", "images") == 0
    out = capsys.readouterr().out
    assert statuses(out)["images"] == "PASS" and "1 frame(s) skipped (gas): 20" in out
    # all listed frames in gas: nothing judged, so skipped (exit 3 when required)
    assert jit_check(env, "--image-frames", frames_file(env, "S1 20\n"), "--require", "images") == 3
    out = capsys.readouterr().out
    assert statuses(out)["images"] == "SKIPPED" and "skipped (gas)" in out
    # no gas.txt at all (the reference runner): the same frame is judged
    monkeypatch.delenv("FAKE_GAS")
    assert jit_check(env, "--image-frames", frames_file(env, "S1 20\n")) == 1


def test_a_listed_screenshot_the_run_did_not_write_fails(env, capsys, monkeypatch):
    assert env.record() == 0
    monkeypatch.setenv("FAKE_SKIP_SHOT", "20_bottom")
    capsys.readouterr()
    assert jit_check(env, "--image-frames", frames_file(env, "S1 10\nS1 20\n")) == 1
    out = capsys.readouterr().out
    assert statuses(out)["images"] == "FAIL" and "frame 20 bottom: the run wrote no screenshot" in out


def _tiny_ppm(width: int, height: int = 1) -> bytes:
    return f"P6\n{width} {height}\n255\n".encode() + bytes(width * height * 3)


def test_images_result_fails_a_listed_frame_the_recording_lacks(tmp_path):
    ref, cand = tmp_path / "ref", tmp_path / "cand"
    for folder in (ref, cand):
        (folder / "shots").mkdir(parents=True)
    for screen in ("top", "bottom"):
        (cand / "shots" / f"f0000020_{screen}.ppm").write_bytes(_tiny_ppm(1))
    (ref / "shots" / "f0000020_bottom.ppm").write_bytes(_tiny_ppm(1))        # the top one is missing
    status, detail, note = golden.images_result(ref, cand, [20])
    assert status == "fail" and "frame 20 top: the recording has no screenshot" in detail
    assert "bottom" not in detail and note == "1 of 2 screenshots fail"


def test_images_result_fails_a_listed_frame_of_another_size(tmp_path):
    ref, cand = tmp_path / "ref", tmp_path / "cand"
    for folder in (ref, cand):
        (folder / "shots").mkdir(parents=True)
    for screen in ("top", "bottom"):
        (ref / "shots" / f"f0000020_{screen}.ppm").write_bytes(_tiny_ppm(1))
        (cand / "shots" / f"f0000020_{screen}.ppm").write_bytes(_tiny_ppm(2))
    status, detail, note = golden.images_result(ref, cand, [20])
    assert status == "fail" and "frame 20 top: different sizes: 1x1 and 2x1" in detail
    assert "frame 20 bottom: different sizes" in detail and note == "2 of 2 screenshots fail"
    (cand / "shots" / "f0000020_top.ppm").write_bytes(_tiny_ppm(1))           # only the bottom stays wrong
    assert golden.images_result(ref, cand, [20])[2] == "1 of 2 screenshots fail"


def test_the_same_and_native_profiles_do_not_take_image_frames(env, capsys, monkeypatch):
    assert env.record() == 0
    capsys.readouterr()
    assert env.check("--image-frames", frames_file(env)) == 2                        # same: exact
    assert "cross profile" in capsys.readouterr().err
    assert jit_check(env, "--profile", "native", "--image-frames", frames_file(env)) == 2
    monkeypatch.setenv("FAKE_PIXELS", "20:200")
    capsys.readouterr()
    assert jit_check(env, "--profile", "native") == 0
    assert statuses(capsys.readouterr().out)["images"] == "INFO"                     # native: information
    monkeypatch.delenv("FAKE_PIXELS")
    assert env.check() == 0 and statuses(capsys.readouterr().out)["images"] == "PASS"   # same: still SHA-256


def test_the_listed_frames_of_the_repository_file_are_used_by_default(env, capsys, monkeypatch):
    listed = env.tmp / "default_frames.txt"
    listed.write_text("S1 10\n", encoding="utf-8")
    monkeypatch.setattr(golden, "IMAGE_FRAMES", listed)
    assert env.record() == 0
    capsys.readouterr()
    assert jit_check(env, "--require", "images") == 0
    assert statuses(capsys.readouterr().out)["images"] == "PASS"


def test_shot_options_go_only_to_a_candidate_that_lists_them(env, capsys, monkeypatch):
    assert env.record() == 0
    prog = env.tmp / "prog"
    prog.mkdir()
    cand = ["--candidate-runner", str(env.script), "--candidate-program", str(prog)]
    # a runner whose --help lists --shots gets them, in the recording's frame-keyed form, and no shared-page dump
    assert env.check(*cand, "--image-frames", frames_file(env), "--ignore", str(env.ignore_file)) == 0
    a = env.calls()[-1]["argv"]
    assert a[a.index("--shots") + 1] == str(env.out / "S1" / "check" / "run1" / "shots")
    assert a[a.index("--shots-from") + 1] == "f10" and a[a.index("--shot-every") + 1] == "f10"
    assert "--dump" not in a and "--trace-timing" not in a
    assert statuses(capsys.readouterr().out)["images"] == "PASS"
    # one that does not, gets none, and `images` is skipped
    monkeypatch.setenv("FAKE_NO_SHOTS", "1")
    assert env.check(*cand, "--image-frames", frames_file(env), "--ignore", str(env.ignore_file)) == 0
    a = env.calls()[-1]["argv"]
    assert not {"--shots", "--shots-from", "--shot-every", "--dump"} & set(a)
    assert statuses(capsys.readouterr().out)["images"] == "SKIPPED"
    # a runner that cannot be asked has none either
    assert golden.runner_has_shots(env.tmp / "no_such_runner") is False
    assert golden.runner_has_shots(env.script) is False                                # (FAKE_NO_SHOTS is still set)
    monkeypatch.delenv("FAKE_NO_SHOTS")
    assert golden.runner_has_shots(env.script) is True
