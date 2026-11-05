"""tools/native/ctest_case_times.py (N0-42): the per-case time table from ctest's LastTest.log, on synthetic logs
in ctest's format (a block per test from `<i>/<n> Testing: <name>`, its output between `Output:` and
`<end of output>`, then the padded `Test time =   <s> sec`)."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TOOL = ROOT / "tools" / "native" / "ctest_case_times.py"
sys.path.insert(0, str(ROOT / "tools" / "native"))

import ctest_case_times as cct  # noqa: E402

RULE = "-" * 58


def block(index: int, total: int, name: str, output: list[str], seconds: str, passed: bool = True) -> str:
    """One test's block of a LastTest.log, as ctest writes it."""
    lines = [
        f"{index}/{total} Testing: {name}",
        f"{index}/{total} Test: {name}",
        f'Command: "/work/build/native/bin/{name}"',
        "Directory: /work/build/native/tests",
        f'"{name}" start time: Nov 05 10:00 UTC',
        "Output:",
        RULE,
        *output,
        "<end of output>",
        f"Test time = {seconds} sec",
        RULE,
        "Test Passed." if passed else "Test Failed.",
        f'"{name}" end time: Nov 05 10:05 UTC',
        f'"{name}" time elapsed: 00:00:00',
        RULE,
        "",
    ]
    return "\n".join(lines) + "\n"


def log(*blocks: str) -> str:
    return "Start testing: Nov 05 10:00 UTC\n" + RULE + "\n" + "".join(blocks) + "End testing: Nov 05 10:05 UTC\n"


def case(name: str, seconds: str) -> list[str]:
    """What doctest prints for a case with MH4N_TEST_DURATIONS=1: our start line, its preamble, its time."""
    return [
        f"case start: {name}",
        "=" * 40,
        "/work/native/tests/x_test.cpp:10:",
        f"TEST CASE:  {name}",
        "",
        f"{seconds} s: {name}",
    ]


PASSED = block(
    1, 2, "gpu_tev_test",
    ["[doctest] doctest version is \"2.4.11\"", "device: llvmpipe (LLVM 20.1.2, 256 bits)",
     *case("tev/one", "0.500000"), *case("tev/two", "12.250000"), *case("tev/three", "3.000000"),
     "[doctest] Status: SUCCESS!"],
    "  15.80",
)
KILLED = block(
    2, 2, "gpu_shadow_test",
    [*case("shadow/first", "2.000000"), *case("shadow/second", "0.010000"), "case start: shadow/stuck",
     "=" * 40, "device: llvmpipe (LLVM 20.1.2, 256 bits)"],
    "300.05", passed=False,
)


def run(capsys, tmp_path: Path, text: str, *args: str, newline: str = "\n") -> tuple[int, list[str]]:
    path = tmp_path / "LastTest.log"
    path.write_bytes(text.replace("\n", newline).encode("utf-8"))
    code = cct.main([str(path), *args])
    return code, capsys.readouterr().out.splitlines()


def test_passed_test_lists_its_cases_longest_first_above_the_minimum(capsys, tmp_path):
    code, out = run(capsys, tmp_path, log(PASSED))
    assert code == 0
    assert out == ["12.250000  gpu_tev_test  tev/two", "3.000000  gpu_tev_test  tev/three"]


def test_min_zero_lists_every_case_of_every_test(capsys, tmp_path):
    code, out = run(capsys, tmp_path, log(PASSED, KILLED), "--min", "0")
    assert code == 0
    assert out[:5] == [
        "12.250000  gpu_tev_test  tev/two",
        "3.000000  gpu_tev_test  tev/three",
        "2.000000  gpu_shadow_test  shadow/first",
        "0.500000  gpu_tev_test  tev/one",
        "0.010000  gpu_shadow_test  shadow/second",
    ]
    assert out[5:] == ["gpu_shadow_test: killed in shadow/stuck"]


