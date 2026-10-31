"""native/src/gpu/backend/rhi.h (N2-26): the renderer-facing interface of the GPU backend.

It is operation level (rhi-spike.md 9.4 item 1): it includes only the standard library and mh4n/ headers,
names no Vulkan identifier and no plume type, no Vulkan image layout, render-pass or descriptor-set type,
compiles alone, and declares every operation 9.4 lists. vulkan_access.h is the Vulkan side the boundary
leaves out, so the rules are different there (it includes volk) and are checked separately."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
BACKEND = ROOT / "native" / "src" / "gpu" / "backend"
RHI = BACKEND / "rhi.h"
ACCESS = BACKEND / "vulkan_access.h"
PLUME_TYPES = ROOT / "native" / "third_party" / "rhi" / "plume" / "patched" / "plume_render_interface_types.h"

# The standard headers rhi.h may include (C++ headers without an extension, and the <c...> forms of C's).
STD_HEADER = re.compile(r"^(c[a-z]+|[a-z_]+)$")


def includes(path: Path):
    found = []
    for line in path.read_text().splitlines():
        m = re.match(r'\s*#\s*include\s*([<"])([^>"]+)[>"]', line)
        if m:
            found.append((m.group(1), m.group(2)))
    return found


def plume_type_names() -> set[str]:
    """Every struct, class, enum, namespace and typedef name of plume's shared types header."""
    names: set[str] = set()
    for line in PLUME_TYPES.read_text().splitlines():
        m = re.match(r"\s*(?:struct|class|enum class|enum|namespace)\s+([A-Za-z_]\w*)", line)
        if m:
            names.add(m.group(1))
        m = re.match(r"\s*typedef\s+[\w:\s\*]+?\s+([A-Za-z_]\w*)\s*;", line)
        if m:
            names.add(m.group(1))
    return names


def tokens(path: Path) -> list[str]:
    # the whole file, comments included: the interface is documented in its own comments
    return re.findall(r"[A-Za-z_]\w*", path.read_text())


def test_rhi_h_includes_only_standard_and_mh4n_headers():
    found = includes(RHI)
    assert found, "rhi.h includes nothing?"
    for kind, name in found:
        if kind == '"':
            assert name.startswith("mh4n/"), f"rhi.h includes a header of its own: {name}"
        else:
            assert STD_HEADER.match(name), f"rhi.h includes <{name}>, which is not a standard header"
    assert ("<", "span") in found and ('"', "mh4n/types.h") in found


def test_rhi_h_names_no_vulkan_identifier():
    bad = sorted({t for t in tokens(RHI) if re.match(r"(Vk|VK_|vk)", t)})
    assert not bad, f"rhi.h names Vulkan identifiers: {bad}"


def test_rhi_h_names_no_plume_type():
    assert not [t for t in tokens(RHI) if "plume" in t.lower()], "rhi.h names plume"
    names = plume_type_names()
    assert len(names) > 50 and "RenderFormat" in names, "the types header was not read"
    clash = sorted({t for t in tokens(RHI) if t in names})
    assert not clash, f"rhi.h uses the names of plume's types: {clash}"


def test_rhi_h_never_names_a_layout_a_render_pass_or_a_descriptor_set():
    """Image layouts, render-pass objects, descriptor sets and pipeline layouts stay below the interface."""
    bad = []
    for t in set(tokens(RHI)):
        if re.search(r"(ImageLayout|TextureLayout|RenderPass|DescriptorSet|PipelineLayout|Descriptor)", t):
            bad.append(t)
        if "Layout" in t and t != "VertexLayout":
            bad.append(t)
    assert not sorted(set(bad)), f"rhi.h names layout, render-pass or descriptor-set types: {sorted(set(bad))}"


def test_rhi_h_compiles_alone():
    compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
    if compiler is None:
        pytest.skip("no C++ compiler")
    result = subprocess.run(
        [compiler, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror", "-Wno-pragma-once-outside-header",
         "-x", "c++", "-I", str(ROOT / "native" / "include"), str(RHI)],
        capture_output=True, text=True, cwd=ROOT)
    assert result.returncode == 0, result.stderr


