# Derived from Azahar src/core/file_sys/patch.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it says
# "GPLv2 or any later version"): the record loop of ApplyIpsPatch (ips_reads_inside, apply_ips).
# Copyright 2019-2026 Citra Emulator Project / Azahar Emulator Project (patch.cpp)
"""LayeredFS (N3-10): native/src/layeredfs, tools/native/layeredfs_oracle and tools/native/romfs_build.py.

The first half needs nothing but Python: romfs_build's build, list and extract (offsets, nested and
non-ASCII names, the IVFC wrapper) and the oracle's patch-case reader. The second half runs Azahar's own
code: it needs MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit), clang++ and the built
`mh4n_layeredfs` (build/native/bin, or MH4N_LAYEREDFS). Without them those tests skip, or fail with
MH4N_REQUIRE_TEST_DATA=1.

For each synthetic case (a game folder, a mods folder) the harness and `mh4n_layeredfs` write the layered
image and the two files are compared byte for byte; or both fail. Every image is also read back with
romfs_build.py, so a wrong answer of both would still be seen.
"""
from __future__ import annotations

import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOLS = os.path.join(ROOT, "tools", "native")


def _load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rb = _load("romfs_build", os.path.join(TOOLS, "romfs_build.py"))
oracle = _load("layeredfs_oracle", os.path.join(TOOLS, "layeredfs_oracle", "layeredfs_oracle.py"))


