#!/usr/bin/env python3
# Derived from Azahar src/video_core/custom_textures/custom_tex_manager.cpp at
# 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; the file says "Licensed under GPLv2
# or any later version"): how a texture pack is read (the file names, the "textures" and "options"
# of pack.json, what no pack.json means).
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (custom_tex_manager.cpp)
"""Which of a capture's textures does an HD texture pack replace? (N2-14)

    hd_hash_check.py HASHES PACK_DIR [--mode auto|new|old] [--list]

HASHES is the output of the capture player's `--check tex-hashes` (native/src/gpu/texture/
tex_hashes.h): lines `<hash> <w>x<h> <format>` in two sections, `# use_new_hash=1` (CityHash64
of the raw bytes) and `# use_new_hash=0` (of the decoded texture). PACK_DIR is a pack folder in
the layout Azahar loads (load/textures/<title id>/): texture files named
`tex1_<w>x<h>_<hash>_<format>[_mip<n>].png` (or .dds, .ktx), and an optional pack.json.

Azahar looks a texture up by its hash alone, with the hash mode the pack asks for:
`options.use_new_hash` of pack.json, and the old mode when the pack has no pack.json. The report
says how many of the captured textures have a file in the pack, under that mode.

Rules taken from Azahar's CustomTexManager:
  - a file name with more than three dot-separated parts is ignored; the last part is the
    extension (png, dds, ktx; anything else is ignored; dds is ignored when the pack skips
    mipmaps, which is the case without a pack.json); a second-to-last part is a material type
    (`norm`) and the file is an extra map of the same texture;
  - the hash of a file comes from its name (`tex1_%ux%u_%llX_%u`, trailing text such as `_mip0`
    allowed) and from the "textures" of pack.json, which maps a hash (hex) to a file name or a
    list of them; a file whose own name is listed there answers to those hashes too.

Exit status: 0 when the report was made, 2 when a file cannot be read or pack.json is unusable.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

HASH_LINE = re.compile(r"^([0-9A-Fa-f]{1,16}) (\d+)x(\d+) (\d+)$")
SECTION = re.compile(r"^# use_new_hash=([01])$")
STEM = re.compile(r"^tex1_(\d+)x(\d+)_([0-9A-Fa-f]+)_(\d+)")
HEX_PREFIX = re.compile(r"^\s*(?:0[xX])?([0-9A-Fa-f]+)")
EXTENSIONS = {"png", "dds", "ktx"}
MASK64 = (1 << 64) - 1


class ToolError(Exception):
    """A file that cannot be used; the message is printed and the exit status is 2."""


@dataclass(frozen=True)
class Captured:
    hash: int
    width: int
    height: int
    format: int


@dataclass
class Pack:
    use_new_hash: bool = False
    has_pack_json: bool = False
    skip_mipmap: bool = True
    # hash -> the files (relative paths) that answer to it
    files: dict[int, list[str]] = field(default_factory=dict)


def parse_hashes(text: str) -> dict[int, list[Captured]]:
    """The two sections of a tex-hashes file: {1: new-hash lines, 0: old-hash lines}."""
    sections: dict[int, list[Captured]] = {}
    current: int | None = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        match = SECTION.match(line)
        if match:
            current = int(match.group(1))
            sections.setdefault(current, [])
            continue
        if line.startswith("#"):
            continue
        match = HASH_LINE.match(line)
        if not match:
            raise ToolError(f"hashes file, line {number}: not '<hash> <w>x<h> <format>': {line[:60]!r}")
        if current is None:
            raise ToolError(f"hashes file, line {number}: a texture before any '# use_new_hash=' section")
        sections[current].append(Captured(int(match.group(1), 16), int(match.group(2)),
                                          int(match.group(3)), int(match.group(4))))
    if not sections:
        raise ToolError("hashes file: no '# use_new_hash=' section (is it the output of --check tex-hashes?)")
    return sections


def _parse_key(key: str) -> int | None:
    """std::stoull(key, &idx, 16): the leading hex digits; none means the key is invalid."""
    match = HEX_PREFIX.match(key)
    if not match:
        return None
    return int(match.group(1), 16) & MASK64


def read_pack_json(path: str) -> tuple[bool, bool, dict[str, list[int]]]:
    """(use_new_hash, skip_mipmap, {file name: [hashes]}) of a pack.json."""
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError) as error:
        raise ToolError(f"{path}: cannot read pack.json: {error}")
    options = data.get("options") if isinstance(data, dict) else None
    if not isinstance(options, dict):
        raise ToolError(f"{path}: no \"options\" object")
    flags = {}
    for name in ("skip_mipmap", "flip_png_files", "use_new_hash"):
        value = options.get(name)
        if not isinstance(value, bool):
            raise ToolError(f"{path}: options.{name} is missing or not a boolean")
        flags[name] = value
    mapping: dict[str, list[int]] = {}
    textures = data.get("textures", {})
    if not isinstance(textures, dict):
        raise ToolError(f"{path}: \"textures\" is not an object")
    for key, value in textures.items():
        digest = _parse_key(key)
        if digest is None:
            continue  # Azahar: "Key is invalid, skipping"
        names = [value] if isinstance(value, str) else value if isinstance(value, list) else []
        for entry in names:
            if isinstance(entry, str):
                mapping.setdefault(entry.replace("\\", "/").rsplit("/", 1)[-1], []).append(digest)
    return flags["use_new_hash"], flags["skip_mipmap"], mapping


def scan_pack(pack_dir: str) -> Pack:
    if not os.path.isdir(pack_dir):
        raise ToolError(f"{pack_dir}: not a directory")
    pack = Pack()
    mapping: dict[str, list[int]] = {}
    config = os.path.join(pack_dir, "pack.json")
    if os.path.isfile(config):
        pack.has_pack_json = True
        pack.use_new_hash, pack.skip_mipmap, mapping = read_pack_json(config)
    # without a pack.json: the legacy defaults (old hash, skip mipmaps)

    for root, _dirs, names in os.walk(pack_dir):
        for name in sorted(names):
            relative = os.path.relpath(os.path.join(root, name), pack_dir).replace(os.sep, "/")
            parts = name.split(".")
            if len(parts) > 3 or len(parts) < 2:
                continue
            extension = parts.pop()
            if extension not in EXTENSIONS:
                continue
            if extension == "dds" and pack.skip_mipmap:
                continue
            if len(parts) > 1:
                parts.pop()  # a material type such as "norm"
            hashes = list(mapping.get(name, []))
            match = STEM.match(parts[-1] if parts else "")
            if match:
                digest = int(match.group(3), 16) & MASK64
                if digest not in hashes:
                    hashes.append(digest)
            for digest in hashes:
                pack.files.setdefault(digest, []).append(relative)
    return pack


def report(sections: dict[int, list[Captured]], pack: Pack, mode: str, listing: bool) -> tuple[str, int]:
    if mode == "auto":
        use_new = pack.use_new_hash
        why = "pack.json" if pack.has_pack_json else "no pack.json: Azahar's legacy default"
    else:
        use_new = mode == "new"
        why = "--mode"
    key = 1 if use_new else 0
    if key not in sections:
        raise ToolError(f"the hashes file has no '# use_new_hash={key}' section")
    textures = sections[key]
    found = [t for t in textures if t.hash in pack.files]
    lines = [
        f"hash mode: use_new_hash={key} ({why})",
        f"pack files with a hash: {sum(len(v) for v in pack.files.values())} for {len(pack.files)} hashes",
        f"captured textures: {len(textures)}",
        f"with a file in the pack: {len(found)}",
        f"without: {len(textures) - len(found)}",
    ]
    if listing:
        for texture in textures:
            files = pack.files.get(texture.hash)
            label = "found  " if files else "missing"
            suffix = f" {', '.join(files)}" if files else ""
            lines.append(f"{label} {texture.hash:016X} {texture.width}x{texture.height} "
                         f"{texture.format}{suffix}")
    return "\n".join(lines) + "\n", len(found)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Which captured textures does an HD pack replace?")
    parser.add_argument("hashes", help="output of the capture player's --check tex-hashes")
    parser.add_argument("pack_dir", help="the texture pack folder")
    parser.add_argument("--mode", choices=("auto", "new", "old"), default="auto",
                        help="hash mode to use (auto: as Azahar picks it for this pack)")
    parser.add_argument("--list", action="store_true", help="one line per captured texture")
    args = parser.parse_args(argv)
    try:
        try:
            with open(args.hashes, encoding="utf-8") as handle:
                text = handle.read()
        except (OSError, UnicodeDecodeError) as error:
            raise ToolError(f"{args.hashes}: {error}")
        text_report, _found = report(parse_hashes(text), scan_pack(args.pack_dir), args.mode, args.list)
    except ToolError as error:
        print(f"hd_hash_check: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(text_report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
