"""tools/native/prepare_game.py with synthetic inputs only: a CIA from test_extract_ncch.make_cia
and NCCHs from tools/make_cxi.py. Nothing here comes from the game."""
import hashlib
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


def make_icon(lockout: int, size: int = 0x36C0) -> bytes:
    """A made-up SMDH: the magic, filler, and the region lockout word at 0x2018."""
    icon = bytearray(b"SMDH" + bytes((i * 7) & 0xFF for i in range(size - 4)))
    struct.pack_into("<I", icon, 0x2018, lockout)
    return bytes(icon)


def exefs_with_icon(code: bytes, icon: bytes) -> bytes:
    hdr = bytearray(0x200)
    code_pad = code.ljust((len(code) + 0x1FF) & ~0x1FF, b"\0")
    struct.pack_into("<8sII", hdr, 0, b".code", 0, len(code))
    struct.pack_into("<8sII", hdr, 16, b"icon", len(code_pad), len(icon))
    return bytes(hdr) + code_pad + icon.ljust((len(icon) + 0x1FF) & ~0x1FF, b"\0")


def make_ncch(compressed: bool, romfs: bytes, pid: int = PID, icon: bytes | None = None,
              exefs: bytes | None = None) -> tuple[bytes, bytes]:
    code = code_image()
    stored = blz_encoder.compress(code) if compressed else code
    exh = make_exheader(program_id=pid, compressed=compressed, text=(0x100000, 2, len(TEXT)),
                        ro=(0x200000, 1, len(RO)), data=(0x300000, 1, len(DATA)))
    if exefs is None:
        exefs = fake_exefs(stored) if icon is None else exefs_with_icon(stored, icon)
    return make_cxi.make_cxi(exh, exefs, romfs), code


@pytest.fixture
def versions(tmp_path):
    p = tmp_path / "versions.json"
    p.write_text(json.dumps({"versions": [{
        "name": "Synthetic v1.3.0 (test)", "target": True, "program_id": f"{PID:016X}",
        "update_title_id": f"{tx.PROGRAM_ID:016X}", "title_version": 1072}]}))
    return str(p)


def write_inputs(tmp_path, compressed=True, base_pid=PID, update_pid=PID, icon=None, base_exefs=None,
                 update_icon=None, update_exefs=None):
    """icon is the base game's, update_icon the update's (N1-30: the update's is the one staged)."""
    base, _ = make_ncch(False, b"BASEROMFS" * 100, base_pid, icon, base_exefs)
    upd, code = make_ncch(compressed, b"UPDROMFS" * 50, update_pid, update_icon, update_exefs)
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


def relabel(update, path, version):
    """The update .cia with another title version in its TMD (as a .cia rebuilt on a PC says)."""
    data = bytearray(open(update, "rb").read())
    pad = lambda n: (n + 63) & ~63  # noqa: E731
    struct.pack_into(">H", data, pad(0x2020) + pad(0xA00) + pad(0x350) + 4 + 0x100 + 0x3C + 0x9C, version)
    path.write_bytes(bytes(data))
    return str(path)


def test_a_version_number_that_lies_is_judged_by_the_code(tmp_path):
    base, update, code = write_inputs(tmp_path, compressed=False)
    relabelled = relabel(update, tmp_path / "v100.cia", 1024)      # v1.0.0, as a decrypted .cia says
    entry = {"name": "Synthetic v1.3.0 (test)", "target": True, "program_id": f"{PID:016X}",
             "update_title_id": f"{tx.PROGRAM_ID:016X}", "title_version": 1072}
    right = tmp_path / "right.json"
    right.write_text(json.dumps({"versions": [dict(entry, code_sha1=hashlib.sha1(code).hexdigest())]}))
    pg.prepare(base, relabelled, str(tmp_path / "game"), str(right))
    assert open(tmp_path / "game" / "code.bin", "rb").read() == code
    wrong = tmp_path / "wrong.json"
    wrong.write_text(json.dumps({"versions": [dict(entry, code_sha1="0" * 40)]}))
    with pytest.raises(pg.PrepareError, match="v1.0.0"):
        pg.prepare(base, relabelled, str(tmp_path / "game2"), str(wrong))


