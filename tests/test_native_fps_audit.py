"""tools/native/fps_audit.py: the 60/120 fps audit (N3-22, N3-34), on synthetic sessions, traces and captures only.

Each of the six comparison items is checked both ways (a pair of runs that passes it, a pair that fails it); `run` is
checked with a fake runner that writes the files the native runner writes. D70's rules (N3-34) are checked by session,
mode and latency: each scene change against its own limit, the gated and the reported items, the bounds both ways.
"""
import json
import os
import struct
import sys
import textwrap
import threading
import time
import zlib

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import capfmt  # noqa: E402
import fps_audit  # noqa: E402
import golden  # noqa: E402
import observable_probe  # noqa: E402
import state_diff  # noqa: E402

HUNT = fps_audit.HUNT_SCENE
TICKS = capfmt.DSP_FRAME_TICKS
TPS = fps_audit.TICKS_PER_SECOND

SESSION = """\
# name: G9
# needs: none
# frames: 300
# checkpoints: 60:0x1000, 150:0x2000, 300:0x2000
# captures: gpu:50:3, dsp:20:10
# init-time: 1767268800.5
f10:A:f6,f20:CIRCLE:0:1:f30
f60:TOUCH:10:20:f6,f90:B:250
"""


def f32bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def sub(tmp_path, name):
    folder = tmp_path / name
    folder.mkdir()
    return folder


# -- synthetic state traces -----------------------------------------------------------------------------------------

def state_trace(rate, frames, scenes, pos=None, quest=None, quest_id=None, clock=None):
    """A state trace of `frames` frames at `rate` fps. scenes: [(frame, scene)] of this run (the scene is unreadable,
    `-`, before the first); pos(f) gives the hunter's (x, y, z) and quest(f) the quest counter, None when unreadable;
    quest_id(f) is the value of an eq observable; clock(f) the game clock in seconds (default f / rate)."""
    lines = ["# statetrace v1 2026-01-01 game=0x0004000000126300 cpu=native new3ds=off"]

    def hx(v):
        return "-" if v is None else f"0x{v:08x}"

    for f in range(1, frames + 1):
        scene = None
        for at, value in scenes:
            if f >= at:
                scene = value
        p = pos(f) if pos else None
        q = quest(f) if quest else None
        qid = quest_id(f) if quest_id else 0
        seconds = clock(f) if clock else f / rate
        lines.append(
            f"f={f} t={round(seconds * TPS)} scene={hx(scene)} applet_state=0x00 frame_counter=0x{f:08x} smain_delta=- "
            f"quest_id=0x{qid:x} stage_loader_state=-"
            f" hunter_x={hx(f32bits(p[0]) if p else None)} hunter_y={hx(f32bits(p[1]) if p else None)}"
            f" hunter_z={hx(f32bits(p[2]) if p else None)} quest_time={hx(q)}")
    return "\n".join(lines) + "\n"


def scaled(scenes, k=2):
    return [(f * k, s) for f, s in scenes]


# Real scene changes: to 0x2000 at frame 120 and to the hunt at frame 200 (60 fps); 0x1000 follows an unreadable scene
SCENES = [(5, 0x1000), (120, 0x2000), (200, HUNT)]


def shifted(by):
    """The scaled run's scene sequence with its first real change `by` frames (at 120 fps) later."""
    return [(10, 0x1000), (240 + by, 0x2000), (400, HUNT)]


def walk(rate, offset=0.0):
    """3 units per second of game time along x, plus a constant offset."""
    return lambda f: (f / rate * 3.0 + offset, 1.0, 2.0)


def countdown(rate, offset=0):
    return lambda f: 100000 - int(60 * f / rate) + offset


# -- synthetic IPC traces -------------------------------------------------------------------------------------------

IPC_HEAD = ["# ipctrace v1 2026-01-01 game=0x0004000000126300 cpu=native new3ds=off",
            "# thread 1 parent=0 entry=00100000 arg=00000000 stack=10000000 prio=48 core=0/0"]


def ipc_trace(path, writes=(), reads=()):
    """fs:USER file Writes [(offset, data)] and Reads [(offset, size, crc)] on thread 1."""
    lines = list(IPC_HEAD) + ["1 seq=1 session=srv:USER cmd=0x00010040 in=0x00010040 desc=- out=0x00010040,0x00000000 "
                              "outdesc=- t=1"]
    seq = 2
    for off, size, crc in reads:
        lines.append(f"1 seq={seq} session=fs:USER/file#0 cmd=0x080200c2 in=0x080200c2,0x{off:08x},0x00000000,"
                     f"0x{size:08x},0x{(size << 4) | 0xC:08x},0x0fffdaf8 desc=mapped-w:4:{size}:- "
                     f"out=0x08020082,0x00000000,0x{size:08x},0x{(size << 4) | 0xC:08x},0x0fffdaf8 "
                     f"outdesc=mapped-w:4:{size}:0x{crc:08x} t={seq}")
        seq += 1
    for off, data in writes:
        n = len(data)
        lines.append(f"1 seq={seq} session=fs:USER/file#1 cmd=0x08030102 in=0x08030102,0x{off:08x},0x00000000,"
                     f"0x{n:08x},0x00010001,0x0000020a,0x32ef8df0 desc=mapped-r:5:{n}:0x{zlib.crc32(data):08x} "
                     f"out=0x08030082,0x00000000,0x{n:08x},0x0000020a,0x32ef8df0 outdesc=- t={seq} "
                     f"data=5:{data.hex()}")
        seq += 1
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# -- synthetic DSP captures -----------------------------------------------------------------------------------------

CHANGES = [90, 93, 97, 100, 104, 107, 111, 115, 118, 122, 125, 129, 133, 136, 140, 144]


def stream_status(t):
    ident = 1 + sum(1 for c in CHANGES if t >= c)
    last = max((c for c in CHANGES if t >= c), default=0)
    return capfmt.make_status(enabled=1, sync_count=ident, buffer_position=(t - last) * 160,
                              current_buffer_id=ident, last_buffer_id=ident - 1)


