"""tools/native/gcap.py: MGPU capture reader, checker, summariser, writer (synthetic captures only)."""
import io
import os
import struct
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import gcap  # noqa: E402

PAGE = gcap.PAGE_SIZE


def page(fill: int) -> bytes:
    return bytes([fill]) * PAGE


def valid_capture(path):
    """INIT, MEM, a command list with its PICA_REG, a display transfer, IRQs, 3 frames."""
    with gcap.Writer(str(path)) as w:
        w.init({gcap.K_PICA_REGS: gcap.pica_regs_block({0x40: 0x11111111})})
        w.mem(0x20000000, bytes(2 * PAGE))
        w.cpu_pages([(0x20000000, page(1))])
        w.submit_cmdlist(0x20001000, 0x100)
        w.pica_reg([(gcap.REG_FOG_MODE, 0x1, 5)])
        w.gpu_pages([(0x20001000, page(9))])
        w.irq(5, 1000)
        w.frame_marker(1200)
        w.gx([3, 0x1000, 0x2000, 0, 0, 0, 0, 0], ticks=10)
        w.irq(5, 3000)
        w.irq(2, 7)
        w.frame_marker(1201)
        w.submit_cmdlist(0x20001000, 0x100)
        w.pica_reg([(gcap.REG_FOG_MODE, 0x1, 7)])
        w.irq(5, 2000)
        w.frame_marker(1202)
    return str(path)


def test_valid_roundtrip_and_check(tmp_path):
    p = valid_capture(tmp_path / "ok.mgpu")
    recs = gcap.read_all(p)
    assert recs[0].type == gcap.INIT
    assert recs[1].type == gcap.MEM
    assert gcap.check_records(recs) == []
    assert gcap.main(["check", p]) == 0
    assert gcap.to_bytes(recs) == open(p, "rb").read()


def test_header_layout(tmp_path):
    buf = io.BytesIO()
    w = gcap.Writer(buf)
    w.frame = 7
    w.ticks = 0x0102030405060708
    w.record(gcap.FRAME, struct.pack("<I", 7))
    raw = buf.getvalue()
    assert raw[:8] == b"MGPU" + struct.pack("<I", 1)
    assert raw[8:] == struct.pack("<IIQI", 12, 7, 0x0102030405060708, 4) + struct.pack("<I", 7)


def test_init_block_sizes_match_documented_values():
    assert gcap.INIT_BLOCK_SIZES == {
        1: 4296, 2: 4096, 3: 3072, 4: 34368, 5: 34368, 6: 256, 7: 24576,
        8: 512, 9: 3584, 10: 284, 11: 288,
    }
    blocks = gcap.parse_init(gcap.build_init(
        {k: bytes(n) for k, n in gcap.INIT_BLOCK_SIZES.items()}))
    assert [k for k, _ in blocks] == list(range(1, 12))


def test_wrong_magic(tmp_path):
    p = tmp_path / "bad.mgpu"
    p.write_bytes(b"XXXX" + struct.pack("<I", 1))
    with pytest.raises(gcap.GcapError, match="magic"):
        gcap.read_all(str(p))
    assert gcap.main(["check", str(p)]) != 0


def test_unknown_version_refused(tmp_path):
    p = tmp_path / "v2.mgpu"
    p.write_bytes(b"MGPU" + struct.pack("<I", 2))
    with pytest.raises(gcap.GcapError, match="version"):
        gcap.read_all(str(p))
    assert gcap.main(["summary", str(p)]) != 0


def test_truncated_payload_and_header(tmp_path):
    p = valid_capture(tmp_path / "ok.mgpu")
    raw = open(p, "rb").read()
    for cut in (len(raw) - 3, len(raw) - 1):
        t = tmp_path / f"t{cut}.mgpu"
        t.write_bytes(raw[:cut])
        with pytest.raises(gcap.GcapError, match="truncated"):
            gcap.read_all(str(t))
        assert gcap.main(["check", str(t)]) != 0
    # cut inside a record header
    t = tmp_path / "hdr.mgpu"
    t.write_bytes(raw[:8 + 5])
    with pytest.raises(gcap.GcapError, match="truncated"):
        gcap.read_all(str(t))
    # a file header cut
    t = tmp_path / "fh.mgpu"
    t.write_bytes(raw[:6])
    with pytest.raises(gcap.GcapError, match="truncated"):
        gcap.read_all(str(t))


def test_unknown_record_type(tmp_path):
    p = tmp_path / "unk.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        w.record(99, b"abcd")
    problems = gcap.check_file(str(p))
    assert any("unknown record type 99" in x for x in problems)
    assert gcap.main(["check", str(p)]) == 1