def test_the_version_is_the_one_whose_update_it_is(tmp_path):
    """Europe's v1.1.0 (an entry with shifts) is chosen by its update's title, not the target."""
    base, update, _ = write_inputs(tmp_path)
    versions = tmp_path / "versions.json"
    versions.write_text(json.dumps({"versions": [
        {"name": "Synthetic USA v1.3.0 (test)", "target": True, "program_id": "0004000000AAAA00",
         "update_title_id": "0004000E00AAAA00", "title_version": 1072},
        {"name": "Synthetic EUR v1.1.0 (test)", "shifts": "config/shifts/x.txt", "program_id": f"{PID:016X}",
         "update_title_id": f"{tx.PROGRAM_ID:016X}", "title_version": 1072},
        {"name": "a base game", "program_id": f"{PID:016X}", "title_version": 0}]}))
    pg.prepare(base, update, str(tmp_path / "game"), str(versions))
    with pytest.raises(pg.PrepareError, match="expected 0004000E00AAAA00 or"):
        pg.prepare(base, base, str(tmp_path / "game2"), str(versions))


def test_update_must_be_a_cia_or_its_installed_content(tmp_path, versions):
    base, _, _ = write_inputs(tmp_path)
    # the base game's own NCCH is not the update's content (its title is the program's)
    with pytest.raises(pg.PrepareError, match="is title 0004000000ABCD00, expected"):
        pg.prepare(base, base, str(tmp_path / "game"), versions)
    ncsd = tmp_path / "game.3ds"
    ncsd.write_bytes(tx.make_ncsd(tx.make_ncch()))
    with pytest.raises(pg.PrepareError, match="not a CIA or an installed update"):
        pg.prepare(base, str(ncsd), str(tmp_path / "game"), versions)


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


# Region lockout words (bit n = region n: JPN, USA, EUR, AUS, CHN, KOR, TWN) the tests cover.
LOCKOUTS = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x7F, 0x06, 0x00, 0x80000041]


@pytest.mark.parametrize("lockout", LOCKOUTS)
def test_icon_is_written_as_is(tmp_path, versions, lockout):
    icon = make_icon(lockout)
    base, update, _ = write_inputs(tmp_path, update_icon=icon)
    out = tmp_path / "game"
    pg.prepare(base, update, str(out), versions)
    assert (out / "icon.bin").read_bytes() == icon
    assert struct.unpack_from("<I", (out / "icon.bin").read_bytes(), 0x2018)[0] == lockout
    assert sorted(os.listdir(out)) == ["code.bin", "exheader.bin", "icon.bin", "info.txt",
                                       "romfs.bin", "update_romfs.bin"]
    text = (out / "info.txt").read_text()
    assert f"icon.bin      : 0x36C0 bytes, CRC-32 {zlib.crc32(icon):08X}" in text
    assert "icon          : none" not in text
    assert "icon source   : update" in text.splitlines()


def test_no_icon_is_not_an_error(tmp_path, versions, capsys):
    base, update, _ = write_inputs(tmp_path)
    out = tmp_path / "game"
    pg.prepare(base, update, str(out), versions)
    assert not (out / "icon.bin").exists()
    text = (out / "info.txt").read_text()
    assert "icon          : none" in text and "icon source   : none" in text.splitlines()
    assert "update has no ExeFS icon" in capsys.readouterr().err


def test_stale_icon_of_an_earlier_run_is_removed(tmp_path, versions):
    out = tmp_path / "game"
    base, update, _ = write_inputs(tmp_path, update_icon=make_icon(0x02))
    pg.prepare(base, update, str(out), versions)
    assert (out / "icon.bin").exists()
    base, update, _ = write_inputs(tmp_path)
    pg.prepare(base, update, str(out), versions)
    assert not (out / "icon.bin").exists()


def test_icon_comes_from_the_update_not_the_base(tmp_path, versions):
    base_icon, upd_icon = make_icon(0x04), make_icon(0x02)
    base, update, _ = write_inputs(tmp_path, icon=base_icon, update_icon=upd_icon)
    pg.prepare(base, update, str(tmp_path / "game"), versions)
    got = (tmp_path / "game" / "icon.bin").read_bytes()
    assert got == upd_icon and got != base_icon
    assert "icon source   : update" in (tmp_path / "game" / "info.txt").read_text().splitlines()


