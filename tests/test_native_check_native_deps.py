"""tools/native/check_native_deps.py: native/ must not use Azahar (synthetic trees)."""
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
ROOT = os.path.abspath(os.path.join(HERE, ".."))
TOOL = os.path.join(ROOT, "tools", "native", "check_native_deps.py")
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))

import check_native_deps as cnd  # noqa: E402


def write(root, rel, text):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


CLEAN_CMAKE = """\
cmake_minimum_required(VERSION 3.25)
# the network layer and video_core are only words in this comment
mh4n_module(SOURCES kernel.cpp DEPENDS mh4n_common mh4n_network)
target_link_libraries(x PRIVATE mh4n_video_core fmt::fmt)
"""

CLEAN_CPP = """\
#include <vector>
#include "mh4n/log.h"
#include "kernel.h"
// #include "core/hle/kernel.h"  (a comment is not an include)
"""


@pytest.fixture
def tree(tmp_path):
    write(tmp_path, "native/CMakeLists.txt", CLEAN_CMAKE)
    write(tmp_path, "native/src/kernel/kernel.cpp", CLEAN_CPP)
    write(tmp_path, "native/src/kernel/kernel.h", "#pragma once\n")
    return tmp_path


def run(root):
    return subprocess.run(
        [sys.executable, TOOL, "--root", str(root)], capture_output=True, text=True, timeout=120
    )


def test_clean_tree(tree):
    result = run(tree)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ok" in result.stdout
    problems, count = cnd.check(tree)
    assert problems == [] and count == 3


@pytest.mark.parametrize(
    "include",
    [
        '#include "core/hle/kernel/kernel.h"',
        "#include <video_core/gpu.h>",
        '#include "audio_core/dsp_interface.h"',
        '#include "common/common_types.h"',
        '#include "network/room.h"',
        '#include "input_common/main.h"',
        '#include "web_service/web_backend.h"',
        '#include "citra_qt/main.h"',
        '#include "citra/config.h"',
        '  #  include   "core/core.h"',
        '#import "common/x.h"',
    ],
)
def test_azahar_include_is_an_offence(tree, include):
    write(tree, "native/src/kernel/bad.cpp", "#include <vector>\n" + include + "\n")
    result = run(tree)
    assert result.returncode == 1
    assert "native/src/kernel/bad.cpp:2:" in result.stdout
    assert result.stdout.count("\n") == 1


@pytest.mark.parametrize("suffix", [".h", ".hpp", ".cc", ".cxx", ".inl", ".mm", ".c"])
def test_every_source_suffix_is_scanned(tree, suffix):
    write(tree, f"native/runner/x{suffix}", '#include "core/core.h"\n')
    assert run(tree).returncode == 1


def test_own_and_standard_headers_are_fine(tree):
    write(
        tree,
        "native/src/kernel/ok.cpp",
        '#include <core.h>\n#include "mh4n/core.h"\n#include "corefoo.h"\n#include <network>\n'
        '#include "mh4n/common/x.h"\n',
    )
    # "core.h" has no slash, so it is not a "core/" path; a path with the prefix mid-way is fine
    assert run(tree).returncode == 0


@pytest.mark.parametrize(
    "line",
    [
        "target_link_libraries(x PRIVATE citra_common)",
        "target_link_libraries(x PUBLIC citra_core fmt::fmt)",
        "target_link_libraries(x PRIVATE video_core)",
        "target_link_libraries(x PRIVATE audio_core)",
        "target_link_libraries(x PRIVATE network)",
        "target_link_libraries(x PRIVATE input_common)",
        "target_link_libraries(x PRIVATE web_service)",
        "mh4n_module(SOURCES a.cpp DEPENDS network)",
        "add_subdirectory(../../port/azahar/src azahar)",
        'include("${CMAKE_SOURCE_DIR}/../port/azahar/CMakeLists.txt")',
        "target_link_libraries(x PRIVATE $<TARGET_FILE:citra_core>)",
    ],
)
def test_azahar_target_or_path_in_cmake_is_an_offence(tree, line):
    write(tree, "native/src/kernel/CMakeLists.txt", "# fine\n" + line + "\n")
    result = run(tree)
    assert result.returncode == 1, result.stdout
    assert "native/src/kernel/CMakeLists.txt:2:" in result.stdout