# The operations of 9.4 and of N2-26's list, by the module that owns them. A rename or a removal is a change
# of a frozen header, which only the orchestrator makes.
CORE = ["Result", "CreateInstance", "InstanceOptions", "CreateBackend", "BackendOptions", "Capabilities", "CapabilityMask",
        "Caps", "PresentWaitAvailable", "PresentWaitMeasuresDisplay", "DisplayTimingAvailable", "CreateBuffer", "BufferUsage",
        "Heap", "Ring", "Map", "Commit", "BindingSet", "Recorder", "BeginRecording", "SetVertexBuffers", "SetIndexBuffer",
        "Bind", "Draw", "DrawIndexed", "Dispatch", "CopyBuffer", "BeginPass", "EndPass", "PassDesc", "GraphicsPipelineDesc",
        "ComputePipelineDesc", "FixedFunctionState", "Stage", "Wait", "TimelineValue", "Submit", "WaitIdle", "ReadBuffer",
        "SetOwnerRuleHandler", "OwnerRule"]
TARGETS = ["Format", "FormatSupport", "QueryFormat", "ChooseFormat", "Image", "ImageDesc", "SamplerDesc", "Sampler",
           "CreateImage", "CreateSampler", "CopyImage", "Blit", "ClearImage", "CopyBufferToImage", "CopyImageToBuffer",
           "BeginTargetPass", "TargetPassDesc", "FeedbackCopy", "PrepareForPresent", "BindingArena", "HandOver", "shadow_source"]
SYNC = ["Retire", "LoadPipelineCache", "SavePipelineCache"]
READBACK = ["PicaFormat", "ReadbackToGuest", "ReadImage", "FrameGpuTimeNs", "MarkFrame"]
STATE = ["SetBlendConstants", "SetStencilReference", "SetStencilMasks"]


@pytest.mark.parametrize("group", [CORE, TARGETS, SYNC, READBACK, STATE], ids=["core", "targets", "sync", "readback", "state"])
def test_rhi_h_declares_every_operation(group):
    present = set(tokens(RHI))
    missing = [name for name in group if name not in present]
    assert not missing, f"rhi.h lacks: {missing}"


def test_the_capability_bits_are_the_ones_of_9_4():
    text = RHI.read_text()
    for bit in ["d24s8", "interlock", "index_uint8", "custom_border_color", "triangle_fans", "logic_op", "dynamic_rendering",
                "pipeline_library", "eds3_blend", "bc7", "timeline", "present_wait", "direct_upload", "geometry_shader"]:
        assert re.search(rf"\bbool {bit} = false;", text), f"Capabilities lacks the bit {bit}"
    for value in ["stride_alignment", "apple", "primitive_restart_always_on", "portability_subset", "driver_id",
                  "driver_name", "device_name", "api_version", "timestamp_period", "max_texel_buffer_elements"]:
        assert re.search(rf"\b{value}\b\s*(=|;)", text), f"Capabilities lacks the value {value}"
    assert "kCapCount = 14" in text


def test_vulkan_access_h_is_the_vulkan_side():
    text = ACCESS.read_text()
    found = includes(ACCESS)
    assert ("<", "volk.h") in found
    assert ('"', "gpu/backend/rhi.h") in found
    assert not [i for i in found if "plume" in i[1]], "vulkan_access.h includes a plume header"
    assert not [t for t in tokens(ACCESS) if "plume" in t.lower()], "vulkan_access.h names plume"
    for name in ["Handles", "CommandBuffer", "SurfaceFrom", "QueueLock", "NativeImage", "WrapExternalImage"]:
        assert re.search(rf"\b{name}\b", text), f"vulkan_access.h lacks {name}"
    for field in ["instance", "physical_device", "device", "queue_family_index", "queue"]:
        assert re.search(rf"\b{field} = ", text), f"VulkanHandles lacks {field}"
