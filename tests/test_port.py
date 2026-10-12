"""PC port tests (no game data).

The test program port/tests/svc_test.c must recompile cleanly, and its code must build
into a code library with tools/recomp/build.py. If a port build is available
(MH4U_RUNNER=path/to/mh4u_runner; see port/README.md), the program also runs under Azahar's kernel with the recompiled code and
with Azahar's JIT, and both must print the same thing.
"""
import os
import re
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(__file__)
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "port", "tests"))

needed = ["clang", "ld.lld", "llvm-objcopy", "llvm-readelf"]
pytestmark = pytest.mark.skipif(any(shutil.which(t) is None for t in needed),
                                reason="clang/lld toolchain not installed")


def test_svc_test_program_recompiles(tmp_path):
    import build_svc_test
    elf = build_svc_test.build(str(tmp_path))
    assert os.path.exists(elf)
    src = os.listdir(tmp_path / "src")
    assert "table.c" in src and any(f.startswith("f") and f.endswith(".c") for f in src)
    # the short-vector routine was found and expanded
    code = "".join((tmp_path / "src" / f).read_text() for f in src if f.endswith(".c"))
    assert "ctx->f.sw[11] = vadd32" in code


def test_code_library_builds(tmp_path):
    import build_svc_test
    from recomp import build as rbuild
    build_svc_test.build(str(tmp_path))
    lib = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                       extra_flags=["-fvisibility=hidden"])
    syms = subprocess.run(["llvm-nm", "-D", "--defined-only", lib], capture_output=True, text=True, check=True).stdout
    exported = {line.split()[-1] for line in syms.splitlines()}
    assert "rc_code_init" in exported and "rc_call" not in exported


def emulated_output(binary: str, code: str, elf: str, cpu: str) -> list[str]:
    r = subprocess.run([binary, elf, "--code", code, "--cpu", cpu, "--ms", "5000"], capture_output=True,
                       text=True, timeout=600)
    assert r.returncode == 0, (cpu, r.returncode, r.stdout[-2000:], r.stderr[-2000:])
    lines = []
    for line in (r.stdout + r.stderr).splitlines():
        m = re.search(r"OutputDebugString:\d+: (.*)$", line)
        if m:
            lines.append(m.group(1))
    return lines


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_recompiled_backend_matches_jit(tmp_path):
    binary = os.environ["MH4U_RUNNER"]
    code = os.environ.get("MH4U_TEST_CODE")
    if code:
        elf = os.environ.get("MH4U_TEST_ELF", os.path.join(ROOT, "build", "port-test", "svc_test.elf"))
    else:   # build program and code library here (tools/recomp/build.py --code-library)
        import build_svc_test
        from recomp import build as rbuild
        elf = build_svc_test.build(str(tmp_path))
        code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                            extra_flags=["-fvisibility=hidden"])
    jit = emulated_output(binary, code, elf, "jit")
    recomp = emulated_output(binary, code, elf, "recomp")
    assert jit and jit[0] == "svc_test: start" and jit[-1] == "svc_test: done", jit
    assert recomp == jit


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_interpreter_fallback_matches_jit(tmp_path):
    """Functions missing from the code library (here: whole worker threads and functions
    called through pointers) run in Azahar's interpreter; the output must not change."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path))
    syms = {}
    for line in subprocess.run(["llvm-readelf", "-s", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC":
            syms[p[7]] = int(p[1], 16) & ~1
    drop = {syms[n] for n in ("worker", "op_add", "op_xor", "op_mul")}
    table = tmp_path / "src" / "table.c"
    lines = [ln for ln in table.read_text().splitlines() if not any(f"{{0x{a:08x}u," in ln for a in drop)]
    count = sum(1 for ln in lines if ln.strip().startswith("{0x"))
    table.write_text("\n".join(re.sub(r"rc_func_count = \d+;", f"rc_func_count = {count};", ln) for ln in lines) + "\n")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    r = subprocess.run([binary, elf, "--code", code, "--cpu", "recomp", "--ms", "5000"], capture_output=True,
                       text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    interpreted = set(re.findall(r"interpreting ([0-9A-F]{8})", r.stdout + r.stderr))
    assert interpreted == {f"{a:08X}" for a in drop}
    assert emulated_output(binary, code, elf, "recomp") == emulated_output(binary, code, elf, "jit")


@pytest.mark.skipif(not os.environ.get("MH4U_RUNNER"), reason="port not built (set MH4U_RUNNER)")
def test_runner_scripted_input_and_screenshots(tmp_path):
    """port/tests/io_test.c reads HID shared memory and draws into the top framebuffer:
    the runner's --input script must reach it and --shots must save what it drew."""
    import build_svc_test
    from recomp import build as rbuild
    binary = os.environ["MH4U_RUNNER"]
    elf = build_svc_test.build(str(tmp_path), "io_test")
    code = rbuild.build(str(tmp_path), 2, "O1", ["RC_PAGED"], "code.so", [rbuild.DLL_GLUE],
                        extra_flags=["-fvisibility=hidden"])
    shots = tmp_path / "shots"
    r = subprocess.run([binary, elf, "--code", code, "--ms", "3000", "--user", str(tmp_path / "user"),
                        "--shots", str(shots), "--input",
                        "300:A,600:TOUCH:10:20:100,900:CIRCLE:1:-0.5:200,1200:START"],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    out = [m.group(1) for m in re.finditer(r"OutputDebugString:\d+: (.*)", r.stdout + r.stderr)]
    events = {}
    for line in out:
        m = re.fullmatch(r"(.*) ([0-9a-f]{8})", line)
        if m:
            events[m.group(1)] = int(m.group(2), 16)
    assert 15 <= events["A pressed, frame"] <= 21          # 300 ms at ~60 polls per second
    assert 69 <= events["START pressed, frame"] <= 75      # 1200 ms
    assert events["touch x"] == 10 and events["touch y"] == 20
    assert "circle right" in events and "circle down" in events
    assert (tmp_path / "user" / "nand").is_dir()
    data = (shots / "0001000_top.ppm").read_bytes()
    magic, size, _, pixels = data.split(b"\n", 3)
    width, height = map(int, size.split())
    assert (magic, width, height) == (b"P6", 400, 240)

    def pixel(x, y):
        i = (y * width + x) * 3
        return tuple(pixels[i:i + 3])
    assert pixel(100, 5) == (0, 255, 0) and pixel(100, 120) == (255, 0, 0) and pixel(300, 120) == (0, 0, 255)
