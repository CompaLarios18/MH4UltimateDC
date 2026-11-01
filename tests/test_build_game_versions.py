"""tools/build_game.py and tools/recompile.py: which versions they build and their addresses (no
compiler needed)."""
import hashlib
import json
import os
import re
import struct
import sys

import pytest

HERE = os.path.dirname(__file__)
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_game  # noqa: E402
import recompile  # noqa: E402
import version_map as vm  # noqa: E402

BASE = 0x00100000


def shifts_file(tmp_path):
    path = tmp_path / "shifts.txt"
    path.write_text(vm.Shifts([(BASE, BASE + 0x100, 0), (BASE + 0x200, BASE + 0x400, 0x20)],
                              (BASE, BASE + 0x400)).dumps())
    return str(path)


def test_buildable_versions():
    assert build_game.buildable({"target": True})
    assert build_game.buildable({"shifts": "config/shifts/x.txt"})
    assert not build_game.buildable({"name": "a base game"})


def test_target_tables_pass_through(tmp_path):
    entries, hooks, tunables = {BASE: "arm"}, {BASE + 0x210: "h"}, {BASE + 0x220: (400, "t")}
    assert build_game.translate_tables({"target": True, "shifts": shifts_file(tmp_path)}, entries, hooks, tunables) \
        == (entries, hooks, tunables)


def test_tables_move_to_the_other_version(tmp_path, capsys):
    version = {"shifts": shifts_file(tmp_path)}
    entries = {BASE: "arm", BASE + 0x150: "thumb", BASE + 0x200: "arm"}
    out = build_game.translate_tables(version, entries, {BASE + 0x210: "h"}, {BASE + 0x220: (400, "t")})
    assert out == ({BASE: "arm", BASE + 0x220: "arm"}, {BASE + 0x230: "h"}, {BASE + 0x240: (400, "t")})
    assert "(1 in code this version changed" in capsys.readouterr().out


def test_a_hook_without_an_address_stops_the_build(tmp_path):
    with pytest.raises(SystemExit, match="hook 0x00100150"):
        build_game.translate_tables({"shifts": shifts_file(tmp_path)}, {}, {BASE + 0x150: "h"}, {})


def test_check_version_reads_the_given_file(tmp_path):
    versions = tmp_path / "versions.json"
    code = b"synthetic code"
    versions.write_text(json.dumps({"versions": [{"name": "X", "code_sha1": hashlib.sha1(code).hexdigest()}]}))
    assert build_game.check_version(None, code, str(versions))["name"] == "X"
    assert build_game.check_version(None, b"other", str(versions)) is None


def known_versions():
    with open(os.path.join(ROOT, "config", "versions.json")) as f:
        return json.load(f)["versions"]


def test_every_hook_and_tunable_has_an_address_in_each_version():
    """A new hook or tunable in config/ needs the shifts of every other version to cover it."""
    versions = [v for v in known_versions() if v.get("shifts")]
    assert versions
    hooks = recompile.load_hooks(os.path.join(ROOT, "config", "hooks.txt"))
    tunables = recompile.load_tunables(os.path.join(ROOT, "config", "tunables.txt"))
    for v in versions:
        shifts = vm.Shifts.load(os.path.join(ROOT, v["shifts"]))
        assert shifts.translate_keys(hooks)[1] == [], v["name"]
        assert shifts.translate_keys(tunables)[1] == [], v["name"]


def text_range(version):
    base, size = (int(x, 16) for x in version["segments"]["text"])
    return base, base + size


