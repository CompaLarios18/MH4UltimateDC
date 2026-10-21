"""The two game-data guards of branch native keep their own allowlists of vendored large files:
config/large_files.txt (tools/check_no_game_data.py, main's guard, which game-data-guard.yml runs on every PR) and
config/native/large_files.txt (tools/native/check_no_game_data.py). A file allowed in the second must be allowed in
the first, by path and hash, or a PR that adds it fails main's guard. Synthetic: it reads only the two lists."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def entries(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        digest, name = line.split(None, 1)
        out[name.strip()] = digest.lower()
    return out


def test_every_native_entry_is_also_in_the_main_guard_list() -> None:
    main_list = entries(ROOT / "config" / "large_files.txt")
    native_list = entries(ROOT / "config" / "native" / "large_files.txt")
    assert native_list, "config/native/large_files.txt has no entry"
    missing = {name: digest for name, digest in native_list.items() if main_list.get(name) != digest}
    assert not missing, (
        "allowed by tools/native/check_no_game_data.py but not by tools/check_no_game_data.py: " f"{sorted(missing)}"
    )