def test_update_without_an_icon_does_not_fall_back_to_the_base_game(tmp_path, versions, capsys):
    # Azahar's ReadIcon reads the update's ExeFS and gets ErrorNotUsed: it does not try the base
    base, update, _ = write_inputs(tmp_path, icon=make_icon(0x04))
    out = tmp_path / "game"
    pg.prepare(base, update, str(out), versions)
    assert not (out / "icon.bin").exists()
    assert "icon source   : none" in (out / "info.txt").read_text().splitlines()
    assert "Azahar" in capsys.readouterr().err


def test_without_an_update_the_base_games_icon_is_used(tmp_path):
    base_icon = make_icon(0x04)
    for name, icon, want in (("with", base_icon, (base_icon, "base game")), ("without", None, (None, "none"))):
        d = tmp_path / name
        d.mkdir()
        (d / "exefs.bin").write_bytes(exefs_with_icon(code_image(), icon) if icon else fake_exefs(code_image()))
        assert pg.choose_icon(str(d), None) == want


def test_choose_icon_prefers_the_update_and_checks_only_it(tmp_path):
    for name, icon in (("base", make_icon(0x04)), ("upd", make_icon(0x02))):
        (tmp_path / name).mkdir()
        (tmp_path / name / "exefs.bin").write_bytes(exefs_with_icon(code_image(), icon))
    assert pg.choose_icon(str(tmp_path / "base"), str(tmp_path / "upd")) == (make_icon(0x02), "update")
    # a base game whose icon is damaged does not matter: its icon is never read
    (tmp_path / "base" / "exefs.bin").write_bytes(b"\0" * 0x10)
    assert pg.choose_icon(str(tmp_path / "base"), str(tmp_path / "upd"))[1] == "update"
    with pytest.raises(pg.PrepareError, match="base game's ExeFS is damaged"):
        pg.choose_icon(str(tmp_path / "base"), None)


def test_icon_through_the_command_line(tmp_path, versions, capsys):
    base, update, _ = write_inputs(tmp_path, update_icon=make_icon(0x02))
    assert pg.main(["--base", base, "--update", update, "-o", str(tmp_path / "g"),
                    "--versions", versions]) == 0
    assert "icon.bin" in capsys.readouterr().out


def damaged_exefs(kind: str) -> bytes:
    good = bytearray(exefs_with_icon(code_image(), make_icon(0x02)))
    if kind == "icon_past_end":
        struct.pack_into("<I", good, 16 + 12, 0x100000)     # icon size far beyond the ExeFS
    elif kind == "code_past_end":
        struct.pack_into("<I", good, 8, 0x100000)
    elif kind == "icon_size0_past_end":
        # no data, so extract_ncch lets it through: only prepare_game's own check catches it,
        # after the work folder exists
        struct.pack_into("<II", good, 16 + 8, 0x100000, 0)
    elif kind == "later_entry_past_end":
        struct.pack_into("<8sII", good, 32, b"logo", 0x100000, 0x10)
    return bytes(good)


@pytest.mark.parametrize("kind", ["icon_past_end", "code_past_end", "later_entry_past_end",
                                  "icon_size0_past_end"])
def test_damaged_exefs_header_is_an_error(tmp_path, versions, kind):
    # the update's ExeFS is the one whose icon is read (N1-30); the base game's is damaged the same
    # way in test_damaged_base_exefs_is_still_refused
    base, update, _ = write_inputs(tmp_path, compressed=False, update_exefs=damaged_exefs(kind))
    with pytest.raises(pg.PrepareError, match="update.*(runs past|damaged)"):
        pg.prepare(base, update, str(tmp_path / "game"), versions)
    assert pg.main(["--base", base, "--update", update, "-o", str(tmp_path / "g"),
                    "--versions", versions]) == 1
    assert not (tmp_path / "g" / ".prepare-tmp").exists()


