"""tools/build_game.py end to end on a synthetic dump (no game data)."""
import os
import shutil
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

pytestmark = pytest.mark.skipif(shutil.which("clang") is None, reason="clang not installed")


def fake_exefs(code: bytes) -> bytes:
    header = bytearray(0x200)
    struct.pack_into("<8sII", header, 0, b".code", 0, len(code))
    return bytes(header) + code + bytes(-len(code) % 0x200)


def test_build_game_synthetic(tmp_path):
    import build_game
    import make_cxi
    # ARM code: mov r0, #42; bx lr / add r0, r0, r1; bx lr
    text = struct.pack("<4I", 0xE3A0002A, 0xE12FFF1E, 0xE0800001, 0xE12FFF1E).ljust(0x1000, b"\0")
    rodata, data = bytes(0x1000), bytes(0x1000)
    exheader = bytearray(0x800)
    exheader[0:8] = b"SYNTH\0\0\0"
    exheader[0x0D] = 0                                         # .code not compressed
    struct.pack_into("<III", exheader, 0x10, 0x00100000, 1, len(text))
    struct.pack_into("<III", exheader, 0x20, 0x00101000, 1, len(rodata))
    struct.pack_into("<III", exheader, 0x30, 0x00102000, 1, len(data))
    struct.pack_into("<Q", exheader, 0x200, 0x0004000000ABCD00)
    cxi = tmp_path / "synthetic.cxi"
    cxi.write_bytes(make_cxi.make_cxi(bytes(exheader), fake_exefs(text + rodata + data)))
    functions = tmp_path / "functions.tsv"
    functions.write_text("address\tsize\tmode\tkind\tname\tstatus\towner\tnotes\n"
                         "0x00100000\t0x8\tarm\tunknown\t\ttodo\t\t\n0x00100008\t0x8\tarm\tunknown\t\ttodo\t\t\n")
    install = tmp_path / "azahar-user"
    rc = build_game.main(["--dump", str(cxi), "--out", str(tmp_path / "out"), "--opt", "O0", "--any-version",
                          "--functions", str(functions), "--extra", "", "--hooks", "", "--install", str(install),
                          "-j", "2"])
    assert rc == 0
    name = build_game.library_name(0x0004000000ABCD00)
    lib = install / "recomp" / name
    assert lib.exists()
    if shutil.which("llvm-nm"):
        syms = subprocess.run(["llvm-nm", "-D", "--defined-only", str(lib)], capture_output=True, text=True).stdout
        assert "rc_code_init" in syms
    table = (tmp_path / "out" / "recomp" / "src" / "table.c").read_text()
    assert "rc_title_id = 0x0004000000abcd00ull" in table and "f_00100008" in table


def test_fetch_compiler_from_zip(tmp_path):
    """--fetch-compiler unpacks an llvm-mingw-style zip and finds its clang (here a stand-in
    script that runs the host clang), and reuses it next time without the zip."""
    import zipfile
    import build_game
    exe = ".exe" if sys.platform.startswith("win") else ""
    archive = tmp_path / "llvm-mingw-test.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr(f"llvm-mingw-test/bin/{build_game.MINGW_CLANG}{exe}", '#!/bin/sh\nexec clang "$@"\n')
        z.writestr("llvm-mingw-test/include/readme.txt", "headers")
    toolchain = tmp_path / "toolchain"
    cc = build_game.fetch_compiler(str(toolchain), str(archive))
    assert cc == str(toolchain / "llvm-mingw-test" / "bin" / (build_game.MINGW_CLANG + exe))
    assert os.access(cc, os.X_OK)
    archive.unlink()
    assert build_game.find_compiler(None, str(toolchain), False) in (cc, "clang", "cc")
    assert build_game.fetch_compiler(str(toolchain), str(archive)) == cc
    if not sys.platform.startswith("win"):
        out = subprocess.run([cc, "--version"], capture_output=True, text=True)
        assert out.returncode == 0 and "clang" in out.stdout


def test_default_user_dir(monkeypatch):
    import build_game
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setenv("XDG_DATA_HOME", "/data")
    assert build_game.default_user_dir() == os.path.join("/data", "azahar-emu")
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setenv("APPDATA", "C:/Users/x/AppData/Roaming")
    assert build_game.default_user_dir() == os.path.join("C:/Users/x/AppData/Roaming", "Azahar")