def write(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def write_tree(root: str, files: dict[str, bytes], dirs: tuple[str, ...] = ()) -> None:
    os.makedirs(root, exist_ok=True)
    for rel, data in files.items():
        write(os.path.join(root, *rel.split("/")), data)
    for rel in dirs:
        os.makedirs(os.path.join(root, *rel.split("/")), exist_ok=True)


def data(size: int, seed: int) -> bytes:
    return random.Random(seed).randbytes(size)


# ---- patches (the formats, written here independently of both implementations) ---------------------------

def ips(records: list[tuple[int, bytes]], rle: list[tuple[int, int, int]] = (), eof: bool = True) -> bytes:
    out = b"PATCH"
    for offset, payload in records:
        out += offset.to_bytes(3, "big") + len(payload).to_bytes(2, "big") + payload
    for offset, count, value in rle:
        out += offset.to_bytes(3, "big") + b"\0\0" + count.to_bytes(2, "big") + bytes([value])
    return out + (b"EOF" if eof else b"")


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


def bps(source: bytes, target: bytes, source_crc_delta: int = 0, target_crc_delta: int = 0) -> bytes:
    out = b"BPS1" + _number(len(source)) + _number(len(target)) + _number(0)
    i = 0
    while i < len(target):
        same = i < len(source) and source[i] == target[i]
        j = i
        while j < len(target) and ((j < len(source) and source[j] == target[j]) == same):
            j += 1
        out += _number(((j - i - 1) << 2) | (0 if same else 1))
        if not same:
            out += target[i:j]
        i = j
    out += struct.pack("<I", (zlib.crc32(source) + source_crc_delta) & 0xFFFFFFFF)
    out += struct.pack("<I", (zlib.crc32(target) + target_crc_delta) & 0xFFFFFFFF)
    return out + struct.pack("<I", zlib.crc32(out))


def ips_reads_inside(patch: bytes) -> bool:
    """True if Azahar's loop over the records reads only bytes of the patch (a record whose bytes run past its
    end makes its memcpy read beyond the vector: undefined behaviour there, an error in the native code)."""
    pos = 5
    while pos < len(patch) - 3:
        if patch[pos:pos + 3] == b"EOF":
            return True
        if pos + 5 > len(patch):
            return False
        length = int.from_bytes(patch[pos + 3:pos + 5], "big")
        if length == 0:
            if pos + 8 > len(patch):
                return False
            pos += 8
        else:
            if pos + 5 + length > len(patch):
                return False
            pos += 5 + length
    return True


def apply_ips(patch: bytes, buffer: bytes) -> bytes | None:
    """What the IPS rules give (None: an error), written from the format's rules for well-formed patches."""
    out = bytearray(buffer)
    pos = 5
    while pos < len(patch) - 3:
        if patch[pos:pos + 3] == b"EOF":
            break
        offset = int.from_bytes(patch[pos:pos + 3], "big")
        length = int.from_bytes(patch[pos + 3:pos + 5], "big")
        if length == 0:
            length = int.from_bytes(patch[pos + 5:pos + 7], "big")
            if offset + length > len(out):
                return None
            out[offset:offset + length] = bytes([patch[pos + 7]]) * length
            pos += 8
            continue
        if offset + length > len(out):
            return None
        out[offset:offset + length] = patch[pos + 5:pos + 5 + length]
        pos += 5 + length
    return bytes(out)


# ---- romfs_build -------------------------------------------------------------------------------------

SAMPLE = {
    "a.txt": b"alpha",
    "b.bin": data(100, 1),
    "data/x.arc": data(1000, 2),
    "data/sub/deep.bin": data(33, 3),
    "eng/data/core.arc": data(300, 4),
    "empty.bin": b"",
}


def test_build_then_list_and_extract_round_trip(tmp_path):
    src = str(tmp_path / "src")
    write_tree(src, SAMPLE, dirs=("hollow",))
    image = str(tmp_path / "image.bin")
    assert rb.main(["build", src, image]) == 0
    with open(image, "rb") as f:
        blob = f.read()
    assert blob[:4] == (0x28).to_bytes(4, "little")
    entries = {e.path: e for e in rb.iter_files(open(image, "rb"))}
    assert set(entries) == set(SAMPLE)
    for path, entry in entries.items():
        assert entry.size == len(SAMPLE[path])
        assert entry.offset % 16 == 0
        assert blob[entry.offset:entry.offset + entry.size] == SAMPLE[path]       # the offset points at the bytes
    # the data region is contiguous and 16-aligned, in the order of the listing
    ordered = list(rb.iter_files(open(image, "rb")))
    for before, after in zip(ordered, ordered[1:]):
        assert after.offset == before.offset + (before.size + 15) // 16 * 16
    assert len(blob) == ordered[-1].offset + (ordered[-1].size + 15) // 16 * 16
    out = str(tmp_path / "x.out")
    assert rb.main(["extract", image, "data/sub/deep.bin", out]) == 0
    with open(out, "rb") as f:
        assert f.read() == SAMPLE["data/sub/deep.bin"]
    assert rb.main(["extract", image, "empty.bin", out]) == 0
    assert os.path.getsize(out) == 0
    assert rb.main(["extract", image, "nothing", out]) == 1


def test_list_prints_path_offset_and_size(tmp_path, capsys):
    src = str(tmp_path / "src")
    write_tree(src, {"one.bin": b"1" * 20, "dir/two.bin": b"2" * 5})
    image = str(tmp_path / "image.bin")
    assert rb.main(["build", src, image]) == 0
    capsys.readouterr()
    assert rb.main(["list", image]) == 0
    lines = capsys.readouterr().out.splitlines()
    parsed = {line.split(" ", 2)[2]: tuple(int(x, 16) for x in line.split(" ")[:2]) for line in lines}
    assert set(parsed) == {"one.bin", "dir/two.bin"}
    base = parsed["one.bin"][0]
    assert parsed["one.bin"][1] == 20 and parsed["dir/two.bin"] == (base + 32, 5)
    assert lines[0].startswith("0x") and lines[0].split(" ")[1].startswith("0x")


def test_names_outside_ascii_and_the_order_of_names(tmp_path):
    files = {"café.txt": b"x", "日本/語.bin": b"yy", "\U0001d11e/clef.bin": b"zzz",
             "B.txt": b"1", "a.txt": b"2", "éa": b"3"}
    src = str(tmp_path / "src")
    write_tree(src, files)
    image = str(tmp_path / "image.bin")
    assert rb.main(["build", src, image]) == 0
    got = {e.path: e for e in rb.iter_files(open(image, "rb"))}
    assert set(got) == set(files)
    names = [e.path for e in rb.iter_files(open(image, "rb"))]
    root_files = [n for n in names if "/" not in n]
    assert root_files == sorted(root_files, key=lambda n: n.encode("utf-8"))      # B.txt before a.txt


def test_a_folder_of_many_files_gets_prime_hash_tables(tmp_path):
    src = str(tmp_path / "src")
    write_tree(src, {f"d{i % 5}/f{i:03}.bin": bytes([i % 256]) * (i % 40) for i in range(120)})
    image = str(tmp_path / "image.bin")
    assert rb.main(["build", src, image]) == 0
    with open(image, "rb") as f:
        blob = f.read()
    (dh_len, fh_len) = struct.unpack_from("<I", blob, 8)[0], struct.unpack_from("<I", blob, 24)[0]
    assert dh_len // 4 == 7                       # 6 folders: 6 | 1
    assert fh_len // 4 == 127                     # 120 files: the next number with no factor up to 17 is 127
    assert len(list(rb.iter_files(open(image, "rb")))) == 120


def test_the_ivfc_wrapper(tmp_path, capsys):
    src = str(tmp_path / "src")
    write_tree(src, SAMPLE)
    plain = str(tmp_path / "plain.bin")
    wrapped = str(tmp_path / "romfs.bin")
    assert rb.main(["build", src, plain]) == 0
    assert rb.main(["build", src, wrapped, "--ivfc"]) == 0
    with open(plain, "rb") as f:
        level3 = f.read()
    with open(wrapped, "rb") as f:
        blob = f.read()
    assert blob[:4] == b"IVFC" and blob[0x1000:0x1000 + len(level3)] == level3
    # level 3 is where tools/verify_ncch.py finds it
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        import verify_ncch
        levels = verify_ncch.ivfc_levels(blob[:0x60], len(blob))
    finally:
        sys.path.remove(os.path.join(ROOT, "tools"))
    assert levels[2][0] == 0x1000 and levels[2][1] == len(level3)
    with open(wrapped, "rb") as f:
        assert rb.ivfc_level3(f) == (0x1000, len(level3))
    # list --ivfc gives offsets in the wrapped file; extract --ivfc reads them
    capsys.readouterr()
    assert rb.main(["list", wrapped, "--ivfc"]) == 0
    listing = {line.split(" ", 2)[2]: [int(x, 16) for x in line.split(" ")[:2]] for line in capsys.readouterr().out.splitlines()}
    for path, (offset, size) in listing.items():
        assert blob[offset:offset + size] == SAMPLE[path]
    out = str(tmp_path / "o.bin")
    assert rb.main(["extract", wrapped, "b.bin", out, "--ivfc"]) == 0
    with open(out, "rb") as f:
        assert f.read() == SAMPLE["b.bin"]
    # without --ivfc the IVFC image is not a RomFS image
    assert rb.main(["list", wrapped]) == 2
    # a plain image is not an IVFC one
    assert rb.main(["list", plain, "--ivfc"]) == 2


@pytest.mark.parametrize("damage, why", [
    (lambda b: b[:10], "cut inside the header"),
    (lambda b: (0x30).to_bytes(4, "little") + b[4:], "bad header size"),
    (lambda b: b[:0x1C] + (0x7FFFFFF0).to_bytes(4, "little") + b[0x20:], "file table out of the image"),
    (lambda b: b[:0x30], "cut inside the tables"),
])
def test_a_damaged_image_is_refused(tmp_path, damage, why):
    src = str(tmp_path / "src")
    write_tree(src, SAMPLE)
    image = str(tmp_path / "image.bin")
    assert rb.main(["build", src, image]) == 0
    with open(image, "rb") as f:
        blob = f.read()
    bad = str(tmp_path / "bad.bin")
    write(bad, damage(blob))
    assert rb.main(["list", bad]) == 2, why
    assert rb.main(["build", str(tmp_path / "no_such_folder"), str(tmp_path / "o.bin")]) == 2


# ---- Azahar's code, through the oracle ------------------------------------------------------------------

def _azahar() -> str | None:
    path = os.path.expanduser(os.environ.get("MH4N_AZAHAR_DIR", ""))
    return path if path and os.path.isdir(os.path.join(path, "src", "core", "file_sys")) else None


def _native_tool() -> str | None:
    path = os.environ.get("MH4N_LAYEREDFS") or os.path.join(ROOT, "build", "native", "bin", "mh4n_layeredfs")
    return path if os.path.isfile(path) else None


def _need(what: str) -> None:
    message = f"needs {what}"
    if os.environ.get("MH4N_REQUIRE_TEST_DATA") == "1":
        pytest.fail(message)
    pytest.skip(message)


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    azahar = _azahar()
    if azahar is None or shutil.which(os.environ.get("CXX", "clang++")) is None:
        _need("MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit) and clang++")
    return oracle.build_driver(azahar, os.environ.get("CXX", "clang++"), str(tmp_path_factory.mktemp("oracle")))


@pytest.fixture(scope="module")
def native():
    path = _native_tool()
    if path is None:
        _need("the built mh4n_layeredfs (build/native/bin, or MH4N_LAYEREDFS)")
    return path


def _first_difference(a: bytes, b: bytes) -> str:
    if a == b:
        return "equal"
    n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
    return f"sizes {len(a)} and {len(b)}, first difference at 0x{n:X}"


class Case:
    def __init__(self, name, src, romfs=None, ext=None, dirs=(), expect=None):
        self.name = name
        self.src = src
        self.romfs = romfs or {}
        self.ext = ext or {}
        self.dirs = dirs
        self.expect = expect          # the files the image should hold (checked on both), or None


def _lay(tmp, case: Case):
    src = str(tmp / "src")
    mods = str(tmp / "mods")
    write_tree(src, case.src, case.dirs)
    if case.romfs:
        write_tree(os.path.join(mods, "romfs"), case.romfs)
    if case.ext:
        write_tree(os.path.join(mods, "romfs_ext"), case.ext)
    os.makedirs(mods, exist_ok=True)
    base = str(tmp / "base.bin")
    assert rb.main(["build", src, base]) == 0
    return base, mods


def _images(driver, native, tmp, case: Case):
    base, mods = _lay(tmp, case)
    theirs = str(tmp / "azahar.bin")
    ours = str(tmp / "ours.bin")
    a = oracle.run_layered(driver, base, mods, theirs)
    b = subprocess.run([native, base, "0", "0", mods, ours], capture_output=True, text=True)
    return base, a, b, theirs, ours


def _files_of(path: str) -> dict[str, bytes]:
    with open(path, "rb") as f:
        entries = list(rb.iter_files(f))
        out = {}
        for e in entries:
            f.seek(e.offset)
            out[e.path] = f.read(e.size)
    return out


def _check(driver, native, tmp, case: Case) -> None:
    base, a, b, theirs, ours = _images(driver, native, tmp, case)
    assert a.returncode == 0, f"{case.name}: Azahar's code failed ({a.returncode}): {a.stderr}"
    assert b.returncode == 0, f"{case.name}: mh4n_layeredfs failed ({b.returncode}): {b.stderr}"
    with open(theirs, "rb") as f:
        expected = f.read()
    with open(ours, "rb") as f:
        got = f.read()
    assert got == expected, f"{case.name}: {_first_difference(expected, got)}"
    if case.expect is not None:
        assert _files_of(ours) == case.expect, f"{case.name}: the files are not the ones the case should give"


def _sample(extra=None) -> dict[str, bytes]:
    files = dict(SAMPLE)
    files.update(extra or {})
    return files


def _cases() -> list[Case]:
    cases: list[Case] = []
    add = cases.append

    add(Case("empty mods folder", _sample(), expect=_sample()))
    add(Case("an empty folder in the game", _sample(), dirs=("hollow", "data/also_empty"), expect=_sample()))

    x = SAMPLE["data/x.arc"]
    add(Case("replacements, bigger and smaller and empty",
             _sample(),
             romfs={"a.txt": data(200, 10), "data/x.arc": b"tiny", "b.bin": b""},
             expect=_sample({"a.txt": data(200, 10), "data/x.arc": b"tiny", "b.bin": b""})))
    add(Case("new files, new folders and an empty one",
             _sample(),
             romfs={"new.bin": data(33, 11), "data/sub/more.bin": data(70, 12), "newdir/inner/f.bin": data(16, 13),
                    "newdir/g.bin": data(15, 14), "A.TXT": b"case", "zero.bin": b""},
             expect=_sample({"new.bin": data(33, 11), "data/sub/more.bin": data(70, 12),
                             "newdir/inner/f.bin": data(16, 13), "newdir/g.bin": data(15, 14), "A.TXT": b"case",
                             "zero.bin": b""})))

    patched = bytearray(x)
    patched[0:7] = b"PATCHED"
    patched[100:150] = b"\xee" * 50
    patched[990:1000] = b"0123456789"
    add(Case("an IPS patch with records and an RLE record",
             _sample(),
             ext={"data/x.arc.ips": ips([(0, b"PATCHED"), (990, b"0123456789")], rle=[(100, 50, 0xEE)])},
             expect=_sample({"data/x.arc": bytes(patched)})))
    deep = SAMPLE["data/sub/deep.bin"]
    deep_new = bytes(b ^ 0x55 if 10 <= i < 20 else b for i, b in enumerate(deep))
    core = SAMPLE["eng/data/core.arc"]
    core_new = core[:5] + b"\x00" + core[6:] + b"\xab" * 123
    b_bin = SAMPLE["b.bin"]
    b_small = b_bin[:60]
    add(Case("BPS patches: same size, growing, shrinking, in sub-folders",
             _sample(),
             ext={"data/sub/deep.bin.bps": bps(deep, deep_new), "eng/data/core.arc.bps": bps(core, core_new),
                  "b.bin.bps": bps(b_bin, b_small)},
             expect=_sample({"data/sub/deep.bin": deep_new, "eng/data/core.arc": core_new,
                             "b.bin": b_small + b"\0" * (len(b_bin) - 60)})))
    add(Case("stubs in the middle, at the end and in a sub-folder",
             _sample({"data/y.arc": data(17, 5), "data/z.arc": data(4096, 6), "data/sub/two.bin": data(9, 7)}),
             ext={"data/y.arc.stub": b"", "data/z.arc.stub": b"", "data/sub/two.bin.stub": b"", "b.bin.stub": b"",
                  "nothing.stub": b"", "nowhere/nothing.stub": b""},
             expect={k: v for k, v in _sample({"data/y.arc": data(17, 5), "data/z.arc": data(4096, 6),
                                               "data/sub/two.bin": data(9, 7)}).items()
                     if k not in ("data/y.arc", "data/z.arc", "data/sub/two.bin", "b.bin")}))
    add(Case("a stub removes a file before or after a patch of it is read",
             _sample({"data/y.arc": data(17, 5), "data/0first.bin": data(3, 4)}),
             ext={"data/x.arc.ips": ips([(0, b"zz")]), "data/x.arc.stub": b"",       # patched, then removed
                  "data/y.arc.stub": b"", "data/y.arc.zzz": ips([(0, b"zz")])},      # removed: nothing to patch
             expect=None))
    add(Case("patches that fail leave the file as it was",
             _sample({"data/y.arc": data(17, 5), "data/z.arc": data(64, 6)}),
             ext={"a.txt.ips": ips([(3, b"0123456789")]), "data/y.arc.bps": bps(data(17, 5), data(17, 8), 1),
                  "data/z.arc.bps": bps(data(64, 6), data(64, 9), 0, 1), "b.bin.ips": b"not a patch",
                  "nofile.bin.ips": ips([(0, b"x")]), "ab": b"short name", "empty.bin.ips": ips([(0, b"x")])},
             expect=_sample({"data/y.arc": data(17, 5), "data/z.arc": data(64, 6)})))
    xnew = bytearray(x)
    xnew[5] ^= 0xFF
    add(Case("odd extensions: applied as BPS when the original exists",
             _sample(),
             ext={"data/x.arc.xyz": bps(x, bytes(xnew)), "data/sub/deep.bin.txt": b"a note", "unrelated.md": b"x"},
             expect=_sample({"data/x.arc": bytes(xnew)})))
    add(Case("names outside ASCII",
             {"café.txt": b"coffee", "日本/語.bin": data(40, 1), "\U0001d11e/clef.bin": data(20, 2),
              "plain.bin": data(8, 3)},
             romfs={"café.txt": b"tea", "\U0001d11e/new€.bin": data(9, 4)},
             ext={"日本/語.bin.ips": ips([(0, b"Z")])},
             expect={"café.txt": b"tea", "日本/語.bin": b"Z" + data(40, 1)[1:],
                     "\U0001d11e/clef.bin": data(20, 2), "plain.bin": data(8, 3),
                     "\U0001d11e/new€.bin": data(9, 4)}))
    add(Case("empty files in the game, replaced and patched",
             {"e1.bin": b"", "e2.bin": b"", "f.bin": data(5, 1), "e3.bin": b""},
             romfs={"e1.bin": b"now full", "f.bin": b""},
             ext={"e2.bin.ips": ips([(0, b"x")]), "e3.bin.stub": b""},
             expect=None))
    add(Case("a patch for a file the mods replaced (the base's bytes are patched, at the original offset)",
             _sample(),
             romfs={"data/x.arc": data(300, 20), "b.bin": data(40, 21)},
             ext={"data/x.arc.ips": ips([(0, b"AB")]), "b.bin.bps": bps(data(100, 1), data(100, 2))},
             expect=None))
    add(Case("a patch for a file the mods created (nothing of the base behind it)",
             _sample(),
             romfs={"fresh.bin": data(50, 22)},
             ext={"fresh.bin.ips": ips([(1, b"Q")])},
             expect=None))
    many = {f"d{i % 7}/f{i:03}.bin": data(i % 50, i) for i in range(400)}
    many_romfs = {f"d{i % 7}/f{i:03}.bin": data(i % 30 + 1, 1000 + i) for i in range(0, 400, 13)}
    many_romfs.update({f"d{i % 7}/g{i:03}.bin": data(5, 2000 + i) for i in range(0, 400, 50)})
    many_ext = {f"d{i % 7}/f{i:03}.bin.stub": b"" for i in range(10, 400, 17)}
    add(Case("four hundred files in seven folders (the hash tables)", many, romfs=many_romfs, ext=many_ext))
    nested = {"/".join(f"n{i}" for i in range(12)) + "/leaf.bin": data(20, 30), "top.bin": data(4, 31)}
    add(Case("a file twelve folders down, patched",
             nested,
             ext={"/".join(f"n{i}" for i in range(12)) + "/leaf.bin.ips": ips([(2, b"deep")])},
             expect=None))
    big = data(2_500_000, 40)
    big_new = bytearray(big)
    for start in (0, 1_000_000, 2_400_000):
        big_new[start:start + 4000] = data(4000, start)
    add(Case("a big file patched by BPS and one replaced", {"big.bin": big, "small.bin": b"s"},
             romfs={"small.bin": data(1_200_000, 41)}, ext={"big.bin.bps": bps(big, bytes(big_new))}, expect=None))
    # ScanDirectoryTree stops entering folders below 256 levels
    level = lambda n: "/".join(["d"] * n)                               # noqa: E731
    add(Case("a patch 256 folders down is applied and one 257 down is not",
             {level(256) + "/f.bin": data(20, 50), level(257) + "/f.bin": data(20, 51)},
             ext={level(256) + "/f.bin.ips": ips([(0, b"A")]), level(257) + "/f.bin.ips": ips([(0, b"B")])},
             expect=None))
    add(Case("a new file named like a folder of the game, and a stub for a file the mods created",
             _sample(),
             romfs={"data": b"a file called data", "fresh/f.bin": data(10, 70), "fresh/g.bin": data(11, 71)},
             ext={"fresh/g.bin.stub": b""},
             expect=_sample({"data": b"a file called data", "fresh/f.bin": data(10, 70)})))
    add(Case("a replacement of the same name in many places",
             _sample(), romfs={k: data(len(v) + 3, 60 + i) for i, (k, v) in enumerate(SAMPLE.items())}, expect=None))
    return cases


CASES = _cases()


def test_there_are_at_least_twenty_cases():
    assert len(CASES) >= 20
    assert len({c.name for c in CASES}) == len(CASES)


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_the_native_image_is_byte_identical_to_azahars(driver, native, tmp_path, case):
    _check(driver, native, tmp_path, case)


def test_no_mods_folder_at_all(driver, native, tmp_path):
    src = str(tmp_path / "src")
    write_tree(src, SAMPLE)
    base = str(tmp_path / "base.bin")
    assert rb.main(["build", src, base]) == 0
    missing = str(tmp_path / "nowhere")
    a = oracle.run_layered(driver, base, missing, str(tmp_path / "a.bin"))
    b = subprocess.run([native, base, "0", "0", missing, str(tmp_path / "b.bin")], capture_output=True, text=True)
    assert a.returncode == 0 and b.returncode == 0
    with open(str(tmp_path / "a.bin"), "rb") as fa, open(str(tmp_path / "b.bin"), "rb") as fb, open(base, "rb") as fc:
        image_a, image_b, original = fa.read(), fb.read(), fc.read()
    assert image_a == image_b
    assert image_a == original          # romfs_build lays an image out as LayeredFS does: nothing changes


def test_an_image_in_a_bigger_file(driver, native, tmp_path):
    case = Case("slice", _sample(), romfs={"a.txt": b"replaced"})
    base, mods = _lay(tmp_path, case)
    wrapped = str(tmp_path / "romfs.bin")
    src = str(tmp_path / "src")
    assert rb.main(["build", src, wrapped, "--ivfc"]) == 0
    with open(base, "rb") as f:
        level3 = f.read()
    with open(wrapped, "ab") as f:
        f.write(b"\xaa" * 777)
    ours = str(tmp_path / "ours.bin")
    done = subprocess.run([native, wrapped, "0x1000", hex(len(level3)), mods, ours], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    theirs = str(tmp_path / "azahar.bin")
    assert oracle.run_layered(driver, base, mods, theirs).returncode == 0
    with open(ours, "rb") as fa, open(theirs, "rb") as fb:
        assert fa.read() == fb.read()
    assert _files_of(ours)["a.txt"] == b"replaced"
    # level 3 found from the header: the same offset
    with open(wrapped, "rb") as f:
        assert rb.ivfc_level3(f)[0] == 0x1000


def test_a_stub_for_the_first_file_of_a_folder_where_azahars_code_throws(driver, native, tmp_path):
    """A known difference: Azahar looks the removed first file up in a map that does not hold it and the
    exception ends the process; the native code removes the file and links the next one."""
    for stubbed in ("a.txt", "data/x.arc"):
        case = Case("first", _sample({"data/y.arc": data(17, 5)}), ext={stubbed + ".stub": b""})
        base, a, b, theirs, ours = _images(driver, native, tmp_path / stubbed.replace("/", "_"), case)
        assert a.returncode == 3 and "threw" in a.stderr, a.stderr
        assert b.returncode == 0, b.stderr
        expect = _sample({"data/y.arc": data(17, 5)})
        del expect[stubbed]
        assert _files_of(ours) == expect


def test_every_file_of_a_folder_stubbed_is_also_where_azahars_code_throws(driver, native, tmp_path):
    case = Case("all", _sample(), ext={"data/x.arc.stub": b"", "data/sub/deep.bin.stub": b""})
    base, a, b, theirs, ours = _images(driver, native, tmp_path, case)
    assert a.returncode == 3 and b.returncode == 0
    expect = _sample()
    del expect["data/x.arc"], expect["data/sub/deep.bin"]
    assert _files_of(ours) == expect


def test_a_base_that_is_not_a_romfs_is_refused_by_the_native_tool(native, tmp_path):
    bad = str(tmp_path / "bad.bin")
    write(bad, b"\0" * 100)
    done = subprocess.run([native, bad, "0", "0", str(tmp_path), str(tmp_path / "o.bin")], capture_output=True, text=True)
    assert done.returncode == 2 and "header" in done.stderr
    done = subprocess.run([native], capture_output=True, text=True)
    assert done.returncode == 1 and "usage" in done.stderr


# ---- patches: the harness's `patch` mode, and the native code through the same patches ---------------------

BUFFER = data(2000, 99)


def _patch_cases() -> list[tuple[str, str, bytes, bytes | None]]:
    """(name, kind, patch, expected result or None for an error) for patches whose result is known by construction."""
    cases: list[tuple[str, str, bytes, bytes | None]] = []
    out = bytearray(BUFFER)
    out[10:13] = b"abc"
    cases.append(("ips: one record", "ips", ips([(10, b"abc")]), bytes(out)))
    out2 = bytearray(BUFFER)
    out2[0:2] = b"hi"
    out2[1998:2000] = b"yo"
    cases.append(("ips: first and last bytes", "ips", ips([(0, b"hi"), (1998, b"yo")]), bytes(out2)))
    out3 = bytearray(BUFFER)
    out3[500:700] = b"\x07" * 200
    cases.append(("ips: rle", "ips", ips([], rle=[(500, 200, 7)]), bytes(out3)))
    cases.append(("ips: no records", "ips", ips([]), BUFFER))
    cases.append(("ips: no EOF marker", "ips", ips([(3, b"zz")], eof=False), BUFFER[:3] + b"zz" + BUFFER[5:]))
    cases.append(("ips: record past the end", "ips", ips([(1999, b"ab")]), None))
    cases.append(("ips: rle past the end", "ips", ips([], rle=[(1990, 11, 1)]), None))
    cases.append(("ips: wrong header", "ips", b"PATCX" + ips([(0, b"a")])[5:], None))
    cases.append(("ips: record to exactly the end", "ips", ips([(1997, b"abc")]), BUFFER[:1997] + b"abc"))
    cases.append(("ips: a later record overwrites an earlier one", "ips", ips([(5, b"aaaa"), (6, b"bb")]),
                 BUFFER[:5] + b"abba" + BUFFER[9:]))
    tgt = bytearray(BUFFER)
    tgt[100:200] = data(100, 7)
    cases.append(("bps: same size", "bps", bps(BUFFER, bytes(tgt)), bytes(tgt)))
    longer = BUFFER + data(300, 8)
    cases.append(("bps: longer target", "bps", bps(BUFFER, longer), longer))
    shorter = BUFFER[:700]
    cases.append(("bps: shorter target (the buffer keeps its size)", "bps", bps(BUFFER, shorter),
                  shorter + b"\0" * 1300))
    cases.append(("bps: identical", "bps", bps(BUFFER, BUFFER), BUFFER))
    cases.append(("bps: source crc", "bps", bps(BUFFER, bytes(tgt), 1), None))
    cases.append(("bps: target crc", "bps", bps(BUFFER, bytes(tgt), 0, 1), None))
    cases.append(("bps: magic", "bps", b"BPS2" + bps(BUFFER, bytes(tgt))[4:], None))
    cases.append(("bps: other source", "bps", bps(data(2000, 1), data(2000, 2)), None))
    return cases


PATCHES = _patch_cases()


@pytest.mark.parametrize("name, kind, patch, expected", PATCHES, ids=[p[0] for p in PATCHES])
def test_the_harness_patch_mode_applies_azahars_rules(driver, tmp_path, name, kind, patch, expected):
    buffer = str(tmp_path / "buffer.bin")
    patch_path = str(tmp_path / f"p.{kind}")
    out = str(tmp_path / "out.bin")
    write(buffer, BUFFER)
    write(patch_path, patch)
    done = oracle.run_patch(driver, buffer, patch_path, out)
    if expected is None:
        assert done.returncode == 2, done.stderr
    else:
        assert done.returncode == 0, done.stderr
        with open(out, "rb") as f:
            assert f.read() == expected
    # the same through the other spelling of the kind
    assert oracle.patch_kind(patch_path) == kind
    assert oracle.run_patch(driver, buffer, patch_path, out, kind=kind).returncode == done.returncode


@pytest.mark.parametrize("name, kind, patch, expected", PATCHES, ids=[p[0] for p in PATCHES])
def test_the_native_code_applies_the_same_patch_to_a_file_of_the_romfs(driver, native, tmp_path, name, kind, patch,
                                                                       expected):
    case = Case(name, {"f.bin": BUFFER, "g.bin": b"other"}, ext={f"f.bin.{kind}": patch},
                expect={"f.bin": BUFFER if expected is None else expected, "g.bin": b"other"})
    _check(driver, native, tmp_path, case)
    if kind == "ips" and expected is not None:
        assert apply_ips(patch, BUFFER) == expected          # the reference rules above agree with both


def test_a_patch_mode_error_is_exit_2_and_unreadable_input_is_1(driver, tmp_path):
    buffer = str(tmp_path / "buffer.bin")
    write(buffer, BUFFER)
    done = oracle.run_patch(driver, buffer, str(tmp_path / "missing.ips"), str(tmp_path / "o"))
    assert done.returncode == 1
    done = subprocess.run([driver], capture_output=True, text=True)
    assert done.returncode == 1 and "usage" in done.stderr


def test_the_oracle_refuses_sources_that_are_not_the_pinned_commit(tmp_path):
    fake = tmp_path / "azahar"
    fake.mkdir()
    subprocess.run(["git", "init", "-q", str(fake)], check=True)
    subprocess.run(["git", "-C", str(fake), "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q",
                    "--allow-empty", "-m", "x"], check=True)
    with pytest.raises(oracle.OracleError, match="pinned commit"):
        oracle.build_driver(str(fake), keep=str(tmp_path / "keep"))
    with pytest.raises(oracle.OracleError, match="not a git checkout"):
        oracle.azahar_commit(str(tmp_path))


def test_the_ports_hunk_is_found_by_the_file_name_and_not_by_line_numbers(tmp_path):
    """git apply --include with the layered_fs.cpp block of a patch that has other files in front of it."""
    azahar = _azahar()
    if azahar is None:
        _need("MH4N_AZAHAR_DIR")
    with open(oracle.PATCH, encoding="utf-8") as f:
        text = f.read()
    start = text.index("diff --git a/src/core/file_sys/layered_fs.cpp")
    end = text.index("diff --git", start + 10)
    block = text[start:end]
    padding = ("diff --git a/src/other/unrelated.cpp b/src/other/unrelated.cpp\n"
               "--- a/src/other/unrelated.cpp\n+++ b/src/other/unrelated.cpp\n@@ -1,2 +1,3 @@\n a\n+b\n c\n")
    patched = tmp_path / "regenerated.patch"
    patched.write_text(padding + block + padding, encoding="utf-8")
    original = oracle.PATCH
    oracle.PATCH = str(patched)
    try:
        tree = tmp_path / "tree"
        tree.mkdir()
        oracle.prepare_tree(azahar, str(tree))
    finally:
        oracle.PATCH = original
    with open(tree / "src" / "core" / "file_sys" / "layered_fs.cpp", encoding="utf-8") as f:
        assert "GetAllFilesFromNestedEntries" in f.read()


def test_random_corruptions_of_patches_give_the_same_image(driver, native, tmp_path):
    """Four hundred patches, a few bytes of each changed at random (seeded), laid over one RomFS at once: every
    file ends up patched or untouched in the same way in both images. Truncated patches are not made: Azahar's
    code reads past the end of those (the native code answers with an error, tested in layeredfs_test)."""
    rng = random.Random(2026)
    files: dict[str, bytes] = {}
    ext: dict[str, bytes] = {}
    for i in range(400):
        source = data(200, 500 + i)
        target = bytearray(source)
        for _ in range(rng.randint(1, 4)):
            at = rng.randrange(0, 190)
            target[at:at + rng.randint(1, 9)] = data(9, i * 7 + at)[:rng.randint(1, 9)] if rng.random() < .5 else b"\0" * 5
        target = bytes(target[:200]) if rng.random() < .8 else bytes(target) + data(rng.randint(1, 40), i)
        if i % 2 == 0:
            patch = bytearray(bps(source, target))
            kind = "bps"
        else:
            patch = bytearray(ips([(rng.randrange(0, 190), data(rng.randint(1, 9), i)) for _ in range(rng.randint(1, 3))],
                                  rle=[(rng.randrange(0, 150), rng.randint(1, 40), rng.randrange(256))]
                                  if rng.random() < .4 else []))
            kind = "ips"
        for _ in range(rng.choice((0, 1, 1, 2, 3))):
            at = rng.randrange(0, len(patch))
            if kind == "ips" and at < 5:
                continue                                  # the header: a changed one is a plain error, tested above
            if kind == "ips" and at >= len(patch) - 3:
                continue                                  # the EOF marker: a changed one makes Azahar read past the end
            changed_patch = bytearray(patch)
            changed_patch[at] ^= 1 << rng.randrange(8)
            if kind == "ips" and not ips_reads_inside(bytes(changed_patch)):
                continue                                  # a length that runs past the patch: undefined in Azahar
            patch = changed_patch
        files[f"f{i:03}.bin"] = source
        ext[f"f{i:03}.bin.{kind}"] = bytes(patch)
    case = Case("random corruptions", files, ext=ext)
    base, a, b, theirs, ours = _images(driver, native, tmp_path, case)
    assert a.returncode == 0, a.stderr
    assert b.returncode == 0, b.stderr
    with open(theirs, "rb") as fa, open(ours, "rb") as fb:
        assert fa.read() == fb.read()
    got = _files_of(ours)
    changed = sum(1 for k, v in got.items() if v != files[k])
    assert 100 < changed < 400          # some patches apply and some fail: the test sees both
