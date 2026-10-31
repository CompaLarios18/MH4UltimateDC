"""tools/native/rhi_patches.py and the vendored plume and VMA trees (N2-25): the applier on synthetic diffs, and the
real trees against their queues, the blob ids and the evidence's checksums."""
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TOOL = ROOT / "tools" / "native" / "rhi_patches.py"
PLUME = ROOT / "native" / "third_party" / "rhi" / "plume"
VMA = ROOT / "native" / "third_party" / "rhi" / "vma"
EVIDENCE = ROOT / "docs" / "research" / "rhi-spike-evidence"
sys.path.insert(0, str(ROOT / "tools" / "native"))

import rhi_patches as rp  # noqa: E402

ORIGINAL = b"alpha\nbeta\ngamma\ndelta\nepsilon\n"

GOOD_PATCH = b"""diff --git a/f.txt b/f.txt
index 1111111..2222222 100644
--- a/f.txt
+++ b/f.txt
@@ -1,3 +1,4 @@
 alpha
-beta
+BETA
+beta2
 gamma
@@ -5,1 +6,1 @@
-epsilon
+EPSILON
"""


def make_root(tmp_path: Path, patches: dict, files: dict | None = None, commit: str = "a" * 40) -> Path:
    root = tmp_path / "plume"
    (root / "pristine").mkdir(parents=True)
    (root / "patches").mkdir()
    for name, data in (files or {"f.txt": ORIGINAL}).items():
        (root / "pristine" / name).write_bytes(data)
    for name, data in patches.items():
        (root / "patches" / name).write_bytes(data)
    (root / "README.md").write_text(f"Pinned commit: `{commit}`\n", encoding="utf-8")
    return root


def run_tool(command: str, root: Path):
    return subprocess.run([sys.executable, str(TOOL), command, "--root", str(root)], capture_output=True, text=True)


# ---- the applier on synthetic diffs ---------------------------------------------------------------------------------

def test_clean_apply_two_hunks():
    (fp,) = rp.parse_patch(GOOD_PATCH, "good")
    assert rp.apply_file_patch(ORIGINAL, fp, "good") == b"alpha\nBETA\nbeta2\ngamma\ndelta\nEPSILON\n"


def test_hunk_that_does_not_apply_is_refused():
    changed = ORIGINAL.replace(b"gamma", b"GAMMA")
    (fp,) = rp.parse_patch(GOOD_PATCH, "good")
    with pytest.raises(rp.PatchError, match="does not apply"):
        rp.apply_file_patch(changed, fp, "good")


def test_hunk_at_the_wrong_line_is_refused():
    shifted = b"zero\n" + ORIGINAL  # the context still exists, one line lower: no fuzz, no offset
    (fp,) = rp.parse_patch(GOOD_PATCH, "good")
    with pytest.raises(rp.PatchError):
        rp.apply_file_patch(shifted, fp, "good")


def test_removed_line_must_match():
    patch = GOOD_PATCH.replace(b"-beta\n", b"-BETAX\n")
    (fp,) = rp.parse_patch(patch, "p")
    with pytest.raises(rp.PatchError, match="does not apply"):
        rp.apply_file_patch(ORIGINAL, fp, "p")


def test_short_hunk_is_a_parse_error():
    with pytest.raises(rp.PatchError, match="shorter than its header"):
        rp.parse_patch(b"--- a/f\n+++ b/f\n@@ -1,3 +1,3 @@\n a\n b\n", "short")


def test_no_newline_at_end_of_file():
    original = b"one\ntwo"
    patch = b"--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n one\n-two\n\\ No newline at end of file\n+three\n\\ No newline at end of file\n"
    (fp,) = rp.parse_patch(patch, "nonl")
    assert rp.apply_file_patch(original, fp, "nonl") == b"one\nthree"


def test_created_and_deleted_files():
    create = b"--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+x\n+y\n"
    (fp,) = rp.parse_patch(create, "c")
    assert rp.apply_file_patch(None, fp, "c") == b"x\ny\n"
    with pytest.raises(rp.PatchError):
        rp.apply_file_patch(b"x\n", fp, "c")  # it already exists
    delete = b"--- a/old.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-x\n-y\n"
    (fp,) = rp.parse_patch(delete, "d")
    assert rp.apply_file_patch(b"x\ny\n", fp, "d") is None


def test_renames_are_not_supported():
    with pytest.raises(rp.PatchError, match="not supported"):
        rp.parse_patch(b"diff --git a/a b/b\nrename from a\nrename to b\n", "r")