def test_cmake_module_files_are_scanned(tree):
    write(tree, "native/cmake/Deps.cmake", "target_link_libraries(x PRIVATE citra_core)\n")
    result = run(tree)
    assert result.returncode == 1
    assert "native/cmake/Deps.cmake:1:" in result.stdout


def test_similar_names_are_not_targets(tree):
    write(
        tree,
        "native/src/kernel/CMakeLists.txt",
        "mh4n_module(SOURCES network.cpp video_core.cpp DEPENDS mh4n_network)\n"
        "# network video_core port/azahar/\n"
        "set(port_azahar_dir x)\n",
    )
    assert run(tree).returncode == 0


def test_hash_inside_quotes_is_not_a_comment(tree):
    write(tree, "native/src/kernel/CMakeLists.txt", 'set(x "#" citra_core)\n')
    assert run(tree).returncode == 1


def test_several_offences_are_all_listed(tree):
    write(tree, "native/a.cpp", '#include "core/a.h"\n#include "common/b.h"\n')
    write(tree, "native/CMakeLists.txt", "target_link_libraries(x PRIVATE citra_core)\n")
    result = run(tree)
    assert result.returncode == 1
    lines = result.stdout.strip().splitlines()
    assert len(lines) == 3
    assert any(l.startswith("native/a.cpp:1:") for l in lines)
    assert any(l.startswith("native/a.cpp:2:") for l in lines)
    assert any(l.startswith("native/CMakeLists.txt:1:") for l in lines)
    assert "3 offence" in result.stderr


PORT_FILES = ("fiber.cpp", "fiber.h", "schedule.cpp", "schedule.h", "hooks.cpp", "hooks.h", "platform.h")


def test_port_src_files_are_checked_when_native_uses_them(tree):
    write(tree, "port/src/hooks.h", '#include "core/frontend/top_screen.h"\n')
    write(tree, "port/src/fiber.h", "#include <memory>\n")
    write(tree, "port/src/arm_recomp.cpp", '#include "core/core.h"\n')  # not on the list
    # native does not mention port/src: nothing to check
    assert run(tree).returncode == 0
    write(
        tree,
        "native/src/kernel/CMakeLists.txt",
        "mh4n_module(SOURCES kernel.cpp)\ntarget_include_directories(k PRIVATE ../../../port/src)\n",
    )
    # the bare directory (N1-01 follow-up: naming only fiber.cpp no longer checks hooks.h, see
    # test_only_fiber_named_*): every listed file that exists is checked; hooks.h does
    result = run(tree)
    assert result.returncode == 1
    assert "port/src/hooks.h:1:" in result.stdout
    assert "fiber.h" not in result.stdout
    assert "arm_recomp" not in result.stdout


@pytest.mark.parametrize("name", PORT_FILES)
def test_each_listed_port_file_is_checked(tree, name):
    write(tree, f"port/src/{name}", '#include "common/common_types.h"\n')
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + "// uses port/src\n")
    result = run(tree)
    assert result.returncode == 1
    assert f"port/src/{name}:1:" in result.stdout


def _port_tree(tree):
    for name in PORT_FILES:
        write(tree, f"port/src/{name}", '#include "core/core.h"\n#include <vector>\n')
    return tree


def _checked(result):
    return {l.split(":")[0] for l in result.stdout.splitlines()}


def test_only_fiber_named_checks_fiber_pair_not_hooks(tree):
    _port_tree(tree)
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/fiber.cpp)\n")
    result = run(tree)
    assert result.returncode == 1
    assert _checked(result) == {"port/src/fiber.cpp", "port/src/fiber.h"}
    assert "hooks" not in result.stdout


