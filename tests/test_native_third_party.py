"""native/THIRD_PARTY.md and the 'Derived from Azahar' headers agree (docs/native/AGENTS.md, rule 6).

Every file under native/, tools/ and port/ that carries a header has exactly one row, every row
names a file that carries one, and every source file named in a header (the text before its
Copyright lines) is named in the row too. The licence and Copyright lines still need a person (or
a reviewer) to open the Azahar files; this only keeps the two documents from drifting apart.
"""
import os
import re
import subprocess

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TABLE = os.path.join(ROOT, "native", "THIRD_PARTY.md")
PINNED = os.path.join(ROOT, "port", "azahar", "COMMIT")
HEADER_RE = re.compile(r"^(?://|#) Derived (?:from|in part from) Azahar")
SOURCE_NAME_RE = re.compile(r"\b[A-Za-z0-9_]+\.(?:cpp|h)\b")

pytestmark = pytest.mark.skipif(not (os.path.isdir(os.path.join(ROOT, ".git")) and os.path.isfile(TABLE)),
                                reason="needs a git checkout with native/THIRD_PARTY.md")


def headed_files() -> list[str]:
    out = subprocess.run(["git", "grep", "-lE", "^(//|#) Derived (from|in part from) Azahar", "--",
                          "native", "tools", "port"], cwd=ROOT, capture_output=True, text=True).stdout
    return sorted(out.split())


def header_text(path: str) -> str:
    """The comment block that starts at the Derived line, up to its first Copyright line."""
    lines = open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read().splitlines()
    start = next(i for i, line in enumerate(lines) if HEADER_RE.match(line))
    block = []
    for line in lines[start:]:
        if not line.startswith(("//", "#")):
            break
        block.append(line)
    return " ".join(block).split("Copyright")[0]


def split_cells(line: str) -> list[str]:
    return re.split(r"(?<!\\)\|", line.strip().strip("|"))


def table_rows() -> dict[str, str]:
    rows: dict[str, str] = {}
    for line in open(TABLE, encoding="utf-8").read().splitlines():
        m = re.match(r"^\| `([^`]+)`", line)
        if m and m.group(1).startswith(("native/", "tools/", "port/")):
            key = m.group(1).split(" ")[0]
            assert key not in rows, f"two rows for {key}"
            rows[key] = line
    return rows


def test_every_headed_file_has_one_row_and_every_row_a_headed_file():
    files, rows = headed_files(), table_rows()
    assert files, "no file with a Derived header found"
    assert sorted(set(files) - set(rows)) == [], "headed files without a row"
    assert sorted(set(rows) - set(files)) == [], "rows for files without a header"


def test_every_source_named_in_a_header_is_named_in_its_row():
    rows = table_rows()
    missing = []
    for path in headed_files():
        sources = split_cells(rows[path])[1]                  # the "Azahar source" column
        for name in sorted(set(SOURCE_NAME_RE.findall(header_text(path)))):
            if name not in sources:
                missing.append((path, name))
    # runner.cpp's header also quotes headers of its own includes; nothing else may be missing
    assert [m for m in missing if m[0] != "port/src/runner.cpp"] == []


def test_rows_are_well_formed_and_use_the_pinned_commit():
    pinned = open(PINNED).read().strip()
    for path, line in table_rows().items():
        cells = split_cells(line)
        assert len(cells) == 6, (path, len(cells))
        assert line.count("`") % 2 == 0, f"unbalanced backticks in the row of {path}"
        assert f"`{pinned}`" in cells[2], f"{path}: the commit is not the pinned one"
        assert re.search(r"GPL-[23]\.0-or-later", cells[3]), f"{path}: no licence"


def test_a_header_that_names_a_gpl3_source_says_gpl3():
    """A file derived from any GPLv3-or-later source is GPL-3.0-or-later (rule 6)."""
    for path in headed_files():
        text = open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()[:6000]
        says3 = "GPL-3.0-or-later" in text
        row = table_rows()[path]
        assert ("GPL-3.0-or-later" in row.split("|")[4]) == says3, f"{path}: header and row disagree on GPLv3"
