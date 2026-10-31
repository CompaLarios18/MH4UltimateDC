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
instead of skipping if lavapipe disappears) and `MH4N_REQUIRE_HEADLESS_SURFACE: "1"` (N2-32: the four cases that need
`VK_EXT_headless_surface` fail instead of skipping when the loader lacks it; see the test helpers below), and a step runs the harness over
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
| `B1`, `B2`, `B3`, `B4`, `B5` (`B<n>_shadow_*`) | zero-attachment pass (patch 01) writing an `R32_UINT` view of an `RGBA8` image (`MUTABLE_FORMAT`) with three overlapping full-screen draws that each add 1: load/store, with interlock (patch 02), compare-and-swap, both, and with a `GENERAL` barrier between the draws | B2 to B5: every texel is 3 (0 of 256 differ). B1 (N2-32): no interlock and no barrier, so the order of the three read-modify-writes is the device's: the case asserts what the shader guarantees (every texel holds 1 to 3: the pass ran and wrote every texel) and logs how many hold 3 (all 256 on lavapipe, whose execution is serial). B2 and B4 need `fragmentShaderInterlock`: a device without it skips them with a message, except lavapipe under `MH4N_REQUIRE_VULKAN=1`, where it fails |
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
  no loader or device, and fail instead under `MH4N_REQUIRE_VULKAN=1`, which means only "a Vulkan device must exist" (N2-32). The headless-surface cases
  (`device/headless_surface` in `gpu_backend_core_test` and `rhi_plume_test`, `instance/the_headless_surface_and_the_debug_messenger_come_when_asked`,
  `presentation/with_a_headless_surface_agrees_with_the_probe`) skip with a message when the loader lacks `VK_EXT_headless_surface` (`MakeHeadlessSurface` and
  `SkipOrFailHeadless`; `rhi_plume_test` has its own copy of the check) and fail for that reason only under `MH4N_REQUIRE_HEADLESS_SURFACE=1`, which the `native` job
  of `native.yml` sets beside `MH4N_REQUIRE_VULKAN` (on the lavapipe stack the extension is there, and its absence means the setup broke). doctest 2.4.11 cannot
  mark a case skipped at run time, so every run-time skip (no device, no headless surface, a capability or format the device lacks, such as the two D24S8 cases)
  goes through `SkipCase` in the same header, which records `{test case, why}`; a doctest listener (`REGISTER_LISTENER` in the header, which the three plume-level
  binaries include too) prints at the end of the run `skipped at run time: N` and one line `  <test case>: <why>` per entry, and `skipped by decorator: <case>`
  for each case registered with `doctest::skip`. A case that has nothing to assert on this device is listed under "skipped at run time" in the run's output;
  doctest's own summary still counts it as passed. On lavapipe the block prints `skipped at run time: 0`. When the validation layer is enabled (`VK_INSTANCE_LAYERS`, or
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
| 2. `HandOver(image, thread, wait)` by the owner; an attachment of an open pass and any depth-stencil target cannot be handed over | `HandOver` (images.cpp) | `HandOver` (and the hand-over does not happen); `ForeignThread` when the caller does not own the image (case `owner_rule/a_hand_over_by_a_thread_that_does_not_own_the_image_is_a_breach_and_does_nothing`) |
| 2. the receiver's first record or sample takes the image over and adds `wait` to its recorder's `required_waits` | `targets::Acquire` | `RequiredWait`, from core's submit path, when the submission does not carry a wait of that stage and at least that value |
| 3. recorders are submitted in the order they were begun, one open recorder per thread | core (`SubmitRecorder`, `MakeRecorder`) | `SubmissionOrder`, `OneOpenRecorder` |

