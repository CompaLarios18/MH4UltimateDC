"""tools/native/setup_native.py on synthetic inputs only: NCCH and CIA files from the prepare_game
tests' builders, and stub commands standing in for build_game.py, mods.py (install) and
mh4n_hdpack. Nothing here comes from the game, and nothing touches the network."""
import json
import os
import shlex
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import mods  # noqa: E402
import setup_native as sn  # noqa: E402
import test_extract_ncch as tx  # noqa: E402
from test_native_prepare_game import PID, make_icon, make_ncch, write_inputs  # noqa: E402,F401

make_icon_bytes = make_icon

PY = shlex.quote(sys.executable)

BUILD_STUB = '''
import os, sys
sys.path.insert(0, {tools!r})
import mods
def log(text):
    with open(os.environ["STUB_LOG"], "a") as f:
        f.write(text + "\\n")
args = sys.argv[1:]
log("build " + " ".join(os.path.basename(a) if os.path.isfile(a) else a for a in args))
user = args[args.index("--install") + 1]
print("extracting " + args[args.index("--dump") + 1])
for i in range(1, 4):
    print(f"compiled {{i}}/3")
if os.environ.get("STUB_BUILD_FAIL"):
    print("boom: no compiler")
    sys.exit(1)
lib = mods.code_library(user, native=True)
os.makedirs(os.path.dirname(lib), exist_ok=True)
open(lib, "wb").write(b"native library")
if not os.environ.get("STUB_NO_RECORD"):
    used = mods.code_inputs(user)           # what build_game.py applies from the mods folder
    mods.write_build_record(lib, used, 0x100, bool(mods.changes_code(used, 0x100)))
print("installed to " + lib)
'''

MODS_STUB = '''
import json, os, sys
sys.path.insert(0, {tools!r})
import mods
def log(text):
    with open(os.environ["STUB_LOG"], "a") as f:
        f.write(text + "\\n")
args = sys.argv[1:]
user = args[args.index("--user-dir") + 1]
command = next(a for a in args if a in ("status", "catalog", "install-from-catalog"))
log("mods " + command)
marker = os.path.join(user, "stub-pack-installed")
if command == "status":
    sys.exit(mods.main(args))
if command == "catalog":
    installed = {{"as": "textures", "id": "mh4u-hd", "version": "3", "files": 2}} if os.path.exists(marker) else None
    print(json.dumps({{"ok": True, "command": "catalog", "entries": [{{"id": "mh4u-hd", "installed": installed}}]}}))
    sys.exit(0)
if os.environ.get("STUB_MODS_FAIL"):
    print(json.dumps({{"ok": False, "command": command, "error": {{"code": "network", "message": "no connection"}}}}))
    sys.exit(1)
print("@" + json.dumps({{"event": "step", "step": 1, "steps": 2, "message": "Downloading the pack"}}))
print("@" + json.dumps({{"event": "download", "done": 5, "total": 10, "what": "pack.zip"}}))
folder = mods.textures_folder(user)
os.makedirs(folder, exist_ok=True)
for n in ("a.png", "b.png"):
    open(os.path.join(folder, n), "wb").write(n.encode() * 100)
open(marker, "w").write("x")
print(json.dumps({{"ok": True, "command": command}}))
'''

HDPACK_STUB = '''
import os, sys
def log(text):
    with open(os.environ["STUB_LOG"], "a") as f:
        f.write(text + "\\n")
pack, out = sys.argv[1], sys.argv[sys.argv.index("-o") + 1]
log("hdpack " + str(len(os.listdir(pack))) + " files")
for i in (1, 2):
    print(f"hdpack: {{i}}/2 textures ({{i * 50.0}}%), 1.0 MiB written, 0 failed, 1 s, about 1 s left", flush=True)
if os.environ.get("STUB_HDPACK_FAIL"):
    print("hdpack: error: disk full")
    sys.exit(int(os.environ["STUB_HDPACK_FAIL"]))
open(out, "wb").write(b"MH4HD" * 20)
'''