def test_only_fiber_named_with_clean_fiber_and_azahar_bound_hooks_is_ok(tree):
    write(tree, "port/src/fiber.cpp", "#include <memory>\n")
    write(tree, "port/src/fiber.h", "#pragma once\n")
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "port/src/hooks.h", '#include "common/common_types.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/fiber.cpp)\n")
    result = run(tree)
    assert result.returncode == 0, result.stdout
    assert "ok" in result.stdout


def test_hooks_named_checks_hooks_pair_with_path_and_line(tree):
    write(tree, "port/src/hooks.cpp", "#include <vector>\n#include \"core/core.h\"\n")
    write(tree, "port/src/hooks.h", '#include "common/common_types.h"\n')
    write(tree, "port/src/fiber.h", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES ../../../port/src/hooks.cpp)\n")
    result = run(tree)
    assert result.returncode == 1
    assert "port/src/hooks.cpp:2:" in result.stdout
    assert "port/src/hooks.h:1:" in result.stdout
    assert "fiber" not in result.stdout


def test_header_named_checks_its_cpp(tree):
    write(tree, "port/src/schedule.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + '#include "../../../port/src/schedule.h"\n')
    result = run(tree)
    assert result.returncode == 1
    assert _checked(result) == {"port/src/schedule.cpp"}


def test_platform_h_named_checks_platform_h_only(tree):
    _port_tree(tree)
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + '#include "../../../port/src/platform.h"\n')
    result = run(tree)
    assert _checked(result) == {"port/src/platform.h"}


def test_unlisted_file_named_is_checked(tree):
    write(tree, "port/src/arm_recomp.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/arm_recomp.cpp)\n")
    result = run(tree)
    assert result.returncode == 1
    assert "port/src/arm_recomp.cpp:1:" in result.stdout


def test_bare_directory_checks_every_listed_file(tree):
    _port_tree(tree)
    write(tree, "port/src/arm_recomp.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "target_include_directories(k PRIVATE ../../../port/src)\n")
    result = run(tree)
    assert _checked(result) == {f"port/src/{n}" for n in PORT_FILES}


@pytest.mark.parametrize("text", ["../port/src/*.cpp", "${PORT_ROOT}/port/src/", "port/src ", "port/src/${X}.cpp"])
def test_glob_or_variable_counts_as_bare_directory(tree, text):
    _port_tree(tree)
    write(tree, "native/src/kernel/CMakeLists.txt", f"file(GLOB S {text})\n")
    assert _checked(run(tree)) == {f"port/src/{n}" for n in PORT_FILES}


def test_native_mentioning_nothing_checks_nothing_from_port_src(tree):
    _port_tree(tree)
    result = run(tree)
    assert result.returncode == 0
    assert "(3 files)" in result.stdout


def test_cmake_comment_mention_is_ignored_but_source_comment_counts(tree):
    _port_tree(tree)
    write(tree, "native/src/kernel/CMakeLists.txt", "# see ../../../port/src/fiber.cpp\nmh4n_module(SOURCES k.cpp)\n")
    assert run(tree).returncode == 0
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + "// like port/src/fiber.cpp\n")
    assert _checked(run(tree)) == {"port/src/fiber.cpp", "port/src/fiber.h"}


def test_a_local_include_of_a_named_file_is_checked_transitively(tree):
    write(tree, "port/src/fiber.cpp", '#include "fiber.h"\n')
    write(tree, "port/src/fiber.h", '#pragma once\n#include "hooks.h"\n#include <vector>\n')
    write(tree, "port/src/hooks.h", '#include "inner/deep.h"\n')
    write(tree, "port/src/inner/deep.h", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/fiber.cpp)\n")
    result = run(tree)
    assert result.returncode == 1
    assert "port/src/inner/deep.h:1:" in result.stdout
    assert _checked(result) == {"port/src/inner/deep.h"}
    write(tree, "port/src/inner/deep.h", "#pragma once\n")
    assert run(tree).returncode == 0


