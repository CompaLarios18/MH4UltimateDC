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