@pytest.mark.parametrize("kind", ["icon_past_end", "code_past_end", "later_entry_past_end"])
def test_damaged_base_exefs_is_still_refused(tmp_path, versions, kind):
    base, update, _ = write_inputs(tmp_path, base_exefs=damaged_exefs(kind))
    with pytest.raises(pg.PrepareError, match="base game.*(runs past|damaged)"):
        pg.prepare(base, update, str(tmp_path / "game"), versions)


def test_exefs_entry_rejects_a_short_exefs():
    with pytest.raises(pg.PrepareError, match="damaged"):
        pg.exefs_entry(b"\0" * 0x100, "icon")


def test_truncated_icon_is_an_error(tmp_path, versions):
    base, update, _ = write_inputs(tmp_path, update_icon=make_icon(0x02)[:0x1000])
    with pytest.raises(pg.PrepareError, match="update's icon is damaged"):
        pg.prepare(base, update, str(tmp_path / "game"), versions)
    # the base game's icon is not read when there is an update: its damage is no error
    base, update, _ = write_inputs(tmp_path, icon=make_icon(0x02)[:0x1000], update_icon=make_icon(0x02))
    pg.prepare(base, update, str(tmp_path / "game2"), versions)


def test_icon_without_magic_is_a_warning_not_an_error(tmp_path, versions, capsys):
    icon = b"XXXX" + make_icon(0x02)[4:]
    base, update, _ = write_inputs(tmp_path, update_icon=icon)
    out = tmp_path / "game"
    info = pg.prepare(base, update, str(out), versions)
    assert (out / "icon.bin").read_bytes() == icon
    assert "no SMDH magic" in capsys.readouterr().err
    text = (out / "info.txt").read_text()
    line = [ln for ln in text.splitlines() if ln.startswith("icon.bin")][0]
    assert f"CRC-32 {zlib.crc32(icon):08X}" in line and "WARNING" in line and "no SMDH magic" in line
    assert any("WARNING" in ln for ln in info)


def test_good_icon_has_no_warning(tmp_path, versions, capsys):
    base, update, _ = write_inputs(tmp_path, update_icon=make_icon(0x02))
    pg.prepare(base, update, str(tmp_path / "game"), versions)
    assert "warning" not in capsys.readouterr().err
    assert "WARNING" not in (tmp_path / "game" / "info.txt").read_text()


def test_exefs_entry_first_icon_wins():
    first, second = b"FIRST" * 4, b"SECOND" * 4
    hdr = bytearray(0x200)
    struct.pack_into("<8sII", hdr, 0, b"icon", 0, len(first))
    struct.pack_into("<8sII", hdr, 16, b"icon", 0x200, len(second))
    exefs = bytes(hdr) + first.ljust(0x200, b"\0") + second.ljust(0x200, b"\0")
    assert pg.exefs_entry(exefs, "icon") == first


def test_exefs_entry_rejects_an_entry_past_the_end():
    hdr = bytearray(0x200)
    struct.pack_into("<8sII", hdr, 0, b"icon", 0, 0x1000)
    with pytest.raises(pg.PrepareError, match="runs past the end"):
        pg.exefs_entry(bytes(hdr) + b"\0" * 0x200, "icon")


def _exefs(entries, body=b""):
    hdr = bytearray(0x200)
    for i, (name, off, size) in enumerate(entries):
        struct.pack_into("<8sII", hdr, i * 16, name, off, size)
    return bytes(hdr) + body


def test_unused_entry_with_a_bogus_offset_is_accepted():
    icon = b"ICON" * 8
    exefs = _exefs([(b"", 0x100000, 0), (b"icon", 0, len(icon))], icon)
    assert pg.exefs_entry(exefs, "icon") == icon


def test_size_zero_icon_is_no_icon_and_only_it_is_checked_for_a_bogus_offset():
    assert pg.exefs_entry(_exefs([(b"icon", 0, 0)]), "icon") is None
    assert pg.exefs_entry(_exefs([(b"logo", 0x100000, 0)]), "icon") is None
    with pytest.raises(pg.PrepareError, match="runs past the end"):
        pg.exefs_entry(_exefs([(b"icon", 0x100000, 0)]), "icon")


def test_first_icon_with_size_zero_wins_over_a_later_one_with_data():
    later = b"LATER" * 4
    exefs = _exefs([(b"icon", 0, 0), (b"icon", 0, len(later))], later)
    assert pg.exefs_entry(exefs, "icon") is None


