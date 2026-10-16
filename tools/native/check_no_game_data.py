#!/usr/bin/env python3
"""Refuse commits that contain game data or files derived from it.

Usage:
    check_no_game_data.py --staged          check what is staged in the index
    check_no_game_data.py --range A..B      check every file version introduced by the commits in A..B
    check_no_game_data.py --range A...B     same, from the merge base of A and B

--range inspects every blob the range introduces (git rev-list --objects), not
the net diff, so a file added in one commit and removed in a later one of the
same push is still reported: it would be published in the history.

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
    paths: Iterable[str],
    read: Reader,
    allowed: dict[str, str],
    skip_content: Callable[[str], bool] = lambda p: False,
) -> list[tuple[str, str]]:
    """Apply the rules to each path. A regular file that cannot be read is an error
    (RuntimeError, exit 2); only entries skip_content() accepts (gitlinks) get name rules only."""
    offences: list[tuple[str, str]] = []
    for path in paths:
        if skip_content(path):
            data = b""
        else:
            try:
                data = read(path)
            except RuntimeError as e:
                raise RuntimeError(f"cannot read {path}: {e}") from e
        for rule in check_file(path, data, allowed):
            offences.append((path, rule))
    return offences


def range_blobs(root: Path, rng: str) -> list[tuple[str, str]]:
    """(blob sha, path) for every blob introduced by the commits of the range."""
    if "..." in rng:
        a, b = rng.split("...", 1)
        a, b = a or "HEAD", b or "HEAD"
        base = git("merge-base", a, b, cwd=root, text=True).strip()
        spec = f"{base}..{b}"
    else:
        a, b = rng.split("..", 1)
        spec = f"{a or 'HEAD'}..{b or 'HEAD'}"
    listing = git("rev-list", "--objects", spec, cwd=root, text=True)
    cand = [ln.split(" ", 1) for ln in listing.splitlines() if " " in ln]
    if not cand:
        return []
    check = subprocess.run(
        ["git", "cat-file", "--batch-check=%(objecttype)"],
        cwd=root, input="".join(f"{sha}\n" for sha, _ in cand),
        capture_output=True, text=True, check=False,
    )
    if check.returncode != 0:
        raise RuntimeError(f"git cat-file --batch-check failed: {check.stderr.strip()}")
    types = check.stdout.splitlines()
    return [(sha, path) for (sha, path), t in zip(cand, types) if t == "blob"]


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
        allowed = read_allowlist(root)
        if args.staged:
            names = split_z(
                git("diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR", cwd=root)
            )
            gitlinks = {
                p for p in names
                if git("ls-files", "-s", "--", p, cwd=root, text=True).startswith("160000")
            }
            offences = run_checks(
                names, lambda p: git("show", f":{p}", cwd=root), allowed, lambda p: p in gitlinks
            )
        else:
            if ".." not in args.range:
                ap.error("--range needs A..B")
            blobs = range_blobs(root, args.range)
            names = sorted({path for _, path in blobs})
            offences = []
            for sha, path in blobs:
                data = git("cat-file", "blob", sha, cwd=root)
                for rule in check_file(path, data, allowed):
                    offences.append((path, rule))
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
