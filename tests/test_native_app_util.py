"""The app's unit tests (port/app/util/tests/): built by hand with a C++ compiler and run.

The tests are doctest binaries (util_test, app_test, multiplayer_test, input_test, keyboard_test); the
CMake option MH4U_APP_TESTS builds them inside Azahar's tree, which CI does not do for them. Here
each is compiled against doctest and {fmt} from native/third_party, and inih, httplib, nlohmann's
json, SDL2's headers and OpenSSL's headers from Azahar's externals (MH4N_AZAHAR_DIR, else
~/azahar), and skipped when something is missing, or when clang is not the compiler available.
"""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
APP = os.path.join(ROOT, "port", "app")
AZAHAR = os.environ.get("MH4N_AZAHAR_DIR", os.path.expanduser("~/azahar"))
EXT = os.path.join(AZAHAR, "externals")
# the tests are written and built with clang (the keyboard test does not compile with GCC's -Werror=range-loop-construct)
CXX = shutil.which("clang++")
CC = shutil.which("clang")

pytestmark = pytest.mark.skipif(CXX is None or CC is None, reason="needs clang and clang++ (GCC does not build these tests)")

UTIL = ["util/file.cpp", "util/log.cpp", "util/strings.cpp"]
IMGUI_DIR = os.path.join(APP, "third_party", "imgui")


def need(*paths: str) -> None:
    for path in paths:
        if not os.path.exists(path):
            pytest.skip(f"{path} is not there (Azahar's sources: MH4N_AZAHAR_DIR)")


@pytest.fixture(scope="module")
def inih(tmp_path_factory: pytest.TempPathFactory) -> list[str]:
    """inih's C and C++ readers, built once: the objects."""
    ini = os.path.join(EXT, "inih", "inih")
    need(os.path.join(ini, "ini.c"), os.path.join(ini, "cpp", "INIReader.cpp"))
    out = tmp_path_factory.mktemp("inih")
    objects = []
    for source, compiler, std in ((os.path.join(ini, "ini.c"), CC, []),
                                  (os.path.join(ini, "cpp", "INIReader.cpp"), CXX, ["-std=c++20"])):
        obj = str(out / (os.path.basename(source) + ".o"))
        run([compiler, *std, "-c", source, "-I" + ini, "-o", obj])
        objects.append(obj)
    return objects


@pytest.fixture(scope="module")
def imgui(tmp_path_factory: pytest.TempPathFactory) -> list[str]:
    """Dear ImGui's core files, built once without the warnings-as-errors the app's files get."""
    out = tmp_path_factory.mktemp("imgui")
    objects = []
    for name in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets"):
        obj = str(out / (name + ".o"))
        run([CXX, "-std=c++20", "-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS", "-I" + IMGUI_DIR, "-c",
             os.path.join(IMGUI_DIR, name + ".cpp"), "-o", obj])
        objects.append(obj)
    return objects


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    done = subprocess.run(cmd, capture_output=True, text=True, **kw)
    assert done.returncode == 0, f"{' '.join(cmd[:3])} ...\n{done.stdout[-3000:]}\n{done.stderr[-6000:]}"
    return done


def build_and_run(tmp_path, name: str, sources: list[str], extra: list[str], objects: list[str] = (),
                  libs: list[str] = ()) -> str:
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-DFMT_HEADER_ONLY=1", "-I" + APP,
             "-I" + os.path.join(ROOT, "native", "third_party", "doctest"),
             "-I" + os.path.join(ROOT, "native", "third_party", "fmt", "include"),
             "-I" + os.path.join(EXT, "inih", "inih"), "-I" + os.path.join(EXT, "inih", "inih", "cpp")]
    exe = str(tmp_path / name)
    run([CXX, *flags, *extra, *[os.path.join(APP, s) for s in sources], *objects, "-o", exe, "-lpthread", *libs])
    return run([exe]).stdout


def test_util_tests(tmp_path):
    out = build_and_run(tmp_path, "util_test", ["util/tests/util_test.cpp", *UTIL], [])
    assert "Status: SUCCESS!" in out
    # (the Windows build counts wchar_t as 16 bits)
    out = build_and_run(tmp_path, "util_test_w", ["util/tests/util_test.cpp", *UTIL], ["-fshort-wchar"])
    assert "Status: SUCCESS!" in out


def test_app_tests(tmp_path, inih):
    out = build_and_run(tmp_path, "app_test", ["util/tests/app_test.cpp", "config.cpp", "launcher/checks.cpp",
                                               os.path.join("..", "src", "options_template.cpp"), *UTIL],
                        ["-I" + os.path.join(ROOT, "port", "src")], inih)
    assert "Status: SUCCESS!" in out


def test_multiplayer_tests(tmp_path, inih):
    need(os.path.join(EXT, "httplib"), os.path.join(EXT, "json"), "/usr/include/openssl/ssl.h")
    out = build_and_run(tmp_path, "multiplayer_test",
                        ["util/tests/multiplayer_test.cpp", "multiplayer.cpp", "invite.cpp", "launcher/lobby.cpp",
                         "config.cpp", *UTIL],
                        ["-I" + os.path.join(EXT, "httplib"), "-I" + os.path.join(EXT, "json"),
                         "-DCPPHTTPLIB_OPENSSL_SUPPORT"], inih, ["-lssl", "-lcrypto"])
    assert "Status: SUCCESS!" in out


def test_input_tests(tmp_path, inih):
    need(os.path.join(EXT, "sdl2", "SDL", "include", "SDL.h"))
    out = build_and_run(tmp_path, "input_test", ["util/tests/input_test.cpp", "input.cpp", "kbm.cpp", "config.cpp", *UTIL],
                        ["-DSDL_MAIN_HANDLED", "-I" + os.path.join(EXT, "sdl2", "SDL", "include")], inih)
    assert "Status: SUCCESS!" in out


def test_keyboard_tests(tmp_path, inih, imgui):
    need(os.path.join(EXT, "sdl2", "SDL", "include", "SDL.h"))
    out = build_and_run(tmp_path, "keyboard_test",
                        ["util/tests/keyboard_test.cpp", "ui/keyboard.cpp", "config.cpp", *UTIL],
                        ["-DSDL_MAIN_HANDLED", "-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS", "-I" + IMGUI_DIR,
                         "-I" + os.path.join(EXT, "sdl2", "SDL", "include")], [*inih, *imgui])
    assert "Status: SUCCESS!" in out
