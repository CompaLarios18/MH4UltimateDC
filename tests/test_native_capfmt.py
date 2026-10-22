# Derived from Azahar src/audio_core/hle/shared_memory.h, hle.cpp and src/core/hle/service/hid/hid.h
# at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; each says "Licensed under GPLv2 or
# any later version"): the synthetic captures put the frame counters, the source status blocks and
# the HID entries at the offsets of those headers, and the expected choice of the region the DSP
# reads restates CurrentRegionIndex of hle.cpp.
# Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (hid.h)
# Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
# Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
"""tools/native/capfmt.py: MDSP / MHID capture reader, summariser, comparator, writer (synthetic captures only)."""
import json
import os
import struct
import sys
import wave
import zlib

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import capfmt  # noqa: E402

TICKS = capfmt.DSP_FRAME_TICKS


def dsp_capture(path, counters, game_frame0=1200, statuses=None, pcm_fill=None, reads=True):
    """One audio frame per counter. statuses: {counter: {source: block}} written to the read region."""
    with capfmt.Writer(str(path), "dsp", game=0x0004000000155400) as w:
        for i, counter in enumerate(counters):
            frame = game_frame0 + i // 2
            tick = i * TICKS
            blocks = (statuses or {}).get(counter, {})
            w.dsp_regions(frame, tick, capfmt.make_region(counter, blocks), capfmt.make_region(counter - 1))
            if reads:
                w.dsp_samples(frame, tick, 0x18000000 + 0x100 * (i % 2), bytes([i % 2 + 1]) * 64)
            samples = [(100 * (i + 1), -100)] * 160 if pcm_fill is None else pcm_fill
            w.dsp_pcm(frame, tick, capfmt.make_pcm(samples))
    return str(path)


def hid_capture(path, frames, change=None):
    """One HID record per game frame; change = {frame: dict of make_hid arguments}."""
    with capfmt.Writer(str(path), "hid") as w:
        for f in frames:
            args = (change or {}).get(f, {})
            w.hid(f, f * 4000000 + 17, capfmt.make_hid(**args))
    return str(path)


# ── format ────────────────────────────────────────────────────────────────────────────


def test_layout_constants():
    assert capfmt.COUNTER_OFFSET == capfmt.REGION_SIZE - 2
    assert capfmt.STATUS_OFFSET == 0x800 + 0x20 + 0x260 + 640  # padding, dsp_status, dsp_debug, final_samples
    assert capfmt.REGIONS_SIZE == 0x10000 and capfmt.PCM_SIZE == 640 and capfmt.HID_SIZE == 0x2B0


def test_roundtrip_and_manifest(tmp_path):
    p = dsp_capture(tmp_path / "a.mdsp", [10, 11, 12])
    raw = open(p, "rb").read()
    assert raw[:4] == b"MDSP" and struct.unpack("<I", raw[4:8])[0] == 1
    recs = capfmt.read_all(p)
    assert [r.type for r in recs[:3]] == [1, 3, 2]
    assert recs[0].frame == 1200 and recs[0].ticks == 0 and len(recs[0].payload) == 0x10000
    assert capfmt.file_kind(p) == "dsp"
    m = json.load(open(p + ".json"))
    assert m["tool"] == "mh4u_runner" and m["format"] == "MDSP" and m["version"] == 1
    assert m["game"] == "0x0004000000155400" and m["frames"] == 3
    assert m["records"] == {"1": 3, "2": 3, "3": 3}


def test_sample_bytes_written_once(tmp_path):
    p = tmp_path / "s.mdsp"
    with capfmt.Writer(str(p), "dsp") as w:
        assert w.dsp_samples(1, 0, 0x1000, b"\x01\x02\x03\x04") is True
        assert w.dsp_samples(2, 5, 0x1000, b"\x01\x02\x03\x04") is False  # same address, size, CRC
        assert w.dsp_samples(3, 6, 0x1000, b"\x01\x02\x03\x05") is True  # same address and size, other CRC
        assert w.dsp_samples(4, 7, 0x2000, b"\x01\x02\x03\x04") is True  # other address
    reads = [capfmt.parse_sample_read(r.payload) for r in capfmt.read(str(p))]
    assert [r[3] is not None for r in reads] == [True, False, True, True]
    assert reads[0][:3] == (0x1000, 4, zlib.crc32(b"\x01\x02\x03\x04"))
    assert reads[1][:3] == reads[0][:3]


def test_reader_rejects_bad_files(tmp_path):
    good = dsp_capture(tmp_path / "g.mdsp", [1, 2])
    raw = open(good, "rb").read()
    cases = {
        "magic": b"MXXX" + raw[4:],
        "version": raw[:4] + struct.pack("<I", 2) + raw[8:],
        "short": raw[:6],
        "truncated_header": raw[:8 + 10],
        "truncated_payload": raw[:8 + 20 + 100],
    }
    for name, data in cases.items():
        path = tmp_path / f"{name}.bin"
        path.write_bytes(data)
        with pytest.raises(capfmt.CapError):
            capfmt.read_all(str(path))
    with pytest.raises(capfmt.CapError):
        capfmt.read_all(str(tmp_path / "missing"))


