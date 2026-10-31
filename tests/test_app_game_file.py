"""port/app/launcher/game_file.cpp: what the launcher reads of the game file before the game starts
(launcher/checks.cpp, CheckGame), on synthetic dumps (never game data): a .cxi, or a .3ds/.cci with
the game in partition 0, decrypted or not by its NCCH's flags, and whether its RomFS starts with the
IVFC header a decrypted one starts with. A file whose flags say decrypted but whose RomFS doesn't
start that way was decrypted wrongly or is damaged (the game stops with a fatal error of its own as
it opens its files); one that ends before its RomFS does is incomplete. Only the headers and four
bytes are read, never the RomFS's hashes."""
from __future__ import annotations

import io
import os
import random
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import extract_ncch  # noqa: E402
import make_cxi  # noqa: E402
import verify_ncch  # noqa: E402

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

MH4U = 0x0004000000126300
MEDIA_UNIT = 0x200
# the headers (an NCSD's and its game's NCCH) and the RomFS's first four bytes
MOST_READ = 2 * 0x200 + 4

DRIVER = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "launcher/game_file.h"

// FILE [FAIL_FROM]: the verdict, then how many bytes were read; a read that reaches FAIL_FROM fails
int main(int argc, char** argv) {
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::uint64_t>(file.tellg());
    const std::uint64_t fail_from = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : UINT64_MAX;
    std::uint64_t bytes = 0;
    const auto verdict = Launcher::GameFile::Check(
        [&](std::uint64_t offset, void* out, std::size_t n) {
            if (offset + n > fail_from) {
                return false;
            }
            bytes += n;
            file.clear();
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
            return file.gcount() == static_cast<std::streamsize>(n);
        },
        size);
    static const char* const names[] = {"unknown", "ok", "encrypted", "bad_romfs", "truncated"};
    std::printf("%s\t%llu\n", names[static_cast<int>(verdict)], static_cast<unsigned long long>(bytes));
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("game_file")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "launcher", "game_file.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def check(driver, path, fail_from=None):
    args = [str(driver), str(path)] + ([hex(fail_from)] if fail_from is not None else [])
    verdict, read = subprocess.run(args, capture_output=True, text=True, check=True).stdout.split()
    return verdict, int(read)


def romfs(level3=b"synthetic level 3".ljust(0x1000, b"\0")) -> bytes:
    """A RomFS as a decrypted one starts: its IVFC header (magic, version 0x10000, the master hash's
    size), padded to 0x1000, where Azahar takes level 3 (the file system) to begin."""
    header = bytearray(0x1000)
    header[:4] = b"IVFC"
    struct.pack_into("<II", header, 4, 0x10000, 0x20)
    return bytes(header) + level3


def garbled() -> bytes:
    """What a RomFS still encrypted, or decrypted with the wrong keys, looks like."""
    return random.Random(46).randbytes(len(romfs()))


def cxi(romfs_data: bytes | None = None, program_id=MH4U) -> bytearray:
    """A decrypted NCCH (.cxi) from tools/make_cxi.py: header, exheader, ExeFS, then the RomFS
    (none for b"": make_cxi marks it NoMountRomFs)."""
    exheader = bytearray(0x800)
    struct.pack_into("<Q", exheader, 0x200, program_id)
    exefs = bytearray(0x200)
    struct.pack_into("<8sII", exefs, 0, b".code", 0, 0x200)
    exefs += bytes(range(256)) * 2
    return bytearray(make_cxi.make_cxi(bytes(exheader), bytes(exefs), romfs() if romfs_data is None else romfs_data))


def ncsd(game: bytes, manual: bytes = b"", padding: int = 0) -> bytearray:
    """A cartridge image (.3ds/.cci): the NCSD header and the card's info up to 0x4000, the game's
    NCCH in partition 0, the manual's in partition 1, then the 0xFF padding of an untrimmed dump."""
    image = bytearray(0x4000)
    image[0x100:0x104] = b"NCSD"
    struct.pack_into("<Q", image, 0x108, MH4U)
    offset = len(image)
    for i, part in enumerate((game, manual)):
        if part:
            struct.pack_into("<II", image, 0x120 + i * 8, offset // MEDIA_UNIT, len(part) // MEDIA_UNIT)
            offset += len(part)
    struct.pack_into("<I", image, 0x104, (offset + padding) // MEDIA_UNIT)
    return image + game + manual + b"\xff" * padding


def write(tmp_path, name, data):
    path = tmp_path / name
    path.write_bytes(bytes(data))
    return path


def romfs_offset(data) -> int:
    """Where the repository's Python tools find the game's RomFS in a dump (verify_ncch.locate,
    extract_ncch.parse_ncch): the C++ check must look at the same place."""
    f = io.BytesIO(bytes(data))
    base = verify_ncch.locate(f)
    return base + extract_ncch.parse_ncch(f, base).romfs_offset


def test_decrypted_dumps_pass(driver, tmp_path):
    game = cxi()
    manual = cxi(garbled(), program_id=MH4U)       # partition 1 isn't looked at
    for name, data in (("game.cxi", game),
                       ("game.trim.3ds", ncsd(game, manual)),         # trimmed: ends with its last partition
                       ("game.cci", ncsd(game, manual, padding=0x20000))):
        assert data[romfs_offset(data):][:4] == b"IVFC", name
        verdict, read = check(driver, write(tmp_path, name, data))
        assert verdict == "ok", name
        # the headers and four bytes, however large the file
        assert read <= MOST_READ, name


def test_damage_past_the_romfs_start_isnt_looked_for(driver, tmp_path):
    """No hashes are checked: a dump with a few bad blocks in its RomFS still plays."""
    data = ncsd(cxi())
    start = romfs_offset(data)
    data[start + 0x1000 + 5] ^= 0xFF
    data[start + 4] ^= 0xFF                         # even the IVFC header's version
    assert check(driver, write(tmp_path, "game.3ds", data))[0] == "ok"


def test_wrongly_decrypted_or_damaged(driver, tmp_path):
    # the flags say decrypted, but the RomFS is what an encrypted one looks like
    game = cxi(garbled())
    for name, data in (("game.cxi", game), ("game.3ds", ncsd(game)), ("game.cci", ncsd(game, cxi(), 0x8000))):
        assert data[romfs_offset(data):][:4] != b"IVFC", name
        verdict, read = check(driver, write(tmp_path, name, data))
        assert verdict == "bad_romfs", name
        assert read <= MOST_READ, name
    # a damaged first sector of the RomFS
    data = ncsd(cxi())
    start = romfs_offset(data)
    data[start:start + MEDIA_UNIT] = bytes(MEDIA_UNIT)
    assert check(driver, write(tmp_path, "zeros.3ds", data))[0] == "bad_romfs"


def test_encrypted(driver, tmp_path):
    game = cxi(garbled())
    game[0x18F] &= ~0x04 & 0xFF                     # flags[7] without NoCrypto
    for name, data in (("game.cxi", game), ("game.3ds", ncsd(game))):
        assert check(driver, write(tmp_path, name, data))[0] == "encrypted", name


def test_incomplete_files(driver, tmp_path):
    full = ncsd(cxi(), cxi())
    start = romfs_offset(full)
    end = start + len(romfs())
    for cut in (end - 1, start + 0x800, start + 2, start - MEDIA_UNIT, 0x4000 + 0x100):
        assert check(driver, write(tmp_path, "cut.3ds", full[:cut]))[0] == "truncated", hex(cut)
    # cut right after the game's RomFS (the manual's partition gone): all of the game is there
    assert check(driver, write(tmp_path, "nomanual.3ds", full[:end]))[0] == "ok"
    game = cxi()
    assert check(driver, write(tmp_path, "cut.cxi", game[:-MEDIA_UNIT]))[0] == "truncated"


def test_nothing_to_say(driver, tmp_path):
    other_unit = cxi(garbled())
    other_unit[0x18E] = 1                           # flags[6]: media units of 0x400 bytes
    prototype = cxi(garbled())
    struct.pack_into("<I", prototype, 0x104, len(prototype))   # its content size in bytes
    cases = {
        "no RomFS": cxi(b""),                       # the runner's .cxi without one (port/README.md)
        "another media unit": other_unit,
        "a prototype": prototype,
        "not a 3DS file": bytes(0x10000),
        "a CIA": struct.pack("<I", 0x2020).ljust(0x4000, b"\0"),
        "too short": b"NCSD",
        "an empty file": b"",
        "partition 0 empty": ncsd(b"")[:0x4000] + cxi(garbled()),
    }
    for name, data in cases.items():
        assert check(driver, write(tmp_path, "case.bin", data))[0] == "unknown", name


def test_a_read_that_fails_says_nothing(driver, tmp_path):
    """A file that can't be read (rather than one too short) keeps the message of the checks
    before this one (CheckGame): this check says nothing."""
    data = ncsd(cxi(garbled()))
    path = write(tmp_path, "game.3ds", data)
    assert check(driver, path)[0] == "bad_romfs"
    for fail_from in (romfs_offset(data), 0x4000, 0):
        assert check(driver, path, fail_from)[0] == "unknown", hex(fail_from)
