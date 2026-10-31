"""The setup's check of the v1.3.0 update (issue #47). Some tools that decrypt or rebuild a .cia
reset the version label of its title metadata (to v0.0.1 or v1.0.0: two bytes at the title
metadata's offset + 0x1DC) and leave the content as it was. port/app/launcher/update_code.cpp
then judges the update by its code, the SHA-1 of the decompressed ExeFS .code that
tools/build_game.py looks up in config/versions.json: before the app's launcher installs a .cia,
and when it finds an installed update with such a label. build_game.py says so when it builds
from such a .cia.

Synthetic CIAs only: the "v1.3.0 code" here is a made-up program whose SHA-1 the test driver
takes as v1.3.0's. The C++ part runs in a small driver built with the C++ compiler found (as
tests/test_app_speed.py builds speed.cpp), the way launcher/checks.cpp uses it."""
from __future__ import annotations

import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
from typing import NamedTuple

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "port", "app")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import blz  # noqa: E402
import extract_ncch  # noqa: E402
import make_cxi  # noqa: E402
from blz_encoder import compress  # noqa: E402
from test_extract_ncch import make_cia  # noqa: E402
from test_make_cxi import fake_exefs  # noqa: E402

UPDATE_ID = 0x0004000E00126300
V130 = (1 << 10) | (3 << 4)                 # 1072
V120 = (1 << 10) | (2 << 4)
RESET = {1: "v0.0.1", 1 << 10: "v1.0.0"}    # the labels such tools were seen to write
CODE_SIZE = 0x3400                          # text (2 pages), read-only data (1 page), data (0x400)

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
needs_cxx = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "launcher/update_code.h"

using namespace Launcher::UpdateCode;

namespace {

std::vector<std::uint8_t> Bytes(const char* path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

const char* Name(Verdict verdict) {
    switch (verdict) {
    case Verdict::Target:
        return "target";
    case Verdict::Relabeled:
        return "relabeled";
    case Verdict::OtherVersion:
        return "other";
    case Verdict::Unchecked:
        return "unchecked";
    }
    return "?";
}

} // namespace

// constants | version N | sha1 FILE... | blz IN OUT | code CIA | cia CIA TARGET_SHA1 |
// installed PROGRAM LABEL TARGET_SHA1: the answer on one line (sha1: a line for each file),
// tab-separated (cia: the verdict, the title id, whether the code was read, the refusal's title
// and text, the note; installed: the verdict, whether the code was read, ok or not, the note)
int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "constants") {
        std::printf("%u\t%s\n", static_cast<unsigned>(TARGET_VERSION), TARGET_CODE_SHA1);
        return 0;
    }
    if (command == "version" && argc == 3) {
        std::printf("%s\n", VersionText(static_cast<std::uint16_t>(std::atoi(argv[2]))).c_str());
        return 0;
    }
    if (command == "sha1") {
        for (int i = 2; i < argc; ++i) {
            const auto data = Bytes(argv[i]);
            std::printf("%s\n", Sha1(data.data(), data.size()).c_str());
        }
        return 0;
    }
    if (command == "blz" && argc == 4) {
        std::vector<std::uint8_t> out;
        if (!Decompress(Bytes(argv[2]), out)) {
            std::printf("fail\n");
            return 1;
        }
        std::ofstream(argv[3], std::ios::binary)
            .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        std::printf("ok\n");
        return 0;
    }
    if (argc < 3) {
        return 2;
    }
    std::ifstream file(argv[2], std::ios::binary);
    const Read read = [&file](std::uint64_t offset, void* out, std::size_t size) {
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
        return file.gcount() == static_cast<std::streamsize>(size);
    };
    if (command == "code" && argc == 3) {
        const Code code = ReadCiaCode(read);
        std::printf("%s\t%s\n", code.ok ? code.sha1.c_str() : "-", code.error.c_str());
        return 0;
    }
    bool code_read = false;
    if (command == "cia" && argc == 4) {
        // as launcher/checks.cpp's CheckUpdateCia calls it (the title metadata read here, where
        // it asks Azahar)
        const Cia cia = ReadCia(read);
        if (!cia.ok) {
            std::printf("unreadable\t\t%s\n", cia.error.c_str());
            return 0;
        }
        const CiaCheck check = CheckCia(
            cia.version,
            [&] {
                code_read = true;
                return ReadCiaCode(read);
            },
            argv[3]);
        std::printf("%s\t%016llX\t%s\t%s\t%s\t%s\n", Name(check.verdict),
                    static_cast<unsigned long long>(cia.title_id), code_read ? "read" : "unread",
                    check.title.c_str(), check.problem.c_str(), check.note.c_str());
        return 0;
    }
    if (command == "installed" && argc == 5) {
        // as CheckUpdate calls it, LABEL being the installed title metadata's
        const auto label = static_cast<std::uint16_t>(std::atoi(argv[3]));
        const InstalledCheck check = CheckInstalled(
            label,
            [&] {
                code_read = true;
                return ReadCode(read, 0);
            },
            argv[4]);
        std::printf("%s\t%s\t%s\t%s\n", Name(check.verdict), code_read ? "read" : "unread",
                    check.ok ? "ok" : "not ok", check.note.c_str());
        return 0;
    }
    return 2;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("update_code")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-I", APP, str(src), os.path.join(APP, "launcher", "update_code.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def run(driver, *args) -> list[str]:
    out = subprocess.run([str(driver), *map(str, args)], capture_output=True, text=True, check=True).stdout
    return out.rstrip("\n").split("\t")


