#!/usr/bin/env python3
"""The per-case times of the doctest binaries, from ctest's LastTest.log (N0-42).

With MH4N_TEST_DURATIONS=1 every binary linked with native/tests/support/test_main.cpp prints `case start: <name>`
when a case starts and doctest's `<seconds> s: <name>` when it ends. ctest writes every test's output, passed or not,
to build/<tree>/Testing/Temporary/LastTest.log (`--output-on-failure` prints only the failed ones), and each ctest run
writes that file again.

    python3 tools/native/ctest_case_times.py LOG... [--min SECONDS]

Prints `<seconds>  <test>  <case>` for every finished case of at least --min seconds (default 1.0), longest first;
then `<test>: killed in <case>` for each test whose last started case has no time (ctest killed it at its TIMEOUT, or
it died where doctest's crash handler could not report it; on Linux doctest reports a crash itself and prints the
case's time); a warning for each block ctest cut; and a note when no per-case line was found at all. Exit 0 after
printing; 2 for a missing file or a bad option.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

BLOCK_START = re.compile(r"^\d+/\d+ Testing: (.+)$")
CASE_END = re.compile(r"^(\d+\.\d{6}) s: (.+)$")
CASE_START = re.compile(r"^case start: (.+)$")
TEST_TIME = re.compile(r"^Test time =\s+([0-9.]+) sec")
# ctest's message when it cut a test's output at its size limit. CTest 4.4.4's ctest.exe holds "[This part of the test
# output was removed since it exceeds the threshold of N bytes.]"; before 3.24 the message was "The rest of the test
# output was removed since it exceeds the threshold of N bytes."; this substring matches both. CTest 4.4.4 does not
# cut LastTest.log at all (passed outputs of up to 69 KB were kept whole at the default 1024-byte limit), so the check
# is a guard for other CTest versions.
CUT_OFF = "test output was removed since it exceeds the threshold"


@dataclass
class TestBlock:
    name: str
    finished: list[tuple[str, float]] = field(default_factory=list)  # (case, seconds), in output order
    started: int = 0
    running: str | None = None  # the last started case with no time after it
    seconds: float | None = None  # ctest's `Test time`
    cut: bool = False


def parse_log(text: str) -> list[TestBlock]:
    """The test blocks of one LastTest.log's text (lines already split on any newline)."""
    tests: list[TestBlock] = []
    current: TestBlock | None = None
    for line in text.splitlines():
        start = BLOCK_START.match(line)
        if start:
            current = TestBlock(start.group(1).strip())
            tests.append(current)
            continue
        if current is None:
            continue
        end = CASE_END.match(line)
        if end:
            current.finished.append((end.group(2), float(end.group(1))))
            current.running = None
            continue
        began = CASE_START.match(line)
        if began:
            current.started += 1
            current.running = began.group(1)
            continue
        time = TEST_TIME.match(line)
        if time:
            current.seconds = float(time.group(1))
            continue
        if CUT_OFF in line:
            current.cut = True
    return tests


def report(tests: list[TestBlock], minimum: float) -> list[str]:
    lines: list[str] = []
    rows = [(seconds, test.name, case) for test in tests for case, seconds in test.finished if seconds >= minimum]
    rows.sort(key=lambda row: row[0], reverse=True)  # stable: equal times keep the logs' order
    lines += [f"{seconds:.6f}  {name}  {case}" for seconds, name, case in rows]
    for test in tests:
        if test.running is not None and not test.cut:
            lines.append(f"{test.name}: killed in {test.running}")
    for test in tests:
        if test.cut:
            took = f" (test time {test.seconds:.2f} s)" if test.seconds is not None else ""
            last = f"; the last case started in what is left: {test.running}" if test.running is not None else ""
            lines.append(f"ctest_case_times: warning: ctest cut the output of {test.name}{took}, "
                         f"so its per-case lines are incomplete{last}")
    if not any(test.finished or test.started for test in tests):
        lines.append("ctest_case_times: no per-case line: MH4N_TEST_DURATIONS=1 was not set for these tests")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="The per-case times of the doctest binaries from ctest's LastTest.log.")
    parser.add_argument("logs", nargs="+", metavar="LOG", help="a ctest LastTest.log")
    parser.add_argument("--min", type=float, default=1.0, metavar="SECONDS",
                        help="list only the cases that took at least this long (default 1.0)")
    args = parser.parse_args(argv)

    tests: list[TestBlock] = []
    for name in args.logs:
        path = Path(name)
        try:
            # universal newlines: ctest on Windows writes CRLF, and no case name may end in \r
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            print(f"ctest_case_times: cannot read {name}: {error.strerror or error}", file=sys.stderr)
            return 2
        tests += parse_log(text)
    for line in report(tests, args.min):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