def test_entry_name_ends_at_the_first_nul():
    icon = b"ICON" * 8
    exefs = _exefs([(b"icon\0xyz", 0, len(icon))], icon)
    assert pg.exefs_entry(exefs, "icon") == icon


# ── the installed update (an NCCH under the virtual SD card) and info.txt's input lines ──────────

def make_tmd(title_id: int, version: int) -> bytes:
    th = 4 + 0x100 + 0x3C
    tmd = bytearray(th + 0xC4 + 0x24 * 64)
    struct.pack_into(">I", tmd, 0, 0x10004)
    struct.pack_into(">Q", tmd, th + 0x4C, title_id)
    struct.pack_into(">H", tmd, th + 0x9C, version)
    return bytes(tmd)


def write_installed(tmp_path, tmd=None, title_id=tx.PROGRAM_ID):
    """The update as Azahar's setup leaves it: <content>/00000000.app (the NCCH of the title
    `title_id`), with a TMD beside it when `tmd` (title, version) is given."""
    code = code_image()
    exh = make_exheader(program_id=PID, compressed=True, text=(0x100000, 2, len(TEXT)),
                        ro=(0x200000, 1, len(RO)), data=(0x300000, 1, len(DATA)))
    stored = blz_encoder.compress(code_image())
    upd = make_cxi.make_cxi(exh, fake_exefs(stored), b"UPDROMFS" * 50, program_id=title_id)
    content = tmp_path / "content"
    content.mkdir(exist_ok=True)
    (content / "00000000.app").write_bytes(upd)
    if tmd:
        (content / "00000000.tmd").write_bytes(make_tmd(*tmd))
    return str(content / "00000000.app"), code


def test_installed_content_is_an_update(tmp_path, versions):
    base, _, _ = write_inputs(tmp_path)
    app, code = write_installed(tmp_path, tmd=(tx.PROGRAM_ID, 1072))
    info = pg.prepare(base, app, str(tmp_path / "game"), versions)
    assert (tmp_path / "game" / "code.bin").read_bytes() == code
    assert (tmp_path / "game" / "update_romfs.bin").read_bytes().startswith(b"UPDROMFS")
    assert any("v1.3.0" in line for line in info if line.startswith("update "))


def test_installed_content_without_a_tmd_is_checked_by_its_title(tmp_path, versions):
    base, _, _ = write_inputs(tmp_path)
    app, _ = write_installed(tmp_path)
    info = pg.prepare(base, app, str(tmp_path / "game"), versions)
    assert any("version not in it" in line for line in info)
    (tmp_path / "x").mkdir()
    other, _ = write_installed(tmp_path / "x", title_id=PID)       # the base game's own NCCH
    with pytest.raises(pg.PrepareError, match="is title 0004000000ABCD00, expected 0004000000ABC000"):
        pg.prepare(base, other, str(tmp_path / "game2"), versions)


def test_installed_content_with_another_version_is_refused(tmp_path, versions):
    base, _, _ = write_inputs(tmp_path)
    app, _ = write_installed(tmp_path, tmd=(tx.PROGRAM_ID, (1 << 10) | (2 << 4)))
    with pytest.raises(pg.PrepareError, match="v1.2.0"):
        pg.prepare(base, app, str(tmp_path / "game"), versions)


def test_a_tmd_of_another_title_beside_the_content_is_ignored(tmp_path, versions):
    base, _, _ = write_inputs(tmp_path)
    app, _ = write_installed(tmp_path, tmd=(PID, 5))
    info = pg.prepare(base, app, str(tmp_path / "game"), versions)
    assert any("version not in it" in line for line in info)


def test_info_names_the_inputs(tmp_path, versions):
    base, update, _ = write_inputs(tmp_path)
    out = tmp_path / "game"
    info = pg.prepare(base, update, str(out), versions)
    want = [f"input base  : {base}, 0x{os.path.getsize(base):X} bytes",
            f"input update: {update}, 0x{os.path.getsize(update):X} bytes"]
    assert info[-2:] == want
    assert (out / "info.txt").read_text().splitlines()[-2:] == want
