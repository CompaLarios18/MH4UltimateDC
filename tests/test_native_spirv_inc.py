"""tools/native/spirv_inc.py (N2-25): GLSL to SPIR-V arrays, on two synthetic shaders the test writes."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "native" / "spirv_inc.py"

pytestmark = pytest.mark.skipif(shutil.which("glslangValidator") is None, reason="glslangValidator is not on the PATH (apt install glslang-tools)")

VERT = """#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""
FRAG = """#version 450
layout(location = 0) out vec4 o;
void main() {
#ifdef RED
    o = vec4(1.0, 0.0, 0.0, 1.0);
#else
    o = vec4(0.0, 1.0, 0.0, 1.0);
#endif
}
"""


def run(in_dir: Path, out: Path):
    return subprocess.run([sys.executable, str(TOOL), str(in_dir), str(out)], capture_output=True, text=True)


def arrays(text: str) -> dict:
    found = {}
    for m in re.finditer(r"static const uint32_t (\w+)\[\] = \{(.*?)\};", text, re.DOTALL):
        found[m.group(1)] = [int(w, 16) for w in re.findall(r"0x[0-9a-f]{8}", m.group(2))]
    return found


def test_list_file_with_options(tmp_path):
    shaders = tmp_path / "shaders"
    shaders.mkdir()
    (shaders / "quad.vert").write_text(VERT)
    (shaders / "colour.frag").write_text(FRAG)
    (shaders / "spirv_inc.txt").write_text(
        "# name file options\n"
        "spv_quad_vert quad.vert\n"
        "spv_colour_green colour.frag\n"
        "\n"
        "spv_colour_red colour.frag -DRED\n"
    )
    out = tmp_path / "out" / "shaders.inc"
    r = run(shaders, out)
    assert r.returncode == 0, r.stderr
    found = arrays(out.read_text())
    assert sorted(found) == ["spv_colour_green", "spv_colour_red", "spv_quad_vert"]
    for name, words in found.items():
        assert words[0] == 0x07230203, name
        assert len(words) > 20
    assert found["spv_colour_green"] != found["spv_colour_red"]  # the -D option made a difference


def test_without_a_list_each_file_gives_one_array_named_after_it(tmp_path):
    shaders = tmp_path / "shaders"
    shaders.mkdir()
    (shaders / "quad.vert").write_text(VERT)
    (shaders / "colour.frag").write_text(FRAG)
    (shaders / "notes.txt").write_text("not a shader")
    out = tmp_path / "shaders.inc"
    assert run(shaders, out).returncode == 0
    found = arrays(out.read_text())
    assert sorted(found) == ["colour_frag", "quad_vert"]
    assert all(w[0] == 0x07230203 for w in found.values())


def test_glsl_error_exits_nonzero_naming_the_file(tmp_path):
    shaders = tmp_path / "shaders"
    shaders.mkdir()
    (shaders / "broken.frag").write_text("#version 450\nvoid main() { this is not glsl }\n")
    out = tmp_path / "shaders.inc"
    r = run(shaders, out)
    assert r.returncode != 0
    assert "broken.frag" in r.stderr
    assert not out.exists()


def test_missing_file_in_the_list_is_named(tmp_path):
    shaders = tmp_path / "shaders"
    shaders.mkdir()
    (shaders / "spirv_inc.txt").write_text("spv_x nothere.frag\n")
    r = run(shaders, tmp_path / "o.inc")
    assert r.returncode != 0 and "nothere.frag" in r.stderr


def test_committed_include_has_the_evidences_arrays(tmp_path):
    # the committed rhi_plume_shaders.inc has the arrays the evidence's build.sh makes
    text = (ROOT / "native" / "tests" / "rhi_plume_shaders.inc").read_text()
    # the nine arrays of the evidence's build.sh, and the compute shader of descriptors/buffer_offset
    assert sorted(arrays(text)) == sorted([
        "spv_fs_vert", "spv_tri_vert", "spv_tri_frag", "spv_sample_frag", "spv_blend_frag",
        "spv_shadow_store_nil", "spv_shadow_store_il", "spv_shadow_cas_nil", "spv_shadow_cas_il",
        "spv_ubo_offset_comp",
    ])
    out = tmp_path / "again.inc"
    assert run(ROOT / "native" / "tests" / "shaders" / "rhi_plume", out).returncode == 0
    # the words depend on the glslang version of the machine, so compare the shape, not the words
    again = arrays(out.read_text())
    assert sorted(again) == sorted(arrays(text))
    assert all(words[0] == 0x07230203 for words in again.values())
