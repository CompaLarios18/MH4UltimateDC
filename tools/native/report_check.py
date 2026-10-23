#!/usr/bin/env python3
"""The gate for a mh4u-native tester report (N3-13, D69): refuse a report that holds anything private
or of the game.

Usage:
    report_check.py REPORT [--name NAME]...

Prints `ok` and exits 0 when the report passes. Otherwise it prints one line per rule that failed,
with line numbers only (never the text of a line, which is what the rules are about) and exits 1.
Exit status 2: the file cannot be read, or a bad command line. `--name` gives a name that must not
appear (the tester's own user names); it can be repeated.

Rules (each failing rule prints its name and the lines):
    marker      the first line is not `mh4u-native alpha report v1`
    lines       more than 650 lines
    binary      a NUL byte, or text that is not UTF-8
    path        an absolute path: a drive path (C:\\ or C:/), a UNC path, or a POSIX path of two or
                more components (not the address of a URL)
    name        a name given with --name (case-insensitive; a name of three letters or more also
                inside a longer word; the <tokens> a redaction leaves are skipped)
    console-id  `console id` followed by 16 hex digits
    mac         a MAC address (six pairs of hex digits with `:` or `-`)
    address     an IPv4 address (four numbers 0 to 255) or an IPv6 address (eight groups, or fewer
                around one `::`); times such as 12:34:56 and names such as mh4n::app::Main are not
    invite      an mh4u:// invite
    setting     a line `password`, `token`, `join`, `lobby`, `last_room` or `nickname` set to a value
    hex         64 or more hex digits in a row (a hash, a key, a dump)
    trace       more than check_no_game_data.py's TRACE_LINE_LIMIT lines that match its trace patterns
    listing     a line that matches its hex-dump, disassembly or byte-array patterns
    dense       a line of check_no_game_data.py's DENSE_LINE_MIN or more base64 or hex characters

The path, address, MAC, console-id and invite rules are the ones native/app/report/redact.cpp
replaces; tests/test_native_report_check.py and native/app/tests/app_report_test.cpp hold the same
lines. The trace, listing and dense rules are imported from check_no_game_data.py.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Callable, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_no_game_data as guard  # noqa: E402

MARKER = "mh4u-native alpha report v1"
MAX_LINES = 650
HEX_RUN = 64
SHOWN_LINES = 12  # line numbers printed per rule

WORD = re.compile(r"[A-Za-z0-9_]")

# ---- paths ----------------------------------------------------------------------------------------------------

def _word(c: str) -> bool:
    return c.isascii() and (c.isalnum() or c == "_")


def _sep(c: str) -> bool:
    return c == "/" or c == "\\"


def _space(c: str) -> bool:
    return c == " " or "\t" <= c <= "\r"


def _component(c: str) -> bool:
    return not _space(c) and c not in "/\\\"'<>|*?,;)]}"


def _path_starts_at(s: str, i: int) -> bool:
    """Does an absolute path start at s[i]: a drive path (C:\\ or C:/), a UNC path, or a POSIX path of two
    or more components? A mirror of PathAt in native/app/report/redact.cpp."""
    n = len(s)
    c = s[i]
    p = s[i - 1] if i > 0 else ""
    if c.isascii() and c.isalpha() and i + 2 < n and s[i + 1] == ":" and _sep(s[i + 2]) and (i == 0 or not _word(p)):
        return True
    if (_sep(c) and i + 1 < n and _sep(s[i + 1]) and (i == 0 or not (_word(p) or _sep(p)))
            and not (p == ":" and c == "/" and s[i + 1] == "/")):
        j = i + 2
        while j < n and not _space(s[j]) and not _sep(s[j]):
            j += 1
        return j > i + 2 and j < n and _sep(s[j])
    if c == "/":
        if i == 0:
            start = True
        elif p == ":":
            start = not (i + 1 < n and s[i + 1] == "/" and not (i + 2 < n and s[i + 2] == "/"))
        else:
            start = not (_word(p) or p in ".~" or _sep(p))
        if start:
            j = i + 1
            while j < n and s[j] == "/":
                j += 1
            first = j
            while j < n and _component(s[j]):
                j += 1
            if j > first:
                k = j
                while k < n and s[k] == "/":
                    k += 1
                return k > j and k < n and _component(s[k])
    return False


def has_path(line: str) -> bool:
    return any(_path_starts_at(line, i) for i in range(len(line)))


# ---- console ids, MACs, invites ---------------------------------------------------------------------------

CONSOLE_ID = re.compile(r"(?<![A-Za-z0-9_])console id\s*[:=]?\s*(?:0x)?[0-9a-f]{16}(?![0-9a-f])", re.IGNORECASE)
MAC = re.compile(
    r"(?<![0-9A-Za-z])(?<![0-9A-Fa-f][:\-])[0-9A-Fa-f]{2}([:\-])(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}"
    r"(?![0-9A-Fa-f])(?![:\-][0-9A-Fa-f])"
)
INVITE = re.compile(r"mh4u://", re.IGNORECASE)
SETTING = re.compile(r"^\s*(password|token|join|lobby|last_room|nickname)\s*=\s*\S", re.IGNORECASE)
HEX_RUN_RE = re.compile(r"[0-9A-Fa-f]{%d}" % HEX_RUN)


# ---- IPv4 -------------------------------------------------------------------------------------------------------

def _ipv4_at(s: str, i: int) -> bool:
    """Does an IPv4 address (four numbers 0..255, 1 to 3 digits each) start at s[i]? A mirror of
    Ipv4At in native/app/report/redact.cpp."""
    n = len(s)
    if not s[i].isascii() or not s[i].isdigit():
        return False
    if i > 0:
        p = s[i - 1]
        if (p.isascii() and p.isdigit()) or (p == "." and i >= 2 and s[i - 2].isascii() and s[i - 2].isdigit()):
            return False
    j = i
    while j < n and ((s[j].isascii() and s[j].isdigit()) or s[j] == "."):
        j += 1
    while j > i and s[j - 1] == ".":
        j -= 1
    parts = 0
    k = i
    while k < j:
        start = k
        value = 0
        while k < j and s[k].isascii() and s[k].isdigit():
            value = value * 10 + int(s[k])
            k += 1
            if k - start > 3:
                return False
        if k == start or value > 255:
            return False
        parts += 1
        if k < j:
            if s[k] != ".":
                return False
            k += 1
    return parts == 4


def has_ipv4(line: str) -> bool:
    return any(_ipv4_at(line, i) for i in range(len(line)))


# ---- IPv6 -------------------------------------------------------------------------------------------------------

HEX_COLON_RUN = re.compile(r"[0-9A-Fa-f:]+")
_HEX = set("0123456789abcdefABCDEF")


def _is_ipv6(s: str, begin: int, end: int) -> bool:
    """Is s[begin:end] an IPv6 address: groups of 1 to 4 hex digits separated by ':', eight of them, or
    fewer around one '::'; at least one group holds a digit. A mirror of IsIpv6 in redact.cpp."""
    if end - begin < 2:
        return False
    groups = 0
    compressions = 0
    digit = False
    i = begin
    if s[i] == ":":
        if s[i + 1] != ":":
            return False
        compressions += 1
        i += 2
    while i < end:
        start = i
        while i < end and s[i] in _HEX:
            digit = digit or s[i].isdigit()
            i += 1
        length = i - start
        if length == 0 or length > 4:
            return False
        groups += 1
        if i == end:
            break
        # s[i] is ':'
        if i + 1 < end and s[i + 1] == ":":
            compressions += 1
            i += 2
            if i == end:
                break  # "1::"
        else:
            i += 1
            if i == end:
                return False  # a lone colon at the end
    if compressions > 1 or not digit:
        return False
    return groups <= 7 if compressions == 1 else groups == 8


def has_ipv6(line: str) -> bool:
    n = len(line)
    for run in HEX_COLON_RUN.finditer(line):
        i, run_end = run.span()
        begin, end = i, run_end
        # a single colon at either edge is punctuation ("addr:2001:db8::1", "2001:db8::1:")
        if end - begin >= 2 and line[begin] == ":" and line[begin + 1] != ":":
            begin += 1
        if end - begin >= 2 and line[end - 1] == ":" and line[end - 2] != ":":
            end -= 1
        word_before = begin == i and i > 0 and WORD.match(line[i - 1]) is not None
        word_after = end == run_end and run_end < n and WORD.match(line[run_end]) is not None
        if not word_before and not word_after and _is_ipv6(line, begin, end):
            return True
    return False


def has_address(line: str) -> bool:
    return has_ipv4(line) or has_ipv6(line)


# ---- names ------------------------------------------------------------------------------------------------------

TOKEN = re.compile(r"<[A-Za-z-]{1,15}>")


def has_name(line: str, name: str) -> bool:
    """Is the name in the line: without case, a name of three characters or more also inside a longer
    word, a shorter one only as a word of its own. The <tokens> that redaction leaves are skipped."""
    name = name.strip().lower()
    if not name:
        return False
    text = TOKEN.sub(" ", line).lower()
    start = 0
    while True:
        at = text.find(name, start)
        if at < 0:
            return False
        if len(name) >= 3:
            return True
        end = at + len(name)
        before = at == 0 or WORD.match(text[at - 1]) is None
        after = end == len(text) or WORD.match(text[end]) is None
        if before and after:
            return True
        start = at + 1


# ---- the report -------------------------------------------------------------------------------------------------

def _lines_where(lines: list[str], test: Callable[[str], bool]) -> list[int]:
    return [number for number, line in enumerate(lines, 1) if test(line)]


def check_text(data: bytes, names: Iterable[str] = ()) -> list[tuple[str, list[int] | str]]:
    """The problems of a report: (rule, line numbers) pairs, or (rule, a note) for a rule about the
    whole report. Empty when the report passes."""
    problems: list[tuple[str, list[int] | str]] = []
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = data.decode("utf-8", "replace")
        problems.append(("binary", "not UTF-8 text"))
    if b"\0" in data:
        problems.append(("binary", "a NUL byte"))
    lines = text.splitlines()

    if not lines or lines[0] != MARKER:
        problems.append(("marker", "the first line is not the marker"))
    if len(lines) > MAX_LINES:
        problems.append(("lines", f"{len(lines)} lines, limit {MAX_LINES}"))

    rules: list[tuple[str, Callable[[str], bool]]] = [
        ("path", has_path),
        ("console-id", lambda line: CONSOLE_ID.search(line) is not None),
        ("mac", lambda line: MAC.search(line) is not None),
        ("address", has_address),
        ("invite", lambda line: INVITE.search(line) is not None),
        ("setting", lambda line: SETTING.match(line) is not None),
        ("hex", lambda line: HEX_RUN_RE.search(line) is not None),
        ("listing", lambda line: any(p.search(line) for p in guard.LISTING_PATTERNS)),
        (
            "dense",
            lambda line: len(line) >= guard.DENSE_LINE_MIN and guard.DENSE_LINE.search(line) is not None,
        ),
    ]
    for rule, test in rules:
        found = _lines_where(lines, test)
        if found:
            problems.append((rule, found))
    for name in names:
        if name.strip():
            found = _lines_where(lines, lambda line, n=name: has_name(line, n))
            if found:
                problems.append(("name", found))
    traces = _lines_where(lines, lambda line: any(p.search(line) for p in guard.TRACE_PATTERNS))
    if len(traces) > guard.TRACE_LINE_LIMIT:
        problems.append(("trace", f"{len(traces)} trace-like lines, limit {guard.TRACE_LINE_LIMIT}"))
    return problems


def describe(problem: tuple[str, list[int] | str]) -> str:
    rule, where = problem
    if isinstance(where, str):
        return f"{rule}: {where}"
    shown = ", ".join(str(n) for n in where[:SHOWN_LINES])
    more = f" and {len(where) - SHOWN_LINES} more" if len(where) > SHOWN_LINES else ""
    return f"{rule}: line {shown}{more}" if len(where) == 1 else f"{rule}: lines {shown}{more}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check a mh4u-native tester report (D69).")
    parser.add_argument("report", help="the report file")
    parser.add_argument("--name", action="append", default=[], metavar="NAME",
                        help="a name that must not appear (repeatable)")
    args = parser.parse_args(argv)
    try:
        data = Path(args.report).read_bytes()
    except OSError as error:
        print(f"report_check: cannot read the report: {error.strerror or error}", file=sys.stderr)
        return 2
    problems = check_text(data, args.name)
    if not problems:
        print("ok")
        return 0
    for problem in problems:
        print(describe(problem))
    print(f"refused: {len(problems)} rule{'s' if len(problems) != 1 else ''} broken")
    return 1


if __name__ == "__main__":
    sys.exit(main())
