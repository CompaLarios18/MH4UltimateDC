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
schedule.h, hooks.cpp, hooks.h, platform.h) get the include rule too, and only the ones native/
uses are checked (and only if they exist):

  - a mention of port/src/<file> in a file under native/ checks that file; a .cpp and its .h
    are a pair, so naming one checks both. A file that is not on the list above (for example
    port/src/arm_recomp.cpp) is checked too when native/ names it: it is Azahar-bound and is
    reported if it includes Azahar headers;
  - a mention of the bare directory (port/src with no file name, a glob such as port/src/*.cpp
    or a variable such as port/src/${X}.cpp) checks every file on the list, because any of
    them could be pulled in; so does a CMake file that holds the port directory in a variable
    (`set(P "${ROOT}/port")`, then `${P}/src/hooks.cpp`) or builds a path under port/ from a
    variable; `//`, `/./`, `/x/../` and backslashes in a path are normalised before the text is
    read;
  - the includes (quoted or in angle brackets) of a checked file that name a file of port/src
    are checked too (and theirs, and so on; absolute paths and paths that leave port/src are not
    followed), so a header that is not named still cannot bring Azahar in;
  - a native/ file that mentions nothing from port/src checks nothing from it.

Best effort, not a sandbox: a symlink under native/ that points into port/src, an upper-case
spelling, a source list kept in a data file (file(STRINGS ...), a .cmake.in) and an include that
only resolves through -I port/src are not seen. The maintainer reads changes to native/ CMake
files and to this script.

A mention in a CMake comment is ignored; one in a comment of a .cpp/.h file counts (it is
conservative: the file is checked although nothing may use it).

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
    # Azahar's own externals (its src/ and externals/ trees), which a native file has no reason to name
    "nihstro/",
    "teakra/",
    "dynarmic/",
    "soundtouch/",
    "cryptopp/",
    "xbyak/",
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
PORT_AZAHAR_RE = re.compile(r"(?<![A-Za-z0-9_.+\-])port/azahar(?![A-Za-z0-9_])")
PORT_SRC_RE = re.compile(r"(?<![A-Za-z0-9_.+\-])port/src(?:/([A-Za-z0-9_][A-Za-z0-9_.+\-/]*))?")
# `port` as the last component of a path (the directory held in a variable, with or without a
# trailing slash, or in a list) or followed by a variable component, in a CMake file. A bare word
# `port` ("the native port") is not a path and does not match.
PORT_DIR_VARIABLE_RE = re.compile(r'(?<=/)port(?:(?=["\s);])|/(?=["\s);$]|$))', re.MULTILINE)
PATH_NOISE_RE = re.compile(r"/(?:\.?/)+")
PARENT_RE = re.compile(r"/(?!\.\.)[^/\s\"';()]+/\.\./")


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


def _normalise_path(text: str) -> str:
    """The spellings of a path that reach the same file made equal: backslashes become '/', '//'
    and '/./' collapse, '/x/../' is resolved."""
    text = text.replace("\\", "/")
    text = PATH_NOISE_RE.sub("/", text)
    while (collapsed := PARENT_RE.sub("/", text)) != text:
        text = collapsed
    return text


def check_includes(path: Path, shown: str) -> list[str]:
    lines = _read(path)
    problems: list[str] = []
    for number, line in enumerate(lines or [], 1):
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        target = _normalise_path(match.group(1).strip())
        while target.startswith("./"):
            target = target[2:]
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
        if PORT_AZAHAR_RE.search(_normalise_path(code)):
            problems.append(f"{shown}:{number}: uses a path under port/azahar/")
    return problems


def _is_cmake(path: Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix in CMAKE_SUFFIXES


def port_src_mentions(text: str, cmake: bool = False) -> tuple[set[str], bool]:
    """(files of port/src named in the text, whether the bare directory is mentioned). `cmake`:
    the text is a CMake file, where the port directory may be held in a variable."""
    named: set[str] = set()
    text = _normalise_path(text)
    bare = cmake and bool(PORT_DIR_VARIABLE_RE.search(text))
    for match in PORT_SRC_RE.finditer(text):
        rel = (match.group(1) or "").rstrip(".-/+")
        after = text[match.end():match.end() + 1]
        base = rel.rsplit("/", 1)[-1]
        if not rel or "." not in base.strip(".") or after in ("*", "$", "{", "?", "["):
            bare = True
        else:
            named.add(rel)
            stem, dot, suffix = rel.rpartition(".")
            if suffix in ("cpp", "h"):
                named.add(f"{stem}.{'h' if suffix == 'cpp' else 'cpp'}")
    return named, bare


def check(root: Path) -> tuple[list[str], int]:
    """Returns (offences, number of files looked at)."""
    native = root / "native"
    problems: list[str] = []
    count = 0
    named: set[str] = set()
    bare = False
    for dirpath, _dirnames, filenames in os.walk(native):
        for name in sorted(filenames):
            path = Path(dirpath) / name
            shown = path.relative_to(root).as_posix()
            is_cmake = _is_cmake(path)
            if is_cmake:
                count += 1
                problems += check_cmake(path, shown)
                text = "\n".join(_strip_cmake_comment(l) for l in _read(path) or [])
            elif path.suffix in SOURCE_SUFFIXES:
                count += 1
                problems += check_includes(path, shown)
                text = "\n".join(_read(path) or [])
            else:
                continue
            found, found_bare = port_src_mentions(text, is_cmake)
            named |= found
            bare = bare or found_bare
    wanted = set(named)
    if bare:
        wanted |= set(PORT_SRC_FILES)
    src = root / "port" / "src"
    queue = sorted(wanted)
    seen: set[str] = set()
    while queue:
        rel = queue.pop()
        if rel in seen:
            continue
        seen.add(rel)
        path = src / rel
        try:
            if not path.is_file():
                continue
        except OSError:                                       # a name too long for the file system
            continue
        count += 1
        problems += check_includes(path, f"port/src/{rel}")
        for line in _read(path) or []:
            match = INCLUDE_RE.match(line)
            if match:
                target = match.group(1).strip().replace("\\", "/")
                if os.path.isabs(target):
                    continue
                local = os.path.normpath(os.path.join(os.path.dirname(rel), target))
                try:
                    if not local.startswith("..") and (src / local).is_file():
                        queue.append(local.replace(os.sep, "/"))
                except OSError:
                    pass
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
