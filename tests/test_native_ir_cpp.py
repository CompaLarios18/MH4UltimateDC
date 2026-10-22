# Derived from Azahar src/core/hle/service/ir/extra_hid.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
# (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the calibration data and the
# calibration answers, restated from its code.
# Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project
"""Our own ir:USER test program (port/tests/ir_cpp.c, N3-08) and its expected output, which is the
reference runner's (Azahar's ExtraHID, the emulated Circle Pad Pro): see docs/native/circle-pad-pro.md.
No game code, no game data.

- Always: the program builds, recompiles and links into a code library (paged and native); the expected
  file is well formed and says what Azahar's code makes (the calibration answers, 50 status packets of a
  centred stick with nothing held, the packets stop at the disconnect).
- With MH4U_RUNNER (the reference runner, `--cpu jit --new-3ds off`): the output equals the expected
  file, twice in a row.
- With MH4N_RUNNER (the native runner): the output equals the expected file. Required from N3-12 on
  (until the native ir:USER connects the Circle Pad Pro the native run fails).
- MH4N_IR_REQUIRE is `reference`, `native` or `both` (comma-separated tokens are fine): a runner it names
  that is not set is a failure (otherwise the test is skipped).

To rewrite the expected file from the reference runner (it must come out the same twice):
    MH4U_RUNNER=path/to/run.sh python3 tests/test_native_ir_cpp.py --regenerate
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_conformance as tc  # noqa: E402  (the helpers: building, running, reading the output)

PROGRAM = "ir_cpp"
EXPECTED_DIR = os.path.join(tc.EXPECTED, "ir")
EXPECTED_FILE = os.path.join(EXPECTED_DIR, PROGRAM + ".txt")
KNOWN_REQUIRE = {"reference", "native", "both"}
STATUSES = 50

# Azahar's calibration data (extra_hid.cpp, "retrieved from a New 3DS"), restated: the first answer is
# [0x11, offset u16, size u16, the 0x40 bytes], the second the unaligned read of offset 0x13, size 0x1F
# (echoed as written, the data read at 0x10 for 0x10 bytes).
CALIBRATION = bytes([
    0x00, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0xF5,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
])
CALIBRATION_ANSWER = bytes([0x11, 0x00, 0x00, 0x40, 0x00]) + CALIBRATION
CALIBRATION_UNALIGNED = bytes([0x11, 0x13, 0x00, 0x1F, 0x00]) + CALIBRATION[0x10:0x20]
# The status packet with no input: header 0x10, the stick x = y = 0x800 (centred, not zero), the battery
# level 0x1F with ZL, ZR and R "not held" bits set, the unknown byte 0.
CENTRED_STATUS = bytes([0x10, 0x00, 0x08, 0x80, 0xFF, 0x00])
NOT_CONNECTED = "c8a10c0d"   # SendIrNop when not connected: description 13, IR, InvalidState, Status

RC_LINE = re.compile(r"ir_cpp: ([a-z0-9_]+): ([0-9a-f]{8})")
PKT_LINE = re.compile(r"ir_cpp: pkt (\d\d) ([0-9a-f]{12})")


def required() -> set[str]:
    tokens = {t.strip() for t in os.environ.get("MH4N_IR_REQUIRE", "").split(",") if t.strip()}
    return {"reference", "native"} if "both" in tokens else tokens


def runner_or_skip(variable: str, name: str) -> str:
    """The runner's path; a skip when it is not set, a failure when MH4N_IR_REQUIRE names it."""
    path = os.environ.get(variable)
    if path:
        return path
    if name in required():
        pytest.fail(f"MH4N_IR_REQUIRE names {name}, but {variable} is not set")
    pytest.skip(f"{variable} not set")


def expected_lines() -> list[str]:
    with open(EXPECTED_FILE, encoding="utf-8") as f:
        return f.read().splitlines()


def values(lines: list[str]) -> dict[str, str]:
    return {m.group(1): m.group(2) for line in lines if (m := RC_LINE.fullmatch(line))}