def test_each_shift_file_is_for_its_version():
    """A version's shifts (config/shifts/) go from the target's code to that version's: the two
    codes' hashes in the header, the target's .text as the range, intervals inside it in order,
    and the last one ends at the target's .text end, which it moves to the version's. Outside
    .text addresses stay the same, so the rest of the layout must be the target's."""
    versions = known_versions()
    target = next(v for v in versions if v.get("target"))
    shifted = [v for v in versions if v.get("shifts")]
    assert shifted
    for v in shifted:
        path = os.path.join(ROOT, v["shifts"])
        shifts = vm.Shifts.load(path)
        assert re.findall(r"code sha1 ([0-9a-f]{40})", " ".join(shifts.header)) \
            == [target["code_sha1"], v["code_sha1"]], v["name"]
        assert shifts.text == text_range(target), v["name"]
        with open(path, encoding="utf-8") as f:
            listed = [tuple(int(x, 16) for x in line.split()) for line in f if line.startswith("0x")]
        assert listed == shifts.intervals, v["name"]     # in order in the file (Shifts sorts them)
        end = shifts.text[0]
        for start, stop, _ in shifts.intervals:
            assert end <= start < stop <= shifts.text[1], (v["name"], hex(start))
            end = stop
        _, stop, shift = shifts.intervals[-1]
        assert stop == text_range(target)[1] and stop + shift == text_range(v)[1], v["name"]
        assert text_range(v)[0] == text_range(target)[0], v["name"]
        for segment in ("rodata", "data", "bss"):
            assert v["segments"][segment] == target["segments"][segment], (v["name"], segment)


def test_recompile_refuses_only_the_versions_with_shifts():
    for v in known_versions():
        assert recompile.shifted_version(v["code_sha1"]) == (v if v.get("shifts") else None), v["name"]
    assert recompile.shifted_version("0" * 40) is None


def synthetic_dump(tmp_path):
    """recompile.py's input: an exheader and its uncompressed code (mov r0, #42; bx lr in a page
    of .text). Returns the code's SHA-1, the versions file to write and the command line."""
    code = struct.pack("<2I", 0xE3A0002A, 0xE12FFF1E).ljust(0x1000, b"\0") + bytes(0x2000)
    exheader = bytearray(0x400)
    struct.pack_into("<III", exheader, 0x10, BASE, 1, 8)
    struct.pack_into("<III", exheader, 0x20, BASE + 0x1000, 1, 0x1000)
    struct.pack_into("<III", exheader, 0x30, BASE + 0x2000, 1, 0x1000)
    (tmp_path / "exheader.bin").write_bytes(bytes(exheader))
    (tmp_path / "code.bin").write_bytes(code)
    versions = tmp_path / "versions.json"
    args = ["--exheader", str(tmp_path / "exheader.bin"), "--code", str(tmp_path / "code.bin"),
            "--versions", str(versions), "-o", str(tmp_path / "out")]
    return hashlib.sha1(code).hexdigest(), versions, args


def test_recompile_sends_a_version_with_shifts_to_build_game(tmp_path, monkeypatch):
    """Recompiled with config/'s tables as they are, a version whose functions moved would get
    every hook point's name at the wrong address, which the port binds without a word."""
    sha1, versions, args = synthetic_dump(tmp_path)
    versions.write_text(json.dumps({"versions": [
        {"name": "EUR v1.1.0", "code_sha1": sha1, "shifts": "config/shifts/eur_v110.txt"}]}))
    monkeypatch.setattr(recompile, "recompile", lambda *a, **k: pytest.fail("recompiled"))
    with pytest.raises(SystemExit, match=r"this code is EUR v1\.1\.0: .* tools/build_game\.py .*"
                                         r"\(config/shifts/eur_v110\.txt\)"):
        recompile.main(args)
    assert not (tmp_path / "out").exists()


def test_recompile_takes_the_target_and_unknown_code(tmp_path, monkeypatch):
    sha1, versions, args = synthetic_dump(tmp_path)
    calls = []
    monkeypatch.setattr(recompile, "recompile", lambda img, entries, out, **k: calls.append(out) or {"report": {}})
    for entry in [{"name": "USA v1.3.0", "target": True, "code_sha1": sha1},
                  {"name": "EUR v1.1.0", "code_sha1": "0" * 40, "shifts": "config/shifts/eur_v110.txt"}]:
        versions.write_text(json.dumps({"versions": [entry]}))
        assert recompile.main(args) == 0
    assert calls == [str(tmp_path / "out")] * 2