@pytest.mark.parametrize("cmake", [
    'set(P "${MH4N_REPO_ROOT}/port")\nmh4n_module(SOURCES k.cpp ${P}/src/hooks.cpp)\n',
    'mh4n_module(SOURCES k.cpp ../../../port/./src/hooks.cpp)\n',
    'mh4n_module(SOURCES k.cpp ../../../port//src/hooks.cpp)\n',
    'mh4n_module(SOURCES k.cpp ../../../port/${SUB}/hooks.cpp)\n',
    'set(P ../../../port)\n',
])
def test_a_path_that_hides_port_src_counts_as_the_bare_directory(tree, cmake):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", cmake)
    result = run(tree)
    assert result.returncode == 1, cmake
    assert "port/src/hooks.cpp:1:" in result.stdout


def test_port_paths_that_are_not_src_do_not_widen_the_check(tree):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt",
          'set(PROGRAM "${MH4N_REPO_ROOT}/port/tests/codehost_test.c")\n# the port\n')
    assert run(tree).returncode == 0


@pytest.mark.parametrize("cmake", [
    'set(P "${R}/port/")\nmh4n_module(SOURCES k.cpp ${P}src/hooks.cpp)\n',
    'set(P ${R}/port/)\nmh4n_module(SOURCES k.cpp ${P}src/hooks.cpp)\n',
    'set(DIRS ${R}/port;${R}/other)\n',
    'mh4n_module(SOURCES k.cpp ../../../port/tests/../src/hooks.cpp)\n',
    'mh4n_module(SOURCES k.cpp ..\\..\\..\\port\\src\\hooks.cpp)\n',
])
def test_more_ways_to_hide_port_src_count_as_the_bare_directory(tree, cmake):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", cmake)
    result = run(tree)
    assert result.returncode == 1, cmake
    assert "port/src/hooks.cpp:1:" in result.stdout


def test_a_backslash_include_path_in_a_native_source_names_the_port_file(tree):
    _port_tree(tree)
    write(tree, "port/src/hooks.h", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + '#include "..\\..\\..\\port\\src\\hooks.h"\n')
    assert "port/src/hooks.h:1:" in run(tree).stdout


@pytest.mark.parametrize("cmake", [
    'message(STATUS "mh4n: building the native port")\n',
    'project(mh4n DESCRIPTION "the native port of a game")\n',
    'find_package(port QUIET)\n',
    'set(SUPPORT_DIR support)\n',
])
def test_the_word_port_alone_is_not_the_port_directory(tree, cmake):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", cmake)
    result = run(tree)
    assert result.returncode == 0, result.stdout


def test_angle_includes_are_followed_cycles_end_and_outside_paths_are_not_followed(tree, tmp_path_factory):
    outside = tmp_path_factory.mktemp("outside") / "evil.h"
    outside.write_text('#include "core/core.h"\n')
    write(tree, "port/src/fiber.cpp", '#include "fiber.h"\n')
    write(tree, "port/src/fiber.h", '#include <angle.h>\n#include "../hooks_outside.h"\n#include "%s"\n#include "a.h"\n' % outside)
    write(tree, "port/src/angle.h", '#pragma once\n')
    write(tree, "port/src/a.h", '#include "b.h"\n')
    write(tree, "port/src/b.h", '#include "a.h"\n')                 # a cycle
    write(tree, "port/hooks_outside.h", '#include "core/core.h"\n')  # one level above port/src
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/fiber.cpp)\n")
    result = run(tree)
    assert result.returncode == 0, result.stdout
    write(tree, "port/src/angle.h", '#include "common/common_types.h"\n')
    result = run(tree)
    assert result.returncode == 1
    assert _checked(result) == {"port/src/angle.h"}


@pytest.mark.parametrize("cmake", [
    'set(DIRS ${R}/port/;${R}/other)\n',               # only the ';' after 'port/' matches
    'set(P ${R}/port/ )\n',                            # only a blank after 'port/' matches
    'set(P ${R}/port )\n',                             # only a blank after 'port' matches
    'set(P\n    ${R}/port/\n)\n',                      # 'port/' at the end of a line
    'set(P ${R}/port/',                                # 'port/' at the very end of the file
])
def test_every_spelling_of_a_port_directory_variable_is_seen(tree, cmake):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", cmake)
    result = run(tree)
    assert result.returncode == 1, repr(cmake)
    assert "port/src/hooks.cpp:1:" in result.stdout


