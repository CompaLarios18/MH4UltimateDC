"""port/app/stop_message.cpp: what the app tells the player when the game doesn't start, or stops
with an error while it runs (main.cpp). The game's own fatal error (Azahar's err:f, ErrorUnknown)
comes with no details at all (issue #46: "The game stopped:" and nothing after it), nor do most of
System::Load's statuses, so each message says what to try and where the log is."""
from __future__ import annotations

import os
import re
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

# Core::System::ResultStatus (Azahar's core/core.h): main.cpp passes them as numbers
NOT_INITIALIZED = 1
SYSTEM_MODE = 3
LOADER = 4
ARTIC_DISCONNECTED = 12
CORE_EXCEPTION = 14
FATAL_ERROR = 17                                    # ErrorUnknown, from err:f only
LOG = r"C:\Users\Player\AppData\Roaming\Azahar\log\mh4u_log.txt"
# SDL 2's message box (X11's, and Windows' own without version 6 of the common controls) breaks lines
# only where the text does and is as wide as the longest; LoadError's older texts reach 181 characters
WIDEST = 110

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <string>
#include "stop_message.h"

// stopped STATUS DETAILS SECONDS LOG | didnt_start STATUS DETAILS LOG | code_mod LOG: the message
int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    const std::string what = argv[1];
    std::string text;
    if (what == "stopped") {
        text = App::StopMessage::Stopped(static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0)), argv[3],
                                         std::atof(argv[4]), argv[5]);
    } else if (what == "didnt_start") {
        text = App::StopMessage::DidntStart(static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0)), argv[3],
                                            argv[4]);
    } else {
        text = App::StopMessage::CodeModDoesntFit(argv[2]);
    }
    std::fputs(text.c_str(), stdout);
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("stop_message")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "stop_message.cpp"), "-o", str(exe)],
                   check=True)
    return exe


def run(driver, *args) -> str:
    return subprocess.run([str(driver), *map(str, args)], capture_output=True, text=True, check=True).stdout


def stopped(driver, status, seconds, details=""):
    return run(driver, "stopped", status, details, seconds, LOG)


def didnt_start(driver, status, details=""):
    return run(driver, "didnt_start", status, details, LOG)


def ends_with_the_log(text):
    assert text.endswith("\n" + LOG), text


def test_fatal_error_as_it_started(driver):
    """#46: err:f 2.3 s in. What to try, cheapest first, and how to reach the launcher's screens."""
    text = stopped(driver, FATAL_ERROR, 2.3)
    assert text.startswith("The game stopped with a fatal error of its own as it started.\n")
    # one report, its cause unconfirmed: a possibility, not the rule
    assert "can mean" in text and "usually" not in text
    tries = [line for line in text.splitlines() if line.startswith("- ")]
    assert len(tries) == 3
    assert "mods" in tries[0] and "Mods" in tries[0]
    assert "Dump the game again with GodMode9" in tries[1] and "Setup" in tries[1]
    # Play already asks when the update is missing; a .cia decrypted wrongly installs all the same
    assert "update again" in tries[2] and "GodMode9" in tries[2]
    # with launcher = off and the setup complete the app plays straight away: Mods and Setup only
    # with --launcher
    assert "--launcher" in text
    assert "report it with the log" in text and '"Fatal error"' in text
    ends_with_the_log(text)


def test_fatal_error_later_asks_for_a_report_first(driver):
    """After the launcher's check and a while of play the port's own bugs are as likely as a damaged
    dump, and a new dump costs the player a session with their 3DS."""
    text = stopped(driver, FATAL_ERROR, 600)
    assert text.startswith("The game stopped with a fatal error of its own.\n")
    assert "as it started" not in text
    assert text.index("please report it with the log") < text.index("dump the game again")
    ends_with_the_log(text)


def test_the_start_is_its_first_30_seconds(driver):
    assert "as it started" in stopped(driver, FATAL_ERROR, 0)
    assert "as it started" in stopped(driver, FATAL_ERROR, 29.9)
    assert "as it started" not in stopped(driver, FATAL_ERROR, 30)


def test_other_stops(driver):
    # with details (the CPU's exception report, Artic's disconnection): as Azahar says them
    text = stopped(driver, CORE_EXCEPTION, 5, details="Unhandled exception at 0x00100000")
    assert text.startswith("The game stopped: Unhandled exception at 0x00100000\n")
    ends_with_the_log(text)
    # without (ErrorNotInitialized): its number, never "The game stopped: " and nothing after it
    for seconds in (5, 600):
        text = stopped(driver, NOT_INITIALIZED, seconds)
        assert text.startswith(f"The game stopped (status {NOT_INITIALIZED}).\n")
        assert "fatal error" not in text
        ends_with_the_log(text)


def test_didnt_start(driver):
    """System::Load sets no details for ErrorLoader, ErrorSystemMode, ErrorN3DSApplication, ..."""
    text = didnt_start(driver, LOADER)
    assert text.startswith(f"The game didn't start (status {LOADER}).\n")
    ends_with_the_log(text)
    text = didnt_start(driver, ARTIC_DISCONNECTED, "Artic Base server disconnected")
    assert text.startswith("The game didn't start: Artic Base server disconnected\n")
    ends_with_the_log(text)


def test_a_code_mod_that_doesnt_fit(driver):
    """ErrorLoader_ErrorPatches(InvalidTitle): a code.bps made for other code (another version, or
    the v1.3.0 update's with the update not installed), or a damaged patch."""
    text = run(driver, "code_mod", LOG)
    assert "code mod" in text and "Mods" in text and "v1.3.0 update" in text and "--launcher" in text
    ends_with_the_log(text)


def test_no_message_says_nothing_after_its_colon(driver):
    for text in (stopped(driver, NOT_INITIALIZED, 5), didnt_start(driver, LOADER), didnt_start(driver, SYSTEM_MODE),
                 stopped(driver, FATAL_ERROR, 5), stopped(driver, FATAL_ERROR, 600)):
        first = text.splitlines()[0]
        assert not re.search(r":\s*$", first), first


def test_lines_fit_a_message_box(driver):
    texts = [stopped(driver, FATAL_ERROR, 2.3), stopped(driver, FATAL_ERROR, 600),
             stopped(driver, NOT_INITIALIZED, 5), stopped(driver, CORE_EXCEPTION, 5, details="x"),
             didnt_start(driver, LOADER), didnt_start(driver, LOADER, "x"), run(driver, "code_mod", LOG)]
    for text in texts:
        for line in text.splitlines()[:-1]:          # the log's path is the player's own
            assert len(line) <= WIDEST, line
