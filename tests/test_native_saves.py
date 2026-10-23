# Derived from Azahar src/core/file_sys/ncch_container.h, src/core/file_sys/archive_backend.h,
# src/core/file_sys/archive_source_sd_savedata.cpp and src/core/file_sys/archive_extsavedata.cpp at
# 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.h says "Licensed under GPLv3
# or any later version"; the others say GPLv2 or any later version): the expected results restate the
# exheader storage info layout, the ArchiveFormatInfo layout and the folder and metadata file names.
# Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.h, archive_backend.h)
# Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_source_sd_savedata.cpp)
# Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (archive_extsavedata.cpp)
"""tools/native/saves.py on synthetic files only: made-up bytes, never a player's save. Includes
non-ASCII user folders (D45)."""
import io
import json
import os
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import saves  # noqa: E402

FMT = "4096,3,7,1"
FMT_BYTES = struct.pack("<IIIB3x", 4096, 3, 7, 1)
SAVE_FILES = {"system": b"S" * 40, "user1": bytes(range(256)) * 9}
EXT_FILES = {"card1": b"c1" * 30, "card2": b"", "quest1": bytes((i * 37 + 11) % 256 for i in range(300))}


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
    out = str(tmp_path / "out")
    e = jrun(capsys, "export-save", "--user", user, out)[1]
    assert set(e) == {"ok", "command", "kind", "files", "bytes", "destination", "left"} and e["left"] == []
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


TOOLS = os.path.join(HERE, "..", "tools", "native")


def test_json_output_survives_an_ascii_stdout(user, tmp_path, monkeypatch):
    """D45: a pipe on Windows uses the ANSI code page; the JSON must not need more than ASCII."""
    src = make_folder(tmp_path / "src", SAVE_FILES)
    raw = io.BytesIO()
    monkeypatch.setattr(sys, "stdout", io.TextIOWrapper(raw, encoding="ascii"))
    code = saves.main(["import-save", src, "--user", user, "--format-info", FMT, "--json"])
    sys.stdout.flush()
    assert code == 0
    r = json.loads(raw.getvalue().decode("ascii"))
    assert r["ok"] and r["destination"] == saves.save_dir(user)


def test_text_output_survives_an_ascii_stdout(user, monkeypatch):
    raw = io.BytesIO()
    monkeypatch.setattr(sys, "stdout", io.TextIOWrapper(raw, encoding="ascii"))
    assert saves.main(["list", "--user", user]) == 0


def failing_nth_read(monkeypatch, n, under):
    """Fail the n-th read of a file under `under` (the source), not the backup's reads."""
    real, calls = saves._read_chunks, []

    def read(path):
        if path.startswith(under):
            calls.append(path)
        if len(calls) == n and path.startswith(under):
            raise OSError(13, "Permission denied", path)
        return real(path)
    monkeypatch.setattr(saves, "_read_chunks", read)


def test_failure_while_staging_leaves_the_archive_untouched(user, tmp_path, monkeypatch, capsys):
    old = {"system": b"old-system", "user1": b"old-user1", "extra": b"old-extra"}
    make_folder(saves.save_dir(user), old)
    meta = struct.pack("<IIIB3x", 1, 2, 3, 0)
    open(saves.save_metadata(user), "wb").write(meta)
    src = make_folder(tmp_path / "src", dict(SAVE_FILES, third=b"3"))
    failing_nth_read(monkeypatch, 2, src)
    code, r = jrun(capsys, "import-save", src, "--user", user)
    assert code == 1 and r["error"]["code"] == "write_failed" and r["error"]["changed"] is False
    assert r["error"]["backup"] and os.path.isdir(r["error"]["backup"])
    assert r["error"]["backup"] in r["error"]["message"] and "not changed" in r["error"]["message"]
    assert snapshot(saves.save_dir(user)) == old
    assert open(saves.save_metadata(user), "rb").read() == meta


def test_failure_while_committing_names_the_backup(user, tmp_path, monkeypatch, capsys):
    old = {"system": b"old-system", "user1": b"old-user1"}
    make_folder(saves.save_dir(user), old)
    src = make_folder(tmp_path / "src", SAVE_FILES)
    calls = []
    real = os.replace

    def replace(a, b):
        if b.startswith(saves.save_dir(user)):
            calls.append(a)                  # the commit's renames, not the backup's
        if len(calls) == 2 and b.startswith(saves.save_dir(user)):
            raise OSError(13, "Permission denied", b)
        return real(a, b)
    monkeypatch.setattr(saves.os, "replace", replace)
    code, r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 1 and r["error"]["changed"] is True and "partly imported" in r["error"]["message"]
    assert r["error"]["backup"] in r["error"]["message"]
    assert not [n for n in os.listdir(saves.save_dir(user)) if n.endswith(saves.TMP_SUFFIX)]
    rel = os.path.relpath(saves.save_dir(user), user)
    assert snapshot(os.path.join(r["error"]["backup"], rel)) == old