@pytest.mark.parametrize("text, cmake", [
    ("target_include_directories(k PRIVATE third_party/support/src)\n", True),
    ("target_include_directories(k PRIVATE ../transport/src ../report/azahar)\n", True),
    ("// see support/src and transport/src for details\n", False),
])
def test_words_that_end_in_port_are_not_the_port_directory(tree, text, cmake):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    if cmake:
        write(tree, "native/src/kernel/CMakeLists.txt", text)
    else:
        write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + text)
    result = run(tree)
    assert result.returncode == 0, result.stdout


@pytest.mark.parametrize("cmake", [
    "add_subdirectory(../../../port//azahar/src azahar)\n",
    "add_subdirectory(../../../port/./azahar/src azahar)\n",
    "add_subdirectory(../../../port/tests/../azahar/src azahar)\n",
    "add_subdirectory(..\\..\\..\\port\\azahar\\src azahar)\n",
])
def test_port_azahar_is_found_however_the_path_is_spelled(tree, cmake):
    result = run(write_cmake(tree, cmake))
    assert result.returncode == 1, cmake
    assert "uses a path under port/azahar/" in result.stdout


def write_cmake(tree, text):
    write(tree, "native/src/kernel/CMakeLists.txt", text)
    return tree


@pytest.mark.parametrize("include", ['"core\\core.h"', '"./core/core.h"', '"video_core//pica.h"', '<common/./x.h>'])
def test_azahar_includes_are_found_however_the_path_is_spelled(tree, include):
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + f"#include {include}\n")
    assert run(tree).returncode == 1, include


def test_a_backslash_include_in_a_checked_port_file_is_followed(tree):
    write(tree, "port/src/fiber.cpp", '#include "fiber.h"\n')
    write(tree, "port/src/fiber.h", '#include "inner\\deep.h"\n')
    write(tree, "port/src/inner/deep.h", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/fiber.cpp)\n")
    assert _checked(run(tree)) == {"port/src/inner/deep.h"}


def test_a_name_too_long_for_the_file_system_does_not_crash_the_check(tree):
    _port_tree(tree)
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + "// see port/src/" + "a" * 300 + ".h\n")
    result = run(tree)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Traceback" not in result.stderr
    write(tree, "port/src/fiber.cpp", '#include "fiber.h"\n')
    write(tree, "port/src/fiber.h", '#include "' + "b" * 300 + '.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", "mh4n_module(SOURCES k.cpp ../../../port/src/fiber.cpp)\n")
    result = run(tree)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Traceback" not in result.stderr


@pytest.mark.parametrize("cmake", [
    "target_compile_options(k PRIVATE -Iport/azahar/src)\n",
    "target_compile_options(k PRIVATE -isystemport/azahar/src)\n",
    "target_compile_options(k PRIVATE -iquoteport/azahar/src)\n",
    "target_compile_options(k PRIVATE /Iport/azahar/src)\n",
])
def test_a_compiler_flag_glued_to_port_azahar_is_found(tree, cmake):
    result = run(write_cmake(tree, cmake))
    assert result.returncode == 1, cmake
    assert "uses a path under port/azahar/" in result.stdout


@pytest.mark.parametrize("flag", ["-Iport/src", "-isystemport/src", "/Iport/src", "-iquoteport/src"])
def test_a_compiler_flag_glued_to_port_src_is_the_bare_directory(tree, flag):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", f"target_compile_options(k PRIVATE {flag})\n")
    result = run(tree)
    assert result.returncode == 1, flag
    assert "port/src/hooks.cpp:1:" in result.stdout


@pytest.mark.parametrize("prefix", ["my-", "my_", "my.", "my+", "1", "x"])
def test_every_character_that_continues_a_name_blocks_port_src_and_port_azahar(tree, prefix):
    _port_tree(tree)
    write(tree, "port/src/hooks.cpp", '#include "core/core.h"\n')
    write(tree, "native/src/kernel/CMakeLists.txt", f"include_directories({prefix}port/src {prefix}port/azahar/src)\n")
    result = run(tree)
    assert result.returncode == 0, (prefix, result.stdout)


@pytest.mark.parametrize("include", ['"tests/../core/core.h"', '"x/y/../../video_core/pica.h"', '"a\\..\\common\\x.h"'])
def test_relative_parent_segments_in_an_include_are_resolved(tree, include):
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + f"#include {include}\n")
    assert run(tree).returncode == 1, include


