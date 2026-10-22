#!/usr/bin/env python3
"""Set the native runtime up from the player's own files (N3-17).

usage: setup_native.py --user DIR --game GAME [--update UPDATE] [--hd | --no-hd]
                       [--hdpack PATH] [--check] [--json]

Three steps, in this order; each is skipped when it is already done and redone when it is
stale (D61's paths, under the user folder DIR):

  game     DIR/native/game/                the game folder (tools/native/prepare_game.py) made
                                           from GAME and UPDATE; skipped while its info.txt
                                           names the same two input files (path and size) and
                                           its files and code.bin still match it.
  library  DIR/recomp/<program id>_native.<dll|so|dylib>
                                           the native code library, built by
                                           tools/build_game.py --dump UPDATE --native
                                           --install DIR --fetch-compiler; skipped when
                                           tools/mods.py status --native says ok.
  hd       DIR/load/textures/<program id>.mh4hd
                                           the HD archive (D14: on unless --no-hd): the
                                           catalog pack mh4u-hd is installed when it isn't
                                           (tools/mods.py install-from-catalog, a download of
                                           about 12 GB that resumes), then mh4n_hdpack turns it
                                           into the archive; skipped while the archive's
                                           .source.json matches the pack folder and the tool.

GAME is the base game (.3ds, .cci, .cxi) and UPDATE the v1.3.0 update (a .cia) or its installed
content; without --update, the content Azahar's setup left under DIR/sdmc is used. mh4n_hdpack
is --hdpack PATH, else $MH4N_HDPACK, else the one on PATH.

--check says, for each step, done, missing or stale, and changes nothing (it creates no file and
no folder). With --json every progress update is one line "@{...}" on standard output, as
tools/mods.py writes them for the launcher, followed by one JSON object with the result; without
it the progress is plain text.

Exit status: 0 everything is done (--check: the question was answered), 1 a step failed (the error
code in the JSON is mods.py's for a failed install, else prepare_failed, build_failed,
hdpack_failed, ...), 2 bad arguments.

The folders it writes hold game data: they stay in the user folder, never commit or share them.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mods  # noqa: E402
import prepare_game  # noqa: E402

STEPS = ("game", "library", "hd")
TITLES = {"game": "Preparing the game files", "library": "Building the native code library",
          "hd": "Setting up the HD textures"}
HD_PACK = "mh4u-hd"                 # the catalog id of the HD texture pack
SOURCE_FORMAT = 1
GAME_FILES = ("exheader.bin", "code.bin", "romfs.bin", "update_romfs.bin")
THROTTLE = 0.25                     # seconds between two progress lines of a child's output
TOOL_NAME = "mh4n_hdpack"

_INPUT = re.compile(r"^input (base  |update): (.*), 0x([0-9A-Fa-f]+) bytes$")
_SIZE = re.compile(r"^([a-z_]+\.bin) *: 0x([0-9A-Fa-f]+) bytes")
_CRC = re.compile(r"CRC-32 ([0-9A-Fa-f]{8})")
_HDPACK = re.compile(r"^hdpack: (\d+)/(\d+) (\w+)")


class SetupError(Exception):
    """A step failed or the arguments are wrong: a code for the launcher, a message for the player."""

    def __init__(self, code: str, message: str, **details):
        super().__init__(message)
        self.code, self.message, self.details = code, message, details


# ── paths ───────────────────────────────────────────────────────────────────

def game_folder(user: str) -> str:
    return os.path.join(user, "native", "game")


def hd_archive(user: str) -> str:
    return mods.textures_folder(user) + ".mh4hd"


def find_installed_update(user: str) -> str | None:
    """The update's content Azahar's setup put under the virtual SD card, the newest if several."""
    title = f"{mods.UPDATE_ID:016x}"
    pattern = os.path.join(glob.escape(user), "sdmc", "Nintendo 3DS", "*", "*", "title", title[:8], title[8:],
                           "content", "*.app")
    found = [p for p in glob.glob(pattern) if os.path.isfile(p)]
    return max(sorted(found), key=os.path.getmtime) if found else None


def find_hdpack(given: str | None) -> list[str] | None:
    """The command that runs mh4n_hdpack: --hdpack, $MH4N_HDPACK, or PATH's; None if there is none."""
    path = given or os.environ.get("MH4N_HDPACK") or shutil.which(TOOL_NAME)
    if not path or not os.path.isfile(path):
        return None
    return [sys.executable, path] if path.lower().endswith(".py") else [path]


