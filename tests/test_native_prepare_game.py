"""tools/native/prepare_game.py with synthetic inputs only: a CIA from test_extract_ncch.make_cia
and NCCHs from tools/make_cxi.py. Nothing here comes from the game."""
import json
import os
import struct
import sys
import zlib

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import blz_encoder  # noqa: E402
import make_cxi  # noqa: E402
import prepare_game as pg  # noqa: E402
import test_extract_ncch as tx  # noqa: E402
from test_exheader import make_exheader  # noqa: E402

PID = 0x0004000000ABCD00
TEXT, RO, DATA = b"T" * 0x1800, b"R" * 0x900, b"D" * 0x700


def code_image() -> bytes:
    # Each segment padded to its pages, as code.bin is laid out.
    return TEXT.ljust(0x2000, b"\0") + RO.ljust(0x1000, b"\0") + DATA.ljust(0x1000, b"\0")


def fake_exefs(code: bytes) -> bytes:
    hdr = bytearray(0x200)
    struct.pack_into("<8sII", hdr, 0, b".code", 0, len(code))
    return bytes(hdr) + code.ljust((len(code) + 0x1FF) & ~0x1FF, b"\0")


def make_ncch(compressed: bool, romfs: bytes, pid: int = PID) -> tuple[bytes, bytes]:
    code = code_image()
    stored = blz_encoder.compress(code) if compressed else code
    exh = make_exheader(program_id=pid, compressed=compressed, text=(0x100000, 2, len(TEXT)),
                        ro=(0x200000, 1, len(RO)), data=(0x300000, 1, len(DATA)))
    return make_cxi.make_cxi(exh, fake_exefs(stored), romfs), code


@pytest.fixture
def versions(tmp_path):
    p = tmp_path / "versions.json"
    p.write_text(json.dumps({"versions": [{
        "name": "Synthetic v1.3.0 (test)", "target": True, "program_id": f"{PID:016X}",
        "update_title_id": f"{tx.PROGRAM_ID:016X}", "title_version": 1072}]}))
    return str(p)


def write_inputs(tmp_path, compressed=True, base_pid=PID, update_pid=PID):
    base, _ = make_ncch(False, b"BASEROMFS" * 100, base_pid)
    upd, code = make_ncch(compressed, b"UPDROMFS" * 50, update_pid)
    (tmp_path / "game.cxi").write_bytes(base)
    (tmp_path / "update.cia").write_bytes(tx.make_cia(upd))
    return str(tmp_path / "game.cxi"), str(tmp_path / "update.cia"), code


def test_writes_the_game_folder(tmp_path, versions):
    base, update, code = write_inputs(tmp_path)
    out = tmp_path / "game"
    info = pg.prepare(base, update, str(out), versions)
    assert sorted(os.listdir(out)) == ["code.bin", "exheader.bin", "info.txt", "romfs.bin",
                                       "update_romfs.bin"]
    assert (out / "code.bin").read_bytes() == code
    assert (out / "exheader.bin").read_bytes()[:5] == b"SYNTH"
    assert len((out / "exheader.bin").read_bytes()) == 0x800
    assert (out / "romfs.bin").read_bytes().startswith(b"BASEROMFS")
    assert (out / "update_romfs.bin").read_bytes().startswith(b"UPDROMFS")
    text = (out / "info.txt").read_text()
    assert f"CRC-32 {zlib.crc32(code):08X}" in text
    assert "v1.3.0" in text and "SYNTH" in text
    assert info and not (out / ".prepare-tmp").exists()


def test_uncompressed_code_is_kept(tmp_path, versions):
    base, update, code = write_inputs(tmp_path, compressed=False)
    pg.prepare(base, update, str(tmp_path / "game"), versions)
    assert (tmp_path / "game" / "code.bin").read_bytes() == code


def test_command_line_and_default_directory(tmp_path, versions, monkeypatch, capsys):
    base, update, _ = write_inputs(tmp_path)
    monkeypatch.chdir(tmp_path)
    assert pg.main(["--base", base, "--update", update, "--versions", versions]) == 0
    assert (tmp_path / "build" / "native-game" / "code.bin").exists()
    assert "build/native-game" in capsys.readouterr().out.replace(os.sep, "/")


def test_other_update_version_is_refused(tmp_path, versions):
    base, update, _ = write_inputs(tmp_path)
    data = bytearray(open(update, "rb").read())
    hdr = struct.unpack_from("<IHHIII", data, 0)
    pad = lambda n: (n + 63) & ~63  # noqa: E731
    tmd_off = pad(0x2020) + pad(0xA00) + pad(0x350)
    struct.pack_into(">H", data, tmd_off + 4 + 0x100 + 0x3C + 0x9C, (1 << 10) | (2 << 4))
    (tmp_path / "old.cia").write_bytes(bytes(data))
    assert hdr[0] == 0x2020
    with pytest.raises(pg.PrepareError, match="v1.2.0"):
        pg.prepare(base, str(tmp_path / "old.cia"), str(tmp_path / "game"), versions)
    assert pg.main(["--base", base, "--update", str(tmp_path / "old.cia"),
                    "-o", str(tmp_path / "game"), "--versions", versions]) == 1


def test_update_must_be_a_cia(tmp_path, versions):
    base, _, _ = write_inputs(tmp_path)
    with pytest.raises(pg.PrepareError, match="not a CIA"):
        pg.prepare(base, base, str(tmp_path / "game"), versions)


def test_wrong_program_is_refused(tmp_path, versions):
    base, update, _ = write_inputs(tmp_path, base_pid=PID + 0x100)
    with pytest.raises(pg.PrepareError, match="base game is program"):
        pg.prepare(base, update, str(tmp_path / "game"), versions)
    base, update, _ = write_inputs(tmp_path, update_pid=PID + 0x100)
    with pytest.raises(pg.PrepareError, match="update's code is program"):
        pg.prepare(base, update, str(tmp_path / "game"), versions)


def test_encrypted_input_is_refused(tmp_path, versions):
    base, update, _ = write_inputs(tmp_path)
    (tmp_path / "enc.3ds").write_bytes(tx.make_ncsd(tx.make_ncch(no_crypto=False)))
    with pytest.raises(pg.PrepareError, match="encrypted"):
        pg.prepare(str(tmp_path / "enc.3ds"), update, str(tmp_path / "game"), versions)


def test_missing_romfs_is_refused(tmp_path, versions):
    upd, _ = make_ncch(True, b"")
    base, _, _ = write_inputs(tmp_path)
    (tmp_path / "update.cia").write_bytes(tx.make_cia(upd))
    with pytest.raises(pg.PrepareError, match="update has no RomFS"):
        pg.prepare(base, str(tmp_path / "update.cia"), str(tmp_path / "game"), versions)
    assert not (tmp_path / "game" / ".prepare-tmp").exists()


def test_romfs_is_streamed_not_read(tmp_path, versions, monkeypatch):
    # The RomFS copy goes through extract_ncch._copy_range in 1 MiB blocks; make the block small
    # to show it is chunked, and check the content is intact.
    monkeypatch.setattr(pg.extract_ncch, "CHUNK", 64)
    base, update, _ = write_inputs(tmp_path)
    pg.prepare(base, update, str(tmp_path / "game"), versions)
    assert (tmp_path / "game" / "romfs.bin").read_bytes().startswith(b"BASEROMFS" * 100)
