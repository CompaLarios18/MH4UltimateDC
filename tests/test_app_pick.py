"""port/app/launcher/pick.cpp on Linux: the system's file dialog (zenity, else kdialog) run as a
program of its own and followed without waiting, and what the launcher makes of its end. A path
chosen; a cancel; a dialog that never showed, said it couldn't, crashed or ended with another
status (the launcher's own browser takes over then, and the log gets what it said); a dialog
ended from the launcher; the AppImage's own libraries kept out of its environment; Steam's Game
Mode. The dialogs are shell scripts of the test's own on an otherwise empty PATH."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
SLEEP = shutil.which("sleep")
pytestmark = pytest.mark.skipif(CXX is None or SLEEP is None or not sys.platform.startswith("linux"),
                                reason="needs a C++ compiler and a shell (zenity and kdialog: Linux)")

DRIVER = r"""
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "launcher/pick.h"

using namespace Launcher::Pick;

// available, gamemode: 1 or 0
// file|folder START SELECT [CANCEL_MS]: the dialog for a game file (or a folder), ended from here
// after CANCEL_MS; then its result: available, program, path, error, a line each
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "available") {
        std::printf("%d\n", Available() ? 1 : 0);
        return 0;
    }
    if (mode == "gamemode") {
        std::printf("%d\n", GameMode() ? 1 : 0);
        return 0;
    }
    Request request;
    request.title = "Your game";
    request.kind = "Monster Hunter 4 Ultimate";
    request.extensions = {"3ds", "cci"};
    request.folder = mode == "folder";
    request.start = argv[2];
    request.select = argv[3];
    const int cancel_ms = argc > 4 ? std::atoi(argv[4]) : -1;
    Dialog dialog;
    dialog.Start(request);
    const auto start = std::chrono::steady_clock::now();
    bool cancelled = false;
    while (!dialog.Poll()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (cancel_ms >= 0 && !cancelled && ms >= cancel_ms) {
            dialog.Cancel();
            cancelled = true;
        }
        if (ms > 20000) {
            std::printf("timeout\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Result& r = dialog.Get();
    std::printf("available\t%d\nprogram\t%s\npath\t%s\nerror\t%s\n", r.available ? 1 : 0, r.program.c_str(),
                r.path.c_str(), r.error.c_str());
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("pick")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-pthread", "-I", APP, str(src), os.path.join(APP, "launcher", "pick.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def dialogs(tmp_path, **scripts):
    """A PATH folder with these programs (shell scripts) and nothing else."""
    folder = tmp_path / "bin"
    folder.mkdir(exist_ok=True)
    for name, body in scripts.items():
        program = folder / name
        program.write_text("#!/bin/sh\n" + body.replace("sleep", SLEEP) + "\n")
        program.chmod(0o755)
    return folder


def output(driver, path_folder, *args, **env) -> str:
    return subprocess.run([str(driver), *map(str, args)], capture_output=True, text=True, check=True,
                          env={"PATH": str(path_folder), **env}, timeout=60).stdout


def run(driver, path_folder, *args, **env) -> dict[str, str]:
    out = output(driver, path_folder, *args, **env)
    return dict(line.split("\t", 1) for line in out.splitlines() if "\t" in line)


def test_a_path_chosen_and_what_zenity_is_asked(driver, tmp_path):
    asked = tmp_path / "asked"
    folder = dialogs(tmp_path, zenity=f'printf "%s\\n" "$@" > "{asked}"\necho /games/mh4u.3ds')
    r = run(driver, folder, "file", "/start/here", "mh4u.3ds")
    assert r == {"available": "1", "program": "zenity", "path": "/games/mh4u.3ds", "error": ""}
    assert asked.read_text().splitlines() == [
        "--file-selection", "--title=Your game",
        "--file-filter=Monster Hunter 4 Ultimate | *.3ds *.3DS *.cci *.CCI", "--file-filter=All files | *",
        "--filename=/start/here/mh4u.3ds"]
    # a folder: zenity's own, opening in the folder
    run(driver, folder, "folder", "/start/here", "")
    assert asked.read_text().splitlines() == [
        "--file-selection", "--title=Your game", "--directory", "--filename=/start/here/"]


def test_kdialog_when_there_is_no_zenity(driver, tmp_path):
    asked = tmp_path / "asked"
    folder = dialogs(tmp_path, kdialog=f'printf "%s\\n" "$@" > "{asked}"\necho /games/x.cci')
    r = run(driver, folder, "file", "/start", "")
    assert (r["program"], r["path"], r["error"]) == ("kdialog", "/games/x.cci", "")
    assert asked.read_text().splitlines() == [
        "--title", "Your game", "--getopenfilename", "/start/", "*.3ds *.3DS *.cci *.CCI|Monster Hunter 4 Ultimate"]


def test_no_dialog_at_all(driver, tmp_path):
    folder = dialogs(tmp_path)
    assert output(driver, folder, "available") == "0\n"
    r = run(driver, folder, "file", "", "")
    assert (r["available"], r["path"], r["error"]) == ("0", "", "")
    assert output(driver, dialogs(tmp_path, kdialog="exit 1"), "available") == "1\n"


def test_a_cancel_is_no_failure(driver, tmp_path):
    folder = dialogs(tmp_path, zenity="sleep 1\nexit 1")
    r = run(driver, folder, "file", "", "")
    assert (r["path"], r["error"]) == ("", "")


@pytest.mark.parametrize("body, said", [
    # gone before it could have shown: Steam's Game Mode, no display, a portal that isn't there
    ("echo 'Gtk-WARNING **: cannot open display: :0' >&2\nexit 1", "cannot open display"),
    ("exit 1", "status 1"),
    # it showed nothing, then said why
    ("sleep 1\necho 'qt.qpa.xcb: could not connect to display' >&2\nexit 1", "could not connect to display"),
    # crashed
    ("echo 'Service crash' >&2\nulimit -c 0\nkill -SEGV $$", "signal 11"),
    # another status
    ("echo 'zenity: unexpected error' >&2\nexit 255", "status 255"),
])
def test_a_dialog_that_didnt_work_says_why(driver, tmp_path, body, said):
    folder = dialogs(tmp_path, zenity=body)
    r = run(driver, folder, "file", "", "")
    assert r["path"] == "" and said in r["error"]
    if ">&2" in body:
        # what it wrote on its error output, for the log
        assert "saying: " in r["error"]


def test_a_dialog_ended_from_the_launcher(driver, tmp_path):
    # (one that never shows, as in Steam's Game Mode: Cancel or the launcher's browser ends it)
    folder = dialogs(tmp_path, zenity="sleep 30\necho /never")
    started = time.monotonic()
    r = run(driver, folder, "file", "", "", 200)
    assert (r["path"], r["error"]) == ("", "")
    assert time.monotonic() - started < 10


def test_the_appimages_own_libraries_stay_out(driver, tmp_path):
    seen = tmp_path / "seen"
    folder = dialogs(tmp_path, zenity=f'echo "LD=$LD_LIBRARY_PATH PRE=${{LD_PRELOAD-unset}}" > "{seen}"\necho /x.3ds')
    mount = tmp_path / ".mount_mh4u"
    r = run(driver, folder, "file", "", "", APPDIR=str(mount),
            LD_LIBRARY_PATH=f"{mount}/usr/optional/libstdc++:/opt/steam/lib",
            LD_PRELOAD=f"{mount}/usr/optional/exec.so")
    assert r["path"] == "/x.3ds"
    assert seen.read_text().strip() == "LD=/opt/steam/lib PRE=unset"


def test_steams_game_mode(driver, tmp_path):
    folder = dialogs(tmp_path)

    def game_mode(**env):
        return output(driver, folder, "gamemode", **env) == "1\n"
    assert game_mode(GAMESCOPE_WAYLAND_DISPLAY="gamescope-0")
    assert game_mode(XDG_CURRENT_DESKTOP="gamescope")
    assert game_mode(SteamGamepadUI="1")
    assert not game_mode(XDG_CURRENT_DESKTOP="KDE") and not game_mode()
