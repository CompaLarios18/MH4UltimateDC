#!/usr/bin/env python3
# Derived from Azahar src/core/file_sys/layered_fs.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: it
# says "GPLv2 or any later version"): the layout of the RomFS tables that LayeredFS writes (the order of
# PrepareBuild, the hash of a name, GetHashTableSize, the 4-byte alignment of names).
# Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project (layered_fs.cpp)
"""Build, list and extract level-3 RomFS images (N3-10). Standard library only.

    romfs_build.py build DIR OUT [--ivfc]
    romfs_build.py list IMAGE [--ivfc]
    romfs_build.py extract IMAGE PATH OUT [--ivfc]

  build     makes a level-3 RomFS image (what the SDK parses: a 0x28-byte header, the hash tables, the
            directory and file tables, then the file data, every file 16-byte aligned) from the files of
            a folder. It is for synthetic test data: the layout is the one Azahar's LayeredFS writes (the
            names of a folder in the order of their UTF-8 bytes, a folder's files before its sub-folders),
            so an image built here is what LayeredFS gives back over an empty mods folder. With --ivfc
            the image is wrapped as a RomFS section: an IVFC header (0x60 bytes, an empty master hash),
            padded to 0x1000, then level 3, then zero-filled levels 1 and 2. The hash levels are not
            real hashes: nothing checks them but tools/verify_ncch.py.
  list      prints one line per file, `0x<offset> 0x<size> <path>`, in the order of the directory tables
            (a folder's files, then its sub-folders). The offset is where the file's bytes start in the
            file given; with --ivfc that is the file of the IVFC image (the game folder's romfs.bin, as
            tools/native/prepare_game.py writes it), not the level-3 image inside it.
  extract   writes one file of the image to OUT. PATH is the file's path in the RomFS, with '/'.
  --ivfc    the image is an IVFC RomFS section (romfs.bin); level 3 is found from the header as
            tools/verify_ncch.py does (it starts at 0x1000, where the native file system serves it from).

Exit status: 0 ok, 1 a path not in the image, 2 a usage problem or an unreadable or damaged image.
Nothing is read whole except what is asked for: `list` and `extract` read the tables and the one file.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from typing import BinaryIO, Iterator, NamedTuple


class RomFsError(Exception):
    pass


# ---- the layout --------------------------------------------------------------------------------------

def _align(value: int, to: int) -> int:
    return (value + to - 1) // to * to


def _name_hash(name: bytes, parent: int) -> int:
    """3dbrew's hash of a name (UTF-16 units) and the offset of its parent directory."""
    units = name.decode("utf-8", "surrogatepass").encode("utf-16-le")
    h = parent ^ 123456789
    for i in range(0, len(units), 2):
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= units[i] | units[i + 1] << 8
    return h


