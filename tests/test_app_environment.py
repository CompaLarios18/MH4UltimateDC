"""port/app/launcher/environment.cpp: the environment of the programs the launcher starts on Linux,
where the app runs from mh4u.AppImage (issue #60: under the launcher the code library's build
failed at its end, while build-code-library.sh, from a terminal, built it). The AppImage runtime's
variables go, and so do the parts of LD_LIBRARY_PATH, LD_PRELOAD, PATH and the like inside its
mount (the checkrt hook's libraries and its exec.so); the rest stays as it was, in its order.
Synthetic environments; any C++ compiler builds the driver."""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include "launcher/environment.h"

// argv: "inside PATH DIR", or the mounts with the environment on stdin, a "NAME=value" a line:
// prints what the launcher's children get, a line "--", then the names of what changed.
int main(int argc, char** argv) {
    using namespace Launcher::Environment;
    if (argc == 4 && std::string(argv[1]) == "inside") {
        std::printf("%d\n", Inside(argv[2], argv[3]) ? 1 : 0);
        return 0;
    }
    const std::vector<std::string> mounts(argv + 1, argv + argc);
    std::vector<std::string> env;
    for (std::string line; std::getline(std::cin, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        env.push_back(line);
    }
    const auto out = WithoutAppImage(env, mounts);
    for (const auto& entry : out) {
        std::printf("%s\n", entry.c_str());
    }
    std::printf("--\n");
    for (const auto& name : Changed(env, out)) {
        std::printf("%s\n", name.c_str());
    }
    return 0;
}
"""

MOUNT = "/tmp/.mount_mh4uAbC123"
STEAM_OVERLAY = ("/home/hunter/.steam/ubuntu12_32/gameoverlayrenderer.so "
                 "/home/hunter/.steam/ubuntu12_64/gameoverlayrenderer.so")


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("environment")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "launcher", "environment.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def run(driver, env, *mounts):
    out = subprocess.run([str(driver), *mounts], input="\n".join(env) + "\n", capture_output=True, text=True,
                         check=True).stdout
    kept, changed = out.split("--\n")
    return kept.splitlines(), changed.splitlines()


def test_what_the_appimage_added_goes(driver):
    """As mh4u.AppImage starts the launcher on a system whose libstdc++ is older than the one it
    bundles (its checkrt hook then puts that one first, and preloads its exec.so), from Steam."""
    env = [
        "HOME=/home/hunter",
        f"PATH={MOUNT}/usr/bin:/home/hunter/.local/bin:/usr/bin",
        f"APPDIR={MOUNT}",
        "APPIMAGE=/home/hunter/Games/mh4u-port-linux/mh4u.AppImage",
        "ARGV0=./mh4u.AppImage",
        "OWD=/home/hunter/Games/mh4u-port-linux",
        f"LD_LIBRARY_PATH={MOUNT}/usr/optional/libstdc++:{MOUNT}/usr/optional/libgcc_s:",
        f"LD_PRELOAD={MOUNT}/usr/optional/exec.so:{STEAM_OVERLAY}",
        f"PYTHONHOME={MOUNT}/usr/",
        f"PYTHONPATH={MOUNT}/usr/share/pyshared/:/home/hunter/lib/python",
        f"_={MOUNT}/AppRun.wrapped",
        "XDG_DATA_HOME=/home/hunter/.local/share",
        "LANG=es_MX.UTF-8",
        "http_proxy=http://proxy.local:3128",
        "MH4U_PYTHON=/usr/bin/python3.14",
        f"LOOKALIKE={MOUNT}4/usr/lib",                    # another folder that starts the same
        "EMPTY=",
    ]
    kept, changed = run(driver, env, MOUNT)
    assert kept == [
        "HOME=/home/hunter",
        "PATH=/home/hunter/.local/bin:/usr/bin",
        f"LD_PRELOAD={STEAM_OVERLAY}",
        "PYTHONPATH=/home/hunter/lib/python",
        "XDG_DATA_HOME=/home/hunter/.local/share",
        "LANG=es_MX.UTF-8",
        "http_proxy=http://proxy.local:3128",
        "MH4U_PYTHON=/usr/bin/python3.14",
        f"LOOKALIKE={MOUNT}4/usr/lib",
        "EMPTY=",
    ]
    assert changed == ["PATH", "APPDIR", "APPIMAGE", "ARGV0", "OWD", "LD_LIBRARY_PATH", "LD_PRELOAD",
                       "PYTHONHOME", "PYTHONPATH", "_"]


def test_the_players_own_parts_stay(driver):
    env = [f"LD_LIBRARY_PATH={MOUNT}/usr/optional/libstdc++:/opt/vulkan-sdk/lib:{MOUNT}/usr/lib",
           f"PATH=/home/hunter/My Tools/bin:{MOUNT}/usr/bin",           # a space in a folder's name
           f"LD_PRELOAD=/usr/lib/libgamemodeauto.so.0 {MOUNT}/usr/optional/exec.so"]
    kept, changed = run(driver, env, MOUNT)
    assert kept == ["LD_LIBRARY_PATH=/opt/vulkan-sdk/lib", "PATH=/home/hunter/My Tools/bin",
                    "LD_PRELOAD=/usr/lib/libgamemodeauto.so.0"]
    assert changed == ["LD_LIBRARY_PATH", "PATH", "LD_PRELOAD"]


def test_the_mount_with_its_links_resolved_too(driver):
    """$APPDIR as the runtime set it and as the system resolves it: parts in either go."""
    env = ["LD_LIBRARY_PATH=/var/tmp/.mount_x/usr/lib:/private/tmp/.mount_x/usr/lib:/usr/local/lib",
           "APPDIR=/var/tmp/.mount_x"]
    kept, _ = run(driver, env, "/var/tmp/.mount_x", "/private/tmp/.mount_x")
    assert kept == ["LD_LIBRARY_PATH=/usr/local/lib"]


def test_not_in_an_appimage_nothing_changes(driver):
    """No mount (not an AppImage, or an APPDIR that isn't this app's): the environment as it is."""
    env = ["APPDIR=/tmp/.mount_other", "LD_LIBRARY_PATH=/tmp/.mount_other/usr/lib", "HOME=/home/hunter",
           "NOT A VARIABLE", "=odd"]
    kept, changed = run(driver, env)
    assert kept == env and changed == []


def test_odd_entries_stay(driver):
    env = ["NOT A VARIABLE", "=odd", f"ODD={MOUNT}=x", f"IN_IT={MOUNT}/x=y"]
    kept, _ = run(driver, env, MOUNT)
    assert kept == ["NOT A VARIABLE", "=odd", f"ODD={MOUNT}=x"]     # (the last one: a path in the mount)


@pytest.mark.parametrize("path, folder, inside", [
    (f"{MOUNT}/usr/bin/mh4u", MOUNT, True),
    (MOUNT, MOUNT, True),
    (f"{MOUNT}4/usr/bin", MOUNT, False),
    ("/tmp", MOUNT, False),
    ("/usr/bin/python3", MOUNT, False),
    ("python3", MOUNT, False),
])
def test_inside(driver, path, folder, inside):
    out = subprocess.run([str(driver), "inside", path, folder], capture_output=True, text=True, check=True).stdout
    assert out.strip() == ("1" if inside else "0")
