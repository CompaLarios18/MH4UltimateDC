# Derived from Azahar src/video_core/custom_textures/custom_tex_manager.cpp at
# 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; the file says "Licensed under GPLv2
# or any later version"): the expected results restate its rules for the file names of a pack, for
# pack.json and for the missing pack.json.
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (custom_tex_manager.cpp)
"""tools/native/hd_hash_check.py on synthetic hash lists and synthetic pack folders (N2-14)."""
import json
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
ROOT = os.path.abspath(os.path.join(HERE, ".."))
TOOL = os.path.join(ROOT, "tools", "native", "hd_hash_check.py")
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))

import hd_hash_check as hhc  # noqa: E402

# Four captured textures; the hashes are made up (they are not hashes of anything).
HASHES = """\
# mh4n tex-hashes 1
# use_new_hash=1
AAAAAAAAAAAAAAA1 64x64 12
00000000000000B2 8x8 0
CCCCCCCCCCCCCCC3 128x32 5
DDDDDDDDDDDDDDD4 16x16 3
# use_new_hash=0
1111111111111111 64x64 12
2222222222222222 8x8 0
3333333333333333 128x32 5
4444444444444444 16x16 3
"""


def touch(root, rel, text=""):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def run(tmp_path, pack, *extra, hashes=HASHES):
    hashes_file = tmp_path / "hashes.txt"
    hashes_file.write_text(hashes)
    return subprocess.run([sys.executable, TOOL, str(hashes_file), str(pack), *extra],
                          capture_output=True, text=True)


def pack_json(use_new_hash=True, skip_mipmap=False, textures=None):
    return json.dumps({"author": "x", "version": "1.0.0", "description": "d",
                       "options": {"skip_mipmap": skip_mipmap, "flip_png_files": True,
                                   "use_new_hash": use_new_hash},
                       "textures": textures or {}})


def summary(result):
    return dict(line.split(": ", 1) for line in result.stdout.splitlines() if ": " in line)


def test_parse_hashes_reads_both_sections():
    sections = hhc.parse_hashes(HASHES)
    assert sorted(sections) == [0, 1]
    assert sections[1][0] == hhc.Captured(0xAAAAAAAAAAAAAAA1, 64, 64, 12)
    assert sections[1][1] == hhc.Captured(0xB2, 8, 8, 0)
    assert sections[0][3] == hhc.Captured(0x4444444444444444, 16, 16, 3)
    assert len(sections[1]) == len(sections[0]) == 4


@pytest.mark.parametrize("text, message", [
    ("", "no '# use_new_hash=' section"),
    ("# use_new_hash=1\nnot a line\n", "line 2"),
    ("AAAA 8x8 0\n", "before any"),
    ("# use_new_hash=1\nAAAA 8x8\n", "line 2"),
])
def test_parse_hashes_refuses_what_it_cannot_read(text, message):
    with pytest.raises(hhc.ToolError, match=message):
        hhc.parse_hashes(text)


