"""The app's launcher (port/app/launcher/) runs the port's tools as child processes and reads
what they print: tools/mods.py --json for the Mods screen and for deploying the mods on Play,
and tools/build_game.py's progress lines for the build's progress bar. These tests pin the
parts of those outputs the launcher reads, run the way it runs them (python -u TOOL --json
--user-dir DIR COMMAND), with synthetic mods and a synthetic program (never game data).

With MH4U_APP set to a built mh4u, they also run its code check, mh4u --check-code GAME, which
the launcher runs in a process of its own to learn what the game's start would say about the
code library.

Two tests read the launcher's source: what mods.py and the code check return for the game chosen
before is left out once another game is chosen, and with no game chosen the Mods screen goes by
the region mods.py takes."""
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

MH4U = 0x0004000000126300


def write(path, data=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def mods(user, *args, code=0):
    """mods.py as the launcher runs it: its one JSON object."""
    r = subprocess.run([sys.executable, "-u", os.path.join(TOOLS, "mods.py"), "--json", "--user-dir", str(user),
                        *args], capture_output=True, text=True, timeout=120)
    assert r.returncode == code, (r.stdout, r.stderr)
    out = json.loads(r.stdout)
    assert out["ok"] is (code == 0)
    return out


def check_overview(out):
    """What the Mods screen shows of the library (launcher/screen_mods.cpp, ApplyOverview)."""
    for key, kind in (("library", str), ("mods_folder", str), ("deploy_state", str), ("problems", list),
                      ("warnings", list), ("mods", list)):
        assert isinstance(out[key], kind), key
    assert out["deploy_state"] in ("up_to_date", "out_of_date", "blocked")
    for mod in out["mods"]:
        for key, kind in (("id", str), ("name", str), ("version", str), ("author", str), ("description", str),
                          ("url", str), ("enabled", bool), ("new", bool), ("position", int), ("files", int),
                          ("wins", int), ("loses", int), ("warnings", list), ("errors", list)):
            assert isinstance(mod[key], kind), (mod["id"], key)
        assert mod["code"] is None or isinstance(mod["code"]["changes_code"], (bool, type(None)))


def test_mods_json_the_launcher_reads(tmp_path):
    user = tmp_path / "azahar"
    user.mkdir()
    a = tmp_path / "Sharper Swords"
    write(a / "romfs" / "data" / "pl_swo010.arc", b"A")
    write(a / "mod.json", json.dumps({"name": "Sharper Swords", "version": "1.2", "author": "someone",
                                      "description": "synthetic"}).encode())
    b = tmp_path / "Menus"
    write(b / "romfs" / "data" / "pl_swo010.arc", b"B")
    c = tmp_path / "Faster Gathering"
    # an IPS patch of one byte at 0x10: in the code segment, so it changes the game's code
    write(c / "exefs" / "code.ips", b"PATCH" + (0x10).to_bytes(3, "big") + (1).to_bytes(2, "big") + b"\x40EOF")

    out = mods(user, "list")
    check_overview(out)
    assert out["mods"] == [] and out["deploy_state"] == "up_to_date"
    for path in (a, b, c):
        out = mods(user, "add", str(path))
        check_overview(out)
        assert out["messages"]
    assert [(m["id"], m["position"], m["enabled"]) for m in out["mods"]] == [
        ("Sharper Swords", 1, True), ("Menus", 2, True), ("Faster Gathering", 3, True)]
    swords, menus, code = out["mods"]
    assert (swords["version"], swords["author"], swords["wins"], swords["loses"]) == ("1.2", "someone", 0, 1)
    assert (menus["wins"], menus["loses"]) == (1, 0)
    assert code["code"]["changes_code"] is True and swords["code"] is None

    # which mod wins a conflict (the details' list)
    out = mods(user, "conflicts")
    assert [(c["path"], c["winner"], c["mods"]) for c in out["conflicts"]] == [
        ("data/pl_swo010.arc", "Menus", ["Sharper Swords", "Menus"])]

    # on/off and the order, by the row's switch and arrows
    check_overview(mods(user, "disable", "Menus"))
    out = mods(user, "enable", "Menus")
    assert out["mods"][1]["enabled"] is True
    out = mods(user, "order", "Sharper Swords", "2")
    assert [m["id"] for m in out["mods"]] == ["Menus", "Sharper Swords", "Faster Gathering"]

    # the state of Azahar's folder and of the code library (status), with a file of the player's
    # own where a mod goes
    write(user.joinpath("load", "mods", "0004000000126300", "romfs", "data", "pl_swo010.arc"), b"mine")
    out = mods(user, "status")
    assert out["deploy"]["state"] == "blocked"
    assert out["deploy"]["in_the_way"] == ["romfs/data/pl_swo010.arc"] and out["deploy"]["problems"] == []
    assert out["code_library"]["state"] == "missing" and isinstance(out["code_library"]["reason"], str)

    # deploying on Play: the files in the way are named, then replaced when the player agrees
    out = mods(user, "deploy", code=1)
    assert out["error"]["code"] == "foreign_files"
    assert out["error"]["files"] == ["romfs/data/pl_swo010.arc"] and out["error"]["message"]
    out = mods(user, "deploy", "--force")
    assert out["deploy"]["state"] == "up_to_date"
    assert out["code_library"]["state"] == "missing"

    # two code mods: the reason it can't deploy
    d = tmp_path / "Other code"
    write(d / "code.ips", b"PATCH" + (0x20).to_bytes(3, "big") + (1).to_bytes(2, "big") + b"\x41EOF")
    out = mods(user, "add", str(d))
    assert out["mods"][-1]["enabled"] is False          # added off: one code mod at a time
    out = mods(user, "enable", "Other code", code=1)
    assert out["error"]["code"] == "code_mod_conflict" and out["error"]["message"]

    out = mods(user, "remove", "Other code")
    check_overview(out)
    assert [m["id"] for m in out["mods"]] == ["Menus", "Sharper Swords", "Faster Gathering"]


def test_catalog_json_and_progress_lines_the_launcher_reads(tmp_path):
    """The Mods screen's catalog (screen_mods.cpp, ApplyCatalog) and an install's progress lines
    (tools.cpp, ModsProgress; WithoutProgress leaves them out of the result)."""
    user = tmp_path / "azahar"
    user.mkdir()
    out = mods(user, "catalog", "--catalog-url", "")
    for key, kind in (("catalog", dict), ("entries", list), ("textures", list), ("textures_folder", str),
                      ("downloads_folder", str), ("warnings", list)):
        assert isinstance(out[key], kind), key
    assert isinstance(out["catalog"]["source"], str) and isinstance(out["catalog"]["updated"], str)
    for e in out["entries"]:
        for key, kind in (("id", str), ("kind", str), ("name", str), ("author", str), ("version", str),
                          ("description", str), ("details", str), ("category", str), ("region", str),
                          ("region_note", str), ("license", str), ("page_url", str), ("note", str),
                          ("recommended", bool), ("direct", bool), ("update", bool), ("needs", list),
                          ("conflicts", list), ("downloaded", int)):
            assert isinstance(e[key], kind), (e["id"], key)
        assert e["installed"] is None and e["size"] is None or isinstance(e["size"], int)
    assert any(e["recommended"] and e["kind"] == "textures" for e in out["entries"])

    # an install from a catalog of the test's own: progress lines, then the result
    pack = tmp_path / "pack.zip"
    import zipfile
    with zipfile.ZipFile(pack, "w") as z:
        z.writestr("pack.json", "{}")
        z.writestr("UI/tex1_8x8_ABCD_13.png", b"png" * 100)
    cat = tmp_path / "catalog.json"
    cat.write_text(json.dumps({"updated": "2026-01-01", "mods": [{
        "id": "pack", "kind": "textures", "name": "Pack", "page_url": "https://example.org/pack",
        "needs": ["custom_textures"], "downloads": [{"url": pack.as_uri(), "size": pack.stat().st_size}]}]}))
    r = subprocess.run([sys.executable, "-u", os.path.join(TOOLS, "mods.py"), "--json", "--user-dir", str(user),
                        "install-from-catalog", "pack", "--catalog", str(cat)], capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 0, (r.stdout, r.stderr)
    lines = r.stdout.splitlines()
    events = [json.loads(line[1:]) for line in lines if line.startswith("@")]
    assert events and all(isinstance(e["event"], str) for e in events)
    for e in events:
        if "done" in e:                 # an update: how far, of what
            assert isinstance(e["done"], int) and isinstance(e["what"], str)
            assert e["total"] is None or isinstance(e["total"], int)
        else:                           # a stage starts: what it is
            assert isinstance(e["message"], str)
    result = json.loads("\n".join(line for line in lines if not line.startswith("@")))
    assert result["ok"] is True and isinstance(result["messages"], list)
    assert result["needs"] == ["custom_textures"]
    out = mods(user, "catalog", "--catalog", str(cat))
    installed = out["entries"][0]["installed"]
    assert installed["as"] == "textures" and isinstance(installed["id"], str)
    check_overview(mods(user, "remove", installed["id"]))
    # a failed install: still one JSON object after the progress lines, with an error code
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"updated": "x", "mods": [{
        "id": "rar", "kind": "textures", "name": "Rar", "page_url": "https://example.org/rar",
        "downloads": [{"url": write(tmp_path / "x.rar", b"Rar!\x1a\x07\x00rest").as_uri()}]}]}))
    r = subprocess.run([sys.executable, "-u", os.path.join(TOOLS, "mods.py"), "--json", "--user-dir", str(user),
                        "install-from-catalog", "rar", "--catalog", str(bad)], capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 1
    err = json.loads("\n".join(line for line in r.stdout.splitlines() if not line.startswith("@")))["error"]
    assert err["code"] == "manual_extract" and err["download"] and err["target"] and err["page_url"]


LAUNCHER = os.path.join(ROOT, "port", "app", "launcher")


def launcher_source(name):
    with open(os.path.join(LAUNCHER, name), encoding="utf-8") as f:
        return f.read()


def call_at(text, start):
    """The call that starts at text[start] (a name, then its arguments), to its closing parenthesis."""
    depth, end = 0, text.index("(", start)
    while True:
        depth += {"(": 1, ")": -1}.get(text[end], 0)
        end += 1
        if depth == 0:
            return text[start:end]


def function_at(text, signature):
    """A function, from its signature to the brace that closes it (at the start of a line)."""
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 2]


