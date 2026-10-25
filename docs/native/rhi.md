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
| `plume/patches/` | `NN-<name>.diff`, 01 to 12: [the list with one line each](../../native/third_party/rhi/plume/README.md#the-patches). 01 to 07 are the files of `docs/research/rhi-spike-evidence/patches/` byte for byte; 08 to 11 were written in N2-25 and 12 in N2-29. |
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

## What patches 08 to 12 add to plume's API

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
- **Custom border colour** (12, N2-29): `RenderBorderColor::CUSTOM` and `RenderSamplerDesc::customBorderColor` (red, green, blue,
  alpha as floats, read only with `CUSTOM`). `VulkanSampler` then creates the sampler with `VK_BORDER_COLOR_FLOAT_CUSTOM_EXT` and a
  `VkSamplerCustomBorderColorCreateInfoEXT` whose format is `VK_FORMAT_UNDEFINED` (one sampler works with every image format),
  and **only when `capabilities.customBorderColor` is set**; on a device without it `createSampler` prints one line to stderr
  (as plume does for a failed `vkCreateSampler`) and returns a sampler object whose `VulkanSampler::vk` is `VK_NULL_HANDLE`: the
  caller (N2-27's `CreateSampler`) asks the capability first and reports the failure itself. The capability bit, which patch 09
  sets from `customBorderColors`, now also needs `customBorderColorWithoutFormat` (both are on for lavapipe, measured; a driver
  with only the first is treated as having none, because a sampler with a format would work with images of that format only). A set `VulkanDeviceExtras::optionalExtensions` that does not name `VK_EXT_custom_border_color` leaves the
  bit off (case `patch12/custom_border_colour_refused_without_the_extension`). `maxCustomBorderColorSamplers` (at least 32,768 on
  lavapipe) is not checked: the caller makes few of them.

## The tests

- `native/tests/rhi_plume_patterns_test.cpp` (ctest name `rhi_plume_patterns_test`, N2-29): [the patterns](#the-validation-patterns).
- `native/tests/rhi_plume_test.cpp` (ctest name `rhi_plume_test`): `device/no_surface`, `device/headless_surface`,
  `device/chosen`, `sync/A1_render_then_sample`, `sync/C3_two_waits_each_with_a_stage` (the two stage masks are read from the
  `vkQueueSubmit` call through volk's function pointer, as `rhi-spike.md` 6.9 traced them), `descriptors/buffer_offset`, `device/core_features`, and
  the control `control/C5`. On lavapipe (one queue family, one physical device) `device/headless_surface` cannot fail on the present-capable family filter and `device/chosen` cannot tell the index from plume's own choice: those halves are for a PC or a second device (`device/chosen` can be shown on one machine with two copies of lavapipe's `libvulkan_lvp.so`, each with its own ICD manifest, listed as `VK_ICD_FILENAMES=a.json:b.json`). A case skips with a message when there is no Vulkan device and fails instead under
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

## The validation patterns

`native/tests/rhi_plume_patterns_test.cpp` ports the patterns of `rhi-spike-evidence/test/main.cpp` that
`rhi_plume_test.cpp` does not hold (A1, C3 and C5 are there) to the patched copy's public API, one doctest case each, with the
result `results-summary.txt` gives for the **patched** build; a pattern whose result differed would be reported with its log, not
adjusted. The shaders are `rhi_plume_shaders.inc` (N2-25's, included); the one shader N2-29 added, `border.frag`, is an array in
the test file with its GLSL in the comment above it (N2-29's allowlist has no shader folder). Each case prints
`rhi_plume_patterns_test: <case>: device: <name>` (llvmpipe in the cloud and in Linux CI). All of them also pass under the layer
(`vk_layer_run.py`, above) with no message, which is the check lavapipe cannot do for itself.

| Case | What it does | Result it pins |
|---|---|---|
| `A2_render_then_sample_in_two_lists` | render X and move it to `SHADER_READ` in list 1, sample it into Y in list 2, two submits on one queue | X is the triangle image, Y equals X |
| `B1`, `B2`, `B3`, `B4`, `B5` (`B<n>_shadow_*`) | zero-attachment pass (patch 01) writing an `R32_UINT` view of an `RGBA8` image (`MUTABLE_FORMAT`) with three overlapping full-screen draws that each add 1: load/store, with interlock (patch 02), compare-and-swap, both, and with a `GENERAL` barrier between the draws | every texel is 3 (0 of 256 differ). B2 and B4 need `fragmentShaderInterlock`: a device without it skips them with a message, except lavapipe under `MH4N_REQUIRE_VULKAN=1`, where it fails |
| `B8_colour_attachment_in_general_layout` | draw into a colour attachment that stays in `GENERAL` (patch 04) | the triangle image |
| `C1_binary_semaphore_graphics_barrier`, `C2_binary_semaphore_none_barrier_per_wait_stage` | producer on queue 1 ends with a `GRAPHICS` / `NONE` barrier and signals a binary semaphore; the consumer on queue 2 waits for it with the stage `GRAPHICS` (patch 03) | the triangle image; unpatched, C2 gave `SYNC-HAZARD-READ-AFTER-WRITE` |
| `C4_timeline_semaphore_with_stage` | the same with a timeline semaphore (patch 05), producer ends `NONE`, the wait at `GRAPHICS` | the triangle image; the semaphore's counter is 1 |
| `D1_draw_on_one_thread_sample_on_another` | thread 1 records and submits the draw, thread 2 waits for its fence, then records the sampling draw | the triangle image; no ThreadSanitizer report |
| `D3_owner_rule_two_threads_one_texture` | two threads record 20 barriers each on one texture, 2,000 rounds; the harness's owner rule: one mutex around recording, thread 2 records after thread 1, lists submitted 1, 2 | 0 record-time overwrites, 0 broken layout chains; no ThreadSanitizer report |
| `D3b_owner_rule_mutex_and_ticket_order` | D3 with each half of the rule load-bearing (below), 19 barriers per list | the same: 0 and 0 |
| `E_pipeline_cache_and_render_pass_cache` | 42 graphics pipelines (2 in the scene, 40 more identical ones) and 2 framebuffers, counted at `vkCreateRenderPass` and `vkCreateGraphicsPipelines` through volk's function pointers; the blob of `savePipelineCache` loaded by a second device | 3 render passes (unpatched: 44), 42 pipelines, none created without the device's pipeline cache, the 40 identical ones share one render pass, the second device accepts the blob (lavapipe: 32 bytes of header) |
| `F_blend_constant_half` | `BLEND_FACTOR` blend, source (1,1,1,1), `setBlendConstants(0.5)` (patch 07) | 128 in every channel (the pipeline's own constant gives 0) |
| `patch09/optional_extension_lists` | the default list has no ray-tracing, acceleration-structure or presentation extension; an empty `optionalExtensions` enables only `VK_KHR_portability_subset` (when offered) and sets none of the optional capabilities; naming `VK_EXT_custom_border_color` enables just that one | as described |
| `patch10/dynamic_stencil_state_d24s8`, `..._d32s8` | a stencil target cleared to 5; four 8x8 quadrants, each with its own reference, compare mask and write mask set on the command list (patch 10): reference 5 (white), reference 6 (black), reference 0x35 under compare mask 0x0F (white: only the dynamic compare mask makes it equal 5), and a draw with no colour that REPLACEs the stencil with reference 0xAB under write mask 0x0F followed by a test against 0x0B (white: only the dynamic write mask leaves 0x0B and not 0xAB). A list whose setters did nothing leaves the pipeline's own reference 0 and fails quadrant 0 | white, black, white, white |
| `patch11/depth_stencil_copy_as_two_regions` | a D24S8 image (and D32S8, the PC's format) cleared to depth 1.0 / stencil 0xAB, then 0.0 / 0x3C, and copied to a buffer: depth plane at the offset, stencil plane right after it | 0xFFFFFF (D24) or 0x3F800000 (D32) and 0xAB, then 0 and 0x3C |
| `patch11/16_bit_colour_formats_where_the_query_says_attachment` | each 16-bit colour format whose `getFormatSupport(...).attachment` is set is created as a target, cleared red / alpha 1, blue / alpha 0 and green / alpha 1, and copied out | the word has each component in the bits Vulkan's `*_PACK16` layout names; lavapipe: `R4G4B4A4` is not an attachment and is skipped, the other six are |
| `patch12/custom_border_colour_out_of_range`, `patch12/custom_border_colour_refused_without_the_extension` | a green 16x16 target sampled with border addressing over `uv * 3 - 1` (`border.frag`): the pixels out of range return the sampler's colour (0.25, 0.5, 0.75, 0.5) = (64, 128, 191, 128), the middle ones return green; an `OPAQUE_BLACK` sampler is the reference; on a device whose `optionalExtensions` leaves the extension out, the custom sampler has a null handle and an ordinary one works | as described |
| `vendor/border_colour_patch_is_listed` | the build's patch list names a `NN-custom-border-colour.diff` with NN at least 12 and equal to its place in the list | the number is read, not written in the test |

**D3, and what a missing mutex shows.** In the harness's form (D3) thread 2 starts recording only after thread 1 has finished, so
the mutex and the wait each serialise the threads alone: removing the mutex, or the wait, from D3 shows nothing (measured:
0 overwrites, 0 broken chains either way). D3b is the same rule with both halves needed: either thread may go first (thread 2 first
in one round of four, thread 1 in one, together in two), the lists take a ticket under the mutex, and they are submitted in ticket
order. Measured on lavapipe, 2,000 rounds: the rule as written gives 0 overwrites and 0 broken chains of 2,000; **without the
mutex** 3,700 to 6,600 record-time overwrites per thread and about 1,400 broken chains (two runs); **without the ticket order**
(lists submitted 1, 2 whatever the recording order) 0 overwrites and about 1,000 broken chains. "Overwrite": after each of its own barriers a thread reads
the texture's layout field (`VulkanTexture::textureLayout`) and it is not the layout it asked for, because the other thread's
record-time store landed in between. "Broken chain": the (old, new) layouts the recorded `vkCmdPipelineBarrier` calls carry, taken
through volk's function pointer, must in submission order form one chain, each old layout the one before it left. Both counts need
no validation layer, so they fail on a driver that accepts everything; the layer's `oldLayout-01197` and `InvalidImageLayout` (control
D2) are a second witness. N2-27's owner-rule checks (`MH4N_RHI_CHECKS`) are the product code for this rule; these cases only show that
the rule is sufficient and what its absence costs.

**The controls** are cases named `control/<pattern>`, skipped unless named (`--test-case=control/B7 --no-skip`, which
`vk_layer_run.py --test-case` does):

| Control | Must produce (under the layer, `vk_layer_run.py --test-case <name>`) |
|---|---|
| `control/B7` | `SYNC-HAZARD-WRITE-AFTER-WRITE` (draws 4 and 5 in command lists of their own, no barrier); exit 1 |
| `control/D4` | `UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout` (lists recorded 1, 2, submitted 2, 1); exit 1 |
| `control/D2` | `VUID-VkImageMemoryBarrier-oldLayout-01197` and `UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout`, in counts that vary per run (the layer's settings print three duplicates each), the cases' own counts of overwrites and broken chains, and a ThreadSanitizer report on the layout store; exit 1. **Not run by the `native-tsan` job** |
| `control/B6` | nothing: the example of a hazard the layer misses (storage-image writes between draws, a new pass for each, no barrier, in one command buffer: no message, `rhi-spike.md` 6.9). Nothing asserts on it; B1, B3 and B5 passing under the layer say nothing about their ordering |

Lavapipe has one queue family with one queue: the legs of the patterns that need two queues (the cross-queue consequence of the
stage mask, C2 on a second queue; an owner-rule hand-over across queues) are the PC's, and `unverified` here (C1 to C4 use two
`RenderCommandQueue` objects, which plume maps onto the family's queues, of which lavapipe has one).

## The ThreadSanitizer job

`native-tsan` (`.github/workflows/native.yml`) builds the multi-thread RHI tests with `-fsanitize=thread` and runs them with
no report allowed (`rhi-spike.md` 9.4 item 16; 6.9 ran D2 and D3 under it with g++ 13).

- **Compiler and runtime.** clang 18.1.3 (on Ubuntu 24.04 `clang` is clang 18; the workflow job names `clang-18` and `clang++-18` because
  the runner images carry several clangs, so that the compiler and the runtime package are the same major version; `setup_env.sh` and
  the cloud machine use plain `clang`, which is 18 there) with the apt package **`libclang-rt-18-dev`**
  (`1:18.1.3-1ubuntu1`), which holds `libclang_rt.tsan-x86_64.a`; the cloud image had clang but not this package, so
  `clang -fsanitize=thread` failed to link until it was installed. `tools/native/setup_env.sh` installs it and prints whether the
  sanitizer links; `docs/native/ENV.md` lists it; the workflow job installs it. The same package name carries the compiler's
  version: another clang needs `libclang-rt-<N>-dev`.
- **Tree.** `build/tsan`, separate from `build/native` (the sanitizer changes the ABI of every object):
  ```
  cmake -S native -B build/tsan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DMH4N_WERROR=ON -DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_CXX_FLAGS=-fsanitize=thread \
        -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread
  RE='^(rhi_.*|gpu_backend_.*|gpu_queue_test|present_test|swapchain_test)$'
  cmake --build build/tsan -j2 --target $(ctest --test-dir build/tsan -N -R "$RE" | sed -n 's/^ *Test *#[0-9]*: //p')
  MH4N_REQUIRE_VULKAN=1 TSAN_OPTIONS=halt_on_error=1:suppressions=$PWD/tools/native/tsan.supp \
        ctest --test-dir build/tsan --no-tests=error -R "$RE" --output-on-failure -V
  ```
  The regex is the one of the validation-layer step (names that do not exist yet match nothing). It runs every case of those test
  binaries, `rhi_plume_patterns_test`'s D1, D3 and D3b among them; the control D2 is skipped by default and must not be run there
  (it races on purpose and would fail the job).
- **Suppressions** (`tools/native/tsan.supp`): `race:` and `mutex:` entries on the file names of the Vulkan loader
  (`libvulkan.so`) and lavapipe's driver library (`libvulkan_lvp.so`), for a driver build whose uninstrumented locking
  ThreadSanitizer cannot see. **They are needed when the machine is busy**: lavapipe's worker threads, whose synchronisation
  ThreadSanitizer cannot see, then give allocator and mutex reports (a data race whose first frame is `free`, `malloc` or
  `pthread_mutex_destroy` called from `libvulkan_lvp.so`). On a quiet machine (loader 1.3.275, Mesa 25.2.8) the two test binaries
  happened to pass with an empty file, which is timing-dependent. Reproduced: with an empty file the patterns test failed (exit 66) in 6 of 8 runs on a loaded machine
  and in 6 of 6 with four busy-loop processes; with the committed file it passed 8 of 8 and 6 of 6. They are not the `called_from_lib:` entries the spec of N2-29 had: `called_from_lib:` entries made
  ThreadSanitizer worse: with the driver entry it stopped the process with "library ... is unloaded" in one run (the loader
  `dlclose`s the driver whenever an instance is destroyed), and across runs and entries it also reported a data race with both
  accesses inside `libLLVM.so`, a lock-order inversion, and "unlock of an unlocked mutex" (with the loader entry, in
  `vkDestroyDevice`), in code that is clean without the entry. **What they cost:** a report with a frame in the loader or in
  lavapipe's driver is hidden, so the job does not catch an unlocked Vulkan call that races inside the driver (a queue used from
  two threads without its lock, say); the guards for that are the validation layer's synchronization validation, the owner-rule
  checks and `MH4N_RHI_CHECKS`. Never a frame of plume or of ours: a report in plume's code outside D2 stops the work package with
  `needs-decision` naming it. That the entries do not hide plume's own races is shown by the control:
  `build/tsan/bin/rhi_plume_patterns_test --test-case=control/D2 --no-skip` with the same options reports `data race` in
  `plume::VulkanCommandList::barriers` (`plume_vulkan.cpp`, the layout store, as in `rhi-spike.md` 6.9).
- **Seen on this machine** (the cloud container: clang 18.1.3, `libclang-rt-18-dev` 1:18.1.3-1ubuntu1, loader 1.3.275, lavapipe from
  Mesa 25.2.8, four cores): the `build/tsan` tree built; `ctest -R "$RE"` matched two tests, `rhi_plume_patterns_test` (23 cases, 3.4 s)
  and `rhi_plume_test`, and both passed with `halt_on_error=1` and the suppressions file, five runs of the first in a row, no report.
  The control D2 under the same options exits 66 with `data race` at `plume_vulkan.cpp:3007` in `VulkanCommandList::barriers`. The
  job itself on GitHub (`ubuntu-24.04`, its own Mesa and loader) has not run: *(CI)*, `pending`. If it reports something in
  lavapipe that this machine did not, the entries of `tsan.supp` are where it is added, naming the library and nothing of plume's.

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

## The backend core (N2-26)

The renderer-facing backend lives in `native/src/gpu/backend/`, namespace `mh4n::gpu::rhi`: `rhi.h` and `vulkan_access.h` at its root, then one module per
directory with one owner each: `plume/` (N2-25), `core/` (N2-26), `targets/` (N2-27), `sync/` and `readback/` (N2-28), `state/` (N2-13). **The two headers are
frozen:** after N2-26 they change only through the orchestrator (a later package that needs a change stops with `needs-decision`). The interface is
documented in `rhi.h`'s own comments, which also carry the traps for the owners of the later slices (the dynamic-state rule, the packed-format bit order,
resources that do not defer their own destruction, the limit of a failed `vkQueueSubmit`).

- **Read from the environment by the core:** `MH4N_RHI_MASK` (a comma list of capability bit names, or `all`; it only turns bits off, a masked feature is not
  enabled on the device so its fallback really runs, an unknown name fails creation with the list of valid names, and the result is logged once) and
  `MH4N_VULKAN_DEVICE` (a text matched, as a case-insensitive substring, against `<device name> (<driver name>)`; no match fails with the list of devices).
  Running a test as the PC's paths would: `MH4N_RHI_MASK=d24s8,interlock ctest --test-dir build/native -R '^gpu_backend_core_test$'`. The core's own test passes
  under each of the 13 bit names and under `all`.
- **`MH4N_RHI_CHECKS`** is a CMake option of `native/src/gpu/backend/core/CMakeLists.txt`, `ON` by default in every build type and `OFF` only for a shipping
  build; it is exported as the public compile definition `MH4N_RHI_CHECKS=1` or `0` of `mh4n_gpu_backend_core`, so the modules that link it put their owner-rule
  checks under the same switch. With it off the two owner-rule test cases skip themselves with a message.
- **Test helpers** (`native/tests/support/vulkan_device.h`): `RequireInstance`, `RequireBackend` and `MakeHeadlessSurface` skip the test with a message when there is
  no loader, device or headless-surface extension, and fail instead under `MH4N_REQUIRE_VULKAN=1`. When the validation layer is enabled (`VK_INSTANCE_LAYERS`, or
  `MH4N_RHI_VALIDATION=1` for a run without the harness) they install a `VK_EXT_debug_utils` messenger that fails the running test on any warning or error message.
  The loading test runs a child process of the test binary with `VK_DRIVER_FILES` pointing at an empty folder (the case `loading/child` is the child's body and
  skips by itself).
- **Without the `timeline` bit** (absent or masked) the core already runs the fence fallback: `core::SubmitRecorder` and `Backend::Impl::WaitFor`/`Completed`, a fence per
  submission and a CPU wait; N2-28 extends them (see its spec) and does not build a second mechanism.
- **Known limits, on purpose:** the constant-alpha blend factors are in the `BlendFactor` enum but refused with a message when blending is on, because the vendored
  backend maps only the constant-colour pair (a later numbered plume patch adds them); a fractional `depth_bias_constant` is refused (plume's is an integer); the device
  policy asks for a graphics, compute and transfer queue family because the vendored backend's own pick does (relaxing it is a plume patch). The core's
  `custom_border_color` bit is the device query ANDed with what plume really enabled (`getCapabilities().customBorderColor`, `core/device.cpp`), and with patch 12
  that capability also needs `customBorderColorWithoutFormat` (both are on for lavapipe): a true bit means a custom-border sampler can be created, and N2-27's
  `CreateSampler` checks the bit before it makes one.

## Targets, layouts and bindings (N2-27)

The images and render targets of the backend: `native/src/gpu/backend/targets/` (module `mh4n_gpu_backend_targets`, namespace
`mh4n::gpu::rhi`). **The interface is documented in `native/src/gpu/backend/rhi.h`** (the "Images, samplers and passes with
attachments" and "Targets, per-draw bindings and hand-over" sections, and the comments on `Format`, `Recorder` and `Backend`);
this section says how the module is built, what it checks and what it leaves to others. The module implements what `rhi.h`
declares for it (`QueryFormat`, `ChooseFormat`, `CreateImage`, `CreateSampler`, the image calls of `Recorder`, `HandOver`,
`BindingArena`, the image, sampler and texel-buffer setters of `BindingSet`) and `NativeImage` and `WrapExternalImage` of
`vulkan_access.h`. It keeps its state in the slots `core/impl.h` reserves (`Backend::Impl::targets`, `Recorder::Impl::targets`)
and never edits `core/`, `rhi.h`, `vulkan_access.h` or `native/third_party/rhi/`. Two headers of its own are not part of the
frozen interface: `targets/upload.h` (`UploadToImage`, `StagingUploadCount`: filling an image from host bytes) and
`targets/formats.h` (the chooser as a pure function over a support table, `ChooseFormatWith`, and `ToVkFormat`).

| File | What |
|---|---|
| `formats.cpp` | `QueryFormat` (patch 11's query), `ChooseFormat`, the `Format` to `VkFormat` table. Derived from Azahar's `CreateFormatTable` (provenance header in the file) |
| `images.cpp` | `CreateImage`, the views, `CreateSampler`, the framebuffer cache, `NativeImage`, `WrapExternalImage`, `HandOver` |
| `layouts.cpp` | the layout policy and the owner rule: `Transition`, `Acquire`, `Settle`, `Normalize` |
| `ops.cpp` | `CopyImage`, `Blit`, `ClearImage`, `CopyBufferToImage`, `CopyImageToBuffer`, `BeginTargetPass`, `FeedbackCopy`, `PrepareForPresent` |
| `bindings.cpp` | image, sampler and texel-buffer slots of a binding set, and the `BindingArena` |
| `upload.cpp` | `UploadToImage` |

### Formats

`QueryFormat(format)` is plume's `getFormatSupport` (patch 11: optimal-tiling features) for the format `core::ToPlumeFormat` gives
(so a PICA name answers for its default storage: `Rgb8` for `R8G8B8A8_UNORM`, `Rgb565` for `R5G6B5`, ...); the mask does not
change it (it is the device's truth; the chooser reads the mask). `ChooseFormat(format, use)`:

| Asked | Result | Conversion |
|---|---|---|
| `Rgb8` | `Rgba8Unorm`, always | `Rgb8ToRgba8` |
| `Rgb565`, `Rgb5A1`, `Rgba4` | the first order that is usable (a target: attachment and sampled; a texture: sampled), the PICA's bits first: `R5G6B5`, `B5G6R5`; `R5G5B5A1`, `B5G5R5A1`, `A1R5G5B5`; `R4G4B4A4`, `B4G4R4A4`. Else `Rgba8Unorm` | `None` for a 16-bit result (a swapped order is read from `format`: `rhi.h` has no conversion value for "swap the components"); `UnpackToRgba8` for the fallback |
| `D24S8` | `D24S8` when the `d24s8` bit is on, else `D32S8` | `None` / `DepthStencilTo32` |
| `Bc1Unorm`, `Bc3Unorm`, `Bc5Unorm`, `Bc7Unorm` | themselves when the `bc7` bit is on and the query says sampled, else `Rgba8Unorm` | `None` / `DecodeBc` ("decode on the CPU") |
| anything else (`D16Unorm` included) | as asked | `None` |

`ChooseFormatWith` is the same rule over any support table, so the MoltenVK case (only the R-first orders are attachments, 4 G9)
is tested on a synthetic table here. Limits: `FormatSupport` has no colour-attachment-blend bit (Azahar's rule uses it), so a
format that is an attachment but not blendable would be chosen (`unverified`; the chooser also does not require blit support, which
only `Blit` needs); `CreateImage` picks the use itself (depth formats `DepthTarget`, block-compressed `Sampled`, every other colour
format `ColorTarget`, since the surface cache will use one image as texture and as target).

### Images, views, samplers

`CreateImage`: 2D, `mip_levels` up to the full chain, 1 or 6 layers, `cube`; the format goes through `ChooseFormat` and
`Image::GetFormat()` says what the device stores. Plume gives every image sampled and both transfer usages; a colour image is a
render target, a depth image a depth target. **Only `shadow_source` gets storage usage and the mutable-format flag** (`metal.md` 3.2;
the module records `plume_flags` and `usage` per image, which the test reads). The format list `R8G8B8A8_UNORM` + `R32_UINT` that
`rhi-spike.md` 9.4 item 4 asks for cannot be expressed through plume (`VkImageFormatListCreateInfo` is not in its API): the flag alone is
used, which Vulkan allows for same-class formats; a patch would add the list (`unverified`: whether a driver compresses less
without it). A shadow source is a 32-bit unsigned-normalised colour image. An **attachment has one level and one layer** (plume's
framebuffer takes the image's whole-image view, which Vulkan wants single-level there); mipped images are textures.

Views are made on first use, by the module with `vkCreateImageView` (plume's own view type cannot make a stencil view): `Sampled`
is every level and layer (2D, 2D array or cube; for a depth-stencil image its depth aspect), `Depth` and `Stencil` one aspect,
`ShadowStorage` is `R32_UINT` of level 0. Every image is bound in `GENERAL`.

`CreateSampler`: filters, wrap modes (`MirrorClampToEdge` needs the device's extension, which the core enables where offered),
LOD range and bias, anisotropy above 1; **never a compare sampler**. A custom border colour (patch 12) is created only when
`Capabilities::custom_border_color` is true (the core's bit: the query, the mask, and what plume really enabled); otherwise
`CreateSampler` returns an error whose text says "custom border colour", and the caller emulates the colour in the shader with
clamp-to-edge. No `CUSTOM` sampler is ever asked of plume on a device without it.

### The layout policy (hidden)

Every image is in `GENERAL` outside a pass; the held layout (`VulkanTexture::textureLayout`, which plume reads while recording
copies) is always the true one because only this module records barriers, always through `VulkanCommandList::barriers` with all
stages (plume takes the next barrier's source stages from the previous one's, so a full mask keeps every barrier a full
dependency). What each call records:

| Call | Barriers (per image) |
|---|---|
| `CopyImage`, `Blit` | source `GENERAL` to the transfer-source layout, destination to the transfer-destination layout (one image on both sides: `GENERAL` to `GENERAL`), the command, then both back to `GENERAL` |
| `ClearImage`, `CopyBufferToImage` | to the transfer-destination layout and back |
| `CopyImageToBuffer` | to the transfer-source layout and back; a memory barrier before it (the readback memory may be reused) and a host-read barrier after it |
| `BeginTargetPass` | colour: `GENERAL` (a memory barrier), depth: plume's depth-attachment layout; framebuffers are cached per attachment pair and made with `colorAttachmentsGeneralLayout` (patch 04) |
| the end of a pass | colour: `GENERAL` to `GENERAL` (what the pass wrote becomes visible), depth: back to `GENERAL` |
| `FeedbackCopy` | the target to the source layout and the twin to the destination layout, one `vkCmdCopyImage` over every mip level, both back to `GENERAL`; a pass that was open is resumed (same attachments, load) |
| `PrepareForPresent` | `GENERAL` to `PRESENT_SRC_KHR` |

A first use of an image moves it from `UNDEFINED` (a new image holds `UNKNOWN`; contents are not kept). A pass is kept open when
the next `BeginTargetPass` has the same attachments and render area (the new clears are done inside it); a copy, clear, blit, upload,
feedback copy or `PrepareForPresent` ends it, and so does `BeginTargetPass` with other attachments or core's `EndPass`. The module
cannot see a `Submit`, so **a pass still open when its recorder is submitted** is ended by the submit with no barrier after it: the
colour image is still in `GENERAL` (patch 04), the depth image still in the attachment layout, and plume's held layout says so; the
next operation on the image (or `Normalize` by a binding) moves it. That is why a depth target cannot be handed over, and why another
thread that samples an image whose owner has not settled it gets a breach. A recorder dropped without a submission puts the
held layout, the stages and the dirty flag of every image it touched back (an `on_submit` hook called with 0). The depth
attachment's layout during the pass is plume's (`unverified` on a device that is not lavapipe, as the spec said).

### The owner rule as built

Checked with `MH4N_RHI_CHECKS` (the core's switch), through the owner-rule handler. Clauses are those of `rhi-spike.md` 9.4 item 5.

| Clause | Where | Breach |
|---|---|---|
| 1. one owning thread, the one that made the image, records on it | `targets::Acquire`, called by every operation, by `BeginTargetPass`, `FeedbackCopy`, `PrepareForPresent` and by the binding setters of the owning thread | `ForeignThread` |
| 1. another thread samples an image only while it is not an attachment of an open pass, and when its owner has settled it in `GENERAL` | `PrepareUse` in `SetSampledImage` and `SetStorageImage` (the wait on the producing value is not checked: the layer's synchronization validation sees a missing one on one queue, 6.9 C2) | `ForeignThread` |
| 2. `HandOver(image, thread, wait)` by the owner; an attachment of an open pass and any depth-stencil target cannot be handed over | `HandOver` (images.cpp) | `HandOver` (and the hand-over does not happen); `ForeignThread` when the caller does not own the image |
| 2. the receiver's first record or sample takes the image over and adds `wait` to its recorder's `required_waits` | `targets::Acquire` | `RequiredWait`, from core's submit path, when the submission does not carry a wait of that stage and at least that value |
| 3. recorders are submitted in the order they were begun, one open recorder per thread | core (`SubmitRecorder`, `MakeRecorder`) | `SubmissionOrder`, `OneOpenRecorder` |

**How a hand-over is expressed.** The sender (the owner, after the submission that produced the image returned `v`) calls
`HandOver(image, receiver_thread_id, Wait{v, stage})` with the stage that will read the image in the receiver's first submission
(`FragmentShader` for a draw that samples it). The receiver records or samples as usual and submits with a `Wait` of that stage and a value
at least `v` (`Backend::Submit(Recorder&&, waits)` of N2-28; until then `core::SubmitRecorder`, as the tests do). Nothing else
changes: the image stays in `GENERAL`. The old owner recording on it afterwards is a `ForeignThread` breach. **Finding for N2-28
(`needs-decision`, in `core/` or plume): a `Wait` at `Stage::Transfer` is mapped to plume's copy group, whose stage flags include
`VK_PIPELINE_STAGE_HOST_BIT`, which the layer reports as `VUID-VkSubmitInfo-pWaitDstStageMask-00078` (the host stage cannot be
waited at). So a hand-over whose first use is a copy or clear cannot be expressed with the right stage today; the tests hand over to
a draw that samples (`FragmentShader`).**

### The feedback copy

`FeedbackCopy(target)` ends the open pass, copies every mip level and layer of the target into a twin (made once per target and
kept: same format, size, mips and layers, `shadow_source` inherited, owned by the thread that made it), leaves both in `GENERAL`, and
begins the pass again (same attachments, load) if one was open: one pass break and one full copy per call (5.6). Sampling a target
that is an attachment of the open pass is an `Error` of `SetSampledImage`; its twin is the way.

### Per-draw bindings

`BindingArena` (one per recording thread) keeps a free list of binding sets per set shape (`BindingSetDesc`); `Acquire` returns a set
no submission in flight uses (its last value is at most `CompletedValue()`, or it was acquired for a recorder that was dropped) or makes one;
the set is attached to the thread's open recorder and stamped with the submission's value by the recorder's `on_submit` hook.
Plume's set object makes its own layout and pool (plume_vulkan.cpp:1885-1944), so after the first frames none is made. A slot binds
a texture and a sampler as two descriptors (`SetSampledImage`, `SetSampler`, descriptor layout `GENERAL`); a texel buffer
(`SetTexelBuffer`, whole buffer, with a `VkBufferView` of the module's, owned by the arena's set); one storage image
(`SetStorageImage`, a shadow source's `ShadowStorage` view only); uniform and storage buffers at an offset are core's
(`SetUniformBuffer` through plume's structured view with stride 1, `Binding a buffer at an offset` above). A binding that uses an
image also records the owner's barrier that brings it to `GENERAL` with its writes visible, on the thread's open recorder (a pass open
on other images is then broken and restarts with load at the next draw: correct, and a tile round trip on a tile-based GPU,
unmeasured). The setters act on the calling thread's open recorder: write and bind a set on the thread that draws with it.

`UploadToImage(backend, recorder, image, region, bytes)` fills an image through a new staging buffer in the Upload heap, kept until
the submission's value has passed; `StagingUploadCount` counts them. Images are never host-writable here, so `direct_upload` (reported
by the core) is not acted on and the staged path is the only one, with the bit on or masked off.

### A surface from the caller (what the PC needs)

On the RX 6700 XT the Windows loader lists no `VK_EXT_headless_surface`, so the swapchain surface there must come from a real window
(`VK_KHR_win32_surface`, or SDL3's), never from the backend. The frozen interface already supports a surface from the caller, in this
order: (1) `InstanceOptions::extensions` names the window system's instance extensions (each is required; one the loader lacks fails
`CreateInstance` naming it); (2) `NativeInstance(instance)` gives the `VkInstance` to create the `VkSurfaceKHR` on, before any backend
exists; (3) `BackendOptions::surface = SurfaceFrom(surface)` makes the device enable `VK_KHR_swapchain` and choose its queue family
among those that can present to the surface (it fails when none can); (4) the caller creates the swapchain on `Handles(*backend).device`
(`queue_family_index` and `queue` there) with the usage its frames need (`TRANSFER_DST` for `ClearImage` and the copies, `COLOR_ATTACHMENT`
for passes, `SAMPLED` or `TRANSFER_SRC` for what reads it); (5) every `vkQueuePresentKHR` goes under `QueueLock`, and nothing
that submits is called while the lock is held. **The backend never creates, resizes or destroys a swapchain or a surface**, and it
keeps only the surface handle (destroy the surface after the backend).

`WrapExternalImage(backend, vkImage, format, width, height)` takes one image of `vkGetSwapchainImagesKHR`; `format` is the member of
`Format` the swapchain's `VkFormat` is (`Bgra8Unorm` or `Rgba8Unorm`) and the size is the swapchain extent: both come from the caller's
swapchain and are not checked against it. The wrapper holds plume's `VulkanTexture(device, VkImage)` with ownership off, so the backend
**never destroys the `VkImage`**, plus a view for a framebuffer (destroyed with the wrapper). One wrapper per swapchain image, made
once and kept while the swapchain lives, so that its held layout persists. **The wrapper must be dropped before the caller recreates or
destroys the swapchain** (and so must every recorder that used it and every view made from it), after the GPU has finished with it
(`Backend::WaitIdle`): the caller (N2b-06) guarantees that, the backend cannot know. A fresh swapchain gets fresh wrappers
(`UNKNOWN` layout, first use from `UNDEFINED`).

`PrepareForPresent(image)` is the last command of the frame's recorder: it ends the open pass and moves the wrapper to
`PRESENT_SRC_KHR` (plume's `RenderTextureLayout::PRESENT`), so the held layout is `PRESENT`. After the next acquire the **first
operation reads the held `PRESENT` as `UNDEFINED`**: the barrier's old layout is `UNDEFINED`, the contents are not kept (an acquired
image's contents are undefined), no barrier ever names `PRESENT_SRC_KHR` as its old layout, and the acquire needs no call into the
backend. Call it before every present, in the recorder whose submission precedes the present. It needs a device made with a surface
(without `VK_KHR_swapchain` the layer rejects the layout).

**What N2b-06 still has to supply** (the named risk: acquire and present with binary semaphores, which no frozen call can wait for or
signal): `Backend::Submit` waits only on the backend's timeline (`Wait`), so (a) a frame's recorder cannot wait for the acquire
semaphore: the test waits on an acquire *fence* on the CPU before it records; (b) `vkQueuePresentKHR` needs something to wait for or the
layer reports `SYNC-HAZARD-PRESENT-AFTER-WRITE` against the render: the test, under `QueueLock`, submits an empty batch that signals a
binary semaphore (its first synchronisation scope is everything submitted before it on the queue, the frame's `Submit` included) and
presents with that semaphore as the wait. This is what the test does and the layer accepts on lavapipe; **it is not a design** (no
claim for the RX 6700 XT, MoltenVK, a second queue family or a present thread that is not the renderer's), and no frozen header was
changed for it.

The swapchain frames are one case of their own, `swapchain/frames_over_a_headless_surface_more_than_it_has_images`; it uses `MakeHeadlessSurface`
unchanged, which fails the test under `MH4N_REQUIRE_VULKAN=1` and skips otherwise on a loader without the headless extension. There is
no Windows or window-system branch in the test. **On the RX 6700 XT that case cannot run** (its surface needs a real window); the
other 26 cases of the file need no surface and are the PC's leg of this package (formats and the chooser with `d24s8` masked, which is the PC's
path, images, passes, layouts, the owner rule, bindings), alongside `external/a_wrapped_image_is_cleared_copied_and_not_destroyed_with_its_wrapper`
(a `VkImage` the test made, no surface).

### Tests (N2-27)

`native/tests/gpu_backend_targets_test.cpp` (ctest name `gpu_backend_targets_test`; shaders in `native/tests/shaders/gpu_backend_targets/`,
`gpu_backend_targets_shaders.inc` generated by `tools/native/spirv_inc.py`). Every expected pixel and byte is worked out in a
comment from the formats. A case skips with a message without a Vulkan device and fails under `MH4N_REQUIRE_VULKAN=1`.

| Case | What it pins |
|---|---|
| `formats/query_agrees_with_vkGetPhysicalDeviceFormatProperties_for_every_format` | `QueryFormat` against the test's own `vkGetPhysicalDeviceFormatProperties` (through `vulkan_access.h`'s physical device) for every `Format`, unmasked and under `bc7` |
| `formats/the_chooser_follows_the_support_table_on_devices_this_machine_is_not` | every rule of `ChooseFormatWith` on synthetic tables (MoltenVK, swapped orders only, none, texture-only, D24S8 off, BC off) |
| `formats/the_chooser_at_the_device_and_under_the_masks` | `ChooseFormat` on the device; `d24s8` masked takes `D32S8`, `bc7` masked says decode on the CPU |
| `images/each_colour_format_cleared_and_copied_to_a_buffer`, `.../each_depth_format_cleared_and_copied_as_two_regions` | RGBA8, RGB8 as RGBA8, RGB5A1, RGB565, RGBA4 (the words of the stored order: `0xAD4A` or `0x5555`, `0x9A8B`/`0x2AA7`/`0xCD45`, `0xA5CF`/`0xC5AF`) and D16, D24S8, D24S8 masked to D32S8 (depth `0xFFFFFF`/`0x3F800000`/`0xFFFF` and stencil `0xAB`, then 0 and `0x3C`) |
| `images/an_upload_goes_through_a_staging_buffer_with_direct_upload_on_or_masked` | the staging count (one per upload) and the same image with the bit on and masked off |
| `images/storage_usage_only_on_shadow_sources`, `images/copies_blits_and_mip_generation_within_one_image` | the recorded creation flags; copies within one image, nearest and linear blits, mip generation |
| `passes/...` | render then sample in one recorder and in two (6.9 A1, A2); the pass kept open (counted at `vkCmdBeginRenderPass` through volk) and ended by a copy; the render area; the feedback copy (a draw samples the previous contents: `(255, 102, 0, 255)`), every mip; blended (`(51, 0, 204, 51)`) and depth-tested draws on D24S8 and on the masked D32S8 |
| `layouts/...` | the exact `(old, new)` layouts of the image barriers (read from `vkCmdPipelineBarrier` through volk) for a clear and a copy, for a pass, for a pass left open at `Submit`, and for a dropped recorder |
| `shadow/imageStore_into_the_r32_uint_view_with_the_interlock_shader_and_without_it` | three overlapping draws in a pass with no attachments: every texel 3, with the interlock shader and (under the `interlock` bit masked in the case's own backend) the compare-and-swap shader |
| `samplers/...`, `bindings/...` | the custom border colour returned out of range `(51, 102, 153, 204)` and refused with the bit off; a texel buffer; 1,000 frames with a fresh texture binding each (the `vkAllocateDescriptorSets` count after frame 10 equals the count after frame 1,000, at most 5) and each draw's uniform offset reaching its shader |
| `owner_rule/...`, `threads/...` | the foreign-thread copy, the two refused hand-overs, the receiver with and without the wait; two threads each rendering into their own images |
| `external/...`, `swapchain/...` | a wrapped `VkImage` cleared, copied, not destroyed by its wrapper; three headless-swapchain frames acquired, wrapped, cleared, `PrepareForPresent`ed and presented under `QueueLock` |

The masks to run it under (the cases that build their own masked backend run that leg whatever the environment says):
`MH4N_RHI_MASK=<name>` for each of `d24s8 interlock index_uint8 custom_border_color triangle_fans logic_op dynamic_rendering
pipeline_library eds3_blend bc7 timeline present_wait direct_upload`, and `all` (`MH4N_RHI_MASK=d24s8,bc7 ctest ... -R '^gpu_backend_targets_test$'`).
With `MH4N_RHI_CHECKS` off the three owner-rule cases skip themselves with a message.

**Run here** (clang 18, lavapipe/llvmpipe, validation layer 1.3.275): `MH4N_REQUIRE_VULKAN=1 ctest --test-dir build/native -R '^gpu_backend_targets_test$'` passes (27 cases, 7,799 assertions, about a second; the log names the
llvmpipe device); the whole binary passes under each of the 13 `MH4N_RHI_MASK` names and under `all`; `python3 tools/native/vk_layer_run.py --build
build/native -R '^gpu_backend_targets_test$'` reports no layer message, the swapchain frames included (the first run of it found
two defects of ours, a reused readback buffer's write-after-write and `PrepareForPresent` on a device without a swapchain, and
one of core/plume: the `Stage::Transfer` wait above); the `build/tsan` tree (the commands of "The ThreadSanitizer job", the two
targets only) ran this test three times and `gpu_backend_core_test` once with `halt_on_error=1` and `tools/native/tsan.supp`: no
report. Mutants tried (one rule broken at a time, the test seen to fail, the rule restored): the foreign-thread breach removed
(2 cases fail), the hand-over wait not added to the recorder (1), `CopyImageToBuffer` not returning the image to `GENERAL` (the layout case),
a pass end that does not move the depth image back (the layout case), the arena never reusing a set (the 1,000-frame case) and the
feedback copy not copying (2 cases). `native-tsan` on GitHub is *(CI)*, `pending`.

**`unverified` / pending, not claimed:** the RX 6700 XT (no D24S8, a real-window surface, its format query and descriptor
alignment), MoltenVK and KosmicKrisp (the R-first-only 16-bit formats, the depth layout), a second queue family (a hand-over across
queues; lavapipe has one), the `native-tsan` job on GitHub *(CI)*, and the blit and attachment cost of `GENERAL` on the target GPUs.

## Not checked on this machine

`readelf -d` for `libX11` is meaningful on `build/native/bin/rhi_plume_test` (no `NEEDED` entry besides libstdc++, libm, libgcc_s, libc and the loader's), not on `mh4u_native_runner`, which does not link plume yet.

The Windows llvm-mingw build, the macOS build, the three CI jobs and the `native-tsan` job on GitHub are `pending (ci)`; the patterns on the RX 6700 XT, MoltenVK and a second queue are the PC's legs (`rhi-spike.md` 9.2) and were not run: the macOS branch of `plume_vulkan.cpp` (`VK_KHR_portability_enumeration`, the portability-subset features, `triangleFan` forced false) and the Windows branch (`NOMINMAX`) were not compiled here.