@pytest.fixture
def env(tmp_path, monkeypatch):
    """Inputs, stubs and a user folder; env.args() is a full command line."""
    tools = os.path.join(HERE, "..", "tools")
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    for name, body in (("build.py", BUILD_STUB), ("mods.py", MODS_STUB), ("hdpack.py", HDPACK_STUB)):
        (stubs / name).write_text(body.format(tools=os.path.abspath(tools)))
    log = tmp_path / "stub.log"
    monkeypatch.setenv("STUB_LOG", str(log))
    for var in ("STUB_BUILD_FAIL", "STUB_NO_RECORD", "STUB_MODS_FAIL", "STUB_HDPACK_FAIL", "MH4N_HDPACK"):
        monkeypatch.delenv(var, raising=False)
    (tmp_path / "inputs").mkdir()
    base, update, code = write_inputs(tmp_path / "inputs")
    versions = tmp_path / "versions.json"
    versions.write_text(json.dumps({"versions": [{
        "name": "Synthetic v1.3.0 (test)", "target": True, "program_id": f"{PID:016X}",
        "update_title_id": f"{tx.PROGRAM_ID:016X}", "title_version": 1072}]}))
    user = tmp_path / "user"
    user.mkdir()

    class Env:
        pass

    e = Env()
    e.tmp, e.user, e.base, e.update, e.code, e.stubs, e.log = tmp_path, user, base, update, code, stubs, log
    e.versions = str(versions)

    def args(*extra, update_arg=True, hd=False, stub_mods=True):
        out = ["--user", str(user), "--game", base, "--versions", e.versions,
               "--build-command", f"{PY} {shlex.quote(str(stubs / 'build.py'))}", "--json"]
        if stub_mods:
            out += ["--mods-command", f"{PY} {shlex.quote(str(stubs / 'mods.py'))}"]
        if update_arg:
            out += ["--update", update]
        out += ["--hdpack", str(stubs / "hdpack.py")] if hd else ["--no-hd"]
        return out + list(extra)

    e.args = args
    e.calls = lambda: log.read_text().splitlines() if log.exists() else []
    return e


def run(capsys, args, code=0):
    """setup_native.main(args): (events, result)."""
    rc = sn.main(args)
    out = capsys.readouterr().out
    events = [json.loads(line[1:]) for line in out.splitlines() if line.startswith("@")]
    body = "\n".join(line for line in out.splitlines() if not line.startswith("@"))
    result = json.loads(body)
    assert rc == code, result
    assert result["ok"] is (code == 0)
    return events, result


def states(result):
    return {s["step"]: s["state"] for s in result["steps"]}


