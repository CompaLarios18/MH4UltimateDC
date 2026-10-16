#!/usr/bin/env python3
"""Checks that native/ does not depend on Azahar (docs/native/wp/N1.md, N1-01).

The native port is Azahar's behaviour, not Azahar's code: individual functions may be copied
with a provenance header (D6), but no Azahar module, header or library is used. Exits 1,
listing each offence as `path:line: reason`, if

  - a file under native/ includes a path that starts with core/, video_core/, audio_core/,
    common/, network/, input_common/, web_service/ or citra;
  - a CMakeLists.txt or .cmake file under native/ names an Azahar target (citra_common,
    citra_core, video_core, audio_core, network, input_common, web_service) or a path under
    port/azahar/.

Files of port/src/ that native/ compiles or includes (fiber.cpp, fiber.h, schedule.cpp,
schedule.h, hooks.cpp, hooks.h, platform.h) get the include rule too. They count as used when
a file under native/ mentions port/src, and they are checked only if they exist.

usage: check_native_deps.py [--root DIR]      DIR is the repository root (default: this checkout)
exit status: 0 clean, 1 offences, 2 no native/ directory under DIR
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

FORBIDDEN_INCLUDE_PREFIXES = (
    "core/",
    "video_core/",
    "audio_core/",
    "common/",
    "network/",
    "input_common/",
    "web_service/",
    "citra",
)
AZAHAR_TARGETS = (
    "citra_common",
    "citra_core",
    "video_core",
    "audio_core",
    "network",
    "input_common",
    "web_service",
)
PORT_SRC_FILES = (
    "fiber.cpp",
    "fiber.h",
    "schedule.cpp",
    "schedule.h",
    "hooks.cpp",
    "hooks.h",
    "platform.h",
)

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".m", ".mm"}
CMAKE_SUFFIXES = {".cmake"}

INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]')
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_.+\-]*")
PORT_AZAHAR_RE = re.compile(r"port/azahar(?![A-Za-z0-9_])")


def _read(path: Path) -> list[str] | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None


def _strip_cmake_comment(line: str) -> str:
    """The line without its `#` comment (a `#` inside double quotes is kept)."""
    in_quote = False
    for i, ch in enumerate(line):
        if ch == '"':
            in_quote = not in_quote
        elif ch == "#" and not in_quote:
            return line[:i]
    return line


def check_includes(path: Path, shown: str) -> list[str]:
    lines = _read(path)
    problems: list[str] = []
    for number, line in enumerate(lines or [], 1):
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        target = match.group(1).strip()
        for prefix in FORBIDDEN_INCLUDE_PREFIXES:
            if target.startswith(prefix):
                problems.append(f"{shown}:{number}: includes '{target}' (Azahar's '{prefix}' headers)")
                break
    return problems


def check_cmake(path: Path, shown: str) -> list[str]:
    lines = _read(path)
    problems: list[str] = []
    for number, line in enumerate(lines or [], 1):
        code = _strip_cmake_comment(line)
        for token in TOKEN_RE.findall(code):
            if token in AZAHAR_TARGETS:
                problems.append(f"{shown}:{number}: names the Azahar target '{token}'")
        if PORT_AZAHAR_RE.search(code):
            problems.append(f"{shown}:{number}: uses a path under port/azahar/")
    return problems


def _is_cmake(path: Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix in CMAKE_SUFFIXES


def check(root: Path) -> tuple[list[str], int]:
    """Returns (offences, number of files looked at)."""
    native = root / "native"
    problems: list[str] = []
    count = 0
    mentions_port_src = False
    for dirpath, _dirnames, filenames in os.walk(native):
        for name in sorted(filenames):
            path = Path(dirpath) / name
            shown = path.relative_to(root).as_posix()
            if _is_cmake(path):
                count += 1
                problems += check_cmake(path, shown)
                lines = _read(path) or []
                mentions_port_src = mentions_port_src or any("port/src" in _strip_cmake_comment(l) for l in lines)
            elif path.suffix in SOURCE_SUFFIXES:
                count += 1
                problems += check_includes(path, shown)
                lines = _read(path) or []
                mentions_port_src = mentions_port_src or any("port/src" in l for l in lines)
    if mentions_port_src:
        for name in PORT_SRC_FILES:
            path = root / "port" / "src" / name
            if path.is_file():
                count += 1
                problems += check_includes(path, f"port/src/{name}")
    return sorted(problems), count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="native/ must not depend on Azahar")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    if not (args.root / "native").is_dir():
        print(f"check_native_deps: no native/ directory under {args.root}", file=sys.stderr)
        return 2
    problems, count = check(args.root)
    if problems:
        for problem in problems:
            print(problem)
        print(f"check_native_deps: {len(problems)} offence(s) in {count} files", file=sys.stderr)
        return 1
    print(f"check_native_deps: ok ({count} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
