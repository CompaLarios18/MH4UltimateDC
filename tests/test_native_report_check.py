"""Tests for tools/native/report_check.py (N3-13, D69) and the files that go with it: one synthetic line
for each rule, both ways; the checker's copy of the guard's patterns in the report module's C++; the
native-alpha issue template. Synthetic data only: documentation-range addresses (192.0.2.x, 2001:db8::),
made-up MAC addresses and names."""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "native" / "report_check.py"
TEMPLATE = ROOT / ".github" / "ISSUE_TEMPLATE" / "native-alpha.yml"
PATTERNS_H = ROOT / "native" / "app" / "report" / "patterns.h"
sys.path.insert(0, str(ROOT / "tools" / "native"))
import check_no_game_data as guard  # noqa: E402
import report_check as rc  # noqa: E402

MARKER = rc.MARKER


def report(*lines: str) -> bytes:
    return ("\n".join((MARKER, *lines)) + "\n").encode()


def rules(data: bytes, names: tuple[str, ...] = ()) -> list[str]:
    return [rule for rule, _ in rc.check_text(data, names)]


def run_tool(path: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), str(path), *args], capture_output=True, text=True)


# ---- the marker and the whole-report rules -------------------------------------------------------------

def test_a_clean_report_passes_and_prints_ok(tmp_path: Path) -> None:
    path = tmp_path / "report-1.txt"
    path.write_bytes(report("== versions ==", "app commit: 3e9b152a10", "[Info] Kernel: thread 4 started"))
    done = run_tool(path)
    assert done.returncode == 0
    assert done.stdout == "ok\n"
    assert rc.check_text(path.read_bytes()) == []


@pytest.mark.parametrize("first", ["", "mh4u-native alpha report v2", "  mh4u-native alpha report v1",
                                   "mh4u-native alpha report v1 ", "something else"])
def test_the_first_line_must_be_the_marker(first: str) -> None:
    data = (first + "\nbody\n").encode()
    assert rules(data) == ["marker"]


def test_an_empty_file_has_no_marker() -> None:
    assert rules(b"") == ["marker"]


def test_the_line_limit() -> None:
    at_limit = report(*["[Info] x"] * (rc.MAX_LINES - 1))
    assert len(at_limit.decode().splitlines()) == 650
    assert rules(at_limit) == []
    over = report(*["[Info] x"] * rc.MAX_LINES)
    assert len(over.decode().splitlines()) == 651
    assert rules(over) == ["lines"]


def test_binary_data_is_refused() -> None:
    assert rules(report("a NUL byte \0 inside")) == ["binary"]
    assert rules(MARKER.encode() + b"\n\xff\xfe not utf-8\n") == ["binary"]
    assert rules(report("caf\u00e9 and \u30e2\u30f3\u30cf\u30f3")) == []  # UTF-8 text is text


# ---- absolute paths ----------------------------------------------------------------------------------------

@pytest.mark.parametrize("line", [
    r"opened C:\Users\Fake Person\Games\game.cxi",
    "opened D:/fake/dir/file.bin",
    r"(E:\a\b.txt)",
    r"share \\fake-server\share\file.txt",
    "share //fake-server/share/file.txt",
    "loaded /opt/fake/lib/libfake.so.1",
    "HOME=/var/fake/x",
    '"/usr/lib/fake.so"',
    "/Users/fake/Library/Application Support/Fake/x",
    "file:///opt/fake/x.cxi",
    "frame 12:/opt/fake/x",
    "/tmp//opt/fake/lib.so",
    r"path:\\fake-server\share\f",
    "see ->/opt/fake/x",
    "--user=/opt/fake/x",
])
def test_absolute_paths_are_refused(line: str) -> None:
    assert rules(report(line)) == ["path"]