def test_killed_test_names_the_case_that_was_running(capsys, tmp_path):
    code, out = run(capsys, tmp_path, log(KILLED))
    assert code == 0
    assert out == ["2.000000  gpu_shadow_test  shadow/first", "gpu_shadow_test: killed in shadow/stuck"]


def test_block_with_ctest_cut_off_message_warns(capsys, tmp_path):
    cut = block(
        1, 1, "gpu_shader_test",
        [*case("shader/a", "1.500000"), "case start: shader/b",
         "...", "The rest of the test output was removed since it exceeds the threshold of 1024 bytes."],
        "  42.00",
    )
    code, out = run(capsys, tmp_path, log(cut))
    assert code == 0
    assert out[0] == "1.500000  gpu_shader_test  shader/a"
    warnings = [line for line in out if "warning" in line]
    assert len(warnings) == 1 and "gpu_shader_test" in warnings[0] and "cut" in warnings[0]
    # a cut block's unfinished case is not reported as a kill (its time may have been removed with the rest): the
    # warning names it instead
    assert "shader/b" in warnings[0]
    assert not any("killed" in line for line in out)


def test_log_without_per_case_lines_says_the_variable_was_not_set(capsys, tmp_path):
    plain = block(1, 1, "common_test", ["[doctest] doctest version is \"2.4.11\"", "[doctest] Status: SUCCESS!"],
                  "   0.05")
    code, out = run(capsys, tmp_path, log(plain))
    assert code == 0
    assert len(out) == 1 and "MH4N_TEST_DURATIONS=1" in out[0] and "not set" in out[0]


def test_crlf_log_gives_case_names_without_carriage_returns(capsys, tmp_path):
    path = tmp_path / "LastTest.log"
    path.write_bytes(log(PASSED, KILLED).replace("\n", "\r\n").encode("utf-8"))
    assert cct.main([str(path), "--min", "0"]) == 0
    raw = capsys.readouterr().out  # not split into lines: splitting would hide a \r at the end of a name
    assert "\r" not in raw
    out = raw.split("\n")
    assert "12.250000  gpu_tev_test  tev/two" in out
    assert "gpu_shadow_test: killed in shadow/stuck" in out
    tests = cct.parse_log((tmp_path / "LastTest.log").read_text(encoding="utf-8", newline=None))
    assert [t.name for t in tests] == ["gpu_tev_test", "gpu_shadow_test"]
    assert all(not c.endswith("\r") for t in tests for c, _ in t.finished)
    assert tests[0].seconds == 15.80 and tests[1].seconds == 300.05  # the padded `Test time =   <s> sec`


def test_several_logs_are_read_together(capsys, tmp_path):
    first, second = tmp_path / "a.log", tmp_path / "b.log"
    first.write_text(log(PASSED), encoding="utf-8")
    second.write_text(log(KILLED), encoding="utf-8")
    assert cct.main([str(first), str(second)]) == 0
    out = capsys.readouterr().out.splitlines()
    assert out == ["12.250000  gpu_tev_test  tev/two", "3.000000  gpu_tev_test  tev/three",
                   "2.000000  gpu_shadow_test  shadow/first", "gpu_shadow_test: killed in shadow/stuck"]


def test_missing_file_and_bad_option_exit_2(tmp_path):
    missing = subprocess.run([sys.executable, str(TOOL), str(tmp_path / "nope.log")], capture_output=True, text=True)
    assert missing.returncode == 2 and "nope.log" in missing.stderr
    (tmp_path / "x.log").write_text(log(PASSED), encoding="utf-8")
    bad = subprocess.run([sys.executable, str(TOOL), str(tmp_path / "x.log"), "--min", "soon"],
                         capture_output=True, text=True)
    assert bad.returncode == 2
    unknown = subprocess.run([sys.executable, str(TOOL), str(tmp_path / "x.log"), "--bogus"],
                             capture_output=True, text=True)
    assert unknown.returncode == 2
