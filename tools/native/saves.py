#!/usr/bin/env python3
# Derived from Azahar src/core/file_sys/ncch_container.cpp, src/core/file_sys/ncch_container.h,
# src/core/file_sys/archive_backend.h, src/core/file_sys/archive_source_sd_savedata.cpp,
# src/core/file_sys/archive_extsavedata.cpp, src/core/hle/service/fs/archive.h and
# src/common/common_paths.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
# ncch_container.cpp, ncch_container.h and archive.h say "Licensed under GPLv3 or any later version";
# archive_backend.h, archive_source_sd_savedata.cpp, archive_extsavedata.cpp and common_paths.h say
# GPLv2 or any later version): NCCHContainer::ReadExtdataId and the exheader storage info layout, the
# ArchiveFormatInfo layout, the SD card folders and metadata file names of the save data and extdata
# archives, SYSTEM_ID, SDCARD_ID and SDMC_DIR.
# Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.cpp)
# Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.h, archive_backend.h, archive.h)
# Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_source_sd_savedata.cpp)
# Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (archive_extsavedata.cpp, common_paths.h)
# Copyright 2013 Dolphin Emulator Project (common_paths.h, second notice)
"""Save and extdata import and export for the native runtime (N3-16, D65).

The player's 3DS save and extdata, as Checkpoint, JKSM and GodMode9 write them (a folder of the
save archive's files: `system`, `user1`...; a folder of the extdata archive's files: `card1`...),
go into and out of the user folder that mh4u-native and Azahar share (D61), in Azahar's layout:

    <user>/sdmc/Nintendo 3DS/<32 zeros>/<32 zeros>/title/00040000/00126300/data/00000001/<files>
    <user>/sdmc/Nintendo 3DS/<32 zeros>/<32 zeros>/title/00040000/00126300/data/00000001.metadata
    <user>/sdmc/Nintendo 3DS/<32 zeros>/<32 zeros>/extdata/00000000/00001263/user/<files>
    <user>/sdmc/Nintendo 3DS/<32 zeros>/<32 zeros>/extdata/00000000/00001263/{boss/, metadata}

    python3 tools/native/saves.py list --user DIR
    python3 tools/native/saves.py import-save SRC --user DIR
    python3 tools/native/saves.py import-extdata SRC --user DIR
    python3 tools/native/saves.py export-save --user DIR DEST
    python3 tools/native/saves.py export-extdata --user DIR DEST

An import first copies what the destination holds to <user>/mh4u-save-backups/<UTC date-time>/ (the
same layout, relative to the user folder), then writes every file to a temporary name, flushes it
and renames it over the old one, so a crash leaves the old file or the new one. Files the source
does not have are removed from the archive afterwards (the archive becomes what the source is; the
backup keeps the old ones). An export copies the files out and refuses a non-empty DEST unless
--force. Close the game first. --json on any command prints one JSON object (as tools/mods.py).
Exit 0 done, 1 refused or failed, 2 usage.
"""
from __future__ import annotations

import argparse
import datetime
import json
import ntpath
import os
import re
import struct
import sys
from typing import Optional

PROGRAM_ID = 0x0004000000126300      # MH4U USA (config/versions.json)

# ── constants ───────────────────────────────────────────────────────────────

# Azahar src/core/hle/service/fs/archive.h: SYSTEM_ID and SDCARD_ID are 32 zeros; src/common/common_paths.h:
# SDMC_DIR "sdmc"; archive_source_sd_savedata.cpp: GetSaveDataPath / GetSaveDataMetadataPath (title id as
# 8+8 lower case hex digits); archive_extsavedata.cpp: GetExtDataContainerPath, GetExtSaveDataPath (ids as
# 8+8 upper case hex digits, then user/, boss/ and the file "metadata").
SYSTEM_ID = SDCARD_ID = "0" * 32
SDMC_DIR = "sdmc"

# The extdata id when <user>/native/game/exheader.bin is absent and --extdata-id is not given. Source: the
# game's unique id 0x1263 (docs/research/g2-save-bytes.md: the cfg:u parameter) and the extdata folder
# `extdata/00000000/00001263/` of docs/native/golden.md, "The save". The dump-check confirms it against
# build/golden/G3/user-before (see docs/SAVES.md and the N3-16 report).
EXTDATA_ID_DEFAULT = 0x0000000000001263