@pytest.mark.parametrize("line", [
    "/tmp alone",
    "a/b/c and 1/2/3 and n/a",
    "https://example.invalid/x/y",
    "relative/path/to/file.txt",
    "ratio 16/9 and 3440x1440@120",
    "opened <path> and <user> and <home>",
    "mh4n::app::Main dsp::DSP std::vector",
    "[00:00:12.345] 12:34:56 16:9",
])
def test_what_is_no_absolute_path_passes(line: str) -> None:
    assert rules(report(line)) == []


# ---- names -------------------------------------------------------------------------------------------------

def test_a_given_name_is_refused_without_case() -> None:
    assert rules(report("hello fakeuser!"), ("fakeuser",)) == ["name"]
    assert rules(report("hello FAKEUSER!"), ("fakeuser",)) == ["name"]
    assert rules(report("hello Fake Console!"), ("fake console",)) == ["name"]
    assert rules(report("hello fakeuser!")) == []  # no name given
    assert rules(report("hello world"), ("fakeuser",)) == []
    assert rules(report("hello fakeuser!"), ("",)) == []


def test_a_name_of_three_letters_or_more_counts_inside_a_longer_word() -> None:
    assert rules(report("as fakeuser123"), ("fakeuser",)) == ["name"]
    assert rules(report("as Alice"), ("al",)) == []  # a short name only as a word of its own
    assert rules(report("as al"), ("al",)) == ["name"]
    assert rules(report("as al-b"), ("al",)) == ["name"]


def test_the_tokens_of_a_redaction_are_not_names() -> None:
    assert rules(report("<path> and <user> and <os-user>"), ("path", "user", "os-user")) == []
    assert rules(report("<path> and path"), ("path",)) == ["name"]


def test_names_from_the_command_line(tmp_path: Path) -> None:
    path = tmp_path / "r.txt"
    path.write_bytes(report("hello fakeuser", "hello someone else"))
    done = run_tool(path, "--name", "fakeuser", "--name", "someone")
    assert done.returncode == 1
    assert "name: line 2\nname: line 3\n" in done.stdout  # one entry per name
    assert "fakeuser" not in done.stdout and "someone" not in done.stdout  # line numbers only


# ---- console ids, MACs, invites, settings, hex ------------------------------------------------------------

@pytest.mark.parametrize("line", [
    "new console id 0123456789abcdef.",
    "Console ID: 0x0123456789ABCDEF",
    "console id=0123456789abcdef",
    "console id0123456789abcdef",
])
def test_console_ids_are_refused(line: str) -> None:
    assert "console-id" in rules(report(line))


@pytest.mark.parametrize("line", [
    "console id <console-id>",
    "console id 0123456789abcde",
    "console id 0123456789abcdef0",
    "the console is fine",
])
def test_what_is_no_console_id_passes(line: str) -> None:
    assert "console-id" not in rules(report(line))


@pytest.mark.parametrize("line", [
    "mac 18:2A:7B:00:00:01 ok",
    "mac aa-bb-cc-dd-ee-ff.",
    "[18:2A:7B:00:00:01]",
])
def test_mac_addresses_are_refused(line: str) -> None:
    assert rules(report(line)) == ["mac"]


@pytest.mark.parametrize("line", [
    "mac <mac>",
    "18:2A:7B:00:00:01:02",  # seven pairs
    "18:2A:7B:00:00",        # five
    "18:2A-7B:00-00:01:02",  # mixed separators
    "x18:2A:7B:00:00:01",
])
def test_what_is_no_mac_address_passes(line: str) -> None:
    assert "mac" not in rules(report(line))


def test_invites_are_refused() -> None:
    assert rules(report("join mh4u://192.0.2.7:24872?password=fakepass")) == ["address", "invite"]  # the host in it is an address too
    assert rules(report("(MH4U://example.invalid:1)")) == ["invite"]
    assert rules(report("<invite>")) == []


@pytest.mark.parametrize("key", ["password", "token", "join", "lobby", "last_room", "nickname", "Password", "NICKNAME"])
def test_secret_settings_are_refused(key: str) -> None:
    assert rules(report(f"{key} = something")) == ["setting"]
    assert rules(report(f"  {key}=x")) == ["setting"]