def write(path, data: bytes):
    path.write_bytes(data)
    return path


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def program(seed: int) -> bytes:
    """A made-up program's code, as the exheader below lays it out (compressible like a real
    one, and ending in zeros, so that it doesn't end in what looks like a compression footer)."""
    rng = random.Random(seed)
    words = [bytes(rng.randrange(256) for _ in range(rng.randrange(2, 9))) for _ in range(24)]
    code = b"".join(rng.choice(words) for _ in range(3400))
    assert len(code) >= CODE_SIZE
    return code[:CODE_SIZE - 8] + bytes(8)


def update_exheader(compressed: bool) -> bytes:
    buf = bytearray(0x800)
    buf[0:8] = b"SYNTH\0\0\0"
    buf[0x0D] = 1 if compressed else 0
    struct.pack_into("<III", buf, 0x10, 0x00100000, 2, 0x1800)      # text
    struct.pack_into("<III", buf, 0x20, 0x00102000, 1, 0x0800)      # read-only data
    struct.pack_into("<III", buf, 0x30, 0x00103000, 1, 0x0400)      # data
    struct.pack_into("<Q", buf, 0x200, UPDATE_ID)
    return bytes(buf)


def update_program(code: bytes, compressed: bool = True, stored: bytes | None = None) -> bytes:
    """The update's program (an NCCH), its .code compressed (the last 0x1000 bytes of it, which
    is quick to make) or `stored` as it is, with the exheader's flag `compressed`."""
    if stored is None:
        stored = compress(code, raw_prefix=CODE_SIZE - 0x1000) if compressed else code
    exefs = fake_exefs({".code": stored, "icon": b"I" * 0x40, "banner": b"B" * 0x40})
    return make_cxi.make_cxi(update_exheader(compressed), exefs, program_id=UPDATE_ID)


def update_cia(code: bytes, label: int, encrypted: bool = False, **program_args) -> bytes:
    return make_cia(update_program(code, **program_args), encrypted=encrypted, title_id=UPDATE_ID, version=label)


class CiaCheck(NamedTuple):
    """What the setup does with a .cia (UpdateCode::CheckCia, as CheckUpdateCia uses it)."""
    verdict: str
    code: str = "read"   # whether its code was read
    title: str = ""      # a refusal's title
    problem: str = ""    # a refusal's text ("": installed)
    note: str = ""       # said once it is installed


