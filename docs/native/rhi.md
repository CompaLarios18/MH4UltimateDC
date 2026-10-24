# The RHI: plume's Vulkan backend, vendored (N2-25)

The renderer's Vulkan layer (decision D4, option 2b of [`../research/rhi-spike.md`](../research/rhi-spike.md)) stands on
[plume](https://github.com/renderbag/plume)'s Vulkan backend: a pristine copy, trimmed to the Vulkan files, plus numbered
patches of ours, under an operation-level wrapper of ours (N2-26 to N2-28; the wrapper's interface is documented in
`native/src/gpu/backend/rhi.h` once N2-26 has written it). This page is the manual of the vendored part. N2-29, N2-27 and
N2-28 add their sections. Until N2-25 is merged it is the one owner of `native/third_party/rhi/` and of the patch queue;
N2-29 owns them after it, and a slice that finds it needs a plume change stops with `needs-decision` instead of editing.

**Agents never contact the plume project.** No issue, pull request, comment or message of any kind: its `CONTRIBUTING.md`
forbids AI-assisted contributions and AI-assisted communication with its maintainers (`rhi-spike.md` 6.6). Reading and
vendoring its MIT code is fine. Every change to plume's code is a numbered patch file of ours, and an upstream fix is
cherry-picked by us as a new numbered patch.

## What is vendored

All under `native/third_party/rhi/` ([`README.md`](../../native/third_party/rhi/README.md) there lists the commits and blob ids):

| Path | What |
|---|---|
| `plume/pristine/` | plume at **`0ce184b19270e49236afbb74052a6b267494b86b`**, unmodified, at upstream paths: `plume_vulkan.cpp`, `plume_vulkan.h`, the three shared headers (`plume_render_interface.h`, `_builders.h`, `_types.h`) and `LICENSE` (MIT, "Copyright (c) 2024 renderbag and contributors"). The trim is by file: the D3D12 and Metal sources, `plume_apple.*`, `metal-cpp`, D3D12MA, the examples and plume's `contrib/Vulkan-Headers` are not vendored. |
| `plume/patches/` | `NN-<name>.diff`, 01 to 11: [the list with one line each](../../native/third_party/rhi/plume/README.md#the-patches). 01 to 07 are the files of `docs/research/rhi-spike-evidence/patches/` byte for byte; 08 to 11 were written in N2-25. |
| `plume/patched/` | `pristine/` with every patch applied: what the build compiles. Committed, so a build needs no `git` or `patch`. Includes `mh4n_patch_list.h` (the pinned commit and the patch names). |
| `volk/`, `vma/` | volk (`be3dbd49bf77052665e96b6c7484af855e7e5f67`) and VulkanMemoryAllocator (`29b35ea4232688c0f42cdff0c10848290760a417`), the commits plume's `contrib/` submodules name. |

One volk, one VMA, one copy of the Vulkan headers: `patched/plume_vulkan.cpp` is the only translation unit that defines
`VOLK_IMPLEMENTATION` and `VMA_IMPLEMENTATION` (a pytest checks it); the headers are `native/third_party/vulkan/`
(v1.3.296; N2-24's copy with `vulkan.h` and `vulkan_beta.h` added), not plume's v1.4.335. plume, volk and VMA compile
against v1.3.296 unchanged. No `VK_USE_PLATFORM_*` is defined anywhere.

The build is `native/src/gpu/backend/plume/` (module `mh4n_gpu_backend_plume`): the static library `mh4n_rhi_plume` (the
patched Vulkan backend with volk and VMA, warnings off, C++17 for plume's files, `PLUME_EXTERNAL_SURFACE_ONLY` defined)
and `vendor_info.cpp`, whose `mh4n::gpu::backend::plume::VendorInfo()` returns the commit and patch names for logs. Inside
`namespace mh4n::gpu::backend::plume`, `plume::X` names that namespace, so spell plume's own types `::plume::X`. Nothing runs
at static initialisation and nothing loads Vulkan until an instance is created (`volkInitialize`, in
`CreateVulkanInterface`): a binary starts on a machine with no Vulkan driver, and `CreateVulkanInterface` then returns
`nullptr` with the text of the failure in `VulkanInterfaceOptions::error`.

## The patch queue

```
python3 tools/native/rhi_patches.py apply    # pristine/ + patches/ -> patched/ (and patched/mh4n_patch_list.h)
python3 tools/native/rhi_patches.py check    # the same into a temporary folder, compared byte for byte; exit 1 names the first differing file
python3 tools/native/rhi_patches.py blobs    # the git blob id of every pristine, volk and VMA file against the README tables
```

The applier is ours (Python standard library): a hunk is applied only at the line it names, and only when every context and
removed line matches exactly; otherwise `apply` stops (exit 2) naming the patch. **Add a patch** as a new numbered file after
the last one; never edit an earlier patch or `patched/` by hand:

1. copy `patched/`, edit the copy, write the unified diff on the pristine paths (`a/<file>`, `b/<file>`), for example with
   `git diff` in a scratch repository whose last commit is `patched/`;
2. save it as `patches/NN-<name>.diff` with the next number and list it in `plume/README.md`;
3. `rhi_patches.py apply`, then `check` and `blobs`; rebuild and run the RHI tests under the layer (below).

Patches 01 to 07 were made one on top of another (01 → 02 → 04, 03 → 05 → 06 → 07): they apply in number order only. A
Windows or macOS build failure is fixed by a new numbered patch, never by editing `patched/`. `.gitattributes` marks
`native/third_party/rhi/**` and the evidence's `patches/*.diff` as `-text`, so a Windows checkout with `core.autocrlf=true`
cannot rewrite files that are compared byte for byte (sha256, blob ids).

## What patches 08 to 11 add to plume's API

(Vulkan-only types are declared in `plume_vulkan.h`, so the shared headers gain no Vulkan type.)

- **`VulkanInterfaceOptions`** and `CreateVulkanInterface(options)`: `extraInstanceExtensions` (required; a missing one fails),
  `headlessSurface` (enable `VK_KHR_surface` and `VK_EXT_headless_surface` when the loader offers them), `debugUtils` (enable
  `VK_EXT_debug_utils` when offered), `validationLayer` (enable `VK_LAYER_KHRONOS_validation` when installed), `error` (the
  text of a failure; it is still printed to stderr). Nothing is required of the instance beyond the loader: with the
  defaults it enables no surface extension and no `VK_EXT_debug_utils`, and plume's debug-build habit of switching the layer
  and the debug-utils extension on is gone (the caller asks). **Decision taken here:** `headlessSurface` is a flag that
  defaults to off, because the spec of N2-25 asks both for "enables `VK_EXT_headless_surface` when the loader offers it" and
  for a test that an instance with no extra extension enables no surface extension; N2-26's `CreateInstance` turns the flag on.
- **`VulkanInterface::createDeviceEx(name, VulkanDeviceExtras)`**: `surface` (a `VkSurfaceKHR`; `VK_KHR_swapchain` is required
  and enabled only with it, and the direct queue family is chosen among those that can present to it, failing when none can),
  `physicalDeviceIndex` (the index in `vkEnumeratePhysicalDevices`' order on plume's instance; -1 keeps plume's choice by type or
  exact name), `coreFeatures` (the core features to enable, ANDed with what the device offers; unset keeps plume's "every
  queried feature"; `robustBufferAccess` is turned on whatever it lists whenever robustness2's `robustBufferAccess2` is enabled,
  because the specification requires it, VUID `...Robustness2FeaturesEXT-robustBufferAccess2-04000`; case `device/core_features`), `optionalExtensions` (the optional extensions to take when offered; unset: the default list),
  `timelineSemaphore`, `error`. `VulkanInterface::enabledExtensionNames`, `VulkanDevice::enabledExtensionNames` and
  `VulkanDevice::enabledCoreFeatures` say what was enabled.
- **Things to know when calling it.** A set `optionalExtensions` *replaces* the default list: a caller that sets it must name the kept extensions (descriptor indexing, robustness2, ...) itself; only `VK_KHR_portability_subset` is always added. A `physicalDeviceIndex` below -1 is treated as -1. Patch 08 sets volk's process-global `vkSetDebugUtilsObjectNameEXT` to null when an instance did not enable `VK_EXT_debug_utils`, so a second instance in the same process changes it for the first (the first loses object names; nothing else).
- **The default optional list** is plume's 15 names minus acceleration structure, ray-tracing pipeline, pipeline library and
  deferred host operations, minus `VK_KHR_present_id`, `VK_KHR_present_wait` and `VK_GOOGLE_display_timing` unless there is a
  surface, plus fragment shader interlock (patch 02), `VK_EXT_index_type_uint8` and `VK_EXT_custom_border_color` (patch 09,
  with their feature structs and the capability bits `indexTypeUint8` and `customBorderColor`). Kept, taken when offered as at
  `0ce184b`: descriptor indexing, scalar block layout, robustness2, buffer device address, sampler mirror clamp, load/store op
  none, sample locations and `VK_KHR_portability_subset` (always when offered, as the specification requires). `unverified`:
  whether plume's own code needs each kept one; turning one off is a later patch, after a read of where plume uses it.
- **Window surfaces are compiled out** (`PLUME_EXTERNAL_SURFACE_ONLY`, defined by our build): `RenderWindow` is an empty
  struct, no `Windows.h`, Xlib or Cocoa header is included, and `createSwapChain` returns `nullptr`. N2b-06 builds its
  swapchain on the backend's handles. `RenderTextureLayout::PRESENT` already exists in plume (`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`,
  `toImageLayout`), so no patch adds it.
- **Dynamic stencil state** (10): the pipeline flag `dynamicStencilEnabled` adds the reference, compare-mask and write-mask
  dynamic states; `RenderCommandList::setStencilReference`, `setStencilCompareMask`, `setStencilWriteMask` set them for both faces.
- **Formats** (11): `D24_UNORM_S8_UINT`, `R5G6B5`/`B5G6R5`, `R5G5B5A1`/`B5G5R5A1`/`A1R5G5B5`, `R4G4B4A4`/`B4G4R4A4`;
  `RenderDevice::getFormatSupport(format)` (attachment, sampled, storage, blit source and destination, optimal tiling); a
  buffer copy of a depth-stencil image is two regions, one aspect each: the depth plane at the footprint's offset (4 bytes per
  texel, D24 in the low 24 bits), then the stencil plane (1 byte per texel) right after it.

## The tests

- `native/tests/rhi_plume_test.cpp` (ctest name `rhi_plume_test`): `device/no_surface`, `device/headless_surface`,
  `device/chosen`, `sync/A1_render_then_sample`, `sync/C3_two_waits_each_with_a_stage` (the two stage masks are read from the
  `vkQueueSubmit` call through volk's function pointer, as `rhi-spike.md` 6.9 traced them), `descriptors/buffer_offset`, `device/core_features`, and
  the control `control/C5`. On lavapipe (one queue family, one physical device) `device/headless_surface` cannot fail on the present-capable family filter and `device/chosen` cannot tell the index from plume's own choice: those halves are for a PC or a second device (the reviewer showed `device/chosen` with two lavapipe copies through `VK_ICD_FILENAMES`). A case skips with a message when there is no Vulkan device and fails instead under
  `MH4N_REQUIRE_VULKAN=1`. Controls are skipped by default; run one with `--test-case=control/C5 --no-skip`.
- Pure-Python tests: `tests/test_native_rhi_patches.py` (the applier on synthetic diffs; `check` and `blobs` on the tree; the
  evidence's sha256 for patches 01 to 07; one volk and one VMA translation unit), `tests/test_native_vk_layer_run.py`,
  `tests/test_native_spirv_inc.py` (skips without `glslangValidator`), `tests/test_native_setup_env.py`.

## Shaders: `spirv_inc.py`

`python3 tools/native/spirv_inc.py IN_DIR OUT.inc` compiles the shaders of `IN_DIR` with
`glslangValidator -V --target-env vulkan1.2` into `[[maybe_unused]] static const uint32_t name[]` arrays. `IN_DIR/spirv_inc.txt`
lists one array per line, `<array name> <file> [glslang options such as -DMODE=1]` (a file may be listed several times with
different options); without it every shader file gives one array named after the file (`tri.vert` becomes `tri_vert`). The
`.inc` files are committed: glslang comes with N2-06, and neither the build nor CI's native jobs need `glslangValidator`.
`native/tests/shaders/rhi_plume/` and `native/tests/rhi_plume_shaders.inc` hold the evidence's shaders (the arrays of its
`build.sh`) plus `ubo_offset.comp`; N2-29 includes the `.inc` and never edits it. Regenerate with
`python3 tools/native/spirv_inc.py native/tests/shaders/rhi_plume native/tests/rhi_plume_shaders.inc` (apt `glslang-tools`).

## The validation-layer harness

lavapipe accepts every invalid call (`rhi-spike.md` 6.9), so every RHI test also runs under the Khronos validation layer
with synchronization validation:

```
python3 tools/native/vk_layer_run.py --build build/native -R '^rhi_plume_test$'
python3 tools/native/vk_layer_run.py --build build/native -R '^rhi_plume_test$' --test-case control/C5   # must report SYNC-HAZARD-READ-AFTER-WRITE and exit 1
```

It runs `ctest --test-dir DIR --no-tests=error -R REGEX --output-on-failure -V` (`-V` because ctest hides the output of a
passing test and the layer's messages must be seen) with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`,
`VK_LAYER_SETTINGS_PATH=tools/native/vk_layer/` (errors and warnings only, synchronization validation, three duplicates per
message; the evidence's settings without best practices and without `perf`), `VK_LAYER_ENABLES` synchronization validation and
`MH4N_REQUIRE_VULKAN=1`. plume registers no debug messenger, so the layer prints to stdout; the harness fails on any line that
names a `VUID-`, `SYNC-HAZARD-` or `UNASSIGNED-` ID or is a `Validation Error` or `Validation Warning`, and on a `BestPractices-`
ID (the settings were not applied), prints each ID with its count, and **fails, never skips, when the layer is not installed**:
`apt install vulkan-validationlayers` (1.3.275 on the cloud image; `tools/native/setup_env.sh` installs it). In `native.yml`
the `native` job installs the package, its `Test` step sets `MH4N_REQUIRE_VULKAN: "1"` (every Vulkan test of the suite fails
instead of skipping if lavapipe disappears), and a step runs the harness over
`'^(rhi_.*|gpu_backend_.*|gpu_queue_test|present_test|swapchain_test)$'` (names that do not exist yet match nothing).
Silence is weak evidence for hazards between draws inside one command buffer: the layer misses storage-image hazards there
(`rhi-spike.md` 6.9, B6).

## Binding a buffer at an offset (read for N2-27)

**Question** (`rhi-spike.md` X5 only inferred the answer): can plume's public API bind a buffer to a descriptor at a non-zero
byte offset, and with what range? Read in the pristine copy, `plume_vulkan.cpp` and the shared headers at `0ce184b`:

**Yes, through the structured view of `setBuffer`; no dynamic offsets.**

- `RenderDescriptorSet::setBuffer(descriptorIndex, buffer, bufferSize = 0, bufferStructuredView = nullptr, bufferFormattedView = nullptr)`
  (`plume_render_interface.h:82`). `RenderBufferStructuredView{structureByteStride, firstElement}` (`plume_render_interface_types.h:835-845`).
- `VulkanDescriptorSet::setBuffer` (`plume_vulkan.cpp:1947-1975`) fills one `VkDescriptorBufferInfo`:
  - `buffer` = the buffer's `VkBuffer` (`:1955`);
  - **`range`** = `bufferSize` if it is above zero, otherwise **the whole buffer's size** `desc.size` (`:1956`). It is not "size minus offset": with an offset and no `bufferSize` the range runs past the end of the buffer;
  - **`offset`**: with a structured view, `firstElement * structureByteStride` (`:1968`; both `uint32_t`, so the product is a 32-bit multiplication that wraps at 4 GiB); with no view, 0 (`:1971`); with a formatted view the `VkBufferView` is passed and the buffer info is not the binding (`:1958-1963`), and the view itself is created over the whole buffer (`:948-949`, `offset = 0`, `range = desc.size`), so **a texel buffer cannot be bound at an offset** through this API;
  - then `setDescriptor` (`:2021-2040`) writes one descriptor with `vkUpdateDescriptorSets` (`:2039`) using the layout binding's descriptor type, which is `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` for `CONSTANT_BUFFER` (`:508-509`) and `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` for the structured and byte-address ranges (`:520-527`).
- The check in the code is `assert(structureByteStride > 0)` (`:1966`) and nothing else: the view is not tied to the descriptor type, so a **uniform buffer binding at an offset works through a structured view with stride 1 (`firstElement` = the byte offset, up to 4 GiB - 1) and an explicit `bufferSize` = the block size.** The layout bindings are plain, not `_DYNAMIC` (`:1169-1175`) and `setDescriptorSet` binds with no dynamic offsets (`vkCmdBindDescriptorSets(..., 1, &set, 0, nullptr)`, `:3528`). So a per-draw offset means rewriting the descriptor (`setBuffer` again) on a set no submission in flight uses, or a set per draw (X5b: each `VulkanDescriptorSet` owns its layout, pool and set, `:1885-1944`).
- **Shown at run time, not only read:** `native/tests/rhi_plume_test.cpp`, case `descriptors/buffer_offset`, binds a `CONSTANT` buffer at `2 * alignment` and then at `5 * alignment` (alignment = `minUniformBufferOffsetAlignment` of the device, at least 16), range 16, reads the block in a compute shader and gets the right values each time, with no validation-layer message (lavapipe, layer 1.3.275).

**What the code does not say, and the run did not settle:**
- plume does **not** check the alignment: `minUniformBufferOffsetAlignment` and `minStorageBufferOffsetAlignment` are the caller's duty (the layer reports `VUID-VkWriteDescriptorSet-descriptorType-00327` and `-00328`), and so is `range <= maxUniformBufferRange` and `offset + range <= buffer size`. The test ran with lavapipe's limits only; the RX 6700 XT's and the M2's limits (and MoltenVK's behaviour) are not measured.
- Whether a rewrite of a set's descriptor is cheap enough per draw on a real driver (`vkUpdateDescriptorSets` per draw) is unmeasured; Azahar uses dynamic offsets over one ring instead.
- How the Direct3D 12 and Metal backends read the same arguments was not read (not vendored).

**Smallest patch, if a cleaner call is wanted (not written; a later patch is a new numbered file added by a WP the orchestrator names):** a `setBufferRange(descriptorIndex, buffer, offset, range)` on `RenderDescriptorSet` with a default that does nothing, implemented in `VulkanDescriptorSet` by the same eight lines of `setBuffer` with the two values passed in (about 12 lines, no behaviour change for existing callers). A true dynamic offset needs a `_DYNAMIC` descriptor type in `toVk` and the layout (`:508`, `:1175`), an offset array argument through `setGraphicsDescriptorSet`/`setComputeDescriptorSet` into `vkCmdBindDescriptorSets` (`:3528`), and the same on the other backends: about 80 to 120 lines (`rhi-spike.md` X5, `inference`). Neither is needed for N2-27 to start: the structured-view form above works today.

## Not checked on this machine

`readelf -d` for `libX11` is meaningful on `build/native/bin/rhi_plume_test` (no `NEEDED` entry besides libstdc++, libm, libgcc_s, libc and the loader's), not on `mh4u_native_runner`, which does not link plume yet.

The Windows llvm-mingw build, the macOS build and the three CI jobs are `pending (ci)`: the macOS branch of `plume_vulkan.cpp` (`VK_KHR_portability_enumeration`, the portability-subset features, `triangleFan` forced false) and the Windows branch (`NOMINMAX`) were not compiled here.