def test_patch_numbers_must_run_without_a_gap(tmp_path):
    root = make_root(tmp_path, {"01-a.diff": GOOD_PATCH, "03-c.diff": GOOD_PATCH})
    with pytest.raises(rp.PatchError, match="expected patch number 02"):
        rp.list_patches(root / "patches")
    root2 = make_root(tmp_path / "x", {"1-a.diff": GOOD_PATCH})
    with pytest.raises(rp.PatchError, match="NN-"):
        rp.list_patches(root2 / "patches")


# ---- apply and check on a synthetic tree ----------------------------------------------------------------------------

def test_apply_then_check(tmp_path):
    second = b"--- a/f.txt\n+++ b/f.txt\n@@ -1,1 +1,1 @@\n-alpha\n+ALPHA\n"
    root = make_root(tmp_path, {"01-first.diff": GOOD_PATCH, "02-second.diff": second}, {"f.txt": ORIGINAL, "untouched.txt": b"u\n"})
    assert run_tool("check", root).returncode == 1  # no patched/ yet
    r = run_tool("apply", root)
    assert r.returncode == 0, r.stderr
    assert (root / "patched" / "f.txt").read_bytes() == b"ALPHA\nBETA\nbeta2\ngamma\ndelta\nEPSILON\n"
    assert (root / "patched" / "untouched.txt").read_bytes() == b"u\n"
    header = (root / "patched" / "mh4n_patch_list.h").read_text()
    assert "a" * 40 in header
    assert '"01-first.diff"' in header and '"02-second.diff"' in header and "kPatchCount = 2" in header
    assert run_tool("check", root).returncode == 0


def test_check_names_the_first_file_that_differs(tmp_path):
    root = make_root(tmp_path, {"01-first.diff": GOOD_PATCH}, {"f.txt": ORIGINAL, "g.txt": b"g\n"})
    assert run_tool("apply", root).returncode == 0
    (root / "patched" / "g.txt").write_bytes(b"edited by hand\n")
    r = run_tool("check", root)
    assert r.returncode == 1
    assert "patched/g.txt" in r.stdout
    (root / "patched" / "g.txt").write_bytes(b"g\n")
    (root / "patched" / "extra.txt").write_bytes(b"x")
    r = run_tool("check", root)
    assert r.returncode == 1 and "patched/extra.txt" in r.stdout


def test_apply_refuses_a_patch_that_does_not_apply(tmp_path):
    root = make_root(tmp_path, {"01-bad.diff": GOOD_PATCH}, {"f.txt": b"something else\n" * 6})
    r = run_tool("apply", root)
    assert r.returncode == 2
    assert "01-bad.diff" in r.stderr and "does not apply" in r.stderr
    assert not (root / "patched").exists()


def test_apply_removes_stale_files(tmp_path):
    root = make_root(tmp_path, {"01-first.diff": GOOD_PATCH})
    (root / "patched").mkdir()
    (root / "patched" / "stale.txt").write_bytes(b"old")
    assert run_tool("apply", root).returncode == 0
    assert not (root / "patched" / "stale.txt").exists()


def test_blobs_on_a_synthetic_tree(tmp_path):
    root = make_root(tmp_path, {"01-first.diff": GOOD_PATCH})
    blob = hashlib.sha1(b"blob %d\0" % len(ORIGINAL) + ORIGINAL).hexdigest()
    readme = root / "README.md"
    readme.write_text(f"Pinned commit: `{'a' * 40}`\n\n| File | Blob id |\n|---|---|\n| `f.txt` | `{blob}` |\n")
    assert run_tool("blobs", root).returncode == 0
    (root / "pristine" / "f.txt").write_bytes(ORIGINAL + b"x\n")
    r = run_tool("blobs", root)
    assert r.returncode == 1 and "f.txt" in r.stdout
    (root / "pristine" / "f.txt").write_bytes(ORIGINAL)
    (root / "pristine" / "unlisted.txt").write_bytes(b"u")
    r = run_tool("blobs", root)
    assert r.returncode == 1 and "unlisted.txt" in r.stdout


def test_git_blob_id_known_value():
    # `printf 'hello\n' | git hash-object --stdin`
    assert rp.git_blob_id(b"hello\n") == "ce013625030ba8dba906f756967f9e9ca394464a"


# ---- the real tree --------------------------------------------------------------------------------------------------

