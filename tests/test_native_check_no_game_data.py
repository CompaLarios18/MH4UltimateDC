"""Tests for tools/native/check_no_game_data.py (synthetic data, temporary git repos)."""
from __future__ import annotations

import hashlib
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "native" / "check_no_game_data.py"
sys.path.insert(0, str(ROOT / "tools" / "native"))
import check_no_game_data as guard  # noqa: E402


def git(repo: Path, *args: str) -> str:
    out = subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
        cwd=repo, capture_output=True, text=True, check=True,
    )
    return out.stdout.strip()


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    git(tmp_path, "init", "-q")
    (tmp_path / "README.md").write_text("synthetic\n")
    git(tmp_path, "add", "-A")
    git(tmp_path, "commit", "-q", "-m", "base")
    return tmp_path


def put(repo: Path, rel: str, data: bytes | str) -> None:
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data if isinstance(data, bytes) else data.encode())


def run(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), "--repo", str(repo), *args],
        capture_output=True, text=True,
    )


def commit_range(repo: Path, files: dict[str, bytes | str]) -> str:
    base = git(repo, "rev-parse", "HEAD")
    for rel, data in files.items():
        put(repo, rel, data)
    git(repo, "add", "-f", "-A")
    git(repo, "commit", "-q", "-m", "change")
    return f"{base}..HEAD"


def test_clean_staged(repo: Path) -> None:
    put(repo, "docs/a.md", "hello\n")
    put(repo, "tools/x.py", "print(1)\n")
    git(repo, "add", "-A")
    r = run(repo, "--staged")
    assert r.returncode == 0, r.stdout + r.stderr


def test_clean_range(repo: Path) -> None:
    rng = commit_range(repo, {"src/a.c": "int x;\n"})
    assert run(repo, "--range", rng).returncode == 0


def test_nothing_staged_is_ok(repo: Path) -> None:
    assert run(repo, "--staged").returncode == 0


@pytest.mark.parametrize(
    "path",
    ["game.3ds", "a/b.CXI", "x.cia", "x.cfa", "x.cci", "x.arc", "slot.sav",
     "code.bin", "sub/exheader.bin", "romfs.bin", "exefs.bin", "baserom/anything.txt"],
)
def test_ci_patterns(repo: Path, path: str) -> None:
    put(repo, path, "x")
    git(repo, "add", "-f", path)
    r = run(repo, "--staged")
    assert r.returncode == 1
    assert path in r.stdout


@pytest.mark.parametrize("path", ["build/log.txt", "export/callgraph.tsv", "baserom/x.txt", "a/build/y.txt"])
def test_forbidden_dirs(repo: Path, path: str) -> None:
    put(repo, path, "x")
    git(repo, "add", "-f", path)
    r = run(repo, "--staged")
    assert r.returncode == 1
    assert path in r.stdout and "forbidden-dir" in r.stdout


@pytest.mark.parametrize("path", ["shot.ppm", "a.bin", "sound.WAV"])
def test_extensions(repo: Path, path: str) -> None:
    put(repo, path, "x")
    git(repo, "add", path)
    r = run(repo, "--staged")
    assert r.returncode == 1 and "extension" in r.stdout


@pytest.mark.parametrize("magic", ["MGPU", "MDSP", "MHID"])
def test_capture_magics(repo: Path, magic: str) -> None:
    put(repo, "cap/frame.dat", magic.encode() + b"\x01\x00\x00\x00rest")
    git(repo, "add", "cap/frame.dat")
    r = run(repo, "--staged")
    assert r.returncode == 1 and "magic" in r.stdout and magic in r.stdout


def test_magic_not_at_start_is_fine() -> None:
    assert guard.check_file("a.dat", b"xMGPU", {}) == []


def test_size_rule_and_exemption(repo: Path) -> None:
    big = b"A" * (2 * 1024 * 1024 + 1)
    put(repo, "third_party/archive.dat", big)
    git(repo, "add", "third_party/archive.dat")
    r = run(repo, "--staged")
    assert r.returncode == 1 and "size" in r.stdout

    digest = hashlib.sha256(big).hexdigest()
    put(repo, "config/native/large_files.txt", f"# vendored\n{digest}  third_party/archive.dat\n")
    assert run(repo, "--staged").returncode == 0

    # Same path, different content: the exemption no longer applies.
    put(repo, "third_party/archive.dat", big + b"B")
    git(repo, "add", "third_party/archive.dat")
    assert run(repo, "--staged").returncode == 1

    # Same content under another path: not exempt either.
    put(repo, "third_party/archive.dat", big)
    put(repo, "other/archive.dat", big)
    git(repo, "add", "-A")
    r = run(repo, "--staged")
    assert r.returncode == 1 and "other/archive.dat" in r.stdout
    assert "third_party/archive.dat" not in r.stdout