@pytest.mark.parametrize("line", [
    "password =",            # no value
    "password",              # no assignment
    "passwords = x",         # another key
    "mypassword = x",
    "[Multiplayer]",
    "fps = 60",
])
def test_what_is_no_secret_setting_passes(line: str) -> None:
    assert rules(report(line)) == []


def test_64_hex_digits_in_a_row() -> None:
    assert rules(report("hash " + "ab" * 32)) == ["hex"]
    assert rules(report("hash " + "ab" * 31 + "a")) == []  # 63
    assert rules(report("hash " + "A" * 70)) == ["hex"]
    assert rules(report("hash " + "ab" * 16 + " " + "cd" * 16)) == []  # broken up


# ---- addresses ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("line", [
    "peer 192.0.2.7:24872 ok",
    "peer 192.0.2.1.",
    "a=198.51.100.255,b=203.0.113.0",
    "ip 0.0.0.0 and 255.255.255.255",
    "peer 2001:db8::1 ok",
    "peer [2001:db8::1]:8080 ok",
    "full 2001:0db8:0000:0000:0000:0000:0000:0001.",
    "loopback ::1, link fe80::1ff:fe23:4567:890a;",
    "addr:2001:db8::7",
    "trailing 2001:db8:: and",
    "1:2:3:4:5:6:7:8",
    "ends 2001:db8::",
])
def test_addresses_are_refused(line: str) -> None:
    assert rules(report(line)) == ["address"]


@pytest.mark.parametrize("line", [
    "version 1.2.3",
    "300.1.1.1",
    "1.2.3.4.5",
    "1.2.3.1234",
    "SDL 3.4.18 on Vulkan 1.3.280",
    "mh4n::app::Main",
    "dsp::DSP",
    "std::vector<int> :: a::b",
    "12:34:56",
    "[00:00:12] 16:9 3440:1440",
    "1:2:3:4:5:6:7",
    "1:2:3:4:5:6:7:8:9",
    "2001:db8:::1",
    "12345::1",
    "::::",
    "frame: 12 scene: ee",
    "dead::beef",       # no digit in any group
    "<address>",
])
def test_what_is_no_address_passes(line: str) -> None:
    assert rules(report(line)) == []


# ---- the guard's rules: traces, listings, dense lines -----------------------------------------------------

def test_trace_lines_up_to_the_guards_limit() -> None:
    limit = guard.TRACE_LINE_LIMIT
    assert limit == 50
    lines = [f"[Warning] ipc: cmd=0x{10000 + i:08x} not implemented" for i in range(limit)]
    assert rules(report(*lines)) == []
    assert rules(report(*lines, "[Warning] ipc: cmd=0x00000001 x")) == ["trace"]
    other = ["svc=0x32 something"] * limit + ["f=1 t=2"]
    assert rules(report(*other)) == ["trace"]  # all the guard's trace patterns count together


@pytest.mark.parametrize("line", [
    "00000010  41 42 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  |ABCDEFGHIJKLMNOP|",
    "0010: 4142 4344 4546 4748 494a",
    "00100000 e1a00000 mov r0, r0",
    "int a[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};",
])
def test_a_single_listing_line_is_refused(line: str) -> None:
    assert rules(report(line)) == ["listing"]


def test_ordinary_lines_are_no_listing() -> None:
    assert rules(report("[Info] Kernel: thread 4 started", "added 1 of 2 files", "dead beef")) == []


def test_dense_lines() -> None:
    assert guard.DENSE_LINE_MIN == 1024
    assert rules(report("[Info] " + "Z" * 1100)) == ["dense"]
    assert rules(report("[Info] " + "Z" * 1000)) == []
    assert rules(report("[Info] " + "Z" * 1100 + " " + "Y" * 10)) == ["dense"]
    assert rules(report("Z" * 500 + " " + "Y" * 600)) == []
    assert rules(report("[Info] " + "A" * 1100)) == ["hex", "dense"]  # (also a run of hex digits)


# ---- the output ---------------------------------------------------------------------------------------------