def test_a_mods_result_for_the_game_chosen_before_is_left_out():
    """The launcher's Runner runs mods.py one command at a time, each with the --program-id of the
    game chosen when it was queued (screen_mods.cpp, ModsGame). One that comes back after another
    game was chosen is left out, as State::ChooseGame reads the new game's mods again: a list for
    the game before would set the library and its deploy_state, and Play, finding it up to date,
    would start the new game without putting its mods in place. ChooseGame reads them again also
    when a job ended while the picker was open (Play's deploy can run under it): such a job keeps
    its kind until its window, which waits for the picker, is closed. A deploy for the game before
    doesn't go on to play, the code check of the game before is left out as well, and Play's later
    stages go on only with the game it was pressed for. Read from the source: the tests don't run
    the launcher."""
    mods_cpp = launcher_source("screen_mods.cpp")
    queued = [call_at(mods_cpp, m.start()) for m in re.finditer(r"\bs\.runner\.Queue\(", mods_cpp)]
    assert len(queued) >= 5                         # list, conflicts, status, catalog, a change
    for call in queued:
        assert re.search(r"Tools::ModsCommand\([^;]*, game\)", call), call    # the game when queued
        assert "[&s, game]" in call, call
        assert re.search(r"\bgame [=!]= ModsGame\(s\)|HandleOverview\(s, game, ", call), call
    # (the library and its error, only for the game chosen now)
    handle = function_at(mods_cpp, "void HandleOverview(")
    assert "const bool current = game == ModsGame(s);" in handle
    assert re.search(r"if \(current\) \{\s*ApplyOverview\(", handle)
    assert re.search(r"if \(!s\.mods\.loaded && current\) \{\s*s\.mods\.error = ", handle)
    # a deploy for the game before: left out before its status is applied or Play goes on; the new
    # game's mods are read, and Play waits for them
    deploy = function_at(mods_cpp, "void DeployMods(")
    assert re.search(r"ModsCommand\([^;]*, game\)", deploy) and "s.job.on_done = [&s, then, game]" in deploy
    done = deploy[deploy.index("s.job.on_done"):]
    left_out = re.search(r"\n        if \(game != ModsGame\(s\)\) \{\n(.*?)\n        \}\n", done, re.S)
    assert left_out and left_out.start() < done.index("ApplyStatus(")
    assert [line.strip() for line in left_out.group(1).splitlines() if not line.strip().startswith("//")] == [
        "s.mods.loaded = false;", "s.mods.error.clear();", "RefreshMods(s);", "return;"]
    launcher_cpp = launcher_source("launcher.cpp")
    choose = function_at(launcher_cpp, "void State::ChooseGame(")
    assert re.search(r"if \(ToolsReady\(\) && \(!job\.Active\(\) \|\| job\.finished\)\) \{\s*mods\.loaded = false;",
                     choose)
    assert choose.index("mods.loaded = false;") < choose.index("RefreshMods(*this);")
    # the code check: ChooseGame checks the new game before its mods are read (which Play waits
    # for), and a check's result counts only for the game it checked
    assert choose.index("CheckCode();") < choose.index("RefreshMods(*this);")
    check = function_at(launcher_cpp, "void State::CheckCode(")
    assert re.search(r"const std::string checked = config\.game;\s*runner\.Queue\(\{[^}]*\"--check-code\", checked\}",
                     check)
    assert re.search(r"\[this, checked\]\(int exit_code, const std::string& output\) \{\s*"
                     r"if \(config\.game != checked\) \{(\s*//[^\n]*)*\n\s*return;\s*\}\s*"
                     r"code = Checks::CodeFromCheck\(", check)
    play = function_at(launcher_cpp, "void State::Play(int stage)")
    assert re.search(r"play_game = config\.game;\s*stage = 1;\s*\} else if \(config\.game != play_game\) \{"
                     r"(\s*//[^\n]*)*\s*return;\s*\}", play)