**The feedback twin follows its target.** `HandOver` of a target that has a twin hands the twin over too, to the same thread with the same `Wait`: the receiver's `FeedbackCopy` takes over the target and the twin (so the twin is not a `ForeignThread` breach), and its recorder carries the hand-over's wait once (it is the same entry for both). A twin made after the hand-over is made by the receiver, which owns it.

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
other 34 cases of the file need no surface and are the PC's leg of this package (formats and the chooser with `d24s8` masked, which is the PC's
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
| `images/an_upload_goes_through_a_staging_buffer_with_direct_upload_on_or_masked` | the staging count (one per upload) and the same bytes with the bit on and masked off: it pins that no direct path exists yet |
| `images/storage_usage_only_on_shadow_sources`, `images/copies_blits_and_mip_generation_within_one_image` | the recorded creation flags; copies within one image, nearest and linear blits, mip generation |
| `passes/...` | render then sample in one recorder and in two (6.9 A1, A2); the pass kept open (counted at `vkCmdBeginRenderPass` through volk) and ended by a copy; the render area; the feedback copy (a draw samples the previous contents: `(255, 102, 0, 255)`), every mip; blended (`(51, 0, 204, 51)`) and depth-tested draws on D24S8 and on the masked D32S8 |
| `layouts/...` | the exact `(old, new)` layouts of the image barriers (read from `vkCmdPipelineBarrier` through volk) for a clear and a copy, for a pass, for a pass left open at `Submit`, and for a dropped recorder |
| `shadow/imageStore_into_the_r32_uint_view_with_the_interlock_shader_and_without_it` | three overlapping draws in a pass with no attachments: every texel 3, with the interlock shader and (under the `interlock` bit masked in the case's own backend) the compare-and-swap shader |
| `samplers/...`, `bindings/...` | the custom border colour returned out of range `(51, 102, 153, 204)` and refused with the bit off; a texel buffer; 1,000 frames with a fresh texture binding each (the `vkAllocateDescriptorSets` count after frame 10 equals the count after frame 1,000, at most 5) and each draw's uniform offset reaching its shader |
| `owner_rule/...`, `threads/...` | the foreign-thread copy, the two refused hand-overs, a hand-over by a non-owner, the receiver with and without the wait, the feedback twin following its target; two threads each rendering into their own images |
| `external/...`, `swapchain/...` | a wrapped `VkImage` cleared, copied, not destroyed by its wrapper; six headless-swapchain frames (more than the swapchain has images, so the held `PRESENT` is read as `UNDEFINED` on every reused image) acquired, wrapped, cleared, `PrepareForPresent`ed and presented under `QueueLock` |

The masks to run it under (the cases that build their own masked backend run that leg whatever the environment says):
`MH4N_RHI_MASK=<name>` for each of `d24s8 interlock index_uint8 custom_border_color triangle_fans logic_op dynamic_rendering
pipeline_library eds3_blend bc7 timeline present_wait direct_upload`, and `all` (`MH4N_RHI_MASK=d24s8,bc7 ctest ... -R '^gpu_backend_targets_test$'`).
With `MH4N_RHI_CHECKS` off the owner-rule cases skip themselves with a message.

**Run here** (clang 18, lavapipe/llvmpipe, validation layer 1.3.275): `MH4N_REQUIRE_VULKAN=1 ctest --test-dir build/native -R '^gpu_backend_targets_test$'` passes (35 cases, 8,046 assertions, about a second; the log names the
llvmpipe device); the whole binary passes under each of the 13 `MH4N_RHI_MASK` names and under `all`; `python3 tools/native/vk_layer_run.py --build
build/native -R '^gpu_backend_targets_test$'` reports no layer message, the swapchain frames included (the first run of it found
two defects of ours, a reused readback buffer's write-after-write and `PrepareForPresent` on a device without a swapchain, and
one of core/plume: the `Stage::Transfer` wait above); the `build/tsan` tree (the commands of "The ThreadSanitizer job", the two
targets only) ran this test three times and `gpu_backend_core_test` once with `halt_on_error=1` and `tools/native/tsan.supp`: no
report. Mutants tried (one rule broken at a time, the test seen to fail, the rule restored): the foreign-thread breach removed
(2 cases fail), the hand-over wait not added to the recorder (1), `CopyImageToBuffer` not returning the image to `GENERAL` (the layout case),
a pass end that does not move the depth image back (the layout case), the arena never reusing a set (the 1,000-frame case) and the
feedback copy not copying (2 cases); after review: the preferred order of `Rgba4` and of `Rgb5A1` swapped, a linear blit made nearest, the integer clear branch removed, the arena handing out a set still pending for the recorder, the stencil view made with the depth aspect, `HandOver` by a non-owner not checked, `Acquire` of the twin removed from `FeedbackCopy` (and `HandOver` not handing the twin over), the clear of a kept-open pass removed, and the layer-count test loosened: each fails a case. `native-tsan` on GitHub is *(CI)*, `pending`.

**`unverified` / pending, not claimed:** the RX 6700 XT (no D24S8, a real-window surface, its format query and descriptor
alignment), MoltenVK and KosmicKrisp (the R-first-only 16-bit formats, the depth layout), a second queue family (a hand-over across
queues; lavapipe has one), the `native-tsan` job on GitHub *(CI)*, and the blit and attachment cost of `GENERAL` on the target GPUs.

## Sync and readback (N2-28)

What the renderer thread, the compositor and the capture player need from the backend beyond drawing: two modules under
`native/src/gpu/backend/`, namespace `mh4n::gpu::rhi`. **The interface is documented in `rhi.h`** (the "sync (N2-28)" and "readback
(N2-28)" groups of `Backend` and `Recorder::MarkFrame`); this section says how it is built, what blocks and when, and what it does
not do. Both modules keep their state in the slots `core/impl.h` reserves (`Backend::Impl::sync`, `::readback`) and edit nothing of
`core/`, `targets/`, `rhi.h`, `vulkan_access.h` or `native/third_party/rhi/`.

| File | What |
|---|---|
| `sync/submit.cpp` | `Backend::Submit(Recorder&&, span<const Wait>)` and `Backend::Retire`, the retire list and its drain |
| `sync/pipeline_cache.cpp` | `LoadPipelineCache`, `SavePipelineCache`, the header check. Derived from Azahar's `vk_pipeline_cache.cpp` (GPL-3.0-or-later; provenance header in the file) |
| `sync/timestamps.cpp` | `Recorder::MarkFrame`, `Backend::FrameGpuTimeNs` |
| `sync/sync.h` | the module's own header (not the frozen interface): `RetiredCount`, `DrainRetired`, `ReleaseRetired`, `LastFrameTimestamps`, `FramesInFlight`, `CheckPipelineCacheBlob` |
| `readback/readback.cpp` | `Backend::ReadbackToGuest`; derived from Azahar's `BlitHelper::DepthToBuffer` (`vk_blit_helper.cpp`), `MortonCopy`/`EncodePixel` (`texture_codec.h`) and `Encode*` (`color.h`), all GPL-2.0-or-later (provenance header in the file) |
| `readback/image.cpp` | `Backend::ReadImage` |
| `readback/shaders/readback.comp`, `readback_shaders.inc` | the compute pass (three variants, `spirv_inc.txt`), its SPIR-V committed; `python3 tools/native/spirv_inc.py native/src/gpu/backend/readback/shaders native/src/gpu/backend/readback/readback_shaders.inc` regenerates it byte for byte. Derived from Azahar's `vulkan_depth_to_buffer.comp`, `texture_codec.h` and `color.h` (provenance header in the file); the test file `gpu_backend_readback_test.cpp` restates the Morton tables and byte orders of the last two and carries the header too |

### Waits and the fence fallback

`Submit(recorder, waits)` goes through core's one submit path (`core::SubmitRecorder`, under the submit lock). A `Wait` is
`{value, stage}` on the backend's own timeline. **With the `timeline` bit**, every wait is submitted: one `vkQueueSubmit`, one stage mask
per wait semaphore (patch 03; the vendored backend maps a `Stage` to its graphics, compute or transfer group, `core::ToPlumeStages`), the
values being the timeline's (patch 05), and the submission signals the next value. The GPU holds the submission; the submitter is **not**
blocked. **Without it** (absent, or masked with `MH4N_RHI_MASK=timeline`) core submits no wait, and `Submit` does the wait itself first: if
the highest value among the waits has not completed, the submitting thread blocks on the CPU in `Backend::Impl::WaitFor` (core's fallback,
which this module extends and does not duplicate: a fence per submission, the completed value advanced in order, a wait in 2 ms slices under the
fence mutex), before it takes the submit lock; other threads keep submitting meanwhile. Values complete in order, so the highest covers
them all, and the values stay comparable with the timeline's. The cost, as the spec says: the submitting thread (a compositor or present thread)
blocks until the work of that value has finished on the GPU instead of queueing behind it. No device of `rhi-spike.md` section 4 lacks timeline
semaphores, so the fallback runs only under the mask.

- **A wait for a value no submission has issued** (above `LastSubmittedValue()`) would block the queue for ever, or the CPU: it is left out with an
  `Error` line of category `RHI` ("a wait for value N was left out"). A requirement it was meant to cover is then a breach, below.
- **`required_waits` and the owner rule.** `core::SubmitRecorder` checks, with `MH4N_RHI_CHECKS`, that each wait the recorder requires (an image
  handed over to its thread, `Recorder::Impl::required_waits`) is covered by a wait **of the same stage and a value at least as high** in the list
  `Submit` passes it: a lower value, or another stage, or no waits, calls the owner-rule handler with `OwnerRule::RequiredWait`. The check does not depend
  on the `timeline` bit: without it the wait is not submitted but the CPU wait has made it true, and the stage is still compared.
- **Stage.** Name the stage that reads. A wait at `FragmentShader` and one at `ComputeShader` in one submission reach `vkQueueSubmit` as two masks,
  each its own (`rhi-spike.md` 6.9 C3: unpatched plume read past its one local). **`Stage::Transfer` is refused by the layer today**
  (`VUID-VkSubmitInfo-pWaitDstStageMask-00078`): plume's `toStageFlags` adds `VK_PIPELINE_STAGE_HOST_BIT` to the copy group and patch 03 passes it
  on. `needs-decision`: a numbered plume patch (owner: not N2-28) that filters the host bit from a wait's mask; until then a hand-over whose first
  use is a copy or clear cannot be expressed with the right stage, and no test here waits at `Transfer`.
- **Queues.** Lavapipe has one queue, so a wait across queues is `unverified` here: the RX 6700 XT's several families and MoltenVK are the legs for it
  (`rhi-spike.md` 9.2). On one queue a pipeline barrier already orders earlier submissions, which is why the hand-over tests can pass with a missing
  wait; they pin the owner-rule check, not the hardware effect.
- **Finding in `core/` (`needs-decision`, not edited here).** In `core::SubmitRecorder`'s fence branch (`core/submit.cpp`) the new `PendingFence` is pushed
  to `pending_fences` **before** `executeCommandLists(..., raw)` submits with that fence; another thread's `Completed()` or `WaitFor()` can then call
  `vkGetFenceStatus`/`vkWaitForFences`/`vkResetFences` on a fence `vkQueueSubmit` is using. The validation layer reports it as
  `UNASSIGNED-Threading-MultipleThreads-Read` (VkFence) when a multi-thread test runs with `MH4N_RHI_MASK=timeline` (or `all`) under
  `vk_layer_run.py`: N2-27's `gpu_backend_core_test` gives it every time under that mask (24 messages in the reviewer's run: 12 `Read` in `vkGetFenceStatus` and 12 `Write` in `vkQueueSubmit` on a `VkFence`), so it is not new with this WP; `gpu_backend_sync_test` and `gpu_backend_readback_test` gave it in 3 of 4 runs and `gpu_backend_targets_test` was clean once: it is timing-dependent. The fix is to push after the
  submit returns (or to hold `fence_mutex` across it). Without the mask, and so in every run the Done-when lists, no message.

### Deferred destruction

`Retire(object, last_use)` keeps `object` (a `shared_ptr<void>`: a buffer, an image, a binding set, anything) until the GPU has passed `last_use`, then
releases it. core has no hook that runs when a value completes, so the list is ours (`SyncData::retired`) and is **drained at these points**, and only
there: a `Retire` (its own entry too), the end of a `Submit` with waits, `MarkFrame(FrameMark::Begin)`, `sync::DrainRetired(backend)` and
`sync::ReleaseRetired(backend)` (`WaitIdle`, then everything). The plain `Submit(Recorder&&)` is core's and does not drain; `WaitIdle` and
`CompletedValue` do not either. An entry is released on the thread that drains, outside the list's lock. A `last_use` that has passed already (or is 0)
is released inside the `Retire` call. A `last_use` above `LastSubmittedValue()` stays until that value is issued and has passed.
**Limit:** a retired buffer, image or binding set holds the backend's state, so a list that still holds one when the last outside reference to the
`Backend` goes keeps the backend, the device and the instance alive (core's `Backend::~Backend` has no hook to drain). Call
`sync::ReleaseRetired(*backend)` before dropping the last `shared_ptr<Backend>` if anything may be retired and in flight; a hook in core's destructor would
make it automatic (`needs-decision`, optional).

### The pipeline cache

`SavePipelineCache()` returns the bytes of the device's `VkPipelineCache` (patch 06 makes one, empty, when the device is made; every pipeline of the vendored
backend is created through it, and render passes are cached by formats). `LoadPipelineCache(blob)` accepts a blob only when its header is a
`VkPipelineCacheHeaderVersionOne` for this device, as Azahar checks its disk cache (`PipelineCache::IsCacheValid`): at least 32 bytes; header length
(offset 0) from 32 to the blob's size; header version (offset 4) 1; vendor id (offset 8) and device id (offset 12) the physical device's; the 16 bytes at offset 16
equal to its `pipelineCacheUUID`. Anything else (an empty blob, 31 bytes, version 2, a changed vendor, device or any UUID byte, a header length of 16) is **refused**:
`LoadPipelineCache` returns false, nothing is loaded, **the device keeps the cache it has** (the empty one when it is called at start-up, as it should be, so
"an empty cache is used"), and one line goes to the mh4n log at **Info**, category `RHI`: `rhi: pipeline cache blob ignored (<reason>); the device keeps its
current cache`. (`DebugSeverity` has only Warning and Error and `rhi.h` has no logging; this is what core does for its one-time mask line. The default sink
writes `[Info] RHI: ...` to stderr; the test installs `mh4n::SetLogSink` and checks the level, the category and the count.) A blob whose header matches but
which the driver refuses is a `Warning` and false. `LoadPipelineCache` replaces the `VkPipelineCache` (plume destroys the old one): **call it before pipelines are
created or while no other thread creates one**. `SavePipelineCache` and pipeline creation from several threads are safe together by the specification (a `VkPipelineCache` is internally synchronised; `inference`, not exercised together here); `Create*Pipeline` itself may be called from several threads at once (plume's scratch vectors are `thread_local`, `rhi-spike.md` 6.1 G10): the
test makes pipelines from four threads at once and draws with them. The files on disk and the key list are N2-16's.

### Readback into guest memory

`ReadbackToGuest(image, rect, format, dest)` writes `rect` of a render target into `dest` in the PICA's layout, through one compute pass of ours
(`readback.comp`: an invocation writes one 32-bit word of the output, finds the pixel each of its bytes belongs to, and reads it with `texelFetch`), into a storage
buffer in the Readback heap, followed by a CPU wait. It is for the checks and the CPU reads N0-14 found, not for every frame.

**Layout** (the same for colour and depth: the PICA writes its framebuffers with the 8x8 Morton tiles it reads textures with, and Azahar's rasterizer cache
uses one `MortonCopy` for both, so there is one layout to implement):

- `rect` is the region of the image; `x`, `y`, `width` and `height` are multiples of 8 and `dest` the region's own tiled bytes (`width * height * bytes per pixel`,
  at least). The tiles go left to right, then the next tile row; 64 pixels per tile in Morton order: the pixel `(x, y)` of a tile (0 to 7) is at index `x0 y0 x1 y1 x2 y2`
  read from the lowest bit (x in the even bits): `(1,0)` is 1, `(0,1)` 2, `(2,0)` 4, `(0,2)` 8, `(4,0)` 16, `(0,4)` 32, `(7,7)` 63 (Azahar's `MORTON_ROW`/`MORTON_X`).
- **Row direction:** guest row 0 is the **last** row of the rect in the image: guest row `g` is image row `rect.y + rect.height - 1 - g`. This is Azahar's
  `MortonCopy`, which reads and writes the linear buffer "bottom up" (and its download copies the image into the buffer top row first). A renderer that draws
  the PICA's row `y` into image row `height - 1 - y` gets the PICA's own layout; N2-13 and N2-23 must confirm which way they draw (`unverified`: no capture has
  been compared).
- **Bytes** (little endian, as Azahar's `Encode*`):

| `PicaFormat` | Bytes per pixel | Memory |
|---|---|---|
| `Rgba8` | 4 | A, B, G, R (byte 0 is alpha) |
| `Rgb8` | 3 | B, G, R |
| `Rgb5A1` | 2 | word: R bits 11 to 15, G 6 to 10, B 1 to 5, A bit 0 |
| `Rgb565` | 2 | word: R bits 11 to 15, G 5 to 10, B 0 to 4 |
| `Rgba4` | 2 | word: R 12 to 15, G 8 to 11, B 4 to 7, A 0 to 3 |
| `D16` | 2 | the 16-bit depth |
| `D24` | 3 | the 24-bit depth, low byte first |
| `D24S8` | 4 | the 24-bit depth in bits 0 to 23, the stencil in bits 24 to 31 (bytes d0, d1, d2, s) |

**What each PICA format can be read from** (anything else is an `Error`, never a conversion): `Rgba8Unorm`/`Bgra8Unorm` images give any colour format (a narrower
component is cut from the top, Azahar's `Convert8To5` and kin: `v >> (8 - n)`); `R5G6B5`/`B5G6R5` give `Rgb565`, `R5G5B5A1`/`B5G5R5A1`/`A1R5G5B5` give `Rgb5A1`,
`R4G4B4A4`/`B4G4R4A4` give `Rgba4`, all with the PICA's bits whatever order the device stores (the pass reads logical components, so the swapped orders need no
special case: the test stores each order and gets the same word); `D16Unorm` gives `D16`; `D24S8` and `D32S8` give `D24` and `D24S8`; `D32Float` gives `D24`.

**D24S8 from `D32S8`, and the rule that converts the depth.** On a device without `D24S8` (the PC) or with the `d24s8` bit masked, the depth image stores a float and the pass converts
it as Azahar does (`vulkan_depth_to_buffer.comp`: `uint(texture(depth, ...).x * (exp2(24.0) - 1.0))`; `EncodeD24` of `texture_codec.h`: `static_cast<u32>(d32 * 0xFFFFFF)`): scale `2^24 - 1 = 16777215`,
**truncated**, the stencil read from the stencil view and placed in the top byte. (An earlier draft of this WP rounded to nearest; the spec says "as Azahar does", so it truncates.) For a depth that is an exact
unorm decode `c / 16777215` the product is the integer `c` in single precision (all 16,777,216 values checked once with a numpy float32 script, not part of the tests), so a `D24S8` image and the `D32S8`
image that stands in for it give the same bytes for depths 0, `0x800000` (nearest float `0x3F000001`, `0.5 + 2^-24`) and `0xFFFFFF`, with stencils 0 and `0xFF`: that is what the cross-format test
compares. **It does not hold for every float.** 0.25 is `4194303.75` of 24 bits: the `D32S8` path truncates it to `0x3FFFFF`, while lavapipe's `D24S8` clear stores the nearest value and reads back `0x400000`;
the test pins each format separately (and the pass-open case too) and does not compare them. `D16` is read as `roundEven(depth * 65535.0)`: Azahar copies D16 raw, so rounding there only recovers the stored integer.
`unverified`: truncation gives the exact integer back for an exact unorm decode, but a driver whose unorm decode is `c * (1 / 16777215)` could make truncation off by one; whether a real GPU (the RX 6700 XT's
`D32S8` path, MoltenVK) does is not measured, and the question goes to the maintainer. Also `unverified`: what the PICA itself does with a depth between two 24-bit values (no capture compared).

**Which thread, and what the checks do.** `ReadbackToGuest` and `ReadImage` record on an internal recorder, through the targets module's first-use path
(`Acquire`, then `Normalize` for the readback; `CopyImageToBuffer` for `ReadImage`): they obey the owner rule as any operation on the image does. **The thread that owns the image
may call them**; so may the thread an image was handed over to (`HandOver`), and then the call itself carries the hand-over's wait (the value waited on the CPU
first, and submitted at the stage the hand-over named and at the compute stage), so there is no `RequiredWait` breach. **Another thread** that was not handed the image calls the
owner-rule handler with `OwnerRule::ForeignThread` (with `MH4N_RHI_CHECKS`; the call then goes on, as the other operations do); without the checks nothing is
reported. A depth-stencil image cannot be handed over (N2-27), so a depth target is read back by its owner. Both functions see **only what has been submitted**: an image the calling
thread's open recorder has recorded on (or that is an attachment of an open pass) is an `Error` ("submit it first"), because the layout plume holds for it was changed at record
time.
**A pass still open when its recorder is submitted** leaves a depth image in plume's attachment layout and the colour image dirty; the readback's `Normalize` is the barrier that
moves it to `GENERAL` with its writes visible, and the test reads a depth image right after such a submission (layout `DEPTH_WRITE` before, `GENERAL` after).
One readback at a time per backend (a mutex around the pass, the buffers and the binding set; each waits for its submission). The output must fit the device's storage-buffer range.

### `ReadImage`

`ReadImage(image, x, y, width, height)` returns the region in the image's own format, row by row without padding: `width * height * bytes per texel`. Colour formats are the texel as
stored (`Rgba8Unorm`: R, G, B, A; a 16-bit packed format: the little-endian word as Vulkan's packed layout names it; `Rgba16Float`: four halves); `D16Unorm` and `D32Float`
the depth texels; `D24S8` and `D32S8` the **depth plane** (4 bytes per texel: D24 in the low 24 bits of a `D24S8` texel, the top byte not defined; the float's bits for `D32S8`) **followed by the
stencil plane** (1 byte per texel), as patch 11's buffer copy lays them out. The block-compressed formats and the vertex formats are an `Error`. The region must lie inside the image;
mip 0, layer 0. The same thread rules as above.

### Timestamps

`Recorder::MarkFrame(Begin)` resets and writes the first of a frame's two timestamps at the bottom of the pipe (outside a pass: `vkCmdResetQueryPool` is not allowed inside one, so a Begin
inside a pass is refused with an `Error` line), `MarkFrame(End)` writes the second, in whichever recorder the End is called (a frame may span recorders). A frame owns a query pool
of two queries (plume's `queryResults` reads a whole pool without waiting and fails when a query of it is unavailable). **`FrameGpuTimeNs()` is valid only after the submission that carried the End mark
has been passed by the GPU** (`CompletedValue() >= its value`): until then it returns the last frame read, 0 before the first. It reads the frames whose End has passed, in order, and recycles their
pools, so a program that never calls it does not grow the list (the next Begin reads too). The value is `end - begin` in nanoseconds (plume converts ticks with the device's
`timestampPeriod`, `Capabilities::timestamp_period`); a queue family without timestamps, or a zero period, gives 0 and one `Warning`. A Begin with no End is abandoned by the next Begin
(its pool returns when its Begin has run); a dropped recorder gives its mark back; an End with no frame begun is ignored with a `Warning`. One frame at a time per backend. `timestampValidBits` below 64 is not
unwrapped (`unverified`: no device met here has fewer).

### Tests (N2-28)

`native/tests/gpu_backend_sync_test.cpp` (12 cases; shaders `native/tests/shaders/gpu_backend_sync/`, `gpu_backend_sync_shaders.inc` by `spirv_inc.py`, also used by the readback test) and
`native/tests/gpu_backend_readback_test.cpp` (14 cases). Every expected pixel and byte is worked out in a comment from the formats. A case skips with a message without a Vulkan device and fails under
`MH4N_REQUIRE_VULKAN=1`.

| Case | What it pins |
|---|---|
| `waits/the_second_submission_waits_on_the_first_at_the_fragment_stage_and_samples_what_it_drew` | the first submission is held by a **gate** (a `vkCmdWaitEvents` on an event the test sets from the host); with timeline semaphores the second `Submit` returns at once while the first is unfinished and the submit info carries one wait, mask with the fragment bit, value = the first's; without (the case runs both legs: the environment's, and a backend with `Cap::Timeline` masked) the submitter is blocked while the gate is closed (300 ms, checked as "has not returned"), nothing is submitted as a wait, and after the gate opens `CompletedValue()` is at least the first's when `Submit` returns; the pixels are the red the first drew in every texel, not the clear colours |
| `waits/a_submission_with_two_waits_passes_two_stage_masks_each_its_own` | the masks and values read from `vkQueueSubmit` through volk: the first has the fragment bit, the second equals `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, values `{v1, v2}`; none without the bit |
| `waits/a_wait_for_a_value_nobody_issued_is_left_out_with_a_message` | the `Error` line, and the submission does not hang |
| `handover/an_image_drawn_on_one_thread_is_sampled_on_another_and_handed_back_by_a_value` | 6.9 D1 and D3 shapes: eight rounds, one thread draws (the pass is open at `Submit`) and hands the image over by the value, the other samples it with a `Wait` of that value, reads the pixels, hands it back; red and green alternate and are what the other thread sees; no breach |
| `handover/the_receiver_must_wait_for_the_value_that_produced_the_image` | exact wait and a higher one: no breach; a lower value, another stage, no waits: one `RequiredWait` breach each |
| `retire/an_object_retired_while_in_flight_is_destroyed_only_after_its_value`, `retire/release_retired_waits_for_the_gpu_and_empties_the_list` | a probe object and an image retired while the gated submission is unfinished are alive after `Retire` and `DrainRetired`, and are released (weak pointer expired) by the first drain point after the value passed; a passed value is released inside `Retire`; `ReleaseRetired` blocks until the gate opens |
| `pipeline_cache/a_saved_blob_is_accepted_by_a_second_backend_and_a_foreign_one_is_refused_with_an_info_line` | the header fields against the test's own `vkGetPhysicalDeviceProperties`; a second backend on the same instance and device accepts the blob; eight corruptions (a UUID byte, the last UUID byte, vendor, device, version, header length, 31 bytes, empty) are each refused with exactly one Info line of category `RHI`; the pure check with hand-written numbers |
| `pipeline_cache/pipelines_made_from_four_threads_at_once_are_all_usable` | 4 threads x 6 pipelines, started together, each thread draws with one into its image: 24 made, 4 draws right |
| `timestamps/...` (3 cases) | not read while the gated frame is in flight (`FrameGpuTimeNs() == 0`), positive after, `end > begin`, the next frame's begin not before the last's end; a Begin with no End, a dropped recorder, an End with no Begin; a Begin inside a pass is refused with the `Error` line and takes no pool |
| `colour/clear_and_readback_of_every_colour_format_into_guest_memory` | RGBA8 `99 33 00 FF`, RGB8 `33 00 FF`, `0xAD4A` (RGB565), `0xAA8B` (RGB5A1), `0xA5C3` (RGBA4), from the PICA names and from an RGBA8 image |
| `colour/every_stored_component_order_of_the_16_bit_formats_gives_the_pica_word` | each stored order the device can sample (`R5G6B5`, `B5G6R5`, `R5G5B5A1`, `B5G5R5A1`, `A1R5G5B5`, `B4G4R4A4`; `R4G4B4A4` is not sampleable on lavapipe and is skipped with a message) uploaded as its own word, read back as the PICA's |
| `colour/the_tiles_are_8x8_morton_with_the_last_row_first`, `colour/a_taller_image_orders_tile_rows_from_the_last_image_row_up` | a pattern image: nine literal pixels (indices 0, 1, 2, 3, 4, 8, 16, 32, 63 and the second tile's first) by hand, then the whole buffer against a model written from Azahar's tables for RGBA8, RGB8, RGB565, RGB5A1, RGBA4; a tile-aligned sub-rect; a 8x16 image |
| `depth/d24s8_and_the_masked_d32s8_give_the_same_bytes_for_every_depth_and_stencil` | depths 0, `0x800000`, `0xFFFFFF` x stencils 0 and `0xFF`, read as `D24S8` and `D24`, on `D24S8` and on `D32S8` (`Cap::D24S8` masked in the case's own backend): the same bytes; then 0.25 pinned per format, not compared: `0x3FFFFF` on `D32S8` (truncated), `0x400000` on lavapipe's `D24S8` |
| `depth/d16_and_d32_float` | `0`, `0x5555` (1/3), `0xAAAA` (2/3, upper half: pins the scale 65535), `0xFFFF`; `D32Float` as `D24`: 0.25 -> `FF FF 3F` (truncated), 1.0 -> `FF FF FF` |
| `depth/an_image_left_in_the_attachment_layout_by_a_pass_open_at_submit_is_read_back_right` | a draw at depth 0.25 and stencil clear `0x5A` on `D24S8` (both formats) and `D16Unorm`, the pass open at `Submit`; the held layout is `DEPTH_WRITE` before the readback, the bytes `FF FF 3F 5A` on `D32S8`, `00 00 40 5A` on lavapipe's `D24S8` (`AA AA` for D16, drawn at depth 2/3), `GENERAL` after; the colour image is red |
| `read_image/...` (2 cases) | the clear colour in `Rgba8Unorm`, `R8Unorm`, `R5G6B5`, `Rgba16Float`, `D16Unorm`, the two planes of `D24S8`/`D32S8`; a sub-region row by row, errors |
| `errors/...`, `owner_rule/...` | bad rects, destinations and formats; an image the open recorder has recorded on; a foreign thread: one `ForeignThread` breach per call, none for the owner or after a hand-over (both with and without the `timeline` bit), no `RequiredWait` |
| `timestamps/a_frame_with_a_clear_and_a_readback_has_a_time_and_the_next_frame_follows_it` | three frames; Begin and a clear in one recorder, the readback (its own submission) between, the End in a recorder submitted after it: `FrameGpuTimeNs() > 0`, increasing |
| `readback/two_threads_reading_back_different_images_at_once_get_their_own_bytes` | a colour image and a depth image read back 25 times each from two threads at once: every result right (TSAN covers the race) |
| `lifetime/a_backend_is_destroyed_with_its_last_handle_after_a_readback_an_upload_and_a_read_image` | four backends, one that did a `ReadbackToGuest`, one an upload (a staging buffer), one a `ReadImage`, one all three: a `weak_ptr` to the backend's private state is alive while the handle is and expired after it is reset (without the release of `CreateBackend`'s handle it stayed alive for the first, second and fourth: seen on the RX 6700 XT) |

Lavapipe's queue thread holds the device while it waits for an event, and creating an image view, a framebuffer or a command list then waits for it too: the gated case therefore makes all of them in a first run
with the gate open (a lavapipe property, `unverified` elsewhere; nothing in the product depends on it).

**Masks to run them under:** `MH4N_RHI_MASK=<name>` for each of `d24s8 interlock index_uint8 custom_border_color triangle_fans logic_op dynamic_rendering pipeline_library eds3_blend bc7 timeline
present_wait direct_upload`, and `all`, and `d24s8,timeline` (the PC's pair). The cases that need a particular leg build their own masked backend, so those legs run whatever the environment says.
`MH4N_RHI_CHECKS` off: the cases that need the checks skip themselves with a message.

**Run here** (clang 18, lavapipe/llvmpipe, validation layer 1.3.275): both binaries pass under the environment as it is and under each of the 13 names, `all` and `d24s8,timeline` (log: llvmpipe);
`python3 tools/native/vk_layer_run.py --build build/native -R '^gpu_backend_(sync|readback)_test$'` reports no layer message (and none under `MH4N_RHI_MASK=d24s8`; under `timeline` and `all` it
reports the `UNASSIGNED-Threading-MultipleThreads-Read` of core's fence branch described above, the same message `gpu_backend_core_test` gives every time under that mask, timing-dependent in the other binaries). The first layer run found one
defect of ours: a reused readback buffer's `SYNC-HAZARD-WRITE-AFTER-WRITE` between two readbacks' passes, now ordered by a memory barrier at the start of each (the finding N2-27 made for its own copies).
The `build/tsan` tree (the commands of "The ThreadSanitizer job" above, with the four targets `gpu_backend_sync_test`, `gpu_backend_readback_test`, `gpu_backend_core_test` and
`gpu_backend_targets_test`; `halt_on_error=1`, `tools/native/tsan.supp`) ran the first two twice each and once more under `MH4N_RHI_MASK=timeline` (after the review changes too), and the other two twice: no report. `native-tsan` on GitHub is *(CI)*, `pending`.
**Mutants** (one rule broken at a time in a copy restored from the commit, the test seen to fail, the rule restored; the build checked to have succeeded, because a mutant that does not compile leaves the old binary running):
- sync: the CPU wait of the fallback removed (`waits/the_second_submission...` fails, in the masked leg and under `MH4N_RHI_MASK=timeline`); `Retire` releasing at once whatever the value (the retire case crashes: the image
  was destroyed in flight); the drain releasing entries whose value has not passed (the same case crashes); the header check skipped (the pipeline-cache case fails); only the UUID comparison skipped (the same case fails);
  every wait kept at `Stage::AllGraphics` (the two-waits case and both hand-over cases fail); the timestamps read before the value passed (the timestamps case hangs on a read and is stopped by `timeout`); the End's
  value not recorded (both timestamps cases fail).
- readback: x and y bits of the Morton index swapped (the three tile cases fail); guest rows not reversed (the same three); the D24 depth **rounded to nearest** instead of truncated, the first draft's rule (the 0.25 line of the depth case, the pass-open case and `d16_and_d32_float` fail, on the `D32S8` leg), and truncation with a +0.75 bias (the same three); the D24 scale `2^24` instead of `2^24 - 1` (the same three: 0.25 comes out `0x400000`); the stencil in the low byte (the depth and pass-open cases fail); the RGB8 bytes in R, G, B order (the colour and tile cases fail); narrower components
  cut from the bottom bits instead of the top (the colour cases fail); `Normalize` skipped (the pass-open depth case fails: the held layout is not `GENERAL`); `Acquire` skipped (the owner-rule case fails);
  the hand-over's waits not carried by the readback (the owner-rule case fails: a `RequiredWait` breach); the open-recorder check removed (the errors case fails); the mutex of `ReadbackToGuest` removed (the two-threads case fails by itself, without ThreadSanitizer); the End mark accepted only in the recorder of its Begin (the timestamps case whose End is in a later recorder fails); `MarkFrame(Begin)` not refused inside a pass (the new Begin-in-pass case fails). The D16 mutant that truncates instead of rounding survives: an equivalent mutant on this device (the stored integer comes back exact from the float decode either way). The case `colour/every_stored_component_order...` also appears in the list
  of every failing readback run only because its skip message (`R4G4B4A4` is not sampleable on lavapipe) prints the case's name; it is not the cause.

**`unverified` / pending, not claimed:** the RX 6700 XT for the counts above (both binaries now pass on it alone, see "Lifetime of a backend" below; the `D32S8` path is the one it runs, the byte checks of the depth cases passed there; its timestamp period and valid bits are not looked at), MoltenVK and KosmicKrisp,
a second queue family (a wait or a hand-over across queues; lavapipe has one), the row direction against a capture (above), the PICA's own depth rounding, the `native-tsan` job on GitHub *(CI)*, and any
claim about the cost of the CPU wait on a real present thread.

### Lifetime of a backend, and the readback test on the RX 6700 XT (N2-28 follow-up)

**What happened.** On the RX 6700 XT (AMD proprietary driver 26.10.1) `gpu_backend_readback_test` failed in `read_image/a_cleared_image_...` (every `ReadImage` returned wrong bytes), then
`vkCreateDevice` failed in the next cases, and the driver took a timeout (WER `LiveKernelEvent` 0x141 in `amdkmdag.sys`). **`ReadImage` was not the cause**, nor the copy, the
invalidate or the wait: run alone, `read_image/a_cleared...` and `read_image/a_region...` pass on the device with the right bytes (the first bytes of the mapped buffer after the wait were the
clear colour, `FF 00 33 99`, `33`, `4A AD`, the halves, `55 55`, `00 00 80 3F`), and so does every other case of the file run alone, and the pair `depth/an_image_left...` then
`read_image/a_cleared...` in one process. The `vkQueueSubmit failed with error code 0xFFFFFFFC` (`VK_ERROR_DEVICE_LOST`) lines of the failing run begin at the **tenth** device of the process, right after the
first clear had completed, and the wrong bytes are what `ReadImage` returns when its submission is refused (the buffer is zero). So the candidates in the work package (a short `Invalidate`, a wait that does
not wait, a layout hazard of the internal recorder) are all excluded by the alone runs, and `ReadbackToGuest` has the same readback path and is not different.

**The defect: a reference cycle that kept every device alive.** `Backend::Impl` holds the modules' slots; what a slot keeps for the life of the backend holds `Backend::Impl` through a strong
reference (every `Buffer`, `BindingSet` and `Pipeline` made by `CreateBuffer`, `MakeBindingSet` and `Create*Pipeline` does: the Impl must outlive them). The readback module keeps its three compute
pipelines, their binding sets and its output and parameter buffers in `ReadbackData`; the targets module keeps the staging buffer of the last upload in `TargetsData::staging_pending` (it is dropped at the
next upload, which may never come); the sync module keeps retired objects. So the `Backend::Impl` of any backend that did a `ReadbackToGuest` or an upload was never destroyed: its device, its queue and its
instance stayed until the process ended, whatever the test or the program did with its `shared_ptr<Backend>`. Lavapipe does not mind (a software device); the test file makes a backend per case, the first seven
cases made nine, all alive when the tenth was made, and the AMD driver lost the tenth. `gpu_backend_core_test` and `gpu_backend_targets_test` made many more devices and passed because their backends do not leak
(the targets file leaks at most a few through its three uploads). `unverified`: why the driver loses a device when nine others are alive (a limit of contexts, or memory) is inferred, not measured; what is
measured is that the leak exists (the new case fails without the release, below) and that with it the whole file passes alone on the device.

**The release.** `CreateBackend` returns a `shared_ptr<Backend>` whose deleter, when the last handle goes, waits for the device (`vkDeviceWaitIdle`) and resets the four slots in the order `Impl`'s destructor
would (`state`, `readback`, `sync`, `targets`) before the handle itself is deleted. The slot's resources then drop their references, and the `Impl` is destroyed with the handle (the device, the queue, the
instance) unless an object of the user's (an `Image` kept after the handle) still holds it: that object keeps the device alive and finds the slots empty, and each module makes its slot again on first use (they all
did already). The handles the modules make for themselves from an `Impl` (`Backend tmp(impl)` in the targets module) are not the returned one and release nothing. **The edit is in `core/device.cpp`**
(`ReleaseWithLastHandle`, a function of 15 lines above `CreateBackend`, and the one line of `CreateBackend` that returns the handle, which was `std::make_shared<Backend>(std::move(impl))`); no other file of
`core/`, `targets/` or `readback/` changed. A module added later (`state/`, N2-13 and N2-34) is covered the same way: a slot that holds resources cannot leak the device.

**Measured on the PC (2026-10-31, the RX 6700 XT, `MH4N_REQUIRE_VULKAN=1 MH4N_VULKAN_DEVICE="RX 6700"`, each executable alone, one process, no GPU reset in the Windows event log and the display adapter `OK` after each):**
`gpu_backend_readback_test` 14 cases passed, 0 failed, 1,150 of 1,150 assertions (it was 7 passed, 6 failed before; the new case, run alone before the release, failed three of its four legs); `gpu_backend_sync_test`
12 passed, 335 of 335 (its first run on the device); `gpu_backend_core_test` 48 passed, 2 skipped, 1,750 of 1,750; `gpu_backend_targets_test` 35 passed, 7,968 of 7,968. On lavapipe the change only frees the devices earlier;
that run is `pending (ci)`.

## Not checked on this machine

`readelf -d` for `libX11` is meaningful on `build/native/bin/rhi_plume_test` (no `NEEDED` entry besides libstdc++, libm, libgcc_s, libc and the loader's), not on `mh4u_native_runner`, which does not link plume yet.

The Windows llvm-mingw build, the macOS build, the three CI jobs and the `native-tsan` job on GitHub are `pending (ci)`; the patterns on the RX 6700 XT, MoltenVK and a second queue are the PC's legs (`rhi-spike.md` 9.2) and were not run: the macOS branch of `plume_vulkan.cpp` (`VK_KHR_portability_enumeration`, the portability-subset features, `triangleFan` forced false) and the Windows branch (`NOMINMAX`) were not compiled here.

## On the RX 6700 XT (N2-31)

**This section is the run before N2-32** (base `6c8f6cd`): its counts and its five failures are of the tests as they were. N2-32 changed the tests (below) and the
new run is `pending (pc)`; no count after N2-32 has been measured. What the section says about the device, the masks and the probe still stands.

**After N2-32 (to be measured on the PC, not seen yet).** The command, for each of `gpu_backend_core_test`, `rhi_plume_test` and `rhi_plume_patterns_test`:
`MH4N_REQUIRE_VULKAN=1 MH4N_VULKAN_DEVICE="RX 6700" <binary>` (without `MH4N_REQUIRE_HEADLESS_SURFACE`). Expected, from reading the code and not from a run: 0 failed
in all three, and `B1_shadow_store_no_interlock` passes and logs how many texels hold 3 (the run before N2-32 saw every texel at 1: none held 3). Each run prints the block
`skipped at run time: <n>` with one line `  <test case>: <why>` per case just before doctest's three summary lines (see the helpers paragraph); that block is what says
what ran. Expected in it: `patch10/dynamic_stencil_state_d24s8` and `patch11/depth_stencil_copy_as_two_regions` (the RX 6700 XT has no D24S8 attachment; the D32S8
half of patch11 still ran), and the headless-surface cases only if the Windows loader still lacks `VK_EXT_headless_surface` (N2-31 saw it lacking: 4 cases,
`device/headless_surface` in two binaries, `instance/the_headless_surface_and_the_debug_messenger_come_when_asked` and
`presentation/with_a_headless_surface_agrees_with_the_probe`). Also printed: `skipped by decorator: <case>` for `loading/child`, the controls (`control/C5`,
`control/B7`, `control/B6`, `control/D4`, `control/D2`) and, on a host that is not Linux, `loading/no_driver_gives_an_error_with_a_message_and_the_process_goes_on`
(registered with `doctest::skip` there). doctest's own summary counts a run-time skip as passed. The doctest counts on lavapipe are core 49 passed and 1 skipped, plume
8 and 1, patterns 23 and 4 (seen); the PC's counts are to be measured (the no-driver case is doctest-skipped on Windows, so core runs one case fewer there).

**Measured on the PC (2026-10-25, the merged tree at `198af96`, the RX 6700 XT, `MH4N_REQUIRE_VULKAN=1 MH4N_VULKAN_DEVICE="RX 6700"`, no
`MH4N_REQUIRE_HEADLESS_SURFACE`):** `gpu_backend_core_test` 48 cases passed, 0 failed, 2 skipped, 1,750 of 1,750 assertions; its `skipped at run time` block names the three
headless-surface cases (the Windows loader still lacks `VK_EXT_headless_surface`) and `loading/no_driver_gives_an_error_with_a_message_and_the_process_goes_on` is skipped
by decorator on Windows. `rhi_plume_test` 8 passed, 0 failed, 1 skipped (`device/headless_surface` at run time), 46 of 46 assertions. `rhi_plume_patterns_test` 23 passed,
0 failed, 4 skipped (the controls), 1,212 of 1,212 assertions; `skipped at run time: 2` names `patch10/dynamic_stencil_state_d24s8` and
`patch11/depth_stencil_copy_as_two_regions` (no D24S8 attachment on this device). So the five failures of the run before N2-32 are gone and the run is clean: 0 failed in the
three binaries. `mh4n_vkprobe --rhi` exits 0 and prints `rhi present_wait: false`, `rhi present_wait_available: false` and `rhi interlock: true` for the AMD proprietary driver, as
the plain probe says (N2-30's PC item).

Run on the maintainer's PC on 2026-10-24, base `6c8f6cd`, no source changed. Build: llvm-mingw 20241217 (`x86_64-w64-mingw32-clang`), Ninja,
`RelWithDebInfo`, `MH4N_WERROR=ON`, the targets `gpu_backend_core_test`, `rhi_plume_test`, `rhi_plume_patterns_test` and `mh4n_vkprobe_tool`
(the executable `mh4n_vkprobe`; the target named `mh4n_vkprobe` is its library). All four build and link on Windows without a warning.
Environment of every run: `MH4N_REQUIRE_VULKAN=1`, `MH4N_VULKAN_DEVICE="RX 6700"`.

- **Device line** (every test log): `device 0 "AMD Radeon RX 6700 XT" driver "AMD proprietary driver" (id 1) api 1.4.315, queue family 0`, chosen because
  its label contains `RX 6700`; discrete, vendor 0x1002, device 0x73df; the probe names driver 26.10.1. It is not llvmpipe (no software device is on the PC).
  The loader is the system's `C:\Windows\System32\vulkan-1.dll`, file version 1.4.309.0.
- **Caps the core logs** (no mask): `d24s8=false interlock=true index_uint8=true custom_border_color=true triangle_fans=true`.

| Binary | Cases passed | Failed | Skipped | Failed or skipped cases |
|---|---|---|---|---|
| `gpu_backend_core_test` | 46 of 49 | 3 | 1 | failed: `instance/the_headless_surface_and_the_debug_messenger_come_when_asked`, `device/headless_surface`, `presentation/with_a_headless_surface_agrees_with_the_probe`; skipped: `loading/child` (the subprocess helper, marked `doctest::skip()`); `loading/no_driver_gives_an_error_with_a_message_and_the_process_goes_on` is counted among the 46 passed but is vacuous here: it ends in a `WARN_MESSAGE` "skipped: the subprocess check runs on Linux" |
| `rhi_plume_test` | 7 of 8 | 1 | 1 | failed: `device/headless_surface`; skipped: the control `control/C5` (controls are skipped by default) |
| `rhi_plume_patterns_test` | 22 of 23 | 1 | 4 | failed: `B1_shadow_store_no_interlock`; skipped: the four controls `control/B7`, `control/B6`, `control/D4` and `control/D2` (all `doctest::skip()`); `patch10/dynamic_stencil_state_d24s8` and `patch11/depth_stencil_copy_as_two_regions` are counted among the 22 passed but make no assertion (message: "skipped: D24S8: the device has no depth-stencil attachment in this format"), so 20 of the 22 exercised the driver |

Assertions: 1,751 of 1,755 (core), 46 of 47 (plume), 1,211 of 1,212 (patterns).

**Masked runs.** Each binary was run under each of the 13 `MH4N_RHI_MASK` bit names (`d24s8`, `interlock`, `index_uint8`, `custom_border_color`,
`triangle_fans`, `logic_op`, `dynamic_rendering`, `pipeline_library`, `eds3_blend`, `bc7`, `timeline`, `present_wait`, `direct_upload`) and under `all`: 42 runs.

| Binary | Result under each of the 14 masks |
|---|---|
| `gpu_backend_core_test` | the same 46 passed, 3 failed, 1 skipped in all 14, the same three failed cases |
| `rhi_plume_test` | the same 7 passed, 1 failed, 1 skipped in all 14 (the core's mask is not read by this binary) |
| `rhi_plume_patterns_test` | the same 22 passed, 1 failed, 4 skipped in all 14 (the core's mask is not read by this binary) |

So no case depends on a masked feature on this device, and none fails because of one: the failures below are the same with and without a mask.

**`mh4n_vkprobe`** (live, exit 0) agrees with `docs/research/vkprobe-results.md` line by line: fragment shader interlock (pixel and sample) `yes`,
`D24_UNORM_S8_UINT` `no` (fallback `D32_SFLOAT_S8_UINT`, `yes`), BC7 `yes`, custom border colours `yes`, timeline semaphores `yes`, dynamic rendering `yes`,
`VK_KHR_present_id` and `VK_KHR_present_wait` `no`, extended dynamic state 3 `yes` (18 of 31 bits), triangle fans `yes`. On lavapipe `d24s8` is true and the
core's `present_wait` is absent as here; the one difference that reaches the tests is `d24s8`.

**The Khronos validation layer is not on this PC.** The loader's layer search (`VK_LOADER_DEBUG=layer`) finds only the implicit layers
`SteamOverlayVulkanLayer64.json` and `SteamFossilizeVulkanLayer64.json` (registry `HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers`); there is no explicit-layer
registry key and no `VK_SDK_PATH`. `VK_LAYER_KHRONOS_validation` is not listed, so `tools/native/vk_layer_run.py` was not run (nothing was downloaded).

### Failures and their classes

1. **`device/headless_surface` (both binaries), `instance/the_headless_surface_and_the_debug_messenger_come_when_asked` and
   `presentation/with_a_headless_surface_agrees_with_the_probe`: test assumption.** First messages: `FATAL ERROR: MH4N_REQUIRE_VULKAN=1 and there is no
   usable Vulkan device: the Vulkan loader lacks VK_EXT_headless_surface` (`support/vulkan_device.h:43`; `rhi_plume_test.cpp:351`: "the loader does not offer
   VK_EXT_headless_surface"), and `CHECK( Has(names, "VK_EXT_headless_surface") )` false at `gpu_backend_core_test.cpp:834`, then line 837. The cases assume the
   loader lists `VK_EXT_headless_surface`, which lavapipe's stack does; this PC's loader with AMD's driver does not list it. No Vulkan call is invalid. The
   surface the swapchain cases (N2-27, N2b-06) need must on this PC come from a real window (`VK_KHR_win32_surface`); that is a design question for the
   orchestrator, not a driver defect. The cases that need a surface did not run here (the presentation half of `device/chosen`, the present-capable family
   filter): they are still the PC's legs.
2. **`B1_shadow_store_no_interlock`: test assumption (a result the shader does not guarantee).** Assertion `CHECK( bad == 0 )` at
   `rhi_plume_patterns_test.cpp:574`, first message `B1: 256 of 256 texels != 3 (texel0 = 1)`. B1 is the shadow write with `imageLoad` plus `imageStore` and no
   interlock, three overlapping draws that each add 1, with no barrier; without the interlock the order of the read-modify-write between overlapping fragments
   is undefined. This run's inference (not something 6.9 states): the GPU ran the overlapping draws without ordering their read-modify-writes (every
   texel 1, not 3), where lavapipe's serial execution gives 3; the evidence is B1 = 1 per texel against B5, which has a barrier, = 3. `rhi-spike.md` 6.9 lists
   the patched B1 as "clean; right" on lavapipe and says silence on B1, B3 and B5 says nothing about their ordering; it called the ordering of the interlock
   variants on a real GPU unverified, and B2 and B4 now give 0 of 256 wrong on the RX 6700 XT (so do B3 and B5). Not a trigger of D4: the interlock the plan uses is `yes` here and `B2` passes. (The case asserts a result the shader
   does not guarantee; whether it should assert the opposite on a real GPU, or skip, is the orchestrator's.)
3. **The two D24S8 cases are not failures** and are not the trigger: the device has no `D24_UNORM_S8_UINT` attachment, as the probe says; the D32S8 path
   (`d24s8` masked) is the PC's path and these two cases only assert where the device has D24S8 (lavapipe). Under `MH4N_REQUIRE_VULKAN=1` they log "skipped" by their own message and are counted as passed (no assertion), instead of failing.

**Verdict:** 75 of the 80 cases that ran pass on the RX 6700 XT; the 5 failures (3 + 1 + 1 over the three binaries, 2 distinct causes: the missing `VK_EXT_headless_surface` and B1) are all of the class "test assumption" about lavapipe, none a backend or patch defect and none a driver difference that the patches and the owner rule cannot repair, so this run does not fire D4's second trigger. `D1`, `D3`, `D3b` (the owner-rule
threading cases of the patterns: 0 layout overwrites and 0 broken chains over 2,000 rounds, both) pass on the AMD driver.
