"""port/app/launcher/process.cpp on Linux and macOS: the programs the launcher starts (Python with
the tools, the compiler through them; the app's own code check; the app again after an update),
run by a driver of the test's own that builds process.cpp and environment.cpp (any C++ compiler).
Issue #60: under mh4u.AppImage the code library's build failed at its end, while
build-code-library.sh, from a terminal, built it. A child starts with SIGCHLD and the requests to
end at their defaults whatever the launcher has them at; how it ended is its status, the signal
it died of, or stopped. On Linux a program outside the AppImage's mount (a folder of the test's
own, with the driver in it as the app) gets the environment without the AppImage's variables and
the parts of the others inside the mount, and the app's own executable keeps them all."""
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
pytestmark = [pytest.mark.skipif(sys.platform.startswith("win"), reason="the POSIX side (Linux, macOS)"),
              pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")]
linux = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="AppImages are Linux's")

DRIVER = r"""
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "launcher/process.h"

// [--ignore-signals] WHAT ARGS...: SIGCHLD, SIGTERM, SIGINT and SIGPIPE ignored here first (as
// whatever started the app may leave them), then WHAT:
//   run PROGRAM ARGS...       runs it as the launcher runs a tool: its output, then how it ended
//   stop PROGRAM ARGS...      starts it and stops it at once (the build's Cancel)
//   self ARGS...              runs this program again by its own path (the code check)
//   detached PROGRAM ARGS...  starts it on its own (the app again after an update)
//   env NAME...               each variable: NAME=value, or NAME unset
//   note                      ChildEnvironmentNote(), for the log
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && args.front() == "--ignore-signals") {
        for (const int sig : {SIGCHLD, SIGTERM, SIGINT, SIGPIPE}) {
            std::signal(sig, SIG_IGN);
        }
        args.erase(args.begin());
    }
    if (args.empty()) {
        return 2;
    }
    const std::string what = args.front();
    args.erase(args.begin());
    if (what == "env") {
        for (const auto& name : args) {
            if (const char* value = std::getenv(name.c_str())) {
                std::printf("%s=%s\n", name.c_str(), value);
            } else {
                std::printf("%s unset\n", name.c_str());
            }
        }
        return 0;
    }
    if (what == "note") {
        std::printf("%s\n", Launcher::ChildEnvironmentNote().c_str());
        return 0;
    }
    std::string error;
    if (what == "detached") {
        if (!Launcher::StartDetached(args, error)) {
            std::printf("didn't start: %s\n", error.c_str());
            return 1;
        }
        std::printf("started\n");
        return 0;
    }
    if (what == "self") {
        args.insert(args.begin(), Launcher::SelfPath());
    } else if (what != "run" && what != "stop") {
        return 2;
    }
    Launcher::Process process;
    if (!process.Start(args, error)) {
        std::printf("didn't start: %s\n", error.c_str());
        return 1;
    }
    if (what == "stop") {
        process.Stop();
    }
    process.Wait();
    std::printf("%s[stopped %d status %d signal %d]\n", process.Output().c_str(), process.WasStopped() ? 1 : 0,
                process.ExitCode(), process.Signal());
    return 0;
}
"""

# what a Python child says of its environment: each name and its value (None: not set)
SHOW = "import os, sys; [print(name, os.environ.get(name)) for name in sys.argv[1:]]"
RUNTIME = ("APPDIR", "APPIMAGE", "ARGV0", "OWD")


@pytest.fixture(scope="module")
def appdir(tmp_path_factory):
    """A folder as an AppImage's mount, with the driver in it as the app (usr/bin/mh4u)."""
    work = tmp_path_factory.mktemp("process")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    mount = work / ".mount_mh4uTest"
    exe = mount / "usr" / "bin" / "mh4u"
    exe.parent.mkdir(parents=True)
    subprocess.run([CXX, "-std=c++20", "-pthread", "-I", APP, str(src), os.path.join(APP, "launcher", "process.cpp"),
                    os.path.join(APP, "launcher", "environment.cpp"), "-o", str(exe)], check=True)
    return mount


