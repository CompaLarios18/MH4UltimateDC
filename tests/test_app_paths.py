"""port/app/launcher/paths.cpp: the paths of the launcher's own file browser. A folder's entries
in the order it shows them (folders first, then the files that fit, names in a person's order,
hidden ones left out), a path as typed or pasted, the nearest folder that is there (where a
picker opens), Windows' network shares, and the places to start from: home, XDG's names for
Desktop and Downloads, and the removable drives of a mount table (a Steam Deck's SD card; network
shares, which aren't looked into). Synthetic folders and files only."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "launcher/paths.h"

using namespace Launcher::Paths;

static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < s.size()) {
        std::size_t end = s.find(',', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        if (end > start) {
            out.push_back(s.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

static std::string Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void Print(const std::vector<Place>& places) {
    for (const auto& p : places) {
        std::printf("%s\t%s\t%d\n", p.label.c_str(), p.path.c_str(), p.drive ? 1 : 0);
    }
}

// list DIR EXTS FILES: "D name" or "F name size" a line, then folder, left and error (yes or "")
// has NAME EXTS, less A B: 1 or 0
// clean, parent, name, what, nearest PATH: the answer
// typed TEXT BASE HOME: the path it reads
// userdir FILE KEY HOME, mounted FILE: from a file's text (mounted: and 1 for a network share)
// places: label, path and 1 for a drive, a line each
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "list") {
        const Listing l = List(argv[2], Split(argv[3]), std::atoi(argv[4]) != 0);
        for (const auto& e : l.entries) {
            if (e.folder) {
                std::printf("D\t%s\n", e.name.c_str());
            } else {
                std::printf("F\t%s\t%llu\n", e.name.c_str(), static_cast<unsigned long long>(e.size));
            }
        }
        std::printf("folder\t%s\nleft\t%llu\nerror\t%s\n", l.folder.c_str(),
                    static_cast<unsigned long long>(l.left_out), l.error.empty() ? "" : "yes");
    } else if (mode == "has") {
        std::printf("%d\n", HasExtension(argv[2], Split(argv[3])) ? 1 : 0);
    } else if (mode == "less") {
        std::printf("%d\n", NameLess(argv[2], argv[3]) ? 1 : 0);
    } else if (mode == "clean") {
        std::printf("%s\n", Clean(argv[2]).c_str());
    } else if (mode == "parent") {
        std::printf("%s\n", Parent(argv[2]).c_str());
    } else if (mode == "name") {
        std::printf("%s\n", Name(argv[2]).c_str());
    } else if (mode == "what") {
        const Kind k = What(argv[2]);
        std::printf("%s\n", k == Kind::Folder ? "folder" : k == Kind::File ? "file" : "missing");
    } else if (mode == "nearest") {
        std::printf("%s\n", NearestFolder(argv[2]).c_str());
    } else if (mode == "typed") {
        std::printf("%s\n", FromTyped(argv[2], argv[3], argv[4]).c_str());
    } else if (mode == "userdir") {
        std::printf("%s\n", UserDir(Read(argv[2]), argv[3], argv[4]).c_str());
    } else if (mode == "mounted") {
        for (const auto& p : Mounted(Read(argv[2]))) {
            std::printf("%s\t%s\t%d\t%d\n", p.label.c_str(), p.path.c_str(), p.drive ? 1 : 0, p.network ? 1 : 0);
        }
    } else if (mode == "places") {
        Print(Places());
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("paths")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    # (Windows: the known folders)
    libs = ["-lshell32", "-lole32"] if os.name == "nt" else []
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "launcher", "paths.cpp"),
                    "-o", str(exe), *libs], check=True)
    return exe


def run(driver, *args, env=None) -> str:
    return subprocess.run([str(driver), *map(str, args)], capture_output=True, text=True, check=True,
                          env=env).stdout


def lines(driver, *args, env=None) -> list[list[str]]:
    return [line.split("\t") for line in run(driver, *args, env=env).splitlines()]


def listing(driver, folder, extensions, files):
    out = lines(driver, "list", folder, extensions, 1 if files else 0)
    entries = [e for e in out if e[0] in ("D", "F")]
    info = {e[0]: e[1] for e in out if e[0] not in ("D", "F")}
    return entries, info


def test_folders_first_then_the_files_that_fit(driver, tmp_path):
    for name in ("b", "A", "Mod 10", "Mod 2", ".config"):
        (tmp_path / name).mkdir()
    for name, size in (("game.3DS", 4), ("Game.cci", 3), ("notes.txt", 2), (".hidden.3ds", 1),
                       ("update.CIA", 5)):
        (tmp_path / name).write_bytes(b"x" * size)
    entries, info = listing(driver, tmp_path, "3ds,cci", True)
    assert entries == [["D", "A"], ["D", "b"], ["D", "Mod 2"], ["D", "Mod 10"],
                       ["F", "game.3DS", "4"], ["F", "Game.cci", "3"]]
    assert info == {"folder": str(tmp_path), "left": "0", "error": ""}
    # a folder asked for: folders only; no extensions: every file that isn't hidden
    assert [e for e in listing(driver, tmp_path, "", False)[0] if e[0] == "F"] == []
    files = [e[1] for e in listing(driver, tmp_path, "", True)[0] if e[0] == "F"]
    assert files == ["game.3DS", "Game.cci", "notes.txt", "update.CIA"]


def test_a_folder_that_cant_be_read_says_so(driver, tmp_path):
    entries, info = listing(driver, tmp_path / "nowhere", "", True)
    assert entries == [] and info["error"] == "yes"


def test_extensions_in_any_case(driver):
    def has(name, extensions):
        return run(driver, "has", name, extensions).strip() == "1"
    assert has("MH4U.3DS", "3ds,cci") and has("pack.tar.gz", "zip,gz") and has("anything", "")
    assert not has("MH4U.3dsx", "3ds") and not has(".3ds", "3ds") and not has("cia", "cia")


def test_names_in_a_persons_order(driver):
    def less(a, b):
        return run(driver, "less", a, b).strip() == "1"
    assert less("Mod 2", "Mod 10") and not less("Mod 10", "Mod 2")
    assert less("apple", "Banana") and less("file9.zip", "file10.zip") and less("v1.2", "v1.10")
    assert not less("same", "same")
    # the same but for case or leading zeros: still one order
    assert less("Mod", "mod") != less("mod", "Mod")
    assert less("disc 01", "disc 1") != less("disc 1", "disc 01")


def test_parent_name_and_tidied_paths(driver, tmp_path):
    deep = tmp_path / "a" / "b"
    assert run(driver, "parent", deep).strip() == str(tmp_path / "a")
    assert run(driver, "name", deep).strip() == "b"
    sep = os.sep
    assert run(driver, "clean", f"{tmp_path}{sep}a{sep}.{sep}b{sep}..{sep}").strip() == str(tmp_path / "a")
    root = os.path.abspath(os.sep)  # "/", or the drive's root on Windows
    assert run(driver, "parent", root).strip() == ""
    assert run(driver, "name", root).strip() == root


def test_what_is_at_a_path(driver, tmp_path):
    (tmp_path / "update.cia").write_bytes(b"x")
    assert run(driver, "what", tmp_path).strip() == "folder"
    assert run(driver, "what", tmp_path / "update.cia").strip() == "file"
    assert run(driver, "what", tmp_path / "nothing").strip() == "missing"


def test_the_nearest_folder_that_is_there(driver, tmp_path):
    # where a picker opens: the last pick's folder, or above it once that has gone (moved, unplugged)
    games = tmp_path / "games"
    games.mkdir()
    (games / "mh4u.3ds").write_bytes(b"x")
    assert run(driver, "nearest", games).strip() == str(games)
    assert run(driver, "nearest", games / "mh4u.3ds").strip() == str(games)
    assert run(driver, "nearest", games / "moved" / "away").strip() == str(games)


def test_typed_and_pasted_paths(driver, tmp_path):
    home = tmp_path / "home"
    games = home / "My Games"
    games.mkdir(parents=True)
    (games / "mh4u.3ds").write_bytes(b"x")
    want = str(games / "mh4u.3ds")

    def typed(text, base=tmp_path):
        return run(driver, "typed", text, base, home).rstrip("\n")
    assert typed(f'"{want}"') == want                    # Windows' Copy as path
    assert typed(f"  '{want}'\t") == want                 # quotes and spaces around it
    assert typed("~/My Games/mh4u.3ds") == want           # ~ for home
    assert typed("My Games", base=home) == str(games)     # from the folder shown
    assert typed("My Games/../My Games/", base=home) == str(games)
    assert typed(games.as_uri() + "/mh4u.3ds") == want    # a file manager's copy
    assert typed("   ") == ""
    if os.name != "nt":
        assert typed(f"{home}/My\\ Games/mh4u.3ds") == want  # a shell's escapes


def test_xdg_names_for_desktop_and_downloads(driver, tmp_path):
    dirs = tmp_path / "user-dirs.dirs"
    dirs.write_text('# written by xdg-user-dirs-update\nXDG_DESKTOP_DIR="$HOME/Escritorio"\n'
                    'XDG_DOWNLOAD_DIR="/data/Descargas"\nXDG_DOWNLOAD_DIRS="$HOME/no"\n'
                    'XDG_MUSIC_DIR="$HOME/M\\"usica"\n')

    def userdir(key):
        return run(driver, "userdir", dirs, key, "/home/deck").rstrip("\n")
    assert userdir("DESKTOP") == "/home/deck/Escritorio"
    assert userdir("DOWNLOAD") == "/data/Descargas"
    assert userdir("MUSIC") == '/home/deck/M"usica'
    assert userdir("VIDEOS") == ""


def test_removable_drives_of_a_mount_table(driver, tmp_path):
    mounts = tmp_path / "mounts"
    mounts.write_text("/dev/nvme0n1p8 / ext4 rw,relatime 0 0\n"
                      "/dev/nvme0n1p8 /home ext4 rw 0 0\n"
                      "/dev/mmcblk0p1 /run/media/deck/SD\\040Card ext4 rw,nosuid 0 0\n"
                      "tmpfs /run/user/1000 tmpfs rw 0 0\n"
                      "/dev/sda1 /media/hunter/USB ext4 rw 0 0\n"
                      "/dev/sdb1 /mnt/data ext4 rw 0 0\n"
                      "/dev/mmcblk1p1 /run/media/mmcblk1p1 exfat rw 0 0\n"
                      # network shares (an automount's point, then what it mounted there), not
                      # looked into before they are opened: a server that is away answers late
                      "systemd-1 /mnt/nas autofs rw,relatime,fd=52,pgrp=1,timeout=0,direct 0 0\n"
                      "nas:/export/games /mnt/nas nfs4 rw,relatime,vers=4.2 0 0\n"
                      "//server/share /media/share cifs rw,vers=3.1.1 0 0\n"
                      "deck@host:/games /mnt/ssh fuse.sshfs rw,nosuid,nodev 0 0\n")
    assert lines(driver, "mounted", mounts) == [
        ["SD card", "/run/media/deck/SD Card", "1", "0"], ["USB", "/media/hunter/USB", "1", "0"],
        ["data", "/mnt/data", "1", "0"], ["SD card (mmcblk1p1)", "/run/media/mmcblk1p1", "1", "0"],
        ["nas", "/mnt/nas", "1", "1"], ["share", "/media/share", "1", "1"], ["ssh", "/mnt/ssh", "1", "1"]]


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="XDG's folders: Linux")
def test_places_start_at_home(driver, tmp_path):
    home = tmp_path / "deck"
    (home / "Descargas").mkdir(parents=True)
    (home / "Desktop").mkdir()
    (home / ".config").mkdir()
    (home / ".config" / "user-dirs.dirs").write_text('XDG_DOWNLOAD_DIR="$HOME/Descargas"\n')
    env = {k: v for k, v in os.environ.items() if k != "XDG_CONFIG_HOME"}
    env["HOME"] = str(home)
    places = lines(driver, "places", env=env)
    assert places[0] == ["Home", str(home), "0"]
    assert ["Desktop", str(home / "Desktop"), "0"] in places
    assert ["Downloads", str(home / "Descargas"), "0"] in places
    assert places[-1] == ["Root (/)", "/", "1"]
    # the drives after the other places
    assert [p[2] for p in places] == sorted(p[2] for p in places)


@pytest.mark.skipif(os.name != "nt", reason="drive letters: Windows")
def test_places_on_windows_have_the_drive_letters(driver):
    places = lines(driver, "places")
    assert places[0][0] == "Home"
    system = os.environ.get("SystemDrive", "C:").upper()
    assert [system, system + "\\", "1"] in places


@pytest.mark.skipif(os.name != "nt", reason="network shares' paths: Windows")
def test_network_shares_on_windows(driver, tmp_path):
    # (the paths only: nothing asks a server)
    # a share's root is as far up as it goes: the server above it is no folder to list
    assert run(driver, "parent", r"\\server\share\Games").strip() == r"\\server\share"
    assert run(driver, "parent", r"\\server\share").strip() == ""
    # a file manager's copy of an address on one, and a drive's written with two slashes
    assert run(driver, "typed", "file://server/share/My%20Games/x.3ds", tmp_path, tmp_path).strip() == \
        r"\\server\share\My Games\x.3ds"
    assert run(driver, "typed", "file://C:/Games/x.3ds", tmp_path, tmp_path).strip() == r"C:\Games\x.3ds"
