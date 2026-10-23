"""tools/native/saves.py on synthetic files only: made-up bytes, never a player's save. Includes
non-ASCII user folders (D45)."""
import json
import os
import struct
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import saves  # noqa: E402

FMT = "4096,3,7,1"
FMT_BYTES = struct.pack("<IIIB3x", 4096, 3, 7, 1)
SAVE_FILES = {"system": b"S" * 40, "user1": bytes(range(256)) * 9}
EXT_FILES = {"card1": b"c1" * 30, "card2": b"", "quest1": os.urandom(300)}


def make_folder(path, files):
    os.makedirs(path, exist_ok=True)
    for name, data in files.items():
        with open(os.path.join(path, name), "wb") as f:
            f.write(data)
    return str(path)


def snapshot(folder):
    return {n: open(os.path.join(folder, n), "rb").read() for n in sorted(os.listdir(folder))}


def run(capsys, *args):
    code = saves.main(list(args))
    out = capsys.readouterr()
    return code, out.out, out.err


def jrun(capsys, *args):
    code, out, _ = run(capsys, *args, "--json")
    return code, json.loads(out)


@pytest.fixture(params=["user", "usér ünï カード"])
def user(request, tmp_path):
    p = tmp_path / request.param
    p.mkdir()
    return str(p)


def test_paths_are_azahars(user):
    z = "0" * 32
    assert saves.save_dir(user) == os.path.join(user, "sdmc", "Nintendo 3DS", z, z, "title", "00040000",
                                                "00126300", "data", "00000001")
    assert saves.save_metadata(user).endswith(os.path.join("data", "00000001.metadata"))
    assert saves.extdata_root(user, 0xABCDEF).endswith(os.path.join("extdata", "00000000", "00ABCDEF"))


