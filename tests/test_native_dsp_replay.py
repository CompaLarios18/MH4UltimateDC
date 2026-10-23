# Derived in part from Azahar src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the byte offsets of the sources' configurations in a region, with which the tests check what the capture of Azahar's DSP holds.
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
"""N3-04's second oracle: mh4n_dsp_replay on a capture made by Azahar's own DSP code.

tools/native/dsp_oracle/replay_capture.py compiles Azahar's source, mixer and effect code (MH4N_AZAHAR_DIR, at
the pinned commit), plays the scenario of replay_cases.txt (two sources of each codec, looping and queued
buffers, a source that starts later, a game rewriting a looping buffer, the delay effect, 200 frames) and
writes an MDSP capture. The built mh4n_dsp_replay (build/native/bin, or MH4N_DSP_REPLAY) must replay it
bit-exact with the SNR line PASS, and one corrupted sample must fail and be listed. The reader of the
scenario's `read` lines is tested without either. Synthetic data only.

Needs Azahar's sources, clang++ and the built tool: without them the tests that use them skip, or fail with
MH4N_REQUIRE_TEST_DATA=1.
"""
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ORACLE = os.path.join(ROOT, "tools", "native", "dsp_oracle")
CASES = os.path.join(ORACLE, "replay_cases.txt")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module      # dataclasses look their module up
    spec.loader.exec_module(module)
    return module


replay = _load("replay_capture", os.path.join(ORACLE, "replay_capture.py"))
oracle, capfmt = replay.oracle, replay.capfmt


def _need(what):
    if os.environ.get("MH4N_REQUIRE_TEST_DATA") == "1":
        pytest.fail(f"needs {what}")
    pytest.skip(f"needs {what}")


# ---- the scenario and its reads (no Azahar needed) ----------------------------------------------------------

def test_the_read_lines_are_taken_out_before_the_oracle_reads_the_case():
    text = "case a mode=frame\nread 3 0x20000000 16\n0 master 1.0\nread 0 0x20000100 32 every\n"
    kept, reads = replay.split_reads(text)
    assert reads == [(3, 0x20000000, 16, False), (0, 0x20000100, 32, True)]
    assert "read" not in kept
    [case] = oracle.parse_cases(kept)
    assert case.mode == "frame" and sorted(e.op for e in case.events) == ["o", "r"]


@pytest.mark.parametrize("line", ["read 1 0x20000000", "read 1 0x20000000 4 always", "read 1 0x20000000 4 every x"])
def test_a_bad_read_line_is_refused(line):
    with pytest.raises(replay.ReplayError, match="read <frame>"):
        replay.split_reads("case a mode=frame\n" + line + "\n")


def test_the_scenario_is_one_frame_case_with_reads_inside_its_memory():
    with open(CASES, encoding="utf-8") as f:
        text, reads = replay.split_reads(f.read())
    [case] = oracle.parse_cases(text)
    assert case.mode == "frame" and case.frames == 200
    assert reads and all(0 <= at < case.frames for at, _, _, _ in reads)
    for _, address, size, _ in reads:
        assert any(f.address <= address and address + size <= f.address + f.size for f in case.fills), hex(address)
    # a source of each codec is started: the format field of its configuration
    formats = set()
    for e in case.events:
        if e.op == "r" and (e.offset - oracle.SHM_SOURCE_CONFIGURATIONS) % oracle.CONFIG_SIZE == oracle.OFF_FLAGS1 \
                and oracle.SHM_SOURCE_CONFIGURATIONS <= e.offset < oracle.SHM_ADPCM_COEFFICIENTS:
            formats.add((struct.unpack("<H", e.data)[0] >> 2) & 3)
    assert formats == {0, 1, 2}


# ---- Azahar's own DSP code, replayed ------------------------------------------------------------------------

@pytest.fixture(scope="module")
def tool():
    path = os.environ.get("MH4N_DSP_REPLAY") or os.path.join(ROOT, "build", "native", "bin", "mh4n_dsp_replay")
    if not os.path.isfile(path):
        _need("the built mh4n_dsp_replay (build/native/bin, or MH4N_DSP_REPLAY)")
    return path


@pytest.fixture(scope="module")
def azahar_capture(tmp_path_factory):
    azahar = os.path.expanduser(os.environ.get("MH4N_AZAHAR_DIR", ""))
    cxx = shutil.which(os.environ.get("CXX", "clang++"))
    if not azahar or not os.path.isdir(os.path.join(azahar, "src", "audio_core", "hle")) or cxx is None:
        _need("MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit) and clang++")
    out = str(tmp_path_factory.mktemp("replay") / "azahar.mdsp")
    with open(CASES, encoding="utf-8") as f:
        counts = replay.make_capture(azahar, f.read(), out, cxx)
    return out, counts


def run(tool, path, *extra):
    return subprocess.run([tool, path, *extra], capture_output=True, text=True, timeout=300)


LINE = re.compile(r"^dsp-replay: (\d+) frames judged of (\d+), (\d+) bit-exact, snr (\S+), worst window (\S+) at (\d+), "
                  r"status (equal|first difference at frame \d+), (PASS|FAIL)$")


def summary(done):
    lines = [l for l in done.stdout.splitlines() if l.startswith("dsp-replay: ") and " frames judged of " in l]
    assert len(lines) == 1 and done.stdout.splitlines()[-1] == lines[0], done.stdout
    match = LINE.match(lines[0])
    assert match, lines[0]
    return match.groups()