def _table_size(count: int) -> int:
    if count < 3:
        return 3
    if count < 19:
        return count | 1
    while any(count % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        count += 1
    return count


class _Dir:
    def __init__(self, name: bytes, parent: "_Dir | None") -> None:
        self.name = name
        self.parent = parent
        self.files: list[_File] = []
        self.dirs: list[_Dir] = []
        self.offset = 0


class _File:
    def __init__(self, name: bytes, parent: _Dir, path: str) -> None:
        self.name = name
        self.parent = parent
        self.path = path
        self.offset = 0          # in the file table
        self.data_offset = 0
        self.size = 0


def _scan(root: str) -> _Dir:
    top = _Dir(b"", None)

    def fill(directory: _Dir, path: str) -> None:
        names = sorted(os.listdir(path), key=lambda n: os.fsencode(n))
        for name in names:
            full = os.path.join(path, name)
            raw = os.fsencode(name)
            if os.path.isdir(full):
                child = _Dir(raw, directory)
                directory.dirs.append(child)
                fill(child, full)
            else:
                file = _File(raw, directory, full)
                file.size = os.path.getsize(full)
                directory.files.append(file)

    fill(top, root)
    return top


def _name_bytes(name: bytes) -> bytes:
    return name.decode("utf-8", "surrogatepass").encode("utf-16-le")


def build_image(root: str) -> bytes:
    """The level-3 RomFS image of the files below `root` (all of it in memory: synthetic data only)."""
    top = _scan(root)
    dir_list: list[_Dir] = []
    file_list: list[_File] = []
    dir_pos = 0
    file_pos = 0

    # the order Azahar's PrepareBuild walks: a folder's files, then its folders, then each folder's content
    def add_dir(d: _Dir) -> None:
        nonlocal dir_pos
        d.offset = dir_pos
        dir_list.append(d)
        dir_pos += 0x18 + _align(len(_name_bytes(d.name)), 4)

    def prepare(d: _Dir) -> None:
        nonlocal file_pos
        for f in d.files:
            f.offset = file_pos
            file_list.append(f)
            file_pos += 0x20 + _align(len(_name_bytes(f.name)), 4)
        for child in d.dirs:
            add_dir(child)
        for child in d.dirs:
            prepare(child)

    add_dir(top)
    prepare(top)

    dir_hash = [0xFFFFFFFF] * _table_size(len(dir_list))
    file_hash = [0xFFFFFFFF] * _table_size(len(file_list))
    dir_meta = bytearray(b"\xff" * dir_pos)
    file_meta = bytearray(b"\xff" * file_pos)

    for d in dir_list:
        parent = d.parent.offset if d.parent else 0
        siblings = d.parent.dirs if d.parent else []
        index = siblings.index(d) if d.parent else 0
        next_sibling = siblings[index + 1].offset if d.parent and index + 1 < len(siblings) else 0xFFFFFFFF
        first_child = d.dirs[0].offset if d.dirs else 0xFFFFFFFF
        first_file = d.files[0].offset if d.files else 0xFFFFFFFF
        bucket = _name_hash(d.name, parent) % len(dir_hash)
        name = _name_bytes(d.name)
        struct.pack_into("<6I", dir_meta, d.offset, parent, next_sibling, first_child, first_file,
                         dir_hash[bucket], len(name))
        dir_hash[bucket] = d.offset
        pad = _align(len(name), 4)
        dir_meta[d.offset + 0x18:d.offset + 0x18 + pad] = name.ljust(pad, b"\0")     # the padding is zeros

    data_pos = 0
    for f in file_list:
        siblings = f.parent.files
        index = siblings.index(f)
        next_sibling = siblings[index + 1].offset if index + 1 < len(siblings) else 0xFFFFFFFF
        f.data_offset = data_pos
        data_pos += _align(f.size, 16)
        bucket = _name_hash(f.name, f.parent.offset) % len(file_hash)
        name = _name_bytes(f.name)
        struct.pack_into("<IIQQII", file_meta, f.offset, f.parent.offset, next_sibling, f.data_offset, f.size,
                         file_hash[bucket], len(name))
        file_hash[bucket] = f.offset
        pad = _align(len(name), 4)
        file_meta[f.offset + 0x20:f.offset + 0x20 + pad] = name.ljust(pad, b"\0")

    dh_off = 0x28
    dh_len = 4 * len(dir_hash)
    dm_off = dh_off + dh_len
    fh_off = dm_off + len(dir_meta)
    fh_len = 4 * len(file_hash)
    fm_off = fh_off + fh_len
    data_off = _align(fm_off + len(file_meta), 16)
    out = bytearray(data_off)
    struct.pack_into("<10I", out, 0, 0x28, dh_off, dh_len, dm_off, len(dir_meta), fh_off, fh_len, fm_off,
                     len(file_meta), data_off)
    struct.pack_into(f"<{len(dir_hash)}I", out, dh_off, *dir_hash)
    out[dm_off:dm_off + len(dir_meta)] = dir_meta
    struct.pack_into(f"<{len(file_hash)}I", out, fh_off, *file_hash)
    out[fm_off:fm_off + len(file_meta)] = file_meta
    for f in file_list:
        with open(f.path, "rb") as source:
            data = source.read()
        out += data + b"\0" * (_align(len(data), 16) - len(data))
    return bytes(out)


def wrap_ivfc(level3: bytes) -> bytes:
    """An IVFC RomFS section around a level-3 image: header, level 3 at 0x1000, zero-filled levels 1 and 2."""
    block_log2 = 12
    block = 1 << block_log2
    l3_off = 0x1000
    l1_off = _align(l3_off + len(level3), block)
    # hash levels: one 0x20-byte hash per block of the level below
    l2_size = -(-len(level3) // block) * 0x20
    l1_size = -(-l2_size // block) * 0x20
    l2_off = _align(l1_off + l1_size, block)
    master_size = -(-l1_size // block) * 0x20
    header = bytearray(0x60)
    header[0:4] = b"IVFC"
    struct.pack_into("<II", header, 4, 0x10000, master_size)
    sizes = (l1_size, l2_size, len(level3))
    logical = 0
    for i, size in enumerate(sizes):
        logical = _align(logical, block)
        struct.pack_into("<QQI", header, 0x0C + 0x18 * i, logical, size, block_log2)
        logical += size
    struct.pack_into("<I", header, 0x54, 0x5C)
    out = bytearray(l2_off + l2_size)
    out[0:0x60] = header
    out[l3_off:l3_off + len(level3)] = level3
    return bytes(out)


# ---- reading -----------------------------------------------------------------------------------------

class Entry(NamedTuple):
    path: str
    offset: int    # in the file the image is read from
    size: int


def ivfc_level3(f: BinaryIO) -> tuple[int, int]:
    """(offset, size) of level 3 in an IVFC RomFS section, found as tools/verify_ncch.py finds it."""
    f.seek(0)
    header = f.read(0x60)
    if len(header) < 0x60 or header[:4] != b"IVFC" or struct.unpack_from("<I", header, 4)[0] != 0x10000:
        raise RomFsError("not an IVFC RomFS section (no IVFC header)")
    (master_size,) = struct.unpack_from("<I", header, 8)
    sizes = []
    for i in range(3):
        _logical, size, block_log2 = struct.unpack_from("<QQI", header, 0x0C + 0x18 * i)
        sizes.append((size, 1 << block_log2))
    (_, _), (_, _), (l3_size, l3_block) = sizes
    l3_off = _align(0x60 + master_size, l3_block)
    f.seek(0, os.SEEK_END)
    if l3_off + l3_size > f.tell():
        raise RomFsError("the IVFC level 3 runs past the end of the file")
    return l3_off, l3_size


def _bounds(f: BinaryIO, ivfc: bool) -> tuple[int, int]:
    if ivfc:
        return ivfc_level3(f)
    f.seek(0, os.SEEK_END)
    return 0, f.tell()


def _read(f: BinaryIO, offset: int, size: int, what: str) -> bytes:
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise RomFsError(f"the image is cut inside its {what}")
    return data


def iter_files(f: BinaryIO, ivfc: bool = False) -> Iterator[Entry]:
    """The files of an image in the order of its directory tables, one at a time (only the two tables are read)."""
    base, size = _bounds(f, ivfc)
    header = _read(f, base, 0x28, "header")
    (header_len, _dho, _dhl, dm_off, dm_len, _fho, _fhl, fm_off, fm_len, data_off) = struct.unpack("<10I", header)
    if header_len != 0x28 or max(dm_off + dm_len, fm_off + fm_len) > size:
        raise RomFsError("not a RomFS image (bad header)")
    dirs = _read(f, base + dm_off, dm_len, "directory table")
    files = _read(f, base + fm_off, fm_len, "file table")
    seen_dirs: set[int] = set()

    def name_at(table: bytes, at: int, head: int, length_at: int) -> str:
        (length,) = struct.unpack_from("<I", table, length_at)
        raw = table[at + head:at + head + length]
        if len(raw) != length or length % 2:
            raise RomFsError("a name runs out of its table")
        return raw.decode("utf-16-le", "surrogatepass")

    def walk(offset: int, prefix: str) -> Iterator[Entry]:
        if offset + 0x18 > len(dirs) or offset in seen_dirs:
            raise RomFsError("the directory table is damaged")
        seen_dirs.add(offset)
        (_parent, _sibling, child, first_file, _hnext, _nlen) = struct.unpack_from("<6I", dirs, offset)
        guard = 0
        file_offset = first_file
        while file_offset != 0xFFFFFFFF:
            guard += 1
            if file_offset + 0x20 > len(files) or guard > len(files):
                raise RomFsError("the file table is damaged")
            (_fp, fsib, fdata, fsize, _fh, _fl) = struct.unpack_from("<IIQQII", files, file_offset)
            name = name_at(files, file_offset, 0x20, file_offset + 28)
            yield Entry(prefix + name, base + data_off + fdata, fsize)
            file_offset = fsib
        guard = 0
        while child != 0xFFFFFFFF:
            guard += 1
            if child + 0x18 > len(dirs) or guard > len(dirs):
                raise RomFsError("the directory table is damaged")
            name = name_at(dirs, child, 0x18, child + 20)
            yield from walk(child, prefix + name + "/")
            (child,) = struct.unpack_from("<I", dirs, child + 4)

    yield from walk(0, "")


# ---- the command line ---------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build")
    build.add_argument("dir")
    build.add_argument("out")
    build.add_argument("--ivfc", action="store_true")
    lst = sub.add_parser("list")
    lst.add_argument("image")
    lst.add_argument("--ivfc", action="store_true")
    extract = sub.add_parser("extract")
    extract.add_argument("image")
    extract.add_argument("path")
    extract.add_argument("out")
    extract.add_argument("--ivfc", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            if not os.path.isdir(args.dir):
                raise RomFsError(f"{args.dir} is not a folder")
            image = build_image(args.dir)
            with open(args.out, "wb") as f:
                f.write(wrap_ivfc(image) if args.ivfc else image)
            return 0
        with open(args.image, "rb") as f:
            if args.command == "list":
                for entry in iter_files(f, args.ivfc):
                    print(f"0x{entry.offset:08X} 0x{entry.size:08X} {entry.path}")
                return 0
            for entry in iter_files(f, args.ivfc):
                if entry.path == args.path:
                    f.seek(entry.offset)
                    remaining = entry.size
                    with open(args.out, "wb") as out:
                        while remaining:
                            chunk = f.read(min(remaining, 1 << 20))
                            if not chunk:
                                raise RomFsError("the image is cut inside the file")
                            out.write(chunk)
                            remaining -= len(chunk)
                    return 0
            print(f"romfs_build: {args.path} is not in the image", file=sys.stderr)
            return 1
    except (RomFsError, OSError) as error:
        print(f"romfs_build: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
