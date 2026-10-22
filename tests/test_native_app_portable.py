"""The app's shared sources are portable (N2b-02): no SDL header, no Azahar header, no httplib or
JSON, and nothing of Dear ImGui's SDL platform backend.

The shared sources are the ones both executables compile, `mh4u` (Azahar, SDL2) and `mh4u-native`
(SDL3): config.cpp, input.cpp, kbm.cpp, invite.cpp, util/*.cpp and ui/*.cpp except the renderer
backends (ui/render_*.cpp, the Azahar app's own). The window and the few services of the host they
need come through port/app/app_window.h. Each is compiled with `clang++ -fsyntax-only` and the exact
flags below: no include path has SDL, Azahar, httplib or nlohmann's json, so a stray include is a
compile error as well as a finding of the text scan. The test skips without clang or an inih
(native/third_party/inih once N2b-03 vendors it, else Azahar's externals: MH4N_AZAHAR_DIR, else
~/azahar).
"""
from __future__ import annotations

import concurrent.futures
import glob
import os
import re
import shutil
import subprocess

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
APP = os.path.join(ROOT, "port", "app")
AZAHAR = os.environ.get("MH4N_AZAHAR_DIR", os.path.expanduser("~/azahar"))
CXX = shutil.which("clang++")

# the shared sources, relative to port/app
SHARED_TOP = ["config.cpp", "input.cpp", "kbm.cpp", "invite.cpp"]

# headers the shared sources must not include
FORBIDDEN_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"]('
    r'SDL[^>"]*'                                   # SDL.h, SDL3/SDL.h, SDL_syswm.h, ...
    r'|httplib\.h|nlohmann/[^>"]*|json\.hpp'       # the network layer's
    r'|(?:core|common|video_core|audio_core|input_common|network|citra_[a-z]+)/[^>"]*'  # Azahar's
    r'|(?:window|frontend|netsync)\.h'             # the Azahar app's window and the port's netsync
    r')[>"]', re.MULTILINE)
# names of Dear ImGui's SDL platform backend, anywhere in a shared source
FORBIDDEN_NAME = re.compile(r"imgui_impl_sdl|ImGui_ImplSDL")


def shared_sources() -> list[str]:
    files = list(SHARED_TOP)
    files += sorted(os.path.relpath(p, APP) for p in glob.glob(os.path.join(APP, "util", "*.cpp")))
    files += sorted(os.path.relpath(p, APP) for p in glob.glob(os.path.join(APP, "ui", "*.cpp"))
                    if not os.path.basename(p).startswith("render_"))
    return [f.replace(os.sep, "/") for f in files]


def inih_dir() -> str | None:
    for candidate in (os.path.join(ROOT, "native", "third_party", "inih"),
                      os.path.join(AZAHAR, "externals", "inih")):
        if os.path.exists(os.path.join(candidate, "inih", "ini.h")):
            return candidate
    return None


def findings(text: str) -> list[str]:
    """What a shared source's text must not have: the forbidden includes and names."""
    found = [m.group(1) for m in FORBIDDEN_INCLUDE.finditer(text)]
    found += FORBIDDEN_NAME.findall(text)
    return found


def compile_flags(inih: str) -> list[str]:
    return ["-std=c++20", "-fsyntax-only", "-DFMT_HEADER_ONLY=1",
            "-I", os.path.join(ROOT, "port", "app"),
            "-I", os.path.join(ROOT, "port", "src"),
            "-I", os.path.join(ROOT, "port", "app", "third_party", "imgui"),
            "-I", os.path.join(ROOT, "native", "third_party", "fmt", "include"),
            "-I", os.path.join(inih, "inih"),
            "-I", os.path.join(inih, "inih", "cpp")]


# ── the scan itself, on synthetic text ──

def test_scan_finds_what_a_shared_source_must_not_have():
    assert findings('#include <SDL.h>\n') == ["SDL.h"]
    assert findings('#include "SDL3/SDL.h"\n') == ["SDL3/SDL.h"]
    assert findings('  #  include <SDL_syswm.h>\n') == ["SDL_syswm.h"]
    assert findings('#include "core/core.h"\n#include "common/logging/log.h"\n') == ["core/core.h", "common/logging/log.h"]
    assert findings('#include "window.h"\n#include "frontend.h"\n#include "netsync.h"\n') == ["window.h", "frontend.h", "netsync.h"]
    assert findings('#include <httplib.h>\n#include <nlohmann/json.hpp>\n') == ["httplib.h", "nlohmann/json.hpp"]
    assert findings('#include <imgui_impl_sdl2.h>\nImGui_ImplSDL2_NewFrame();\n') == ["imgui_impl_sdl", "ImGui_ImplSDL"]


def test_scan_leaves_the_portable_includes_alone():
    text = ('#include <algorithm>\n#include <fmt/format.h>\n#include <imgui.h>\n#include "app_window.h"\n'
            '#include "config.h"\n#include "runtime.h"\n#include "hooks.h"\n#include "ui/layer.h"\n'
            '#include "util/log.h"\n// the keys are SDL\'s scancodes (see SDL2/SDL_Scancode)\n')
    assert findings(text) == []


# ── the shared sources ──

def test_the_list_of_shared_sources():
    files = shared_sources()
    for expected in ("config.cpp", "input.cpp", "kbm.cpp", "invite.cpp", "util/file.cpp", "util/log.cpp",
                     "util/strings.cpp", "ui/ui.cpp", "ui/menu.cpp", "ui/keyboard.cpp", "ui/key_guides.cpp",
                     "ui/multiplayer_tab.cpp", "ui/hint.cpp", "ui/options.cpp", "ui/fps_counter.cpp"):
        assert expected in files, expected
    assert not [f for f in files if "render_" in f]
    for f in files:
        assert os.path.exists(os.path.join(APP, f)), f


def test_shared_sources_include_nothing_of_sdl_azahar_httplib_or_json():
    bad = {}
    for rel in shared_sources():
        with open(os.path.join(APP, rel), encoding="utf-8") as handle:
            found = findings(handle.read())
        if found:
            bad[rel] = found
    assert bad == {}, f"shared sources with forbidden includes or names: {bad}"


@pytest.mark.skipif(CXX is None, reason="needs clang++")
def test_shared_sources_compile_without_sdl_azahar_httplib_or_json():
    inih = inih_dir()
    if inih is None:
        pytest.skip("no inih: native/third_party/inih or Azahar's externals (MH4N_AZAHAR_DIR)")
    flags = compile_flags(inih)

    def check(rel: str) -> tuple[str, str]:
        done = subprocess.run([CXX, *flags, os.path.join(APP, rel)], capture_output=True, text=True)
        return rel, "" if done.returncode == 0 else done.stderr[-1500:]

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        failed = {rel: err for rel, err in pool.map(check, shared_sources()) if err}
    assert failed == {}, "shared sources that do not compile:\n" + "\n".join(f"{r}:\n{e}" for r, e in failed.items())