def test_check_catches_structure_errors(tmp_path):
    # frames going back
    p = tmp_path / "frames.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        w.frame_marker(10)
        w.frame_marker(9)
    assert any("goes back" in x for x in gcap.check_file(str(p)))
    # command list without PICA_REG
    p = tmp_path / "nopica.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        w.submit_cmdlist(0x20000000, 16)
        w.frame_marker(1)
    assert any("not followed by PICA_REG" in x for x in gcap.check_file(str(p)))
    # command list at the very end
    p = tmp_path / "end.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        w.submit_cmdlist(0x20000000, 16)
    assert any("not followed by PICA_REG" in x for x in gcap.check_file(str(p)))
    # PICA_REG after a non-submit GX
    p = tmp_path / "stray.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        w.gx([3, 0, 0, 0, 0, 0, 0, 0])
        w.pica_reg([])
    assert any("does not follow" in x for x in gcap.check_file(str(p)))
    # no INIT first
    p = tmp_path / "noinit.mgpu"
    with gcap.Writer(str(p)) as w:
        w.frame_marker(1)
    assert any("must be INIT" in x for x in gcap.check_file(str(p)))
    # wrong block size and unknown kind
    p = tmp_path / "blk.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init({gcap.K_FOG_LUT: bytes(4)})
    assert any("FOG_LUT" in x for x in gcap.check_file(str(p)))
    p = tmp_path / "kind.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init({77: b""})
    assert any("kind 77" in x for x in gcap.check_file(str(p)))
    # bad payload sizes
    p = tmp_path / "sizes.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        w.record(gcap.IRQ, b"\0" * 8)
        w.record(gcap.MEM, struct.pack("<II", 0, 5) + b"abc")
        w.record(gcap.FLUSH, struct.pack("<III", 9, 0, 0))
    probs = gcap.check_file(str(p))
    assert len(probs) == 3


def test_memory_reconstruction(tmp_path):
    p = tmp_path / "mem.mgpu"
    base = 0x20000000
    with gcap.Writer(str(p)) as w:
        w.init()                                             # 0
        w.mem(base, b"\xaa" * (3 * PAGE))                    # 1
        w.cpu_pages([(base + PAGE, page(0x11))])             # 2
        w.gpu_pages([(base + PAGE, page(0x22)),              # 3
                     (base + 2 * PAGE, page(0x33))])
        w.mem(base + PAGE - 2, b"\x01\x02\x03\x04")          # 4: crosses a page boundary
    recs = gcap.read_all(str(p))
    assert gcap.check_records(recs) == []
    m = gcap.memory_at(recs, 1)
    assert m.pages == {}
    m = gcap.memory_at(recs, 2)
    assert m.read(base, 4) == b"\xaa" * 4 and m.read(base + 3 * PAGE, 2) == b"\0\0"
    m = gcap.memory_at(recs, 3)
    assert m.read(base + PAGE, 1) == b"\x11" and m.read(base + 2 * PAGE, 1) == b"\xaa"
    m = gcap.memory_at(recs, 4)
    assert m.read(base + PAGE, 1) == b"\x22" and m.read(base + 2 * PAGE, 1) == b"\x33"
    assert m.read(base, 1) == b"\xaa" and m.has_page(base)
    m = gcap.memory_at(recs, 5)
    assert m.read(base + PAGE - 2, 4) == b"\x01\x02\x03\x04"
    assert m.read(base + PAGE - 3, 6) == b"\xaa\x01\x02\x03\x04\x22"


def test_register_replay_with_masks(tmp_path):
    p = tmp_path / "regs.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init({gcap.K_PICA_REGS: gcap.pica_regs_block({0x10: 0xAABBCCDD})})   # 0
        w.submit_cmdlist(0x20000000, 16)                                          # 1
        w.pica_reg([(0x10, 0xF, 0x01020304), (0x11, 0x2, 0xFFFFFFFF)])           # 2
        w.submit_cmdlist(0x20000000, 16)                                          # 3
        w.pica_reg([(0x10, 0x5, 0x99999999), (0x11, 0x0, 0x12345678),           # 4
                    (0x2FF, 0x8, 0x77000000)])
    recs = gcap.read_all(str(p))
    assert gcap.check_records(recs) == []
    r = gcap.registers_at(recs, 2)
    assert r[0x10] == 0xAABBCCDD and len(r) == 0x300
    r = gcap.registers_at(recs, 3)
    assert r[0x10] == 0x01020304 and r[0x11] == 0x0000FF00
    r = gcap.registers_at(recs, 5)
    # mask 0x5: bytes 0 and 2 from the new value, bytes 1 and 3 kept
    assert r[0x10] == 0x01990399 and r[0x11] == 0x0000FF00 and r[0x2FF] == 0x77000000
    with pytest.raises(gcap.GcapError):
        gcap.registers_at(recs, 0)


def test_mask_expand():
    assert gcap.mask_expand(0) == 0
    assert gcap.mask_expand(0xF) == 0xFFFFFFFF
    assert gcap.mask_expand(0x9) == 0xFF0000FF