def problems(lines: list[str]) -> list[str]:
    """Everything that is wrong with the program's output, as a list of messages (empty: it says what
    Azahar's code makes)."""
    found: list[str] = []
    if not lines or lines[0] != "ir_cpp: start":
        found.append("the first line is not 'ir_cpp: start'")
    if not lines or lines[-1] != "ir_cpp: done":
        found.append("the last line is not 'ir_cpp: done'")
    for line in lines:
        if "FAIL" in line or "OutputDebugString" in line:
            found.append(f"failure line: {line!r}")
    packets = [PKT_LINE.fullmatch(line) for line in lines if line.startswith("ir_cpp: pkt ")]
    if None in packets or [int(m.group(1)) for m in packets if m] != list(range(1, STATUSES + 1)):
        found.append(f"the status packets are not {STATUSES} lines numbered 1 to {STATUSES}")
    for m in packets:
        if m and bytes.fromhex(m.group(2)) != CENTRED_STATUS:
            found.append(f"packet {m.group(1)} is not the centred stick with nothing held: {m.group(2)}")
    answers = [line.split(" ")[-1] for line in lines if line.startswith("ir_cpp: calibration")
               and not line.startswith("ir_cpp: calibration_answers")]
    if answers != [CALIBRATION_ANSWER.hex(), CALIBRATION_UNALIGNED.hex()]:
        found.append("the calibration answers are not Azahar's")
    v = values(lines)
    expect = {"require_unknown_device": "00000000", "require_rc": "00000000", "connection_event": "00000000",
              "header_status": "00020202", "header_connected": "00000101",
              "send_polling_rc": "00000000", "send_calibration_rc": "00000000", "send_unaligned_rc": "00000000",
              "send_past_the_end_rc": "00000000", "send_wrong_size_rc": "00000000", "send_unknown_rc": "00000000",
              "statuses": f"{STATUSES:08x}", "calibration_answers": "00000002", "period_within_5_percent": "00000001",
              "disconnect_rc": "00000000", "quiet_after_disconnect": "00000001",
              "send_after_disconnect_rc": NOT_CONNECTED, "finalize_rc": "00000000"}
    for key, want in expect.items():
        if v.get(key) != want:
            found.append(f"{key}: {v.get(key)} (expected {want})")
    return found


# ── synthetic output: the checks above on lines made here ──────────────────────────────

def synthetic() -> list[str]:
    lines = ["ir_cpp: start"]
    for key in ("require_unknown_device", "require_rc", "connection_event", "header_status", "header_connected",
                "send_polling_rc", "send_calibration_rc", "send_unaligned_rc", "send_past_the_end_rc",
                "send_wrong_size_rc", "send_unknown_rc"):
        lines.append(f"ir_cpp: {key}: " + {"header_status": "00020202", "header_connected": "00000101"}.get(key, "00000000"))
    lines.append("ir_cpp: calibration " + CALIBRATION_ANSWER.hex())
    lines.append("ir_cpp: calibration_unaligned " + CALIBRATION_UNALIGNED.hex())
    lines += [f"ir_cpp: pkt {i:02d} {CENTRED_STATUS.hex()}" for i in range(1, STATUSES + 1)]
    lines += [f"ir_cpp: statuses: {STATUSES:08x}", "ir_cpp: calibration_answers: 00000002",
              "ir_cpp: period_within_5_percent: 00000001", "ir_cpp: disconnect_rc: 00000000",
              "ir_cpp: quiet_after_disconnect: 00000001", f"ir_cpp: send_after_disconnect_rc: {NOT_CONNECTED}",
              "ir_cpp: finalize_rc: 00000000", "ir_cpp: done"]
    return lines


def test_well_formed_output_has_no_problems():
    assert problems(synthetic()) == []


@pytest.mark.parametrize("damage, expect", [
    (lambda l: [x for x in l if "pkt 17 " not in x], "not 50 lines"),
    (lambda l: [x.replace("10000880ff00", "10000880df00") if "pkt 30 " in x else x for x in l], "packet 30"),
    (lambda l: [x.replace("10000880ff00", "10ff0f80ff00") if "pkt 01 " in x else x for x in l], "packet 01"),
    (lambda l: [x for x in l if not x.startswith("ir_cpp: calibration_unaligned")], "calibration answers"),
    (lambda l: [x.replace("1113001f00", "1110001000") for x in l], "calibration answers"),
    (lambda l: l[:-1] + ["ir_cpp: FAIL receive event 00000003", l[-1]], "failure line"),
    (lambda l: [x.replace("quiet_after_disconnect: 00000001", "quiet_after_disconnect: 00000000") for x in l], "quiet_after"),
    (lambda l: [x.replace("period_within_5_percent: 00000001", "period_within_5_percent: 00000000") for x in l], "period_within"),
    (lambda l: l[:-1], "last line"),
    (lambda l: [x.replace("header_status: 00020202", "header_status: 00010101") for x in l], "header_status"),
])
def test_malformed_output_is_found(damage, expect):
    found = problems(damage(synthetic()))
    assert any(expect in p for p in found), found