def test_reader_rejects_bad_records(tmp_path):
    head = capfmt.FILE_HEADER.pack(b"MDSP", 1)

    def rec(t, payload):
        return capfmt.REC_HEADER.pack(t, 0, 0, len(payload)) + payload

    bad_crc = struct.pack("<III", 0x1000, 4, 0) + b"abcd"
    bad_len = struct.pack("<III", 0x1000, 8, 0) + b"abcd"
    for name, data in {"type": rec(9, b""), "regions": rec(1, bytes(10)), "pcm": rec(2, bytes(10)),
                       "crc": rec(3, bad_crc), "len": rec(3, bad_len), "short": rec(3, bytes(4))}.items():
        p = tmp_path / name
        p.write_bytes(head + data)
        with pytest.raises(capfmt.CapError):
            capfmt.read_all(str(p))
        assert capfmt.main(["check", str(p)]) == 1
    hp = tmp_path / "h"
    hp.write_bytes(capfmt.FILE_HEADER.pack(b"MHID", 1) + rec(1, bytes(10)))
    with pytest.raises(capfmt.CapError):
        capfmt.read_all(str(hp))


def test_writer_refuses_bad_record(tmp_path):
    with capfmt.Writer(str(tmp_path / "x"), "dsp") as w:
        with pytest.raises(capfmt.CapError):
            w.dsp_pcm(0, 0, b"short")
    with pytest.raises(capfmt.CapError):
        capfmt.Writer(str(tmp_path / "y"), "gpu")


# ── decoding ──────────────────────────────────────────────────────────────────────────


def test_current_region_rules():
    assert capfmt.current_region(5, 4) == 0
    assert capfmt.current_region(4, 5) == 1
    assert capfmt.current_region(0xFFFF, 0) == 1  # wraparound: region 0 is the old one
    assert capfmt.current_region(0, 0xFFFF) == 0
    assert capfmt.current_region(0xFFFF, 0xFFFE) == 0  # no wraparound
    assert capfmt.current_region(0xFFFE, 0xFFFF) == 1  # no wraparound, the other way
    assert capfmt.current_region(0, 0) == 1            # a tie: the DSP reads region 1 (not `>`)
    assert capfmt.current_region(7, 7) == 1
    assert capfmt.current_region(0xFFFF, 0xFFFF) == 1
    assert capfmt.current_region(1, 0) == 0            # the smallest margin


def test_dsp_counter_and_statuses():
    st = capfmt.make_status(enabled=1, sync_count=7, buffer_position=0x1234, current_buffer_id=3, last_buffer_id=2)
    r0 = capfmt.make_region(0x20, {5: st})
    r1 = capfmt.make_region(0x1F)
    payload = r0 + r1
    assert capfmt.region_counters(payload) == (0x20, 0x1F)
    assert capfmt.dsp_counter(payload) == 0x20
    assert capfmt.source_statuses(payload, 0)[5] == st
    assert capfmt.source_statuses(payload, 1)[5] == capfmt.make_status()
    assert len(capfmt.source_statuses(payload, 0)) == 24


def test_hid_state_decoding():
    buttons = capfmt.BUTTON_BITS["A"] | capfmt.BUTTON_BITS["START"] | capfmt.BUTTON_BITS["CIRCLE_UP"]
    data = capfmt.make_hid(buttons=buttons, circle=(-105, 77), touch=(160, 120, 1), index=5, touch_index=3)
    st = capfmt.hid_state(data)
    assert st.key() == (buttons, -105, 77, 160, 120, 1)
    assert "A,START" in st.describe() and "CIRCLE_UP" in st.describe()
    # an index past the 8 entries wraps rather than reading outside the block
    assert capfmt.hid_state(capfmt.make_hid(buttons=1, index=0)).buttons == 1


def test_ir_header():
    data = struct.pack("<II8B", 1, 2, 2, 0, 2, 0, 1, 0, 1, 0) + bytes(16)
    h = capfmt.ir_header(data)
    assert h["latest_receive_error"] == 1 and h["latest_send_error"] == 2
    assert h["connection_status"] == 2 and h["connected"] == 1 and h["initialized"] == 1


# ── summary and wav ───────────────────────────────────────────────────────────────────


def test_summary_dsp(tmp_path):
    p = dsp_capture(tmp_path / "a.mdsp", [10, 11, 12, 13], statuses={11: {0: capfmt.make_status(enabled=1)}})
    s = capfmt.summarize(p)
    assert s["kind"] == "dsp" and s["dsp_frames"] == 4 and s["pcm_frames"] == 4
    assert s["pcm_non_silent_frames"] == 4
    assert s["dsp_counter_first"] == 10 and s["dsp_counter_last"] == 13
    assert s["sample_reads"] == 4 and s["sample_reads_unique"] == 2 and s["sample_reads_without_bytes"] == 2
    assert s["sample_bytes_stored"] == 128 and s["sources_enabled_max"] == 1
    assert s["first_frame"] == 1200 and s["last_frame"] == 1201


def test_summary_silent(tmp_path):
    p = dsp_capture(tmp_path / "a.mdsp", [1, 2], pcm_fill=[(0, 0)], reads=False)
    s = capfmt.summarize(p)
    assert s["pcm_non_silent_frames"] == 0 and s["sample_reads"] == 0 and s["records"] == {"REGIONS": 2, "PCM": 2}


