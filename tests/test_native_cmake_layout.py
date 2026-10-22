"""native/CMakeLists.txt: new modules, tools, tests and registrations are found without editing a
shared file (N1-01).

The app tree (native/app, N2b-03) is checked the same way: off by default (nothing needs SDL3),
and with -DMH4N_APP=ON (skipped without SDL3 3.4.18, found through CMAKE_PREFIX_PATH; `source
~/.mh4n_env`) an app module that registers a `runner` option shows it in `mh4u-native --help` and
not in `mh4u_native_runner --help`.

Copies native/ into a temporary directory, adds to the copy a module two levels down, a module
that depends on it and on mh4n_common, a module nothing references that holds a registration and
an option, a tool, a test and a test with its own main, then configures with the project's
configure line, builds, runs ctest and the tool. Skipped without cmake, ninja and clang.
"""
import os
import re
import shutil
import subprocess
import textwrap

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NATIVE = os.path.join(ROOT, "native")

pytestmark = pytest.mark.skipif(
    not all(shutil.which(t) for t in ("cmake", "ninja", "clang", "clang++"))
    or not os.path.isdir(NATIVE),
    reason="needs cmake, ninja, clang and clang++ (and native/)",
)


def cmake_version():
    out = subprocess.run(["cmake", "--version"], capture_output=True, text=True).stdout
    match = re.search(r"(\d+)\.(\d+)", out)
    return (int(match.group(1)), int(match.group(2))) if match else (0, 0)


@pytest.fixture(scope="module", autouse=True)
def _need_cmake_3_25():
    if cmake_version() < (3, 25):
        pytest.skip("needs cmake 3.25 or later")


def configure_cmd(src, build):
    # the configure line of docs/native/wp/N1.md
    return [
        "cmake", "-S", src, "-B", build, "-G", "Ninja",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DMH4N_WERROR=ON",
    ]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=1500, **kw)


def write(root, rel, text):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(textwrap.dedent(text))


