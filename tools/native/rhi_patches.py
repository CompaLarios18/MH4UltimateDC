#!/usr/bin/env python3
"""The patch queue of the vendored plume (N2-25; docs/native/rhi.md).

native/third_party/rhi/plume/ holds
    pristine/   plume's files at the pinned commit, unmodified, at their upstream paths
    patches/    NN-<name>.diff, unified diffs on the pristine paths, applied in number order
    patched/    pristine/ with every patch applied (what the build compiles), written by `apply`
    README.md   the pinned commit, the git blob id of each pristine file and the patch list

Commands (Python 3 standard library only; the diff applier is ours, so neither `git` nor `patch` is needed):
    apply   write patched/ (and patched/mh4n_patch_list.h) from pristine/ and patches/
    check   apply into a temporary folder and compare byte for byte with patched/; exit 1 naming the
            first file that differs
    blobs   the git blob id of each pristine file (and of the other vendored files listed in
            native/third_party/rhi/README.md) against the tables of the READMEs

Exit status: 0 ok, 1 the tree disagrees with the queue or a README, 2 a patch does not apply or bad usage.
A hunk is applied only at the line it names and only when every context and removed line matches exactly.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT / "native" / "third_party" / "rhi" / "plume"
PATCH_LIST_HEADER = "mh4n_patch_list.h"
PATCH_NAME = re.compile(r"^(\d\d)-[A-Za-z0-9._+-]+\.diff$")
HUNK_HEADER = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
PINNED_LINE = re.compile(r"^Pinned commit: `([0-9a-f]{40})`", re.MULTILINE)
BLOB_ROW = re.compile(r"^\| `([^`]+)` \| `([0-9a-f]{40})` \|", re.MULTILINE)


class PatchError(Exception):
    """A patch that cannot be read or does not apply exactly."""


@dataclass
class Hunk:
    old_start: int
    old_count: int
    new_count: int
    # (op, text without its newline, ends_with_newline); op is ' ', '-' or '+'
    lines: list[tuple[str, bytes, bool]] = field(default_factory=list)


@dataclass
class FilePatch:
    old_path: str | None  # None: the file is created
    new_path: str | None  # None: the file is deleted
    hunks: list[Hunk] = field(default_factory=list)


def _strip_prefix(path: bytes) -> str | None:
    text = path.split(b"\t")[0].decode("utf-8")
    if text == "/dev/null":
        return None
    if text.startswith(("a/", "b/")):
        return text[2:]
    raise PatchError(f"unexpected path in the diff header: {text!r}")


def parse_patch(data: bytes, name: str = "<patch>") -> list[FilePatch]:
    """Parse a unified diff in git's form (`diff --git`, `index`, `---`, `+++`, hunks)."""
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    files: list[FilePatch] = []
    i = 0
    current: FilePatch | None = None
    while i < len(lines):
        line = lines[i]
        if line.startswith(b"diff --git "):
            current = None
            i += 1
            continue
        if line.startswith((b"rename ", b"copy ", b"similarity ", b"GIT binary patch", b"Binary files")):
            raise PatchError(f"{name}: renames, copies and binary patches are not supported ({line[:40]!r})")
        if line.startswith(b"--- "):
            if i + 1 >= len(lines) or not lines[i + 1].startswith(b"+++ "):
                raise PatchError(f"{name}: line {i + 1}: '---' without '+++'")
            current = FilePatch(_strip_prefix(line[4:]), _strip_prefix(lines[i + 1][4:]))
            files.append(current)
            i += 2
            continue
        if line.startswith(b"@@"):
            if current is None:
                raise PatchError(f"{name}: line {i + 1}: a hunk before any file header")
            m = HUNK_HEADER.match(line)
            if not m:
                raise PatchError(f"{name}: line {i + 1}: bad hunk header {line[:60]!r}")
            hunk = Hunk(
                int(m.group(1)),
                int(m.group(2)) if m.group(2) is not None else 1,
                int(m.group(4)) if m.group(4) is not None else 1,
            )
            i += 1
            seen_old = seen_new = 0
            while i < len(lines) and (seen_old < hunk.old_count or seen_new < hunk.new_count):
                body = lines[i]
                op = body[:1]
                if op == b"\\":
                    if not hunk.lines:
                        raise PatchError(f"{name}: line {i + 1}: a stray 'No newline' marker")
                    o, t, _ = hunk.lines[-1]
                    hunk.lines[-1] = (o, t, False)
                    i += 1
                    continue
                if op not in (b" ", b"-", b"+"):
                    # a blank line inside a hunk is a context line whose space was stripped
                    if body == b"":
                        op, body = b" ", b" "
                    else:
                        raise PatchError(f"{name}: line {i + 1}: bad hunk line {body[:60]!r}")
                hunk.lines.append((op.decode(), body[1:], True))
                if op in (b" ", b"-"):
                    seen_old += 1
                if op in (b" ", b"+"):
                    seen_new += 1
                i += 1
            # a trailing 'No newline' marker belongs to this hunk
            while i < len(lines) and lines[i].startswith(b"\\"):
                o, t, _ = hunk.lines[-1]
                hunk.lines[-1] = (o, t, False)
                i += 1
            if seen_old != hunk.old_count or seen_new != hunk.new_count:
                raise PatchError(
                    f"{name}: hunk at old line {hunk.old_start} is shorter than its header "
                    f"(-{hunk.old_count} +{hunk.new_count}, got -{seen_old} +{seen_new})"
                )
            current.hunks.append(hunk)
            continue
        # index / mode / new file lines and anything else between files
        i += 1
    if not files:
        raise PatchError(f"{name}: no file in the diff")
    return files


