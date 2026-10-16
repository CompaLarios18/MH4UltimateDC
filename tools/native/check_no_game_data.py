#!/usr/bin/env python3
"""Refuse commits that contain game data or files derived from it.

Usage:
    check_no_game_data.py --staged          check what is staged in the index
    check_no_game_data.py --range A..B      check the files added or changed between A and B
    check_no_game_data.py --range A...B     same, from the merge base of A and B

Rules (each offending path is printed with the rule it broke; exit status 1):
    ci-pattern    the patterns of the guard in .github/workflows/ci.yml
    forbidden-dir anything under build/, baserom/ or export/ (at any depth)
    extension     .ppm, .bin and .wav files
    magic         files that start with the capture magics MGPU, MDSP or MHID
    size          files over 2 MB, unless config/native/large_files.txt lists
                  their path and SHA-256
    trace         text files with more than 50 lines that look like trace lines

Only the names and the rule are printed, never the content of a file.
Exit status: 0 clean, 1 offending files, 2 usage or git error.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable, Iterable, Optional

MAX_BYTES = 2 * 1024 * 1024
TRACE_LINE_LIMIT = 50
MAGICS = (b"MGPU", b"MDSP", b"MHID")
ALLOWLIST = "config/native/large_files.txt"

# Same expression as the grep in .github/workflows/ci.yml (case-insensitive).
CI_PATTERN = re.compile(
    r"\.(3ds|cci|cia|cxi|cfa|arc|sav)$|(^|/)(code|exheader|romfs|exefs)\.bin$|^baserom/",
    re.IGNORECASE,
)
FORBIDDEN_DIRS = frozenset({"build", "baserom", "export"})
FORBIDDEN_EXTENSIONS = frozenset({".ppm", ".bin", ".wav"})

TRACE_PATTERNS = tuple(
    re.compile(p)
    for p in (
        r"^\d+ [0-9A-F]{2} pc=[0-9A-F]{8}",  # SVC trace
        r"^# thread ",  # thread lines
        r"seq=\d+ session=",  # IPC trace
        r"^f=\d+ t=\d+",  # state trace
        r"svc=0x",
        r"cmd=0x",
    )
)

Reader = Callable[[str], bytes]


def git(*args: str, cwd: Optional[Path] = None, text: bool = False):
    proc = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=text, check=False
    )
    if proc.returncode != 0:
        err = proc.stderr if text else proc.stderr.decode("utf-8", "replace")
        raise RuntimeError(f"git {' '.join(args)} failed: {err.strip()}")
    return proc.stdout


def split_z(data: bytes) -> list[str]:
    return [p.decode("utf-8", "surrogateescape") for p in data.split(b"\0") if p]


def load_allowlist(text: str) -> dict[str, str]:
    """Parse '<sha256> <path>' lines into {path: sha256}."""
    allowed: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2 and re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
            allowed[parts[1].strip()] = parts[0].lower()
    return allowed


def is_text(data: bytes) -> bool:
    return b"\0" not in data[:8192]


def count_trace_lines(data: bytes) -> int:
    count = 0
    for raw in data.decode("utf-8", "replace").splitlines():
        if any(p.search(raw) for p in TRACE_PATTERNS):
            count += 1
    return count


def check_file(path: str, data: bytes, allowed: dict[str, str]) -> list[str]:
    """Return the names of the rules the file breaks."""
    broken: list[str] = []
    if CI_PATTERN.search(path):
        broken.append("ci-pattern (game data file name)")
    parts = path.split("/")
    if any(p in FORBIDDEN_DIRS for p in parts[:-1]):
        broken.append("forbidden-dir (build/, baserom/ or export/)")
    ext = Path(path).suffix.lower()
    if ext in FORBIDDEN_EXTENSIONS:
        broken.append(f"extension ({ext} files are not allowed)")
    if data[:4] in MAGICS:
        broken.append(f"magic (capture file starting with {data[:4].decode()})")
    if len(data) > MAX_BYTES:
        digest = hashlib.sha256(data).hexdigest()
        if allowed.get(path) != digest:
            broken.append(
                f"size ({len(data)} bytes > {MAX_BYTES}; list path and sha256 in {ALLOWLIST})"
            )
    if is_text(data):
        n = count_trace_lines(data)
        if n > TRACE_LINE_LIMIT:
            broken.append(f"trace ({n} lines look like trace lines, limit {TRACE_LINE_LIMIT})")
    return broken


def run_checks(
    paths: Iterable[str], read: Reader, allowed: dict[str, str]
) -> list[tuple[str, str]]:
    offences: list[tuple[str, str]] = []
    for path in paths:
        try:
            data = read(path)
        except RuntimeError:
            data = b""  # unreadable entry (submodule, vanished file): name rules still apply
        for rule in check_file(path, data, allowed):
            offences.append((path, rule))
    return offences


def read_allowlist(root: Path) -> dict[str, str]:
    f = root / ALLOWLIST
    if f.is_file():
        return load_allowlist(f.read_text(encoding="utf-8", errors="replace"))
    return {}


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--staged", action="store_true", help="check the staged changes")
    mode.add_argument("--range", metavar="A..B", help="check the files changed in a commit range")
    ap.add_argument("--repo", default=".", help="repository to check (default: current directory)")
    args = ap.parse_args(argv)

    try:
        root = Path(git("rev-parse", "--show-toplevel", cwd=Path(args.repo), text=True).strip())
        if args.staged:
            names = split_z(
                git("diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR", cwd=root)
            )
            read: Reader = lambda p: git("show", f":{p}", cwd=root)
        else:
            rng = args.range
            if ".." not in rng:
                ap.error("--range needs A..B")
            end = rng.split("...")[-1] if "..." in rng else rng.split("..")[-1]
            end = end or "HEAD"
            names = split_z(
                git("diff", "--name-only", "-z", "--diff-filter=ACMR", rng, cwd=root)
            )
            read = lambda p: git("show", f"{end}:{p}", cwd=root)
        offences = run_checks(names, read, read_allowlist(root))
    except RuntimeError as e:
        print(f"check_no_game_data: {e}", file=sys.stderr)
        return 2

    if offences:
        for path, rule in offences:
            print(f"{path}: {rule}")
        print(
            f"check_no_game_data: {len(offences)} problem(s) in {len({p for p, _ in offences})} file(s); "
            "game data and files derived from it never go in the repository.",
            file=sys.stderr,
        )
        return 1
    print(f"check_no_game_data: ok ({len(names)} file(s) checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