def test_with_no_game_chosen_the_mods_screen_goes_by_the_region_mods_py_takes(tmp_path):
    """With no game chosen the launcher passes no --program-id, and mods.py goes by the region whose
    update is installed, USA's when both or neither are (detect_game), as the setup's update check
    does (s.update.region; nullptr while neither is installed). The empty library names that
    region's title folder, the only one add takes then. While neither update is installed, the
    catalog says that a texture pack goes in the USA game's folder, which the European game,
    chosen later, doesn't read. Read from the source."""
    import mods
    assert mods.detect_game(str(tmp_path)) == MH4U                  # neither update installed: USA
    mods_cpp = launcher_source("screen_mods.cpp")
    empty = mods_cpp[mods_cpp.index("if (s.mods.loaded && s.mods.mods.empty()) {"):]
    empty = empty[:empty.index("Ui::EndCard();")]
    assert "const Checks::Region& game = s.update.region ? *s.update.region : Checks::REGIONS[0];" in empty
    assert 'const std::string titles = fmt::format("{:016X}/", game.program_id);' in empty
    assert '"layout (romfs/, exefs/), a title folder (" + titles + ", luma/titles/...)' in empty
    catalog = function_at(mods_cpp, "void DrawCatalog(")
    note = re.search(r"\n    if \(!s\.update\.region\) \{\n(.*?)\n    \}\n", catalog, re.S)
    assert note and note.start() < catalog.index('"Add a texture pack..."')
    assert re.search(r'Ui::StatusLine\(Ui::Status::Warn, "No game chosen yet",\s*"A texture pack goes in the USA '
                     r"game's texture folder", note.group(1))


