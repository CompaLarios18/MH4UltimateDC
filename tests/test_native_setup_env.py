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


def test_apt_step_installs_the_validation_layer(tmp_path):
    """N2-25: the RHI tests run under VK_LAYER_KHRONOS_validation, so the apt step must ask for it (and for the
    shader compiler spirv_inc.py uses). A fake dpkg says nothing is installed; a fake apt-get records its arguments."""
    fake = tmp_path / "bin"
    fake.mkdir()
    log = tmp_path / "apt.log"
    (fake / "dpkg").write_text("#!/bin/sh\nexit 1\n")
    # `update` succeeds so that the script reaches `install`, which fails and ends the script before pip and SDL
    (fake / "apt-get").write_text(f"#!/bin/sh\necho \"$@\" >> {log}\ncase \"$*\" in *update*) exit 0;; esac\nexit 1\n")
    for name in ("dpkg", "apt-get"):
        p = fake / name
        p.chmod(p.stat().st_mode | stat.S_IEXEC)
    home = tmp_path / "home"
    home.mkdir()
    r = _run(["--no-sdl"], path_prefix=str(fake), home=home)
    assert r.returncode != 0
    installs = [line.split() for line in log.read_text().splitlines() if " install " in f" {line} "]
    assert installs, "setup_env.sh never ran apt-get install"
    packages = installs[0]
    for package in ("vulkan-validationlayers", "glslang-tools", "mesa-vulkan-drivers", "libvulkan-dev"):
        assert package in packages, f"{package} is missing from the apt install line"
