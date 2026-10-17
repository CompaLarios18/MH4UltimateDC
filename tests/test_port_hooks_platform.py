"""port/src/hooks.cpp depends only on RCtx, guest memory and Port::Platform (N0-18): it includes
nothing of Azahar, does not pull in netsync.h, does not choose the memory mode itself, and compiles
in every memory mode without an Azahar include path, so the native runtime can reuse it as it is."""
import os
import re
import shutil
import subprocess

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HOOKS = [os.path.join(ROOT, "port", "src", "hooks.cpp"), os.path.join(ROOT, "port", "src", "hooks.h")]
AZAHAR_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](?:core|video_core|common|network|audio_core)/')


def text(path):
    return open(path, encoding="utf-8").read()


def test_hooks_include_nothing_of_azahar():
    for path in HOOKS:
        bad = [n for n, line in enumerate(text(path).splitlines(), 1) if AZAHAR_INCLUDE.match(line)]
        assert bad == [], f"{os.path.basename(path)} includes Azahar headers at lines {bad}"


def test_hooks_do_not_include_netsync_or_define_the_memory_mode():
    for path in HOOKS:
        assert not re.search(r'^\s*#\s*include\s*[<"][^>"]*netsync\.h', text(path), re.M), path
    assert not re.search(r"^\s*#\s*define\s+RC_PAGED\b", text(HOOKS[0]), re.M)


def test_the_azahar_build_still_gives_hooks_the_paged_mode():
    cmake = text(os.path.join(ROOT, "port", "CMakeLists.txt"))
    assert re.search(r"set_source_files_properties\([^)]*src/hooks\.cpp[^)]*COMPILE_DEFINITIONS[^)]*RC_PAGED", cmake, re.S)


@pytest.mark.skipif(shutil.which("clang++") is None, reason="clang++ not installed")
@pytest.mark.parametrize("define", [None, "RC_PAGED", "RC_NATIVE"])
def test_hooks_compile_without_an_azahar_include_path(define):
    cmd = ["clang++", "-std=c++20", "-fsyntax-only", "-Wno-unused-parameter", "-Wno-unused-const-variable",
           "-I", os.path.join(ROOT, "port", "src"), "-I", os.path.join(ROOT, "tools", "recomp", "runtime"),
           "-I", os.path.join(ROOT, "native", "third_party", "fmt", "include"), HOOKS[0]]
    if define:
        cmd[2:2] = [f"-D{define}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr[-2000:]
