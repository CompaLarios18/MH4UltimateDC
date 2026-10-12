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
                          "--functions", str(functions), "--extra", "", "--install", str(install), "-j", "2"])
    assert rc == 0
    name = build_game.library_name(0x0004000000ABCD00)
    lib = install / "recomp" / name
    assert lib.exists()
    if shutil.which("llvm-nm"):
        syms = subprocess.run(["llvm-nm", "-D", "--defined-only", str(lib)], capture_output=True, text=True).stdout
        assert "rc_code_init" in syms
    table = (tmp_path / "out" / "recomp" / "src" / "table.c").read_text()
    assert "rc_title_id = 0x0004000000abcd00ull" in table and "f_00100008" in table