def test_summary_hid_and_cli(tmp_path, capsys):
    p = tmp_path / "h.mhid"
    with capfmt.Writer(str(p), "hid") as w:
        w.hid(1, 0, capfmt.make_hid())
        w.hid(2, 1, capfmt.make_hid(buttons=capfmt.BUTTON_BITS["B"], touch=(1, 2, 1)))
        w.hid(3, 2, capfmt.make_hid(circle=(10, 0)))
        w.ir(3, 2, struct.pack("<II8B", 0, 0, 2, 0, 2, 0, 1, 0, 1, 0) + bytes(48))
    s = capfmt.summarize(str(p))
    assert s["hid_records"] == 3 and s["hid_distinct_frames"] == 3 and s["frames_with_buttons"] == 1
    assert s["frames_with_touch"] == 1 and s["frames_with_circle_pad"] == 1 and s["buttons_seen"] == ["B"]
    assert s["ir_records"] == 1 and s["ir_connected_records"] == 1
    assert capfmt.main(["summary", str(p)]) == 0
    assert "kind: hid" in capsys.readouterr().out
    assert capfmt.main(["summary", str(p), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["hid_records"] == 3


def test_pcm_to_wav(tmp_path):
    p = dsp_capture(tmp_path / "a.mdsp", [1, 2, 3])
    out = tmp_path / "o.wav"
    assert capfmt.main(["pcm-to-wav", p, str(out)]) == 0
    with wave.open(str(out), "rb") as w:
        assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (2, 2, 32728)
        assert w.getnframes() == 3 * 160
        first = struct.unpack("<hh", w.readframes(1))
        assert first == (100, -100)
    assert capfmt.main(["pcm-to-wav", hid_capture(tmp_path / "h", [1]), str(tmp_path / "x.wav")]) == 2


def test_pcm_to_wav_with_an_output_that_cannot_be_written(tmp_path, capsys):
    p = dsp_capture(tmp_path / "a.mdsp", [1])
    capsys.readouterr()
    assert capfmt.main(["pcm-to-wav", p, str(tmp_path / "no-such-folder" / "o.wav")]) == 2
    assert capfmt.main(["pcm-to-wav", p, str(tmp_path)]) == 2  # a folder
    err = capsys.readouterr().err
    assert err.count("capfmt: cannot write ") == 2


def test_pcm_to_wav_never_hands_wave_a_path(tmp_path, monkeypatch):
    # wave.open() on a path that cannot be opened leaves a half-made writer whose __del__ prints a
    # traceback on Python 3.11/3.12; opening the file first avoids it on every version
    p = dsp_capture(tmp_path / "a.mdsp", [1])
    seen = []
    real = wave.open

    def spy(f, mode=None):
        seen.append(f)
        return real(f, mode)

    monkeypatch.setattr(capfmt.wave, "open", spy)
    assert capfmt.main(["pcm-to-wav", p, str(tmp_path / "o.wav")]) == 0
    assert len(seen) == 1 and not isinstance(seen[0], (str, bytes, os.PathLike))


def test_pcm_to_wav_with_an_unwritable_output_prints_no_traceback(tmp_path):
    # a fresh interpreter, so that a stray 'Exception ignored in' from a finalizer reaches stderr
    import subprocess
    p = dsp_capture(tmp_path / "a.mdsp", [1])
    tool = os.path.join(os.path.dirname(__file__), "..", "tools", "native", "capfmt.py")
    for out in (str(tmp_path / "no-such-folder" / "o.wav"), str(tmp_path)):
        r = subprocess.run([sys.executable, tool, "pcm-to-wav", p, out], capture_output=True, text=True)
        assert r.returncode == 2
        assert r.stderr.count("capfmt: cannot write ") == 1, r.stderr
        assert "Exception ignored" not in r.stderr and "Traceback" not in r.stderr, r.stderr


def test_pcm_to_wav_refuses_the_capture_as_its_output(tmp_path, capsys):
    p = dsp_capture(tmp_path / "a.mdsp", [1, 2])
    before = open(p, "rb").read()
    capsys.readouterr()
    assert capfmt.main(["pcm-to-wav", p, p]) == 2
    assert "the output is the capture itself" in capsys.readouterr().err
    assert open(p, "rb").read() == before


def test_pcm_to_wav_removes_the_partial_output_of_a_truncated_capture(tmp_path, capsys):
    p = dsp_capture(tmp_path / "a.mdsp", [1, 2, 3])
    raw = open(p, "rb").read()
    cut = tmp_path / "cut.mdsp"
    cut.write_bytes(raw[:-30])
    out = tmp_path / "o.wav"
    capsys.readouterr()
    assert capfmt.main(["pcm-to-wav", str(cut), str(out)]) == 2
    assert not out.exists()
    assert "capfmt:" in capsys.readouterr().err


# ── diff-hid ──────────────────────────────────────────────────────────────────────────


def test_diff_hid_equal_ignores_ticks(tmp_path, capsys):
    a = hid_capture(tmp_path / "a", range(100, 110))
    b = tmp_path / "b"
    with capfmt.Writer(str(b), "hid") as w:
        for f in range(100, 110):
            w.hid(f, 999 + f, capfmt.make_hid())  # other ticks
    assert capfmt.main(["diff-hid", a, str(b)]) == 0
    assert capsys.readouterr().out.strip() == "equal"


def test_diff_hid_ignores_other_bytes(tmp_path):
    a = hid_capture(tmp_path / "a", [1])
    b = tmp_path / "b"
    raw = bytearray(capfmt.make_hid())
    raw[0:8] = b"\x99" * 8  # index_reset_ticks: not compared
    raw[0x108:0x110] = b"\x55" * 8  # accelerometer: not compared
    with capfmt.Writer(str(b), "hid") as w:
        w.hid(1, 0, bytes(raw))
    assert capfmt.diff_hid(a, str(b)) == []


@pytest.mark.parametrize("field,args", [
    ("buttons", {"buttons": 1}),
    ("circle", {"circle": (5, -5)}),
    ("touch", {"touch": (10, 20, 1)}),
])
def test_diff_hid_different(tmp_path, capsys, field, args):
    a = hid_capture(tmp_path / "a", range(5))
    b = hid_capture(tmp_path / "b", range(5), change={3: args})
    assert capfmt.main(["diff-hid", a, str(b)]) == 1
    out = capsys.readouterr().out
    assert "frame 3" in out and "frame 2" not in out and "frame 4" not in out


def test_diff_hid_missing_and_extra_frames(tmp_path):
    a = hid_capture(tmp_path / "a", range(5))
    b = hid_capture(tmp_path / "b", [0, 1, 2, 4, 5])
    problems = capfmt.diff_hid(a, str(b))
    assert "frame 3: only in the reference" in problems and "frame 5: only in the candidate" in problems


def test_diff_hid_limit_and_kind_errors(tmp_path, capsys):
    a = hid_capture(tmp_path / "a", range(30))
    b = hid_capture(tmp_path / "b", range(30), change={f: {"buttons": 1} for f in range(30)})
    problems = capfmt.diff_hid(a, str(b))
    assert len(problems) == 11 and problems[-1] == "... and 20 more frames differ"
    d = dsp_capture(tmp_path / "d", [1])
    assert capfmt.main(["diff-hid", a, d]) == 2
    assert "not a HID capture" in capsys.readouterr().err
    assert capfmt.main(["diff-hid", a, str(tmp_path / "nope")]) == 2


# ── diff-dsp ──────────────────────────────────────────────────────────────────────────


def test_diff_dsp_equal(tmp_path, capsys):
    a = dsp_capture(tmp_path / "a", [10, 11, 12])
    b = dsp_capture(tmp_path / "b", [10, 11, 12], pcm_fill=[(1, 1)])  # other PCM and samples are not compared
    assert capfmt.main(["diff-dsp", a, b]) == 0
    assert capsys.readouterr().out.strip() == "equal"


def test_diff_dsp_equal_when_game_frames_differ(tmp_path):
    a = dsp_capture(tmp_path / "a", [10, 11, 12, 13], game_frame0=1200)
    b = dsp_capture(tmp_path / "b", [10, 11, 12, 13], game_frame0=1337)
    assert capfmt.diff_dsp(a, b) == []


def test_diff_dsp_different_status(tmp_path, capsys):
    st = capfmt.make_status(enabled=1, buffer_position=64)
    a = dsp_capture(tmp_path / "a", [10, 11, 12], statuses={11: {3: st}})
    b = dsp_capture(tmp_path / "b", [10, 11, 12], statuses={11: {3: capfmt.make_status(enabled=1, buffer_position=96)}})
    assert capfmt.main(["diff-dsp", a, b]) == 1
    out = capsys.readouterr().out
    assert "DSP frame 11 region 0 source 3" in out and "buffer_position=0x00000040" in out
    assert "DSP frame 10" not in out and "DSP frame 12" not in out


def test_diff_dsp_different_counter(tmp_path):
    a = dsp_capture(tmp_path / "a", [10, 11, 12])
    b = dsp_capture(tmp_path / "b", [10, 12, 13])
    problems = capfmt.diff_dsp(a, b)
    assert any("counter 11 in the reference, 12 in the candidate" in p for p in problems)


def test_diff_dsp_frames_option(tmp_path, capsys):
    st = capfmt.make_status(enabled=1)
    a = dsp_capture(tmp_path / "a", [1, 2, 3, 4], statuses={4: {0: st}})
    b = dsp_capture(tmp_path / "b", [1, 2, 3, 4])
    assert capfmt.main(["diff-dsp", a, b]) == 1
    capsys.readouterr()
    assert capfmt.main(["diff-dsp", a, b, "--frames", "3"]) == 0  # the difference is in the 4th
    assert capfmt.main(["diff-dsp", a, b, "--frames", "4"]) == 1


def test_diff_dsp_length_and_start_alignment(tmp_path):
    a = dsp_capture(tmp_path / "a", [10, 11, 12, 13])
    shorter = dsp_capture(tmp_path / "b", [10, 11])
    assert any("only in the reference" in p for p in capfmt.diff_dsp(a, shorter))
    assert capfmt.diff_dsp(a, shorter, frames=2) == []
    later = dsp_capture(tmp_path / "c", [12, 13])  # the candidate starts two DSP frames later
    assert capfmt.diff_dsp(a, later) == []
    assert capfmt.diff_dsp(later, a) == []
    st = capfmt.make_status(enabled=1)
    other = dsp_capture(tmp_path / "d", [12, 13], statuses={13: {1: st}})
    assert capfmt.diff_dsp(a, other)


def test_diff_dsp_region_one_status_and_kind_error(tmp_path, capsys):
    a = dsp_capture(tmp_path / "a", [10, 11])
    b = tmp_path / "b"
    with capfmt.Writer(str(b), "dsp") as w:
        for i, c in enumerate([10, 11]):
            other = capfmt.make_region(c - 1, {2: capfmt.make_status(enabled=1)} if c == 11 else {})
            w.dsp_regions(1200 + i, i, capfmt.make_region(c), other)
    problems = capfmt.diff_dsp(a, str(b))
    assert len(problems) == 1 and "DSP frame 11 region 1 source 2" in problems[0]
    h = hid_capture(tmp_path / "h", [1])
    assert capfmt.main(["diff-dsp", a, h]) == 2
    assert "not a DSP capture" in capsys.readouterr().err


def test_cli_check(tmp_path, capsys):
    p = dsp_capture(tmp_path / "a", [1])
    assert capfmt.main(["check", p]) == 0
    assert "ok" in capsys.readouterr().out
    assert capfmt.main(["check", str(tmp_path / "missing")]) == 2


# ── diff-dsp: the region the DSP reads ───────────────────────────────────────────────


def pair_capture(path, pairs):
    """One audio frame per (region 0 counter, region 1 counter) pair."""
    with capfmt.Writer(str(path), "dsp", game=0x0004000000155400) as w:
        for i, (c0, c1) in enumerate(pairs):
            w.dsp_regions(1200, i * TICKS, capfmt.make_region(c0), capfmt.make_region(c1))
            w.dsp_pcm(1200, i * TICKS, capfmt.make_pcm([(1, 1)] * 160))
    return str(path)


def test_diff_dsp_keys_on_the_region_the_dsp_reads(tmp_path):
    # Region 1 holds the higher counter, region 0 never changes: the key is region 1's counter.
    # A capture that starts one DSP frame later must be aligned by it and compare equal.
    a = pair_capture(tmp_path / "a", [(3, 10), (3, 11), (3, 12)])
    b = pair_capture(tmp_path / "b", [(3, 11), (3, 12)])
    assert capfmt.diff_dsp(a, b) == []
    assert capfmt.diff_dsp(b, a) == []
    assert capfmt.summarize(a)["dsp_counter_first"] == 10
    assert capfmt.summarize(a)["dsp_counter_last"] == 12


def test_diff_dsp_region_one_counter_differs(tmp_path):
    a = pair_capture(tmp_path / "a", [(3, 10), (3, 11)])
    b = pair_capture(tmp_path / "b", [(3, 10), (3, 12)])
    problems = capfmt.diff_dsp(a, b)
    assert any("DSP frame #1" in p and "11" in p and "12" in p for p in problems)


def test_diff_dsp_keys_through_wraparound(tmp_path):
    # Region 0 sits at 0xFFFF while region 1 has wrapped: the DSP reads region 1, keys 0, 1, 2.
    a = pair_capture(tmp_path / "a", [(0xFFFF, 0), (0xFFFF, 1), (0xFFFF, 2)])
    b = pair_capture(tmp_path / "b", [(0xFFFF, 1), (0xFFFF, 2)])
    assert capfmt.diff_dsp(a, b) == []
    assert capfmt.summarize(a)["dsp_counter_first"] == 0
    c = pair_capture(tmp_path / "c", [(0xFFFF, 0), (0xFFFF, 1), (0xFFFF, 5)])
    assert any("counter 2 in the reference, 5 in" in p for p in capfmt.diff_dsp(a, c))


def test_diff_dsp_region_zero_read_when_higher(tmp_path):
    a = pair_capture(tmp_path / "a", [(12, 3), (13, 3), (14, 3)])
    b = pair_capture(tmp_path / "b", [(13, 3), (14, 3)])
    assert capfmt.diff_dsp(a, b) == []
    assert capfmt.summarize(a)["dsp_counter_first"] == 12


# ── diff-dsp --align: two timing models (N0-32, D51) ──────────────────────────────────────

# The stream time at which the buffer id of source 0 advances (a model's own DSP counter is the stream time
# plus that model's shift).
CHANGES = [90, 93, 97, 100, 104, 107, 111, 115, 118, 122, 125, 129, 133, 136, 140, 144]


def stream_status(t, buffer_id_at=None):
    ident = 1 + sum(1 for c in CHANGES if t >= c)
    last = max((c for c in CHANGES if t >= c), default=0)
    return capfmt.make_status(enabled=1, sync_count=ident, buffer_position=(t - last) * 160,
                              current_buffer_id=ident, last_buffer_id=ident - 1)


def stream(path, start, t_first, count, tweak=None, source=0, sources=None, stretch_after=None):
    """`count` DSP frames; frame i has the DSP counter (start + i) and holds the audio stream at time t_first + i
    (from `stretch_after` on the stream falls one frame behind, and one more every three frames: no constant shift).
    As on the console, the two regions swap with the counter: the region of the counter's parity (`counter & 1`) is the
    one the DSP reads and holds the frame's status; the other holds the previous frame's, with the counter before.
    `tweak(i, region, number, block)` may change a status block (`region` is the region's own number)."""
    with capfmt.Writer(str(path), "dsp", game=0x0004000000155400) as w:
        for i in range(count):
            counter = (start + i) & 0xFFFF
            t = t_first + i
            if stretch_after is not None and i >= stretch_after:
                t -= 1 + (i - stretch_after) // 3
            regions = []
            for region in (0, 1):
                age = 0 if region == (counter & 1) else 1                 # 0: the current frame's, 1: the previous one's
                mine = {}
                for number in ([source] if sources is None else sources):
                    block = stream_status(t - age)
                    if tweak is not None:
                        block = tweak(i, region, number, block)
                    mine[number] = block
                regions.append(capfmt.make_region((counter - age) & 0xFFFF, mine))
            w.dsp_regions(1200 + i // 2, i * TICKS, regions[0], regions[1])
            w.dsp_pcm(1200 + i // 2, i * TICKS, capfmt.make_pcm([(i + 1, 1)] * 160))
    return str(path)


def aligned_pair(tmp_path, shift=22, **cand):
    """The reference holds stream times 100..139 under counters 100..139; the candidate's counters are `shift`
    later for the same audio and its capture starts ten audio frames earlier (counters shift - 10 on)."""
    ref = stream(tmp_path / "ref", 100, 100, 40)
    other = stream(tmp_path / "cand", 100 + shift - 10, 90, 40, **cand)
    return ref, other


def test_a_constant_shift_passes_and_is_printed(tmp_path, capsys):
    ref, cand = aligned_pair(tmp_path, 22)
    assert capfmt.diff_dsp(ref, cand)                                      # the plain comparison keys on the counter
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert result.equal and result.shift == 22 and "+22 DSP frames" in result.summary
    assert capfmt.main(["diff-dsp", ref, cand]) == 1
    capsys.readouterr()
    assert capfmt.main(["diff-dsp", ref, cand, "--align"]) == 0
    out = capsys.readouterr().out
    assert out.startswith("equal after alignment") and "shift +22 DSP frames" in out
    assert "status blocks compared" in out and "buffer changes at the shift" in out


@pytest.mark.parametrize("shift", [0, 1, 22, -7, 48, -48])
def test_any_constant_shift_up_to_the_bound_passes(tmp_path, shift):
    ref, cand = aligned_pair(tmp_path, shift)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert result.equal and result.shift == shift, result.problems


def test_a_shift_over_the_bound_fails(tmp_path, capsys):
    ref, cand = aligned_pair(tmp_path, 49)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and result.shift == 49 and "more than the 48 allowed" in result.problems[0]
    assert capfmt.diff_dsp_aligned(ref, cand, max_shift=49).equal
    ref, cand = aligned_pair(tmp_path, 22)
    assert capfmt.main(["diff-dsp", ref, cand, "--align", "--max-shift", "10"]) == 1
    out = capsys.readouterr().out
    assert "shift +22 DSP frames" in out and "more than the 10 allowed" in out
    assert capfmt.main(["diff-dsp", ref, cand, "--align", "--max-shift", "22"]) == 0
    ref, cand = aligned_pair(tmp_path, -49)
    assert not capfmt.diff_dsp_aligned(ref, cand).equal


def test_a_shift_that_changes_fails(tmp_path):
    # from frame 20 on the candidate's audio falls one more frame behind with every frame: no constant shift
    ref, cand = aligned_pair(tmp_path, 22, stretch_after=20)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal
    assert any("no counterpart" in p for p in result.problems) or any("source 0" in p for p in result.problems)
    # a candidate that is simply one frame later for the second half: a status block differs after the shift
    ref, cand = aligned_pair(tmp_path, 22, stretch_after=10)
    assert not capfmt.diff_dsp_aligned(ref, cand).equal


def test_a_buffer_change_more_than_one_frame_off_the_shift_is_named(tmp_path):
    # the candidate's stream falls behind from its frame 20 on: its later changes are 2 and more frames off the shift of 22
    ref = stream(tmp_path / "ref", 100, 100, 40)
    cand = stream(tmp_path / "cand", 112, 90, 40, stretch_after=20)             # late by 1, 2, 3 ... frames
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and result.shift == 22
    assert any("has no counterpart within 1 frame" in p for p in result.problems)


def test_a_status_block_that_differs_after_the_shift_fails(tmp_path, capsys):
    def tweak(i, region, number, block):
        if (i, region, number) == (25, 1, 5):
            return capfmt.make_status(enabled=1, sync_count=3, buffer_position=7)
        return block
    ref = stream(tmp_path / "ref", 100, 100, 40, sources=[0, 5])
    cand = stream(tmp_path / "cand", 112, 90, 40, sources=[0, 5], tweak=tweak)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and result.shift == 22
    assert any("region 1 source 5" in p and "sync_count=0x0003" in p for p in result.problems)
    assert capfmt.main(["diff-dsp", ref, cand, "--align"]) == 1
    out = capsys.readouterr().out
    assert "shift +22" in out and "region 1 source 5" in out
    # a difference in the source the alignment is not made on, in the frames before the overlap, is not compared
    outside = stream(tmp_path / "cand2", 112, 90, 40, sources=[0, 5],
                     tweak=lambda i, r, n, b: capfmt.make_status(enabled=1, sync_count=9) if (i, n) == (2, 5) else b)
    assert capfmt.diff_dsp_aligned(ref, outside).equal


def test_every_status_field_is_compared_after_the_shift(tmp_path):
    base = dict(enabled=1, dirty=0, sync_count=4, buffer_position=640, current_buffer_id=4, last_buffer_id=3)
    for field in ("dirty", "sync_count", "buffer_position", "last_buffer_id", "enabled"):
        changed = dict(base)
        changed[field] = base[field] + 1
        ref = stream(tmp_path / "ref", 100, 100, 40, sources=[0, 3],
                     tweak=lambda i, r, n, b: capfmt.make_status(**base) if (i, r, n) == (18, 0, 3) else b)
        cand = stream(tmp_path / "cand", 112, 90, 40, sources=[0, 3],
                      tweak=lambda i, r, n, b, c=changed: capfmt.make_status(**c) if (i, r, n) == (28, 0, 3) else b)
        assert not capfmt.diff_dsp_aligned(ref, cand).equal, field


def test_an_extra_or_a_missing_buffer_change_fails(tmp_path):
    ref = stream(tmp_path / "ref", 100, 100, 40)
    # the candidate's audio stops advancing: one buffer never changes
    def freeze(i, region, number, block):
        return stream_status(111 - region) if i >= 24 else block                # the stream stays at time 111 from then on
    cand = stream(tmp_path / "cand", 112, 90, 40, tweak=freeze)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and any("no counterpart" in p for p in result.problems)


def test_no_buffer_id_in_common_fails(tmp_path):
    ref = stream(tmp_path / "ref", 100, 100, 40)

    def other_ids(i, region, number, block):
        return capfmt.make_status(enabled=1, current_buffer_id=int.from_bytes(block[8:10], "little") + 500)
    cand = stream(tmp_path / "cand", 112, 90, 40, tweak=other_ids)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and "no buffer id" in result.problems[0]


def test_the_counter_wraps_around(tmp_path):
    ref = stream(tmp_path / "ref", 0xFFF0, 100, 40)                              # counters 0xFFF0 .. 0x0017
    cand = stream(tmp_path / "cand", (0xFFF0 + 22 - 10) & 0xFFFF, 90, 40)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert result.equal and result.shift == 22, result.problems
    back = capfmt.diff_dsp_aligned(cand, ref)                                    # and the other way round
    assert back.equal and back.shift == -22


def test_without_an_enabled_source_the_plain_comparison_is_made(tmp_path):
    a = dsp_capture(tmp_path / "a", [10, 11, 12])
    b = dsp_capture(tmp_path / "b", [10, 11, 12])
    result = capfmt.diff_dsp_aligned(a, b)
    assert result.equal and result.shift == 0 and "nothing to align on" in result.summary
    # a capture of other counters is a difference of the plain kind, not a shift
    c = dsp_capture(tmp_path / "c", [10, 12, 13])
    assert not capfmt.diff_dsp_aligned(a, c).equal
    # a source enabled on one side only
    d = dsp_capture(tmp_path / "d", [10, 11, 12], statuses={11: {0: capfmt.make_status(enabled=1)}})
    result = capfmt.diff_dsp_aligned(a, d)
    assert not result.equal and "no source is enabled" in result.problems[0]
    assert not capfmt.diff_dsp_aligned(d, a).equal


def test_an_enabled_source_that_never_changes_buffer_is_compared_at_shift_zero(tmp_path):
    steady = {c: {0: capfmt.make_status(enabled=1, current_buffer_id=4)} for c in range(10, 14)}
    a = dsp_capture(tmp_path / "a", [10, 11, 12, 13], statuses=steady)
    b = dsp_capture(tmp_path / "b", [10, 11, 12, 13], statuses=steady)
    result = capfmt.diff_dsp_aligned(a, b)
    assert result.equal and result.shift == 0 and "never changes buffer" in result.summary
    late = dsp_capture(tmp_path / "c", [12, 13, 14, 15], statuses={c: steady[10] for c in range(12, 16)})
    assert capfmt.diff_dsp_aligned(a, late).problems == capfmt.diff_dsp(a, late) != []      # keyed on the counters, as the plain one
    moving = {c: {0: capfmt.make_status(enabled=1, current_buffer_id=4 + (c >= 12))} for c in range(10, 14)}
    m = dsp_capture(tmp_path / "m", [10, 11, 12, 13], statuses=moving)
    result = capfmt.diff_dsp_aligned(a, m)
    assert not result.equal and "never in the reference" in result.problems[0]


def test_the_samples_are_not_compared_when_aligning(tmp_path):
    ref = stream(tmp_path / "ref", 100, 100, 40)
    cand = stream(tmp_path / "cand", 112, 90, 40)
    assert capfmt.diff_dsp_aligned(ref, cand).equal       # the PCM fill differs with the frame index: not compared


def test_the_alignment_options_have_their_usage_errors(tmp_path, capsys):
    ref, cand = aligned_pair(tmp_path, 22)
    for extra in (["--max-shift", "10"], ["--align", "--max-shift", "-1"], ["--align", "--frames", "5"]):
        with pytest.raises(SystemExit) as stop:
            capfmt.main(["diff-dsp", ref, cand, *extra])
        assert stop.value.code == 2, extra
    capsys.readouterr()
    h = hid_capture(tmp_path / "h", [1])
    assert capfmt.main(["diff-dsp", ref, h, "--align"]) == 2
    assert "not a DSP capture" in capsys.readouterr().err
    assert capfmt.DEFAULT_MAX_SHIFT == 48                                  # D51


def test_the_alignment_prints_no_samples(tmp_path, capsys):
    ref, cand = aligned_pair(tmp_path, 22)
    capfmt.main(["diff-dsp", ref, cand, "--align"])
    out = capsys.readouterr().out
    assert "PCM" not in out and "sample" not in out.lower()


# ── diff-dsp --align, odd shifts and the larger bounds of the native profile (N0-33, D55) ──────────────

def test_an_odd_shift_compares_the_region_the_shift_maps_to(tmp_path):
    ref, cand = aligned_pair(tmp_path, 21)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert result.equal and result.shift == 21, result.problems
    assert "the shift is odd" in result.summary and "region 1 - r" in result.summary
    # compared region against the same region, the same frames differ: that is what the mapping is for
    frames_ref, frames_cand = capfmt._dsp_frames(ref), capfmt._dsp_frames(cand)
    by_cand = dict(frames_cand)
    plain = mapped = 0
    for counter, payload in frames_ref:
        other = by_cand.get((counter + 21) & 0xFFFF)
        if other is None:
            continue
        for region in (0, 1):
            plain += capfmt.source_statuses(payload, region) != capfmt.source_statuses(other, region)
            mapped += capfmt.source_statuses(payload, region) != capfmt.source_statuses(other, 1 - region)
    assert plain > 0 and mapped == 0
    # an even shift keeps the regions as they are
    even = capfmt.diff_dsp_aligned(*aligned_pair(tmp_path, 22))
    assert even.equal and "the shift is odd" not in even.summary


@pytest.mark.parametrize("shift", [1, -1, 21, -7, -205, -209, 203, 299, -299])
def test_odd_and_even_shifts_up_to_the_native_bound_pass(tmp_path, shift):
    ref, cand = aligned_pair(tmp_path, shift)
    result = capfmt.diff_dsp_aligned(ref, cand, max_shift=300)
    assert result.equal and result.shift == shift, result.problems
    assert capfmt.diff_dsp_aligned(ref, cand, max_shift=abs(shift)).equal
    assert not capfmt.diff_dsp_aligned(ref, cand, max_shift=abs(shift) - 1).equal


def test_the_native_bound_is_an_option_and_the_default_stays_d51s(tmp_path, capsys):
    ref, cand = aligned_pair(tmp_path, -209)
    assert capfmt.DEFAULT_MAX_SHIFT == 48
    assert capfmt.main(["diff-dsp", ref, cand, "--align"]) == 1
    assert "shift -209 DSP frames" in capsys.readouterr().out
    assert capfmt.main(["diff-dsp", ref, cand, "--align", "--max-shift", "300"]) == 0
    out = capsys.readouterr().out
    assert "shift -209 DSP frames" in out and "the shift is odd" in out
    assert capfmt.main(["diff-dsp", ref, cand, "--align", "--max-shift", "208"]) == 1


def test_an_odd_shift_still_fails_a_status_block_that_differs_in_the_mapped_region(tmp_path):
    def tweak(i, region, number, block):
        if (i, region, number) == (22, 1, 5):
            return capfmt.make_status(enabled=1, sync_count=3, buffer_position=7)
        return block
    ref = stream(tmp_path / "ref", 100, 100, 40, sources=[0, 5])
    cand = stream(tmp_path / "cand", 111, 90, 40, sources=[0, 5], tweak=tweak)         # shift 21
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and result.shift == 21
    assert any("(candidate region 1) source 5" in p or "(candidate region 0) source 5" in p for p in result.problems)
    # the block at the same index on the other region is not what the tweak changed
    assert capfmt.diff_dsp_aligned(ref, stream(tmp_path / "ok", 111, 90, 40, sources=[0, 5])).equal


def test_an_odd_shift_needs_regions_that_alternate_with_the_counter(tmp_path):
    # a candidate that always keeps the frame the DSP reads in region 0 (no swap) cannot be the same stream at an odd shift
    def fixed(path, start, t_first, count):
        with capfmt.Writer(str(path), "dsp", game=0x0004000000155400) as w:
            for i in range(count):
                counter = (start + i) & 0xFFFF
                t = t_first + i
                w.dsp_regions(1200, i * TICKS, capfmt.make_region(counter, {0: stream_status(t)}),
                              capfmt.make_region((counter - 1) & 0xFFFF, {0: stream_status(t - 1)}))
                w.dsp_pcm(1200, i * TICKS, capfmt.make_pcm([(1, 1)] * 160))
        return str(path)
    ref = fixed(tmp_path / "ref", 100, 100, 40)
    odd = fixed(tmp_path / "odd", 111, 90, 40)
    even = fixed(tmp_path / "even", 112, 90, 40)
    assert capfmt.diff_dsp_aligned(ref, even).equal
    assert not capfmt.diff_dsp_aligned(ref, odd).equal


def test_the_odd_shift_message_names_both_regions(tmp_path):
    ref = stream(tmp_path / "ref", 100, 100, 40)
    cand = stream(tmp_path / "cand", 111, 90, 40, tweak=lambda i, r, n, b: capfmt.make_status(enabled=1, sync_count=9)
                  if i == 30 else b)
    result = capfmt.diff_dsp_aligned(ref, cand)
    assert not result.equal and any("region 0 (candidate region 1)" in p or "region 1 (candidate region 0)" in p
                                    for p in result.problems)