def driver(appdir, env, *args):
    r = subprocess.run([str(appdir / "usr" / "bin" / "mh4u"), *args], env=env, capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def parts(name):
    """This test's own value of a path list, without empty parts (one the launcher's children
    would lose too, once a part of it went)."""
    return ":".join(part for part in os.environ.get(name, "").split(":") if part)


def appimage_env(appdir, **changes):
    """The environment as the AppImage's runtime and AppRun hand it to the app: its variables,
    and its folders first in PATH and LD_LIBRARY_PATH."""
    env = {name: value for name, value in os.environ.items() if name not in RUNTIME}
    env.update(APPDIR=str(appdir), APPIMAGE=str(appdir.parent / "mh4u.AppImage"), ARGV0="./mh4u.AppImage",
               OWD=str(appdir.parent), PATH=":".join(filter(None, [f"{appdir}/usr/bin", parts("PATH")])),
               LD_LIBRARY_PATH=":".join(filter(None, [f"{appdir}/usr/optional/libstdc++", "/opt/mh4u-test/lib",
                                                      parts("LD_LIBRARY_PATH")])),
               MH4U_TEST_KEPT="kept")
    env.update(changes)
    return env


@linux
def test_a_program_outside_the_appimage_gets_the_environment_without_it(appdir):
    out = driver(appdir, appimage_env(appdir), "run", sys.executable, "-c", SHOW, *RUNTIME, "PATH",
                 "LD_LIBRARY_PATH", "MH4U_TEST_KEPT")
    assert out.splitlines() == [
        *(f"{name} None" for name in RUNTIME),
        f"PATH {parts('PATH') or None}",
        "LD_LIBRARY_PATH " + ":".join(filter(None, ["/opt/mh4u-test/lib", parts("LD_LIBRARY_PATH")])),
        "MH4U_TEST_KEPT kept",
        "[stopped 0 status 0 signal 0]",
    ]


@linux
def test_the_apps_own_executable_keeps_it_all(appdir):
    """The code check (mh4u --check-code) runs from the mount: it needs what the AppImage set."""
    env = appimage_env(appdir)
    out = driver(appdir, env, "self", "env", "APPDIR", "PATH", "LD_LIBRARY_PATH", "MH4U_TEST_KEPT")
    assert out.splitlines() == [f"APPDIR={appdir}", f"PATH={env['PATH']}",
                                f"LD_LIBRARY_PATH={env['LD_LIBRARY_PATH']}", "MH4U_TEST_KEPT=kept",
                                "[stopped 0 status 0 signal 0]"]


@linux
def test_the_log_names_what_they_dont_get(appdir):
    note = driver(appdir, appimage_env(appdir), "note").strip()
    assert note.startswith(f"programs outside the AppImage ({appdir}) start without")
    names = note.rsplit("environment: ", 1)[1].split(", ")
    assert sorted(names) == sorted([*RUNTIME, "PATH", "LD_LIBRARY_PATH"])


@linux
def test_an_appdir_that_isnt_this_apps_changes_nothing(appdir, tmp_path):
    """$APPDIR from another AppImage that started this app: the app's executable isn't in it."""
    other = tmp_path / ".mount_other"
    other.mkdir()
    library_path = ":".join(filter(None, [f"{other}/usr/lib", parts("LD_LIBRARY_PATH")]))
    env = appimage_env(appdir, APPDIR=str(other), LD_LIBRARY_PATH=library_path)
    out = driver(appdir, env, "run", sys.executable, "-c", SHOW, "APPDIR", "LD_LIBRARY_PATH")
    assert out.splitlines()[:2] == [f"APPDIR {other}", f"LD_LIBRARY_PATH {library_path}"]
    assert driver(appdir, env, "note") == "\n"


@linux
def test_the_app_started_again_on_its_own_gets_it_too(appdir, tmp_path):
    """StartDetached, as after an update: the new app outside the old one's mount, SIGCHLD at its
    default."""
    written = tmp_path / "detached.txt"
    code = ("import os, signal, sys\n"
            "state = (os.environ.get('APPDIR'), signal.getsignal(signal.SIGCHLD) == signal.SIG_DFL)\n"
            "with open(sys.argv[1] + '.part', 'w') as f:\n"
            "    f.write(repr(state))\n"
            "os.replace(sys.argv[1] + '.part', sys.argv[1])\n")
    out = driver(appdir, appimage_env(appdir), "--ignore-signals", "detached", sys.executable, "-c", code,
                 str(written))
    assert out == "started\n"
    deadline = time.time() + 30
    while not written.exists():
        assert time.time() < deadline, "the program started on its own wrote nothing"
        time.sleep(0.1)
    assert written.read_text() == "(None, True)"


def test_children_start_with_the_signals_at_their_defaults(appdir):
    """SIGCHLD ignored by whatever started the app would be ignored in Python too, which then
    couldn't wait for its compilers (every status reads 0), nor clang for its linker."""
    code = ("import signal; print(signal.getsignal(signal.SIGCHLD) == signal.SIG_DFL, "
            "signal.getsignal(signal.SIGTERM) == signal.SIG_DFL, signal.getsignal(signal.SIGINT) != signal.SIG_IGN)")
    out = driver(appdir, dict(os.environ), "--ignore-signals", "run", sys.executable, "-c", code)
    # (the driver's own wait doesn't work with SIGCHLD ignored: only what the child said counts)
    assert out.splitlines()[0] == "True True True"


def test_how_a_child_ended(appdir):
    env = dict(os.environ)

    def ended(what, code):
        return driver(appdir, env, what, sys.executable, "-c", code).splitlines()[-1]
    assert ended("run", "import sys; sys.exit(3)") == "[stopped 0 status 3 signal 0]"
    # (the system's out-of-memory killer sends SIGKILL)
    assert ended("run", "import os, signal; os.kill(os.getpid(), signal.SIGKILL)") == "[stopped 0 status -1 signal 9]"
    # Cancel: stopped, no status and no signal of its own
    assert ended("stop", "import time; time.sleep(60)") == "[stopped 1 status -1 signal 0]"