def check_cia(driver, cia, target_sha1: str) -> CiaCheck:
    verdict, title_id, *rest = run(driver, "cia", cia, target_sha1)
    assert title_id == f"{UPDATE_ID:016X}"
    return CiaCheck(verdict, *rest)


class InstalledCheck(NamedTuple):
    """What the setup says of the installed update (UpdateCode::CheckInstalled, as CheckUpdate
    uses it): ok is Checks::Update::State::Ok, else OtherVersion."""
    verdict: str
    code: str
    ok: bool
    note: str = ""


def check_installed(driver, program, label: int, target_sha1: str) -> InstalledCheck:
    verdict, code, ok, note = run(driver, "installed", program, label, target_sha1)
    return InstalledCheck(verdict, code, ok == "ok", note)


# ── the C++ check (port/app/launcher/update_code.cpp) ──

@needs_cxx
def test_the_target_is_config_versions_json(driver):
    with open(os.path.join(ROOT, "config", "versions.json")) as f:
        target = next(v for v in json.load(f)["versions"] if v.get("target"))
    assert run(driver, "constants") == [str(target["title_version"]), target["code_sha1"]]
    assert [run(driver, "version", v) for v in (V130, V120, *RESET)] == [["1.3.0"], ["1.2.0"], ["0.0.1"],
                                                                          ["1.0.0"]]


@needs_cxx
def test_sha1_matches_hashlib(driver, tmp_path):
    rng = random.Random(7)
    sizes = (0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 1000, 0x12345)
    data = [bytes(rng.randrange(256) for _ in range(size)) for size in sizes]
    files = [write(tmp_path / f"data{size}", d) for size, d in zip(sizes, data)]
    out = subprocess.run([str(driver), "sha1", *map(str, files)], capture_output=True, text=True, check=True).stdout
    assert out.splitlines() == [sha1(d) for d in data]


@needs_cxx
@pytest.mark.parametrize("seed", range(4))
@pytest.mark.parametrize("raw_prefix", [0, 0x40])
def test_decompression_matches_blz_py(driver, tmp_path, seed, raw_prefix):
    rng = random.Random(seed)
    words = [bytes(rng.randrange(256) for _ in range(rng.randrange(2, 9))) for _ in range(24)]
    data = b"".join(rng.choice(words) for _ in range(rng.randrange(200, 900)))
    packed = write(tmp_path / "packed", compress(data, raw_prefix=raw_prefix))
    assert run(driver, "blz", packed, tmp_path / "out") == ["ok"]
    assert (tmp_path / "out").read_bytes() == blz.decompress(packed.read_bytes()) == data


@needs_cxx
def test_decompression_of_a_hand_built_stream(driver, tmp_path):
    # (tests/test_blz.py's) literals Z, Y, X, then an 18-byte copy from 3 bytes on: "XYZ" * 7
    region = bytes([0x00, 0xF0]) + b"XYZ" + bytes([0x10])
    packed = b"RAW!" + region + struct.pack("<II", (8 << 24) | (len(region) + 8), 21 - (len(region) + 8))
    assert run(driver, "blz", write(tmp_path / "packed", packed), tmp_path / "out") == ["ok"]
    assert (tmp_path / "out").read_bytes() == b"RAW!" + b"XYZ" * 7


@needs_cxx
def test_decompression_refuses_what_blz_py_refuses(driver, tmp_path):
    # too short; a footer of zeros; a copy from past the end of the output
    past_the_end = bytes([0xFF, 0x0F, 0x80, 0xFF]) + struct.pack("<II", (9 << 24) | 12, 16)
    for bad in (b"short", bytes(32), past_the_end):
        with pytest.raises(blz.BLZError):
            blz.decompress(bad)
        r = subprocess.run([str(driver), "blz", str(write(tmp_path / "bad", bad)), str(tmp_path / "out")],
                           capture_output=True, text=True)
        assert (r.returncode, r.stdout) == (1, "fail\n")


