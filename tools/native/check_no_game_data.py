#!/usr/bin/env python3
"""Refuse commits that contain game data or files derived from it.

Usage:
    check_no_game_data.py --staged          check what is staged in the index
    check_no_game_data.py --range A..B      check every (blob, path) of every commit in A..B
    check_no_game_data.py --range A...B     same, from the merge base of A and B

--range inspects every added or modified path of every commit of the range
(git log --raw, merges against each parent), not the net diff and not a
deduplicated blob list: a file added in one commit and removed in a later one is
still reported, and so is a forbidden name whose content also exists under a
clean name or already in the base.

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


def name_rules(path: str) -> list[str]:
    broken: list[str] = []
    if CI_PATTERN.search(path):
        broken.append("ci-pattern (game data file name)")
    parts = path.split("/")
    if any(p in FORBIDDEN_DIRS for p in parts[:-1]):
        broken.append("forbidden-dir (build/, baserom/ or export/)")
    ext = Path(path).suffix.lower()
    if ext in FORBIDDEN_EXTENSIONS:
        broken.append(f"extension ({ext} files are not allowed)")
    return broken


def content_rules(data: bytes) -> list[str]:
    """Rules that depend only on the content (the size rule is exempted per path by the caller)."""
    broken: list[str] = []
    if data[:4] in MAGICS:
        broken.append(f"magic (capture file starting with {data[:4].decode()})")
    if len(data) > MAX_BYTES:
        broken.append(
            f"size ({len(data)} bytes > {MAX_BYTES}; list path and sha256 in {ALLOWLIST})"
        )
    if is_text(data):
        n = count_trace_lines(data)
        if n > TRACE_LINE_LIMIT:
            broken.append(f"trace ({n} lines look like trace lines, limit {TRACE_LINE_LIMIT})")
    return broken


def check_file(path: str, data: bytes, allowed: dict[str, str]) -> list[str]:
    """Return the names of the rules the file breaks."""
    broken = name_rules(path)
    digest = hashlib.sha256(data).hexdigest() if len(data) > MAX_BYTES else ""
    for rule in content_rules(data):
        if rule.startswith("size") and allowed.get(path) == digest:
            continue
        broken.append(rule)
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


def range_entries(root: Path, rng: str) -> list[tuple[str, str, str]]:
    """(mode, blob sha, path) for every added or modified entry of every commit in the range.

    Merge commits report their diff against each parent (-m), so a file that only a
    merge resolution introduces is covered. Deleted entries are skipped."""
    if "..." in rng:
        a, b = rng.split("...", 1)
        a, b = a or "HEAD", b or "HEAD"
        base = git("merge-base", a, b, cwd=root, text=True).strip()
        spec = f"{base}..{b}"
    else:
        a, b = rng.split("..", 1)
        spec = f"{a or 'HEAD'}..{b or 'HEAD'}"
    raw = git(
        "log", "--format=", "--raw", "-z", "--no-abbrev", "--no-renames", "-m", spec,
        cwd=root,
    )
    toks = [t.decode("utf-8", "surrogateescape") for t in raw.split(b"\0")]
    entries: list[tuple[str, str, str]] = []
    i = 0
    while i < len(toks):
        tok = toks[i]
        if not tok.startswith(":"):
            i += 1
            continue
        fields = tok[1:].split()
        if len(fields) < 5 or i + 1 >= len(toks):
            raise RuntimeError(f"unexpected git log --raw output near {tok!r}")
        new_mode, new_sha, status, path = fields[1], fields[3], fields[4], toks[i + 1]
        i += 2
        if status.startswith("D") or set(new_sha) == {"0"}:
            continue
        entries.append((new_mode, new_sha, path))
    return entries


def check_entries(
    root: Path, entries: list[tuple[str, str, str]], allowed: dict[str, str]
) -> list[tuple[str, str]]:
    """Name rules for every (sha, path); content rules once per blob sha."""
    offences: list[tuple[str, str]] = []
    cache: dict[str, tuple[list[str], list[str], str]] = {}
    for mode, sha, path in entries:
        rules = name_rules(path)
        if mode != "160000":
            if sha not in cache:
                data = git("cat-file", "blob", sha, cwd=root)
                found = content_rules(data)
                cache[sha] = (
                    [r for r in found if not r.startswith("size")],
                    [r for r in found if r.startswith("size")],
                    hashlib.sha256(data).hexdigest() if len(data) > MAX_BYTES else "",
                )
            plain, size, digest = cache[sha]
            rules += plain
            if size and allowed.get(path) != digest:
                rules += size
        offences += [(path, r) for r in rules]
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
            entries = range_entries(root, args.range)
            names = sorted({path for _, _, path in entries})
            offences = check_entries(root, entries, allowed)
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
