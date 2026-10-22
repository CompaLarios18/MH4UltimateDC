"""Code patches from the mods folder (N3-09): native/src/code_patch and the `mh4n_code_patch` tool.

Thirty-odd synthetic mods folders (invented bytes) are applied to a synthetic code file three ways:
  - Azahar's own patch.cpp through N3-10's harness (`layeredfs_oracle.py patch`), after the steps of
    ncch.cpp / ncch_container.cpp that the harness does not hold (a `code.bin` replacement, the bss pages
    appended, the first patch of the order exefs/code.ips, exefs/code.bps, code.ips, code.bps) written here
    from the sources;
  - `mh4n_code_patch --mods DIR --bss-pages N IN OUT`;
  - tools/code_mods.py's `apply_mods(code, user, program id, bss size)`, which is what build_game.py applies
    (N3-31, D79: it appends the pages like the loader and returns the code without them).
The native tool and Azahar's code must give the same bytes, or both an error. The native tool and code_mods.py
must agree in every case: the test compares code_mods.py's result with the native buffer without its bss
pages (what the code library is built from), or both fail.

Needs MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit), clang++ and the built `mh4n_code_patch`
(build/native/bin, or MH4N_CODE_PATCH); without them the tests skip, or fail with MH4N_REQUIRE_TEST_DATA=1.
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOLS = os.path.join(ROOT, "tools")
PROGRAM_ID = 0x0004000000126300


def _load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


oracle = _load("layeredfs_oracle", os.path.join(TOOLS, "native", "layeredfs_oracle", "layeredfs_oracle.py"))
code_mods = _load("code_mods_under_test", os.path.join(TOOLS, "code_mods.py"))

PAGE = 0x1000


def _need(what: str) -> None:
    if os.environ.get("MH4N_REQUIRE_TEST_DATA") == "1":
        pytest.fail(f"needs {what}")
    pytest.skip(f"needs {what}")


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    azahar = os.path.expanduser(os.environ.get("MH4N_AZAHAR_DIR", ""))
    if not azahar or not os.path.isdir(os.path.join(azahar, "src", "core", "file_sys")) \
            or shutil.which(os.environ.get("CXX", "clang++")) is None:
        _need("MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit) and clang++")
    return oracle.build_driver(azahar, os.environ.get("CXX", "clang++"), str(tmp_path_factory.mktemp("oracle")))


@pytest.fixture(scope="module")
def native():
    path = os.environ.get("MH4N_CODE_PATCH") or os.path.join(ROOT, "build", "native", "bin", "mh4n_code_patch")
    if not os.path.isfile(path):
        _need("the built mh4n_code_patch (build/native/bin, or MH4N_CODE_PATCH)")
    return path


# ---- the formats, written independently of every implementation ----------------------------------------

def code_bytes(size: int, seed: int) -> bytes:
    return bytes((seed + i * 7 + (i >> 5)) & 0xFF for i in range(size))


def ips(records=(), rle=(), eof: bool = True, tail: bytes = b"") -> bytes:
    out = b"PATCH"
    for offset, payload in records:
        out += offset.to_bytes(3, "big") + len(payload).to_bytes(2, "big") + payload
    for offset, count, value in rle:
        out += offset.to_bytes(3, "big") + b"\0\0" + count.to_bytes(2, "big") + bytes([value])
    return out + (b"EOF" if eof else b"") + tail


def _number(n: int) -> bytes:
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def _signed(n: int) -> bytes:
    return _number((abs(n) << 1) | (1 if n < 0 else 0))


def bps(source: bytes, target: bytes, source_crc_delta: int = 0, target_crc_delta: int = 0, commands=None) -> bytes:
    """A BPS from `commands` [(kind, length, arg)] (0 SourceRead, 1 TargetRead with bytes, 2 SourceCopy and
    3 TargetCopy with a relative offset), or by default one SourceRead/TargetRead run per stretch."""
    out = b"BPS1" + _number(len(source)) + _number(len(target)) + _number(0)
    if commands is None:
        commands, i = [], 0
        while i < len(target):
            same = i < len(source) and source[i] == target[i]
            j = i
            while j < len(target) and ((j < len(source) and source[j] == target[j]) == same):
                j += 1
            commands.append((0, j - i, None) if same else (1, j - i, target[i:j]))
            i = j
    for kind, length, arg in commands:
        out += _number(((length - 1) << 2) | kind)
        if kind == 1:
            out += arg
        elif kind in (2, 3):
            out += _signed(arg)
    out += struct.pack("<I", (zlib.crc32(source) + source_crc_delta) & 0xFFFFFFFF)
    out += struct.pack("<I", (zlib.crc32(target) + target_crc_delta) & 0xFFFFFFFF)
    return out + struct.pack("<I", zlib.crc32(out))


# ---- the cases ------------------------------------------------------------------------------------------

CODE = code_bytes(0x180, 1)
REPLACEMENT = code_bytes(0x140, 77)
ORDER = ("exefs/code.ips", "exefs/code.bps", "code.ips", "code.bps")


class Case:
    def __init__(self, name: str, files: dict[str, bytes], pages: int = 0, code: bytes = CODE):
        self.name, self.files, self.pages, self.code = name, files, pages, code


def _flip(data: bytes, at: int) -> bytes:
    out = bytearray(data)
    out[at] ^= 0x5A
    return bytes(out)


def _cases() -> list[Case]:
    c: list[Case] = []
    add = lambda *a, **k: c.append(Case(*a, **k))  # noqa: E731
    with_bss = lambda code, pages: code + bytes(pages * PAGE)  # noqa: E731
    edited = lambda base, at, data: base[:at] + data + base[at + len(data):]  # noqa: E731

    add("empty folder, no bss", {})
    add("empty folder, two bss pages", {}, pages=2)
    add("IPS records in exefs", {"exefs/code.ips": ips([(0, b"\xde\xad"), (0x20, b"hello world")])})
    add("IPS records in the folder", {"code.ips": ips([(5, b"\x01\x02\x03")])}, pages=1)
    add("IPS RLE", {"exefs/code.ips": ips(rle=[(0x10, 0x40, 0xAA)])})
    add("IPS records and RLE mixed", {"code.ips": ips([(0, b"AB"), (0x100, b"CDEF")], rle=[(0x30, 3, 0), (0x31, 1, 9)])})
    add("IPS record ends at the last byte", {"code.ips": ips([(0x17F, b"\x77")])})
    add("IPS record past the end, no bss", {"exefs/code.ips": ips([(0x17F, b"\x01\x02")])})
    add("IPS RLE past the end, no bss", {"exefs/code.ips": ips(rle=[(0x170, 0x20, 1)])})
    add("IPS record across the end into the bss", {"exefs/code.ips": ips([(0x17E, b"\x01\x02\x03\x04")])}, pages=1)
    add("IPS record in the bss", {"code.ips": ips([(0x400, b"bss data"), (0x1000, b"\x09")])}, pages=2)
    add("IPS RLE in the bss", {"code.ips": ips(rle=[(0x180, 0x200, 0xC3)])}, pages=1)
    add("IPS record past the end of the bss", {"code.ips": ips([(0x1180, b"\x01")])}, pages=1)
    add("IPS without the EOF marker", {"code.ips": ips([(8, b"\x42")], eof=False)})
    add("IPS with data after EOF", {"code.ips": ips([(8, b"\x42")], tail=b"\x00\x00\x20\x00\x02\xFF\xFF")})
    add("IPS with nothing in it", {"exefs/code.ips": ips()})
    src = CODE
    add("BPS of the code", {"exefs/code.bps": bps(src, _flip(edited(src, 0x60, b"patched!"), 0x7F))})
    add("BPS for the code, with a bss", {"code.bps": bps(src, edited(src, 4, b"\xAA\xBB"))}, pages=2)
    add("BPS that grows the code", {"exefs/code.bps": bps(src, src + b"\x11" * 0x30)}, pages=0)
    tgt = bytearray(src[:8] + src[0x40:0x50] + b"WXYZ")        # SourceRead 8, SourceCopy 16 from +0x40, TargetRead 4
    for k in range(8):                                          # TargetCopy 8 from +2 of the target (overlaps itself)
        tgt.append(tgt[2 + k])
    tgt += b"\x01" * 0x20
    add("BPS with SourceCopy and TargetCopy",
        {"code.bps": bps(src, bytes(tgt), commands=[(0, 8, None), (2, 16, 0x40), (1, 4, b"WXYZ"), (3, 8, 2),
                                                    (1, 0x20, b"\x01" * 0x20)])})
    add("BPS with a wrong source CRC", {"code.bps": bps(src, edited(src, 0, b"x"), source_crc_delta=1)})
    add("BPS with a wrong target CRC", {"exefs/code.bps": bps(src, edited(src, 0, b"x"), target_crc_delta=1)})
    add("BPS made for the code and its bss",
        {"exefs/code.bps": bps(with_bss(src, 1), edited(with_bss(src, 1), 0x1100, b"in the bss"))}, pages=1)
    add("code.bin replacement", {"exefs/code.bin": REPLACEMENT}, pages=1)
    add("code.bin replacement in the folder and an IPS", {"code.bin": REPLACEMENT, "code.ips": ips([(0x10, b"mod")])})
    add("both code.bin, the exefs one wins", {"exefs/code.bin": REPLACEMENT, "code.bin": code_bytes(0x30, 5)}, pages=1)
    add("replacement and a BPS made for it",
        {"code.bin": REPLACEMENT, "exefs/code.bps": bps(REPLACEMENT, edited(REPLACEMENT, 0x20, b"zzzz"))}, pages=1)
    add("replacement and an IPS that writes into the bss",
        {"exefs/code.bin": REPLACEMENT, "exefs/code.ips": ips([(0x140, b"\x07" * 8)])}, pages=1)
    add("two patches, only the first applies (IPS, then BPS)",
        {"exefs/code.ips": ips([(0, b"first")]), "exefs/code.bps": bps(src, edited(src, 0, b"second"))})
    add("two patches, the first is a BPS and the second would fail",
        {"exefs/code.bps": bps(src, edited(src, 1, b"bps")), "code.ips": ips([(0x7000, b"fail")])})
    add("a failing first patch is not skipped",
        {"exefs/code.ips": ips([(0x7000, b"fail")]), "code.ips": ips([(0, b"fine")])})
    add("all four patches, the exefs IPS applies",
        {n: (ips([(0, bytes([i + 1]))]) if n.endswith("ips") else bps(src, edited(src, 0, bytes([i + 1]))))
         for i, n in enumerate(ORDER)})
    add("the folder's BPS when it is the only one left",
        {"code.bps": bps(src, edited(src, 2, b"last")), "code.bin": src})
    return c


CASES = _cases()
def _first(path_dir: str, names) -> str | None:
    return next((n for n in names if os.path.isfile(os.path.join(path_dir, *n.split("/")))), None)


def _read(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def _write(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def _azahar(driver, case: Case, mods: str, tmp: str) -> bytes | None:
    """ncch.cpp / ncch_container.cpp's steps around Azahar's patch.cpp (run by the harness)."""
    code = case.code
    replacement = _first(mods, ("exefs/code.bin", "code.bin"))
    if replacement:
        code = _read(os.path.join(mods, *replacement.split("/")))
    code += bytes(case.pages * PAGE)
    patch = _first(mods, ORDER)
    if not patch:
        return code
    buffer, out = os.path.join(tmp, "azahar_in.bin"), os.path.join(tmp, "azahar_out.bin")
    _write(buffer, code)
    done = oracle.run_patch(driver, buffer, os.path.join(mods, *patch.split("/")), out)
    if done.returncode == 2:
        return None
    assert done.returncode == 0, f"the harness failed ({done.returncode}): {done.stderr}"
    return _read(out)


def _native(native, case: Case, mods: str, tmp: str):
    code_in, out = os.path.join(tmp, "native_in.bin"), os.path.join(tmp, "native_out.bin")
    _write(code_in, case.code)
    if os.path.exists(out):
        os.remove(out)
    done = subprocess.run([native, "--mods", mods, "--bss-pages", str(case.pages), code_in, out],
                          capture_output=True, text=True)
    if done.returncode == 2:
        assert "patch error" in done.stderr and "mods: " in done.stderr
        assert not os.path.exists(out), "an output was written on a patch error"
        return None, done
    assert done.returncode == 0, f"mh4n_code_patch failed ({done.returncode}): {done.stderr}"
    return _read(out), done


def _code_mods(case: Case, user: str) -> bytes | None:
    try:
        return code_mods.apply_mods(case.code, user, PROGRAM_ID, case.pages * PAGE)[0]
    except code_mods.ModError:
        return None


def _make(tmp, case: Case):
    user = str(tmp / "user")
    mods = os.path.join(user, "load", "mods", f"{PROGRAM_ID:016X}")
    os.makedirs(mods, exist_ok=True)
    for rel, data in case.files.items():
        _write(os.path.join(mods, *rel.split("/")), data)
    work = str(tmp / "work")
    os.makedirs(work, exist_ok=True)
    return user, mods, work


def test_there_are_at_least_thirty_cases_with_the_ones_the_spec_names():
    assert len(CASES) >= 30
    assert len({c.name for c in CASES}) == len(CASES)


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_native_equals_azahars_patch_cpp(driver, native, tmp_path, case):
    user, mods, work = _make(tmp_path, case)
    theirs = _azahar(driver, case, mods, work)
    ours, _ = _native(native, case, mods, work)
    if theirs is None or ours is None:
        assert theirs is None and ours is None, f"{case.name}: one fails, the other does not"
    else:
        assert ours == theirs, f"{case.name}: different bytes (sizes {len(theirs)} and {len(ours)})"


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_native_equals_code_mods_py_without_the_bss_pages(native, tmp_path, case):
    user, mods, work = _make(tmp_path, case)
    ours, _ = _native(native, case, mods, work)
    theirs = _code_mods(case, user)
    if theirs is None or ours is None:
        assert theirs is None and ours is None, f"{case.name}: code_mods.py and the loader disagree on failing"
        return
    # code_mods.py's result is the code only: the native buffer without its bss pages (what the library is built from)
    assert theirs == ours[:len(ours) - case.pages * PAGE], f"{case.name}: code_mods.py and the loader differ"


def test_the_tool_exit_codes_and_usage(native, tmp_path):
    case = Case("usage", {"code.ips": ips([(0x7000, b"x")])})
    user, mods, work = _make(tmp_path, case)
    _, done = _native(native, case, mods, work)
    assert done.returncode == 2 and "mods: code.ips: patch error" in done.stderr
    assert str(tmp_path) not in done.stderr          # no host path in the message
    assert subprocess.run([native, "--mods", mods], capture_output=True).returncode == 1
    assert subprocess.run([native, "--mods", mods, "--bss-pages", "x", "a", "b"], capture_output=True).returncode == 1
    assert subprocess.run([native, "--mods", mods, "--bss-pages", "0", str(tmp_path / "missing"), str(tmp_path / "o")],
                          capture_output=True).returncode == 3