def test_summary_fog_irq_and_counts(tmp_path):
    p = valid_capture(tmp_path / "ok.mgpu")
    s = gcap.summarize(gcap.read_all(p))
    assert s["counts"]["INIT"] == 1 and s["counts"]["FRAME"] == 3
    assert s["counts"]["GX"] == 3 and s["counts"]["PICA_REG"] == 2
    assert s["gx"] == {"DISPLAY_TRANSFER": 1, "SUBMIT_CMDLIST": 2}
    assert s["frames"] == {"count": 3, "first": 1200, "last": 1202}
    assert s["fog_modes"] == {"fog": 1, "gas": 1}
    assert s["irq"]["P3D"] == {"id": 5, "count": 3, "min_ns": 1000, "median_ns": 2000,
                               "max_ns": 3000}
    assert s["irq"]["PDC0"]["count"] == 1 and s["irq"]["PDC0"]["median_ns"] == 7
    text = gcap.format_summary(s)
    assert "gas: 1" in text and "P3D (id 5): count=3 min=1000 median=2000 max=3000" in text


def test_summary_even_median_and_fog_mask(tmp_path):
    p = tmp_path / "m.mgpu"
    with gcap.Writer(str(p)) as w:
        w.init()
        for d in (10, 20, 40, 100):
            w.irq(6, d)
        w.submit_cmdlist(0x20000000, 16)
        # mask without byte 0: not a fog mode write; value 3 = unknown mode
        w.pica_reg([(gcap.REG_FOG_MODE, 0x2, 7), (gcap.REG_FOG_MODE, 0x1, 0x10003)])
    s = gcap.summarize(gcap.read_all(str(p)))
    assert s["irq"]["DMA"]["median_ns"] == 30
    assert s["fog_modes"] == {"other(3)": 1}


def test_cli_summary(tmp_path, capsys):
    p = valid_capture(tmp_path / "ok.mgpu")
    assert gcap.main(["summary", p]) == 0
    assert "SUBMIT_CMDLIST: 2" in capsys.readouterr().out
    assert gcap.main(["summary", p, "--json"]) == 0
    assert '"fog_modes"' in capsys.readouterr().out


def test_pica_reg_validation():
    bad_pad = struct.pack("<I", 1) + struct.pack("<HBBI", 1, 0xF, 1, 0)
    with pytest.raises(gcap.GcapError, match="pad"):
        gcap.parse_pica_reg(bad_pad)
    out_of_range = struct.pack("<I", 1) + struct.pack("<HBBI", 0x300, 0xF, 0, 0)
    with pytest.raises(gcap.GcapError, match="range"):
        gcap.parse_pica_reg(out_of_range)
    bad_mask = struct.pack("<I", 1) + struct.pack("<HBBI", 1, 0x1F, 0, 0)
    with pytest.raises(gcap.GcapError, match="mask"):
        gcap.parse_pica_reg(bad_mask)


def test_unaligned_page_address_rejected():
    payload = struct.pack("<II", 1, 0x20000010) + bytes(PAGE)
    with pytest.raises(gcap.GcapError, match="aligned"):
        gcap.parse_pages(payload)


def _capture_with(tmp_path, name, init_payload=None, tail=None):
    p = tmp_path / name
    with gcap.Writer(str(p)) as w:
        if init_payload is None:
            w.init()
        else:
            w.record(gcap.INIT, init_payload)
        if tail:
            tail(w)
    return str(p)


def _full_blocks():
    return {k: bytes(n) for k, n in gcap.INIT_BLOCK_SIZES.items()}


def test_init_duplicate_kind(tmp_path):
    b = _full_blocks()
    payload = gcap.build_init(b) + struct.pack("<II", 2, len(b[2])) + b[2]
    payload = struct.pack("<I", len(b) + 1) + payload[4:]
    p = _capture_with(tmp_path, "dup.mgpu", payload)
    assert any("increasing order" in x for x in gcap.check_file(p))
    assert gcap.main(["check", p]) != 0


def test_init_out_of_order_kind(tmp_path):
    b = _full_blocks()
    order = [2, 1] + list(range(3, 12))
    payload = struct.pack("<I", len(order))
    for k in order:
        payload += struct.pack("<II", k, len(b[k])) + b[k]
    p = _capture_with(tmp_path, "order.mgpu", payload)
    assert any("increasing order" in x for x in gcap.check_file(p))


def test_init_missing_kind(tmp_path):
    b = _full_blocks()
    del b[3]
    p = _capture_with(tmp_path, "missing.mgpu", gcap.build_init(b))
    assert any("missing block kinds 3" in x for x in gcap.check_file(p))


def test_frame_payload_must_match_header(tmp_path):
    def tail(w):
        w.record(gcap.FRAME, struct.pack("<I", 5), frame=6)
    p = _capture_with(tmp_path, "frame.mgpu", None, tail)
    assert any("FRAME" in x and "header frame" in x for x in gcap.check_file(p))


def test_summary_on_malformed_fixed_record_is_clean(tmp_path, capsys):
    def tail(w):
        w.record(gcap.FRAME, b"\x01\x02")
    p = _capture_with(tmp_path, "short.mgpu", None, tail)
    assert gcap.main(["summary", p]) == 1
    assert "gcap:" in capsys.readouterr().err
