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


def test_apt_step_installs_the_threadsanitizer_runtime(tmp_path):
    """N2-29: the native-tsan job builds with clang -fsanitize=thread, which needs clang 18's ThreadSanitizer runtime
    (libclang-rt-18-dev on Ubuntu 24.04, the package that holds libclang_rt.tsan); setup_env.sh installs it and says so
    in its report. Same fakes as the test above."""
    fake = tmp_path / "bin"
    fake.mkdir()
    log = tmp_path / "apt.log"
    (fake / "dpkg").write_text("#!/bin/sh\nexit 1\n")
    (fake / "apt-get").write_text(f"#!/bin/sh\necho \"$@\" >> {log}\ncase \"$*\" in *update*) exit 0;; esac\nexit 1\n")
    for name in ("dpkg", "apt-get"):
        p = fake / name
        p.chmod(p.stat().st_mode | stat.S_IEXEC)
    home = tmp_path / "home"
    home.mkdir()
    r = _run(["--no-sdl"], path_prefix=str(fake), home=home)
    assert r.returncode != 0
    installs = [line.split() for line in log.read_text().splitlines() if " install " in f" {line} "]
    assert installs and "libclang-rt-18-dev" in installs[0]
    script = SCRIPT.read_text()
    assert "ThreadSanitizer runtime" in script and "-fsanitize=thread" in script


WORKFLOW = SCRIPT.parent.parent.parent / ".github" / "workflows" / "native.yml"
SUPPRESSIONS = SCRIPT.parent / "tsan.supp"


def _tsan_job() -> str:
    """The text of the `native-tsan` job (from its key to the next job's), without a YAML parser."""
    text = WORKFLOW.read_text(encoding="utf-8")
    start = text.index("\n  native-tsan:\n")
    nxt = [m for m in (text.find("\n  # ", start + 10), text.find("\n  native-windows:", start)) if m != -1]
    return text[start:min(nxt)] if nxt else text[start:]


def test_tsan_job_builds_with_the_sanitizer_in_every_flag_and_installs_the_runtime():
    job = _tsan_job()
    for variable in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS"):
        assert f"-D{variable}=-fsanitize=thread" in job, variable
    assert "-B build/tsan" in job and "build/native" not in job  # a tree of its own
    # one compiler version for the compiler and the runtime package, named because the runner has several clangs
    assert "clang-18" in job and "-DCMAKE_C_COMPILER=clang-18" in job and "-DCMAKE_CXX_COMPILER=clang++-18" in job
    # the runtime package is the one setup_env.sh installs
    assert "libclang-rt-18-dev" in job and "libclang-rt-18-dev" in SCRIPT.read_text()
    # the targets come from ctest -N over the RHI regex (names that do not exist yet match nothing)
    regex = "^(rhi_.*|gpu_backend_.*|gpu_queue_test|present_test|swapchain_test)$"
    assert job.count(regex) == 1  # written once, in the job's env
    assert f'  TSAN_TESTS: "{regex}"' in job and f"-R '{regex}'" in WORKFLOW.read_text(encoding="utf-8")  # the layer step of `native` has the same
    assert "ctest --test-dir build/tsan -N -R" in job
    assert 'MH4N_REQUIRE_VULKAN: "1"' in job
    assert "halt_on_error=1" in job and "tools/native/tsan.supp" in job
    # the control that races on purpose is never named
    assert "control/D2" not in job and "--no-skip" not in job


def test_tsan_suppressions_name_only_the_loader_and_the_software_driver():
    """The suppressions name the Vulkan loader, lavapipe's driver library and the LLVM lavapipe loads (a race: entry only,
    `libLLVM` as a substring of its versioned file names: a report seen in CI had libLLVM.so.20.1 frames only), and nothing
    else (nothing of plume or of ours). They are race:/mutex: entries: a called_from_lib entry for a library the loader
    dlcloses (the driver, when an instance is destroyed) makes ThreadSanitizer stop the process with "library ... is
    unloaded"."""
    lines = [ln.strip() for ln in SUPPRESSIONS.read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    assert lines, "tsan.supp has no suppression"
    allowed = {("race", "libvulkan.so"), ("race", "libvulkan_lvp.so"), ("race", "libLLVM"), ("mutex", "libvulkan.so")}
    entries = [tuple(line.partition(":")[::2]) for line in lines]
    for entry, line in zip(entries, lines):
        assert entry in allowed, line
    assert len(entries) == len(set(entries)) and set(entries) == allowed