def test_the_capture_holds_what_the_reference_runner_writes(azahar_capture):
    path, counts = azahar_capture
    assert counts[1] == 200 and counts[2] == 200 and counts[3] > 200
    records = capfmt.read_all(path)
    for r in records:
        capfmt.check_record("dsp", r)
    assert sum(r.type == 1 for r in records) == 200 and sum(r.type == 2 for r in records) == 200
    # sample bytes only the first time a key appears
    with_bytes = [r for r in records if r.type == 3 and len(r.payload) > 12]
    without = [r for r in records if r.type == 3 and len(r.payload) == 12]
    assert 0 < len(with_bytes) < len(without)
    # a mix that is not silent, the effect and an aux bus configured, sources of each codec enabled
    pcm = [r.payload for r in records if r.type == 2]
    assert sum(1 for p in pcm if any(p)) > 150
    formats, effect, bus = set(), False, False
    for r in records:
        if r.type != 1:
            continue
        for region in (0, 1):
            base = region * 0x8000
            for s in range(24):
                config = base + oracle.SHM_SOURCE_CONFIGURATIONS + s * oracle.CONFIG_SIZE
                if r.payload[config + oracle.OFF_ENABLE]:
                    formats.add((struct.unpack_from("<H", r.payload, config + oracle.OFF_FLAGS1)[0] >> 2) & 3)
            dsp_config = base + oracle.SHM_DSP_CONFIGURATION
            effect = effect or struct.unpack_from("<H", r.payload, dsp_config + oracle.DC_DELAY_EFFECT + 2)[0] != 0
            bus = bus or struct.unpack_from("<H", r.payload, dsp_config + oracle.DC_AUX_BUS_ENABLE)[0] != 0
    assert formats == {0, 1, 2} and effect and bus


def test_azahars_dsp_replays_bit_exact_through_our_dsp(tool, azahar_capture, tmp_path):
    path, _ = azahar_capture
    report = tmp_path / "report.txt"
    done = run(tool, path, "--report", str(report))
    assert done.returncode == 0, done.stdout + done.stderr
    judged, total, exact, snr, worst, at, status, verdict = summary(done)
    assert (int(total), status, verdict, snr) == (200, "equal", "PASS", "inf")
    assert int(judged) >= 160 and int(exact) == int(judged)        # D75's share: at least 80 % judged
    assert "not bit-exact" not in done.stdout
    assert report.read_text().splitlines()[-1] == done.stdout.splitlines()[-1]


def corrupt(path, out, record_number, sample, xor):
    """Copy of the capture with one output sample of the `record_number`-th PCM record changed."""
    with open(path, "rb") as f:
        data = bytearray(f.read())
    at, seen = 8, 0
    while at < len(data):
        rtype, _, _, size = struct.unpack_from("<IIQI", data, at)
        at += 20
        if rtype == capfmt.DSP_PCM:
            if seen == record_number:
                value = struct.unpack_from("<H", data, at + 2 * sample)[0] ^ xor
                struct.pack_into("<H", data, at + 2 * sample, value)
                break
            seen += 1
        at += size
    else:
        raise AssertionError("no such record")
    with open(out, "wb") as f:
        f.write(data)


def test_one_corrupted_sample_fails_and_is_listed(tool, azahar_capture, tmp_path):
    path, _ = azahar_capture
    bad = str(tmp_path / "bad.mdsp")
    corrupt(path, bad, record_number=100, sample=50, xor=0x4000)
    done = run(tool, bad)
    assert done.returncode == 1, done.stdout
    judged, total, exact, snr, worst, at, status, verdict = summary(done)
    assert verdict == "FAIL" and status == "equal"
    assert int(exact) == int(judged) - 1
    assert float(worst) < 60.0
    listed = re.findall(r"not bit-exact: frame (\d+) \(record (\d+)\), largest difference (\d+)", done.stdout)
    assert listed == [("101", "100", "16384")] or (len(listed) == 1 and listed[0][1] == "100"), listed


def test_a_difference_of_one_is_listed_but_the_snr_still_passes(tool, azahar_capture, tmp_path):
    path, _ = azahar_capture
    bad = str(tmp_path / "off_by_one.mdsp")
    corrupt(path, bad, record_number=120, sample=7, xor=1)
    done = run(tool, bad)
    judged, total, exact, snr, worst, at, status, verdict = summary(done)
    assert int(exact) == int(judged) - 1 and float(snr) > 60 and verdict == "PASS"
    # every judged frame that is not bit-exact is listed: D75 turns a listed frame into a needs-decision
    assert len(re.findall(r"not bit-exact: frame", done.stdout)) == 1
    assert done.returncode == 0


def test_a_capture_cut_short_or_not_a_capture_exits_2(tool, azahar_capture, tmp_path):
    path, _ = azahar_capture
    with open(path, "rb") as f:
        data = f.read()
    cut = tmp_path / "cut.mdsp"
    cut.write_bytes(data[:-100])
    done = run(tool, str(cut))
    assert done.returncode == 2 and "truncated" in done.stderr
    junk = tmp_path / "junk.mdsp"
    junk.write_bytes(b"MHID\x01\x00\x00\x00")
    assert run(tool, str(junk)).returncode == 2
    assert run(tool, str(tmp_path / "nothing.mdsp")).returncode == 2
