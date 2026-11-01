"""tools/build_game.py: which versions it builds and their addresses (no compiler needed)."""
import json
import os
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
    import hashlib
    versions.write_text(json.dumps({"versions": [{"name": "X", "code_sha1": hashlib.sha1(code).hexdigest()}]}))
    assert build_game.check_version(None, code, str(versions))["name"] == "X"
    assert build_game.check_version(None, b"other", str(versions)) is None


def test_every_hook_and_tunable_has_an_address_in_each_version():
    """A new hook or tunable in config/ needs the shifts of every other version to cover it."""
    with open(os.path.join(ROOT, "config", "versions.json")) as f:
        versions = [v for v in json.load(f)["versions"] if v.get("shifts")]
    assert versions
    hooks = recompile.load_hooks(os.path.join(ROOT, "config", "hooks.txt"))
    tunables = recompile.load_tunables(os.path.join(ROOT, "config", "tunables.txt"))
    for v in versions:
        shifts = vm.Shifts.load(os.path.join(ROOT, v["shifts"]))
        assert shifts.translate_keys(hooks)[1] == [], v["name"]
        assert shifts.translate_keys(tunables)[1] == [], v["name"]