def test_exactly_2mb_is_allowed() -> None:
    assert guard.check_file("a.dat", b"A" * guard.MAX_BYTES, {}) == []


def svc_line(i: int) -> str:
    return f"{i} 32 pc=0010ABCD r0=00000001\n"


@pytest.mark.parametrize(
    "make",
    [
        svc_line,
        lambda i: f"# thread {i} entry=0x00100000\n",
        lambda i: f"seq={i} session=3 name=x\n",
        lambda i: f"f={i} t=5 pc=0x00100000\n",
        lambda i: f"op svc=0x{i:02x} r=0x00000000\n",
        lambda i: f"op cmd=0x{i:04x} r=0x00000000\n",
    ],
    ids=["svc", "thread", "ipc", "state", "svc-field", "cmd-field"],
)
def test_trace_rule(repo: Path, make) -> None:
    put(repo, "notes/t.txt", "".join(make(i) for i in range(51)))
    git(repo, "add", "notes/t.txt")
    r = run(repo, "--staged")
    assert r.returncode == 1 and "trace" in r.stdout


def test_trace_rule_limit_and_binary(repo: Path) -> None:
    put(repo, "notes/ok.txt", "".join(svc_line(i) for i in range(50)))
    assert guard.check_file("n.txt", "".join(svc_line(i) for i in range(50)).encode(), {}) == []
    assert guard.check_file("n.txt", "".join(svc_line(i) for i in range(51)).encode(), {})
    # Binary data is not scanned as text.
    assert guard.check_file("n.dat", b"\0" + "".join(svc_line(i) for i in range(60)).encode(), {}) == []


def test_range_only_checks_the_range(repo: Path) -> None:
    put(repo, "old.ppm", "x")
    git(repo, "add", "old.ppm")
    git(repo, "commit", "-q", "-m", "old dirt")
    rng = commit_range(repo, {"docs/new.md": "fine\n"})
    assert run(repo, "--range", rng).returncode == 0


def test_range_dirty_commit(repo: Path) -> None:
    rng = commit_range(repo, {"docs/new.md": "fine\n", "x/y.sav": "s"})
    r = run(repo, "--range", rng)
    assert r.returncode == 1
    assert "x/y.sav" in r.stdout and "docs/new.md" not in r.stdout


def test_range_dirty_in_middle_commit_removed_later_is_not_reported(repo: Path) -> None:
    # The check works on the net diff A..B, as the CI step does.
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "a.ppm", "x")
    git(repo, "add", "a.ppm")
    git(repo, "commit", "-q", "-m", "add")
    git(repo, "rm", "-q", "a.ppm")
    git(repo, "commit", "-q", "-m", "remove")
    assert run(repo, "--range", f"{base}..HEAD").returncode == 0


def test_range_three_dots(repo: Path) -> None:
    git(repo, "checkout", "-q", "-b", "side")
    put(repo, "s.wav", "x")
    git(repo, "add", "s.wav")
    git(repo, "commit", "-q", "-m", "side")
    git(repo, "checkout", "-q", "-")
    put(repo, "main.md", "m\n")
    git(repo, "add", "main.md")
    git(repo, "commit", "-q", "-m", "main")
    r = run(repo, "--range", "HEAD...side")
    assert r.returncode == 1 and "s.wav" in r.stdout and "main.md" not in r.stdout


def test_output_never_prints_content(repo: Path) -> None:
    put(repo, "x.ppm", "SECRET-CONTENT-123")
    git(repo, "add", "x.ppm")
    r = run(repo, "--staged")
    assert "SECRET-CONTENT-123" not in r.stdout + r.stderr


def test_bad_range_is_usage_error(repo: Path) -> None:
    assert run(repo, "--range", "nonexistent..HEAD").returncode == 2


def test_allowlist_parsing() -> None:
    h = "a" * 64
    parsed = guard.load_allowlist(f"# c\n\n{h}  dir/f.dat\nbadline\n{h[:10]} short\n")
    assert parsed == {"dir/f.dat": h}


def test_shipped_allowlist_is_comments_only() -> None:
    text = (ROOT / "config" / "native" / "large_files.txt").read_text()
    assert text.startswith("#")
    assert guard.load_allowlist(text) == {}