def copy_native(dst):
    shutil.copytree(
        NATIVE, dst, ignore=shutil.ignore_patterns("build", "CMakeCache.txt", "CMakeFiles")
    )
    # files outside native/ that a module may compile or include, at the same relative places
    # (dst is <tree>/native): port/src/fiber.* and tools/recomp/runtime/recomp.h
    tree = os.path.dirname(os.path.abspath(dst))
    for rel in ("port/src/fiber.h", "port/src/fiber.cpp", "tools/recomp/runtime/recomp.h"):
        src = os.path.join(ROOT, rel)
        if os.path.isfile(src):
            out = os.path.join(tree, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(src, out)


def add_extras(native):
    # a module two levels down: mh4n_x_y
    write(native, "include/mh4n/layout_probe.h", """\
        #pragma once
        namespace mh4n { int XYValue(); int UserValue(); }
        """)
    write(native, "src/x/y/CMakeLists.txt", "mh4n_module(SOURCES xy.cpp)\n")
    write(native, "src/x/y/xy.cpp", """\
        #include "mh4n/layout_probe.h"
        #include "mh4n/crc32.h"
        namespace mh4n {
        int XYValue() { return static_cast<int>(Crc32("abc", 3) & 0xff) - 0xC2 + 7; }   // 7
        }
        """)
    # a module that depends on that one and on mh4n_common; its name sorts before 'x'
    write(native, "src/aaa_user/CMakeLists.txt",
          "mh4n_module(SOURCES user.cpp DEPENDS mh4n_x_y mh4n_common)\n")
    write(native, "src/aaa_user/user.cpp", """\
        #include "mh4n/layout_probe.h"
        #include "mh4n/log.h"
        namespace mh4n {
        int UserValue() { MH4N_LOG(Trace, "Layout", "user"); return XYValue() * 2; }   // 14
        }
        """)
    # a module nothing references: its registrations must still run
    write(native, "src/orphan/CMakeLists.txt", "mh4n_module(SOURCES orphan.cpp)\n")
    write(native, "src/orphan/orphan.cpp", """\
        #include "mh4n/options.h"
        #include "mh4n/registry.h"
        MH4N_REGISTER(int, "layout-orphan", 42);
        MH4N_OPTION("layout-tool", "--orphan-option", "", "registered by a module nobody names");
        """)
    # a tool
    write(native, "tools/layout_tool/CMakeLists.txt",
          "mh4n_program(layout_tool SOURCES main.cpp)\n")
    write(native, "tools/layout_tool/main.cpp", """\
        #include <cstdio>
        #include "mh4n/layout_probe.h"
        #include "mh4n/options.h"
        #include "mh4n/registry.h"
        int main(int argc, char** argv) {
            const mh4n::Options o = mh4n::ParseOptions("layout-tool", argc, argv);
            if (!o.ok()) return 2;
            const int* orphan = mh4n::Registry<int>::Instance().Find("layout-orphan");
            std::printf("orphan=%d user=%d option=%d\\n", orphan ? *orphan : -1,
                        mh4n::UserValue(), o.Has("--orphan-option") ? 1 : 0);
            return 0;
        }
        """)
    # an ordinary test, and one with its own main
    write(native, "tests/layout_probe_test.cpp", """\
        #include <doctest/doctest.h>
        #include "mh4n/layout_probe.h"
        #include "mh4n/registry.h"
        TEST_CASE("the new modules are linked and the orphan registered") {
            CHECK(mh4n::XYValue() == 7);
            CHECK(mh4n::UserValue() == 14);
            REQUIRE(mh4n::Registry<int>::Instance().Find("layout-orphan") != nullptr);
            CHECK(*mh4n::Registry<int>::Instance().Find("layout-orphan") == 42);
        }
        """)
    write(native, "tests/layout_own_main_test.cpp", """\
        #define MH4N_TEST_OWN_MAIN
        #include <doctest/doctest.h>
        #include <cstdio>
        #include "mh4n/registry.h"
        TEST_CASE("runs under the test's own main") {
            CHECK(mh4n::Registry<int>::Instance().Find("layout-orphan") != nullptr);
        }
        int main(int argc, char** argv) {
            std::printf("own main ran\\n");
            return doctest::Context(argc, argv).run();
        }
        """)


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    work = str(tmp_path_factory.mktemp("native_layout"))
    native = os.path.join(work, "native")
    build = os.path.join(work, "build")
    copy_native(native)
    add_extras(native)
    conf = run(configure_cmd(native, build))
    assert conf.returncode == 0, conf.stdout + conf.stderr
    built = run(["cmake", "--build", build])
    assert built.returncode == 0, built.stdout[-6000:] + built.stderr[-3000:]
    return native, build


def test_ctest_runs_new_and_existing_tests(built):
    _native, build = built
    result = run(["ctest", "--test-dir", build, "--output-on-failure"])
    assert result.returncode == 0, result.stdout + result.stderr
    for name in ("layout_probe_test", "layout_own_main_test", "common_test", "registry_test",
                 "options_test", "runner_help"):
        assert re.search(rf"Test\s+#\d+: {name}\b.*Passed", result.stdout), result.stdout


def test_tool_finds_the_unreferenced_registration_and_option(built):
    _native, build = built
    result = run([os.path.join(build, "bin", "layout_tool"), "--orphan-option"])
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "orphan=42 user=14 option=1"
    result = run([os.path.join(build, "bin", "layout_tool")])
    assert result.stdout.strip() == "orphan=42 user=14 option=0"


def test_own_main_is_used_only_when_asked(built):
    _native, build = built
    own = run([os.path.join(build, "bin", "layout_own_main_test")])
    assert own.returncode == 0 and "own main ran" in own.stdout
    plain = run([os.path.join(build, "bin", "layout_probe_test")])
    assert plain.returncode == 0 and "own main ran" not in plain.stdout
    assert "Status: SUCCESS" in plain.stdout


def test_module_library_names_follow_the_path(built):
    _native, build = built
    libs = sorted(os.listdir(os.path.join(build, "lib")))
    for name in ("libmh4n_common.a", "libmh4n_x_y.a", "libmh4n_aaa_user.a", "libmh4n_orphan.a"):
        assert name in libs, libs


def test_a_dependency_on_a_missing_module_is_an_error(tmp_path):
    native = str(tmp_path / "native")
    copy_native(native)
    write(native, "src/broken/CMakeLists.txt",
          "mh4n_module(SOURCES b.cpp DEPENDS mh4n_does_not_exist)\n")
    write(native, "src/broken/b.cpp", "int broken_value() { return 1; }\n")
    result = run(configure_cmd(native, str(tmp_path / "build")))
    assert result.returncode != 0
    assert "mh4n_does_not_exist" in result.stderr


# ---- the app tree (native/app, N2b-03) ----

def copy_port_app(native):
    """port/app next to the copy (native/app names the shared sources as ../../../port/app)."""
    tree = os.path.dirname(os.path.abspath(native))
    shutil.copytree(os.path.join(ROOT, "port", "app"), os.path.join(tree, "port", "app"))


def sdl3_available(tmp):
    write(tmp, "CMakeLists.txt", """\
        cmake_minimum_required(VERSION 3.25)
        project(find_sdl3 LANGUAGES NONE)
        find_package(SDL3 3.4.18 CONFIG REQUIRED)
        """)
    return run(["cmake", "-S", tmp, "-B", os.path.join(tmp, "build")]).returncode == 0


@pytest.fixture(scope="module")
def sdl3(tmp_path_factory):
    if not sdl3_available(str(tmp_path_factory.mktemp("sdl3_probe"))):
        pytest.skip("needs SDL3 3.4.18 (source ~/.mh4n_env)")


def test_the_app_tree_is_off_by_default_and_needs_no_sdl3(tmp_path):
    native = str(tmp_path / "native")
    build = str(tmp_path / "build")
    copy_native(native)
    conf = run(configure_cmd(native, build))
    assert conf.returncode == 0, conf.stdout + conf.stderr
    assert "SDL3" not in conf.stdout + conf.stderr
    cache = open(os.path.join(build, "CMakeCache.txt")).read()
    assert "MH4N_APP:BOOL=OFF" in cache
    targets = run(["cmake", "--build", build, "--target", "help"]).stdout
    assert "mh4u_native_runner" in targets
    for name in ("mh4u-native", "mh4n_app_shared", "mh4n_imgui", "app_shared_test"):
        assert name not in targets, name


@pytest.fixture(scope="module")
def built_app(built, sdl3):
    """The module's tree again with an app module that registers a runner option, a nested one that
    depends on it and a test, configured with MH4N_APP=ON in the same build folder."""
    native, build = built
    copy_port_app(native)
    write(native, "app/probe/CMakeLists.txt", "mh4n_app_module(SOURCES probe.cpp)\n")
    write(native, "app/probe/probe.cpp", """\
        #include "mh4n/options.h"
        #include "mh4n/registry.h"
        MH4N_OPTION("runner", "--app-probe-option", "", "registered by an app module");
        MH4N_REGISTER(int, "app-probe", 11);
        """)
    write(native, "app/deep/inner/CMakeLists.txt",
          "mh4n_app_module(SOURCES inner.cpp DEPENDS mh4n_app_probe)\n")
    write(native, "app/deep/inner/inner.cpp", """\
        #include "app_window.h"
        namespace probe { int Inner() { return App::Scancode::Escape; } }
        """)
    write(native, "app/tests/probe_app_test.cpp", """\
        #include <doctest/doctest.h>
        #include "app_window.h"
        #include "mh4n/registry.h"
        namespace probe { int Inner(); }
        TEST_CASE("app modules are linked whole, at any depth") {
            REQUIRE(mh4n::Registry<int>::Instance().Find("app-probe") != nullptr);
            CHECK(*mh4n::Registry<int>::Instance().Find("app-probe") == 11);
            CHECK(probe::Inner() == 41);
        }
        """)
    conf = run(configure_cmd(native, build) + ["-DMH4N_APP=ON"])
    assert conf.returncode == 0, conf.stdout + conf.stderr
    result = run(["cmake", "--build", build, "--target", "mh4u-native", "mh4u_native_runner",
                  "probe_app_test", "app_shared_test"])
    assert result.returncode == 0, result.stdout[-6000:] + result.stderr[-3000:]
    return native, build


def test_an_app_option_shows_in_mh4u_native_help_and_not_in_the_runners(built_app):
    _native, build = built_app
    app = run([os.path.join(build, "bin", "mh4u-native"), "--help"])
    assert app.returncode == 0, app.stderr
    assert "usage: mh4u-native" in app.stdout
    assert "--app-probe-option" in app.stdout
    assert "--init-time" in app.stdout            # the runtime's options are listed too
    runner = run([os.path.join(build, "bin", "mh4u_native_runner"), "--help"])
    assert runner.returncode == 0
    assert "--app-probe-option" not in runner.stdout


def test_mh4u_native_answers_version_and_refuses_the_rest(built_app):
    _native, build = built_app
    exe = os.path.join(build, "bin", "mh4u-native")
    version = run([exe, "--version"])
    assert version.returncode == 0 and re.match(r"mh4u-native \S+\n", version.stdout)
    for args in (["--no-such-option"], ["positional"], []):
        refused = run([exe] + args)
        assert refused.returncode == 2, args


def test_app_modules_are_named_after_their_path_and_the_tests_run(built_app):
    _native, build = built_app
    libs = sorted(os.listdir(os.path.join(build, "lib")))
    for name in ("libmh4n_app_probe.a", "libmh4n_app_deep_inner.a", "libmh4n_app_shared.a",
                 "libmh4n_app_host.a"):
        assert name in libs, libs
    result = run(["ctest", "--test-dir", build, "--no-tests=error", "-R",
                  "^(probe_app_test|app_shared_test)$", "--output-on-failure"])
    assert result.returncode == 0, result.stdout + result.stderr
    for name in ("probe_app_test", "app_shared_test"):
        assert re.search(rf"Test\s+#\d+: {name}\b.*Passed", result.stdout), result.stdout


def test_a_native_src_module_cannot_depend_on_an_app_module(sdl3, tmp_path):
    native = str(tmp_path / "native")
    copy_native(native)
    copy_port_app(native)
    write(native, "src/broken/CMakeLists.txt", "mh4n_module(SOURCES b.cpp DEPENDS mh4n_app_shared)\n")
    write(native, "src/broken/b.cpp", "int broken_value() { return 1; }\n")
    result = run(configure_cmd(native, str(tmp_path / "build")) + ["-DMH4N_APP=ON"])
    assert result.returncode != 0
    message = " ".join(result.stderr.split())          # (CMake wraps the line)
    assert "mh4n_app_shared" in message and "not a module under native/src" in message


def test_an_app_module_cannot_depend_on_a_missing_app_module(sdl3, tmp_path):
    native = str(tmp_path / "native")
    copy_native(native)
    copy_port_app(native)
    write(native, "app/broken/CMakeLists.txt", "mh4n_app_module(SOURCES b.cpp DEPENDS mh4n_app_nope)\n")
    write(native, "app/broken/b.cpp", "int broken_value() { return 1; }\n")
    result = run(configure_cmd(native, str(tmp_path / "build")) + ["-DMH4N_APP=ON"])
    assert result.returncode != 0
    assert "mh4n_app_nope" in result.stderr