def test_pack_with_new_hash_counts_textures_that_have_a_file(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", pack_json(use_new_hash=True))
    touch(pack, "tex1_64x64_AAAAAAAAAAAAAAA1_12_mip0.png")      # found
    touch(pack, "sub/dir/tex1_8x8_B2_0.png")                     # found, in a folder, short hash
    touch(pack, "tex1_16x16_1111111111111111_3.png")             # an old-mode hash: not for this mode
    touch(pack, "readme.txt")
    result = run(tmp_path, pack)
    assert result.returncode == 0, result.stderr
    out = summary(result)
    assert out["hash mode"].startswith("use_new_hash=1 (pack.json)")
    assert out["captured textures"] == "4"
    assert out["with a file in the pack"] == "2"
    assert out["without"] == "2"


def test_pack_without_pack_json_uses_the_old_hash(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "tex1_64x64_1111111111111111_12.png")           # an old-mode hash
    touch(pack, "tex1_8x8_00000000000000B2_0.png")              # a new-mode hash
    result = run(tmp_path, pack)
    out = summary(result)
    assert out["hash mode"].startswith("use_new_hash=0 (no pack.json")
    assert out["with a file in the pack"] == "1"
    assert out["without"] == "3"
    # --mode overrides the choice
    forced = summary(run(tmp_path, pack, "--mode", "new"))
    assert forced["hash mode"].startswith("use_new_hash=1 (--mode")
    assert forced["with a file in the pack"] == "1"


def test_pack_json_may_ask_for_the_old_hash(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", pack_json(use_new_hash=False))
    touch(pack, "tex1_8x8_2222222222222222_0.png")
    out = summary(run(tmp_path, pack))
    assert out["hash mode"].startswith("use_new_hash=0 (pack.json)")
    assert out["with a file in the pack"] == "1"


def test_file_name_rules(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", pack_json(use_new_hash=True, skip_mipmap=False))
    touch(pack, "tex1_64x64_AAAAAAAAAAAAAAA1_12.png")            # counts
    touch(pack, "tex1_8x8_00000000000000B2_0.norm.png")          # a normal map: still a file for it
    touch(pack, "tex1_128x32_CCCCCCCCCCCCCCC3_5.dds")            # dds is fine when mipmaps are used
    touch(pack, "tex1_16x16_DDDDDDDDDDDDDDD4_3.a.b.c.png")       # more than three parts: ignored
    touch(pack, "tex1_16x16_DDDDDDDDDDDDDDD4_3.jpg")             # not an image type Azahar loads
    out = summary(run(tmp_path, pack))
    assert out["with a file in the pack"] == "3"
    assert out["without"] == "1"


def test_dds_is_ignored_when_the_pack_skips_mipmaps(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", pack_json(use_new_hash=True, skip_mipmap=True))
    touch(pack, "tex1_128x32_CCCCCCCCCCCCCCC3_5.dds")
    assert summary(run(tmp_path, pack))["with a file in the pack"] == "0"
    # and without a pack.json, which also skips mipmaps
    other = tmp_path / "other"
    touch(other, "tex1_128x32_3333333333333333_5.dds")
    assert summary(run(tmp_path, other))["with a file in the pack"] == "0"


def test_pack_json_textures_map_a_hash_to_files(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", pack_json(use_new_hash=True, textures={
        "AAAAAAAAAAAAAAA1": "hd/menu.png",                        # one file
        "CCCCCCCCCCCCCCC3": ["hd/a.png", "hd\\b.png"],            # a list, a Windows path
        "not hex": "hd/c.png",                                    # an invalid key: skipped
        "DDDDDDDDDDDDDDD4": "hd/missing.png",                     # a file that is not there
    }))
    touch(pack, "hd/menu.png")
    touch(pack, "hd/a.png")
    touch(pack, "elsewhere/b.png")                                # matched by its own name only
    result = run(tmp_path, pack, "--list")
    assert result.returncode == 0, result.stderr
    out = summary(result)
    assert out["with a file in the pack"] == "2"
    assert "found   AAAAAAAAAAAAAAA1 64x64 12 hd/menu.png" in result.stdout
    assert "found   CCCCCCCCCCCCCCC3 128x32 5 elsewhere/b.png, hd/a.png" in result.stdout \
        or "found   CCCCCCCCCCCCCCC3 128x32 5 hd/a.png, elsewhere/b.png" in result.stdout
    assert "missing DDDDDDDDDDDDDDD4 16x16 3" in result.stdout
    assert "missing 00000000000000B2 8x8 0" in result.stdout


def test_a_file_named_like_a_texture_and_listed_in_pack_json_answers_to_both(tmp_path):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", pack_json(use_new_hash=True, textures={"00000000000000B2": "tex1_64x64_AAAAAAAAAAAAAAA1_12.png"}))
    touch(pack, "tex1_64x64_AAAAAAAAAAAAAAA1_12.png")
    assert summary(run(tmp_path, pack))["with a file in the pack"] == "2"


@pytest.mark.parametrize("config", [
    "not json",
    json.dumps({"textures": {}}),
    json.dumps({"options": {"skip_mipmap": False, "flip_png_files": True}}),
    json.dumps({"options": {"skip_mipmap": False, "flip_png_files": True, "use_new_hash": "yes"}}),
])
def test_an_unusable_pack_json_is_an_error(tmp_path, config):
    pack = tmp_path / "pack"
    touch(pack, "pack.json", config)
    result = run(tmp_path, pack)
    assert result.returncode == 2
    assert "pack.json" in result.stderr


def test_missing_inputs_and_a_hashes_file_without_the_needed_section(tmp_path):
    assert run(tmp_path, tmp_path / "no-such-folder").returncode == 2
    pack = tmp_path / "pack"
    pack.mkdir()
    only_new = "# use_new_hash=1\nAAAAAAAAAAAAAAA1 64x64 12\n"
    result = run(tmp_path, pack, hashes=only_new)            # no pack.json -> old mode -> no section
    assert result.returncode == 2
    assert "use_new_hash=0" in result.stderr
    missing = subprocess.run([sys.executable, TOOL, str(tmp_path / "none.txt"), str(pack)],
                             capture_output=True, text=True)
    assert missing.returncode == 2


def test_the_report_of_an_empty_pack(tmp_path):
    pack = tmp_path / "pack"
    pack.mkdir()
    out = summary(run(tmp_path, pack))
    assert out["with a file in the pack"] == "0"
    assert out["without"] == "4"
