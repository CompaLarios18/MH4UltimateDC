#!/usr/bin/env python3
"""Make the native runtime's game folder from the player's own dump (N1-03).

usage: prepare_game.py --base GAME --update UPDATE.cia [-o DIR]

GAME is the base game (.3ds, .cci or .cxi) and UPDATE.cia the v1.3.0 update title, both
decrypted (see tools/extract_ncch.py). DIR defaults to build/native-game (build/game is
tools/build_game.py's). The folder holds:

    exheader.bin       the update's exheader
    code.bin           the update's code, decompressed
    romfs.bin          the base game's RomFS section
    update_romfs.bin   the update's RomFS section
    info.txt           titles, versions, sizes, CRC-32 of code.bin

The folder contains game data: it stays in build/ (git-ignored), never commit or share it. The
RomFS sections are gigabytes and are streamed, never read into memory.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import blz  # noqa: E402
import exheader  # noqa: E402
import extract_ncch  # noqa: E402

DEFAULT_OUT = os.path.join("build", "native-game")
DEFAULT_VERSIONS = os.path.join(ROOT, "config", "versions.json")
CHUNK = 1 << 20


class PrepareError(Exception):
    pass


def load_target(versions_path: str) -> dict:
    with open(versions_path) as f:
        data = json.load(f)
    for entry in data["versions"]:
        if entry.get("target"):
            return entry
    raise PrepareError(f"{versions_path} has no target version")


def check_update(update_path: str, target: dict) -> extract_ncch.CiaContent0:
    """The update must be a CIA of the target title and version (v1.3.0)."""
    try:
        with open(update_path, "rb") as f:
            if extract_ncch.detect(f) != "cia":
                raise PrepareError(f"{update_path} is not a CIA: --update takes the v1.3.0 update "
                                   "title (a .cia)")
            content = extract_ncch.cia_content0(f)
    except extract_ncch.DumpError as e:
        raise PrepareError(str(e)) from e
    want_id = int(target["update_title_id"], 16)
    want_version = int(target["title_version"])
    if content.title_id != want_id:
        raise PrepareError(f"the update is title {content.title_id:016X}, expected {want_id:016X}")
    if content.title_version != want_version:
        v = content.title_version
        raise PrepareError(f"the update is v{v >> 10}.{(v >> 4) & 0x3F}.{v & 0xF}, but only "
                           f"{target['name'].split(' (')[0]} is supported")
    return content


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

        with open(os.path.join(outdir, "code.bin"), "wb") as f:
            f.write(code)
        with open(os.path.join(outdir, "exheader.bin"), "wb") as f:
            with open(os.path.join(upd_dir, "exheader.bin"), "rb") as src:
                f.write(src.read())
        os.replace(os.path.join(base_dir, "romfs.bin"), os.path.join(outdir, "romfs.bin"))
        os.replace(os.path.join(upd_dir, "romfs.bin"), os.path.join(outdir, "update_romfs.bin"))

        v = content.title_version
        info = [
            f"base game     : {base_hdr.title}, program {base_hdr.program_id:016X}",
            f"update        : {upd_hdr.title}, title {content.title_id:016X}, "
            f"v{v >> 10}.{(v >> 4) & 0x3F}.{v & 0xF} (title version {v})",
            f"exheader.bin  : 0x{os.path.getsize(os.path.join(outdir, 'exheader.bin')):X} bytes",
            f"code.bin      : 0x{have:X} bytes (was compressed: "
            f"{upd_hdr.code_compressed}), CRC-32 {crc32_of_file(os.path.join(outdir, 'code.bin')):08X}",
            f"romfs.bin     : 0x{os.path.getsize(os.path.join(outdir, 'romfs.bin')):X} bytes",
            f"update_romfs.bin: 0x{os.path.getsize(os.path.join(outdir, 'update_romfs.bin')):X} bytes",
        ]
        with open(os.path.join(outdir, "info.txt"), "w") as f:
            f.write("\n".join(info) + "\n")
        return info
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True, help="base game: .3ds, .cci or .cxi")
    ap.add_argument("--update", required=True, help="the v1.3.0 update title (.cia)")
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