def test_case_only_difference_does_not_delete_the_new_file(user, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(saves, "_norm", lambda n: n.casefold())     # what Windows and macOS do
    make_folder(saves.save_dir(user), {"system": b"old", "user1": b"old1"})
    src = make_folder(tmp_path / "src", {"System": b"new", "user1": b"new1"})
    code, r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 0 and r["removed"] == []


def test_source_names_that_collide_by_case_are_refused(user, tmp_path, monkeypatch, capsys):
    probe = tmp_path / "CaseProbe"
    probe.write_bytes(b"")
    if (tmp_path / "caseprobe").exists():
        pytest.skip("this filesystem ignores case: two names that differ only by case cannot exist side by side")
    monkeypatch.setattr(saves, "_norm", lambda n: n.casefold())
    src = make_folder(tmp_path / "src", {"System": b"a", "system": b"b"})
    code, r = jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)
    assert code == 1 and r["error"]["code"] == "case_collision"
    assert not os.path.exists(os.path.join(user, "sdmc"))


def test_replace_failure_keeps_old_content_and_propagates(tmp_path, monkeypatch):
    p = str(tmp_path / "f")
    saves.atomic_write(p, [b"old"])

    def boom(a, b):
        raise OSError(13, "Permission denied", b)
    monkeypatch.setattr(saves.os, "replace", boom)
    with pytest.raises(OSError):
        saves.atomic_write(p, [b"new"])
    assert open(p, "rb").read() == b"old" and os.listdir(tmp_path) == ["f"]


def test_crash_before_the_rename_leaves_a_tmp_that_everything_ignores(user, tmp_path, capsys):
    make_folder(saves.save_dir(user), {"system": b"old", "user1": b"old1"})
    open(saves.save_metadata(user), "wb").write(FMT_BYTES)
    target = os.path.join(saves.save_dir(user), "system")
    code = ("import os, sys; sys.path.insert(0, sys.argv[1]); import saves; "
            "saves.os.replace = lambda a, b: os._exit(3); saves.atomic_write(sys.argv[2], [b'new'])")
    proc = subprocess.run([sys.executable, "-c", code, TOOLS, target])
    assert proc.returncode == 3
    assert open(target, "rb").read() == b"old"
    leftover = target + saves.TMP_SUFFIX
    assert os.path.exists(leftover)
    assert [n for n, _ in saves.list_files(saves.save_dir(user), "x")] == ["system", "user1"]
    r = jrun(capsys, "list", "--user", user)[1]
    assert [f["name"] for f in r["save"]["files"]] == ["system", "user1"]
    out = str(tmp_path / "out")
    assert jrun(capsys, "export-save", "--user", user, out)[0] == 0
    assert sorted(os.listdir(out)) == ["system", "user1"]
    src = make_folder(tmp_path / "src", SAVE_FILES)
    assert jrun(capsys, "import-save", src, "--user", user)[0] == 0
    assert snapshot(saves.save_dir(user)) == SAVE_FILES      # no leftover either


def test_stale_tmp_of_another_name_is_cleaned_by_an_import(user, tmp_path, capsys):
    make_folder(saves.save_dir(user), {"zzz" + saves.TMP_SUFFIX: b"x", "system": b"o"})
    src = make_folder(tmp_path / "src", SAVE_FILES)
    assert jrun(capsys, "import-save", src, "--user", user, "--format-info", FMT)[0] == 0
    assert sorted(os.listdir(saves.save_dir(user))) == ["system", "user1"]


def test_list_reports_an_archive_folder_with_a_subfolder(user, capsys):
    make_folder(saves.save_dir(user), SAVE_FILES)
    os.makedirs(os.path.join(saves.save_dir(user), "sub"))
    make_folder(os.path.join(saves.extdata_root(user, 0x1263), "user"), EXT_FILES)
    code, r = jrun(capsys, "list", "--user", user)
    assert code == 0 and "sub" in r["save"]["error"] and r["save"]["files"] == []
    assert len(r["extdata"]["files"]) == 3
    code, out, _ = run(capsys, "list", "--user", user)
    assert code == 0 and "sub" in out


def test_native_deleted_leftovers_are_not_archive_files(user, tmp_path, capsys):
    make_folder(saves.save_dir(user), dict(SAVE_FILES, **{"user1.deleted.123.4": b"x"}))
    assert [n for n, _ in saves.list_files(saves.save_dir(user), "x")] == ["system", "user1"]
    out = str(tmp_path / "out")
    assert jrun(capsys, "export-save", "--user", user, out)[0] == 0
    assert sorted(os.listdir(out)) == ["system", "user1"]


def test_force_export_reports_what_it_left(user, tmp_path, capsys):
    make_folder(saves.save_dir(user), SAVE_FILES)
    dest = make_folder(tmp_path / "dest", {"keep": b"k", "system": b"mine"})
    r = jrun(capsys, "export-save", "--user", user, dest, "--force")[1]
    assert r["left"] == ["keep"]


def test_short_exheader_message_names_the_option(user, tmp_path, capsys):
    game = os.path.join(user, "native", "game")
    os.makedirs(game)
    open(os.path.join(game, "exheader.bin"), "wb").write(b"short")
    src = make_folder(tmp_path / "ext", EXT_FILES)
    code, r = jrun(capsys, "import-extdata", src, "--user", user, "--format-info", "1,1,1,0")
    assert code == 1 and "--extdata-id" in r["error"]["message"]


def test_extdata_id_option_usage_error_is_readable(user, capsys):
    code, r = jrun(capsys, "list", "--user", user, "--extdata-id", "zz")
    assert code == 2 and "lambda" not in r["error"]["message"] and "not a number" in r["error"]["message"]
