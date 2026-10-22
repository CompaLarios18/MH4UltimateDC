# Derived in part from Azahar src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the byte offsets and bit positions of SourceConfiguration that the tests check the commands' writes against.
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
"""The oracle of the native HLE DSP sources (N3-02): tools/native/dsp_oracle/.

The case reader, the commands (the bytes they write into a source's configuration, with the
middle-endian 32-bit values and the dirty bits), the data generators and the table writer are tested on
synthetic cases and a synthetic driver answer; the last test runs the whole harness on Azahar's sources
(MH4N_AZAHAR_DIR, at the pinned commit) and checks that it writes the committed
native/tests/dsp_hle_expected.inc byte for byte, twice. That one needs Azahar's sources and clang++:
without them it skips, or fails with MH4N_REQUIRE_TEST_DATA=1.
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ORACLE = os.path.join(ROOT, "tools", "native", "dsp_oracle")
CASES = os.path.join(ROOT, "native", "tests", "dsp_hle_cases.txt")
TABLE = os.path.join(ROOT, "native", "tests", "dsp_hle_expected.inc")

spec = importlib.util.spec_from_file_location("dsp_oracle", os.path.join(ORACLE, "dsp_oracle.py"))
oracle = importlib.util.module_from_spec(spec)
sys.modules["dsp_oracle"] = oracle      # dataclasses look their module up
spec.loader.exec_module(oracle)


def events_of(text: str):
    [case] = oracle.parse_cases(text)
    return case.events


def test_the_generator_is_a_32_bit_lcg_with_16_bits_out():
    rng = oracle.Lcg(1)
    first = (1 * 1664525 + 1013904223) & 0xFFFFFFFF
    assert rng.next() == first >> 16
    assert rng.next() == ((first * 1664525 + 1013904223) & 0xFFFFFFFF) >> 16


@pytest.mark.parametrize("kind, size", [("noise", 33), ("walk16", 64), ("walk8", 40), ("adpcm", 29)])
def test_the_data_kinds_have_their_size_and_are_repeatable(kind, size):
    a = oracle.generate(kind, 7, size)
    assert len(a) == size and a == oracle.generate(kind, 7, size) and a != oracle.generate(kind, 8, size)


def test_walk16_stays_in_range_and_adpcm_has_valid_headers():
    samples = struct.unpack("<2000h", oracle.generate("walk16", 3, 4000))
    assert max(samples) <= 30000 and min(samples) >= -30000 and len(set(samples)) > 100
    data = oracle.generate("adpcm", 3, 800)
    assert all((data[i] >> 4) < 8 and (data[i] & 15) <= 8 for i in range(0, 800, 8))


def test_a_case_with_a_header_memory_and_commands():
    [case] = oracle.parse_cases(
        "# nothing\ncase c source=5 frames=10\nmem 0x20000100 walk16 seed=9 bytes=64\n0 enable 1\n3 sync 12  # a comment\n")
    assert (case.name, case.source, case.frames) == ("c", 5, 10)
    assert case.fills == [oracle.Fill(0x20000100, "walk16", 9, 64)]
    assert [(e.frame, e.op) for e in case.events] == [(0, "c"), (0, "d"), (3, "c"), (3, "d")]


def test_defaults():
    [case] = oracle.parse_cases("case c\n")
    assert (case.source, case.frames, case.fills, case.events) == (0, 200, [], [])


def test_a_sweep_is_the_product_of_its_lists_and_substitutes_in_the_lines():
    cases = oracle.parse_cases(
        "case s_${a}_${b} sweep a=none,linear b=0.5,1.0\n0 interp ${a}\n0 rate ${b}\n")
    assert [c.name for c in cases] == ["s_none_0_5", "s_none_1_0", "s_linear_0_5", "s_linear_1_0"]
    rates = [struct.unpack("<f", next(e.data for e in c.events if e.offset == oracle.OFF_RATE))[0] for c in cases]
    assert rates == [0.5, 1.0, 0.5, 1.0]
    modes = [next(e.data for e in c.events if e.offset == oracle.OFF_INTERP)[0] for c in cases]
    assert modes == [2, 2, 1, 1]


def test_enable_sync_rate_and_interp_write_their_field_and_dirty_bit():
    ev = events_of("case c\n0 enable 1\n0 sync 0x1234\n0 rate 1.5\n0 interp poly\n")
    writes = {e.offset: e.data for e in ev if e.op == "c"}
    assert writes[160] == b"\x01" and writes[162] == b"\x34\x12"
    assert writes[52] == struct.pack("<f", 1.5) and writes[56] == b"\x00"
    dirty = 0
    for e in ev:
        if e.op == "d":
            dirty |= struct.unpack("<I", e.data)[0]
    assert dirty == (1 << 16) | (1 << 28) | (1 << 18) | (1 << 17)


def test_a_32_bit_value_is_written_with_its_halves_swapped():
    assert oracle.dsp32(0x12345678) == struct.pack("<I", 0x56781234)
    ev = events_of("case c\n0 embedded addr=0x20000000 len=0x11223344 id=9\n")
    writes = {e.offset: e.data for e in ev if e.op == "c"}
    assert writes[172] == struct.pack("<I", 0x00002000) and writes[176] == struct.pack("<I", 0x33441122)
    assert writes[190] == b"\x09\x00" and writes[188] == b"\x00\x00"


def test_the_embedded_buffer_keeps_the_play_position_and_flags():
    ev = events_of("case c\n0 embedded addr=0x20000000 len=10 id=1 loop adirty pos=7 ps=0x23 yn1=5 yn2=-1\n")
    writes = {e.offset: e.data for e in ev if e.op == "c"}
    assert writes[188] == b"\x03\x00"                        # adirty | loop << 1
    assert writes[164] == oracle.dsp32(7) and writes[182] == b"\x23\x00"
    assert writes[184] == b"\x05\x00" and writes[186] == b"\xff\xff"
    dirty = [struct.unpack("<I", e.data)[0] for e in ev if e.op == "d"]
    assert dirty == [(1 << 30) | (1 << 21)]


def test_a_queued_buffer_is_written_in_its_slot_and_marks_it_dirty():
    ev = events_of("case c\n0 queue 2 addr=0x20000010 len=20 id=3 loop adirty yn1=1 yn2=2 ps=3\n")
    writes = {e.offset: e.data for e in ev if e.op == "c"}
    base = 76 + 40
    assert writes[base] == oracle.dsp32(0x20000010) and writes[base + 4] == oracle.dsp32(20)
    assert writes[base + 8] == b"\x03\x00" and writes[base + 10] == b"\x01\x00" and writes[base + 12] == b"\x02\x00"
    assert writes[base + 14] == b"\x01" and writes[base + 15] == b"\x01" and writes[base + 16] == b"\x03\x00"
    assert [(e.op, e.data) for e in ev if e.op == "b"] == [("b", b"\x04\x00")]
    assert [struct.unpack("<I", e.data)[0] for e in ev if e.op == "d"] == [1 << 19]


def test_the_commands_that_share_a_word_compose_it():
    ev = events_of("case c\n0 embedded addr=0x20000000 len=10 id=1 adirty\n"
                   "1 partial len=20 loop\n2 partial len=30\n3 format pcm8 stereo\n4 format adpcm mono\n")
    flags2 = [e.data for e in ev if e.offset == 188]
    assert flags2 == [b"\x01\x00", b"\x03\x00", b"\x01\x00"]          # the dirty bit of the ADPCM stays
    flags1 = [e.data for e in ev if e.offset == 180]
    assert flags1 == [b"\x02\x00", b"\x09\x00"]                       # stereo/PCM8, then mono/ADPCM


def test_gain_filters_and_coefficients():
    ev = events_of("case c source=3\n0 gain 1 0.5 0.25 1 0\n0 filters 1 1\n0 simple 1 -2\n0 biquad 1 2 3 4 5\n"
                   "0 coeffs 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 -15\n")
    writes = {e.offset: e.data for e in ev}
    assert writes[4 + 16] == struct.pack("<4f", 0.5, 0.25, 1, 0)
    assert writes[58] == b"\x03\x00" and writes[60] == struct.pack("<2h", 1, -2)
    assert writes[64] == struct.pack("<5h", 1, 2, 3, 4, 5)
    assert writes[32 * 3] == struct.pack("<16h", *range(15), -15)       # row 3 of the table
    assert [e.op for e in ev if e.offset == 96] == ["a"]
    dirty = 0
    for e in ev:
        if e.op == "d":
            dirty |= struct.unpack("<I", e.data)[0]
    assert dirty == (1 << 26) | (1 << 22) | (1 << 23) | (1 << 24) | (1 << 2)


def test_random_values_are_repeatable_and_in_range():
    a = oracle.values(["rand:5:100"], 16, "x")
    assert a == oracle.values(["rand:5:100"], 16, "x") and a != oracle.values(["rand:6:100"], 16, "x")
    assert all(-100 <= v <= 100 for v in a) and len(set(a)) > 4
    assert all(-32767 <= v <= 32767 for v in oracle.values(["rand:1"], 5, "x"))


def test_sleep_wakeup_poke_and_resets():
    ev = events_of("case c\n1 sleep\n2 wakeup\n3 poke 0x20000010 aabb\n4 reset\n4 partial_reset\n")
    assert [(e.frame, e.op) for e in ev] == [(1, "S"), (2, "W"), (3, "m"), (4, "d"), (4, "d")]
    assert ev[2].offset == 0x20000010 and ev[2].data == b"\xaa\xbb"
    assert struct.unpack("<I", ev[3].data)[0] == 1 << 29 and struct.unpack("<I", ev[4].data)[0] == 1 << 4


@pytest.mark.parametrize("text, message", [
    ("0 enable 1\n", "before the first"),
    ("case a b\n", "expected key=value"),
    ("case a frames=0\n", "frames 0"),
    ("case a source=24\n", "source 24"),
    ("case a zoom=2\n", "unknown key"),
    ("case bad-name\n", "bad name"),
    ("case a\nmem 0x20000000 noise seed=1\n", "mem <address>"),
    ("case a\nmem 0x20000000 hum seed=1 bytes=4\n", "kind hum"),
    ("case a\nmem 0x20400000 noise seed=1 bytes=4\n", "outside the first 4 MiB"),
    ("case a\nmem 0x20000000 walk16 seed=1 bytes=3\n", "even"),
    ("case a\n200 enable 1\n", "frame 200"),
    ("case a\n0 dance\n", "unknown command"),
    ("case a\n0 enable\n", "needs 1"),
    ("case a\n0 gain 3 1 1 1 1\n", "mix is 0 to 2"),
    ("case a\n0 queue 4 addr=0 len=0 id=0\n", "slot is 0 to 3"),
    ("case a\n0 queue 0 addr=0 len=0\n", "needs id="),
    ("case a\n0 embedded addr=0 len=0 id=0 colour=3\n", "unknown argument"),
    ("case a\n0 simple 1\n", "needs 2 numbers"),
    ("case a\n0 simple 70000 0\n", "does not fit"),
    ("case a\n0 coeffs rand:1:40000\n", "rand max"),
    ("case a\n0 format pcm16\n", "needs 2"),
    ("case a\n0 format pcm16 5\n", "2-bit"),
    ("case a\n0 poke 0x10 aa\n", "first 4 MiB"),
    ("case a\ncase a\n", "two cases named a"),
])
def test_bad_cases_are_refused_with_the_line(text, message):
    with pytest.raises(oracle.CaseError, match=message):
        oracle.parse_cases(text)
    if not text.startswith("case a\ncase a"):
        with pytest.raises(oracle.CaseError, match="line"):
            oracle.parse_cases(text)


def test_the_driver_input_lists_memory_events_and_the_end_of_each_case():
    [case] = oracle.parse_cases("case c source=2 frames=3\nmem 0x20000000 noise seed=1 bytes=4\n1 sleep\n1 enable 1\n")
    lines = oracle.driver_input([case]).splitlines()
    assert lines[0] == "case c 3 2"
    assert lines[1] == "mem 20000000 " + oracle.generate("noise", 1, 4).hex()
    assert lines[2] == "ev 1 S 0 -"
    assert lines[3] == "ev 1 c 160 01" and lines[4] == "ev 1 d 0 00000100"
    assert lines[-1] == "end"


def row(frame: int, name: str = "c") -> str:
    return f"frame {name} {frame} 0000000a 0000000b 0000000c 0000000d"


def test_the_table_writer_copies_the_drivers_answers_and_echoes_the_cases():
    [case] = oracle.parse_cases("case c frames=2\nmem 0x20000000 noise seed=1 bytes=4\n0 enable 1\n")
    results = oracle.parse_driver_output(row(0) + "\n" + row(1) + "\n", [case])
    assert results == {"c": "0000000a0000000b0000000c0000000d" * 2}
    table = oracle.render_inc([case], results, "c" * 40, "d" * 64)
    lines = table.splitlines()
    assert lines[0].startswith("// Generated by") and "DO NOT EDIT" in lines[0]
    assert "c" * 40 in lines[1] and "d" * 64 in lines[1]
    body = "\n".join(line for line in lines if not line.startswith("//"))
    crc = zlib.crc32(oracle.generate("noise", 1, 4))
    assert body.startswith(f'{{"c", 2, 0, {{{{0x20000000, 4, 0, 1, {crc:#x}}}}},')
    assert "{0, 'c', 160, \"01\"}, {0, 'd', 0, \"00000100\"}" in body
    assert body.rstrip().endswith('\n "0000000a0000000b0000000c0000000d0000000a0000000b0000000c0000000d"},')
    assert oracle.render_inc([case], results, "c" * 40, "d" * 64) == table       # deterministic


def test_a_driver_answer_that_does_not_fit_the_cases_is_refused():
    [case] = oracle.parse_cases("case c frames=2\n")
    with pytest.raises(oracle.CaseError, match="0 frames answered, 2 asked"):
        oracle.parse_driver_output("", [case])
    with pytest.raises(oracle.CaseError, match="out of order"):
        oracle.parse_driver_output(row(1) + "\n" + row(0) + "\n", [case])
    with pytest.raises(oracle.CaseError, match="unexpected"):
        oracle.parse_driver_output("what\n", [case])
    with pytest.raises(oracle.CaseError, match="unexpected"):
        oracle.parse_driver_output(row(0, "other") + "\n", [case])
    with pytest.raises(oracle.CaseError, match="bad CRCs"):
        oracle.parse_driver_output(row(0).replace("0000000a", "zzzzzzzz") + "\n", [case])


def test_the_committed_cases_file_parses_covers_the_wp_and_matches_the_table():
    with open(CASES, encoding="utf-8") as f:
        text = f.read()
    cases = oracle.parse_cases(text)
    assert len(cases) >= 100
    names = {c.name for c in cases}
    # every format x interpolation mode x rate of the sweep, mono and stereo, the loops, the queues out of
    # order, partial updates, both filters with random coefficients, ADPCM coefficient changes
    for fmt in ("pcm16_mono", "pcm16_stereo", "pcm8_mono", "pcm8_stereo", "adpcm_mono"):
        for interp in ("none", "linear", "poly"):
            assert any(n.startswith(f"{fmt}_{interp}_r") for n in names), (fmt, interp)
    for rate in ("0_25", "0_5", "1_0", "1_37", "2_9", "4_0"):
        assert f"pcm16_mono_linear_r{rate}" in names
    for wanted in ("loop_pcm16_mono", "loop_adpcm_mono", "ids_out_of_order", "embedded_among_queued",
                   "partial_update_pcm16_grow_shrink", "partial_update_shrink_below_position",
                   "partial_update_ignored_adpcm", "partial_update_ignored_pcm8", "filter_simple",
                   "filter_biquad", "filter_random_simple_1", "filter_random_biquad_1", "adpcm_coefficient_changes",
                   "adpcm_history_dirty_flag", "gain_ramps", "sleep_and_wakeup", "address_unmapped_then_valid",
                   "address_low_bits_are_masked", "rate_zero_and_negative", "loop_pcm16_rewritten_in_memory"):
        assert wanted in names, wanted
    assert all(c.frames >= 100 for c in cases)
    assert sum(1 for c in cases if c.frames == 200) >= 100
    with open(TABLE, encoding="utf-8") as f:
        table = f.read()
    assert sum(1 for line in table.splitlines() if line.startswith('{"')) == len(cases)   # a case added without regenerating
    for case in cases:
        assert f'{{"{case.name}", {case.frames}, {case.source}, ' in table
    # the cases file the table was made from
    import hashlib
    assert hashlib.sha256(text.encode("utf-8")).hexdigest() in table


def test_the_standins_say_what_they_replace():
    standin = os.path.join(ORACLE, "standin")
    found = []
    for folder, _, files in os.walk(standin):
        for name in files:
            with open(os.path.join(folder, name), encoding="utf-8") as f:
                head = f.read(900)
            assert "DSP oracle" in head, name
            found.append(os.path.relpath(os.path.join(folder, name), standin).replace(os.sep, "/"))
    for needed in ("common/common_types.h", "common/swap.h", "common/assert.h", "common/common_funcs.h",
                   "common/logging/log.h", "core/memory.h", "boost/serialization/access.hpp",
                   "boost/serialization/array.hpp", "boost/serialization/deque.hpp",
                   "boost/serialization/priority_queue.hpp", "boost/serialization/vector.hpp"):
        assert needed in found


def _azahar() -> str | None:
    path = os.path.expanduser(os.environ.get("MH4N_AZAHAR_DIR", ""))
    return path if path and os.path.isdir(os.path.join(path, "src", "audio_core", "hle")) else None


def test_the_harness_on_azahars_sources_reproduces_the_committed_table(tmp_path):
    azahar, cxx = _azahar(), shutil.which(os.environ.get("CXX", "clang++"))
    if azahar is None or cxx is None:
        message = "needs MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit) and clang++"
        if os.environ.get("MH4N_REQUIRE_TEST_DATA") == "1":
            pytest.fail(message)
        pytest.skip(message)
    with open(TABLE, "rb") as f:
        committed = f.read()
    for run in (1, 2):                                              # twice: nothing depends on the run
        out = tmp_path / f"table{run}.inc"
        done = subprocess.run(
            ["python3", os.path.join(ORACLE, "dsp_oracle.py"), "--azahar", azahar, "--out", str(out),
             "--cxx", cxx], capture_output=True, text=True, timeout=900)
        assert done.returncode == 0, done.stderr
        assert out.read_bytes() == committed, f"run {run} differs from native/tests/dsp_hle_expected.inc"


def test_the_crcs_are_written_in_lines_of_eight_frames():
    [case] = oracle.parse_cases("case c frames=20\n")
    results = {"c": "".join(f"{i:08x}" * 4 for i in range(20))}
    body = oracle.render_inc([case], results, "c" * 40, "d" * 64).splitlines()
    literals = [line for line in body if line.startswith(' "')]
    assert [len(line.rstrip("},")) - 3 for line in literals] == [256, 256, 128]
    assert "".join(line.strip().rstrip("},").strip('"') for line in literals) == results["c"]
    assert all(len(line) < 300 for line in body if not line.startswith("//") and "'c'" not in line)
