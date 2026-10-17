"""port/tests/build_svc_test.py must run its tools by the full path shutil.which finds.

On Windows CreateProcess appends ".exe" only to a name without an extension, so a bare
"ld.lld" is not found although ld.lld.exe is on PATH. Synthetic: nothing is compiled."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "port", "tests"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import build_svc_test as b  # noqa: E402


def test_every_tool_is_run_by_the_resolved_path(tmp_path, monkeypatch):
    calls = []

    def fake_tool(*names):
        return f"/fake/bin/{names[0]}.exe"

    def fake_run(argv, **kw):
        calls.append(argv[0])
        if os.path.basename(argv[0]).startswith("llvm-objcopy"):
            with open(argv[-1], "wb") as f:
                f.write(b"")
        return subprocess.CompletedProcess(argv, 0, stdout="", stderr="")

    monkeypatch.setattr(b.dt, "tool", fake_tool)
    monkeypatch.setattr(b.subprocess, "run", fake_run)
    monkeypatch.setattr(b.recompile, "recompile",
                        lambda *a, **k: {"problems": [], "report": {"trap_instructions": 0}})
    b.build(str(tmp_path), "svc_test")
    assert calls, "nothing ran"
    assert all(c.startswith("/fake/bin/") for c in calls), calls
    assert {os.path.basename(c) for c in calls} == {
        "clang.exe", "ld.lld.exe", "llvm-objcopy.exe", "llvm-readelf.exe"}
