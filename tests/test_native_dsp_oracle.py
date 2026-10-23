# Derived in part from Azahar src/audio_core/hle/shared_memory.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the byte offsets and bit positions of SourceConfiguration and DspConfiguration, and the offsets of the structures in a region, that the tests check the commands' writes against.
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
"""The oracle of the native HLE DSP (N3-02 the sources, N3-03 the mixers, the effects and the frame):
tools/native/dsp_oracle/.

The case reader, the commands (the bytes they write into a source's configuration, or, in a frame case, into
the region the DSP reads, with the middle-endian 32-bit values and the dirty bits), the data generators and
the table writer are tested on synthetic cases and a synthetic driver answer; the last test runs the whole
harness on Azahar's sources
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
    # N3-03: the frame cases (gains, the aux buses read from and written to the shared memory, the delay
    # effect, the master volume, each output format, 24 sources at once)
    frame_names = {c.name for c in cases if c.mode == "frame"}
    assert len(frame_names) >= 34
    for wanted in ("frame_silent_until_master_volume", "frame_master_volume_0_5", "frame_routing_to_the_three_mixes",
                   "frame_output_format_mono", "frame_output_format_stereo", "frame_output_format_surround",
                   "frame_aux_bus_0", "frame_aux_bus_1", "frame_delay_on_mix_0_2_frames",
                   "frame_delay_on_mix_1_5_frames", "frame_delay_edges", "frame_24_sources",
                   "frame_delay_saturates_to_the_s32_range", "frame_delay_818_and_819_frames",
                   "frame_counters_wrap", "frame_sleep_and_wakeup", "frame_reverb_does_nothing",
                   "frame_dirty_bits_are_cleared", "frame_saturation_40_0"):
        assert wanted in frame_names, wanted
    twenty_four = next(c for c in cases if c.name == "frame_24_sources")
    configs = {e.offset - oracle.SHM_SOURCE_CONFIGURATIONS for e in twenty_four.events if e.op == "r"}
    assert {k for k in range(24) if any(oracle.CONFIG_SIZE * k <= o < oracle.CONFIG_SIZE * (k + 1) for o in configs)} == set(range(24))
    with open(TABLE, encoding="utf-8") as f:
        table = f.read()
    assert sum(1 for line in table.splitlines() if line.startswith('{"')) == len(cases)   # a case added without regenerating
    for case in cases:
        third = case.first_counter if case.mode == "frame" else case.source
        assert f'{{"{case.name}", {case.frames}, {third}, ' in table
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
                   "common/logging/log.h", "common/string_util.h", "core/memory.h", "boost/serialization/access.hpp",
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


# ---- the frame cases (N3-03) -------------------------------------------------------------------------------------

def frame_events(text: str):
    [case] = oracle.parse_cases("case f mode=frame\n" + text)
    return case.events


def test_the_shared_memory_offsets_are_those_of_azahars_structures():
    # shared_memory.h: SharedMemory lists the structures in this order, ASSERT_DSP_STRUCT gives the sizes
    assert oracle.SHM_DSP_STATUS == 0x800 and oracle.SHM_FINAL_SAMPLES == 0xA80
    assert oracle.SHM_SOURCE_STATUSES == 0xD00 and oracle.SHM_DSP_CONFIGURATION == 0x2860
    assert oracle.SHM_INTERMEDIATE_MIX == 0x2924 and oracle.SHM_SOURCE_CONFIGURATIONS == 0x3D24
    assert oracle.SHM_ADPCM_COEFFICIENTS == 0x4F24
    assert oracle.SHM_ADPCM_COEFFICIENTS + 768 + 2 * (0x100 + 0xC0 + 0x180 + 0xA + 0x13A3) + 2 == oracle.REGION_SIZE
    assert oracle.DC_AUX_BUS_ENABLE == 40 and oracle.DC_DELAY_EFFECT == 44 and oracle.DC_REVERB_EFFECT == 84


def test_a_frame_case_has_a_mode_and_a_first_counter():
    [case] = oracle.parse_cases("case f mode=frame fc=65500 frames=10\n")
    assert (case.mode, case.first_counter, case.frames) == ("frame", 65500, 10)
    [case] = oracle.parse_cases("case s\n")
    assert (case.mode, case.first_counter) == ("source", 0)


def test_a_source_command_in_a_frame_case_writes_into_that_sources_configuration_in_the_region():
    ev = frame_events("0 s3 enable 1\n0 s3 queue 2 addr=0x20000010 len=20 id=3\n0 s3 coeffs rand:1\n")
    base = oracle.SHM_SOURCE_CONFIGURATIONS + 3 * 192
    writes = {e.offset: e.data for e in ev if e.op == "r"}
    assert writes[base + 160] == b"\x01"
    assert writes[base + 76 + 40] == oracle.dsp32(0x20000010)
    assert writes[oracle.SHM_ADPCM_COEFFICIENTS + 3 * 32] == b"".join(
        struct.pack("<h", v) for v in oracle.values(["rand:1"], 16, "x"))
    assert [(e.op, e.offset, e.data) for e in ev if e.op == "p"] == [("p", base + 74, b"\x04\x00")]
    ors = [(e.offset, struct.unpack("<I", e.data)[0]) for e in ev if e.op == "o"]
    assert ors == [(base, 1 << 16), (base, 1 << 19), (base, 1 << 2)]


def test_the_commands_of_the_mixers_write_the_dsp_configuration_and_its_dirty_bits():
    ev = frame_events("0 master 0.5\n0 auxreturn 1 0.25\n0 outfmt surround\n0 auxbus 1 1\n0 headphones 1\n")
    cfg = oracle.SHM_DSP_CONFIGURATION
    writes = {e.offset - cfg: e.data for e in ev if e.op == "r"}
    assert writes[4] == struct.pack("<f", 0.5) and writes[12] == struct.pack("<f", 0.25)
    assert writes[22] == b"\x02\x00" and writes[42] == b"\x01\x00" and writes[26] == b"\x01\x00"
    bits = 0
    for e in ev:
        assert e.op in "ro"
        if e.op == "o":
            assert e.offset == cfg
            bits |= struct.unpack("<I", e.data)[0]
    assert bits == (1 << 16) | (1 << 25) | (1 << 26) | (1 << 9) | (1 << 28)


def test_the_delay_command_writes_the_block_and_the_main_dirty_bit():
    ev = frame_events("0 delay 1 frames=7 g=-64 a=96 b=3 work=0x20100000 outputs=2\n")
    block = next(e for e in ev if e.op == "r")
    assert block.offset == oracle.SHM_DSP_CONFIGURATION + 44 + 20
    assert block.data == (struct.pack("<HHHH", 1, 1, 0, 2) + oracle.dsp32(0x20100000) +
                          struct.pack("<Hhhh", 7, -64, 96, 3))
    assert [struct.unpack("<I", e.data)[0] for e in ev if e.op == "o"] == [1 << 11]
    ev = frame_events("0 delay 0 enable=0 dirty=0 nomain\n")
    assert [e.op for e in ev] == ["r"] and ev[0].data[:4] == struct.pack("<HH", 0, 0)


def test_auxmix_is_a_generated_event_with_the_crc_of_its_samples():
    ev = frame_events("0 auxmix 2 seed=9 amp=1000\n")
    [e] = ev
    assert (e.op, e.offset) == ("g", oracle.SHM_INTERMEDIATE_MIX + 2560)
    seed, size, amp, crc = struct.unpack("<IIII", e.data)
    assert (seed, size, amp) == (9, 2560, 1000)
    data = oracle.generate_pcm32(9, 2560, 1000)
    assert crc == zlib.crc32(data) and len(data) == 2560
    samples = struct.unpack("<640i", data)
    assert max(samples) <= 1000 and min(samples) >= -1000 and len(set(samples)) > 100
    assert oracle.generate_pcm32(9, 2560, 1000) == data and oracle.generate_pcm32(10, 2560, 1000) != data
    assert len(oracle.generate_pcm32(1, 7, 5)) == 7


def test_sleep_wakeup_poke_dspwrite_and_dspdirty_in_a_frame_case():
    ev = frame_events("1 sleep\n2 wakeup\n3 poke 0x20000010 aabb\n4 dspdirty 0x80\n5 dspwrite 4 0000803f\n")
    assert [(e.frame, e.op) for e in ev] == [(1, "S"), (2, "W"), (3, "m"), (4, "o"), (5, "r")]
    assert ev[3].offset == oracle.SHM_DSP_CONFIGURATION and struct.unpack("<I", ev[3].data)[0] == 0x80
    assert ev[4].offset == oracle.SHM_DSP_CONFIGURATION + 4


def test_reverb_is_noise_with_its_dirty_bit():
    ev = frame_events("0 reverb 1 rand:4\n")
    block = next(e for e in ev if e.op == "r")
    assert block.offset == oracle.SHM_DSP_CONFIGURATION + 84 + 52 and len(block.data) == 52
    assert len(set(block.data)) > 10
    assert [struct.unpack("<I", e.data)[0] for e in ev if e.op == "o"] == [1 << 13]


@pytest.mark.parametrize("text, message", [
    ("case a mode=voice\n", "mode is source or frame"),
    ("case a mode=frame source=2\n", "no source="),
    ("case a fc=3\n", "fc= is for frame cases"),
    ("case a mode=frame fc=70000\n", "fc 70000"),
    ("case a mode=frame\n0 enable 1\n", "unknown command"),
    ("case a mode=frame\n0 s24 enable 1\n", "s0 to s23"),
    ("case a mode=frame\n0 s1\n", "s0 to s23"),
    ("case a mode=frame\n0 s1 sleep\n", "whole DSP"),
    ("case a mode=frame\n0 s1 poke 0x20000000 aa\n", "whole DSP"),
    ("case a mode=frame\n0 master\n", "needs 1"),
    ("case a mode=frame\n0 auxreturn 2 1.0\n", "index is 0 or 1"),
    ("case a mode=frame\n0 auxmix 3 seed=1\n", "bus is 1 or 2"),
    ("case a mode=frame\n0 auxmix 1\n", "needs seed="),
    ("case a mode=frame\n0 auxmix 1 seed=1 amp=4000000000\n", "amp is"),
    ("case a mode=frame\n0 dspwrite 0 00\n", "after its dirty word"),
    ("case a mode=frame\n0 dspwrite 195 0000\n", "after its dirty word"),
    ("case a mode=frame\n0 delay 0 colour=3\n", "unknown argument"),
    ("case a mode=frame\n0 reverb 0 zero\n", "rand:"),
])
def test_bad_frame_cases_are_refused_with_the_line(text, message):
    with pytest.raises(oracle.CaseError, match=message):
        oracle.parse_cases(text)


def test_the_driver_input_of_a_frame_case_names_the_offsets_for_the_driver_to_check():
    [case] = oracle.parse_cases("case f mode=frame fc=5 frames=3\nmem 0x20000000 noise seed=1 bytes=4\n1 sleep\n")
    lines = oracle.driver_input([case]).splitlines()
    assert lines[0] == "fcase f 3 5 " + " ".join(str(v) for v in (
        oracle.SHM_SOURCE_CONFIGURATIONS, oracle.SHM_ADPCM_COEFFICIENTS, oracle.SHM_DSP_CONFIGURATION,
        oracle.SHM_INTERMEDIATE_MIX, oracle.SHM_SOURCE_STATUSES, oracle.SHM_FINAL_SAMPLES, oracle.SHM_DSP_STATUS))
    assert lines[1].startswith("mem 20000000 ") and lines[2] == "ev 1 S 0 -" and lines[-1] == "end"


def test_a_frame_case_in_the_table_ends_with_1_and_carries_its_first_counter():
    cases = oracle.parse_cases("case s frames=1\ncase f mode=frame fc=65500 frames=1\n")
    results = {c.name: "0000000a0000000b0000000c0000000d" for c in cases}
    body = [l for l in oracle.render_inc(cases, results, "c" * 40, "d" * 64).splitlines() if not l.startswith("//")]
    assert body[0].startswith('{"s", 1, 0, ')
    frame_start = next(i for i, l in enumerate(body) if l.startswith('{"f"'))
    assert body[frame_start].startswith('{"f", 1, 65500, ')
    assert body[-1] == ' "0000000a0000000b0000000c0000000d", 1},'
    assert body[frame_start - 1] == ' "0000000a0000000b0000000c0000000d"},'


def test_the_frame_cases_of_a_sweep_name_themselves_by_their_values():
    cases = oracle.parse_cases("case g_${v} mode=frame sweep v=0.5,-1.0\n0 master ${v}\n")
    assert [c.name for c in cases] == ["g_0_5", "g__1_0"] and all(c.mode == "frame" for c in cases)