@pytest.mark.parametrize("header", ["nihstro/shader_binary.h", "teakra/teakra.h", "dynarmic/interface/A32/a32.h",
                                    "soundtouch/SoundTouch.h", "cryptopp/sha.h", "xbyak/xbyak.h"])
def test_azahars_externals_are_forbidden_includes(tree, header):
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + f"#include <{header}>\n")
    result = run(tree)
    assert result.returncode == 1
    assert header in result.stdout


def test_no_native_directory(tmp_path):
    result = run(tmp_path)
    assert result.returncode == 2
    assert "no native/" in result.stderr


def test_the_checkout_itself_is_clean():
    if not os.path.isdir(os.path.join(ROOT, "native")):
        pytest.skip("no native/ directory in this checkout")
    result = run(ROOT)
    assert result.returncode == 0, result.stdout + result.stderr


# ---- the app tree (native/app, N2b-03) ----

APP_CMAKE = "mh4n_app_module(SOURCES main.cpp ../../../port/app/config.cpp)\n"


@pytest.mark.parametrize("include", ["<SDL.h>", '"SDL.h"', "<SDL2/SDL.h>", "<SDL_vulkan.h>", '"SDL_syswm.h"',
                                     "<httplib.h>", "<nlohmann/json.hpp>", '"json.hpp"'])
def test_sdl2_httplib_and_json_includes_are_offences_in_the_app_tree(tree, include):
    write(tree, "native/app/host/host.cpp", CLEAN_CPP + f"#include {include}\n")
    result = run(tree)
    assert result.returncode == 1
    assert "native/app/host/host.cpp:5:" in result.stdout


@pytest.mark.parametrize("include", ["<SDL3/SDL.h>", "<SDL3/SDL_main.h>", '"imgui.h"', "<imgui_impl_sdl3.h>",
                                     "<INIReader.h>", "<ini.h>", '"app_window.h"'])
def test_sdl3_imgui_and_inih_includes_are_fine_in_the_app_tree(tree, include):
    write(tree, "native/app/host/host.cpp", CLEAN_CPP + f"#include {include}\n")
    result = run(tree)
    assert result.returncode == 0, result.stdout + result.stderr


def test_azahar_include_is_an_offence_in_the_app_tree(tree):
    write(tree, "native/app/host/host.cpp", CLEAN_CPP + '#include "core/core.h"\n')
    assert run(tree).returncode == 1


def test_the_rules_of_the_app_tree_do_not_reach_the_other_trees(tree):
    # native/tests and the runner may include what they like of SDL2's names (they never will), and
    # an SDL2 include outside native/app is not this tool's business, only SDL3 and ImGui under src
    write(tree, "native/tests/x_test.cpp", CLEAN_CPP + "#include <httplib.h>\n")
    assert run(tree).returncode == 0


@pytest.mark.parametrize("include", ["<SDL3/SDL.h>", "<SDL.h>", '"SDL3/SDL_video.h"', '"imgui.h"',
                                     "<imgui_impl_sdl3.h>", '"ImGui/imgui.h"'])
def test_sdl_and_imgui_are_offences_under_native_src(tree, include):
    write(tree, "native/src/kernel/kernel.cpp", CLEAN_CPP + f"#include {include}\n")
    result = run(tree)
    assert result.returncode == 1
    assert "native/src/kernel/kernel.cpp:5:" in result.stdout


def test_a_header_under_native_src_is_checked_too(tree):
    write(tree, "native/src/kernel/kernel.h", "#pragma once\n#include <SDL3/SDL.h>\n")
    result = run(tree)
    assert result.returncode == 1 and "native/src/kernel/kernel.h:2:" in result.stdout