def test_import_into_empty_writes_metadata(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    code, r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 0 and r["ok"] and r["metadata"] == "written" and r["backup"] is None
    assert snapshot(saves.save_dir(user)) == SAVE_FILES
    assert open(saves.save_metadata(user), "rb").read() == FMT_BYTES
    assert not os.path.exists(os.path.join(user, saves.BACKUPS))
    assert not [n for n in os.listdir(saves.save_dir(user)) if n.endswith(saves.TMP_SUFFIX)]


def test_import_without_format_info_fails_clearly_and_touches_nothing(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    saved = saves.FORMAT_INFO["save"]
    saves.FORMAT_INFO["save"] = None
    try:
        code, r = jrun(capsys, "import-save", src, "--user", user)
    finally:
        saves.FORMAT_INFO["save"] = saved
    assert code == 1 and r["error"]["code"] == "no_format_info" and "--format-info" in r["error"]["message"]
    assert not os.path.exists(os.path.join(user, "sdmc"))


def test_import_into_full_backs_up_first_and_keeps_metadata(user, tmp_path, capsys):
    old = {"system": b"old-system", "user1": b"old-user1", "extra": b"gone"}
    make_folder(saves.save_dir(user), old)
    meta = struct.pack("<IIIB3x", 1, 2, 3, 0)
    open(saves.save_metadata(user), "wb").write(meta)
    src = make_folder(tmp_path / "src", SAVE_FILES)
    code, r = jrun(capsys, "import-save", src, "--user", user)
    assert code == 0 and r["metadata"] == "kept" and r["removed"] == ["extra"]
    assert snapshot(saves.save_dir(user)) == SAVE_FILES
    assert open(saves.save_metadata(user), "rb").read() == meta
    backup = r["backup"]
    assert os.path.dirname(backup) == os.path.join(user, saves.BACKUPS)
    rel = os.path.relpath(saves.save_dir(user), user)
    assert snapshot(os.path.join(backup, rel)) == old
    assert open(os.path.join(backup, rel) + ".metadata", "rb").read() == meta


def test_two_imports_in_the_same_second_make_two_backups(user, tmp_path, monkeypatch, capsys):
    import datetime
    monkeypatch.setattr(saves, "_utcnow", lambda: datetime.datetime(2026, 10, 23, 1, 2, 3,
                                                                     tzinfo=datetime.timezone.utc))
    make_folder(saves.save_dir(user), {"system": b"a"})
    src = make_folder(tmp_path / "src", SAVE_FILES)
    _, r1 = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    _, r2 = jrun(capsys, "import-save", src, "--user", user)
    assert os.path.basename(r1["backup"]) == "20261023-010203"
    assert os.path.basename(r2["backup"]) == "20261023-010203-2"
    assert len(saves.do_list(user, None)["backups"]) == 2


def test_export_then_import_is_byte_identical(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    assert jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)[0] == 0
    out = str(tmp_path / "out")
    code, r = jrun(capsys, "export-save", "--user", user, out)
    assert code == 0 and snapshot(out) == SAVE_FILES
    user2 = str(tmp_path / "user2")
    os.makedirs(user2)
    assert jrun(capsys, "import-save", out, "--user", user2, "--format-info", FMT)[0] == 0
    assert snapshot(saves.save_dir(user2)) == SAVE_FILES
    assert open(saves.save_metadata(user2), "rb").read() == open(saves.save_metadata(user), "rb").read()


def test_extdata_roundtrip_default_id_and_boss_folder(user, tmp_path, capsys):
    src = make_folder(tmp_path / "ext", EXT_FILES)
    code, r = jrun(capsys, "import-extdata", src, "--user", user, "--format-info", "0,1,2,0")
    assert code == 0 and r["extdata_id_source"] == "constant" and r["extdata_id"] == "0000000000001263"
    root = saves.extdata_root(user, 0x1263)
    assert snapshot(os.path.join(root, "user")) == EXT_FILES
    assert os.path.isdir(os.path.join(root, "boss"))
    assert open(os.path.join(root, "metadata"), "rb").read() == struct.pack("<IIIB3x", 0, 1, 2, 0)
    out = str(tmp_path / "out")
    assert jrun(capsys, "export-extdata", "--user", user, out)[0] == 0
    assert snapshot(out) == EXT_FILES


def synthetic_exheader(ext_save=0, unique=0, other=0):
    data = bytearray(0x800)
    base = 0x230
    data[base:base + 8] = ext_save.to_bytes(8, "little")
    data[base + 0x10:base + 0x18] = unique.to_bytes(8, "little")
    data[base + 0x1F] = other
    return bytes(data)


def test_extdata_id_from_exheader(user, tmp_path, capsys):
    game = os.path.join(user, "native", "game")
    os.makedirs(game)
    open(os.path.join(game, "exheader.bin"), "wb").write(synthetic_exheader(ext_save=0x00ABCDE1))
    src = make_folder(tmp_path / "ext", EXT_FILES)
    code, r = jrun(capsys, "import-extdata", src, "--user", user, "--format-info", "1,1,1,0")
    assert r["extdata_id_source"] == "exheader" and r["extdata_id"] == "0000000000abcde1"
    assert os.path.isdir(os.path.join(saves.extdata_root(user, 0xABCDE1), "user"))


def test_extdata_id_extended_access_and_option(user, tmp_path):
    path = tmp_path / "e.bin"
    path.write_bytes(synthetic_exheader(unique=(0x55555 << 40) | 7, other=2))
    assert saves.read_exheader_extdata_id(str(path)) == 0x55555
    path.write_bytes(synthetic_exheader(unique=0x7, other=2))
    assert saves.read_exheader_extdata_id(str(path)) == 7
    path.write_bytes(synthetic_exheader())
    assert saves.read_exheader_extdata_id(str(path)) == 0
    assert saves.resolve_extdata_id(user, 0x99) == (0x99, "option")
    path.write_bytes(b"short")
    with pytest.raises(saves.Error) as e:
        saves.read_exheader_extdata_id(str(path))
    assert e.value.code == "bad_exheader"


def test_extdata_id_unknown_is_an_error(user, monkeypatch):
    monkeypatch.setattr(saves, "EXTDATA_ID_DEFAULT", None)
    with pytest.raises(saves.Error) as e:
        saves.resolve_extdata_id(user, None)
    assert e.value.code == "no_extdata_id"


def test_source_with_subfolder_is_refused(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    os.makedirs(os.path.join(src, "sub"))
    code, r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 1 and r["error"]["code"] == "not_flat" and r["error"]["name"] == "sub"
    assert not os.path.exists(os.path.join(user, "sdmc"))


@pytest.mark.skipif(not hasattr(os, "symlink") or os.name == "nt", reason="symlinks")
def test_source_with_a_link_is_refused(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    os.symlink(os.path.join(src, "system"), os.path.join(src, "link"))
    code, r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 1 and r["error"]["code"] == "not_flat"


def test_empty_or_missing_source_is_refused(user, tmp_path, capsys):
    (tmp_path / "empty").mkdir()
    assert jrun(capsys, "import-save", str(tmp_path / "empty"), "--user", user, "--format-info", FMT)[0] == 1
    assert jrun(capsys, "import-save", str(tmp_path / "nope"), "--user", user, "--format-info", FMT)[0] == 1


def test_export_refuses_non_empty_dest_unless_force(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    dest = make_folder(tmp_path / "dest", {"system": b"mine", "other": b"keep"})
    code, r = jrun(capsys, "export-save", "--user", user, dest)
    assert code == 1 and r["error"]["code"] == "dest_not_empty"
    assert snapshot(dest) == {"system": b"mine", "other": b"keep"}
    code, r = jrun(capsys, "export-save", "--user", user, dest, "--force")
    assert code == 0 and snapshot(dest) == dict(SAVE_FILES, other=b"keep")


def test_export_of_nothing_and_into_a_file(user, tmp_path, capsys):
    assert jrun(capsys, "export-save", "--user", user, str(tmp_path / "o"))[1]["error"]["code"] == "nothing"
    src = make_folder(tmp_path / "src", SAVE_FILES)
    jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    f = tmp_path / "afile"
    f.write_bytes(b"x")
    assert jrun(capsys, "export-save", "--user", user, str(f))[1]["error"]["code"] == "dest"


def test_failed_write_leaves_old_file_and_no_temp(user, tmp_path, monkeypatch):
    make_folder(saves.save_dir(user), {"system": b"old"})
    target = os.path.join(saves.save_dir(user), "system")

    def boom():
        yield b"new"
        raise OSError("disk full")
    with pytest.raises(OSError):
        saves.atomic_write(target, boom())
    assert open(target, "rb").read() == b"old"
    assert os.listdir(saves.save_dir(user)) == ["system"]


def test_atomic_write_replaces_existing(tmp_path):
    p = str(tmp_path / "f")
    saves.atomic_write(p, [b"one"])
    saves.atomic_write(p, [b"two", b"three"])
    assert open(p, "rb").read() == b"twothree" and os.listdir(tmp_path) == ["f"]


def test_long_path_prefix():
    assert saves._long("C:\\a\\b", nt=True) == "C:\\a\\b"
    long = "C:\\" + "d" * 250 + "\\file"
    assert saves._long(long, nt=True) == "\\\\?\\" + long
    unc = "\\\\server\\share\\" + "d" * 250
    assert saves._long(unc, nt=True) == "\\\\?\\UNC\\" + unc[2:]
    assert saves._long("\\\\?\\C:\\x", nt=True) == "\\\\?\\C:\\x"
    assert saves._long("/a/" + "d" * 300, nt=False) == "/a/" + "d" * 300


def test_list_json_shape(user, tmp_path, capsys):
    code, r = jrun(capsys, "list", "--user", user)
    assert code == 0 and r["ok"] and r["command"] == "list" and r["program_id"] == "0004000000126300"
    assert r["save"]["files"] == [] and r["save"]["metadata"] is False and r["backups"] == []
    src = make_folder(tmp_path / "src", SAVE_FILES)
    jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    jrun(capsys, "import-save", src, "--user", user)
    r = jrun(capsys, "list", "--user", user)[1]
    assert r["save"]["files"] == [{"name": n, "size": len(d)} for n, d in sorted(SAVE_FILES.items())]
    assert r["save"]["metadata"] is True and len(r["backups"]) == 1
    assert set(r["extdata"]) >= {"path", "extdata_id", "extdata_id_source", "files", "metadata"}


def test_import_json_shape(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)[1]
    assert set(r) == {"ok", "command", "kind", "files", "bytes", "destination", "metadata", "backup", "removed"}
    assert r["command"] == "import-save" and r["bytes"] == sum(map(len, SAVE_FILES.values()))


def test_usage_errors_exit_2(user, capsys):
    code, r = jrun(capsys, "import-save", "--user", user)
    assert code == 2 and r["error"]["code"] == "usage"
    with pytest.raises(SystemExit) as e:
        saves.main(["list"])
    assert e.value.code == 2


def test_format_info_command_prints_numbers_only(tmp_path, capsys):
    f = tmp_path / "metadata"
    f.write_bytes(FMT_BYTES)
    code, out, _ = run(capsys, "format-info", str(f))
    assert code == 0 and out.strip() == "4096,3,7,1"
    code, r = jrun(capsys, "format-info", str(f))
    assert r["total_size"] == 4096 and r["number_files"] == 7 and r["duplicate_data"] == 1
    f.write_bytes(b"123")
    assert jrun(capsys, "format-info", str(f))[1]["error"]["code"] == "bad_metadata"


def test_text_output_and_bad_format_info(user, tmp_path, capsys):
    src = make_folder(tmp_path / "src", SAVE_FILES)
    code, out, _ = run(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 0 and "imported 2 file(s)" in out
    assert run(capsys, "list", "--user", user)[0] == 0
    assert run(capsys, "export-save", "--user", user, str(tmp_path / "o"))[0] == 0
    code, _, err = run(capsys, "import-save", src, "--user", user, "--format-info", "1,2")
    assert code == 2 and "four numbers" in err
