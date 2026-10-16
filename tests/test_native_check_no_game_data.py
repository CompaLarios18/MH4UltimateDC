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


def test_range_dirty_added_and_removed_within_range_is_reported(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "a.ppm", "x")
    git(repo, "add", "a.ppm")
    git(repo, "commit", "-q", "-m", "add")
    git(repo, "rm", "-q", "a.ppm")
    git(repo, "commit", "-q", "-m", "remove")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "a.ppm" in r.stdout


def test_range_dirty_in_merged_branch_is_reported(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    git(repo, "checkout", "-q", "-b", "side")
    put(repo, "side/t.wav", "x")
    git(repo, "add", "side/t.wav")
    git(repo, "commit", "-q", "-m", "side")
    git(repo, "rm", "-q", "side/t.wav")
    git(repo, "commit", "-q", "-m", "side rm")
    git(repo, "checkout", "-q", "-")
    put(repo, "m.md", "m\n")
    git(repo, "add", "m.md")
    git(repo, "commit", "-q", "-m", "main")
    git(repo, "merge", "-q", "--no-ff", "-m", "merge", "side")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "side/t.wav" in r.stdout


def test_range_dirty_commit_before_range_is_accepted(repo: Path) -> None:
    put(repo, "old.ppm", "x")
    git(repo, "add", "old.ppm")
    git(repo, "commit", "-q", "-m", "old")
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "ok.md", "fine\n")
    git(repo, "add", "ok.md")
    git(repo, "commit", "-q", "-m", "ok")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 0, r.stdout


def test_unreadable_regular_file_is_an_error() -> None:
    def read(p: str) -> bytes:
        raise RuntimeError("gone")

    with pytest.raises(RuntimeError, match="a.md"):
        guard.run_checks(["a.md"], read, {})
    # gitlinks keep the name rules only
    assert guard.run_checks(["sub/x.sav"], read, {}, lambda p: True)


def test_staged_submodule_gitlink_is_not_an_error(repo: Path) -> None:
    sha = git(repo, "rev-parse", "HEAD")
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{sha},sub")
    assert run(repo, "--staged").returncode == 0


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


# --- every path of every commit in --range (hardening round) ---


def commit_all(repo: Path, msg: str = "c") -> None:
    git(repo, "add", "-f", "-A")
    git(repo, "commit", "-q", "-m", msg)


def test_range_duplicate_content_under_forbidden_second_name(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "a/clean.txt", "same bytes\n")
    put(repo, "export/b.txt", "same bytes\n")
    put(repo, "z/dump.ppm", "same bytes\n")
    commit_all(repo)
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1
    assert "export/b.txt" in r.stdout and "z/dump.ppm" in r.stdout
    assert "a/clean.txt" not in r.stdout


def test_range_duplicate_content_across_commits(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "clean.txt", "dup\n")
    commit_all(repo, "one")
    put(repo, "build/out.txt", "dup\n")
    commit_all(repo, "two")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "build/out.txt" in r.stdout


@pytest.mark.parametrize("content", ["", "README content\n"])
def test_range_forbidden_path_with_content_already_in_base(repo: Path, content: str) -> None:
    # README.md holds "synthetic\n"; the empty blob is added to the base here.
    put(repo, "empty.txt", "")
    commit_all(repo, "empty in base")
    base = git(repo, "rev-parse", "HEAD")
    data = "" if content == "" else "synthetic\n"
    put(repo, "baserom/x.txt", data)
    put(repo, "snap.wav", data)
    commit_all(repo)
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1
    assert "baserom/x.txt" in r.stdout and "snap.wav" in r.stdout