def split_command(text: str | None, default: list[str]) -> list[str]:
    return shlex.split(text, posix=os.name != "nt") if text else default


# ── events ──────────────────────────────────────────────────────────────────

class Events:
    """The progress of the run. With --json each update is one line "@{...}" on standard output
    (port/app/launcher/tools.cpp reads the last one: "message" says what is going on, "done" and
    "total" how far, as tools/mods.py's); otherwise plain text."""

    def __init__(self, as_json: bool, stream=None):
        self.as_json = as_json
        self.stream = stream or sys.stdout

    def emit(self, event: str, step: str, **data) -> None:
        if self.as_json:
            self.stream.write("@" + json.dumps(dict(data, event=event, step=step), ensure_ascii=False) + "\n")
        else:
            text = data.get("message")
            if text is None and "done" in data:
                text = f"{data.get('what', 'progress')}: {data['done']} of {data.get('total')}"
            if text:
                self.stream.write(text + "\n")
        self.stream.flush()

    def step(self, step: str, state: str, index: int, count: int, message: str, **data) -> None:
        """A step starts (state running) or ends (done, skipped, failed)."""
        self.emit("step", step, state=state, index=index, steps=count, message=message, **data)


# ── running the other tools ─────────────────────────────────────────────────

def run_child(cmd: list[str], on_line) -> tuple[int, list[str]]:
    """Run cmd, calling on_line for each line it writes (its two streams together); returns its
    exit status and its lines. A missing program is a SetupError."""
    env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8")
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
    except OSError as e:
        raise SetupError("not_found", f"{cmd[0]}: {e}") from e
    lines: list[str] = []
    assert proc.stdout is not None
    for raw in proc.stdout:
        line = raw.rstrip("\r\n")
        lines.append(line)
        on_line(line)
    return proc.wait(), lines


def last_line(lines: list[str]) -> str:
    for line in reversed(lines):
        if line.strip() and not line.startswith("@"):
            return line.strip()
    return "it ended without saying why"


def mods_json(command: list[str], user: str, *args: str, on_line=lambda line: None) -> dict:
    """Run tools/mods.py --json and return its result object; a failure raises with mods.py's own
    error code."""
    cmd = command + ["--json", "--user-dir", user, *args]
    rc, lines = run_child(cmd, on_line)
    body = "\n".join(line for line in lines if not line.startswith("@"))
    try:
        out = json.loads(body)
    except ValueError:
        out = None
    if not isinstance(out, dict):
        raise SetupError("mods_failed", f"mods.py {args[0]}: {last_line(lines)}")
    if rc != 0 or not out.get("ok", False):
        err = out.get("error") if isinstance(out.get("error"), dict) else {}
        details = {k: v for k, v in err.items() if k not in ("code", "message")}
        raise SetupError(str(err.get("code") or "mods_failed"), str(err.get("message") or last_line(lines)),
                         **details)
    return out


class Throttle:
    """Passes on a child's lines a few times a second (a build prints thousands), and the last one
    at the end."""

    def __init__(self, send):
        self.send, self.last, self.pending = send, 0.0, None

    def __call__(self, line: str) -> None:
        if not line.strip():
            return
        now = time.monotonic()
        if now - self.last >= THROTTLE:
            self.last, self.pending = now, None
            self.send(line)
        else:
            self.pending = line

    def flush(self) -> None:
        if self.pending is not None:
            self.send(self.pending)
            self.pending = None


# ── step 1: the game folder ─────────────────────────────────────────────────