@needs_cxx
@pytest.mark.parametrize("kind", ["end compressed", "all compressed", "not compressed", "decompressed already"])
def test_the_code_is_read_as_build_game_reads_it(driver, tmp_path, kind):
    """The code's SHA-1 is the one build_game.py checks: extract_ncch.py's .code, decompressed
    by codebin2elf.load_code (also when the exheader says compressed and the tool that made the
    file had decompressed it)."""
    import codebin2elf
    import exheader as exh
    code = program(3)
    args = {"end compressed": {}, "all compressed": {"stored": compress(code)},
            "not compressed": {"compressed": False}, "decompressed already": {"stored": code}}[kind]
    cia = write(tmp_path / "update.cia", update_cia(code, 1, **args))
    extract_ncch.extract(str(cia), str(tmp_path / "dump"))
    header = exh.parse((tmp_path / "dump" / "exheader.bin").read_bytes())
    assert codebin2elf.load_code(header, (tmp_path / "dump" / "code.bin").read_bytes()) == code
    assert run(driver, "code", cia) == [sha1(code), ""]


@needs_cxx
def test_label_v130_with_v130_code_is_installed(driver, tmp_path):
    code = program(1)
    cia = write(tmp_path / "update.cia", update_cia(code, V130))
    assert check_cia(driver, cia, sha1(code)) == CiaCheck("target", code="unread")


@needs_cxx
@pytest.mark.parametrize("label", sorted(RESET))
def test_reset_label_with_v130_code_is_installed_with_a_note(driver, tmp_path, label):
    code = program(1)
    cia = write(tmp_path / "update.cia", update_cia(code, label))
    assert check_cia(driver, cia, sha1(code)) == CiaCheck("relabeled", note=(
        f"The .cia's version label says {RESET[label]}, but its code is v1.3.0's (some decryption tools reset "
        "the label), so it's used as v1.3.0."))


@needs_cxx
@pytest.mark.parametrize("label", [V120, *sorted(RESET)])
def test_another_version_is_refused(driver, tmp_path, label):
    """Another label on other code: refused, naming the version by its label, which some tools
    reset (no "update v0.0.1" is said to be there)."""
    cia = write(tmp_path / "update.cia", update_cia(program(2), label))
    found = {V120: "v1.2.0", **RESET}[label]
    assert check_cia(driver, cia, sha1(program(1))) == CiaCheck("other", title="This isn't the update", problem=(
        f"The .cia's version label says {found}, and its code isn't v1.3.0's either: it holds another update, "
        "or a changed one. The port is made for v1.3.0: its code library is recompiled from that update's code."))


@needs_cxx
def test_label_v130_with_other_code_is_installed_as_before(driver, tmp_path):
    """A v1.3.0 label is taken as it is, as before this check: its code isn't read, so the files
    that were accepted are still accepted whatever the check would make of them. build_game.py
    refuses other code when the library is built (test_build_game_judges_the_code below)."""
    cia = write(tmp_path / "update.cia", update_cia(program(2), V130))
    assert check_cia(driver, cia, sha1(program(1))) == CiaCheck("target", code="unread")


@needs_cxx
def test_another_label_whose_code_cant_be_read_is_refused_with_the_label_case(driver, tmp_path):
    """Refused under a title that says it couldn't be checked (not that it isn't the update), with
    what to do."""
    code = program(1)

    def unchecked(why):
        return CiaCheck("unchecked", title="This .cia couldn't be checked", problem=(
            "The .cia's version label says v0.0.1; the port is made for v1.3.0. Some decryption tools reset that "
            f"label on a v1.3.0 update, but this file's code couldn't be read to tell ({why}). Build the .cia "
            "again with GodMode9 from the update installed on your 3DS."))

    # flagged compressed, and neither compressed nor as long as the segments
    cia = write(tmp_path / "a.cia", update_cia(code, 1, stored=bytes(0x1000)))
    assert check_cia(driver, cia, sha1(code)) == unchecked("its program's code can't be decompressed")
    # the program encrypted with the title key
    cia = write(tmp_path / "b.cia", update_cia(code, 1, encrypted=True))
    assert check_cia(driver, cia, sha1(code)) == unchecked("its program is encrypted")
    # cut short in the program's ExeFS
    cia = write(tmp_path / "c.cia", update_cia(code, 1)[:-0x300])
    assert check_cia(driver, cia, sha1(code)) == unchecked("its program's ExeFS can't be read")


