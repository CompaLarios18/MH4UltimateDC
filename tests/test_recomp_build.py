"""tools/recomp/build.py: incremental builds keep objects of different settings apart."""
import os
import shutil
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

pytestmark = pytest.mark.skipif(shutil.which("clang") is None, reason="clang not installed")


def test_objects_per_build_variant(tmp_path):
    from recomp import build as rbuild
    src = tmp_path / "src"
    src.mkdir()
    (src / "a.c").write_text("#ifdef VARIANT\nint variant = 1;\n#endif\nint a(void) { return 1; }\n")
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [])
    rbuild.build(str(tmp_path), 1, "O0", [], "two.so", [], extra_flags=["-DVARIANT"])
    objs = sorted(os.listdir(tmp_path / "obj"))
    assert len(objs) == 2 and all(o.startswith("a-O0-") for o in objs)
    stamp = {o: os.path.getmtime(tmp_path / "obj" / o) for o in objs}
    rbuild.build(str(tmp_path), 1, "O0", [], "one.so", [])        # nothing to recompile
    assert {o: os.path.getmtime(tmp_path / "obj" / o) for o in objs} == stamp
