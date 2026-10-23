#!/usr/bin/env python3
"""Make the native runtime's game folder from the player's own dump (N1-03).

usage: prepare_game.py --base GAME --update UPDATE [-o DIR]

GAME is the base game (.3ds, .cci or .cxi) and UPDATE the v1.3.0 update title (a .cia) or
its installed content (the .app that Azahar's setup put under the virtual SD card's
title/0004000e/00126300/content/), both decrypted (see tools/extract_ncch.py). DIR defaults
to build/native-game (build/game is tools/build_game.py's). The folder holds:

    exheader.bin       the update's exheader
    code.bin           the update's code, decompressed
    icon.bin           the update's ExeFS 'icon' file (SMDH), when it has one (N1-30: Azahar's
                       ReadIcon reads the update's, not the base game's; none in the update
                       means none here); the cfg service reads its region lockout for
                       --region auto (D38)
    romfs.bin          the base game's RomFS section
    update_romfs.bin   the update's RomFS section
    info.txt           titles, versions, sizes, CRC-32 of code.bin, where icon.bin came from
                       ("icon source" line; a folder without it is stale), and the two input files
                       (path and size) the folder was made from (tools/native/setup_native.py
                       skips this step while they and code.bin are unchanged)

The folder contains game data: it stays in build/ (git-ignored), never commit or share it. The
RomFS sections are gigabytes and are streamed, never read into memory.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import zlib
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import blz  # noqa: E402
import exheader  # noqa: E402
import extract_ncch  # noqa: E402

DEFAULT_OUT = os.path.join("build", "native-game")
DEFAULT_VERSIONS = os.path.join(ROOT, "config", "versions.json")
CHUNK = 1 << 20
SMDH_MAGIC = b"SMDH"
SMDH_SIZE = 0x36C0          # what native's cfg service requires of icon.bin
EXEFS_HEADER_SIZE = 0x200
ICON_SOURCE_KEY = "icon source   : "   # info.txt line: "update", "base game" or "none"
EXEFS_ENTRIES = 10         # 10 entries of name[8], offset, size (3dbrew ExeFS)


class PrepareError(Exception):
    pass


def load_target(versions_path: str) -> dict:
    with open(versions_path) as f:
        data = json.load(f)
    for entry in data["versions"]:
        if entry.get("target"):
            return entry
    raise PrepareError(f"{versions_path} has no target version")


@dataclass(frozen=True)
class UpdateInfo:
    """What the update says about itself: its title and, when it can be read, its version."""
    title_id: int
    title_version: int | None       # None: an installed NCCH with no TMD beside it
    source: str                     # "cia", or "ncch"


def tmd_beside(path: str, title_id: int) -> int | None:
    """The title version of the TMD file beside an installed content (the .tmd next to the .app
    of the virtual SD card), None when there is none for that title. The highest wins."""
    best = None
    folder = os.path.dirname(os.path.abspath(path))
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        return None
    for name in names:
        if not name.lower().endswith(".tmd"):
            continue
        try:
            with open(os.path.join(folder, name), "rb") as f:
                tmd = f.read(0x1000)
        except OSError:
            continue
        if len(tmd) < 4:
            continue
        sig_len = {0x10000: 0x200 + 0x3C, 0x10001: 0x100 + 0x3C, 0x10002: 0x3C + 0x40,
                   0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}.get(
            struct.unpack_from(">I", tmd, 0)[0])
        if sig_len is None or len(tmd) < 4 + sig_len + 0x9E:
            continue
        th = 4 + sig_len
        if struct.unpack_from(">Q", tmd, th + 0x4C)[0] != title_id:
            continue
        version = struct.unpack_from(">H", tmd, th + 0x9C)[0]
        best = version if best is None else max(best, version)
    return best


def check_update(update_path: str, target: dict) -> UpdateInfo:
    """The update must be a CIA of the target title and version (v1.3.0), or the content an
    installation of that CIA left on the virtual SD card (an NCCH: its title is checked, and its
    version too when the TMD that sits beside it can be read)."""
    want_id = int(target["update_title_id"], 16)
    want_version = int(target["title_version"])
    try:
        with open(update_path, "rb") as f:
            kind = extract_ncch.detect(f)
            if kind == "cia":
                content = extract_ncch.cia_content0(f)
                found = UpdateInfo(content.title_id, content.title_version, "cia")
            elif kind == "ncch":
                found = UpdateInfo(extract_ncch.parse_ncch(f, 0).program_id, None, "ncch")
            else:
                raise PrepareError(f"{update_path} is not a CIA or an installed update: --update takes "
                                   "the v1.3.0 update title (a .cia) or its installed content (a .app)")
    except extract_ncch.DumpError as e:
        raise PrepareError(str(e)) from e
    if found.source == "ncch":
        # an installed content is the update title's: the base game's own NCCH is not it
        if found.title_id != want_id:
            raise PrepareError(f"the update is title {found.title_id:016X}, expected {want_id:016X}")
        found = UpdateInfo(want_id, tmd_beside(update_path, want_id), "ncch")
        if found.title_version is None:
            return found
    elif found.title_id != want_id:
        raise PrepareError(f"the update is title {found.title_id:016X}, expected {want_id:016X}")
    if found.title_version != want_version:
        v = found.title_version
        raise PrepareError(f"the update is v{v >> 10}.{(v >> 4) & 0x3F}.{v & 0xF}, but only "
                           f"{target['name'].split(' (')[0]} is supported")
    return found


def crc32_of_file(path: str) -> int:
    crc = 0
    with open(path, "rb") as f:
        while block := f.read(CHUNK):
            crc = zlib.crc32(block, crc)
    return crc & 0xFFFFFFFF


def extract_to(src: str, outdir: str, label: str) -> list[str]:
    try:
        return extract_ncch.extract(src, outdir, with_romfs=True)
    except (extract_ncch.DumpError, OSError) as e:
        raise PrepareError(f"{label}: {e}") from e


def exefs_entry(exefs: bytes, wanted: str, label: str = "base game") -> bytes | None:
    """The bytes of one ExeFS file, None when the ExeFS has no such file.

    A damaged header (too short, an entry running past the end) is an error: it must not look
    like a game without that file. LABEL names the title in the messages."""
    if len(exefs) < EXEFS_HEADER_SIZE:
        raise PrepareError(f"the {label}'s ExeFS is damaged: 0x{len(exefs):X} bytes, smaller "
                           f"than its 0x{EXEFS_HEADER_SIZE:X}-byte header")
    found = None
    seen = False            # Azahar's LoadSectionExeFS stops at the FIRST entry with the name
    for i in range(EXEFS_ENTRIES):
        raw, off, size = struct.unpack_from("<8sII", exefs, i * 16)
        # Azahar compares the name with strcmp: it ends at the first NUL
        name = raw.split(b"\0", 1)[0].decode("ascii", errors="replace")
        is_wanted = name == wanted
        # Like Azahar's LoadSectionExeFS and extract_ncch, only an entry that has data or is the
        # one we read is checked: a bogus offset on an unused (size 0) entry is not an error
        if (size or is_wanted) and EXEFS_HEADER_SIZE + off + size > len(exefs):
            raise PrepareError(f"the {label}'s ExeFS is damaged: entry {name!r} (offset "
                               f"0x{off:X}, size 0x{size:X}) runs past the end of the ExeFS")
        if is_wanted and not seen:
            seen = True
            if size:
                found = exefs[EXEFS_HEADER_SIZE + off:EXEFS_HEADER_SIZE + off + size]
    return found


def read_icon(exefs_dir: str, label: str = "base game") -> bytes | None:
    """The ExeFS 'icon' (SMDH) of the title extracted into EXEFS_DIR, None when it has none."""
    with open(os.path.join(exefs_dir, "exefs.bin"), "rb") as f:
        icon = exefs_entry(f.read(), "icon", label)
    if icon is None:
        return None
    if len(icon) < SMDH_SIZE:
        raise PrepareError(f"the {label}'s icon is damaged: 0x{len(icon):X} bytes (an SMDH is "
                           f"0x{SMDH_SIZE:X} bytes)")
    return icon


def choose_icon(base_dir: str, update_dir: str | None) -> tuple[bytes | None, str]:
    """The icon the game folder gets and where it comes from: "update", "base game" or "none".

    Azahar's AppLoader_NCCH::ReadIcon reads the ExeFS of overlay_ncch, which is the update's NCCH
    whenever the update is loaded (and it is, for this game). When the update has no 'icon' the
    read fails (ErrorNotUsed) and Azahar does not go back to the base game's: the same here, the
    folder gets no icon. Without an update the base game's is read, as the only NCCH there is."""
    if update_dir is None:
        icon = read_icon(base_dir, "base game")
        return icon, ("base game" if icon is not None else "none")
    icon = read_icon(update_dir, "update")
    return icon, ("update" if icon is not None else "none")


