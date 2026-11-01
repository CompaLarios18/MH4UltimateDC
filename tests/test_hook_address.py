"""port/src/hook_address.h: where the port's hooks find a hook point in a code library. A library
lists its hook points' names (RCodeInfo.hook_names, config/hooks.txt), so the same hooks run on
EUR v1.1.0, whose functions sit a little further along than USA v1.3.0's: a name the library has
is at the library's address, a name it lacks is missing (0), never USA's address; only a library
without names (built before they were listed) is taken as USA v1.3.0's.

Synthetic tables only (the addresses are made up). The header is built into a small driver with
the C++ compiler found, as tests/test_update_code.py builds its own."""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "hook_address.h"

// [names|nonames] USA_ADDR NAME [ADDR NAME]...: the library's hook points (sorted, as
// recompile.py writes them), then the address FindHookPoint gives for NAME
int main(int argc, char** argv) {
    if (argc < 4 || argc % 2) {
        return 2;
    }
    std::vector<u32> addrs;
    std::vector<const char*> names;
    for (int i = 4; i + 1 < argc; i += 2) {
        addrs.push_back(static_cast<u32>(std::strtoul(argv[i], nullptr, 16)));
        names.push_back(argv[i + 1]);
    }
    RCodeInfo info{};
    info.hook_addrs = addrs.data();
    info.hook_names = std::string_view(argv[1]) == "names" ? names.data() : nullptr;
    info.hook_count = static_cast<u32>(addrs.size());
    std::printf("%08x\n", Port::Hooks::FindHookPoint(info, argv[3], static_cast<u32>(std::strtoul(argv[2], nullptr, 16))));
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("hook_address")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", os.path.join(ROOT, "port", "src"),
                    "-I", os.path.join(ROOT, "tools", "recomp", "runtime"), str(src), "-o", str(exe)], check=True)
    return exe


# a made-up library of another version: its two hook points a little further along than USA's
LIBRARY = ["00200050", "Game::update", "00300060", "Game::start"]


def find(driver, names: str, usa_addr: int, name: str, library=LIBRARY) -> int:
    out = subprocess.run([str(driver), names, f"{usa_addr:x}", name, *library], capture_output=True, text=True,
                         check=True).stdout
    return int(out, 16)


def test_a_name_the_library_has_is_at_its_address(driver):
    assert find(driver, "names", 0x00300010, "Game::start") == 0x00300060
    assert find(driver, "names", 0x00200000, "Game::update") == 0x00200050


def test_a_name_the_library_lacks_is_missing_not_at_usas_address(driver):
    assert find(driver, "names", 0x00400000, "Game::stop") == 0
    assert find(driver, "names", 0x00300060, "game::start") == 0     # names match exactly


def test_a_library_without_names_is_usas(driver):
    assert find(driver, "nonames", 0x00300010, "Game::start") == 0x00300010
    assert find(driver, "nonames", 0x00400000, "Game::stop") == 0x00400000
