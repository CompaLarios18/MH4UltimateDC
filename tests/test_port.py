"""PC port tests (no game data).

The test program port/tests/svc_test.c must recompile cleanly. If a port build is
available (MH4U_TEST_HEADLESS=path/to/mh4u_test_headless, see port/README.md), the
program also runs under Azahar's kernel with the recompiled code and with Azahar's JIT,
and both must print the same thing.
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


def emulated_output(binary: str, elf: str, cpu: str) -> list[str]:
    r = subprocess.run([binary, elf, "--cpu", cpu, "--ms", "5000"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (cpu, r.returncode, r.stdout[-2000:], r.stderr[-2000:])
    lines = []
    for line in (r.stdout + r.stderr).splitlines():
        m = re.search(r"OutputDebugString:\d+: (.*)$", line)
        if m:
            lines.append(m.group(1))
    return lines


@pytest.mark.skipif(not os.environ.get("MH4U_TEST_HEADLESS"), reason="port not built (set MH4U_TEST_HEADLESS)")
def test_recompiled_backend_matches_jit():
    binary = os.environ["MH4U_TEST_HEADLESS"]
    elf = os.environ.get("MH4U_TEST_ELF", os.path.join(ROOT, "build", "port-test", "svc_test.elf"))
    jit = emulated_output(binary, elf, "jit")
    recomp = emulated_output(binary, elf, "recomp")
    assert jit and jit[0] == "svc_test: start" and jit[-1] == "svc_test: done", jit
    assert recomp == jit