def _split_lines(data: bytes) -> list[tuple[bytes, bool]]:
    """(text, ends_with_newline) for each line of a file."""
    out: list[tuple[bytes, bool]] = []
    for raw in data.split(b"\n"):
        out.append((raw, True))
    if out and out[-1][0] == b"":
        out.pop()  # the file ended with a newline
    elif out:
        out[-1] = (out[-1][0], False)
    return out


def _join_lines(lines: list[tuple[bytes, bool]]) -> bytes:
    return b"".join(text + (b"\n" if nl else b"") for text, nl in lines)


def apply_file_patch(original: bytes | None, fp: FilePatch, name: str) -> bytes | None:
    """Apply one file's hunks to `original` (None when the file does not exist); None when it is deleted."""
    if fp.old_path is None:
        if original is not None:
            raise PatchError(f"{name}: {fp.new_path} is created but already exists")
        old = []
    else:
        if original is None:
            raise PatchError(f"{name}: {fp.old_path} does not exist")
        old = _split_lines(original)
    out: list[tuple[bytes, bool]] = []
    cursor = 0
    for hunk in fp.hunks:
        # a hunk with no old lines names the line before it
        start = hunk.old_start if hunk.old_count == 0 else hunk.old_start - 1
        if start < cursor or start > len(old):
            raise PatchError(f"{name}: {fp.old_path or fp.new_path}: hunk at line {hunk.old_start} is out of place")
        out.extend(old[cursor:start])
        idx = start
        for op, text, nl in hunk.lines:
            if op in (" ", "-"):
                if idx >= len(old) or old[idx] != (text, nl):
                    got = old[idx][0][:60] if idx < len(old) else b"<end of file>"
                    raise PatchError(
                        f"{name}: {fp.old_path or fp.new_path}: hunk at line {hunk.old_start} does not apply: "
                        f"line {idx + 1} is {got!r}, the patch expects {text[:60]!r}"
                    )
                idx += 1
            if op in (" ", "+"):
                out.append((text, nl))
        cursor = idx
    out.extend(old[cursor:])
    if fp.new_path is None:
        if out:
            raise PatchError(f"{name}: {fp.old_path} is deleted but lines remain")
        return None
    return _join_lines(out)


def list_patches(patches_dir: Path) -> list[Path]:
    """The patch files in number order; the numbers must run 01, 02, ... without a gap."""
    found: list[tuple[int, Path]] = []
    for path in sorted(patches_dir.iterdir()):
        if path.name.startswith("."):
            continue
        m = PATCH_NAME.match(path.name)
        if not m:
            raise PatchError(f"{path}: a patch file is named NN-<name>.diff")
        found.append((int(m.group(1)), path))
    for position, (number, path) in enumerate(found, start=1):
        if number != position:
            raise PatchError(f"{path.name}: expected patch number {position:02d}")
    return [p for _, p in found]


def read_tree(directory: Path) -> dict[str, bytes]:
    tree: dict[str, bytes] = {}
    for path in sorted(directory.rglob("*")):
        if path.is_file():
            tree[path.relative_to(directory).as_posix()] = path.read_bytes()
    return tree


def pinned_commit(root: Path) -> str:
    readme = root / "README.md"
    if not readme.is_file():
        raise PatchError(f"{readme} is missing")
    m = PINNED_LINE.search(readme.read_text(encoding="utf-8"))
    if not m:
        raise PatchError(f"{readme}: no line 'Pinned commit: `<40 hex digits>`'")
    return m.group(1)


def patch_list_header(commit: str, names: list[str]) -> bytes:
    items = ",\n".join(f'    "{n}"' for n in names)
    text = (
        "// Written by tools/native/rhi_patches.py apply. Do not edit.\n"
        "#pragma once\n"
        "\n"
        "namespace mh4n_rhi_plume {\n"
        f'inline constexpr const char* kPlumeCommit = "{commit}";\n'
        f"inline constexpr unsigned kPatchCount = {len(names)};\n"
        "inline constexpr const char* kPatchFiles[] = {\n"
        f"{items}\n"
        "};\n"
        "}  // namespace mh4n_rhi_plume\n"
    )
    return text.encode("utf-8")