def test_range_evil_merge_adds_forbidden_path(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    git(repo, "checkout", "-q", "-b", "side")
    put(repo, "side.md", "side\n")
    commit_all(repo, "side")
    git(repo, "checkout", "-q", "-")
    put(repo, "main.md", "main\n")
    commit_all(repo, "main")
    git(repo, "merge", "-q", "--no-ff", "--no-commit", "side")
    put(repo, "export/evil.txt", "only in the merge\n")
    git(repo, "add", "-f", "export/evil.txt")
    git(repo, "commit", "-q", "-m", "evil merge")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "export/evil.txt" in r.stdout


def test_range_added_then_deleted_forbidden_name_with_clean_twin(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "keep.txt", "twin\n")
    put(repo, "gone.ppm", "twin\n")
    commit_all(repo, "add")
    git(repo, "rm", "-q", "gone.ppm")
    git(repo, "commit", "-q", "-m", "rm")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "gone.ppm" in r.stdout


def test_range_deleting_a_forbidden_path_is_not_reported(repo: Path) -> None:
    put(repo, "old.ppm", "x")
    commit_all(repo, "old")
    base = git(repo, "rev-parse", "HEAD")
    git(repo, "rm", "-q", "old.ppm")
    git(repo, "commit", "-q", "-m", "rm")
    assert run(repo, "--range", f"{base}..HEAD").returncode == 0


def test_range_content_rule_still_applies_to_duplicate_blob(repo: Path) -> None:
    base = git(repo, "rev-parse", "HEAD")
    put(repo, "a.txt", b"MGPU" + b"\0" * 8)
    put(repo, "b.txt", b"MGPU" + b"\0" * 8)
    commit_all(repo)
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "a.txt" in r.stdout and "b.txt" in r.stdout


def test_range_not_fooled_by_show_signature_config(repo: Path) -> None:
    git(repo, "config", "log.showSignature", "true")
    rng = commit_range(repo, {"export/q.txt": "x\n"})
    r = run(repo, "--range", rng)
    assert r.returncode == 1 and "export/q.txt" in r.stdout


def test_range_not_fooled_by_ignore_submodules_config(repo: Path) -> None:
    git(repo, "config", "diff.ignoreSubmodules", "all")
    base = git(repo, "rev-parse", "HEAD")
    sha = git(repo, "rev-parse", "HEAD")
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{sha},export/sub")
    git(repo, "commit", "-q", "-m", "gitlink")
    r = run(repo, "--range", f"{base}..HEAD")
    assert r.returncode == 1 and "export/sub" in r.stdout
    git(repo, "reset", "-q", "--soft", base)
    r = run(repo, "--staged")
    assert r.returncode == 1 and "export/sub" in r.stdout


def test_parse_raw_rejects_unknown_tokens() -> None:
    with pytest.raises(RuntimeError):
        guard.parse_raw(b"No signature\0:000000 100644 " + b"0" * 40 + b" " + b"a" * 40 + b" A\0f\0")


def test_staged_type_change_symlink_to_magic_file(repo: Path) -> None:
    (repo / "lnk").symlink_to("README.md")
    git(repo, "add", "lnk")
    git(repo, "commit", "-q", "-m", "link")
    (repo / "lnk").unlink()
    put(repo, "lnk", b"MGPU" + b"\0" * 8)
    git(repo, "add", "lnk")
    r = run(repo, "--staged")
    assert r.returncode == 1 and "lnk" in r.stdout


def test_staged_glob_named_gitlink_does_not_hide_file(repo: Path) -> None:
    put(repo, "a.dat", b"MGPU" + b"\0" * 8)
    git(repo, "add", "a.dat")
    sha = git(repo, "rev-parse", "HEAD")
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{sha},)x.dat")
    r = run(repo, "--staged")
    assert r.returncode == 1 and "a.dat: magic" in r.stdout


@pytest.mark.parametrize("path", [".bin", "a/.PPM", ".wav", "dir/.BIN"])
def test_extension_rule_for_dotfile_names(repo: Path, path: str) -> None:
    put(repo, path, "x")
    git(repo, "add", "-f", path)
    r = run(repo, "--staged")
    assert r.returncode == 1 and "extension" in r.stdout
    git(repo, "commit", "-q", "-m", "c")
    assert run(repo, "--range", "HEAD~1..HEAD").returncode == 1


@pytest.mark.parametrize("path", ["Export/x.txt", "BUILD/x.txt", "Export/a/b.tsv"])
def test_forbidden_dir_is_case_insensitive(repo: Path, path: str) -> None:
    put(repo, path, "x")
    git(repo, "add", "-f", path)
    r = run(repo, "--staged")
    assert r.returncode == 1 and "forbidden-dir" in r.stdout