# Azahar src/core/file_sys/archive_backend.h, ArchiveFormatInfo (little endian, 16 bytes):
# u32 total_size, u32 number_directories, u32 number_files, u8 duplicate_data, 3 padding bytes. Azahar writes
# the struct from memory, so the padding is uninitialised there (N1-13's note: those 3 bytes may differ from
# a recording); this tool writes zeros.
FORMAT_INFO_STRUCT = struct.Struct("<IIIB3x")

# What the metadata files hold when the destination has none, per kind: (total_size, number_directories,
# number_files, duplicate_data). The numbers were read with `format-info` from the two metadata files of the
# user-after folders of the G1, G2 and G6 recordings, which agree with each other; they are constants of the
# archive formats Azahar writes for this game, the same for every run. (`python3 tools/native/saves.py format-info
# FILE` prints the four numbers of a metadata file.) --format-info still overrides them.
FORMAT_INFO: dict[str, Optional[tuple[int, int, int, int]]] = {
    "save": (262144, 0, 4, 1),
    "extdata": (4294967295, 64, 64, 0),
}

BACKUPS = "mh4u-save-backups"
TMP_SUFFIX = ".mh4n-tmp"
KINDS = ("save", "extdata")
DELETED_RE = re.compile(r"^.+\.deleted\.\d+\.\d+$")


def _norm(name: str) -> str:
    """The key under which two file names are the same file: Windows and macOS file systems ignore case."""
    return name.casefold() if os.name == "nt" or sys.platform == "darwin" else name


class Error(Exception):
    """Something refused or failed: a code for the launcher, a message for the player."""

    def __init__(self, code: str, message: str, **details):
        super().__init__(message)
        self.code, self.message, self.details = code, message, details


# ── paths ───────────────────────────────────────────────────────────────────

def _long(path: str, nt: Optional[bool] = None) -> str:
    """On Windows a path past the old 260 character limit needs the \\\\?\\ prefix; the other systems are as they are."""
    if nt is None:
        nt = os.name == "nt"
    if not nt:
        return path
    p = ntpath.abspath(path)
    if p.startswith("\\\\?\\"):
        return p
    if len(p) < 240:
        return p
    if p.startswith("\\\\"):
        return "\\\\?\\UNC\\" + p[2:]
    return "\\\\?\\" + p


def _sd_root(user: str) -> str:
    return os.path.join(user, SDMC_DIR, "Nintendo 3DS", SYSTEM_ID, SDCARD_ID)


def save_dir(user: str) -> str:
    high, low = PROGRAM_ID >> 32, PROGRAM_ID & 0xFFFFFFFF
    return os.path.join(_sd_root(user), "title", f"{high:08x}", f"{low:08x}", "data", "00000001")


def save_metadata(user: str) -> str:
    return save_dir(user) + ".metadata"


def extdata_root(user: str, extdata_id: int) -> str:
    return os.path.join(_sd_root(user), "extdata", f"{extdata_id >> 32:08X}", f"{extdata_id & 0xFFFFFFFF:08X}")


def read_exheader_extdata_id(path: str) -> Optional[int]:
    """The extdata id of an exheader.bin, as Azahar's NCCHContainer::ReadExtdataId reads it: the storage info
    of the ACI at 0x200 + 0x30 (ext_save_data_id 8 bytes, system save ids 8, accessible unique ids 8, access info 7,
    other attributes 1); with the extended save data access bit (other attributes >> 1) the first non-zero of the
    six 20-bit ids, the unique-ids field's before the first field's. None when the file has none."""
    with open(_long(path), "rb") as f:
        data = f.read(0x300)
    base = 0x230
    if len(data) < base + 0x20:
        raise Error("bad_exheader", f"{os.path.basename(path)} is too short to be an exheader: run the native "
                        "setup again, or give --extdata-id")
    ext_save = int.from_bytes(data[base:base + 8], "little")
    unique = int.from_bytes(data[base + 0x10:base + 0x18], "little")
    other = data[base + 0x1F]
    if other >> 1:
        ids = [(unique >> 40) & 0xFFFFF, (unique >> 20) & 0xFFFFF, unique & 0xFFFFF,
               (ext_save >> 40) & 0xFFFFF, (ext_save >> 20) & 0xFFFFF, ext_save & 0xFFFFF]
        return next((i for i in ids if i), None)
    return ext_save