def test_require_names_are_known():
    assert required() <= KNOWN_REQUIRE | {"reference", "native"}, required()


# ── the files ──────────────────────────────────────────────────────────────────────

def test_program_is_short_and_known_and_not_a_conformance_program():
    import build_svc_test
    with open(os.path.join(tc.PORT_TESTS, PROGRAM + ".c"), encoding="utf-8") as f:
        assert len(f.read().splitlines()) < tc.MAX_SOURCE_LINES
    assert PROGRAM in build_svc_test.programs()
    assert PROGRAM not in tc.PROGRAMS       # never in test_conformance.py's PROGRAMS
    assert not PROGRAM.startswith("conf_")
    assert sorted(os.listdir(EXPECTED_DIR)) == [PROGRAM + ".txt"]


def test_expected_file_is_wellformed():
    assert problems(expected_lines()) == []


# ── building and running ───────────────────────────────────────────────────────────

pytestmark_toolchain = pytest.mark.skipif(not tc.have_toolchain(), reason="clang/lld toolchain not installed")


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    cache: dict[bool, tuple[str, str]] = {}

    def get(native: bool = False) -> tuple[str, str]:
        if native not in cache:
            # the paged and the native library share a directory (one recompilation)
            out = (os.path.dirname(cache[not native][0]) if (not native) in cache
                   else str(tmp_path_factory.mktemp(PROGRAM)))
            cache[native] = tc.build_program(out, PROGRAM, native=native)
        return cache[native]
    return get


@pytestmark_toolchain
def test_program_builds_and_recompiles(built):
    elf, lib = built()
    assert os.path.exists(elf) and os.path.exists(lib)
    assert os.path.exists(built(native=True)[1])


@pytestmark_toolchain
def test_reference_runner_matches_expected_twice(built, tmp_path):
    runner = runner_or_skip("MH4U_RUNNER", "reference")
    elf, lib = built()
    first = tc.run_reference(runner, elf, lib, str(tmp_path / "user1"))
    second = tc.run_reference(runner, elf, lib, str(tmp_path / "user2"))
    assert first == expected_lines()
    assert second == first


@pytestmark_toolchain
def test_native_runner_matches_expected(built, tmp_path):
    runner = runner_or_skip("MH4N_RUNNER", "native")
    elf, lib = built(native=True)
    r = subprocess.run([runner, elf, "--code", lib, "--ms", str(tc.RUN_MS), "--user", str(tmp_path / "user")],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
    got = tc.debug_lines(r.stdout)
    assert r.returncode == 0, (r.returncode, r.stdout[-2000:])
    assert got == expected_lines()


# ── regenerating the expected file ─────────────────────────────────────────────────

def regenerate() -> int:
    runner = os.environ.get("MH4U_RUNNER")
    if not runner:
        print("set MH4U_RUNNER to the reference runner", file=sys.stderr)
        return 2
    os.makedirs(EXPECTED_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        elf, lib = tc.build_program(os.path.join(tmp, "build"), PROGRAM)
        runs = [tc.run_reference(runner, elf, lib, os.path.join(tmp, f"user{i}")) for i in range(2)]
    bad = problems(runs[0])
    if runs[0] != runs[1] or bad:
        print(f"{PROGRAM}: {'unstable output' if runs[0] != runs[1] else bad[:3]}; not written", file=sys.stderr)
        return 1
    with open(EXPECTED_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(runs[0]) + "\n")
    print(f"{PROGRAM}: {len(runs[0])} lines")
    return 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["--regenerate"]:
        sys.exit(regenerate())
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