def test_the_output_names_rules_and_lines_never_text(tmp_path: Path) -> None:
    path = tmp_path / "report.txt"
    secret = "C:\\Users\\Fake Person\\secret.cxi"
    path.write_bytes(report("fine", f"opened {secret}", "fine", "peer 192.0.2.7:1"))
    done = run_tool(path)
    assert done.returncode == 1
    assert "path: line 3" in done.stdout
    assert "address: line 5" in done.stdout
    assert "Fake Person" not in done.stdout and "192.0.2.7" not in done.stdout and "secret" not in done.stdout
    assert "refused: 2 rules broken" in done.stdout


def test_many_lines_are_summarised(tmp_path: Path) -> None:
    path = tmp_path / "report.txt"
    path.write_bytes(report(*["peer 192.0.2.7:1"] * 30))
    done = run_tool(path)
    assert done.returncode == 1
    assert "address: lines 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 and 18 more" in done.stdout


def test_an_unreadable_file_is_exit_2(tmp_path: Path) -> None:
    done = run_tool(tmp_path / "missing.txt")
    assert done.returncode == 2
    assert "cannot read" in done.stderr
    assert run_tool(tmp_path).returncode == 2  # a folder


def test_crlf_line_ends_are_fine(tmp_path: Path) -> None:
    path = tmp_path / "report.txt"
    path.write_bytes((MARKER + "\r\n[Info] fine\r\n").encode())
    assert run_tool(path).stdout == "ok\n"


# ---- the report module's copy of the guard's patterns --------------------------------------------------------

def _cpp_strings(array: str) -> list[str]:
    text = PATTERNS_H.read_text(encoding="utf-8")
    body = re.search(rf"{array}\[\] = \{{(.*?)\n\}};", text, re.DOTALL)
    assert body, f"{array} not found in patterns.h"
    return re.findall(r'R"MH4N\((.*?)\)MH4N"', body.group(1), re.DOTALL)


def test_patterns_h_copies_the_guards_patterns() -> None:
    assert _cpp_strings("kTracePatterns") == [p.pattern for p in guard.TRACE_PATTERNS]
    assert _cpp_strings("kListingPatterns") == [p.pattern for p in guard.LISTING_PATTERNS]


def test_patterns_h_copies_the_limits() -> None:
    text = PATTERNS_H.read_text(encoding="utf-8")

    def constant(name: str) -> int:
        found = re.search(rf"{name} = (\d+);", text)
        assert found, name
        return int(found.group(1))

    assert constant("kTraceLineLimit") == guard.TRACE_LINE_LIMIT
    assert constant("kDenseLineMin") == guard.DENSE_LINE_MIN
    assert constant("kMaxReportLines") == rc.MAX_LINES
    assert constant("kHexRunLimit") == rc.HEX_RUN
    assert MARKER in (ROOT / "native" / "app" / "report" / "report.h").read_text(encoding="utf-8")


# ---- the issue template ---------------------------------------------------------------------------------------