def read_info(folder: str) -> list[str] | None:
    try:
        with open(os.path.join(folder, "info.txt"), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return None


def same_file(a: str, b: str) -> bool:
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def game_state(user: str, game: str, update: str) -> tuple[str, str]:
    """done, missing or stale for the game folder, and why."""
    folder = game_folder(user)
    info = read_info(folder)
    if info is None:
        return "missing", "the game folder has not been made"
    inputs, sizes, crc = {}, {}, None
    for line in info:
        m = _INPUT.match(line)
        if m:
            inputs[m.group(1).strip()] = (m.group(2), int(m.group(3), 16))
            continue
        m = _SIZE.match(line)
        if m:
            sizes[m.group(1)] = int(m.group(2), 16)
            if m.group(1) == "code.bin":
                c = _CRC.search(line)
                crc = int(c.group(1), 16) if c else None
    for key, path, label in (("base", game, "base game"), ("update", update, "update")):
        if key not in inputs:
            return "stale", "its info.txt does not say which files it was made from"
        recorded, size = inputs[key]
        if not same_file(recorded, path):
            return "stale", f"it was made from another {label} file"
        if os.path.getsize(path) != size:
            return "stale", f"the {label} file has changed size"
    for name in GAME_FILES:
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            return "stale", f"{name} is missing"
        if name in sizes and os.path.getsize(path) != sizes[name]:
            return "stale", f"{name} is not the size info.txt says"
    if "icon.bin" in sizes and not os.path.isfile(os.path.join(folder, "icon.bin")):
        return "stale", "icon.bin is missing"
    if crc is None or prepare_game.crc32_of_file(os.path.join(folder, "code.bin")) != crc:
        return "stale", "code.bin is not the one info.txt describes"
    return "done", "made from the same files"


def run_game(user: str, game: str, update: str, versions: str) -> None:
    folder = game_folder(user)
    try:
        os.makedirs(os.path.dirname(folder), exist_ok=True)
        # info.txt is written last: if this fails half way the folder reads as missing, not as done
        try:
            os.remove(os.path.join(folder, "info.txt"))
        except FileNotFoundError:
            pass
        prepare_game.prepare(game, update, folder, versions)
    except prepare_game.PrepareError as e:
        raise SetupError("prepare_failed", str(e)) from e
    except OSError as e:
        raise SetupError("io", str(e)) from e


# ── step 2: the native code library ─────────────────────────────────────────

def library_state(mods_cmd: list[str], user: str) -> tuple[str, str]:
    out = mods_json(mods_cmd, user, "status", "--native")
    state = out.get("code_library", {})
    name, reason = state.get("state"), str(state.get("reason", ""))
    if name == "ok":
        return "done", reason
    if name == "missing":
        return "missing", reason
    return "stale", reason or f"the code library is {name}"


def run_library(build_cmd: list[str], mods_cmd: list[str], events: Events, user: str, update: str) -> None:
    throttle = Throttle(lambda line: events.emit("progress", "library", message=line))
    rc, lines = run_child(build_cmd + ["--dump", update, "--native", "--install", user, "--fetch-compiler"],
                          throttle)
    throttle.flush()
    if rc != 0:
        raise SetupError("build_failed", last_line(lines), exit_status=rc)
    state, reason = library_state(mods_cmd, user)
    if state != "done":
        raise SetupError("build_failed", f"the code library was built, but it is {state}: {reason}")


# ── step 3: the HD archive ──────────────────────────────────────────────────

def pack_fingerprint(folder: str) -> dict:
    """What the pack folder holds: its file count, total bytes and newest modification time."""
    count = total = newest = 0
    for top, _dirs, files in os.walk(folder):
        for name in files:
            try:
                st = os.stat(os.path.join(top, name))
            except OSError:
                continue
            count += 1
            total += st.st_size
            newest = max(newest, st.st_mtime_ns)
    return {"files": count, "bytes": total, "newest_mtime_ns": newest}


def tool_fingerprint(command: list[str] | None) -> dict | None:
    """The size and modification time of mh4n_hdpack: a new build of it may write another archive."""
    if not command:
        return None
    st = os.stat(command[-1])
    return {"size": st.st_size, "mtime_ns": st.st_mtime_ns}


def read_source(archive: str) -> dict | None:
    try:
        with open(archive + ".source.json", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def hd_state(user: str, hdpack: list[str] | None) -> tuple[str, str]:
    archive = hd_archive(user)
    if not os.path.isfile(archive) or os.path.getsize(archive) == 0:
        return "missing", "the HD archive has not been made"
    source = read_source(archive)
    if source is None or source.get("format") != SOURCE_FORMAT:
        return "stale", "the archive has no record of the pack it was made from"
    pack = pack_fingerprint(mods.textures_folder(user))
    for key, value in pack.items():
        if source.get(key) != value:
            return "stale", "the texture pack has changed since the archive was made"
    tool = tool_fingerprint(hdpack)
    if tool is not None and source.get("tool") != tool:
        return "stale", f"{TOOL_NAME} has changed since the archive was made"
    return "done", "made from the pack as it is now"


def run_hd(mods_cmd: list[str], hdpack: list[str], events: Events, user: str) -> None:
    folder, archive = mods.textures_folder(user), hd_archive(user)

    def on_mods_line(line: str) -> None:
        if not line.startswith("@"):
            return
        try:
            data = json.loads(line[1:])
        except ValueError:
            return
        if not isinstance(data, dict):
            return
        # mods.py numbers its own stages with "step": keep that as "part" (ours names the step)
        part, parts = data.pop("step", None), data.pop("steps", None)
        if part is not None:
            data["part"], data["parts"] = part, parts
        events.emit(str(data.pop("event", "progress")), "hd", **data)

    installed = pack_fingerprint(folder)["files"] > 0
    if installed:
        entries = mods_json(mods_cmd, user, "catalog").get("entries", [])
        installed = any(e.get("id") == HD_PACK and e.get("installed") for e in entries)
    if not installed:
        events.emit("progress", "hd", message="Downloading the HD texture pack")
        mods_json(mods_cmd, user, "install-from-catalog", HD_PACK, on_line=on_mods_line)

    for path in (archive + ".source.json",):       # a failed run must not leave a record that matches
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    events.emit("progress", "hd", message="Converting the textures")
    last = [0.0]

    def on_tool_line(line: str) -> None:
        m = _HDPACK.match(line)
        if m and time.monotonic() - last[0] >= THROTTLE:
            last[0] = time.monotonic()
            events.emit("install", "hd", what="Converting " + m.group(3), done=int(m.group(1)),
                        total=int(m.group(2)))

    os.makedirs(os.path.dirname(archive), exist_ok=True)
    rc, lines = run_child(hdpack + [folder, "-o", archive], on_tool_line)
    if rc != 0:
        raise SetupError("hdpack_failed" if rc != 4 else "interrupted", last_line(lines), exit_status=rc)
    if not os.path.isfile(archive):
        raise SetupError("hdpack_failed", f"{TOOL_NAME} ended without writing {os.path.basename(archive)}")
    record = dict(pack_fingerprint(folder), format=SOURCE_FORMAT, tool=tool_fingerprint(hdpack))
    tmp = archive + ".source.json.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
    os.replace(tmp, archive + ".source.json")


# ── the whole run ───────────────────────────────────────────────────────────

def plan(args) -> list[str]:
    return [s for s in STEPS if s != "hd" or args.hd]


def check_all(args, update: str, mods_cmd: list[str], hdpack: list[str] | None) -> list[dict]:
    out = []
    for step in plan(args):
        if step == "game":
            state, reason = game_state(args.user, args.game, update)
        elif step == "library":
            state, reason = library_state(mods_cmd, args.user)
        else:
            state, reason = hd_state(args.user, hdpack)
        out.append({"step": step, "state": state, "reason": reason})
    return out


def setup(args, events: Events, update: str, build_cmd: list[str], mods_cmd: list[str],
          hdpack: list[str] | None) -> list[dict]:
    steps = plan(args)
    results: list[dict] = []
    for index, step in enumerate(steps, 1):
        try:
            if step == "game":
                state, reason = game_state(args.user, args.game, update)
            elif step == "library":
                state, reason = library_state(mods_cmd, args.user)
            else:
                state, reason = hd_state(args.user, hdpack)
            if state == "done":
                events.step(step, "skipped", index, len(steps), f"{TITLES[step]}: already done", reason=reason)
                results.append({"step": step, "state": "skipped", "reason": reason})
                continue
            if step == "hd" and hdpack is None:
                raise SetupError("no_hdpack", f"{TOOL_NAME} was not found: pass --hdpack PATH, set $MH4N_HDPACK or "
                                              "put it on PATH (or turn the HD textures off with --no-hd)")
            events.step(step, "running", index, len(steps), TITLES[step], was=state)
            if step == "game":
                run_game(args.user, args.game, update, args.versions)
            elif step == "library":
                run_library(build_cmd, mods_cmd, events, args.user, update)
            else:
                run_hd(mods_cmd, hdpack, events, args.user)
        except SetupError as e:
            events.step(step, "failed", index, len(steps), e.message, error=dict(e.details, code=e.code))
            results.append({"step": step, "state": "failed"})
            e.details["step"] = step
            e.details["steps"] = results
            raise
        events.step(step, "done", index, len(steps), f"{TITLES[step]}: done")
        results.append({"step": step, "state": "done", "was": state})
    return results


def build_parser() -> argparse.ArgumentParser:
    class Parser(argparse.ArgumentParser):
        def error(self, message):
            raise SetupError("usage", message)

    ap = Parser(prog="setup_native.py", description=__doc__.splitlines()[0])
    ap.add_argument("--user", required=True, help="the user folder (D61); mh4u-native's --user")
    ap.add_argument("--game", required=True, help="the base game: .3ds, .cci or .cxi")
    ap.add_argument("--update", help="the v1.3.0 update (.cia), or its installed content (.app); default: the "
                                     "content found under the user folder")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--hd", dest="hd", action="store_true", default=True, help="the HD textures step (default, D14)")
    g.add_argument("--no-hd", dest="hd", action="store_false", help="leave the HD textures out")
    ap.add_argument("--hdpack", help=f"{TOOL_NAME} (default: $MH4N_HDPACK, then PATH)")
    ap.add_argument("--check", action="store_true", help="say what is done, missing or stale; change nothing")
    ap.add_argument("--json", action="store_true", help="progress as '@{...}' lines, then one JSON object")
    ap.add_argument("--versions", default=prepare_game.DEFAULT_VERSIONS, help=argparse.SUPPRESS)
    ap.add_argument("--build-command", help=argparse.SUPPRESS)
    ap.add_argument("--mods-command", help=argparse.SUPPRESS)
    return ap


def finish(as_json: bool, result: dict, rc: int) -> int:
    if as_json:
        print(json.dumps(result, indent=1))
    elif not result.get("ok"):
        print(f"setup_native.py: {result['error']['message']}", file=sys.stderr)
    return rc


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    as_json = "--json" in argv
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(errors="replace")
        except (ValueError, OSError):
            pass
    try:
        args = build_parser().parse_args(argv)
    except SetupError as e:
        return finish(as_json, {"ok": False, "command": "setup", "error": {"code": e.code, "message": e.message}}, 2)
    events = Events(as_json)
    result: dict = {"ok": True, "command": "setup", "check": args.check, "user": args.user}
    try:
        args.user = os.path.abspath(args.user)
        result["user"] = args.user
        for label, path in (("game", args.game), ("update", args.update)):
            if path and not os.path.isfile(path):
                raise SetupError("not_found", f"the {label} file {path} is not there")
        update = args.update or find_installed_update(args.user)
        if not update:
            raise SetupError("no_update", "no update was given and none is installed under the user folder: "
                                          "pass --update (the v1.3.0 update .cia)")
        build_cmd = split_command(args.build_command, [sys.executable, os.path.join(ROOT, "tools", "build_game.py")])
        mods_cmd = split_command(args.mods_command, [sys.executable, os.path.join(ROOT, "tools", "mods.py")])
        hdpack = find_hdpack(args.hdpack) if args.hd else None
        if args.check:
            result["steps"] = check_all(args, update, mods_cmd, hdpack)
            result["complete"] = all(s["state"] == "done" for s in result["steps"])
            if not as_json:
                for s in result["steps"]:
                    print(f"{s['step']}: {s['state']} ({s['reason']})")
        else:
            result["steps"] = setup(args, events, update, build_cmd, mods_cmd, hdpack)
            result["complete"] = True
        if not args.check and not as_json:
            print("The native runtime is set up.")
        return finish(as_json, result, 0)
    except SetupError as e:
        err = {"code": e.code, "message": e.message}
        result.update(ok=False, error=err)
        for key in ("step", "steps"):
            if key in e.details:
                result[key] = e.details.pop(key)
        err.update(e.details)
        return finish(as_json, result, 2 if e.code == "usage" else 1)


if __name__ == "__main__":
    sys.exit(main())