def _app_port_tree(tree):
    write(tree, "native/app/shared/CMakeLists.txt", APP_CMAKE)
    write(tree, "native/app/shared/main.cpp", CLEAN_CPP)
    write(tree, "port/app/config.cpp", '#include "config.h"\n#include "runtime.h"\n')
    write(tree, "port/app/config.h", "#pragma once\n")
    write(tree, "port/app/runtime.h", '#pragma once\n#include "app_window.h"\n')
    write(tree, "port/app/app_window.h", "#pragma once\nstruct SDL_Window;\n")
    write(tree, "port/app/window.cpp", "#include <SDL.h>\n#include \"core/core.h\"\n")   # the Azahar app's own
    write(tree, "port/app/input.cpp", "#include <SDL.h>\n")
    write(tree, "port/app/util/file.cpp", "#include <httplib.h>\n")


def test_a_port_app_source_the_app_tree_names_is_checked_with_what_it_includes(tree):
    _app_port_tree(tree)
    assert run(tree).returncode == 0                        # clean, and window.cpp is not named
    write(tree, "port/app/app_window.h", '#pragma once\n#include <SDL.h>\n')
    result = run(tree)
    assert result.returncode == 1
    assert "port/app/app_window.h:2:" in result.stdout      # reached through config.cpp -> runtime.h


def test_a_named_port_app_source_with_azahar_or_httplib_includes_is_an_offence(tree):
    _app_port_tree(tree)
    write(tree, "port/app/config.cpp", '#include "config.h"\n#include "core/core.h"\n#include "httplib.h"\n')
    result = run(tree)
    assert result.returncode == 1
    assert "port/app/config.cpp:2:" in result.stdout and "port/app/config.cpp:3:" in result.stdout


def test_a_port_app_header_named_checks_its_cpp(tree):
    _app_port_tree(tree)
    write(tree, "native/app/shared/CMakeLists.txt", "mh4n_app_module(SOURCES m.cpp ../../../port/app/input.h)\n")
    write(tree, "port/app/input.h", "#pragma once\n")
    result = run(tree)
    assert result.returncode == 1 and "port/app/input.cpp:1:" in result.stdout


def test_a_port_app_file_not_named_is_not_checked(tree):
    _app_port_tree(tree)
    write(tree, "native/app/shared/CMakeLists.txt", "mh4n_app_module(SOURCES m.cpp)\ntarget_include_directories(m PUBLIC ../../../port/app)\n")
    assert run(tree).returncode == 0                        # only the directory, as an include path


def test_a_glob_after_port_app_checks_every_shared_source(tree):
    _app_port_tree(tree)
    write(tree, "native/app/shared/CMakeLists.txt", "file(GLOB s ../../../port/app/*.cpp)\n")
    write(tree, "port/app/ui/menu.cpp", "#include <SDL2/SDL.h>\n")
    write(tree, "port/app/ui/render_vk.cpp", "#include <SDL.h>\n")    # the Azahar app's renderer
    result = run(tree)
    assert result.returncode == 1
    checked = _checked(result)
    assert {"port/app/input.cpp", "port/app/util/file.cpp", "port/app/ui/menu.cpp"} <= checked
    assert "port/app/window.cpp" not in checked and "port/app/ui/render_vk.cpp" not in checked


def test_a_port_app_mention_in_a_cmake_comment_is_ignored(tree):
    _app_port_tree(tree)
    write(tree, "native/app/shared/CMakeLists.txt", "# ../../../port/app/input.cpp\nmh4n_app_module(SOURCES m.cpp)\n")
    assert run(tree).returncode == 0


def test_the_third_party_files_of_the_app_tree_are_scanned_for_azahar_but_name_no_sources(tree):
    write(tree, "native/app/third_party/imgui/backends/imgui_impl_sdl3.cpp", '#include "imgui.h"\n#include <SDL3/SDL.h>\n')
    write(tree, "native/third_party/inih/inih/ini.h", "#pragma once\n")
    assert run(tree).returncode == 0