def build_patched(root: Path) -> dict[str, bytes]:
    """The patched tree (relative path -> bytes), including mh4n_patch_list.h, from pristine/ and patches/."""
    pristine = root / "pristine"
    if not pristine.is_dir():
        raise PatchError(f"{pristine} is missing")
    tree = read_tree(pristine)
    patches = list_patches(root / "patches")
    for patch in patches:
        for fp in parse_patch(patch.read_bytes(), patch.name):
            target = fp.new_path if fp.new_path is not None else fp.old_path
            assert target is not None
            current = tree.get(fp.old_path) if fp.old_path is not None else None
            result = apply_file_patch(current, fp, patch.name)
            if fp.old_path is not None:
                tree.pop(fp.old_path, None)
            if result is not None:
                tree[target] = result
    tree[PATCH_LIST_HEADER] = patch_list_header(pinned_commit(root), [p.name for p in patches])
    return tree


def write_tree(tree: dict[str, bytes], directory: Path) -> None:
    for rel, data in tree.items():
        dest = directory / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)


def cmd_apply(root: Path) -> int:
    tree = build_patched(root)
    patched = root / "patched"
    if patched.exists():
        for old in sorted(patched.rglob("*"), reverse=True):
            if old.is_file():
                old.unlink()
            elif old.is_dir():
                try:
                    old.rmdir()
                except OSError:
                    pass
    write_tree(tree, patched)
    print(f"rhi_patches: wrote {len(tree)} files to {patched} ({len(list_patches(root / 'patches'))} patches)")
    return 0


def cmd_check(root: Path) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        write_tree(build_patched(root), Path(tmp))
        expected = read_tree(Path(tmp))
    patched = root / "patched"
    actual = read_tree(patched) if patched.is_dir() else {}
    for rel in sorted(set(expected) | set(actual)):
        if rel not in actual:
            print(f"rhi_patches: patched/{rel} is missing (run `rhi_patches.py apply`)")
            return 1
        if rel not in expected:
            print(f"rhi_patches: patched/{rel} is not produced by the queue (run `rhi_patches.py apply`)")
            return 1
        if expected[rel] != actual[rel]:
            print(f"rhi_patches: patched/{rel} differs from pristine/ plus the patches (run `rhi_patches.py apply`)")
            return 1
    print(f"rhi_patches: patched/ matches pristine/ plus {len(list_patches(root / 'patches'))} patches ({len(expected)} files)")
    return 0


def git_blob_id(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def _check_table(readme: Path, base: Path) -> list[str]:
    """Problems between the `| `file` | `blob id` |` rows of a README and the files under `base`."""
    problems: list[str] = []
    rows = {m.group(1): m.group(2) for m in BLOB_ROW.finditer(readme.read_text(encoding="utf-8"))}
    if not rows:
        return [f"{readme}: no blob table"]
    for rel, blob in rows.items():
        path = base / rel
        if not path.is_file():
            problems.append(f"{readme.name}: {rel} is listed but not in the tree")
        elif git_blob_id(path.read_bytes()) != blob:
            problems.append(f"{readme.name}: {rel} has blob id {git_blob_id(path.read_bytes())}, the table says {blob}")
    return problems


def cmd_blobs(root: Path) -> int:
    problems: list[str] = []
    readme = root / "README.md"
    if not readme.is_file():
        print(f"rhi_patches: {readme} is missing")
        return 1
    # plume: every file of pristine/ must be listed, and every row must match its file
    listed = {m.group(1) for m in BLOB_ROW.finditer(readme.read_text(encoding="utf-8"))}
    problems += _check_table(readme, root / "pristine")
    for rel in read_tree(root / "pristine"):
        if rel not in listed:
            problems.append(f"{readme.name}: pristine/{rel} is not in the blob table")
    # volk, VMA: the table of the README one level up lists them relative to its folder
    parent_readme = root.parent / "README.md"
    if parent_readme.is_file():
        problems += _check_table(parent_readme, root.parent)
    for problem in problems:
        print(f"rhi_patches: {problem}")
    if problems:
        return 1
    print(f"rhi_patches: blob ids match ({len(listed)} pristine files)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="The patch queue of the vendored plume (N2-25).")
    parser.add_argument("command", choices=("apply", "check", "blobs"))
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="the plume folder (default: native/third_party/rhi/plume)")
    args = parser.parse_args(argv)
    try:
        return {"apply": cmd_apply, "check": cmd_check, "blobs": cmd_blobs}[args.command](args.root)
    except PatchError as exc:
        print(f"rhi_patches: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