def resolve_extdata_id(user: str, option: Optional[int]) -> tuple[int, str]:
    """(id, where it came from): --extdata-id, else <user>/native/game/exheader.bin, else the constant."""
    if option is not None:
        return option, "option"
    exheader = os.path.join(user, "native", "game", "exheader.bin")
    if os.path.isfile(_long(exheader)):
        found = read_exheader_extdata_id(exheader)
        if found:
            return found, "exheader"
    if EXTDATA_ID_DEFAULT is not None:
        return EXTDATA_ID_DEFAULT, "constant"
    raise Error("no_extdata_id", "the extdata id is not known: run the native setup first (it writes "
                "native/game/exheader.bin) or give --extdata-id")


def parse_format_info(text: str) -> tuple[int, int, int, int]:
    try:
        parts = [int(p, 0) for p in text.replace(" ", "").split(",")]
    except ValueError:
        parts = []
    if len(parts) != 4 or any(p < 0 for p in parts) or any(p > 0xFFFFFFFF for p in parts[:3]) or parts[3] > 255:
        raise Error("usage", "--format-info takes four numbers: TOTAL_SIZE,DIRECTORIES,FILES,DUPLICATE_DATA")
    return parts[0], parts[1], parts[2], parts[3]


def read_format_info(path: str) -> tuple[int, int, int, int]:
    with open(_long(path), "rb") as f:
        raw = f.read(FORMAT_INFO_STRUCT.size)
    if len(raw) != FORMAT_INFO_STRUCT.size:
        raise Error("bad_metadata", f"{os.path.basename(path)} is not a 16-byte format information file")
    return FORMAT_INFO_STRUCT.unpack(raw)


# ── files ───────────────────────────────────────────────────────────────────

def _fsync_dir(path: str) -> None:
    if os.name == "nt":
        return
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def atomic_write(path: str, chunks) -> int:
    """Write the chunks to a temporary name beside `path`, flush and fsync, then rename over it (os.replace:
    Windows renames over an existing file too). Returns the bytes written."""
    tmp = path + TMP_SUFFIX
    size = 0
    try:
        with open(_long(tmp), "wb") as f:
            for chunk in chunks:
                f.write(chunk)
                size += len(chunk)
            f.flush()
            os.fsync(f.fileno())
        os.replace(_long(tmp), _long(path))
    except BaseException:
        try:
            os.remove(_long(tmp))
        except OSError:
            pass
        raise
    _fsync_dir(os.path.dirname(path))
    return size


def _remove(path: str) -> None:
    try:
        os.remove(_long(path))
    except OSError:
        pass


def _stage(src: str, tmp: str) -> None:
    """Copy `src` to the temporary name `tmp`, flushed and fsynced; no rename."""
    try:
        with open(_long(tmp), "wb") as f:
            for chunk in _read_chunks(src):
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())
    except BaseException:
        _remove(tmp)
        raise