def dsp_file(path, start, t_first, count, tweak=None):
    """`count` DSP frames, the counter `start + i`, holding the audio stream at time `t_first + i`."""
    with capfmt.Writer(str(path), "dsp", game=0x0004000000126300) as w:
        for i in range(count):
            counter = (start + i) & 0xFFFF
            regions = []
            for region in (0, 1):
                age = 0 if region == (counter & 1) else 1
                block = stream_status(t_first + i - age)
                if tweak is not None:
                    block = tweak(i, region, block)
                regions.append(capfmt.make_region((counter - age) & 0xFFFF, {0: block}))
            w.dsp_regions(1200 + i // 2, i * TICKS, regions[0], regions[1])
            w.dsp_pcm(1200 + i // 2, i * TICKS, capfmt.make_pcm([(i + 1, 1)] * 160))


ALIGNED = ((100, 100, 40), (100 + 17 - 10, 90, 40))        # the candidate's counters are 17 later for the same audio


# -- a pair of runs on disk -----------------------------------------------------------------------------------------

def make_pair(tmp_path, *, scenes=SCENES, scenes_scaled=None, pos=(None, None), quest=(None, None),
              quest_id=(None, None), clock=(None, None), frames=300, frames_scaled=None,
              checkpoints=((60, 0x1000), (150, 0x2000), (300, HUNT)), exit_codes=(0, 0), writes=([], []), dsp=None,
              dsp_start=20, dsp_windows=None, session="G9", mode=None, io_latency="azahar"):
    """Writes OUT/ with meta.json, observable.txt, r60/ and r120/ as `run` leaves them. `dsp` is one capture pair at
    game frame `dsp_start`; `dsp_windows` {start: pair} adds more. `mode` None writes a meta file from before the
    mode was recorded (real time)."""
    root = tmp_path / "out"
    (root / "r60").mkdir(parents=True)
    (root / "r120").mkdir()
    (root / "observable.txt").write_text(fps_audit.observable_text(state_diff.DEFAULT_OBSERVABLE.read_text()),
                                         encoding="utf-8")
    n120 = frames * 2 if frames_scaled is None else frames_scaled
    s120 = scaled(scenes) if scenes_scaled is None else scenes_scaled
    (root / "r60" / "state.txt").write_text(
        state_trace(60, frames, scenes, pos[0], quest[0], quest_id[0], clock[0]), encoding="utf-8")
    (root / "r120" / "state.txt").write_text(
        state_trace(120, n120, s120, pos[1], quest[1], quest_id[1], clock[1]), encoding="utf-8")
    ipc_trace(root / "r60" / "ipc.txt", writes=writes[0])
    ipc_trace(root / "r120" / "ipc.txt", writes=writes[1])
    windows = dict(dsp_windows or {})
    if dsp:
        windows[dsp_start] = dsp
    for start, (first, second) in windows.items():
        dsp_file(root / "r60" / f"dsp-{start}.mdsp", *first)
        dsp_file(root / "r120" / f"dsp-{start}.mdsp", *second)
    names = [f"dsp-{start}.mdsp" for start in windows]
    meta = {
        "version": 1, "session": session, "factor": 2, "io_latency": io_latency, "observable": "observable.txt",
        "frames": frames, "checkpoints": [list(c) for c in checkpoints], "dsp": names,
        "dsp_starts": {f"dsp-{start}.mdsp": start for start in windows},
        "runs": {"60": {"exit": exit_codes[0], "seconds": 1.0, "timed_out": False, "rate": 60, "frames": frames,
                        "dir": "r60"},
                 "120": {"exit": exit_codes[1], "seconds": 1.0, "timed_out": False, "rate": 120, "frames": frames * 2,
                         "dir": "r120"}}}
    if mode is not None:
        meta["mode"] = mode
    (root / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
    return root


BLOB = bytes(range(32))


def good_pair(tmp_path, **kwargs):
    """A pair that passes every item (the hunter walks 3 units a second, the countdown runs 60 a second)."""
    kwargs.setdefault("pos", (walk(60), walk(120)))
    kwargs.setdefault("quest", (countdown(60), countdown(120)))
    kwargs.setdefault("dsp", ALIGNED)
    kwargs.setdefault("writes", ([(0, BLOB)], [(0, BLOB)]))
    return make_pair(tmp_path, **kwargs)


def audit(root, **kwargs):
    """The items of a pair by name."""
    pair = fps_audit.read_pair(root)
    kwargs.setdefault("masks_file", None)
    return {i.name: i for i in fps_audit.compare_pair(pair, **kwargs)}


# D70's first guesses for the position and the quest clock: D70 (Decided) set no relative part, 3,700 units and 1.09 s,
# but the options still give the rule's other parts, which these tests check
FIRST_GUESSES = {"position_relative": 0.02, "position_absolute": 0.05, "quest_seconds": 1.0}


def chain(n, start=30, step=25):
    """n real scene changes at 60 fps, change #i to 0x2000 + i at frame start + step * i (0x1000 follows the
    unreadable scene, so it is not a change)."""
    return [(5, 0x1000)] + [(start + step * i, 0x2000 + i) for i in range(n)]


def chain_scaled(n, moved=None, by=0, start=30, step=25):
    """The same changes at 120 fps, change #moved `by` frames (at 120 fps) later."""
    return [(10, 0x1000)] + [(2 * (start + step * i) + (by if i == moved else 0), 0x2000 + i) for i in range(n)]


def compare_main(root, *extra):
    """`compare` on a folder with the synthetic Writes (the repository's mask file has lines for G2, G3 and G4)."""
    return fps_audit.main(["compare", str(root), "--fs-write-mask", "none", *extra])


def compare_json(root):
    return json.loads((root / "compare.json").read_text(encoding="utf-8"))


def item_lines(text):
    return {line.split()[0]: line for line in text.splitlines()
            if line.split() and line.split()[0] in fps_audit.ITEMS}


# -- scale ----------------------------------------------------------------------------------------------------------

def test_scale_is_observable_probes_scale(tmp_path):
    src = tmp_path / "G9.txt"
    src.write_text(SESSION, encoding="utf-8")
    out = tmp_path / "G9-x2.txt"
    assert fps_audit.main(["scale", str(src), "--factor", "2", "-o", str(out)]) == 0
    assert out.read_text(encoding="utf-8") == observable_probe.scale_session(SESSION, 2)
    text = out.read_text(encoding="utf-8")
    assert "# name: G9-x2" in text and "# frames: 600" in text
    assert "# checkpoints: 120:0x1000, 300:0x2000, 600:0x2000" in text
    assert "f20:A:f12" in text and "f40:CIRCLE:0:1:f60" in text and "f180:B:250" in text
    assert "# captures: gpu:50:3, dsp:20:10" in text            # the captures line is not scaled: run scales its starts
    assert b"\r" not in out.read_bytes()


def test_scale_refuses_a_bad_factor_and_a_missing_file(tmp_path, capsys):
    src = tmp_path / "G9.txt"
    src.write_text(SESSION, encoding="utf-8")
    assert fps_audit.main(["scale", str(src), "--factor", "0", "-o", str(tmp_path / "o.txt")]) == 2
    assert fps_audit.main(["scale", str(tmp_path / "none.txt"), "-o", str(tmp_path / "o.txt")]) == 2
    assert "fps_audit:" in capsys.readouterr().err


# -- the observable file and the arguments --------------------------------------------------------------------------

def test_the_observable_file_gets_the_four_lines_as_ignore_without_touching_the_base():
    base = state_diff.DEFAULT_OBSERVABLE.read_text(encoding="utf-8")
    text = fps_audit.observable_text(base)
    parsed = {o.name: o for o in state_diff.parse_observable(text)}
    for name in ("hunter_x", "hunter_y", "hunter_z", "quest_time"):
        assert name in parsed and not parsed[name].compare
    assert parsed["scene"].compare and parsed["quest_id"].compare
    assert fps_audit.observable_text(text) == text                    # already there: nothing added twice
    assert "hunter_x" not in base                                     # config/** is the orchestrator's


def test_a_bad_base_observable_file_is_refused():
    with pytest.raises(fps_audit.AuditError):
        fps_audit.observable_text("[0x1] 3 broken eq\n")


def write_session(tmp_path, text=SESSION):
    path = tmp_path / "G9.txt"
    path.write_text(text, encoding="utf-8")
    return path


def test_runner_args_for_both_rates(tmp_path):
    session = golden.parse_session(write_session(tmp_path))
    caps = [c for c in session.captures if c.kind == "dsp"]
    a60 = fps_audit.runner_args(program="game", code="lib.dll", user="u60", session=session, session_file="s.txt",
                                rate=60, factor_of_rate=1, io_latency="azahar", observable="obs.txt",
                                out=tmp_path / "r60", captures=caps)
    assert a60[:2] == ["game", "--code"] and a60[a60.index("--set") + 1] == "0:fps=60"
    assert a60[a60.index("--frames") + 1] == "300" and a60[a60.index("--init-time") + 1] == "1767268800.5"
    assert a60[a60.index("--io-latency") + 1] == "azahar" and "--trace-ipc-full" in a60
    assert a60[a60.index("--capture-dsp") + 1] == f"20:10:{tmp_path / 'r60' / 'dsp-20.mdsp'}"
    a120 = fps_audit.runner_args(program="game", code="lib.dll", user="u120", session=session, session_file="s.txt",
                                 rate=120, factor_of_rate=2, io_latency="none", observable="obs.txt",
                                 out=tmp_path / "r120", captures=caps)
    assert a120[a120.index("--set") + 1] == "0:fps=120" and a120[a120.index("--io-latency") + 1] == "none"
    assert a120[a120.index("--capture-dsp") + 1] == f"40:10:{tmp_path / 'r120' / 'dsp-20.mdsp'}"   # the start is scaled


def test_dsp_captures_add_to_the_sessions_and_refuse_a_double(tmp_path):
    session = golden.parse_session(write_session(tmp_path))
    caps = fps_audit.dsp_captures(session, [fps_audit.parse_dsp_capture("600:300")])
    assert [(c.start, c.frames) for c in caps] == [(20, 10), (600, 300)]
    with pytest.raises(fps_audit.AuditError):
        fps_audit.dsp_captures(session, [fps_audit.parse_dsp_capture("20:300")])
    for bad in ("600", "a:3", "6:0", "6:3:4"):
        with pytest.raises(fps_audit.AuditError):
            fps_audit.parse_dsp_capture(bad)


def test_the_base_name_of_a_scaled_session():
    assert fps_audit.base_name("G3-x2") == "G3" and fps_audit.base_name("G3") == "G3"
    assert fps_audit.base_name("G3-perf") == "G3-perf" and fps_audit.base_name("-x2") == "-x2"


# -- the lock -------------------------------------------------------------------------------------------------------

def test_the_lock_is_taken_released_and_waited_for(tmp_path):
    lock = tmp_path / "locks" / "emulator"
    with fps_audit.Lock(str(lock)):
        assert lock.is_dir()
    assert not lock.exists()
    lock.mkdir(parents=True)                                          # somebody else holds it
    threading.Timer(0.3, lock.rmdir).start()
    started = time.monotonic()
    with fps_audit.Lock(str(lock), poll=0.05):
        assert lock.is_dir() and time.monotonic() - started >= 0.25
    assert not lock.exists()
    lock.mkdir()
    with pytest.raises(fps_audit.AuditError):
        with fps_audit.Lock(str(lock), poll=0.01, wait=0.05):
            pass
    assert lock.is_dir()                                              # a lock that was not ours is left alone


# -- run, with a fake runner ----------------------------------------------------------------------------------------

FAKE = textwrap.dedent('''
    import json, os, sys, time
    sys.path.insert(0, {tools!r})
    import capfmt
    argv = sys.argv[1:]
    def opt(name, default=None):
        return argv[argv.index(name) + 1] if name in argv else default
    rate = int(opt("--set").split("fps=")[1])
    with open(os.environ["FAKE_LOG"], "a") as f:
        f.write(json.dumps({{"argv": argv, "tz": os.environ.get("TZ"), "cwd": os.getcwd(),
                            "path": os.environ.get("PATH", "").split(os.pathsep)[0],
                            "lock": os.path.isdir(os.environ["FAKE_LOCK"])}}) + "\\n")
    with open(os.path.join(opt("--user"), "ran.txt"), "a") as f:
        f.write("run\\n")
    if os.environ.get("FAKE_HANG") == str(rate):
        time.sleep(30)
    frames = int(opt("--frames"))
    with open(opt("--state-trace"), "w") as f:
        f.write("# statetrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\\n")
        for n in range(1, frames + 1):
            f.write("f=%d t=%d scene=0x1\\n" % (n, n))
    open(opt("--trace-ipc"), "w").write("# ipctrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\\n")
    for i, a in enumerate(argv):
        if a == "--capture-dsp":
            start, count, path = argv[i + 1].split(":", 2)
            with capfmt.Writer(path, "dsp") as w:
                w.dsp_regions(int(start), 0, capfmt.make_region(1), capfmt.make_region(0))
    sys.exit(int(os.environ.get("FAKE_EXIT_" + str(rate), "0")))
''')


@pytest.fixture
def fake(tmp_path, monkeypatch):
    runner = tmp_path / "fake_runner.py"
    runner.write_text(FAKE.format(tools=os.path.join(HERE, "..", "tools", "native")), encoding="utf-8")
    log = tmp_path / "fake.log"
    lock = tmp_path / "locks" / "emulator"
    monkeypatch.setenv("FAKE_LOG", str(log))
    monkeypatch.setenv("FAKE_LOCK", str(lock))
    game = tmp_path / "game"
    game.mkdir()
    code = tmp_path / "lib.dll"
    code.write_bytes(b"x")
    before = tmp_path / "user-before"
    (before / "nand").mkdir(parents=True)
    (before / "nand" / "save.bin").write_bytes(b"save")
    return {"runner": runner, "log": log, "lock": lock, "game": game, "code": code, "before": before}


def run_argv(tmp_path, fake, out, *extra, mode="azahar"):
    return ["run", str(write_session(tmp_path)), "--runner", str(fake["runner"]), "--program", str(fake["game"]),
            "--code", str(fake["code"]), "--user-before", str(fake["before"]), "--io-latency", mode,
            "--out", str(out), "--lock", str(fake["lock"]), *extra]


def calls(fake):
    return [json.loads(line) for line in fake["log"].read_text().splitlines()]


def test_run_plays_both_rates_from_fresh_copies(tmp_path, fake):
    out = tmp_path / "audit"
    tools = tmp_path / "toolchain"
    assert fps_audit.main(run_argv(tmp_path, fake, out, "--path", str(tools), "--dsp", "200:15", "--runner-arg=--gpu",
                                   "--runner-arg=null-display", "--runner-arg={out}/shots", mode="none")) == 0
    first, second = calls(fake)
    assert first["argv"][first["argv"].index("--set") + 1] == "0:fps=60"
    assert second["argv"][second["argv"].index("--set") + 1] == "0:fps=120"
    assert first["tz"] == "UTC0" and first["path"] == str(tools) and first["lock"] and second["lock"]
    assert not fake["lock"].exists()                                  # released
    assert os.path.realpath(first["cwd"]) == os.path.realpath(out / "r60")
    assert os.path.realpath(second["cwd"]) == os.path.realpath(out / "r120")
    for call, k, frames in ((first, 1, "300"), (second, 2, "600")):
        argv = call["argv"]
        assert argv[argv.index("--frames") + 1] == frames and argv[argv.index("--io-latency") + 1] == "none"
        assert argv[argv.index("--input-file") + 1].endswith("G9.txt" if k == 1 else "G9-x2.txt")
        starts = [argv[i + 1].split(":")[0] for i, a in enumerate(argv) if a == "--capture-dsp"]
        assert starts == [str(20 * k), str(200 * k)]
        assert "hunter_x" in open(argv[argv.index("--observable") + 1]).read()
        assert argv[-3:-1] == ["--gpu", "null-display"] and argv[-1] == str(out / f"r{60 * k}") + "/shots"
    # fresh copies of user-before, which is left alone
    assert (out / "r60" / "user" / "nand" / "save.bin").read_bytes() == b"save"
    assert (out / "r60" / "user" / "ran.txt").read_text() == "run\n" == (out / "r120" / "user" / "ran.txt").read_text()
    assert not (fake["before"] / "ran.txt").exists()
    meta = json.loads((out / "meta.json").read_text())
    assert meta["session"] == "G9" and meta["factor"] == 2 and meta["io_latency"] == "none"
    assert meta["checkpoints"] == [[60, 0x1000], [150, 0x2000], [300, 0x2000]]
    assert meta["dsp"] == ["dsp-20.mdsp", "dsp-200.mdsp"]
    assert [meta["runs"][r]["exit"] for r in ("60", "120")] == [0, 0]
    assert (out / "G9-x2.txt").read_text() == observable_probe.scale_session(SESSION, 2)
    assert (out / "r120" / "dsp-200.mdsp").is_file()


def test_run_reports_a_failing_runner_and_does_not_overwrite(tmp_path, fake, monkeypatch):
    out = tmp_path / "audit"
    monkeypatch.setenv("FAKE_EXIT_120", "3")
    assert fps_audit.main(run_argv(tmp_path, fake, out)) == 1
    meta = json.loads((out / "meta.json").read_text())
    assert [meta["runs"][r]["exit"] for r in ("60", "120")] == [0, 3]
    assert fps_audit.main(run_argv(tmp_path, fake, out)) == 2         # the runs are there: --force or another folder
    monkeypatch.delenv("FAKE_EXIT_120")
    assert fps_audit.main(run_argv(tmp_path, fake, out, "--force")) == 0
    assert not fake["lock"].exists()


def test_force_deletes_only_a_folder_that_run_wrote(tmp_path, fake, capsys):
    out = tmp_path / "audit"
    (out / "r60").mkdir(parents=True)                                  # a killed run: no meta.json yet
    (out / "r60" / "state.txt").write_text("", encoding="utf-8")
    assert fps_audit.main(run_argv(tmp_path, fake, out, "--force")) == 0
    assert (out / "meta.json").is_file()
    other = tmp_path / "work"
    (other / "r60").mkdir(parents=True)
    (other / "notes.txt").write_text("mine", encoding="utf-8")         # not a file of `run`
    capsys.readouterr()
    assert fps_audit.main(run_argv(tmp_path, fake, other, "--force")) == 2
    assert "also holds notes.txt" in capsys.readouterr().err
    assert (other / "notes.txt").read_text(encoding="utf-8") == "mine" and len(calls(fake)) == 2


def test_the_deterministic_mode_is_an_option_recorded_and_printed(tmp_path, fake, capsys):
    out = tmp_path / "audit"
    assert fps_audit.main(run_argv(tmp_path, fake, out, "--deterministic", "--seed", "3",
                                   "--runner-arg=--capture-hid")) == 0
    for call in calls(fake):
        argv = call["argv"]
        assert "--deterministic" in argv and argv[argv.index("--seed") + 1] == "3"
        assert argv.index("--deterministic") < argv.index("--capture-hid")
    meta = json.loads((out / "meta.json").read_text())
    assert meta["mode"] == "deterministic" and meta["seed"] == 3
    first = fps_audit.format_report(fps_audit.read_pair(out), [], False)[0]   # the fake's traces have no hunter
    assert first.endswith("--io-latency azahar, deterministic mode (--seed 3)")
    assert fps_audit.main(run_argv(tmp_path, fake, tmp_path / "b", "--seed", "3")) == 2      # a seed needs the mode


def test_compare_names_the_mode_of_an_older_meta_file(tmp_path):
    root = good_pair(tmp_path)
    pair = fps_audit.read_pair(root)
    assert fps_audit.format_report(pair, [], False)[0].endswith("--io-latency azahar, real-time mode")
    meta = json.loads((root / "meta.json").read_text())
    meta["runner_args"] = ["--deterministic", "--seed", "2"]                  # before `mode` was recorded
    (root / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
    pair = fps_audit.read_pair(root)
    assert fps_audit.format_report(pair, [], False)[0].endswith("deterministic mode (--seed 2)")


def test_the_default_timeout_is_an_hour_per_run():
    args = fps_audit.build_parser().parse_args(["run", "G1", "--runner", "r", "--program", "p", "--code", "c",
                                                "--user-before", "u", "--io-latency", "azahar", "--out", "o"])
    assert args.timeout == 3600 and not args.deterministic and args.seed is None


def test_run_times_out_a_hanging_runner_and_compare_fails_it(tmp_path, fake, monkeypatch, capsys):
    out = tmp_path / "audit"
    monkeypatch.setenv("FAKE_HANG", "120")
    assert fps_audit.main(run_argv(tmp_path, fake, out, "--timeout", "2")) == 1
    meta = json.loads((out / "meta.json").read_text())
    assert meta["runs"]["120"]["exit"] == 124 and meta["runs"]["120"]["timed_out"] is True
    assert "timed out" in (out / "r120" / "runner.log").read_text()
    capsys.readouterr()
    assert fps_audit.main(["compare", str(out), "--brief"]) == 1
    text = capsys.readouterr().out
    assert "FAIL" in text and "timed out" in text


@pytest.mark.parametrize("missing", ["runner", "program", "code", "user_before"])
def test_run_refuses_what_it_cannot_find(tmp_path, fake, missing, capsys):
    out = tmp_path / "audit"
    argv = run_argv(tmp_path, fake, out)
    flag = {"runner": "--runner", "program": "--program", "code": "--code", "user_before": "--user-before"}[missing]
    argv[argv.index(flag) + 1] = str(tmp_path / "nowhere")
    assert fps_audit.main(argv) == 2
    assert "not found" in capsys.readouterr().err and not fake["log"].exists()


def test_run_refuses_a_bad_io_latency_and_factor(tmp_path, fake):
    with pytest.raises(SystemExit):
        fps_audit.main(run_argv(tmp_path, fake, tmp_path / "a", mode="fast"))
    assert fps_audit.main(run_argv(tmp_path, fake, tmp_path / "b") + ["--factor", "1"]) == 2


# -- compare: the runs ----------------------------------------------------------------------------------------------

def test_a_good_pair_passes_every_item(tmp_path):
    items = audit(good_pair(tmp_path), position_until=250, require=["position", "quest_time", "dsp", "writes"])
    assert {n: i.status for n, i in items.items()} == {
        n: "pass" for n in ("runs", "scenes", "times", "checkpoints", "observables", "dsp", "writes")}, \
        {n: i.summary for n, i in items.items()}


def test_compare_exit_codes_and_files(tmp_path, capsys):
    root = good_pair(tmp_path)
    assert fps_audit.main(["compare", str(root), "--fs-write-mask", "none"]) == 0
    out = capsys.readouterr().out
    assert out.splitlines()[0].startswith("fps_audit: G9, 60 fps against 120 fps (x2), --io-latency azahar")
    assert out.splitlines()[-1].startswith("verdict: pass;")
    assert (root / "compare.txt").is_file()
    data = json.loads((root / "compare.json").read_text())
    tol = data["tolerances"]
    assert tol["time_frames"] is None and tol["time_default_frames"] == 4            # D70's table, no override
    assert tol["position_relative"] == 0 and tol["position_absolute"] == 3700 and tol["quest_seconds"] == 1.09
    assert tol["max_shift"] == 48
    assert data["mode"] == "real-time" and data["seed"] is None and data["verdict"] == "pass"
    assert [i["name"] for i in data["items"]][:2] == ["runs", "scenes"]
    bad = make_pair(sub(tmp_path, "bad"), scenes_scaled=shifted(40))
    assert fps_audit.main(["compare", str(bad), "--fs-write-mask", "none", "--brief"]) == 1
    assert "verdict: FAIL (times)" in capsys.readouterr().out


def test_compare_usage_errors(tmp_path, capsys):
    assert fps_audit.main(["compare", str(tmp_path / "none")]) == 2
    root = good_pair(tmp_path)
    assert fps_audit.main(["compare", str(root), "--require", "scenes"]) == 2
    assert fps_audit.main(["compare", str(root), "--hunt-scene", "scene"]) == 2
    assert "fps_audit:" in capsys.readouterr().err
    (root / "meta.json").write_text("{}", encoding="utf-8")
    assert fps_audit.main(["compare", str(root)]) == 2


def test_runs_item_both_ways(tmp_path):
    assert audit(good_pair(tmp_path))["runs"].status == "pass"
    bad = audit(good_pair(sub(tmp_path, "x"), exit_codes=(0, 1)))
    assert bad["runs"].status == "fail" and "exit status 1" in bad["runs"].summary


def test_a_run_that_stopped_early_fails_the_runs_item(tmp_path):
    items = audit(good_pair(tmp_path, frames_scaled=500))
    assert items["runs"].status == "fail" and "stopped at frame 500" in items["runs"].summary


def test_a_missing_state_trace_stops_the_comparison_at_the_runs_item(tmp_path):
    root = good_pair(tmp_path)
    (root / "r120" / "state.txt").unlink()
    items = audit(root)
    assert list(items) == ["runs"] and items["runs"].status == "fail" and "no state trace" in items["runs"].summary


# -- item 1: the scene sequence -------------------------------------------------------------------------------------

def test_scenes_item_both_ways(tmp_path):
    ok = audit(good_pair(tmp_path))["scenes"]
    assert ok.status == "pass" and ok.summary.startswith("2 real scene changes")
    swapped = [(10, 0x1000), (240, HUNT), (400, 0x2000)]                 # another scene at the first real change
    items = audit(good_pair(sub(tmp_path, "b"), scenes_scaled=swapped))
    assert items["scenes"].status == "fail" and "differ at change #0" in items["scenes"].summary
    assert items["times"].status == "n/a"


def test_scenes_item_fails_on_an_extra_change_and_on_none(tmp_path):
    extra = shifted(0) + [(500, 0x4000)]
    items = audit(good_pair(sub(tmp_path, "a"), scenes_scaled=extra))
    assert items["scenes"].status == "fail"
    assert "(2 changes at 60 fps, 3 at 120 fps)" in items["scenes"].summary
    flat = [(1, 0x1000)]
    items = audit(good_pair(sub(tmp_path, "b"), scenes=flat, scenes_scaled=flat, checkpoints=()))
    assert items["scenes"].status == "fail" and "nothing to compare" in items["scenes"].summary


# -- item 2: the game time of each scene change ---------------------------------------------------------------------

def test_times_item_passes_at_exactly_four_frames_and_fails_beyond(tmp_path):
    ok = audit(good_pair(sub(tmp_path, "a"), scenes_scaled=shifted(8)))["times"]       # 8 frames at 120 fps = 4 at 60
    assert ok.status == "pass" and "66.7 ms" in ok.summary
    bad = audit(good_pair(sub(tmp_path, "b"), scenes_scaled=shifted(9)))["times"]      # 4.5 frames
    assert bad.status == "fail" and "changes #0" in bad.summary and "OVER" in "\n".join(bad.details)
    early = audit(good_pair(sub(tmp_path, "c"), scenes_scaled=shifted(-9)))["times"]   # earlier counts the same
    assert early.status == "fail"


def test_times_tolerance_is_an_option_and_the_spread_is_reported(tmp_path):
    root = good_pair(tmp_path, scenes_scaled=shifted(9))
    wide = audit(root, time_frames=5)["times"]
    assert wide.status == "pass"
    assert wide.data["worst_ms"] == pytest.approx(75.0) and wide.data["limit_frames"] == 5
    assert [c["ms"] for c in wide.data["changes"]] == [75.0, 0.0]


def test_times_reports_the_game_clock_beside_the_frames(tmp_path):
    # the same frame / fps, but the 120 fps run's clock is 4 s ahead from the first scene change on (N3-22's finding)
    root = good_pair(tmp_path, clock=(None, lambda f: f / 120 + (4.0 if f >= 240 else 0.0)))
    item = audit(root)["times"]
    assert item.status == "pass"
    offsets = [c["clock_offset_s"] for c in item.data["changes"]]
    assert offsets == [pytest.approx(4.0, abs=0.01), pytest.approx(4.0, abs=0.01)]
    assert "clock" in "\n".join(item.details)


# -- item 3: the eq observables at the checkpoints ------------------------------------------------------------------

def test_checkpoints_item_both_ways(tmp_path):
    ok = audit(good_pair(tmp_path))["checkpoints"]
    assert ok.status == "pass" and "quest_id" in ok.summary
    bad = audit(good_pair(sub(tmp_path, "a"), quest_id=(lambda f: 7, lambda f: 7 if f < 200 else 9)))["checkpoints"]
    assert bad.status == "fail" and "frames 150, 300" in bad.summary          # the 120 fps run differs from frame 200
    assert any("quest_id 0x00000007 | 0x00000009" in d for d in bad.details)


def test_checkpoints_compare_the_eq_values_at_the_scaled_frame(tmp_path):
    # quest_id changes at frame 150 at 60 fps and frame 300 at 120 fps: equal at the checkpoint (150, 300)
    ok = audit(good_pair(sub(tmp_path, "a"), quest_id=(lambda f: 1 if f < 150 else 2,
                                                       lambda f: 1 if f < 300 else 2)))["checkpoints"]
    assert ok.status == "pass"
    late = audit(good_pair(sub(tmp_path, "b"), quest_id=(lambda f: 1 if f < 150 else 2,
                                                         lambda f: 1 if f < 301 else 2)))["checkpoints"]
    assert late.status == "fail" and "frames 150" in late.summary


def test_checkpoints_fail_when_the_scene_is_not_the_checkpoints(tmp_path):
    item = audit(good_pair(tmp_path, checkpoints=((150, 0x7777),)))["checkpoints"]
    assert item.status == "fail" and "the checkpoint wants 0x00007777" in "\n".join(item.details)


def test_checkpoints_fail_when_a_run_never_reaches_one(tmp_path):
    item = audit(good_pair(tmp_path, checkpoints=((150, 0x2000), (900, HUNT))))["checkpoints"]
    assert item.status == "fail" and "no record" in "\n".join(item.details)


# -- item 4: the observables at matched times -----------------------------------------------------------------------

def test_position_within_the_tolerance_passes_and_beyond_fails(tmp_path):
    # 3 units per second travelled: the limit is 0.02 * 3 + 0.05 = 0.11
    ok = audit(good_pair(sub(tmp_path, "a"), pos=(walk(60), walk(120, 0.10))), **FIRST_GUESSES)["observables"]
    assert ok.data["position"]["status"] == "pass" and ok.status == "pass"
    bad = audit(good_pair(sub(tmp_path, "b"), pos=(walk(60), walk(120, 0.12))), **FIRST_GUESSES)["observables"]
    assert bad.status == "fail" and bad.data["position"]["status"] == "fail"
    assert "first over at" in bad.data["position"]["summary"]
    assert bad.data["quest_time"]["status"] == "pass"                      # the other value is judged on its own


def test_position_limit_follows_the_distance_travelled(tmp_path):
    # a hunter that stands still gets the absolute 0.05 only; one that runs 100 units a second gets 2.05
    def stand(offset=0.0):
        return lambda f: (5.0 + offset, 1.0, 2.0)

    def run(rate, offset=0.0):
        return lambda f: (f / rate * 100.0 + offset, 1.0, 2.0)

    def status(name, pos):
        return audit(good_pair(sub(tmp_path, name), pos=pos), **FIRST_GUESSES)["observables"].data["position"]["status"]

    assert status("a", (stand(), stand(0.06))) == "fail"
    assert status("b", (stand(), stand(0.04))) == "pass"
    assert status("c", (run(60), run(120, 2.0))) == "pass"
    assert status("d", (run(60), run(120, 2.2))) == "fail"


def test_position_options_are_the_tolerances(tmp_path):
    root = good_pair(tmp_path, pos=(walk(60), walk(120, 0.2)))
    assert audit(root, **FIRST_GUESSES)["observables"].data["position"]["status"] == "fail"
    wider = audit(root, position_relative=0.02, position_absolute=0.2)["observables"]
    assert wider.data["position"]["status"] == "pass"
    relative = audit(root, position_relative=0.06, position_absolute=0.05)["observables"]           # 0.18 + 0.05
    assert relative.data["position"]["status"] == "pass"


def test_quest_time_within_one_second_passes_and_beyond_fails(tmp_path):
    ok = audit(good_pair(sub(tmp_path, "a"), quest=(countdown(60), countdown(120, 60))),
               quest_seconds=1.0)["observables"]                                     # 60 units = 1 s
    assert ok.data["quest_time"]["status"] == "pass"
    root = good_pair(sub(tmp_path, "b"), quest=(countdown(60), countdown(120, -61)))
    bad = audit(root, quest_seconds=1.0)["observables"]
    assert bad.status == "fail" and bad.data["quest_time"]["status"] == "fail"
    assert "limit 1.0 s" in bad.data["quest_time"]["summary"] and bad.data["position"]["status"] == "pass"
    assert audit(root, quest_seconds=1.1)["observables"].status == "pass"
    assert audit(root, quest_seconds=1.0, quest_units=120)["observables"].status == "pass"   # 61 units: half a second


def test_the_observables_are_compared_in_the_hunt_only(tmp_path):
    # the position differs wildly before the hunt (frame 200 at 60 fps): not a position there (N3-21)
    def before_the_hunt(f):
        return (f / 120 * 3.0 + (500.0 if f < 400 else 0.0), 1.0, 2.0)

    root = good_pair(tmp_path, pos=(walk(60), before_the_hunt))
    assert audit(root, **FIRST_GUESSES)["observables"].data["position"]["status"] == "pass"
    assert audit(root, hunt_scene=None, **FIRST_GUESSES)["observables"].data["position"]["status"] == "fail"
    assert audit(root)["observables"].data["position"]["rows"][0]["second"] == 4     # frame 240 is the first in the hunt


def test_unreadable_observables_are_not_applicable_unless_required(tmp_path):
    root = make_pair(tmp_path)
    item = audit(root)["observables"]
    assert item.status == "n/a" and item.data["position"]["status"] == "n/a"
    required = audit(root, require=["position", "quest_time"])["observables"]
    assert required.status == "fail" and "(required)" in required.data["position"]["summary"]
    assert audit(root, require=["position"])["observables"].data["quest_time"]["status"] == "n/a"


def test_a_quest_counter_that_never_moves_compares_nothing(tmp_path):
    constant = lambda f: 7           # noqa: E731
    root = good_pair(tmp_path, quest=(constant, constant))
    item = audit(root)["observables"]
    assert item.data["quest_time"]["status"] == "n/a" and "constant" in item.data["quest_time"]["summary"]
    assert item.status == "pass" and item.data["position"]["status"] == "pass"
    required = audit(root, require=["quest_time"])["observables"]
    assert required.status == "fail" and "(required)" in required.data["quest_time"]["summary"]
    moving = audit(good_pair(sub(tmp_path, "m"), quest=(constant, countdown(120))))["observables"]
    assert moving.data["quest_time"]["status"] == "fail"             # one side moves: the difference is judged


def test_two_constant_quest_counters_further_apart_than_the_tolerance_fail(tmp_path):
    # D70's 1.09 s: 65 units (1.083 s) are within it, 66 (1.1 s) beyond
    near = audit(good_pair(sub(tmp_path, "near"), quest=(lambda f: 700, lambda f: 765)))["observables"]
    assert near.data["quest_time"]["status"] == "n/a"
    assert "65 units = 1.083 s apart (limit 1.09 s)" in near.data["quest_time"]["summary"]
    far_root = good_pair(sub(tmp_path, "far"), quest=(lambda f: 700, lambda f: 766))
    far = audit(far_root)["observables"]
    assert far.status == "fail" and far.data["quest_time"]["status"] == "fail"
    assert "over the quest-clock tolerance" in far.data["quest_time"]["summary"]
    assert "0x" not in far.data["quest_time"]["summary"]             # the difference is printed, not the values
    assert audit(far_root, quest_seconds=1.2)["observables"].data["quest_time"]["status"] == "n/a"
    # the bound is inclusive: exactly at the limit (60 units = 1 s against 1 s) is within it, one unit more is over
    exact = audit(good_pair(sub(tmp_path, "exact"), quest=(lambda f: 700, lambda f: 760)), quest_seconds=1.0)
    assert exact["observables"].data["quest_time"]["status"] == "n/a"
    assert "60 units = 1 s apart (limit 1.0 s)" in exact["observables"].data["quest_time"]["summary"]
    past = audit(good_pair(sub(tmp_path, "past"), quest=(lambda f: 700, lambda f: 761)), quest_seconds=1.0)
    assert past["observables"].data["quest_time"]["status"] == "fail"


def test_a_position_that_is_not_a_position_fails(tmp_path):
    far = lambda f: (1.0e7, 0.0, 0.0)           # noqa: E731
    root = good_pair(tmp_path, pos=(far, far))
    assert audit(root)["observables"].data["position"]["status"] == "fail"


def test_position_until_reports_the_part_before_the_first_attack(tmp_path):
    def diverge(f):                    # equal until 5 s, then the 120 fps hunter is elsewhere
        return (f / 120 * 3.0 + (400.0 if f / 120 > 5.0 else 0.0), 1.0, 2.0)

    root = good_pair(tmp_path, pos=(walk(60), diverge), frames=600)
    item = audit(root, position_until=290, **FIRST_GUESSES)["observables"]
    assert item.data["position"]["status"] == "fail"
    early = item.data["position_until"]
    assert early["frame"] == 290 and early["over"] == 0 and early["seconds"] == 1


def test_a_jump_that_comes_a_few_frames_early_is_one_sample_off_and_the_slack_column_says_so(tmp_path):
    # the hunter jumps 500 units at 6 s of game time; at 120 fps the jump comes 2 frames of the 60 fps clock early
    def jump(rate, early_frames):
        edge = 6 * rate - early_frames * rate // 60
        return lambda f: (500.0 if f > edge else 0.0, 1.0, 2.0)

    root = good_pair(tmp_path, pos=(jump(60, 0), jump(120, 2)), frames=600)
    item = audit(root, **FIRST_GUESSES)["observables"]
    row = [r for r in item.data["position"]["rows"] if r["second"] == 6][0]
    assert item.data["position"]["status"] == "fail" and row["over"]       # the rule is the pointwise one
    assert row["distance"] == pytest.approx(500.0) and row["slack_distance"] == 0.0
    assert all(r["slack_ratio"] <= 1 for r in item.data["position"]["rows"])
    assert "time slack" in item.data["position"]["summary"]


def test_the_spread_is_in_the_rows(tmp_path):
    root = good_pair(tmp_path, pos=(walk(60), walk(120, 0.05)), quest=(countdown(60), countdown(120, 30)))
    item = audit(root, **FIRST_GUESSES)["observables"]
    row = item.data["position"]["rows"][0]
    assert row["distance"] == pytest.approx(0.05, abs=1e-3) and row["limit"] == pytest.approx(0.11, abs=1e-3)
    assert item.data["quest_time"]["rows"][0]["seconds"] == pytest.approx(0.5)


# -- item 5: the DSP capture ----------------------------------------------------------------------------------------

def test_dsp_item_passes_an_aligned_pair_and_prints_the_shift(tmp_path):
    item = audit(good_pair(tmp_path))["dsp"]
    assert item.status == "pass" and "+17 DSP frames" in item.summary and item.data["shifts"] == [17]
    assert "status blocks compared" in "\n".join(item.details)


def test_dsp_item_fails_a_shift_over_the_bound_and_takes_it_as_an_option(tmp_path):
    root = good_pair(tmp_path, dsp=((100, 100, 40), (100 + 49 - 10, 90, 40)))
    bad = audit(root)["dsp"]
    assert bad.status == "fail" and "more than the 48 allowed" in "\n".join(bad.details)
    assert audit(root, max_shift=49)["dsp"].status == "pass"


def test_dsp_item_fails_a_status_block_that_differs(tmp_path):
    def tweak(i, region, block):
        return capfmt.make_status(enabled=1, sync_count=5, buffer_position=7, current_buffer_id=3,
                                  last_buffer_id=2) if i == 20 else block

    root = good_pair(tmp_path)
    dsp_file(root / "r120" / "dsp-20.mdsp", 100 + 17 - 10, 90, 40, tweak=tweak)
    bad = audit(root)["dsp"]
    assert bad.status == "fail" and "DIFFERENT" in "\n".join(bad.details)


def test_dsp_item_without_a_capture_is_not_applicable_unless_required(tmp_path):
    root = make_pair(tmp_path)
    assert audit(root)["dsp"].status == "n/a"
    required = audit(root, require=["dsp"])["dsp"]
    assert required.status == "fail" and "(required)" in required.summary


def test_dsp_item_fails_a_capture_a_run_did_not_write(tmp_path):
    root = good_pair(tmp_path)
    (root / "r120" / "dsp-20.mdsp").unlink()
    assert audit(root)["dsp"].status == "fail"
    root2 = good_pair(sub(tmp_path, "b"))
    (root2 / "r60" / "dsp-20.mdsp").write_bytes(b"junk")
    assert audit(root2)["dsp"].status == "fail"


def test_dsp_item_names_the_clock_offset_at_the_start_of_the_window_as_information(tmp_path):
    # the 120 fps run's game clock is ahead by 4 s at the capture's game frame; the audio's measured shift is +17
    # DSP frames: the offset is printed beside the shift, never as the shift (N3-22: shifts were 11-85 % of it)
    root = good_pair(tmp_path, clock=(None, lambda f: f / 120 + 4.0))
    item = audit(root)["dsp"]
    text = "\n".join(item.details)
    assert "+4.000 s = +818 DSP frames, as information (the measured shift is +17 DSP frames)" in text
    assert "differs by that much" not in text
    assert item.status == "pass" and item.data["shifts"] == [17]      # the offset does not enter the rule


def test_dsp_item_says_when_no_shift_was_measured(tmp_path):
    def silent(i, region, block):
        return capfmt.make_status(enabled=0, sync_count=0, buffer_position=0, current_buffer_id=0, last_buffer_id=0)

    root = good_pair(tmp_path, clock=(None, lambda f: f / 120 + 4.0))
    dsp_file(root / "r120" / "dsp-20.mdsp", 100 + 17 - 10, 90, 40, tweak=silent)     # nothing to align on
    item = audit(root)["dsp"]
    assert item.status == "fail"
    assert "+818 DSP frames, as information (no shift was measured)" in "\n".join(item.details)


# -- item 6: the fs: Writes -----------------------------------------------------------------------------------------

def masks(tmp_path, line="G9 32 0 4 4  a field derived from the clock"):
    path = tmp_path / "masks.txt"
    path.write_text(line + "\n", encoding="utf-8")
    return str(path)


def with_byte(index, value):
    data = bytearray(BLOB)
    data[index] = value
    return bytes(data)


def test_writes_item_passes_equal_writes_whatever_the_reads(tmp_path):
    root = good_pair(tmp_path)
    ipc_trace(root / "r60" / "ipc.txt", writes=[(0, BLOB), (64, b"abcd")], reads=[(0, 16, 1), (16, 16, 2)])
    ipc_trace(root / "r120" / "ipc.txt", writes=[(0, BLOB), (64, b"abcd")], reads=[(16, 16, 2), (900, 8, 9)])
    item = audit(root)["writes"]
    assert item.status == "pass" and "2 Writes" in item.summary


@pytest.mark.parametrize("label,other", [("bytes", [(0, with_byte(10, 99))]), ("offset", [(8, BLOB)]),
                                         ("fewer", []), ("more", [(0, BLOB), (0, BLOB)])])
def test_writes_item_fails_other_bytes_another_offset_and_another_count(tmp_path, label, other):
    root = good_pair(tmp_path)
    ipc_trace(root / "r120" / "ipc.txt", writes=other)
    item = audit(root)["writes"]
    assert item.status == "fail" and "Write" in item.summary


def test_writes_item_applies_the_write_masks_of_the_base_session(tmp_path):
    root = good_pair(sub(tmp_path, "ok"), writes=([(0, with_byte(5, 1))], [(0, with_byte(6, 2))]))
    assert audit(root)["writes"].status == "fail"
    masked = audit(root, masks_file=masks(tmp_path))["writes"]
    assert masked.status == "pass" and "1 masked" in masked.summary
    outside = good_pair(sub(tmp_path, "outside"), writes=([(0, with_byte(5, 1))], [(0, with_byte(9, 2))]))   # byte 9
    assert audit(outside, masks_file=masks(tmp_path))["writes"].status == "fail"
    scaled_name = good_pair(sub(tmp_path, "named"), session="G9-x2",
                            writes=([(0, with_byte(5, 1))], [(0, with_byte(6, 2))]))
    assert audit(scaled_name, masks_file=masks(tmp_path))["writes"].status == "pass"       # the lines of `G9`


def test_writes_item_fails_a_mask_that_matches_no_write(tmp_path):
    root = good_pair(tmp_path)
    item = audit(root, masks_file=masks(tmp_path, "G9 99 0 4 4  matches nothing"))["writes"]
    assert item.status == "fail" and "matches no Write" in item.summary
    other = audit(root, masks_file=masks(tmp_path, "G2 99 0 4 4  another session's line"))["writes"]
    assert other.status == "pass"


def test_writes_item_without_any_write_is_not_applicable_unless_required(tmp_path):
    root = good_pair(tmp_path, writes=([], []))
    assert audit(root)["writes"].status == "n/a"
    required = audit(root, require=["writes"])["writes"]
    assert required.status == "fail" and "(required)" in required.summary


def test_writes_item_fails_a_trace_that_is_not_one(tmp_path):
    root = good_pair(tmp_path)
    (root / "r120" / "ipc.txt").write_text("not a trace\n", encoding="utf-8")
    assert audit(root)["writes"].status == "fail"
    (root / "r120" / "ipc.txt").unlink()
    assert audit(root)["writes"].status == "fail"


# -- the report -----------------------------------------------------------------------------------------------------

def test_the_report_names_the_failing_and_not_applicable_items(tmp_path):
    root = make_pair(tmp_path, scenes_scaled=shifted(20))
    pair = fps_audit.read_pair(root)
    items = fps_audit.compare_pair(pair, masks_file=None)
    lines = fps_audit.format_report(pair, items, verbose=False)
    assert lines[-1].startswith("verdict: FAIL (times)") and "not applicable" in lines[-1]
    assert not any(line.startswith("    ") for line in lines)
    assert any(line.startswith("times") and "FAIL" in line for line in lines)


# -- D70's rules (N3-34) --------------------------------------------------------------------------------------------

@pytest.mark.parametrize("session,change,by,limit,status", [
    ("G2", 0, 8, 4, "pass"), ("G2", 0, 9, 4, "fail"),            # every other change: 4 frames
    ("G2", 1, 40, 20, "pass"), ("G2", 1, 41, 20, "fail"),        # the first aDemo: 20 frames
    ("G1", 1, 40, 20, "pass"), ("G4", 1, 41, 20, "fail"),
    ("G3", 3, 20, 10, "pass"), ("G4", 5, 21, 10, "fail"),        # the three changes into the village: 10 frames
    ("G2", 4, 20, 10, "pass"), ("G3", 5, 21, 10, "fail"),
    ("G2", 7, 38, 19, "pass"), ("G2", 6, 39, 19, "fail"),        # G2's opening: 19 frames
    ("G3", 6, 9, 4, "fail"), ("G4", 7, 9, 4, "fail"),            # not G3's nor G4's
    ("G3", 8, 11, 5.5, "pass"), ("G3", 9, 12, 5.5, "fail"),      # G3's aGameSys and aQuest of the quest load: 5.5
    ("G2", 8, 11, 4, "fail"),
    ("G6", 1, 9, 4, "fail"), ("G6", 3, 8, 4, "pass"),            # a session the table does not name: 4 frames
])
def test_each_scene_change_has_its_own_limit(tmp_path, session, change, by, limit, status):
    root = make_pair(tmp_path, scenes=chain(10), scenes_scaled=chain_scaled(10, change, by), checkpoints=(),
                     session=session, mode="deterministic")
    item = audit(root)["times"]
    assert item.status == status, item.summary
    row = item.data["changes"][change]
    assert row["limit_frames"] == limit and row["limit_ms"] == pytest.approx(limit * 1000 / 60, abs=0.01)
    assert row["over"] == (status == "fail")
    assert all(r["limit_frames"] == 4 for r in item.data["changes"] if r["index"] in (0, 2))


def test_compare_txt_prints_each_changes_limit_and_compare_json_has_it(tmp_path):
    root = make_pair(tmp_path, scenes=chain(10), scenes_scaled=chain_scaled(10, 1, 40), checkpoints=(),
                     session="G3", mode="deterministic")
    assert compare_main(root, "--require", "") == 0               # G3's own --require would fail the n/a observables
    text = (root / "compare.txt").read_text(encoding="utf-8")
    assert "333.3 ms, 20.00 frames at 60 fps, limit 20 frames (333.3 ms)" in text
    assert "limit 5.5 frames (91.7 ms)" in text and "limit 4 frames (66.7 ms)" in text
    data = compare_json(root)
    times = next(i for i in data["items"] if i["name"] == "times")
    assert [c["limit_frames"] for c in times["data"]["changes"]] == [4, 20, 4, 10, 10, 10, 4, 4, 5.5, 5.5]
    assert all(k in times["data"]["changes"][0] for k in ("index", "frame", "frame_scaled", "ms", "frames_at_60",
                                                         "clock_offset_s", "limit_ms", "over"))
    assert data["tolerances"]["time_table"] == {"1": 20, "3": 10, "4": 10, "5": 10, "8": 5.5, "9": 5.5}
    assert {i["name"]: i["gated"] for i in data["items"]} == {
        "runs": True, "scenes": True, "times": True, "checkpoints": True, "observables": True, "dsp": False,
        "writes": True}


def test_time_frames_replaces_the_table_and_the_verdict_says_so(tmp_path, capsys):
    root = make_pair(tmp_path, scenes=chain(10), scenes_scaled=chain_scaled(10, 1, 40), checkpoints=(),
                     session="G2", mode="deterministic")
    assert compare_main(root) == 0                                 # 20 frames: within the first aDemo's limit
    assert "--time-frames" not in capsys.readouterr().out.splitlines()[-1]
    assert compare_main(root, "--time-frames", "4") == 1
    verdict = capsys.readouterr().out.splitlines()[-1]
    assert verdict.startswith("verdict: FAIL (times)") and "--time-frames 4 for every change" in verdict
    data = compare_json(root)
    assert data["tolerances"]["time_frames"] == 4
    times = next(i for i in data["items"] if i["name"] == "times")
    assert all(c["limit_frames"] == 4 for c in times["data"]["changes"])
    assert compare_main(root, "--time-frames", "20") == 0
    assert audit(root, time_frames=19.5)["times"].status == "fail"


def test_g3_in_real_time_reports_a_different_sequence_where_g1_and_an_unnamed_session_fail(tmp_path, capsys):
    swapped = [(10, 0x1000), (240, HUNT), (400, 0x2000)]
    g3 = good_pair(sub(tmp_path, "g3"), scenes_scaled=swapped, session="G3", mode="real-time")
    assert compare_main(g3) == 0
    out = capsys.readouterr().out
    lines = item_lines(out)
    assert "FAIL" in lines["scenes"] and "reported" in lines["scenes"] and "gated" not in lines["scenes"]
    assert "gated" in lines["runs"] and "gated" in lines["writes"]
    verdict = out.splitlines()[-1]
    assert verdict.startswith("verdict: pass (reported: scenes") and "2 gated, 5 reported" in verdict
    assert {i["name"]: i["gated"] for i in compare_json(g3)["items"]} == {
        "runs": True, "scenes": False, "times": False, "checkpoints": False, "observables": False, "dsp": False,
        "writes": True}
    for name in ("G1", "G6"):                                     # G6 takes G1's rules
        root = good_pair(sub(tmp_path, name), scenes_scaled=swapped, session=name, mode="real-time")
        assert compare_main(root) == 1
        out = capsys.readouterr().out
        assert out.splitlines()[-1].startswith("verdict: FAIL (scenes") and "gated" in item_lines(out)["scenes"]
    # the deterministic mode gates G3's sequence
    det = good_pair(sub(tmp_path, "g3det"), scenes_scaled=swapped, session="G3", mode="deterministic")
    assert compare_main(det) == 1


def test_real_time_gates_g1_g2_and_g4_but_reports_their_observables(tmp_path, capsys):
    for name in ("G1", "G2", "G4"):
        root = good_pair(sub(tmp_path, name), session=name, mode="real-time",
                         pos=(walk(60), walk(120, 4000.0)))        # far beyond 3,700 units
        assert compare_main(root) == 0
        out = capsys.readouterr().out
        assert "reported" in item_lines(out)["observables"] and "FAIL" in item_lines(out)["observables"]
        assert out.splitlines()[-1].startswith("verdict: pass (reported: observables")
        det = good_pair(sub(tmp_path, name + "det"), session=name, mode="deterministic",
                        pos=(walk(60), walk(120, 4000.0)))
        assert compare_main(det) == 1                              # gated in the deterministic mode
        capsys.readouterr()


def test_a_none_folder_reports_every_item_and_exits_0(tmp_path, capsys):
    root = good_pair(tmp_path, session="G4", mode="deterministic", io_latency="none", scenes_scaled=shifted(40),
                     dsp=((100, 100, 40), (100 + 70 - 10, 90, 40)), exit_codes=(0, 1),
                     writes=([(0, BLOB)], [(0, with_byte(3, 9))]))
    assert compare_main(root) == 0
    out = capsys.readouterr().out
    lines = item_lines(out)
    assert set(lines) == set(fps_audit.ITEMS)
    assert all("reported" in line and "gated" not in line for line in lines.values())
    verdict = out.splitlines()[-1]
    assert verdict.startswith("verdict: pass (reported: runs, times, dsp, writes)") and "0 gated, 7 reported" in verdict
    assert "every item reported" in out.splitlines()[1]
    data = compare_json(root)
    assert not any(i["gated"] for i in data["items"]) and data["verdict"] == "pass"
    assert data["reported_failures"] == ["runs", "times", "dsp", "writes"]


@pytest.mark.parametrize("mode,shift,status", [
    ("deterministic", 57, "pass"), ("deterministic", 58, "pass"), ("deterministic", 59, "fail"),
    ("real-time", 48, "pass"), ("real-time", 49, "fail"),
])
def test_the_dsp_bound_of_g1s_boot_window_follows_the_mode(tmp_path, mode, shift, status):
    root = good_pair(tmp_path, session="G1", mode=mode, dsp_start=1240,
                     dsp=((100, 100, 40), (100 + shift - 10, 90, 40)))
    item = audit(root)["dsp"]
    assert item.gated and item.status == status and item.data["shifts"] == [shift]
    assert item.data["max_shift"] == (58 if mode == "deterministic" else 48)
    assert compare_main(root) == (0 if status == "pass" else 1)
    assert compare_json(root)["tolerances"]["max_shift"] == (58 if mode == "deterministic" else 48)
    assert compare_main(root, "--max-shift", str(shift)) == 0          # the option overrides the bound


def test_any_other_dsp_window_is_reported_with_the_clock_offset_at_its_start(tmp_path, capsys):
    over = ((100, 100, 40), (100 + 49 - 10, 90, 40))
    # G1: the boot window gates and passes, a window at frame 200 is reported, over the bound, with its offset
    root = good_pair(tmp_path, session="G1", mode="real-time", dsp_start=1240, dsp=ALIGNED, dsp_windows={200: over},
                     clock=(None, lambda f: f / 120 + 4.0))
    item = audit(root)["dsp"]
    assert item.gated and item.status == "pass" and item.reported_fail == ["dsp-200.mdsp"]
    text = "\n".join(item.details)
    assert "dsp-1240.mdsp (gated)" in text and "dsp-200.mdsp (reported)" in text
    assert "+4.000 s = +818 DSP frames, as information (the measured shift is +49 DSP frames)" in text
    assert compare_main(root) == 0
    assert capsys.readouterr().out.splitlines()[-1].startswith("verdict: pass (reported: dsp (dsp-200.mdsp))")
    # G2's and G3's windows never gate, in either mode
    for name in ("G2", "G3"):
        other = good_pair(sub(tmp_path, name), session=name, mode="deterministic", dsp_start=6000,
                          dsp=((100, 100, 40), (100 + 70 - 10, 90, 40)))
        item = audit(other, require=[])["dsp"]
        assert not item.gated and item.status == "fail"
        assert compare_main(other) == 0
        assert "dsp" in capsys.readouterr().out.splitlines()[-1]


@pytest.mark.parametrize("offset,status", [(3699.5, "pass"), (3700.5, "fail")])
def test_the_position_bound_is_3700_units_with_no_relative_part(tmp_path, offset, status):
    def run(rate, shift=0.0):
        return lambda f: (f / rate * 100.0 + shift, 1.0, 2.0)          # 100 units a second: no extra allowance

    root = good_pair(tmp_path, session="G3", mode="deterministic", pos=(run(60), run(120, offset)))
    item = audit(root)["observables"]
    assert item.gated and item.data["position"]["status"] == status
    assert item.data["position"]["rows"][0]["limit"] == 3700
    assert compare_main(root) == (0 if status == "pass" else 1)


@pytest.mark.parametrize("units,status", [(65, "pass"), (66, "fail")])
def test_the_quest_clock_bound_is_1_09_seconds(tmp_path, units, status):
    root = good_pair(tmp_path, session="G3", mode="deterministic", quest=(countdown(60), countdown(120, units)))
    item = audit(root)["observables"]
    assert item.gated and item.data["quest_time"]["status"] == status
    assert "limit 1.09 s" in item.data["quest_time"]["summary"]
    assert compare_main(root) == (0 if status == "pass" else 1)


def test_g3s_require_and_position_until_are_its_defaults(tmp_path):
    root = make_pair(tmp_path, session="G3", mode="deterministic")      # no hunter, no DSP capture
    items = audit(root)
    assert items["observables"].status == "fail" and "(required)" in items["observables"].data["position"]["summary"]
    assert items["dsp"].status == "fail" and not items["dsp"].gated
    assert audit(root, require=[])["observables"].status == "n/a"
    assert compare_main(root) == 1 and compare_main(root, "--require", "") == 0
    assert compare_json(root)["require"] == [] and compare_json(root)["position_until"] == 15862
    walked = audit(good_pair(sub(tmp_path, "walk"), session="G3", mode="deterministic"))["observables"]
    assert walked.data["position_until"]["frame"] == 15862
    other = audit(good_pair(sub(tmp_path, "g2"), session="G2", mode="deterministic"))["observables"]
    assert "position_until" not in other.data                          # only G3 has one
    assert audit(make_pair(sub(tmp_path, "g1"), session="G1", mode="deterministic"))["observables"].status == "n/a"


def test_the_rules_line_names_the_session_mode_and_latency(tmp_path):
    def rules(**kwargs):
        root = good_pair(sub(tmp_path, str(len(list(tmp_path.iterdir())))), **kwargs)
        return fps_audit.format_report(fps_audit.read_pair(root), [], False)[1]

    g3 = rules(session="G3", mode="real-time")
    assert "G3, real-time mode, --io-latency azahar" in g3 and "gated runs, writes;" in g3
    g6 = rules(session="G6", mode="deterministic")
    assert "G6 (not named: G1's rules)" in g6 and "reported dsp" in g6
    g1 = rules(session="G1", mode="deterministic")
    assert "dsp (the window at frame 1240" in g1
    assert "every item reported" in rules(session="G2", io_latency="none")


# D70's tables as the spec gives them, written out here so that a change to the tool's own tables fails a test

# (2): each change's limit in frames at 60 fps, for a chain of 10 changes (#0 to #9); G6 is not named: 4 everywhere
D70_LIMITS = {
    "G1": [4, 20, 4, 4, 4, 4, 4, 4, 4, 4],
    "G2": [4, 20, 4, 10, 10, 10, 19, 19, 4, 4],
    "G3": [4, 20, 4, 10, 10, 10, 4, 4, 5.5, 5.5],
    "G4": [4, 20, 4, 10, 10, 10, 4, 4, 4, 4],
    "G6": [4, 4, 4, 4, 4, 4, 4, 4, 4, 4],
}

# the items an `azahar` pair gates, besides dsp (which gates G1's boot window only); G6 takes G1's rules
_RT = {"runs", "scenes", "times", "checkpoints", "writes"}
_DET = {"runs", "scenes", "times", "checkpoints", "observables", "writes"}
D70_GATED = {
    "real-time": {"G1": _RT, "G2": _RT, "G4": _RT, "G6": _RT, "G3": {"runs", "writes"}},
    "deterministic": {"G1": _DET, "G2": _DET, "G3": _DET, "G4": _DET, "G6": _DET},
}

# a good pair's arguments that make exactly one item fail
FAILING = {
    "runs": dict(exit_codes=(0, 1)),
    "scenes": dict(scenes_scaled=[(10, 0x1000), (240, 0x3000), (400, HUNT)],      # another scene at change #0
                   checkpoints=((60, 0x1000), (300, HUNT))),
    "times": dict(scenes_scaled=shifted(40)),                                      # change #0 20 frames late
    "checkpoints": dict(quest_id=(lambda f: 7, lambda f: 7 if f < 200 else 9)),
    "observables": dict(pos=(walk(60), walk(120, 4000.0))),                        # beyond 3,700 units
    "writes": dict(writes=([(0, BLOB)], [(0, with_byte(3, 9))])),
}


@pytest.mark.parametrize("session", sorted(D70_LIMITS))
def test_every_sessions_scene_time_limits_are_d70s_table(tmp_path, session):
    root = make_pair(tmp_path, scenes=chain(10), scenes_scaled=chain_scaled(10), checkpoints=(), session=session,
                     mode="deterministic")
    item = audit(root)["times"]
    assert item.status == "pass"
    assert [c["limit_frames"] for c in item.data["changes"]] == D70_LIMITS[session]


@pytest.mark.parametrize("name", sorted(FAILING))
@pytest.mark.parametrize("mode", ["real-time", "deterministic"])
@pytest.mark.parametrize("session", ["G1", "G2", "G3", "G4", "G6"])
def test_an_item_that_fails_decides_the_exit_status_where_d70_gates_it(tmp_path, capsys, session, mode, name):
    root = good_pair(tmp_path, session=session, mode=mode, **FAILING[name])
    gated = name in D70_GATED[mode][session]
    assert compare_main(root) == (1 if gated else 0)
    out = capsys.readouterr().out
    data = compare_json(root)
    assert [i["name"] for i in data["items"] if i["status"] == "fail"] == [name]       # this item and no other
    assert next(i for i in data["items"] if i["name"] == name)["gated"] is gated
    line = item_lines(out)[name]
    assert "FAIL" in line and ("gated" if gated else "reported") in line
    verdict = out.splitlines()[-1]
    if gated:
        assert data["failed"] == [name] and verdict.startswith(f"verdict: FAIL ({name})")
    else:
        assert data["failed"] == [] and data["reported_failures"] == [name]
        assert verdict.startswith(f"verdict: pass (reported: {name})")


@pytest.mark.parametrize("mode", ["real-time", "deterministic"])
@pytest.mark.parametrize("session", ["G1", "G2", "G3", "G4", "G6"])
def test_the_gated_items_of_each_session_and_mode(tmp_path, session, mode):
    root = good_pair(tmp_path, session=session, mode=mode, dsp_start=1240)
    assert compare_main(root) == 0
    data = compare_json(root)
    want = D70_GATED[mode][session] | ({"dsp"} if session == "G1" else set())        # G1's boot window
    assert {i["name"]: i["gated"] for i in data["items"]} == {n: n in want for n in fps_audit.ITEMS}
    assert set(data["rules"]["gated"]) == want and set(data["rules"]["reported"]) == set(fps_audit.ITEMS) - want
    assert data["rules"]["boot_windows"] == ([1240] if session == "G1" else [])


def test_g1s_dsp_stays_gated_without_its_boot_window(tmp_path, capsys):
    over = ((100, 100, 40), (100 + 49 - 10, 90, 40))              # +49: over the real-time bound
    root = good_pair(tmp_path, session="G1", mode="real-time", dsp_start=200, dsp=over)   # no window at 1240
    assert compare_main(root) == 0
    out = capsys.readouterr().out
    line = item_lines(out)["dsp"]
    assert "n/a" in line and "gated" in line and "no gated window (the window at frame 1240)" in line
    assert out.splitlines()[-1].startswith("verdict: pass (reported: dsp (dsp-200.mdsp))")
    dsp = next(i for i in compare_json(root)["items"] if i["name"] == "dsp")
    assert dsp["gated"] and dsp["status"] == "n/a"
    assert compare_main(root, "--require", "dsp") == 1                # required: a gated failure
    out = capsys.readouterr().out
    line = item_lines(out)["dsp"]
    assert "FAIL" in line and "gated" in line and "no gated window" in line and "(required)" in line
    assert out.splitlines()[-1].startswith("verdict: FAIL (dsp; reported: dsp (dsp-200.mdsp))")
    # no DSP capture at all
    bare = make_pair(sub(tmp_path, "bare"), session="G1", mode="real-time")
    assert compare_main(bare) == 0
    dsp = next(i for i in compare_json(bare)["items"] if i["name"] == "dsp")
    assert dsp["gated"] and dsp["status"] == "n/a"
    assert compare_main(bare, "--require", "dsp") == 1
    dsp = next(i for i in compare_json(bare)["items"] if i["name"] == "dsp")
    assert dsp["gated"] and dsp["status"] == "fail" and "(required)" in dsp["summary"]
    assert compare_json(bare)["failed"] == ["dsp"]


def test_compares_options_override_d70s_bounds(tmp_path):
    def run(rate, shift=0.0):
        return lambda f: (f / rate * 100.0 + shift, 1.0, 2.0)          # 100 units a second

    # position 3,700.5 units apart, quest counter 66 units = 1.1 s apart: both over D70's bounds
    root = good_pair(tmp_path, session="G3", mode="deterministic", pos=(run(60), run(120, 3700.5)),
                     quest=(countdown(60), countdown(120, 66)))

    def observables():
        return next(i for i in compare_json(root)["items"] if i["name"] == "observables")["data"]

    assert compare_main(root) == 1
    assert observables()["position"]["status"] == "fail" and observables()["quest_time"]["status"] == "fail"
    assert compare_main(root, "--position-absolute", "3701", "--quest-seconds", "1.2") == 0
    tol = compare_json(root)["tolerances"]
    assert tol["position_absolute"] == 3701 and tol["quest_seconds"] == 1.2 and tol["position_relative"] == 0
    assert compare_main(root, "--position-absolute", "3701") == 1      # the quest clock is still over
    assert compare_main(root, "--quest-seconds", "1.2") == 1           # the position is still over
    # the relative part: 0.01 of the 100 units travelled in the last second plus 3,700
    assert compare_main(root, "--position-relative", "0.01", "--quest-seconds", "1.2") == 0
    tol = compare_json(root)["tolerances"]
    assert tol["position_relative"] == 0.01 and tol["position_absolute"] == 3700
    assert observables()["position"]["rows"][0]["limit"] == pytest.approx(3701, abs=0.01)
    # --position-until replaces G3's 15862
    assert compare_main(root, "--position-until", "290", "--position-absolute", "3701", "--quest-seconds", "1.2") == 0
    assert compare_json(root)["position_until"] == 290 and observables()["position_until"]["frame"] == 290


@pytest.mark.parametrize("option,value", [
    ("--time-frames", "-1"), ("--time-frames", "nan"), ("--time-frames", "inf"),
    ("--position-absolute", "0"), ("--position-absolute", "nan"), ("--position-absolute", "-3700"),
    ("--position-relative", "-0.5"), ("--position-relative", "inf"),
    ("--quest-seconds", "nan"), ("--quest-seconds", "-1"), ("--quest-units", "0"), ("--quest-units", "nan"),
    ("--max-shift", "-1"),
])
def test_compare_refuses_a_bound_that_is_not_a_finite_value_of_0_or_more(tmp_path, capsys, option, value):
    root = good_pair(tmp_path, session="G3", mode="deterministic")
    assert compare_main(root, f"{option}={value}") == 2
    assert f"fps_audit: {option} " in capsys.readouterr().err
    assert not (root / "compare.json").exists()


def test_a_meta_file_without_io_latency_is_taken_as_azahar_and_any_other_value_is_reported(tmp_path, capsys):
    root = good_pair(tmp_path, session="G2", mode="real-time", writes=([(0, BLOB)], [(0, with_byte(3, 9))]))
    meta = json.loads((root / "meta.json").read_text(encoding="utf-8"))
    del meta["io_latency"]
    (root / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
    assert compare_main(root) == 1                                     # gated as azahar: the Writes differ
    out = capsys.readouterr().out.splitlines()
    assert out[0].endswith("--io-latency azahar (not in meta.json: taken as azahar), real-time mode")
    assert "--io-latency azahar: gated runs, scenes" in out[1]
    data = compare_json(root)
    assert data["io_latency"] == "azahar" and data["io_latency_note"] == "not in meta.json: taken as azahar"
    assert data["failed"] == ["writes"]
    # a value `run` never writes (N3-22's hand-made analysis folders carry a note there): every item reported
    for value in ("60 fps run of A (as 60) against 120 fps run of B (as 120)", "reference", "", "Azahar"):
        meta["io_latency"] = value
        (root / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
        assert compare_main(root) == 0
        out = capsys.readouterr().out.splitlines()
        assert f"--io-latency {value} (neither azahar nor none: not a pair of `fps_audit.py run`" in out[0]
        assert out[1].endswith(f"--io-latency {value}: every item reported (D71: a value but azahar or none gates "
                               f"nothing; `azahar` is the gated latency)")
        assert out[-1].startswith("verdict: pass (reported: writes); 0 gated, 7 reported")
        data = compare_json(root)
        assert data["io_latency"] == value and data["io_latency_note"].startswith("neither azahar nor none")
        assert not any(i["gated"] for i in data["items"]) and data["rules"]["gated"] == []
    meta["io_latency"] = "none"
    (root / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
    assert compare_main(root) == 0
    assert compare_json(root)["io_latency"] == "none" and compare_json(root)["io_latency_note"] is None
    meta["io_latency"] = "azahar"
    (root / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
    assert compare_main(root) == 1
    assert compare_json(root)["io_latency"] == "azahar" and compare_json(root)["io_latency_note"] is None
