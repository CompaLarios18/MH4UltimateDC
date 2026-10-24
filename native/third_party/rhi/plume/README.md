# plume, the Vulkan backend (vendored, N2-25)

[plume](https://github.com/renderbag/plume) is a render interface over Direct3D 12, Vulkan and Metal
("Copyright (c) 2024 renderbag and contributors", MIT; the licence is `pristine/LICENSE`). This folder
holds its Vulkan backend as a pristine copy plus numbered patches of ours (decision D4, option 2b;
`docs/research/rhi-spike.md`). The manual, with the regime and how to add a patch, is
[`docs/native/rhi.md`](../../../../docs/native/rhi.md).

**Agents never contact the plume project** (no issue, pull request, comment or message of any kind): its
`CONTRIBUTING.md` forbids AI-assisted contributions and AI-assisted communication with its maintainers.
Every change to plume's code is a numbered patch file of ours.

Pinned commit: `0ce184b19270e49236afbb74052a6b267494b86b`
(`git clone https://github.com/renderbag/plume`, `git checkout` that commit; "Support texture readback
through copyTextureRegion (#114)").

## Layout

| Folder | What |
|---|---|
| `pristine/` | plume's files at the pinned commit, unmodified, at their upstream paths. The trim is by file: the D3D12 and Metal sources, `metal-cpp`, D3D12MA, the examples, `plume_apple.*` and plume's `contrib/` Vulkan-Headers are not vendored. |
| `patches/` | `NN-<name>.diff`, unified diffs on the pristine paths, applied in number order. |
| `patched/` | `pristine/` with every patch applied: what the build compiles. Written by `tools/native/rhi_patches.py apply` and committed, so a build needs no `git` or `patch`. `patched/mh4n_patch_list.h` holds the pinned commit and the patch file names as string constants (`mh4n::gpu::backend::plume::VendorInfo()` returns them). |

## Pristine files and their git blob ids

The blob id is what `git rev-parse 0ce184b:<file>` gives in plume's repository; `tools/native/rhi_patches.py blobs`
recomputes it for each file here. A row that does not match means the pristine copy was edited: undo the edit and
write a patch instead.

| File | Blob id |
|---|---|
| `LICENSE` | `b05f2417451261d8c8bfa2dc543981c32228e25d` |
| `plume_render_interface.h` | `20485570540adaf0d97c50a67c8acfb13cfc8645` |
| `plume_render_interface_builders.h` | `acfedcea9ed32269cd631475a5f45ceafc743b4d` |
| `plume_render_interface_types.h` | `ad63e0ee5ed9095889aa8c2252d956575fc847a6` |
| `plume_vulkan.cpp` | `4fdc4bfd3482a9306d36c523a4052c2e4afd8ca0` |
| `plume_vulkan.h` | `1d36b12cfd4addd918f0b5194c9c5639873cd668` |

## The patches

Patches 01 to 07 are the files of `docs/research/rhi-spike-evidence/patches/`, byte for byte (their sha256 is in
that folder's `SHA256SUMS`); they were made one on top of another (01, 02, 04 and 03, 05, 06, 07 chains), so
they are applied in number order only. 08 to 11 were written for this repository. The changed-line figures
are `git diff --numstat` (added plus removed).

| No. | File | What | Lines |
|---|---|---|---:|
| 01 | `01-p7-mutable-format-zero-attachment.diff` | `MUTABLE_FORMAT` texture flag; framebuffer width and height without attachments; viewport and scissor count at least 1 | 15 |
| 02 | `02-p2-fragment-shader-interlock.diff` | `VK_EXT_fragment_shader_interlock`: optional extension, feature struct in both chains, capability | 19 |
| 03 | `03-p3-per-wait-stage-mask.diff` | one stage mask per wait semaphore (`executeCommandListsEx`) | 17 |
| 04 | `04-p7b-general-attachment-layout.diff` | framebuffer flag `colorAttachmentsGeneralLayout`: colour attachments stay in `GENERAL` (depth attachments untouched) | 9 |
| 05 | `05-p3b-timeline-semaphores.diff` | timeline semaphores: feature, capability, `createTimelineSemaphore`, submit info with wait and signal values | 63 |
| 06 | `06-p4-pipeline-cache-renderpass-cache.diff` | one `VkPipelineCache` per device with load and save; a render-pass cache keyed by formats | 76 |
| 07 | `07-p5-blend-constants.diff` | blend constants dynamic (`dynamicBlendConstantsEnabled`, `setBlendConstants`) | 12 |
| 08 | `08-surfaces-and-device-requirements.diff` | `PLUME_EXTERNAL_SURFACE_ONLY` compiles out plume's window surfaces and window types (no X11, no `VK_USE_PLATFORM_*`); `VulkanInterfaceOptions` (extra instance extensions, `headlessSurface`, `debugUtils`, `validationLayer`, an error string) and `CreateVulkanInterface(options)`; nothing is required of the instance; `VulkanDeviceExtras` with a `VkSurfaceKHR` (`VK_KHR_swapchain` only with it, the direct queue family chosen among those that can present to it) and `createDeviceEx`; `enabledExtensionNames` on `VulkanInterface` and `VulkanDevice`; failure text through an out parameter | 260 |
| 09 | `09-device-and-enabled-features.diff` | `VulkanDeviceExtras::physicalDeviceIndex`, `coreFeatures`, `optionalExtensions`, `timelineSemaphore`; ray tracing, acceleration structure, pipeline library and deferred host operations leave the default list; the presentation extensions only with a surface; `VK_EXT_index_type_uint8` and `VK_EXT_custom_border_color` with their feature structs and capability bits; `enabledCoreFeatures`; `robustBufferAccess` is always enabled together with robustness2's `robustBufferAccess2` (VUID 04000), whatever `coreFeatures` lists | 167 |
| 10 | `10-dynamic-stencil-state.diff` | pipeline flag `dynamicStencilEnabled` (stencil reference, compare mask and write mask dynamic); command-list setters `setStencilReference`, `setStencilCompareMask`, `setStencilWriteMask` | 27 |
| 11 | `11-formats-and-format-query.diff` | `D24_UNORM_S8_UINT`; the 16-bit colour formats in both component orders; `RenderDevice::getFormatSupport` (attachment, sampled, storage, blit); buffer copies of depth-stencil images as two regions (one aspect each) | 106 |
| 12 | `12-custom-border-colour.diff` | `RenderBorderColor::CUSTOM` and `RenderSamplerDesc::customBorderColor` (RGBA floats): the sampler is created with `VK_BORDER_COLOR_FLOAT_CUSTOM_EXT` and a `VkSamplerCustomBorderColorCreateInfoEXT` (format undefined) only when the device has the capability, otherwise `createSampler` prints a message and returns a sampler with a null handle; `capabilities.customBorderColor` now also needs `customBorderColorWithoutFormat` | 21 |

`RenderTextureLayout::PRESENT` (`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`) exists at the pinned commit
(`plume_render_interface_types.h`, `toImageLayout` in `plume_vulkan.cpp`), so no patch adds it.

## Adding a patch

A change to plume's code is a new numbered file after the last one, never an edit of an earlier file or of
`patched/` by hand:

1. start from `patched/` (all patches applied), edit a copy, and write the unified diff of your edit on the
   pristine paths (`git diff` in a scratch repository that holds `patched/` works; paths are `a/<file>`,
   `b/<file>`);
2. save it as `patches/NN-<name>.diff` with the next number;
3. list it in the table above (one line);
4. `python3 tools/native/rhi_patches.py apply`, then `check` and `blobs`;
5. build and run the RHI tests (`docs/native/rhi.md`).

An upstream fix is cherry-picked by us as a new numbered patch, not by changing the pinned commit.