def _stage_bytes(data: bytes, tmp: str) -> None:
    try:
        with open(_long(tmp), "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except BaseException:
        _remove(tmp)
        raise


def _read_chunks(path: str):
    with open(_long(path), "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                return
            yield chunk


def copy_file(src: str, dst: str) -> int:
    return atomic_write(dst, _read_chunks(src))


def list_files(folder: str, what: str) -> list[tuple[str, int]]:
    """The archive files of a folder, sorted: regular files only. A sub-folder, a link or anything else refuses."""
    out = []
    try:
        names = sorted(os.listdir(_long(folder)))
    except OSError as e:
        raise Error("source", f"cannot read {what} folder {folder}: {e.strerror or e}") from e
    for name in names:
        full = os.path.join(folder, name)
        if name.endswith(TMP_SUFFIX) or DELETED_RE.match(name):
            continue                             # leftovers of a crash: ours, and the native fs's .deleted.<pid>.<n>
        if os.path.islink(_long(full)) or not os.path.isfile(_long(full)):
            raise Error("not_flat", f"{what} folder {folder} holds {name!r}, which is not a plain file: a save "
                        "archive's folder holds files only (no sub-folders)", name=name)
        out.append((name, os.path.getsize(_long(full))))
    return out


def _utcnow() -> datetime.datetime:
    return datetime.datetime.now(datetime.timezone.utc)


def _backup_root(user: str) -> str:
    stamp = _utcnow().strftime("%Y%m%d-%H%M%S")
    base = os.path.join(user, BACKUPS)
    path, n = os.path.join(base, stamp), 1
    while os.path.exists(_long(path)):
        n += 1
        path = os.path.join(base, f"{stamp}-{n}")
    return path


def _backup(user: str, paths: list[str]) -> Optional[str]:
    """Copy each existing file of `paths` to the backup folder, in the same layout relative to the user folder."""
    present = [p for p in paths if os.path.isfile(_long(p))]
    if not present:
        return None
    root = _backup_root(user)
    for p in present:
        rel = os.path.relpath(p, user)
        dst = os.path.join(root, rel)
        os.makedirs(_long(os.path.dirname(dst)), exist_ok=True)
        copy_file(p, dst)
    return root


# ── archives ────────────────────────────────────────────────────────────────

class Archive:
    """One of the two archives in a user folder: where its files and its metadata file are."""

    def __init__(self, kind: str, user: str, extdata_id: Optional[int] = None, id_source: str = ""):
        self.kind, self.user = kind, user
        self.extdata_id, self.id_source = extdata_id, id_source
        if kind == "save":
            self.files, self.metadata = save_dir(user), save_metadata(user)
        else:
            root = extdata_root(user, extdata_id)
            self.files, self.metadata = os.path.join(root, "user"), os.path.join(root, "metadata")
            self.boss = os.path.join(root, "boss")

    def existing(self) -> list[tuple[str, int]]:
        if not os.path.isdir(_long(self.files)):
            return []
        return list_files(self.files, f"the {self.kind}")


def _format_info_for(kind: str, option: Optional[str]) -> Optional[tuple[int, int, int, int]]:
    if option is not None:
        return parse_format_info(option)
    return FORMAT_INFO[kind]


def do_import(kind: str, src: str, user: str, extdata_id: Optional[int], format_option: Optional[str]) -> dict:
    if format_option is not None:
        parse_format_info(format_option)         # a bad option is refused even when the metadata exists
    if not os.path.isdir(_long(src)):
        raise Error("source", f"{src} is not a folder")
    files = list_files(src, "source")
    if not files:
        raise Error("source", f"{src} holds no files")
    ext_id, id_source = (resolve_extdata_id(user, extdata_id) if kind == "extdata" else (None, ""))
    arc = Archive(kind, user, ext_id, id_source)
    old = arc.existing()
    have_metadata = os.path.isfile(_long(arc.metadata))
    info = None
    if not have_metadata:
        info = _format_info_for(kind, format_option)
        if info is None:
            raise Error("no_format_info", f"the {kind} archive has no metadata file here and its format information "
                        "is not known to this tool yet: give --format-info TOTAL_SIZE,DIRECTORIES,FILES,DUPLICATE_DATA "
                        "(the numbers of an existing metadata file; `saves.py format-info FILE` prints them)")
    keys = [_norm(n) for n, _ in files]
    if len(set(keys)) != len(keys):
        raise Error("case_collision", f"{src} holds two files whose names differ only in capital letters, which "
                    "this computer's file system cannot keep apart")
    # Nothing has been touched until here.
    backup = _backup(user, [os.path.join(arc.files, n) for n, _ in old] + ([arc.metadata] if have_metadata else []))
    partly = f"; the {kind} may be partly imported, and the old files are in the backup {backup}" if backup else ""
    os.makedirs(_long(arc.files), exist_ok=True)
    if kind == "extdata":
        os.makedirs(_long(arc.boss), exist_ok=True)
    for name in os.listdir(_long(arc.files)):    # temporary files of an earlier crash
        if name.endswith(TMP_SUFFIX):
            _remove(os.path.join(arc.files, name))
    # Stage: every new file complete on disk under a temporary name. A failure here leaves the archive as it was.
    staged: list[tuple[str, str]] = []           # (temporary path, final path)
    try:
        for name, _ in files:
            final = os.path.join(arc.files, name)
            staged.append((final + TMP_SUFFIX, final))
            _stage(os.path.join(src, name), final + TMP_SUFFIX)
        if not have_metadata:
            final = arc.metadata
            staged.append((final + TMP_SUFFIX, final))
            _stage_bytes(FORMAT_INFO_STRUCT.pack(*info), final + TMP_SUFFIX)
    except OSError as e:
        for tmp, _ in staged:
            _remove(tmp)
        raise Error("write_failed", f"could not write {e.filename or 'a file'}: {e.strerror or e}; the {kind} "
                    "was not changed" + (f" (the old files are also in the backup {backup})" if backup else ""),
                    backup=backup, changed=False) from e
    # Commit: renames, then the files the source lacks.
    removed = []
    try:
        for tmp, final in staged:
            os.replace(_long(tmp), _long(final))
        keep = set(keys)
        for name, _ in old:
            if _norm(name) not in keep:
                os.remove(_long(os.path.join(arc.files, name)))
                removed.append(name)
        _fsync_dir(arc.files)
    except OSError as e:
        for tmp, _ in staged:
            _remove(tmp)
        raise Error("write_failed", f"could not finish the import ({e.filename or 'a file'}: {e.strerror or e})"
                    + partly, backup=backup, changed=True) from e
    written = sum(size for _, size in files)
    wrote_metadata = not have_metadata
    result = {"kind": kind, "files": [{"name": n, "size": sz} for n, sz in files], "bytes": written,
              "destination": arc.files, "metadata": "written" if wrote_metadata else "kept",
              "backup": backup, "removed": removed}
    if ext_id is not None:
        result["extdata_id"] = f"{ext_id:016x}"
        result["extdata_id_source"] = id_source
    return result


def do_export(kind: str, user: str, dest: str, force: bool, extdata_id: Optional[int]) -> dict:
    ext_id, id_source = (resolve_extdata_id(user, extdata_id) if kind == "extdata" else (None, ""))
    arc = Archive(kind, user, ext_id, id_source)
    files = arc.existing()
    if not files:
        raise Error("nothing", f"there is no {kind} in {user} to export")
    if os.path.exists(_long(dest)):
        if not os.path.isdir(_long(dest)):
            raise Error("dest", f"{dest} is not a folder")
        if os.listdir(_long(dest)) and not force:
            raise Error("dest_not_empty", f"{dest} is not empty: give an empty or new folder, or --force to "
                        "write over files of the same names")
    os.makedirs(_long(dest), exist_ok=True)
    written = 0
    for name, _ in files:
        written += copy_file(os.path.join(arc.files, name), os.path.join(dest, name))
    exported = {_norm(n) for n, _ in files}
    left = sorted(n for n in os.listdir(_long(dest)) if _norm(n) not in exported)   # --force leaves these alone
    result = {"kind": kind, "files": [{"name": n, "size": s} for n, s in files], "bytes": written,
              "destination": dest, "left": left}
    if ext_id is not None:
        result["extdata_id"] = f"{ext_id:016x}"
        result["extdata_id_source"] = id_source
    return result


def _archive_summary(arc: "Archive") -> dict:
    """What an archive holds; a folder `list` cannot read as an archive's is reported, not raised."""
    meta = os.path.isfile(_long(arc.metadata))
    try:
        files = [{"name": n, "size": sz} for n, sz in arc.existing()]
    except Error as e:
        return {"path": arc.files, "error": e.message, "files": [], "metadata": meta}
    return {"path": arc.files, "files": files, "metadata": meta}


def do_list(user: str, extdata_id: Optional[int]) -> dict:
    out: dict = {"user": user, "program_id": f"{PROGRAM_ID:016x}", "backups": []}
    out["save"] = _archive_summary(Archive("save", user))
    try:
        ext_id, source = resolve_extdata_id(user, extdata_id)
    except Error as e:
        out["extdata"] = {"error": e.message, "files": [], "metadata": False}
    else:
        out["extdata"] = dict(_archive_summary(Archive("extdata", user, ext_id, source)),
                              extdata_id=f"{ext_id:016x}", extdata_id_source=source)
    base = os.path.join(user, BACKUPS)
    if os.path.isdir(_long(base)):
        out["backups"] = sorted(n for n in os.listdir(_long(base)) if os.path.isdir(_long(os.path.join(base, n))))
    return out


def do_format_info(path: str) -> dict:
    total, dirs, files, dup = read_format_info(path)
    return {"total_size": total, "number_directories": dirs, "number_files": files, "duplicate_data": dup,
            "as_option": f"{total},{dirs},{files},{dup}"}


# ── command line ────────────────────────────────────────────────────────────

def print_text(command: str, r: dict) -> None:
    if command == "list":
        for kind in KINDS:
            a = r[kind]
            if "path" not in a:
                print(f"{kind}: {a['error']}")
                continue
            print(f"{kind}: {a['path']}")
            if "error" in a:
                print(f"  {a['error']}")
                continue
            print(f"  {len(a['files'])} file(s)" + (", metadata present" if a["metadata"] else ", no metadata"))
            for f in a["files"]:
                print(f"  {f['name']}  {f['size']} bytes")
        print(f"backups: {len(r['backups'])}" + ("" if not r["backups"] else "  (" + ", ".join(r["backups"]) + ")"))
    elif command == "format-info":
        print(r["as_option"])
    elif command.startswith("import"):
        print(f"imported {len(r['files'])} file(s), {r['bytes']} bytes, into {r['destination']}")
        print(f"metadata {r['metadata']}" + (f"; removed {', '.join(r['removed'])}" if r["removed"] else ""))
        print(f"backup: {r['backup']}" if r["backup"] else "nothing was there to back up")
    else:
        print(f"exported {len(r['files'])} file(s), {r['bytes']} bytes, to {r['destination']}")
        if r["left"]:
            print(f"left alone in the folder: {', '.join(r['left'])}")


def parse_id(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a number (decimal or 0x hex)") from None


def build_parser(json_errors: bool) -> argparse.ArgumentParser:
    class Parser(argparse.ArgumentParser):
        def error(self, message):
            if json_errors:
                raise Error("usage", message)
            super().error(message)

    def common(p: argparse.ArgumentParser, user: bool = True) -> None:
        p.add_argument("--json", action="store_true", help="print one JSON object (for the launcher)")
        if user:
            p.add_argument("--user", required=True, metavar="DIR", help="the user folder both executables share (D61)")
            p.add_argument("--extdata-id", type=parse_id, default=None,
                           help="the extdata id (default: native/game/exheader.bin's, else the built-in one)")

    ap = Parser(prog="saves.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True, parser_class=Parser)
    p = sub.add_parser("list", help="what the user folder holds")
    common(p)
    for name in ("import-save", "import-extdata"):
        p = sub.add_parser(name, help="a folder of the archive's files into the user folder")
        p.add_argument("src", metavar="SRC")
        common(p)
        p.add_argument("--format-info", metavar="TOTAL,DIRS,FILES,DUP",
                       help="for a metadata file that has to be made: the archive's format information")
    for name in ("export-save", "export-extdata"):
        p = sub.add_parser(name, help="the archive's files out of the user folder into a folder")
        p.add_argument("dest", metavar="DEST")
        common(p)
        p.add_argument("--force", action="store_true", help="write into a DEST that is not empty")
    p = sub.add_parser("format-info", help="print the four numbers of a 16-byte metadata file (numbers only)")
    p.add_argument("file", metavar="FILE")
    common(p, user=False)
    return ap


def main(argv=None) -> int:
    args = sys.argv[1:] if argv is None else list(argv)
    as_json = "--json" in args
    command = next((a for a in args if a in ("list", "import-save", "import-extdata", "export-save",
                                             "export-extdata", "format-info")), None)
    if not as_json and hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(errors="replace")
        except (ValueError, OSError):
            pass
    try:
        ns = build_parser(as_json).parse_args(args)
        command = ns.command
        try:
            if command == "format-info":
                result = do_format_info(ns.file)
            else:
                user = os.path.abspath(ns.user)
                if command == "list":
                    result = do_list(user, ns.extdata_id)
                elif command.startswith("import"):
                    result = do_import(command.split("-")[1], os.path.abspath(ns.src), user, ns.extdata_id,
                                       ns.format_info)
                else:
                    result = do_export(command.split("-")[1], user, os.path.abspath(ns.dest), ns.force,
                                       ns.extdata_id)
        except OSError as e:
            raise Error("io", f"{e.strerror or e}" + (f": {e.filename}" if e.filename else "")) from e
    except Error as e:
        if as_json:
            print(json.dumps({"ok": False, "command": command,
                              "error": dict(e.details, code=e.code, message=e.message)}, indent=1))
        else:
            print(f"saves.py: {e.message}", file=sys.stderr)
        return 2 if e.code == "usage" else 1
    if as_json:
        print(json.dumps(dict(result, ok=True, command=command), indent=1))
    else:
        print_text(command, result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