@needs_cxx
def test_the_installed_update_is_judged_by_its_code(driver, tmp_path):
    """CheckUpdate: the installed title metadata's label, and the installed program's code when
    that label isn't v1.3.0's (Azahar's installer copies the .cia's title metadata as it is)."""
    code = program(1)
    app = write(tmp_path / "00000000.app", update_program(code))
    v130 = sha1(code)
    assert check_installed(driver, app, V130, v130) == InstalledCheck("target", "unread", True)
    assert check_installed(driver, app, 1, v130) == InstalledCheck("relabeled", "read", True, (
        "Installed in Azahar's virtual SD card; its version label says v0.0.1, but its code is v1.3.0's (some "
        "decryption tools reset the label)"))
    for label, found in ((V120, "v1.2.0"), (1, "v0.0.1")):
        assert check_installed(driver, app, label, sha1(program(2))) == InstalledCheck("other", "read", False, (
            f"The installed update's version label says {found}, and its code isn't v1.3.0's either; the port is "
            "made for v1.3.0"))
    broken = write(tmp_path / "broken.app", update_program(code)[:0x400])
    assert check_installed(driver, broken, 1, v130) == InstalledCheck("unchecked", "read", False, (
        "The installed update's version label says v0.0.1; the port is made for v1.3.0 (some decryption tools "
        "reset that label on a v1.3.0 update, but the installed code couldn't be read to tell: its program's "
        "extended header can't be read)"))
    assert check_installed(driver, broken, V130, v130) == InstalledCheck("target", "unread", True)


# ── tools/build_game.py ──

def test_label_of_a_cia(tmp_path):
    cia = write(tmp_path / "update.cia", update_cia(program(1), 1))
    assert extract_ncch.cia_title_version(str(cia)) == 1
    assert extract_ncch.cia_title_version(str(write(tmp_path / "update.cxi", update_program(program(1))))) is None
    assert extract_ncch.version_text(V130) == "1.3.0"


def test_build_game_judges_the_code(tmp_path, monkeypatch, capsys):
    """A v1.3.0 code with a reset label is built from, with a line that says so; other code is
    refused whatever its label. (Stopped where the recompiling starts.)"""
    import build_game
    code = program(1)
    versions = write(tmp_path / "versions.json", json.dumps({"versions": [
        {"name": "synthetic v1.3.0", "target": True, "title_version": V130, "code_sha1": sha1(code)}]}).encode())

    class Recompiling(Exception):
        pass

    def recompiling(*args, **kwargs):
        raise Recompiling

    monkeypatch.setattr(build_game.recompile, "code_size", recompiling)
    args = ["--out", str(tmp_path / "out"), "--versions", str(versions), "--no-mods"]
    note = ("  the .cia's version label says v0.0.1, but its code is v1.3.0's (some decryption tools reset the "
            "label): it is used as v1.3.0\n")
    for label, says in ((1, True), (V130, False)):
        cia = write(tmp_path / f"update-{label}.cia", update_cia(code, label))
        with pytest.raises(Recompiling):
            build_game.main(["--dump", str(cia), *args])
        out = capsys.readouterr().out
        assert "version: synthetic v1.3.0\n" in out
        assert (note in out) is says
    # the installed program the launcher builds from has no label of its own
    assert build_game.label_note(str(write(tmp_path / "00000000.app", update_program(code))),
                                 {"title_version": V130}) is None
    for label in (1, V130):
        other = write(tmp_path / f"other-{label}.cia", update_cia(program(2), label))
        with pytest.raises(SystemExit, match="isn't a version the project knows"):
            build_game.main(["--dump", str(other), *args])