def icon_warning(icon: bytes, label: str = "update") -> str | None:
    """A missing SMDH magic is only a warning: Azahar (ParseRegionLockoutInfo) and native's cfg
    check the size alone, so a game that runs there must not be refused here."""
    if icon[:4] != SMDH_MAGIC:
        return f"the {label}'s icon has no SMDH magic; icon.bin is written anyway, as Azahar would read it"
    return None


def read_exheader(path: str) -> exheader.ExHeader:
    with open(path, "rb") as f:
        return exheader.parse(f.read())


def prepare(base: str, update: str, outdir: str, versions: str = DEFAULT_VERSIONS) -> list[str]:
    target = load_target(versions)
    content = check_update(update, target)
    want_pid = int(target["program_id"], 16)

    os.makedirs(outdir, exist_ok=True)
    work = os.path.join(outdir, ".prepare-tmp")
    shutil.rmtree(work, ignore_errors=True)
    try:
        base_dir, upd_dir = os.path.join(work, "base"), os.path.join(work, "update")
        extract_to(base, base_dir, "base game")
        extract_to(update, upd_dir, "update")
        base_hdr = read_exheader(os.path.join(base_dir, "exheader.bin"))
        upd_hdr = read_exheader(os.path.join(upd_dir, "exheader.bin"))
        if base_hdr.program_id != want_pid:
            raise PrepareError(f"the base game is program {base_hdr.program_id:016X}, expected "
                               f"{want_pid:016X}")
        if upd_hdr.program_id != want_pid:
            raise PrepareError(f"the update's code is program {upd_hdr.program_id:016X}, expected "
                               f"{want_pid:016X}")
        for d, label in ((base_dir, "base game"), (upd_dir, "update")):
            if not os.path.exists(os.path.join(d, "romfs.bin")):
                raise PrepareError(f"the {label} has no RomFS")

        with open(os.path.join(upd_dir, "code.bin"), "rb") as f:
            code = f.read()
        if upd_hdr.code_compressed:
            try:
                code = blz.decompress(code)
            except blz.BLZError as e:
                raise PrepareError(f"update code.bin: {e}") from e
        # The text, ro and data segments must fit in the decompressed code.
        have = len(code)
        if have < upd_hdr.text.size + upd_hdr.ro.size + upd_hdr.data.size:
            raise PrepareError(f"decompressed code is 0x{have:X} bytes, smaller than its "
                               "segments; is the exheader from the same title?")

        icon, icon_source = choose_icon(base_dir, upd_dir)
        warning = icon_warning(icon, icon_source) if icon is not None else None
        if warning:
            print(f"warning: {warning}", file=sys.stderr)
        if icon is None:
            print("warning: the update has no ExeFS icon: icon.bin is not written, as Azahar's "
                  "ReadIcon finds none either (it does not use the base game's)", file=sys.stderr)
        with open(os.path.join(outdir, "code.bin"), "wb") as f:
            f.write(code)
        icon_path = os.path.join(outdir, "icon.bin")
        if icon is not None:
            with open(icon_path, "wb") as f:
                f.write(icon)
        elif os.path.exists(icon_path):
            os.remove(icon_path)        # a stale icon of an earlier run must not outlive it
        with open(os.path.join(outdir, "exheader.bin"), "wb") as f:
            with open(os.path.join(upd_dir, "exheader.bin"), "rb") as src:
                f.write(src.read())
        os.replace(os.path.join(base_dir, "romfs.bin"), os.path.join(outdir, "romfs.bin"))
        os.replace(os.path.join(upd_dir, "romfs.bin"), os.path.join(outdir, "update_romfs.bin"))

        v = content.title_version
        version = (f"v{v >> 10}.{(v >> 4) & 0x3F}.{v & 0xF} (title version {v})" if v is not None
                   else "installed content, version not in it")
        info = [
            f"base game     : {base_hdr.title}, program {base_hdr.program_id:016X}",
            f"update        : {upd_hdr.title}, title {content.title_id:016X}, {version}",
            f"exheader.bin  : 0x{os.path.getsize(os.path.join(outdir, 'exheader.bin')):X} bytes",
            f"code.bin      : 0x{have:X} bytes (was compressed: "
            f"{upd_hdr.code_compressed}), CRC-32 {crc32_of_file(os.path.join(outdir, 'code.bin')):08X}",
            (f"icon.bin      : 0x{len(icon):X} bytes, CRC-32 {zlib.crc32(icon) & 0xFFFFFFFF:08X}"
             + (f" (WARNING: {warning})" if warning else "")
             if icon is not None else "icon          : none"),
            f"{ICON_SOURCE_KEY}{icon_source}",
            f"romfs.bin     : 0x{os.path.getsize(os.path.join(outdir, 'romfs.bin')):X} bytes",
            f"update_romfs.bin: 0x{os.path.getsize(os.path.join(outdir, 'update_romfs.bin')):X} bytes",
            f"input base  : {base}, 0x{os.path.getsize(base):X} bytes",
            f"input update: {update}, 0x{os.path.getsize(update):X} bytes",
        ]
        with open(os.path.join(outdir, "info.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(info) + "\n")
        return info
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True, help="base game: .3ds, .cci or .cxi")
    ap.add_argument("--update", required=True,
                    help="the v1.3.0 update title (.cia) or its installed content (.app)")
    ap.add_argument("-o", "--outdir", default=DEFAULT_OUT)
    ap.add_argument("--versions", default=DEFAULT_VERSIONS, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    try:
        info = prepare(args.base, args.update, args.outdir, args.versions)
    except PrepareError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print("\n".join(info))
    print(f"game folder written to {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