def fake_exefs(code: bytes) -> bytes:
    header = bytearray(0x200)
    struct.pack_into("<8sII", header, 0, b".code", 0, len(code))
    return bytes(header) + code + bytes(-len(code) % 0x200)


def synthetic_cxi(path, program_id=MH4U):
    import make_cxi
    text = struct.pack("<4I", 0xE3A0002A, 0xE12FFF1E, 0xE0800001, 0xE12FFF1E).ljust(0x1000, b"\0")
    exheader = bytearray(0x800)
    exheader[0:8] = b"SYNTH\0\0\0"
    struct.pack_into("<III", exheader, 0x10, 0x00100000, 1, len(text))
    struct.pack_into("<III", exheader, 0x20, 0x00101000, 1, 0x1000)
    struct.pack_into("<III", exheader, 0x30, 0x00102000, 1, 0x1000)
    struct.pack_into("<Q", exheader, 0x200, program_id)
    path.write_bytes(make_cxi.make_cxi(bytes(exheader), fake_exefs(text + bytes(0x2000))))
    return path


@pytest.mark.skipif(shutil.which("clang") is None, reason="clang not installed")
def test_build_progress_lines(tmp_path):
    """The lines launcher/tools.cpp (BuildProgress) turns into the progress bar's stages."""
    cxi = synthetic_cxi(tmp_path / "synthetic.cxi")
    functions = tmp_path / "functions.tsv"
    functions.write_text("address\tsize\tmode\tkind\tname\tstatus\towner\tnotes\n"
                         "0x00100000\t0x8\tarm\tunknown\t\ttodo\t\t\n0x00100008\t0x8\tarm\tunknown\t\ttodo\t\t\n")
    install = tmp_path / "azahar"
    r = subprocess.run([sys.executable, "-u", os.path.join(TOOLS, "build_game.py"), "--dump", str(cxi),
                        "--out", str(tmp_path / "out"), "--opt", "O0", "--any-version", "--functions",
                        str(functions), "--extra", "", "--hooks", "", "--install", str(install), "-j", "1"],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stdout + r.stderr
    lines = r.stdout.splitlines()
    starts = ("extracting ", "recompiling", "compiling with ", "linked ", "installed to ")
    for start in starts:
        assert any(line.startswith(start) for line in lines), start
    # in that order
    found = [next(i for i, line in enumerate(lines) if line.startswith(s)) for s in starts]
    assert found == sorted(found)
    assert any(re.fullmatch(r"  compiled (\d+)/\1 \(\d+s\)", line) for line in lines)
    assert any(re.fullmatch(r"\d+ functions, \d+ hook points", line) for line in lines)
    # the library is where the launcher looks for it (launcher/checks.cpp, LibraryPath)
    ext = "dll" if sys.platform.startswith("win") else "dylib" if sys.platform == "darwin" else "so"
    assert (install / "recomp" / f"{MH4U:016X}.{ext}").is_file()


APP = os.environ.get("MH4U_APP")


@pytest.mark.skipif(not APP, reason="the app isn't built (set MH4U_APP to mh4u)")
def test_code_check_without_a_library(tmp_path):
    """mh4u --check-code: no library for MH4U, so the notice and status 2 (launcher.h)."""
    game = synthetic_cxi(tmp_path / "game.cxi")
    user = tmp_path / "azahar"
    user.mkdir()
    r = subprocess.run([APP, "--user", str(user), "--check-code", str(game)], capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 2, r.stdout + r.stderr
    assert r.stdout.startswith(f"backend: no recompiled code for {MH4U:016X}")
    assert "notice:\nThe MH4U PC port's code library isn't installed" in r.stdout
    # its log is a file of its own, beside the app's
    assert (user / "log" / "mh4u_check_log.txt").is_file()
    assert not (user / "log" / "mh4u_log.txt").exists()


@pytest.mark.skipif(not APP, reason="the app isn't built (set MH4U_APP to mh4u)")
@pytest.mark.skipif(shutil.which("clang") is None, reason="clang not installed")
def test_code_check_with_another_programs_library(tmp_path):
    """A library built from other code than the game's: status 2 and why."""
    import build_game
    game = synthetic_cxi(tmp_path / "game.cxi")
    other = synthetic_cxi(tmp_path / "other.cxi")
    data = bytearray(other.read_bytes())
    # other code at the same program id: the library won't match the game's code
    data[-0x2200] ^= 0xFF
    other.write_bytes(bytes(data))
    functions = tmp_path / "functions.tsv"
    functions.write_text("address\tsize\tmode\tkind\tname\tstatus\towner\tnotes\n"
                         "0x00100000\t0x8\tarm\tunknown\t\ttodo\t\t\n")
    user = tmp_path / "azahar"
    assert build_game.main(["--dump", str(other), "--out", str(tmp_path / "out"), "--opt", "O0", "--any-version",
                            "--functions", str(functions), "--extra", "", "--hooks", "", "--install", str(user),
                            "--no-mods", "-j", "1"]) == 0
    r = subprocess.run([APP, "--user", str(user), "--check-code", str(game)], capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 2, r.stdout + r.stderr
    assert "isn't the code that was recompiled" in r.stdout
    assert "notice:\nThe game's code isn't the code the MH4U PC port's library was built from" in r.stdout
