"""Kernel conformance programs (port/tests/conf_*.c and svc_test.c): our own 3DS programs
whose output is Azahar's, committed as port/tests/expected/<program>.txt (no game data).

- Always: every program builds, recompiles and links into a code library (paged and native).
- With MH4U_RUNNER (the reference runner, `--cpu jit --new-3ds off`): the output equals the
  expected file, twice in a row.
- With MH4N_RUNNER (the native runner): the output equals it too, except for the programs in
  NATIVE_NOT_YET.

To rewrite the expected files from the reference runner (they must come out the same twice):
    MH4U_RUNNER=path/to/run.sh python3 tests/test_conformance.py --regenerate [PROGRAM ...]
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PORT_TESTS = os.path.join(ROOT, "port", "tests")
EXPECTED = os.path.join(PORT_TESTS, "expected")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, PORT_TESTS)

PROGRAMS = ["svc_test", "conf_sync", "conf_wait", "conf_arbiter", "conf_timer", "conf_sched",
            "conf_mem", "conf_info", "conf_srv", "conf_services"]
# Programs the native runner does not pass yet, and what each is waiting for. The work package that
# provides it takes the program out of the set.
NATIVE_NOT_YET: set[str] = {
    # N1-05 does not alias shared-memory pages: a block mapped at a second address is refused
    "conf_mem",
}

NEEDED_TOOLS = ["clang", "ld.lld", "llvm-objcopy", "llvm-readelf"]
MAX_SOURCE_LINES = 300
RUN_MS = 5000
DEBUG_LINE = re.compile(r"OutputDebugString:\d+: (.*)$")


def have_toolchain() -> bool:
    return all(shutil.which(t) for t in NEEDED_TOOLS)


def build_program(out: str, program: str, native: bool = False) -> tuple[str, str]:
    """Builds the ELF, recompiles it and builds its code library: (elf, library). The native
    library (RC_NATIVE, flat memory) is the one the native runner loads."""
    import build_svc_test
    from recomp import build as rbuild
    elf = build_svc_test.build(out, program)
    if native:
        lib = rbuild.build(out, 2, "O1", rbuild.native_defines(True, False), "mh4u_code_native.so",
                           [rbuild.DLL_GLUE], extra_flags=["-fvisibility=hidden"])
    else:
        lib = rbuild.build(out, 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                           extra_flags=["-fvisibility=hidden"])
    return elf, lib


def debug_lines(text: str) -> list[str]:
    """The text of the OutputDebugString lines (Azahar's log puts its source line number after
    "OutputDebugString:"; it is not part of the output)."""
    return [m.group(1) for line in text.splitlines() if (m := DEBUG_LINE.search(line))]


def run_reference(runner: str, elf: str, lib: str, user_dir: str) -> list[str]:
    # a fresh --user directory: cfg's defaults, not the machine's settings
    r = subprocess.run([runner, elf, "--code", lib, "--cpu", "jit", "--new-3ds", "off", "--ms", str(RUN_MS),
                        "--user", user_dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       timeout=600)
    assert r.returncode == 0, (r.returncode, r.stdout[-2000:])
    return debug_lines(r.stdout)


def run_native(runner: str, elf: str, lib: str, user_dir: str) -> list[str]:
    r = subprocess.run([runner, elf, "--code", lib, "--ms", str(RUN_MS), "--user", user_dir],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
    assert r.returncode == 0, (r.returncode, r.stdout[-2000:])
    return debug_lines(r.stdout)


def expected_lines(program: str) -> list[str]:
    with open(os.path.join(EXPECTED, program + ".txt"), encoding="utf-8") as f:
        return f.read().splitlines()


pytestmark = pytest.mark.skipif(not have_toolchain(), reason="clang/lld toolchain not installed")


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    cache: dict[tuple[str, bool], tuple[str, str]] = {}

    def get(program: str, native: bool = False) -> tuple[str, str]:
        key = (program, native)
        if key not in cache:
            # the paged and the native library share a directory (one recompilation), as the
            # recompiled code does not depend on the mode
            out = str(tmp_path_factory.mktemp(program) if (program, not native) not in cache
                      else os.path.dirname(cache[(program, not native)][0]))
            cache[key] = build_program(out, program, native)
        return cache[key]
    return get


@pytest.mark.parametrize("program", PROGRAMS)
def test_program_builds_and_recompiles(built, program):
    elf, lib = built(program)
    assert os.path.exists(elf) and os.path.exists(lib)
    _, native_lib = built(program, native=True)
    assert os.path.exists(native_lib)


def all_programs() -> list[str]:
    import build_svc_test
    return build_svc_test.programs()


@pytest.mark.parametrize("program", all_programs())
def test_every_port_test_program_compiles(tmp_path, program):
    """Every port/tests/*.c program (io_test and bench included, not only the conformance ones)
    compiles and links, so a clash with svc.h is caught without a runner."""
    import build_svc_test
    assert os.path.exists(build_svc_test.build(str(tmp_path), program))


def test_every_port_test_program_is_known():
    assert {"svc_test", "io_test", "bench"} <= set(all_programs())
    assert set(PROGRAMS) <= set(all_programs())


@pytest.mark.parametrize("program", [p for p in PROGRAMS if p.startswith("conf_")])
def test_conformance_programs_are_short(program):
    with open(os.path.join(PORT_TESTS, program + ".c"), encoding="utf-8") as f:
        assert len(f.read().splitlines()) < MAX_SOURCE_LINES


@pytest.mark.parametrize("program", PROGRAMS)
def test_expected_file_is_wellformed(program):
    lines = expected_lines(program)
    assert lines[0] == f"{program}: start"
    assert lines[-1] == f"{program}: done"
    assert all(line.strip() == line and line for line in lines)
    assert all("OutputDebugString" not in line for line in lines)
    if program.startswith("conf_"):
        # one "<check>: <value>" line per check
        assert all(re.fullmatch(r"[A-Za-z0-9_.:]+: \S.*", line) for line in lines[1:-1]), lines


def test_native_not_yet_names_conformance_programs():
    assert NATIVE_NOT_YET <= set(PROGRAMS)
    # what the native runner must pass today
    assert {"svc_test", "conf_sync", "conf_wait", "conf_arbiter", "conf_timer", "conf_sched"} <= (
        set(PROGRAMS) - NATIVE_NOT_YET)


def test_every_conf_program_is_listed():
    on_disk = sorted(f[:-2] for f in os.listdir(PORT_TESTS) if f.startswith("conf_") and f.endswith(".c"))
    assert on_disk == sorted(p for p in PROGRAMS if p.startswith("conf_"))
    on_disk = sorted(f[:-4] for f in os.listdir(EXPECTED) if f.endswith(".txt"))
    assert on_disk == sorted(PROGRAMS)


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="reference runner not set (MH4U_RUNNER)")
@pytest.mark.parametrize("program", PROGRAMS)
def test_reference_runner_matches_expected_twice(built, tmp_path, program):
    elf, lib = built(program)
    runner = os.environ["MH4U_RUNNER"]
    first = run_reference(runner, elf, lib, str(tmp_path / "user1"))
    second = run_reference(runner, elf, lib, str(tmp_path / "user2"))
    assert first == expected_lines(program)
    assert second == first


@pytest.mark.skipif(not os.environ.get("MH4N_RUNNER"), reason="native runner not set (MH4N_RUNNER)")
@pytest.mark.parametrize("program", [p for p in PROGRAMS if p not in NATIVE_NOT_YET])
def test_native_runner_matches_expected(built, tmp_path, program):
    elf, lib = built(program, native=True)
    assert run_native(os.environ["MH4N_RUNNER"], elf, lib, str(tmp_path / "user")) == expected_lines(program)


def regenerate(programs: list[str]) -> int:
    runner = os.environ.get("MH4U_RUNNER")
    if not runner:
        print("set MH4U_RUNNER to the reference runner", file=sys.stderr)
        return 2
    os.makedirs(EXPECTED, exist_ok=True)
    status = 0
    for program in programs or PROGRAMS:
        with tempfile.TemporaryDirectory() as tmp:
            elf, lib = build_program(os.path.join(tmp, "build"), program)
            runs = [run_reference(runner, elf, lib, os.path.join(tmp, f"user{i}")) for i in range(2)]
        if runs[0] != runs[1] or not runs[0] or runs[0][-1] != f"{program}: done":
            print(f"{program}: unstable or incomplete output; not written", file=sys.stderr)
            status = 1
            continue
        with open(os.path.join(EXPECTED, program + ".txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(runs[0]) + "\n")
        print(f"{program}: {len(runs[0])} lines")
    return status


if __name__ == "__main__":
    if sys.argv[1:2] == ["--regenerate"]:
        sys.exit(regenerate(sys.argv[2:]))
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