def snapshot(root):
    """Every path under root with its size and modification time."""
    out = {}
    for top, dirs, files in os.walk(root):
        for name in dirs + files:
            p = os.path.join(top, name)
            st = os.lstat(p)
            out[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
    return out


def ips(offset, data):
    return b"PATCH" + offset.to_bytes(3, "big") + len(data).to_bytes(2, "big") + data + b"EOF"


# ── a full run, and the second one ──────────────────────────────────────────

def test_run_makes_the_game_folder_and_the_library(env, capsys):
    events, result = run(capsys, env.args())
    assert states(result) == {"game": "done", "library": "done"} and result["complete"] is True
    game = env.user / "native" / "game"
    assert (game / "code.bin").read_bytes() == env.code
    info = (game / "info.txt").read_text().splitlines()
    assert info[-2:] == [f"input base  : {env.base}, 0x{os.path.getsize(env.base):X} bytes",
                         f"input update: {env.update}, 0x{os.path.getsize(env.update):X} bytes"]
    lib = mods.code_library(str(env.user), native=True)         # D61: <16 hex>_native.<ext>
    assert os.path.basename(lib).startswith("0004000000126300_native.") and os.path.isfile(lib)
    assert os.path.isfile(env.user / "recomp" / "0004000000126300_native.build.json")
    assert not (env.user / "load").exists()                  # --no-hd: no HD step, no pack
    # the build was asked exactly as the spec says, with the update, for the native runtime
    build = [c for c in env.calls() if c.startswith("build")]
    assert len(build) == 1 and build[0].startswith("build --dump update.cia --native --install ")
    assert build[0].endswith(" --fetch-compiler")


def test_progress_events_are_mods_py_shaped(env, capsys):
    events, _ = run(capsys, env.args())
    assert all(isinstance(e, dict) and "event" in e and e["step"] in ("game", "library") for e in events)
    steps = [e for e in events if e["event"] == "step"]
    assert [(e["step"], e["state"]) for e in steps] == [("game", "running"), ("game", "done"),
                                                        ("library", "running"), ("library", "done")]
    assert all(e["steps"] == 2 and isinstance(e["message"], str) and "done" not in e for e in steps)
    assert [e["index"] for e in steps] == [1, 1, 2, 2]
    # the build's own lines are stages the launcher can show: "message", no "done"
    progress = [e for e in events if e["event"] == "progress"]
    assert progress and progress[-1]["message"].startswith("installed to")
    assert all(e["step"] == "library" and "done" not in e for e in progress)


def test_events_go_to_standard_output_only_with_json_and_are_text_otherwise(env, capsys):
    args = [a for a in env.args() if a != "--json"]
    rc = sn.main(args)
    out = capsys.readouterr().out
    assert rc == 0 and "@" not in out and "The native runtime is set up." in out


def test_second_run_skips_every_step(env, capsys):
    run(capsys, env.args(hd=True))
    calls = env.calls()
    events, result = run(capsys, env.args(hd=True))
    assert states(result) == {"game": "skipped", "library": "skipped", "hd": "skipped"}
    assert [(e["step"], e["state"]) for e in events if e["event"] == "step"] == [
        ("game", "skipped"), ("library", "skipped"), ("hd", "skipped")]
    assert not [c for c in env.calls()[len(calls):] if not c.startswith("mods status")]   # nothing ran


def test_check_reports_each_step_and_changes_nothing(env, capsys):
    _, result = run(capsys, env.args("--check", hd=True))
    assert states(result) == {"game": "missing", "library": "missing", "hd": "missing"}
    assert result["complete"] is False and all(s["reason"] for s in result["steps"])
    run(capsys, env.args(hd=True))
    before = snapshot(env.user)
    calls = env.calls()
    _, result = run(capsys, env.args("--check", hd=True))
    assert states(result) == {"game": "done", "library": "done", "hd": "done"} and result["complete"] is True
    assert snapshot(env.user) == before                      # listing, sizes and mtimes
    assert not [c for c in env.calls()[len(calls):] if not c.startswith("mods status")]


def test_check_creates_nothing_not_even_the_user_folder(env, capsys):
    ghost = env.tmp / "not-yet"
    args = env.args("--check", stub_mods=False)
    args[args.index("--user") + 1] = str(ghost)
    _, result = run(capsys, args)                            # the real mods.py status, in a child
    assert states(result) == {"game": "missing", "library": "missing"}
    assert not ghost.exists()
    # and with the real mods.py over a populated user folder (it reads, it does not write)
    run(capsys, env.args())
    before = snapshot(env.user)
    _, result = run(capsys, env.args("--check", stub_mods=False))
    assert states(result) == {"game": "done", "library": "done"}
    assert snapshot(env.user) == before


def test_check_text_output(env, capsys):
    rc = sn.main([a for a in env.args("--check") if a != "--json"])
    out = capsys.readouterr().out
    assert rc == 0 and out.splitlines()[0].startswith("game: missing (")


# ── the game folder: done, missing, stale ───────────────────────────────────

def game_check(env, capsys, **kw):
    _, result = run(capsys, env.args("--check", **kw))
    step = next(s for s in result["steps"] if s["step"] == "game")
    return step["state"], step["reason"]


def test_game_folder_done_missing_and_stale(env, capsys, tmp_path):
    assert game_check(env, capsys)[0] == "missing"
    run(capsys, env.args())
    assert game_check(env, capsys)[0] == "done"
    game = env.user / "native" / "game"
    # the game file replaced by another of a different size
    other = tmp_path / "other.cxi"
    other.write_bytes(open(env.base, "rb").read() + bytes(0x200))
    args = env.args("--check")
    args[args.index("--game") + 1] = str(other)
    _, result = run(capsys, args)
    assert states(result)["game"] == "stale"
    assert "another base game file" in result["steps"][0]["reason"]
    # the same bytes at another path
    copy = tmp_path / "copy.cxi"
    copy.write_bytes(open(env.base, "rb").read())
    args[args.index("--game") + 1] = str(copy)
    assert states(run(capsys, args)[1])["game"] == "stale"
    # the update file grown
    with open(env.update, "ab") as f:
        f.write(b"\0" * 64)
    state, reason = game_check(env, capsys)
    assert state == "stale" and "update file has changed size" in reason


@pytest.mark.parametrize("damage, why", [
    ("delete code", "code.bin is missing"),
    ("edit code", "code.bin is not the one info.txt describes"),
    ("truncate romfs", "romfs.bin is not the size"),
    ("delete update romfs", "update_romfs.bin is missing"),
    ("old info", "does not say which files"),
])
def test_game_folder_stale_when_a_file_is_not_what_info_says(env, capsys, damage, why):
    run(capsys, env.args())
    game = env.user / "native" / "game"
    if damage == "delete code":
        os.remove(game / "code.bin")
    elif damage == "edit code":
        data = bytearray((game / "code.bin").read_bytes())
        data[5] ^= 1
        (game / "code.bin").write_bytes(bytes(data))
    elif damage == "truncate romfs":
        (game / "romfs.bin").write_bytes(b"short")
    elif damage == "delete update romfs":
        os.remove(game / "update_romfs.bin")
    else:
        lines = (game / "info.txt").read_text().splitlines()
        (game / "info.txt").write_text("\n".join(l for l in lines if not l.startswith("input ")) + "\n")
    state, reason = game_check(env, capsys)
    assert state == "stale" and why in reason
    # a run redoes it, and it is done again
    events, result = run(capsys, env.args())
    assert states(result)["game"] == "done"
    assert game_check(env, capsys)[0] == "done"


def test_game_folder_of_the_earlier_version_is_stale_and_a_run_remakes_it(env, capsys):
    # N1-30: before, icon.bin was the base game's and info.txt said nothing of its source
    (env.tmp / "inputs2").mkdir()
    base, update, _ = write_inputs(env.tmp / "inputs2", icon=make_icon_bytes(0x04),
                                   update_icon=make_icon_bytes(0x02))
    args = env.args()
    args[args.index("--game") + 1] = base
    args[args.index("--update") + 1] = update
    run(capsys, args)
    game = env.user / "native" / "game"
    assert (game / "icon.bin").read_bytes() == make_icon_bytes(0x02)
    # make it what the earlier version wrote: the base game's icon, no "icon source" line
    (game / "icon.bin").write_bytes(make_icon_bytes(0x04))
    lines = (game / "info.txt").read_text().splitlines()
    (game / "info.txt").write_text("\n".join(l for l in lines if not l.startswith("icon source")) + "\n")
    _, result = run(capsys, args + ["--check"])
    step = next(s for s in result["steps"] if s["step"] == "game")
    assert step["state"] == "stale" and "earlier version" in step["reason"]
    events, result = run(capsys, args)
    assert states(result)["game"] == "done"
    assert (game / "icon.bin").read_bytes() == make_icon_bytes(0x02)
    assert "icon source   : update" in (game / "info.txt").read_text().splitlines()
    _, result = run(capsys, args + ["--check"])
    assert states(result)["game"] == "done"


@pytest.mark.parametrize("claim, why", [("base game", "not the update's"), ("none", "says the update has none")])
def test_game_folder_whose_icon_source_does_not_fit_is_stale(env, capsys, claim, why):
    (env.tmp / "inputs2").mkdir()
    base, update, _ = write_inputs(env.tmp / "inputs2", update_icon=make_icon_bytes(0x02))
    args = env.args()
    args[args.index("--game") + 1] = base
    args[args.index("--update") + 1] = update
    run(capsys, args)
    info = env.user / "native" / "game" / "info.txt"
    info.write_text(info.read_text().replace("icon source   : update", f"icon source   : {claim}"))
    state, reason = game_check_with(env, capsys, args)
    assert state == "stale" and why in reason


def game_check_with(env, capsys, args):
    _, result = run(capsys, args + ["--check"])
    step = next(s for s in result["steps"] if s["step"] == "game")
    return step["state"], step["reason"]


def test_game_folder_without_info_txt_is_missing_even_with_files(env, capsys):
    run(capsys, env.args())
    os.remove(env.user / "native" / "game" / "info.txt")
    assert game_check(env, capsys)[0] == "missing"


def test_failed_prepare_leaves_the_folder_missing_not_done(env, capsys):
    run(capsys, env.args())
    builds = len([c for c in env.calls() if c.startswith("build")])
    (env.tmp / "junk.cia").write_bytes(b"\0" * 0x400)
    args = env.args()
    args[args.index("--update") + 1] = str(env.tmp / "junk.cia")
    _, result = run(capsys, args, code=1)
    assert result["error"]["code"] == "prepare_failed" and result["step"] == "game"
    assert not (env.user / "native" / "game" / "info.txt").exists()
    assert len([c for c in env.calls() if c.startswith("build")]) == builds   # nothing after the failed step


def test_the_installed_update_is_the_default(env, capsys):
    app = env.user / "sdmc" / "Nintendo 3DS" / ("0" * 32) / ("0" * 32) / "title" / "0004000e" / "00126300" / "content"
    app.mkdir(parents=True)
    # the installed content: the update's NCCH, with the title of the update
    import blz_encoder
    import make_cxi
    import test_native_prepare_game as tp
    upd_exh = make_ncch(True, b"UPDROMFS" * 50)[0][0x200:0xA00]
    stored = blz_encoder.compress(tp.code_image())
    (app / "00000000.app").write_bytes(make_cxi.make_cxi(upd_exh, tp.fake_exefs(stored), b"UPDROMFS" * 50,
                                                         program_id=tx.PROGRAM_ID))
    args = env.args(update_arg=False)
    _, result = run(capsys, args)
    assert states(result) == {"game": "done", "library": "done"}
    info = (env.user / "native" / "game" / "info.txt").read_text()
    assert f"input update: {app / '00000000.app'}, 0x{os.path.getsize(app / '00000000.app'):X} bytes" in info
    assert any(c.startswith("build --dump 00000000.app ") for c in env.calls())
    _, result = run(capsys, args)
    assert states(result) == {"game": "skipped", "library": "skipped"}


def test_no_update_is_an_error(env, capsys):
    _, result = run(capsys, env.args(update_arg=False), code=1)
    assert result["error"]["code"] == "no_update"
    assert not (env.user / "native").exists()


def test_a_missing_input_file_is_an_error(env, capsys):
    args = env.args()
    args[args.index("--game") + 1] = str(env.tmp / "nope.cxi")
    _, result = run(capsys, args, code=1)
    assert result["error"]["code"] == "not_found"


# ── the code library: done, missing, stale ──────────────────────────────────

def library_check(env, capsys):
    _, result = run(capsys, env.args("--check"))
    step = next(s for s in result["steps"] if s["step"] == "library")
    return step["state"], step["reason"]


def test_library_done_missing_and_stale(env, capsys):
    assert library_check(env, capsys)[0] == "missing"
    run(capsys, env.args())
    assert library_check(env, capsys)[0] == "done"
    lib = mods.code_library(str(env.user), native=True)
    # a build again that left no record (an older build_game.py): not known to match
    os.remove(mods.build_record_path(lib))
    state, reason = library_check(env, capsys)
    assert state == "stale" and "no record" in reason
    # a code mod that changes the code since it was built
    run(capsys, env.args())                                  # the run rebuilds a stale library
    assert library_check(env, capsys)[0] == "done"
    mod = env.user / "load" / "mods" / "0004000000126300" / "exefs"
    mod.mkdir(parents=True)
    (mod / "code.ips").write_bytes(ips(0x10, b"\1"))
    state, reason = library_check(env, capsys)
    assert state == "stale" and "build it again" in reason
    calls = len([c for c in env.calls() if c.startswith("build")])
    _, result = run(capsys, env.args())
    assert states(result)["library"] == "done" and len([c for c in env.calls() if c.startswith("build")]) == calls + 1


def test_the_azahar_library_does_not_count(env, capsys):
    """Only <program id>_native.<ext> is the native library, and each build keeps its own record."""
    run(capsys, env.args())
    azahar = mods.code_library(str(env.user))
    open(azahar, "wb").write(b"azahar library")
    mods.write_build_record(azahar, [], 0x100, False)
    assert mods.build_record_path(azahar) != mods.build_record_path(mods.code_library(str(env.user), True))
    assert library_check(env, capsys)[0] == "done"
    os.remove(mods.code_library(str(env.user), True))
    assert library_check(env, capsys)[0] == "missing"


def test_a_failed_build_is_reported_with_its_last_line(env, capsys, monkeypatch):
    monkeypatch.setenv("STUB_BUILD_FAIL", "1")
    events, result = run(capsys, env.args(), code=1)
    assert result["error"] == {"code": "build_failed", "message": "boom: no compiler", "exit_status": 1}
    assert result["step"] == "library" and states(result) == {"game": "done", "library": "failed"}
    failed = [e for e in events if e["event"] == "step" and e["state"] == "failed"]
    assert failed and failed[0]["step"] == "library" and failed[0]["error"]["code"] == "build_failed"
    _, check = run(capsys, env.args("--check"))
    assert states(check) == {"game": "done", "library": "missing"}


def test_a_build_that_leaves_no_usable_library_is_a_failure(env, capsys, monkeypatch):
    monkeypatch.setenv("STUB_NO_RECORD", "1")
    _, result = run(capsys, env.args(), code=1)
    assert result["error"]["code"] == "build_failed" and "it is stale" in result["error"]["message"]


# ── the HD archive: done, missing, stale ────────────────────────────────────

def hd_check(env, capsys):
    _, result = run(capsys, env.args("--check", hd=True))
    step = next(s for s in result["steps"] if s["step"] == "hd")
    return step["state"], step["reason"]


def test_hd_installs_the_pack_converts_it_and_records_its_source(env, capsys):
    events, result = run(capsys, env.args(hd=True))
    assert states(result) == {"game": "done", "library": "done", "hd": "done"}
    pack = mods.textures_folder(str(env.user))
    archive = pack + ".mh4hd"
    assert os.path.basename(archive) == "0004000000126300.mh4hd" and os.path.isfile(archive)
    record = json.load(open(archive + ".source.json"))
    assert record["format"] == 1 and record["files"] == 2 and record["bytes"] == 2 * 500
    assert record["newest_mtime_ns"] == max(os.stat(os.path.join(pack, n)).st_mtime_ns for n in os.listdir(pack))
    tool = os.stat(env.stubs / "hdpack.py")
    assert record["tool"] == {"size": tool.st_size, "mtime_ns": tool.st_mtime_ns}
    hd = [e for e in events if e["step"] == "hd"]
    # mods.py's own events go on, its "step" counter renamed so the step name stays
    assert {"event": "download", "step": "hd", "done": 5, "total": 10, "what": "pack.zip"} in hd
    # mods.py's own "step" lines are progress, never mistaken for ours (which carry a state)
    parts = [e for e in hd if e.get("part") == 1 and e.get("parts") == 2]
    assert parts and all(e["event"] == "progress" and e["step"] == "hd" and "state" not in e for e in parts)
    assert all("state" in e for e in hd if e["event"] == "step")
    # mh4n_hdpack's lines become the launcher's "N of M files" update
    installs = [e for e in hd if e["event"] == "install"]
    assert installs and installs[0]["what"] == "Converting textures" and installs[0]["total"] == 2
    assert env.calls().count("mods install-from-catalog") == 1
    assert hd_check(env, capsys)[0] == "done"


def test_hd_does_not_download_a_pack_that_is_installed(env, capsys):
    run(capsys, env.args(hd=True))
    os.remove(mods.textures_folder(str(env.user)) + ".mh4hd")
    n = len([c for c in env.calls() if c == "mods install-from-catalog"])
    _, result = run(capsys, env.args(hd=True))
    assert states(result)["hd"] == "done"
    assert len([c for c in env.calls() if c == "mods install-from-catalog"]) == n   # converted only
    assert "mods catalog" in env.calls()


def test_hd_done_missing_and_stale(env, capsys):
    run(capsys, env.args())
    assert hd_check(env, capsys)[0] == "missing"
    run(capsys, env.args(hd=True))
    assert hd_check(env, capsys)[0] == "done"
    pack = mods.textures_folder(str(env.user))
    archive = pack + ".mh4hd"
    # a file added to the pack
    open(os.path.join(pack, "c.png"), "wb").write(b"c")
    state, reason = hd_check(env, capsys)
    assert state == "stale" and "pack has changed" in reason
    run(capsys, env.args(hd=True))
    assert hd_check(env, capsys)[0] == "done"
    # a file of the pack touched (only its modification time)
    st = os.stat(os.path.join(pack, "a.png"))
    os.utime(os.path.join(pack, "a.png"), ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
    assert hd_check(env, capsys)[0] == "stale"
    run(capsys, env.args(hd=True))
    # a file of the pack changed in size
    open(os.path.join(pack, "a.png"), "ab").write(b"more")
    assert hd_check(env, capsys)[0] == "stale"
    run(capsys, env.args(hd=True))
    assert hd_check(env, capsys)[0] == "done"
    # the archive without its record
    os.remove(archive + ".source.json")
    state, reason = hd_check(env, capsys)
    assert state == "stale" and "no record" in reason
    run(capsys, env.args(hd=True))
    # a new build of mh4n_hdpack
    tool = env.stubs / "hdpack.py"
    st = tool.stat()
    os.utime(tool, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
    state, reason = hd_check(env, capsys)
    assert state == "stale" and "mh4n_hdpack has changed" in reason
    run(capsys, env.args(hd=True))
    assert hd_check(env, capsys)[0] == "done"
    # the archive gone, or empty
    open(archive, "wb").close()
    assert hd_check(env, capsys)[0] == "missing"
    os.remove(archive)
    assert hd_check(env, capsys)[0] == "missing"
    # the pack folder gone: the archive can't be vouched for
    run(capsys, env.args(hd=True))
    import shutil
    shutil.rmtree(pack)
    assert hd_check(env, capsys)[0] == "stale"


def test_hd_without_the_tool_does_not_download_anything(env, capsys, monkeypatch):
    args = env.args()
    args.remove("--no-hd")
    args += ["--hdpack", str(env.tmp / "no-such-tool")]
    _, result = run(capsys, args, code=1)
    assert result["error"]["code"] == "no_hdpack" and result["step"] == "hd"
    assert not any(c == "mods install-from-catalog" for c in env.calls())
    # $MH4N_HDPACK is the next place it looks
    args = env.args()
    args.remove("--no-hd")
    monkeypatch.setenv("MH4N_HDPACK", str(env.stubs / "hdpack.py"))
    _, result = run(capsys, args)
    assert states(result)["hd"] == "done"


def test_hd_failures_keep_their_codes_and_leave_no_matching_record(env, capsys, monkeypatch):
    monkeypatch.setenv("STUB_MODS_FAIL", "1")
    _, result = run(capsys, env.args(hd=True), code=1)
    assert result["error"]["code"] == "network" and result["error"]["message"] == "no connection"
    assert result["step"] == "hd" and states(result) == {"game": "done", "library": "done", "hd": "failed"}
    monkeypatch.delenv("STUB_MODS_FAIL")
    run(capsys, env.args(hd=True))
    archive = mods.textures_folder(str(env.user)) + ".mh4hd"
    open(os.path.join(mods.textures_folder(str(env.user)), "c.png"), "wb").write(b"c")
    monkeypatch.setenv("STUB_HDPACK_FAIL", "3")
    _, result = run(capsys, env.args(hd=True), code=1)
    assert result["error"] == {"code": "hdpack_failed", "message": "hdpack: error: disk full", "exit_status": 3}
    assert not os.path.exists(archive + ".source.json")
    assert hd_check(env, capsys)[0] == "stale"               # the old archive is not vouched for
    monkeypatch.setenv("STUB_HDPACK_FAIL", "4")
    _, result = run(capsys, env.args(hd=True), code=1)
    assert result["error"]["code"] == "interrupted"


def test_no_hd_leaves_the_step_out(env, capsys):
    _, result = run(capsys, env.args("--check"))
    assert [s["step"] for s in result["steps"]] == ["game", "library"]
    _, result = run(capsys, env.args("--check", hd=True))
    assert [s["step"] for s in result["steps"]] == ["game", "library", "hd"]


def test_hd_is_the_default(env, capsys):
    args = env.args("--check")
    args.remove("--no-hd")
    args += ["--hdpack", str(env.stubs / "hdpack.py")]
    _, result = run(capsys, args)
    assert [s["step"] for s in result["steps"]] == ["game", "library", "hd"]


# ── arguments ───────────────────────────────────────────────────────────────

def test_usage_errors_exit_2_and_say_so_in_json(env, capsys):
    for bad in (["--game", env.base], ["--user", str(env.user)], ["--user", str(env.user), "--game", env.base,
                                                                  "--hd", "--no-hd"]):
        rc = sn.main(bad + ["--json"])
        result = json.loads(capsys.readouterr().out)
        assert rc == 2 and result["ok"] is False and result["error"]["code"] == "usage"
    rc = sn.main(["--bogus"])
    assert rc == 2 and "setup_native.py:" in capsys.readouterr().err


def test_find_installed_update_takes_the_newest(env):
    root = env.user / "sdmc" / "Nintendo 3DS" / "a" / "b" / "title" / "0004000e" / "00126300" / "content"
    root.mkdir(parents=True)
    old, new = root / "00000000.app", root / "00000001.app"
    old.write_bytes(b"1")
    new.write_bytes(b"2")
    os.utime(old, (1, 1))
    assert sn.find_installed_update(str(env.user)) == str(new)
    other = env.user / "sdmc" / "Nintendo 3DS" / "a" / "b" / "title" / "0004000e" / "00126301" / "content"
    other.mkdir(parents=True)
    (other / "00000000.app").write_bytes(b"3")
    assert sn.find_installed_update(str(env.user)) == str(new)         # only this game's update counts
    assert sn.find_installed_update(str(env.tmp / "empty")) is None


def test_throttle_passes_the_last_line(monkeypatch):
    sent = []
    t = sn.Throttle(sent.append)
    clock = iter([10.0, 10.01, 10.02, 20.0])
    monkeypatch.setattr(sn.time, "monotonic", lambda: next(clock))
    for line in ("a", "b", "c", "d"):
        t(line)
    t.flush()
    assert sent == ["a", "d"]


def test_hd_io_error_is_a_final_json_not_a_traceback(env, capsys, monkeypatch):
    run(capsys, env.args(hd=True))
    open(os.path.join(mods.textures_folder(str(env.user)), "c.png"), "wb").write(b"c")   # stale
    def full(*a, **k):
        raise OSError(28, "No space left on device")
    monkeypatch.setattr(sn.os, "replace", full)
    _, result = run(capsys, env.args(hd=True), code=1)
    assert result["error"]["code"] == "io" and result["step"] == "hd"
    assert "No space" in result["error"]["message"]


def test_mods_json_skips_a_stray_line_before_the_object(tmp_path):
    stub = tmp_path / "m.py"
    stub.write_text("print('warning: odd')\nprint('{')\nprint(' \"ok\": true, \"x\": 1')\nprint('}')\n")
    out = sn.mods_json([sys.executable, str(stub)], str(tmp_path), "status")
    assert out["x"] == 1
    stub.write_text('print("only noise")\n')
    with pytest.raises(sn.SetupError) as e:
        sn.mods_json([sys.executable, str(stub)], str(tmp_path), "status")
    assert e.value.code == "mods_failed"
