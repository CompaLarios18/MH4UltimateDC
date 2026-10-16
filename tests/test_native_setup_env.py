"""Tests for tools/native/setup_env.sh (synthetic: no network, no installs)."""
import os
import stat
import subprocess
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "tools" / "native" / "setup_env.sh"


def _run(args, path_prefix=None, home=None):
    env = dict(os.environ)
    if path_prefix:
        env["PATH"] = f"{path_prefix}:{env['PATH']}"
    if home:
        env["HOME"] = str(home)
    return subprocess.run(["bash", str(SCRIPT), *args], capture_output=True, text=True, env=env, timeout=60)


def test_syntax():
    assert subprocess.run(["bash", "-n", str(SCRIPT)]).returncode == 0


def test_help_lists_flags():
    r = _run(["--help"])
    assert r.returncode == 0
    for flag in ("--no-sdl", "--with-azahar", "--azahar-only"):
        assert flag in r.stdout


def test_unknown_flag_fails():
    r = _run(["--bogus"])
    assert r.returncode != 0
    assert "setup_env: done" not in r.stdout


def test_failing_apt_step_exits_nonzero(tmp_path):
    fake = tmp_path / "bin"
    fake.mkdir()
    # dpkg reports nothing installed, apt-get always fails
    for name in ("dpkg", "apt-get"):
        p = fake / name
        p.write_text("#!/bin/sh\nexit 1\n")
        p.chmod(p.stat().st_mode | stat.S_IEXEC)
    home = tmp_path / "home"
    home.mkdir()
    r = _run(["--no-sdl"], path_prefix=str(fake), home=home)
    assert r.returncode != 0
    assert "setup_env: done" not in r.stdout
    assert "setup_env: done" not in r.stderr