def test_tree_check_passes():
    r = subprocess.run([sys.executable, str(TOOL), "check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_tree_blobs_pass():
    r = subprocess.run([sys.executable, str(TOOL), "blobs"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_patches_01_to_07_are_the_evidence_files():
    sums = {}
    for line in (EVIDENCE / "SHA256SUMS").read_text().splitlines():
        digest, _, name = line.partition("  ")
        sums[name] = digest
    queue = sorted(p for p in (PLUME / "patches").iterdir() if p.suffix == ".diff")
    checked = 0
    for patch in queue[:7]:
        key = f"./patches/{patch.name}"
        assert key in sums, f"{patch.name} is not in SHA256SUMS"
        assert hashlib.sha256(patch.read_bytes()).hexdigest() == sums[key], patch.name
        checked += 1
    assert checked == 7
    # the evidence folder keeps its own copies, which are the same bytes
    for patch in queue[:7]:
        assert (EVIDENCE / "patches" / patch.name).read_bytes() == patch.read_bytes()


def test_queue_is_numbered_from_one():
    names = [p.name for p in rp.list_patches(PLUME / "patches")]
    assert len(names) >= 11
    assert [int(n[:2]) for n in names] == list(range(1, len(names) + 1))


def test_every_patch_is_listed_in_the_readme():
    readme = (PLUME / "README.md").read_text(encoding="utf-8")
    for patch in rp.list_patches(PLUME / "patches"):
        assert f"`{patch.name}`" in readme, patch.name


def test_pinned_commit():
    assert rp.pinned_commit(PLUME) == "0ce184b19270e49236afbb74052a6b267494b86b"


def test_pristine_files_are_the_trimmed_vulkan_set():
    names = sorted(rp.read_tree(PLUME / "pristine"))
    assert names == [
        "LICENSE",
        "plume_render_interface.h",
        "plume_render_interface_builders.h",
        "plume_render_interface_types.h",
        "plume_vulkan.cpp",
        "plume_vulkan.h",
    ]


def _border_colour_patch():
    """The patch that adds the custom border colour: found by its name, so a renumbering does not break the tests."""
    found = [p for p in rp.list_patches(PLUME / "patches") if p.name[2:] == "-custom-border-colour.diff"]
    assert len(found) == 1
    return found[0]


def test_custom_border_colour_patch_is_a_new_file_after_the_earlier_ones():
    """N2-29: patch 12 is a file of its own after N2-25's patches 01 to 11 (never an edit of one of them), touches only the
    two plume files it must, and the patched copy has the create-info chained behind the device's capability."""
    patch = _border_colour_patch()
    queue = rp.list_patches(PLUME / "patches")
    assert int(patch.name[:2]) >= 12 and queue.index(patch) >= 11
    touched = sorted(fp.new_path for fp in rp.parse_patch(patch.read_bytes(), patch.name))
    assert touched == ["plume_render_interface_types.h", "plume_vulkan.cpp"]
    assert f"`{patch.name}`" in (PLUME / "README.md").read_text(encoding="utf-8")
    assert f'"{patch.name}"' in (PLUME / "patched" / "mh4n_patch_list.h").read_text(encoding="utf-8")

    cpp = (PLUME / "patched" / "plume_vulkan.cpp").read_text(encoding="utf-8")
    ctor = cpp[cpp.index("VulkanSampler::VulkanSampler("):]
    ctor = ctor[:ctor.index("VulkanSampler::~VulkanSampler")]
    gate = ctor.index("!device->capabilities.customBorderColor")
    chain = ctor.index("VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT")
    create = ctor.index("vkCreateSampler(")
    assert gate < chain < create  # the create-info is chained only after the capability check, before the call
    assert "return;" in ctor[gate:chain]  # a device without the extension gets a message and a null sampler
    assert "VK_BORDER_COLOR_FLOAT_CUSTOM_EXT" in cpp
    assert "customBorderColorFeatures.customBorderColorWithoutFormat" in cpp
    header = (PLUME / "patched" / "plume_render_interface_types.h").read_text(encoding="utf-8")
    assert "float customBorderColor[4]" in header and "CUSTOM" in header


def test_custom_border_colour_patch_changes_only_what_it_says(tmp_path):
    """Without the patch the queue still applies (11 patches) and the border-colour code is absent; with it the patched
    copy differs from that tree in the two plume files and the patch list only."""
    patch = _border_colour_patch()
    root = tmp_path / "plume"
    (root / "patches").mkdir(parents=True)
    for source in (PLUME / "patches").iterdir():
        if source != patch:
            (root / "patches" / source.name).write_bytes(source.read_bytes())
    (root / "pristine").mkdir()
    for name, data in rp.read_tree(PLUME / "pristine").items():
        (root / "pristine" / name).write_bytes(data)
    (root / "README.md").write_bytes((PLUME / "README.md").read_bytes())
    without = rp.build_patched(root)
    with_patch = rp.read_tree(PLUME / "patched")
    differing = sorted(name for name in with_patch if with_patch[name] != without.get(name))
    assert differing == ["mh4n_patch_list.h", "plume_render_interface_types.h", "plume_vulkan.cpp"]
    assert b"VK_BORDER_COLOR_FLOAT_CUSTOM_EXT" not in without["plume_vulkan.cpp"]
    assert b"SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT" not in without["plume_vulkan.cpp"]
    assert b"customBorderColor[4]" not in without["plume_render_interface_types.h"]


# ---- the VMA queue --------------------------------------------------------------------------------------------------

UPSTREAM_MAPPED_DATA_FIX = "4f687952b8c7aa250dfc25f78e269b9834872458"


def _function(source: str, signature: str) -> str:
    """One function of vk_mem_alloc.h, from its signature to the brace that closes it at column 0."""
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


def test_vma_check_and_blobs_pass():
    for command in ("check", "blobs"):
        r = run_tool(command, VMA)
        assert r.returncode == 0, r.stdout + r.stderr
        assert "vma" in r.stdout


def test_vma_pinned_commit():
    assert rp.pinned_commit(VMA) == "29b35ea4232688c0f42cdff0c10848290760a417"


def test_vma_pristine_files_are_the_header_and_the_licence_as_they_were():
    tree = rp.read_tree(VMA / "pristine")
    assert sorted(tree) == ["LICENSE.txt", "vk_mem_alloc.h"]
    # the blob ids of 29b35ea, which the two files had before they moved into pristine/
    assert rp.git_blob_id(tree["vk_mem_alloc.h"]) == "6f71d5b4604d8e58aba4f340552ab092be190606"
    assert rp.git_blob_id(tree["LICENSE.txt"]) == "a080d9bf71fcf40b4714c6fab0130f6a9d973d7f"


def test_vma_queue_is_the_upstream_fix_and_patched_holds_only_the_library():
    names = [p.name for p in rp.list_patches(VMA / "patches")]
    assert names == ["01-upstream-4f68795-mapped-data-sync.diff"]
    patch = VMA / "patches" / names[0]
    note = patch.read_text(encoding="utf-8").split("diff --git", 1)[0]  # the text before the first file header
    assert UPSTREAM_MAPPED_DATA_FIX in note and "#525" in note and "MIT" in note
    assert [fp.new_path for fp in rp.parse_patch(patch.read_bytes(), patch.name)] == ["vk_mem_alloc.h"]
    assert f"`{names[0]}`" in (VMA / "README.md").read_text(encoding="utf-8")
    assert sorted(rp.read_tree(VMA / "patched")) == ["LICENSE.txt", "vk_mem_alloc.h"]  # no mh4n_patch_list.h here
    assert names[0] not in (PLUME / "patched" / "mh4n_patch_list.h").read_text(encoding="utf-8")


def test_vma_patched_header_reads_the_atomic_flag_in_allocate_page():
    """Patch 01 (upstream 4f68795, issue #525): AllocatePage, under the block vector's mutex, read each block's
    m_pMappedData, which vmaMapMemory and vmaUnmapMemory of another allocation in the block write under the block's own
    mutex only (the data race of the native-tsan job, docs/native/rhi.md); it now reads an atomic flag that follows the
    pointer wherever the pointer changes."""
    pristine = (VMA / "pristine" / "vk_mem_alloc.h").read_text(encoding="utf-8")
    patched = (VMA / "patched" / "vk_mem_alloc.h").read_text(encoding="utf-8")
    page = "VkResult VmaBlockVector::AllocatePage("
    assert "pCurrBlock->GetMappedData() != VMA_NULL" in _function(pristine, page)  # the read the patch replaces
    assert "m_IsMapped" not in pristine
    assert "pCurrBlock->IsMapped()" in _function(patched, page)
    assert "GetMappedData() != VMA_NULL" not in _function(patched, page)
    assert "#define VMA_ATOMIC_BOOL std::atomic<bool>" in patched
    assert "VMA_ATOMIC_BOOL m_IsMapped;" in patched
    assert "bool IsMapped() const { return m_IsMapped.load(); }" in patched
    assert "m_pMappedData(VMA_NULL),\n    m_IsMapped(false){}" in patched
    mapped = _function(patched, "VkResult VmaDeviceMemoryBlock::Map(")
    assert mapped.index("vkMapMemory") < mapped.index("m_IsMapped.store(true);")
    for signature in ("void VmaDeviceMemoryBlock::Unmap(", "void VmaDeviceMemoryBlock::PostFree(",
                      "void VmaDeviceMemoryBlock::Destroy("):
        assert re.search(r"m_pMappedData = VMA_NULL;\n\s*m_IsMapped\.store\(false\);", _function(patched, signature)), signature


def test_without_root_both_queues_run_and_the_worse_status_wins(tmp_path, monkeypatch, capsys):
    r = subprocess.run([sys.executable, str(TOOL), "check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert 0 <= r.stdout.index("plume/patched/ matches") < r.stdout.index("vma/patched/ matches")
    # a stale queue and a good one: the stale one decides the status, and the good one still runs after it
    stale = make_root(tmp_path / "a", {"01-first.diff": GOOD_PATCH})
    good = make_root(tmp_path / "b", {"01-first.diff": GOOD_PATCH})
    monkeypatch.setattr(rp, "QUEUES", [stale, good])
    assert rp.main(["apply"]) == 0
    (stale / "patched" / "f.txt").write_bytes(b"edited by hand\n")
    capsys.readouterr()
    assert rp.main(["check"]) == 1
    out = capsys.readouterr().out
    assert "f.txt differs" in out and "patched/ matches" in out
    (stale / "patches" / "01-first.diff").write_bytes(GOOD_PATCH.replace(b" gamma", b" GAMMA"))
    assert rp.main(["check"]) == 2  # a patch that does not apply is worse than a stale tree
    assert rp.main(["check", "--root", str(good)]) == 0


def test_the_build_compiles_the_patched_vma():
    cmake = (ROOT / "native" / "src" / "gpu" / "backend" / "plume" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert '"${MH4N_RHI_DIR}/vma/patched"' in cmake
    assert "vma/pristine" not in cmake and '"${MH4N_RHI_DIR}/vma"' not in cmake


# ---- one volk, one VMA, no window-system header ---------------------------------------------------------------------

def _translation_units():
    """C, C++ and Objective-C++ sources and headers under native/ that a build compiles or includes: not plume's pristine copy (never built)."""
    skip = PLUME / "pristine"
    for path in (ROOT / "native").rglob("*"):
        if path.suffix in (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc", ".mm") and skip not in path.parents:
            yield path


def test_exactly_one_volk_and_one_vma_translation_unit():
    volk, vma = [], []
    vendored = {PLUME.parent / "volk", PLUME.parent / "vma"}  # the libraries' own headers mention the macros
    for path in _translation_units():
        if vendored & set(path.parents):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if re.search(r"^\s*#\s*define\s+VOLK_IMPLEMENTATION\b", text, re.MULTILINE):
            volk.append(path.relative_to(ROOT).as_posix())
        if re.search(r"^\s*#\s*define\s+VMA_IMPLEMENTATION\b", text, re.MULTILINE):
            vma.append(path.relative_to(ROOT).as_posix())
    assert volk == ["native/third_party/rhi/plume/patched/plume_vulkan.cpp"]
    assert vma == ["native/third_party/rhi/plume/patched/plume_vulkan.cpp"]


def test_patched_copy_has_no_window_system_include_when_external_surface_only():
    # patch 08: the only window-system headers sit in branches that PLUME_EXTERNAL_SURFACE_ONLY skips
    types = (PLUME / "patched" / "plume_render_interface_types.h").read_text()
    assert "#if defined(PLUME_EXTERNAL_SURFACE_ONLY)" in types
    assert types.index("PLUME_EXTERNAL_SURFACE_ONLY") < types.index("#include <Windows.h>")
    header = (PLUME / "patched" / "plume_vulkan.h").read_text()
    assert header.index("PLUME_EXTERNAL_SURFACE_ONLY") < header.index("VK_USE_PLATFORM_WIN32_KHR")


def test_gitattributes_keep_the_compared_files_byte_exact():
    text = (ROOT / ".gitattributes").read_text()
    assert "native/third_party/rhi/** -text" in text.splitlines()
    assert "docs/research/rhi-spike-evidence/patches/*.diff -text" in text.splitlines()