def parse_yaml_subset(text: str):
    """The block-style YAML the issue forms use: mappings, lists, quoted or plain scalars, `|` blocks.
    (PyYAML is not in requirements.txt, so the test carries this small parser, and compares it with
    PyYAML when that happens to be installed.)"""
    lines = text.split("\n")
    pos = 0

    def indent_of(line: str) -> int:
        return len(line) - len(line.lstrip(" "))

    def peek():
        nonlocal pos
        while pos < len(lines) and (not lines[pos].strip() or lines[pos].lstrip().startswith("#")):
            pos += 1
        return (indent_of(lines[pos]), lines[pos].strip()) if pos < len(lines) else None

    def scalar(value: str):
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            return value[1:-1]
        if value in ("true", "false"):
            return value == "true"
        assert not value.startswith(("[", "{")), f"flow collections are not in the subset: {value}"
        return value

    def block_scalar(parent: int) -> str:
        nonlocal pos
        collected: list[str] = []
        base = None
        while pos < len(lines):
            line = lines[pos]
            if line.strip() and indent_of(line) <= parent:
                break
            if line.strip() and base is None:
                base = indent_of(line)
            collected.append(line[base:] if base is not None and line.strip() else "")
            pos += 1
        return "\n".join(collected).rstrip("\n") + "\n"

    def parse(indent: int):
        item = peek()
        assert item is not None
        return parse_list(indent) if item[1].startswith("- ") or item[1] == "-" else parse_map(indent)

    def parse_map(indent: int):
        nonlocal pos
        result = {}
        while True:
            item = peek()
            if item is None or item[0] != indent or item[1].startswith("- "):
                return result
            key, _, rest = item[1].partition(":")
            assert _, f"line {pos + 1}: not a mapping line"
            rest = rest.strip()
            pos += 1
            if rest in ("|", ">"):
                result[key] = block_scalar(indent)
            elif rest:
                result[key] = scalar(rest)
            else:
                following = peek()
                if following is not None and (following[0] > indent or following[1].startswith("- ") and following[0] >= indent):
                    result[key] = parse(following[0])
                else:
                    result[key] = None

    def parse_list(indent: int):
        nonlocal pos
        result = []
        while True:
            item = peek()
            if item is None or item[0] != indent or not item[1].startswith("- "):
                return result
            content = item[1][2:]
            if re.match(r"[A-Za-z_][\w-]*:( |$)", content):
                # "- key: value": the mapping continues on the lines below at the indent after "- "
                lines[pos] = " " * (indent + 2) + content
                result.append(parse_map(indent + 2))
            else:
                pos += 1
                result.append(scalar(content))

    return parse(0)


def test_the_issue_template_parses_and_has_what_github_needs() -> None:
    form = parse_yaml_subset(TEMPLATE.read_text(encoding="utf-8"))
    assert form["name"] == "Native alpha report"
    assert form["description"]
    assert form["title"].startswith("[native alpha]")
    assert form["labels"] == ["native-alpha"]
    body = form["body"]
    assert [item["type"] for item in body] == [
        "markdown", "input", "dropdown", "dropdown", "textarea", "textarea", "textarea", "checkboxes"]
    ids = []
    for item in body:
        attributes = item["attributes"]
        if item["type"] == "markdown":
            assert "id" not in item and attributes["value"].strip()
            continue
        assert re.fullmatch(r"[A-Za-z0-9_-]+", item["id"])
        ids.append(item["id"])
        assert attributes["label"]
        if item["type"] == "dropdown":
            assert len(attributes["options"]) >= 2 and all(isinstance(o, str) for o in attributes["options"])
        if item["type"] == "checkboxes":
            assert all(o["label"] for o in attributes["options"])
    assert len(ids) == len(set(ids))
    assert {"version", "platform", "what", "steps", "report", "checks"} <= set(ids)


def test_the_issue_template_says_what_it_needs_to() -> None:
    form = parse_yaml_subset(TEMPLATE.read_text(encoding="utf-8"))
    intro = form["body"][0]["attributes"]["value"]
    assert "Read your report before you attach it" in intro
    for never in ("memory contents", "screenshots", "captures", "saves", "game files", "game text", "file paths",
                  "user name", "console ids", "MAC", "IP addresses", "room invites"):
        assert never in intro
    report_item = next(item for item in form["body"] if item.get("id") == "report")
    assert "report-<number>.txt" in report_item["attributes"]["description"]
    assert report_item["validations"]["required"] is True
    checks = next(item for item in form["body"] if item.get("id") == "checks")
    assert checks["attributes"]["options"][0]["required"] is True
    assert "read the report" in checks["attributes"]["options"][0]["label"]


def test_the_issue_template_matches_pyyaml_when_it_is_installed() -> None:
    ours = parse_yaml_subset(TEMPLATE.read_text(encoding="utf-8"))
    try:
        import yaml
    except ImportError:
        return  # requirements.txt does not list PyYAML: the parser above is the check
    assert yaml.safe_load(TEMPLATE.read_text(encoding="utf-8")) == ours
