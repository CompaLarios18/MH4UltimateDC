# RHI spike: the evidence for D4 (render interface and backends)

## 0. Header

What this is: the evidence that settles [D4](../native/DECISIONS.md) (the render hardware interface and its backends) and a recommendation, the deliverable of N2-01 ([`N2a.md`](../native/wp/N2a.md)). It covers what the three target devices offer (RX 6700 XT, Apple M2, lavapipe), what the renderer asks of its graphics layer (read from Azahar's Vulkan renderer), how plume meets that, and what each of the three options costs.

- **Status:** `review`. D4 waits for Luis (the maintainer) and for his run of `mh4n_vkprobe` on his Mac ([3.4](#34-the-m2-pending-luiss-run)). The planner reviews the recommendation; Luis decides.
- **Date:** 2026-10-23. **Written by:** the cloud orchestrator's N2-01 session. Four research notes (lavapipe and the PC, plume, Azahar, the M2) were written first; this document turns them into the deliverable. Every claim carried over was either re-read in the source named next to it or is labelled; the re-checks that caught errors are in [11](#11-corrections-to-earlier-documents-found-while-checking) and [12](#12-what-could-not-be-checked).
- **The census.** `pica-census.md` (N0-13) is not on this branch. This document cites it by name and uses only these numbers: four scenes (title, character creation, one Arena fight, one gas cutscene), 3, 3, 10 and 10 frames. Every table about the census says *seen in those scenes* or *not seen in those scenes*, never "never".

**Labels.** Each point says where it comes from.

| Label | Meaning |
|---|---|
| `source: <ref>` | read in the named source in this work (2026-10-23) |
| `inference` | concluded from what was read, not run |
| `unverified` | could not be checked; the text says what would check it |
| `memory` | what the writer recalls about a library or a spec, not read here |
| `stand-in` | a number from another person's M2, from a published report, MoltenVK's source or Apple's tables |
| `pending Luis's run` | a number only the maintainer's own Mac can give |

**Reference forms.** `plume@0ce184b plume_vulkan.cpp:1234` (a file of that repository); `azahar@5f3306d renderer_vulkan/vk_instance.cpp:471` (paths under `src/video_core/` unless they start with `src/`, `externals/`, `CMake` or `.ci/`); `MoltenVK@v1.4.2 path:line` (`MVKDevice.mm` is `MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm`); `Mesa@f3f63ef path`; `report 52371`; `FST p.N` (Apple's Metal Feature Set Tables, page N of the 8-page file); `MSL p.N` (page of the footer "Page N of 383"); `vkprobe-pc.json features[id]`; `needs.md` is the orchestrator's list of needs for this spike (not in the repository), reproduced in [2](#2-what-the-renderer-needs).

**The versions every number comes from**

| What | Version | Note |
|---|---|---|
| plume | `0ce184b19270e49236afbb74052a6b267494b86b` (2026-10-21, 76 commits, no tags) | https://github.com/renderbag/plume |
| Azahar | `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` (2026-10-10) | the commit [`metal.md`](metal.md) pins; a one-commit checkout |
| MoltenVK | `v1.4.2` (released 2026-07-20, merge commit `db66022`), `v1.4.1` (`db445ff`) for reference, `main` at `5189f8d` (2026-10-13) | https://github.com/KhronosGroup/MoltenVK |
| SPIRV-Cross | `6c09849` (MoltenVK 1.4.2's pin), `aa217ae` (main's pin) | https://github.com/KhronosGroup/SPIRV-Cross |
| Mesa (KosmicKrisp) | `f3f63ef` (2026-10-16) | https://gitlab.freedesktop.org/mesa/mesa.git |
| gpuinfo reports | **52371**: Apple M2, MoltenVK 1.4.2, macOS 26.6.2, Vulkan 1.4.357, submitted 2026-10-07 21:50:02. **52372**: Apple M2, KosmicKrisp `vulkan-sdk-1.4.363`, macOS 26.6.2, submitted 2026-10-07 21:52:00. 51106 (MoltenVK 1.4.1) and 52375 (KosmicKrisp, macOS 27.0.1) only where named | https://vulkan.gpuinfo.org/displayreport.php?id=N |
| Apple | Metal Feature Set Tables dated 2026-05-21; Metal Shading Language Specification 4.1 (created 2026-06-04) | retrieved 2026-10-23 |
| lavapipe | Mesa 25.2.8-0ubuntu0.24.04.4, LLVM 20.1.2, device Vulkan 1.4.318, loader 1.3.275 | the cloud machine |
| RX 6700 XT | `docs/research/vkprobe-pc.json`, committed in `ba1ba93` | [3.2](#32-rx-6700-xt-windows-amds-own-driver) |
| the probe | `mh4n_vkprobe` built from this repository at `69dd2c2` | `build/native/bin/mh4n_vkprobe`; the probe's source is unchanged in the next commit |
| 3dbrew | `GPU/Internal_Registers`, "last edited on 18 October 2026" | retrieved 2026-10-23 |

## 1. Summary

What each device offers, by the probe (`yes` means the extension is listed *and* the bit is set; M2 rows are `stand-in`):

| Device | Offers | Lacks |
|---|---|---|
| RX 6700 XT, AMD's own driver 26.10.1 | everything the probe asks for, **pixel and sample interlock**, geometry shaders, barycentrics, fans, logic ops | `D24_UNORM_S8_UINT` (use `D32_SFLOAT_S8_UINT`), rasterization-order access, `present_id`/`present_wait` |
| M2, MoltenVK 1.4.2 | interlock (raster order groups), fans (emulated), 8-bit indices, BC7, dynamic rendering | `logicOp`, geometry shaders, custom border colours, D24, graphics pipeline library; barycentrics are listed but SPIRV-Cross cannot translate them; `present_wait` returns at command-buffer completion, not at display |
| M2, KosmicKrisp | `logicOp`; needs macOS 26 | **no interlock**, no barycentrics, no geometry shaders, no D24 |
| M2, native Metal (Apple8) | framebuffer fetch, raster order groups | 24-bit depth, logic-op API, fans, 8-bit indices, geometry stage |
| lavapipe | all of it except barycentrics and `present_id`/`present_wait` | reports *more* than the PC (D24S8, rasterization-order access), so it hides the PC's fallbacks |

- **What plume lacks for MH4U:** no interlock and no image `MUTABLE_FORMAT`, so the shadow write (seen in those scenes) has no valid path; render-pass layouts fixed at `COLOR_ATTACHMENT_OPTIMAL`; no `VkPipelineCache`; no blend-constant setter; binary semaphores only (no timeline); no format-support query; no D24S8 or narrow colour formats in `RenderFormat`; a swapchain that cannot take an external surface; Metal takes only a compiled metallib. About 500 Vulkan lines of patches (inference). The README says barrier management and texture transitions are still being refined.

| Option | Meets the needs seen in those scenes | Cost (inference) |
|---|---|---|
| 1 plume as it is | no: shadow write fails, 700 to 900 lines of workarounds | N2-21 stays M, with a degraded renderer |
| 2a plume extended, all backends kept | yes, by about 500 Vulkan lines of patches plus 300 to keep D3D12 and Metal compiling | about S for patches, then N2-21 as M; a permanent fork (upstream refuses AI-assisted patches) |
| 2b plume's Vulkan backend vendored and trimmed | as 2a, without D3D12 and Metal | about M for trim, patches and tests; N2-21 shrinks |
| 3 our own thin RHI over Vulkan | yes, by design | about L for the RHI (or N2-21 grown to L), about one to two M more than 2b |

- **Recommendation: option 3**, with plume (MIT) as a second reference beside Azahar. The evidence rejects option 1, collapses 2a into 2b (D3D12 is not needed and plume's Metal backend has nothing we need), and leaves 2b against 3 as a close call on cost that the stability criterion of D56 tips to 3: the gaps in plume sit in barriers, layouts, render passes and device creation, which is where a PICA renderer works hardest.
- **What would flip it to 2b:** a validation-clean run of plume with its patches on lavapipe and the RX 6700 XT, patches plus rework at or under 600 lines, or an estimate for our own RHI above about 1.5M tokens ([9](#9-recommendation)).
- **Pending:** Luis's Mac run ([3.4](#34-the-m2-pending-luiss-run)); more captured scenes (fans, logic ops, 8-bit indices and the other needs not seen in those scenes); the gas mode (N2-10); a read of `plume_metal.cpp` beyond grep; the `vulkaninfo` file the PC results say they compared with is not in the repository.

## 2. What the renderer needs

The needs list is the orchestrator's (`needs.md`, written 2026-10-23), keyed by the IDs below. *Seen* means seen in the four census scenes (title, character creation, one Arena fight, one gas cutscene; 3, 3, 10 and 10 frames, counted independently twice). `pica-census.md`'s findings used here: geometry shaders are used in Point mode in 39 to 90 percent of the draws and about 1 percent of the vertices; `D24S8` is the only depth format seen; shadow passes with a Shadow2D texture and a white border colour; fog; lighting Config0 and Config1 only; one procedural-texture configuration, always inside the geometry-shader sprite pass; gas in the cutscene only.

| ID | Need | Seen in those scenes? | What the RHI must allow |
|---|---|---|---|
| G1 | No GPU geometry stage: the PICA geometry shader runs on the CPU, or its points are expanded in a vertex stage | seen | streamed dynamic vertex/index buffers; vertex pulling (storage-buffer reads in a vertex shader) if the points are expanded on the GPU |
| G2 | Lit draws' per-triangle quaternion fix-up | seen (lighting Config0/1) | `geometryShader` or fragment barycentrics; else skip the fix-up |
| G3 | `D24S8` depth | seen (the only depth format) | D24S8 where it exists, else `D32_SFLOAT_S8_UINT` with conversions; depth/stencil readback into the PICA layout; depth read as colour for transfers |
| G4 | Shadow passes: write path and Shadow2D lookup with a white border | seen | storage-image writes from a fragment shader on an `R32_UINT` view of the colour target, ordered by pixel interlock, else a compare-and-swap loop; the lookup |
| G5 | Fog and lighting lookup tables | seen | texel buffers, 1D textures or uniform arrays |
| G6 | Procedural textures | seen (one configuration) | tables plus shader maths |
| G7 | Gas | seen (cutscene only) | open (N2-10): a density pass and a shading pass; if the mode reads the destination colour, framebuffer fetch, raster-order access, interlock or copy-and-barrier |
| G8 | Plain draw state: blend (equations, factors, constant colour), alpha test (in the shader), stencil, depth compare, write masks, scissor, viewport, culling, depth bias; up to 12 attributes in several vertex buffers; 16-bit indices | seen | all of it, with the state that changes per draw dynamic or cheap |
| G9 | Render-target and texture formats: colour RGBA8, RGB8, RGB5A1, RGB565, RGBA4; depth D16, D24, D24S8; textures decoded on the CPU into RGBA8 and friends | seen | the formats, or a fallback per format |
| G10 | Pipelines compiled off the draw thread (N2-16) | project need (principle 5), not a census item | a `VkPipelineCache` blob on disk, worker threads, an ubershader while one compiles; graphics pipeline library, dynamic rendering and extended dynamic state help |
| G11 | BC7 for the HD pack (N2-17, N2-18) | project need, not a census item | BC7 sampled in optimal tiling; uploads on a worker thread |
| N1 | All 16 logic ops without blending | **not seen** | hardware `logicOp`, else shader emulation (Azahar's SPIR-V generator does 4 of 16); on Metal, framebuffer fetch |
| N2 | Triangle fans | **not seen** | native topology, else rewrite as an indexed list (N2-05) |
| N3 | Min/max blending | **not seen** | Vulkan `MIN`/`MAX` ignore blend factors, so a shader path unless both factors are One |
| N4 | 8-bit indices | **not seen** | `VK_EXT_index_type_uint8`, else widen to 16 bits (N2-05) |
| N5 | Vertex strides that are not a multiple of 4 | **not seen** | repack on the CPU where the device asks for alignment |
| N6 | Cube and projection textures | **not seen** | cube images and cube shadow lookups; projective sampling is shader maths |
| R1 | Instance apart from the device; surface; headless | app need (N2-21) | `CreateInstance(extra_extensions)`, `CreateBackend(instance, surface)`, `VK_EXT_headless_surface`, a queue family that can present |
| R2 | One device shared by several threads (D56) | app need | one submit lock; each submission returns a timeline value another submitter can wait on |
| R3 | Presentation timing (D57) | app need | `VK_KHR_present_id` + `present_wait` when the device has them; `PresentWaitAvailable()`; FIFO / MAILBOX / IMMEDIATE, resize, minimise |
| R4 | Raw handles for Dear ImGui | app need | instance, physical device, device, queue family, queue and the recording `VkCommandBuffer` readable |
| R5 | Readback and timestamps | app need | `ReadImage`; write-back into guest memory in the PICA's Morton-tiled layout; GPU timestamps per frame |
| R6 | Vulkan loaded at run time (volk) | rule of `N1.md` | no driver: creating the backend fails with a message, nothing else does |
| R7 | Runs on lavapipe | rule of N2a | every N2 check runs in the cloud on lavapipe |
| R8 | Shader input | D27, D5 | SPIR-V from glslang and from our fragment generator; for Metal, SPIR-V to MSL through SPIRV-Cross |
| R9 | Metal-only features (the rule of [`metal.md`](metal.md)) | rule | framebuffer fetch and raster order groups are native-Metal items unless the Vulkan-on-Metal driver already gives them: [7](#7-metal) |

**Needs the notes added** (renumbered here; the notes numbered theirs separately: plume's X1 is X1; Azahar's X1 to X5 are X2 to X6; the M2 note's X1 to X3 are X7 to X9; the M2 note's X4, the Vulkan-on-Metal driver choice, is not a need but a fact of the target matrix, [7](#7-metal)):

| ID | Need | Why it is needed |
|---|---|---|
| X1 | A format-support query: attachment / sampled / storage / blit per format | G3's D24S8-else-D32S8, G9's narrow formats, G11's BC7 and N1's fallback depend on it; plume answers none of them (`source: plume@0ce184b`, no call of `vkGetPhysicalDeviceFormatProperties` in `plume_vulkan.cpp`) |
| X2 | The fragment shader writes depth in every non-shadow draw (`gl_FragDepth`), so no early-Z for PICA draws | `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:186, 199-216` |
| X3 | Vertex attribute formats `*_SSCALED`/`*_USCALED` (8 and 16 bits, 1 to 4 components) with an integer-format fallback and shader conversion, and 3-component aliasing | `source: azahar@5f3306d renderer_vulkan/vk_instance.cpp:72-97, 350-395` |
| X4 | A render pass with zero attachments and a storage-image write target (the shadow write) | `source: azahar@5f3306d renderer_vulkan/vk_texture_runtime.cpp:1491-1499` |
| X5 | Three dynamic-offset uniform-buffer descriptors over one ring buffer and a fresh texture set per draw | `source: azahar@5f3306d renderer_vulkan/vk_pipeline_cache.cpp:59-83` |
| X6 | "Copy the bound colour target to a sampled twin between draws" as an operation: pass break, copy, resume with `LOAD` | min/max emulation (N3) and any sampled render target; `source: azahar@5f3306d renderer_vulkan/vk_texture_runtime.cpp:1197-1299`, `vk_rasterizer.cpp:718-732` |
| X7 | Primitive restart is always on under MoltenVK | an index of 0xFFFF in a non-strip draw would read as a restart; `source: MoltenVK@v1.4.2 Docs/MoltenVK_Runtime_UserGuide.md:345`; whether the PICA can submit such indices is `unverified` |
| X8 | Blend state cannot be dynamic on MoltenVK (`extendedDynamicState3ColorBlendEnable`, `ColorBlendEquation`, `ColorWriteMask`, `LogicOpEnable` false) | each blend configuration is a pipeline; G10's count depends on it; `source: report 52371`, `MoltenVK@v1.4.2 MVKDevice.mm:699-729` |
| X9 | No MAILBOX present mode on MoltenVK (FIFO and IMMEDIATE, 2 to 3 images) | R3 lists MAILBOX; `source: report 52371` (surface tab), `MoltenVK@v1.4.2 MVKDevice.mm:2151-2154` |

**What a later capture could change.** The census is four scenes of 3, 3, 10 and 10 frames.
- Every *not seen in those scenes* row (N1 to N6) may become *seen* in a quest, a village, a gathering hub or another cutscene. On the M2 without a native backend, only N1 would then be missing on MoltenVK 1.4.x (fans, 8-bit indices and odd strides are present there, [7](#7-metal)); on the PC nothing is missing.
- G7 is open: only one scene had gas, and neither Azahar nor 3dbrew gives the per-fragment formula. A capture of more gas frames, or hardware tests, decides whether the mode reads the destination colour (and so whether G7 needs framebuffer fetch, interlock or a copy per draw).
- Counts the notes could not give: how many hardware-vertex-shader lit draws there are (the only ones that need G2's barycentrics or fixed geometry shader; the geometry-shader draws are CPU draws on every host); how many draws sample their own render target or need min/max emulation (pass breaks plus full-image copies); how often a constant blend colour, a changing stencil reference or an RGB565/RGB5A1/RGBA4 render target occurs (these decide how much plume's baked-in state costs); how many distinct fragment-state keys a play session makes (N2-07).
- G4 is seen, so its answer is not at risk; what a capture could change is which of its paths (interlock or compare-and-swap) matters on which host.

## 3. The devices

The probe is `mh4n_vkprobe` ([`vkprobe.md`](vkprobe.md)): one line per feature, `yes` only when the device lists the extension *and* reports the bit, `unknown` when the device or file does not say. It reports a device's capability, not proof that a code path using it works.

### 3.1 lavapipe

The CPU Vulkan driver the cloud CI runs everything on.

Commands (all exit status 0):

```
build/native/bin/mh4n_vkprobe                      > lavapipe-probe.txt
build/native/bin/mh4n_vkprobe --json               > lavapipe-probe.json
vulkaninfo --summary ; vulkaninfo ; vulkaninfo --json     # writes VP_VULKANINFO_llvmpipe_(LLVM_20_1_2,_256_bits)_25_2_8.json
build/native/bin/mh4n_vkprobe --from-json VP_VULKANINFO_llvmpipe_*.json
```

Versions (`source: vulkaninfo --summary`, `dpkg -l`): device `llvmpipe (LLVM 20.1.2, 256 bits)`, `PHYSICAL_DEVICE_TYPE_CPU`, vendorID 0x10005, `apiVersion` 1.4.318, driver `Mesa 25.2.8-0ubuntu0.24.04.4 (LLVM 20.1.2)`, `driverID` MESA_LLVMPIPE, `conformanceVersion` 1.3.1.1. The loader is 1.3.275 (`libvulkan1 1.3.275.0-1build1`), so the device reports a higher version than the loader's instance version. One device. Instance layers: `VK_LAYER_INTEL_nullhw`, `VK_LAYER_MESA_device_select`, `VK_LAYER_MESA_overlay`; no validation layer.

The probe's table, verbatim (`source: lavapipe-probe.txt`, the output of the first command):

```
device: llvmpipe (LLVM 20.1.2, 256 bits)
  type: CPU (vendor 0x10005, device 0x0000)
  driver: llvmpipe Mesa 25.2.8-0ubuntu0.24.04.4 (LLVM 20.1.2)
  api version: 1.4.318
  source: live
  features:
    logic ops (logicOp)                                 yes
    fragment shader interlock, pixel                    yes
    fragment shader interlock, sample                   yes
    storage image writes from fragment shaders          yes
    geometry shaders                                    yes
    fragment shader barycentrics                        no (fallback: geometry shaders for the quaternion fix-up, else skip it)  [VK_KHR_fragment_shader_barycentric is not on the device]
    triangle fans                                       yes  [a full Vulkan device: fans are core]
    8-bit indices (VK_EXT_index_type_uint8)             yes
    custom border colours (VK_EXT_custom_border_color)  yes
    shaderClipDistance                                  yes
    VK_EXT_shader_stencil_export                        yes
    VK_EXT_rasterization_order_attachment_access        yes
    depth-stencil D24_UNORM_S8_UINT                     yes
    depth-stencil D32_SFLOAT_S8_UINT                    yes
    BC7 textures (BC7_UNORM_BLOCK)                      yes
    dynamic rendering                                   yes
    VK_EXT_graphics_pipeline_library                    yes
    VK_KHR_present_id (presentId)                       no (fallback: estimate when a frame is shown from the present call (not measured))  [VK_KHR_present_id is not on the device; device level: a swapchain's support needs a surface]
    VK_KHR_present_wait (presentWait)                   no (fallback: estimate when a frame is shown from the present call (not measured))  [VK_KHR_present_wait is not on the device; device level: a swapchain's support needs a surface]
    push descriptors (VK_KHR_push_descriptor)           yes
    timeline semaphores                                 yes
    VK_EXT_extended_dynamic_state (1)                   yes
    VK_EXT_extended_dynamic_state2                      yes
    VK_EXT_extended_dynamic_state3                      yes  [16 of 31 bits (see the device's extendedDynamicState3* features)]
```

`--from-json` of `vulkaninfo`'s own file gives the same table; `diff` shows one line, `source: live` against `source: vulkaninfo` (all 24 feature lines, the `[why]` texts and "16 of 31 bits" identical). The file has 161 device extensions and 155 formats, `D24_UNORM_S8_UINT` among them, so no line fell back to "not listed in the file". This agrees with [`vkprobe.md`](vkprobe.md) (`--from-json`).

The facts that matter (`source: vulkaninfo` full output, 1301 lines):
- **One memory type.** One heap of 15.71 GiB, `DEVICE_LOCAL`, and one memory type with `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`. Staging and device-local memory are the same memory, so the split of a discrete GPU (and the staging path that follows from it) is not exercised.
- **One queue.** One queue family, `queueCount = 1`, graphics, compute, transfer and sparse binding, 64 timestamp bits, `timestampPeriod = 1`. R2's shared queue is the only case that can be tested; a transfer-only family (which AMD drivers expose, `memory`) cannot.
- **No present.** `present support = false` in `vulkaninfo`'s queue listing, from a run with no display (`'DISPLAY' environment variable not set`): presentation is not tested, and a swapchain was never created. `VK_KHR_swapchain` (revision 70) and `VK_EXT_swapchain_maintenance1` are listed.
- **The headless surface is listed.** `VK_EXT_headless_surface` (revision 1) is among the 24 instance extensions, with `VK_KHR_xcb_surface`, `VK_KHR_xlib_surface` and `VK_KHR_wayland_surface`. That is what N2-21's headless test and N2b-06's swapchain test rely on.
- **Not in the device extension list** (a count of 0 in `vulkaninfo`): any `barycentric`, `present_id`, `present_wait`, `present_timing` or `display_timing`. `VK_EXT_fragment_shader_interlock` (pixel and sample true, shading-rate false), `VK_KHR_timeline_semaphore`, `VK_EXT_graphics_pipeline_library` (fast linking true), `VK_EXT_custom_border_color`, `VK_EXT_index_type_uint8`, `VK_KHR_dynamic_rendering`, `VK_EXT_rasterization_order_attachment_access` (and the ARM name) and `VK_KHR_push_descriptor` are listed. Core features true: `logicOp`, `geometryShader`, `fragmentStoresAndAtomics`, `vertexPipelineStoresAndAtomics`, `textureCompressionBC`, `independentBlend`, `dualSrcBlend`.
- **Limits:** `maxImageDimension2D` 16384, `maxPushConstantsSize` 256, `maxBoundDescriptorSets` 8, `maxColorAttachments` 8, `maxVertexInputBindings` 32, `maxVertexInputBindingStride` 2048, `minUniformBufferOffsetAlignment` 16, `maxStorageBufferRange` 134217728, `maxTexelBufferElements` 134217728, `nonCoherentAtomSize` 64.

### 3.2 RX 6700 XT (Windows, AMD's own driver)

The reference PC. The probe's table, verbatim (`source: vkprobe-pc.json`, read back with `mh4n_vkprobe --from-json docs/research/vkprobe-pc.json`, exit 0; the table equals the JSON's content line for line; the reader prints `source: live` because the file is the probe's own `--json`):

```
device: AMD Radeon RX 6700 XT
  type: discrete GPU (vendor 0x1002, device 0x73df)
  driver: AMD proprietary driver 26.10.1 (AMD proprietary shader compiler)
  api version: 1.4.315
  source: live
  features:
    logic ops (logicOp)                                 yes
    fragment shader interlock, pixel                    yes
    fragment shader interlock, sample                   yes
    storage image writes from fragment shaders          yes
    geometry shaders                                    yes
    fragment shader barycentrics                        yes
    triangle fans                                       yes  [a full Vulkan device: fans are core]
    8-bit indices (VK_EXT_index_type_uint8)             yes
    custom border colours (VK_EXT_custom_border_color)  yes
    shaderClipDistance                                  yes
    VK_EXT_shader_stencil_export                        yes
    VK_EXT_rasterization_order_attachment_access        no (fallback: fragment shader interlock, else a copy and a barrier per draw)  [VK_EXT_rasterization_order_attachment_access is not on the device]
    depth-stencil D24_UNORM_S8_UINT                     no (fallback: D32_SFLOAT_S8_UINT)  [D32_SFLOAT_S8_UINT is available]
    depth-stencil D32_SFLOAT_S8_UINT                    yes
    BC7 textures (BC7_UNORM_BLOCK)                      yes
    dynamic rendering                                   yes
    VK_EXT_graphics_pipeline_library                    yes
    VK_KHR_present_id (presentId)                       no (fallback: estimate when a frame is shown from the present call (not measured))  [VK_KHR_present_id is not on the device; device level: a swapchain's support needs a surface]
    VK_KHR_present_wait (presentWait)                   no (fallback: estimate when a frame is shown from the present call (not measured))  [VK_KHR_present_wait is not on the device; device level: a swapchain's support needs a surface]
    push descriptors (VK_KHR_push_descriptor)           yes
    timeline semaphores                                 yes
    VK_EXT_extended_dynamic_state (1)                   yes
    VK_EXT_extended_dynamic_state2                      yes
    VK_EXT_extended_dynamic_state3                      yes  [18 of 31 bits (see the device's extendedDynamicState3* features)]
```

- **The driver is AMD's, not RADV.** The file's `driver` field reads "AMD proprietary driver 26.10.1 (AMD proprietary shader compiler)", API 1.4.315, vendor 0x1002, device 0x73df (`source: vkprobe-pc.json device`). RADV is Mesa's driver and reports a Mesa driver name (`memory`). D4's row cites RADV since Mesa 23.2 and AMDVLK since 2024.Q4.1 as the places AMD interlock was known; the Windows driver is neither, and the spike was to find out what it exposes (N2-01's trap).
- **The strongest statement about interlock the evidence allows.** `source: vkprobe-pc.json features[fragmentShaderPixelInterlock]` and `features[fragmentShaderSampleInterlock]` are both `"status": "yes"`. By the probe's rule ([`vkprobe.md`](vkprobe.md), "What it prints") that means: the live probe, on this device and driver, found `VK_EXT_fragment_shader_interlock` in the device's extension list *and* the feature bits `fragmentShaderPixelInterlock` and `fragmentShaderSampleInterlock` true. That is all. The file does not hold the extension's revision or the shading-rate bit, nor does it show that a shader using the interlock compiles, orders correctly, or runs at a useful speed on this driver; no shader was run. Azahar's Vulkan renderer uses only the pixel bit (`source: azahar@5f3306d renderer_vulkan/vk_instance.cpp:582-587`) and has no AMD rule at all (a search of `vk_instance.cpp` for AMD finds nothing), so on this driver it would take the interlock path whenever the device offers it (`inference`; no user report was read).
- **What the file does not contain.** It is the probe's own `--json` and has four keys: `mh4n_vkprobe`, `source`, `device`, `features` (24 entries). No extension list, no formats list (only the probe's judgement of D24S8, D32S8 and BC7), no limits, no queue families, no memory heaps, no list of which of the 31 `extendedDynamicState3*` bits are set (only "18 of 31 bits"), no instance extensions.
- **The `vulkaninfo` file is not in the repository.** [`vkprobe-results.md`](vkprobe-results.md) says the run was "checked against the SDK's `vulkaninfo --json` on the same device" and quotes "198 device extensions", "`vulkaninfo` names the file driver 2.0.353" and the absence of `VK_GOOGLE_display_timing` and `VK_EXT_present_timing`. `docs/research/` holds only `vkprobe-pc.json`, `vkprobe-results.md` and `vkprobe.md`, and `git ls-files` finds no `VP_VULKANINFO*` file. Those four statements are therefore `unverified` here; the probe's own `presentId` and `presentWait` lines (both `no`, device level) are checked. What would verify the rest: that file, run through `mh4n_vkprobe --from-json`. D57's row repeats the absence of display timing and present timing.
- **Both run dates.** [`vkprobe-results.md`](vkprobe-results.md) says "Run on 2026-10-21", with Native CI run 39710163710 (artifact `mh4n-runner-windows`). The commit that added the file, `ba1ba93`, has the author date 2026-10-21 19:51:45 -0700, which is 2026-10-22 02:51 UTC. The task text said 2026-10-22. The repository shows 2026-10-21 for the run (the results file) and for the commit (in the author's local time, UTC-7); in UTC the commit falls on 2026-10-22, which may be where the task text's date comes from (`inference`). The JSON carries no date.
- **Against `vkprobe-results.md`:** every `yes`/`no` and number in its text (RX 6700 XT, 0x1002/0x73df, 26.10.1, API 1.4.315, "18 of its 31 bits", D24S8 no, D32S8 yes, rasterization-order no, present id/wait no) matches the JSON.

### 3.3 The M2, stand-in

**Luis's own run of the probe on his M2 is pending.** Every M2 number below comes from another person's M2 or from sources, and is a `stand-in`; the table is filled in [3.4](#34-the-m2-pending-luiss-run). The report's submitter is not shown on the page, and the core count, the display and the exact macOS configuration are unknown (`unverified`). A different macOS can change which Metal features MoltenVK sees.

**The report.** `report 52371` (https://vulkan.gpuinfo.org/displayreport.php?id=52371): Apple M2, integrated GPU, vendorID 0x106B, deviceID 0x1A060208, driver MoltenVK 1.4.2 (`driverVersion` 0.2.2210, which is `MVK_VERSION` decoded as a Vulkan version: `source: MoltenVK@v1.4.2 MoltenVK/MoltenVK/API/mvk_private_api.h:65-69`, `MVKDevice.mm:2412`), conformance 1.4.4.0, Vulkan 1.4.357, macOS 26.6.2 arm64, 130 device extensions. The newest MoltenVK *release* is 1.4.2 (`git ls-remote --tags` lists no later tag, retrieved 2026-10-23). It was made with the Vulkan Capabilities Viewer, not with `mh4n_vkprobe`; its JSON profile (`api/v3/getprofile.php?id=52371`, 241,141 bytes) is the Vulkan Profiles layout the probe's `--from-json` reads. The "portability" download is byte-identical.

**How the probe reads it.** Direct, `--from-json` shows three gaps of the *export* (not of the reader): the device name and type are missing (`device: (unnamed)`), `D24_UNORM_S8_UINT` is `unknown` (the export drops formats with no feature, as `vulkaninfo` does), and 19 of the 130 extensions are missing (among them `VK_KHR_dynamic_rendering_local_read`, `VK_KHR_swapchain_maintenance1`, `VK_KHR_present_id2`, `VK_KHR_present_wait2`; none is read by a probe line). The probe also labels the file `source: vulkaninfo`. A small converter (kept in the scratch folder, not in the repository) fills exactly those three gaps from the report's own page and nothing else: device name, type and ID from the property table, the missing extension rows, and every row of the optimal-tiling table whose cells are all empty (42 rows, among them `D24_UNORM_S8_UINT` and `X8_D24_UNORM_PACK32`) as formats with no features. The only differences to the direct run are the device lines and the D24 line (`unknown` becomes `no`). Twelve lines were also read by hand on the report page and agree with the probe.

The probe's table for report 52371, converted, verbatim (`stand-in`, `source: report 52371`):

```
device: Apple M2
  type: integrated GPU (vendor 0x106b, device 0x1a060208)
  driver: MoltenVK 1.4.2
  api version: 1.4.357
  source: vulkaninfo
  features:
    logic ops (logicOp)                                 no (fallback: emulate the ops in the fragment shader (partial: 4 of 16 ops on MoltenVK))  [logicOp is false]
    fragment shader interlock, pixel                    yes
    fragment shader interlock, sample                   yes
    storage image writes from fragment shaders          yes
    geometry shaders                                    no (fallback: skip the per-triangle quaternion fix-up, or run it in a compute pass)  [geometryShader is false]
    fragment shader barycentrics                        yes
    triangle fans                                       yes  [portability subset: triangleFans is true]
    8-bit indices (VK_EXT_index_type_uint8)             yes
    custom border colours (VK_EXT_custom_border_color)  no (fallback: emulate the border colour in the shader)  [VK_EXT_custom_border_color is not on the device]
    shaderClipDistance                                  yes
    VK_EXT_shader_stencil_export                        yes
    VK_EXT_rasterization_order_attachment_access        no (fallback: fragment shader interlock, else a copy and a barrier per draw)  [VK_EXT_rasterization_order_attachment_access is not on the device]
    depth-stencil D24_UNORM_S8_UINT                     no (fallback: D32_SFLOAT_S8_UINT)  [D32_SFLOAT_S8_UINT is available]
    depth-stencil D32_SFLOAT_S8_UINT                    yes
    BC7 textures (BC7_UNORM_BLOCK)                      yes
    dynamic rendering                                   yes
    VK_EXT_graphics_pipeline_library                    no (fallback: a pipeline cache and compilation on worker threads)  [VK_EXT_graphics_pipeline_library is not on the device]
    VK_KHR_present_id (presentId)                       yes  [device level: a swapchain's support needs a surface]
    VK_KHR_present_wait (presentWait)                   yes  [device level: a swapchain's support needs a surface]
    push descriptors (VK_KHR_push_descriptor)           yes
    timeline semaphores                                 yes
    VK_EXT_extended_dynamic_state (1)                   yes
    VK_EXT_extended_dynamic_state2                      yes
    VK_EXT_extended_dynamic_state3                      yes  [5 of 31 bits (see the device's extendedDynamicState3* features)]
```

**KosmicKrisp on the same M2 (`report 52372`)**, same procedure; macOS 26.6.2, `driverInfo` "vulkan-sdk-1.4.363 (git-269a955c89)", Vulkan 1.4.363, conformance 1.4.3.2, submitted two minutes after 52371 (that they are one machine is `inference`). A second KosmicKrisp report, 52375 (macOS 27.0.1, `vulkan-sdk-1.4.357.1`), was also read. Differences to MoltenVK 1.4.2:

| Probe line | KosmicKrisp 52372 | MoltenVK 1.4.2 52371 |
|---|---|---|
| logic ops | **yes** | no |
| interlock, pixel and sample | **no** (`VK_EXT_fragment_shader_interlock` not listed) | yes, yes |
| barycentrics | **no** (neither the KHR nor the NV extension) | yes (listed; unusable, below) |
| triangle fans | yes (a full device, no portability subset; emulated, `Mesa@f3f63ef src/kosmickrisp/vulkan/kk_cmd_draw.c:1842-1843`) | yes (portability subset) |
| extended dynamic state 3 | 7 of 31 bits | 5 of 31 bits |
| all other probe lines | equal (D24 no, D32S8 yes, BC7 yes, geometry no, custom border no, graphics pipeline library no, present id/wait yes) | |

Other differences read by hand on the pages: `maxUniformBufferRange` 65536 against 4294967295; `maxVertexInputBindingStride` 2048 against 1073741824; `maxPushConstantsSize` 256 against 4096; `minUniformBufferOffsetAlignment` 64 against 16; `maxBoundDescriptorSets` 32 against 8; `timestampPeriod` 41.6667 against 1; no `VK_KHR_portability_subset`; no `VK_GOOGLE_display_timing`. KosmicKrisp lists `VK_EXT_attachment_feedback_loop_layout`, `VK_KHR_dynamic_rendering_local_read`, `VK_EXT_provoking_vertex` and `VK_EXT_primitive_restart_index`, which MoltenVK 1.4.2 does not. MoltenVK 1.4.1 (`report 51106`, the version Azahar pins) differs from 1.4.2 only in `VK_KHR_shader_quad_control` and the API version; its probe table is identical.

**MoltenVK, from its source** (`MoltenVK@v1.4.2`; nothing was compiled or run; what MoltenVK does on an M2 is only what the report shows):

| Item | What the source says | Source |
|---|---|---|
| `logicOp` | `_features.logicOp = getMVKConfig().useMetalPrivateAPI`; the build setting `MVK_USE_METAL_PRIVATE_API` is OFF by default. Upstream CI builds a macOS artifact with it. How the release tarball Azahar downloads is built could not be read (`unverified`) | `MVKDevice.mm:2765`; `CMakeLists.txt:70`; `.github/workflows/CI.yml:25-31, 48` |
| `geometryShader` | never assigned, always false; Metal has no geometry stage | `grep geometryShader MVKDevice.mm`: no match |
| interlock | listed only if `rasterOrderGroups` (Apple4 and later, so the M2); implemented through SPIRV-Cross's `SPV_EXT_fragment_shader_interlock` since 1.0.37. One known gap: `force_fragment_with_side_effects_execution = false`, because enabling it fails dEQP `pixel_ordered` discard tests ("Requires investigation") | `MVKDevice.mm:2632, 3530-3532`; `MVKPipeline.mm:1672-1673`; `Docs/Whats_New.md:1803, 1838` |
| `VK_EXT_rasterization_order_attachment_access` | **absent** at 1.4.1, 1.4.2 and main | `MoltenVK/Layers/MVKExtensions.def` (whole file) |
| barycentrics | listed and the bit set unconditionally when Metal reports support, but SPIRV-Cross (pin `6c09849`) **throws** on a per-vertex input: "PerVertexKHR decoration is not supported in MSL." Same text at main's pin `aa217ae`. `pervertexEXT` inputs are what G2's fragment-shader route needs | `MVKDevice.mm:2635-2645`; `SPIRV-Cross@6c09849 spirv_msl.cpp:3920-3921`; `@aa217ae spirv_msl.cpp:3926-3927` |
| custom border colour | the extension is absent; the three Metal border enums are the only ones (white is one of them), so an opaque white border works without it | `MVKExtensions.def`; `mvk_datatypes.mm:344-355` |
| 8-bit indices | listed; Metal has no 8-bit index type, so MoltenVK converts with a compute pass at draw time | `MVKCmdDraw.mm:400-432, 467-470` |
| triangle fans | `triangleFans = true` hard-coded; encoded as an indexed indirect triangle list; added in 1.2.5 | `MVKDevice.mm:610`; `MVKCmdDraw.mm:182-186`; `Docs/Whats_New.md:599` |
| vertex stride alignment | `vertexStrideAlignment` 4, set to 1 from Apple5; the M2 report says 1 | `MVKDevice.mm:2433, 2568`; `report 52371` |
| depth formats | `D24_UNORM_S8_UINT` and `X8_D24_UNORM_PACK32` have no features on Apple GPUs (`isDepth24Stencil8PixelFormatSupported` false); D32S8 is the substitute | `MVKDevice.mm:126-129`; `MVKPixelFormats.mm:912, 914, 1488` |
| BC7 | `textureCompressionBC` from `MTLDevice.supportsBCTextureCompression`; the report shows true and BC7 sampled | `MVKDevice.mm:118-120, 2786` |
| graphics pipeline library | absent (and `VK_KHR_pipeline_library`, `VK_EXT_shader_object`) | `MVKExtensions.def` |
| timeline semaphores | implemented with `MTLSharedEvent` | `MVKSync.h:321-329` |
| extended dynamic state 3 | true: depth clamp, polygon mode, depth clip, sample locations, line rasterization mode. **False, so baked into pipelines:** `ColorBlendEnable`, `ColorBlendEquation`, `ColorWriteMask`, `LogicOpEnable`, `RasterizationSamples`, `SampleMask`, `AlphaToCoverageEnable` | `MVKDevice.mm:699-729` |
| primitive restart | "always enabled, as Metal does not support disabling it" | `Docs/MoltenVK_Runtime_UserGuide.md:345` |
| present modes | FIFO always, IMMEDIATE when Metal allows it; no MAILBOX | `MVKDevice.mm:2149-2155` |
| `present_wait` | `waitForPresent` waits on a condition variable until `_currentPresentId >= presentId`; the id advances in `notifyPresentComplete`, called from the Metal command buffer's **completion handler**, with a comment that the last callbacks "can occasionally stall". The drawable's presented time feeds `VK_GOOGLE_display_timing` instead. So the wait is released when the GPU work finishes, **not when the frame is on the display** (`inference` that this is not enough for D57's "time a frame is shown") | `MVKSwapchain.mm:246-285`; `MVKImage.mm:1677-1681, 1712-1724` |
| framebuffer fetch | MoltenVK does not set SPIRV-Cross's option `use_framebuffer_fetch_subpasses` (it appears only in the pipeline-cache serialiser; the SPIRV-Cross default is false). With `VK_EXT_rasterization_order_attachment_access` absent, MoltenVK 1.4.2 gives no framebuffer fetch (`inference`). `VK_KHR_dynamic_rendering_local_read` is listed; what it compiles to was not traced (`unverified`) | `MVKPipeline.mm:2888` (1.4.1: `:2848`; main: `:2820`) |
| fast math | `MVK_CONFIG_FAST_MATH_ENABLED` default **2** ("on demand": fast math unless a shader's float-controls limit it), not 1 | `Docs/MoltenVK_Configuration_Parameters.md:201-210`; `MVKEnvironment.h:183-184` |

**KosmicKrisp, from its source** (`Mesa@f3f63ef`): "implemented on top of Metal 4, which requires macOS 26 and up" (`docs/drivers/kosmickrisp.rst:4-8`); `.logicOp = true`, done by lowering the blend in the fragment shader (`src/kosmickrisp/vulkan/kk_physical_device.c:240`, `kk_shader.c:420-428`; that the destination read becomes a Metal framebuffer fetch is `inference`); no `geometryShader`; neither `EXT_fragment_shader_interlock` nor `KHR_fragment_shader_barycentric` in the extension table (`kk_physical_device.c:95-215`; a barycentric *property* line at `:896` is a leftover in the properties struct, not an exposure); `EXT_custom_border_color` is `KK_EXPERIMENTAL(CUSTOM_BORDER)`, off unless `MESA_KK_EXPERIMENTAL=custom_border` (`kk_physical_device.c:177`; whether it works on an M2 is `unverified`); present id/wait are in the table (`:150-166`), their semantics were not read.

**MoltenVK's status.** The README of `main` (`5189f8d`) carries a deprecation notice added on 2026-10-13 (`0aa76ad`): "Khronos has deprecated MoltenVK ... maintained by the community on a best-effort basis ... Khronos now recommends KosmicKrisp on Apple Silicon Macs. MoltenVK remains the recommended option for Intel-based Macs and for iOS" (`source: MoltenVK@main README.md:6-14`). The notice is not in the `v1.4.2` tag; `Docs/Whats_New.md` on main calls the next release "1.4.3 - Released TBD"; no release followed 1.4.2.

**Apple's tables, native Metal on an Apple8 GPU** (the M2: "M2-series | Metal 3 & 4 | Apple8", `FST p.2`; a feature is available from the family named in its row onward):

| Topic | Apple's row | M2 |
|---|---|---|
| Framebuffer fetch / programmable blending | "Programmable blending", family Apple2 (`FST p.3`); MSL fragment input `color(m)`, "macOS: Metal 2.3 and later" (`MSL p.146`). That the table row and the attribute are the same capability is the usual reading (and `metal.md`'s), not a sentence in either document (`inference`) | available |
| Raster order groups | "Raster order groups", family Apple4 (`FST p.4`); "Maximum number of raster order groups, per fragment function: 8" for Apple4 to Apple10 (`FST p.7`); `[[raster_order_group(index)]]` on textures and device buffers (`MSL p.137`) | available, 8 |
| 24-bit depth | no row; `Depth24Unorm_Stencil8` exists for macOS only and is deprecated in 27.0 (Apple's `mtlpixelformat/depth24unorm_stencil8` page); MoltenVK's `isDepth24Stencil8PixelFormatSupported` is false on Apple GPUs | absent |
| Logic operations | `MTLRenderPipelineColorAttachmentDescriptor` and `MTLRenderPipelineDescriptor` list no logic-op property; the MSL spec has no logic-op text. "No public API" is a statement about the pages searched (`unverified` for any other document) | none |
| Min/max blending | `MTLBlendOperation` has `min` and `max` | native |
| Triangle fans; 8-bit indices | `MTLPrimitiveType`: point, line, lineStrip, triangle, triangleStrip; `MTLIndexType`: uint16, uint32 | none |
| Geometry stage | none in the tables or the MSL spec; object/mesh functions "Mesh shading", family Apple7 (`FST p.5`; an earlier brief said Apple9, the 2026 table says Apple7) | none; mesh available |
| Barycentrics, primitive id | Apple7 (`FST p.4`); "Access to pre-raster per-vertex values" Apple10 (`FST p.6`) | barycentric and primitive id yes; per-vertex access not on Apple8 (that this row is the counterpart of `pervertexEXT` is not established) |
| Border colours | `opaqueWhite` among three; "Border color", family Apple7 (`FST p.3`) | white available |
| Vertex stride alignment | 4 B for Apple2 and 3, 1 B from Apple4 (`FST p.7`) | 1 B |
| Barriers inside a render pass | Apple3 to Apple10 do not support memory barriers that include the fragment or tile stage or `renderTargets` scope in a render encoder (`FST p.6`, note 3) | not available: "copy and barrier" inside a pass is not a Metal option here (`inference` for the port) |
| BC formats | "BC pixel formats", family Apple9, note 2: some Apple7/Apple8 GPUs support them in iPadOS, check `MTLDevice.supportsBCTextureCompression` (`FST p.3, p.6`) | not guaranteed by the table; report 52371 shows `textureCompressionBC true` |
| Binary archives, function specialization | Apple3, Apple2 (`FST p.5, p.4`) | available |

How fast framebuffer fetch or raster order groups are on an M2 is `unknown`; only a measurement answers it.

### 3.4 The M2, pending Luis's run

Ready to take his output. The procedure is [`vkprobe.md`](vkprobe.md) ("Running it", "What to send back") with the corrections the stand-in reading found.

**Commands (Terminal on the Mac)**

```
# 1. The program: CI artifact mh4n-vkprobe-macos (pushes to native and wp/** branches, manual dispatch; 30 days; not pull requests)
chmod +x mh4n_vkprobe
xattr -d com.apple.quarantine mh4n_vkprobe          # if macOS says it cannot verify the developer
./mh4n_vkprobe > vkprobe-mac.txt
./mh4n_vkprobe --json > vkprobe-mac.json

# 2. The machine
sw_vers                                             # macOS version and build
sysctl -n machdep.cpu.brand_string                  # "Apple M2"
system_profiler SPHardwareDataType                  # model, memory
system_profiler SPDisplaysDataType                  # chip, GPU core count, Metal line, displays and refresh rate

# 3. Which Vulkan drivers answer (the Vulkan SDK's tools)
vulkaninfo --summary
vulkaninfo --json                                   # writes VP_VULKANINFO_<device>_<driver>.json; then: mh4n_vkprobe --from-json VP_VULKANINFO_*.json
VK_LOADER_DEBUG=driver ./mh4n_vkprobe --json > /dev/null 2> loader.txt
ls "$VULKAN_SDK"/lib 2>/dev/null | grep -i "vulkan\|molten"; ls /opt/homebrew/lib /usr/local/lib 2>/dev/null | grep -i "vulkan\|molten"
brew list --versions molten-vk vulkan-loader 2>/dev/null

# 4. If two drivers answer (MoltenVK and KosmicKrisp): probe each alone
./mh4n_vkprobe --loader /path/to/libMoltenVK.dylib --json > vkprobe-mac-moltenvk.json
VK_DRIVER_FILES=<path to one ICD .json> ./mh4n_vkprobe --json > vkprobe-mac-kosmickrisp.json
MESA_KK_EXPERIMENTAL=custom_border ./mh4n_vkprobe   # only with KosmicKrisp: the custom border line should change from no
```

`VK_LOADER_DEBUG`, `VK_DRIVER_FILES` and the installed name of KosmicKrisp's ICD manifest are named from `memory`, not from a source read here (`unverified`); the manifest is built as `kosmickrisp_mesa_icd` (`Mesa@f3f63ef src/kosmickrisp/vulkan/meson.build:173-176`). `MESA_KK_EXPERIMENTAL` is read in `kk_debug.c:22-34`.

**Corrections to what the procedure tells him to expect** (the stand-in reading, [11](#11-corrections-to-earlier-documents-found-while-checking)):
- For MoltenVK 1.4.x expect **`yes` for triangle fans**, not `no`. Expect `no` for logic ops, geometry shaders, custom border colours, `VK_EXT_graphics_pipeline_library` and `D24_UNORM_S8_UINT`.
- The `yes` for barycentrics on MoltenVK means "listed", not "usable".
- The `yes` for present id and present wait is the device's answer only; the wait is released at command-buffer completion (above).
- Two drivers can answer on a Mac with the LunarG SDK, so two `Apple M2` devices may appear: tell them apart by the `driver:` line. KosmicKrisp needs macOS 26 or later; MoltenVK 1.4.2 needs macOS 12 or later (`Docs/Whats_New.md:29`).
- `logicOp` depends on the MoltenVK build the loader finds: a build with Metal's private API answers `yes`. Write down which build it is (the SDK's, Homebrew's, Azahar's bundled `libMoltenVK.dylib`).
- The probe does not read limits, the portability subset beyond `triangleFans`, present modes, instance extensions or the 16-bit colour formats; `vulkaninfo --json` has them.

**Files to send back:** `vkprobe-mac.json` (and `-moltenvk`, `-kosmickrisp` if two drivers answer), `vkprobe-mac.txt`, `loader.txt`, the `VP_VULKANINFO_*.json` file(s), and the output of the one-liners (chip, core count, macOS version, refresh rate, which Vulkan libraries exist). Nothing in them is game data.

**The table to fill.** One row per probe line; the stand-in columns are reports 52371 and 52372 as converted in [3.3](#33-the-m2-stand-in).

| Probe line | Stand-in: MoltenVK 1.4.2 | Stand-in: KosmicKrisp | Luis's run |
|---|---|---|---|
| logic ops | no | yes | |
| fragment shader interlock, pixel | yes | no | |
| fragment shader interlock, sample | yes | no | |
| storage image writes from fragment shaders | yes | yes | |
| geometry shaders | no | no | |
| fragment shader barycentrics | yes (listed; unusable) | no | |
| triangle fans | yes | yes | |
| 8-bit indices | yes | yes | |
| custom border colours | no | no (experimental switch) | |
| shaderClipDistance | yes | yes | |
| `VK_EXT_shader_stencil_export` | yes | yes | |
| `VK_EXT_rasterization_order_attachment_access` | no | no | |
| depth-stencil `D24_UNORM_S8_UINT` | no | no | |
| depth-stencil `D32_SFLOAT_S8_UINT` | yes | yes | |
| BC7 | yes | yes | |
| dynamic rendering | yes | yes | |
| `VK_EXT_graphics_pipeline_library` | no | no | |
| `VK_KHR_present_id` | yes (device level) | yes (device level) | |
| `VK_KHR_present_wait` | yes (device level) | yes (device level) | |
| push descriptors | yes | yes | |
| timeline semaphores | yes | yes | |
| `VK_EXT_extended_dynamic_state` (1) | yes | yes | |
| `VK_EXT_extended_dynamic_state2` | yes | yes | |
| `VK_EXT_extended_dynamic_state3` | yes (5 of 31 bits) | yes (7 of 31 bits) | |

**What his run settles** (each also in [10](#10-questions-only-luiss-mac-run-or-more-captured-scenes-can-settle)):
1. Which driver or drivers his loader returns, with which version and library path, and whether his MoltenVK is a private-API build (`logicOp`).
2. Whether his line set equals the stand-in's, and which lines differ (macOS version, GPU core count).
3. Whether `VK_EXT_fragment_shader_interlock` is listed on his MoltenVK: if not, shadow writes on the Mac fall back to compare-and-swap.
4. Whether `textureCompressionBC` is true on his macOS (the HD pack's BC7, G11).
5. Whether the display's refresh rate and Low Power Mode matter to the later measurements (`system_profiler`).

## 4. Needs against the hardware

One matrix. Cells: `yes`; `no`; `fb` (no, but a fallback exists and the text names it); `?` (unknown: the probe has no line for it and nothing else was read); `api` (a question of the interface, not of the hardware); `open` (undecided in the project). Tags: `[pc]` `vkprobe-pc.json`; `[lp]` the lavapipe probe or `vulkaninfo`; `[R]` report 52371 and `[K]` report 52372 (both `stand-in`); `[B]` MoltenVK 1.4.2 source and `[KS]` Mesa KosmicKrisp source ([3.3](#33-the-m2-stand-in)); `[FST]` Apple's tables; `[az]` Azahar's code ([5](#5-what-the-proven-renderer-asks-of-its-graphics-layer)). "M2 native Metal" is Apple's tables and the MSL spec; nothing was run on a Metal device. A `yes` is a capability, not proof that the path works.

| ID | RX 6700 XT | M2, MoltenVK 1.4.2 | M2, KosmicKrisp | M2, native Metal | lavapipe |
|---|---|---|---|---|---|
| G1 vertex pulling, streamed buffers | `?` (no probe line; Azahar needs only streamed buffers `[az]`) | yes `[B]` `MVKDevice.mm:2782` | yes `[KS]` | yes `[FST]` p.4 (read/write buffers Apple3), p.7 (31 buffer arguments) | yes `[lp]` `vertexPipelineStoresAndAtomics` |
| G2 quaternion fix-up | yes `[pc]` geometry shaders and barycentrics | fb: skip the fix-up. `geometryShader` no `[R]`; barycentrics listed but SPIRV-Cross throws on per-vertex inputs `[B]` | fb: skip. no geometry, no barycentric extension `[K]` | no. `barycentric_coord` yes (Apple7); per-vertex access only Apple10; buffer-by-`primitive_id` and mesh routes untried `[FST]` | yes by geometry shader; barycentrics no `[lp]` |
| G3 D24S8 | fb: `D32_SFLOAT_S8_UINT` `[pc]` | fb: D32S8 `[R]` | fb: D32S8 `[K]` | fb: no 24-bit depth `[FST]` | yes (D24S8 and D32S8) `[lp]` |
| G4 shadow write and lookup | yes: storage writes, pixel interlock `[pc]` | yes (interlock listed = raster order groups); not run `[R][B]` | fb: no interlock, so the compare-and-swap loop `[K]` | yes: raster order groups, 8 per fragment function `[FST]` p.4, p.7 | yes `[lp]` |
| G5, G6 lookup tables, procedural textures | `?` (not probed; core Vulkan, `memory`) | yes `[R]` `maxTexelBufferElements` 67,108,864 | yes `[K]` 268,435,456 | yes `[FST]` p.8 (256 M px) | yes `[lp]` 134,217,728 |
| G7 gas | open (N2-10); no rasterization-order access, interlock yes `[pc]` | open; no framebuffer fetch from MoltenVK (`inference` from [3.3](#33-the-m2-stand-in)); `VK_KHR_dynamic_rendering_local_read` listed, untraced | open; local-read and feedback-loop extensions listed `[K]`, untraced | open; framebuffer fetch available | open; rasterization-order access and interlock yes `[lp]` |
| G8 plain draw state | `?` (not probed) | yes, but blend state is baked into pipelines (X8) and primitive restart is always on (X7) `[R][B]` | as MoltenVK, blend baked `[K]` | yes (blend factors, constant colour, write mask) `[FST]` | `?` |
| G9 formats | fb for D24; colour formats not probed `[pc]` | RGB8 no (use RGBA8); `R5G6B5`, `R5G5B5A1`, `A1R5G5B5`, `R4G4B4A4` are attachments with blending; the `B5G6R5`, `B5G5R5A1`, `B4G4R4A4` variants are sampled only; D24 no `[R]` | not compared one by one | `?` (16-bit names on Apple GPUs not read) | RGBA8 attachments yes (plume experiment, [6](#6-plume-against-the-needs)); D24 yes `[lp]` |
| G10 pipelines off-thread | yes: graphics pipeline library, dynamic rendering, extended dynamic state 1 to 3 (18 of 31 bits) `[pc]` | fb: plain `VkPipelineCache`; no pipeline library; dynamic state 3 only 5 bits `[R]` | fb: same; 7 bits `[K]` | yes: binary archives (Apple3), function specialization (Apple2) `[FST]` p.4, p.5 | yes: pipeline library yes, 16 of 31 bits `[lp]` |
| G11 BC7 | yes `[pc]` | yes, `textureCompressionBC` true `[R]` | yes `[K]` | `?`: the table says Apple9 with an Apple8 caveat; check `supportsBCTextureCompression` `[FST]` p.3, p.6 | yes `[lp]` |
| N1 all 16 logic ops | yes `[pc]` | fb: `logicOp` false; shader emulation does 4 of 16 `[R][az]` | yes `[K]` (shader-side lowering `[KS]`) | fb: no pipeline API; framebuffer fetch makes all 16 possible `[FST]` | yes `[lp]` |
| N2 triangle fans | yes `[pc]` | yes: `triangleFans` true, emulated by an indexed indirect list `[R][B]` | yes, emulated `[K][KS]` | no: rewrite in the backend `[FST]` | yes `[lp]` |
| N3 min/max blending | ops yes; PICA factors need a shader path unless both factors are One, on every host `[az]` | same | same (`?` for the ops) | ops native `[FST]`; whether the PICA semantics need fetch is not established | same as the PC |
| N4 8-bit indices | yes `[pc]` | yes, converted by a compute pass at draw time `[R][B]` | yes `[K]` | no: widen `[FST]` | yes `[lp]` |
| N5 odd strides | yes (full device) | yes: alignment 1 on Apple8, 4 below Apple5 `[R][B]` | yes (no portability subset) `[K]` | yes: 1 B from Apple4 `[FST]` p.7 | yes `[lp]` |
| N6 cube, projection | `?` | `imageCubeArray` true; cube shadow compare not checked `[R]` | `imageCubeArray` true `[K]` | yes (cube arrays Apple4) `[FST]` | `?` |
| R1 instance, surface, headless | `?` (no instance list in the file) | headless surface listed `[R]` | `?` | api (`CAMetalLayer`) | yes: `VK_EXT_headless_surface` `[lp]` |
| R2 shared device, timeline | yes timeline `[pc]` | yes (`MTLSharedEvent`); 4 families of 1 queue `[R][B]` | yes `[K]` | `MTLSharedEvent` | yes timeline; **one queue** `[lp]` |
| R3 presentation timing | no: `present_id`, `present_wait` `[pc]` | listed yes, but released at command-buffer completion; `VK_GOOGLE_display_timing` listed `[R][B]` | listed yes, semantics unverified; no display timing `[K]` | drawable presented time exists (MoltenVK uses it) `[B]` | no `[lp]`; no surface to test |
| R4 raw handles | api | api | api | api | api |
| R5 readback, timestamps | `?` (not in the file) | yes: `timestampPeriod` 1, 64 valid bits `[R]` | yes: period 41.6667 `[K]` | counter sampling points exist, not evaluated | yes: period 1, 64 bits `[lp]` |
| R6 Vulkan at run time | `?` | pending Luis's run (loader or `libMoltenVK.dylib`) | needs the loader and an ICD manifest | n/a | yes (`libvulkan.so.1`) |
| R7 lavapipe | n/a | n/a | n/a | n/a | the device itself |
| R8 SPIR-V in | yes | yes (MoltenVK converts) | yes (NIR to MSL) | SPIR-V to MSL through SPIRV-Cross is the plan; plume's Metal takes only a metallib (6) | yes |
| R9 framebuffer fetch, raster order groups | see [7](#7-metal) | | | | |
| X1 format query, X5 per-draw descriptors, X6 copy-of-target | api | api | api | api (Apple3 to Apple10: no fragment-stage barrier inside a pass, so X6 is a pass break) `[FST]` p.6 | api |
| X2 depth output, X3 vertex formats, X4 zero-attachment pass | `?` | `?` | `?` | `?` | `?` |
| X7 primitive restart always on | no | **yes** (hazard) `[B]` | `?` | yes (Metal) | no |
| X8 blend state dynamic | partly (18 of 31 bits, which ones unknown) `[pc]` | **no** (5 of 31) `[R]` | **no** (7) `[K]` | baked | partly (16) `[lp]` |
| X9 MAILBOX | `?` | **no** (FIFO, IMMEDIATE) `[R]` | no (IMMEDIATE, FIFO) `[K]` | n/a | no surface |

**What lavapipe cannot exercise by default.** Lavapipe reports *more* than the PC in two places and *fewer* in one, and it is a different kind of device:
- It takes paths the PC does not use: `D24_UNORM_S8_UINT` directly (the PC needs `D32_SFLOAT_S8_UINT` and conversions), and `VK_EXT_rasterization_order_attachment_access` for G7 (absent on the PC). A test that passes there says nothing about the PC's D32S8 path. `source: lavapipe-probe.txt`, `vkprobe-pc.json features[D24_UNORM_S8_UINT, rasterizationOrderAttachmentAccess]`.
- It does not take the fallbacks the PC or the M2 need, because it reports the feature: D32S8 depth with conversions (G3, G9), the compare-and-swap shadow loop (G4; needed on KosmicKrisp), widening of 8-bit indices (N4), the fan rewrite (N2, which the M2 should use, [7](#7-metal)), logic-op emulation (N1; MoltenVK), emulated custom border colours, render passes instead of dynamic rendering, the pipeline-cache-only path instead of the pipeline library (G10; both M2 drivers), CPU decoding of BC7, binary semaphores instead of timeline. `inference` from the probe.
- It shares two paths with the PC, so those *are* exercised: geometry shaders present, and no `present_id`/`present_wait` (R3).
- It cannot test at all: fragment barycentrics (the extension is absent, so the barycentric route for G2 cannot be forced on); presentation (no surface; `present support = false`); a transfer-only queue family or a second queue; the discrete-GPU memory split (one memory type); and, for every driver, real behaviour and speed (the AMD driver, MoltenVK, KosmicKrisp).
- Lavapipe's extended dynamic state 3 has 16 bits against the PC's 18 and MoltenVK's 5; the set of bits differs per driver and the probe prints only the count.

**What a capability mask in the RHI would buy.** A switch that makes the device report a feature as absent (`RhiCaps`, overridable from a command-line option or the environment) would let lavapipe run each fallback listed above in CI, with lavapipe's real rendering: D32S8-only depth, no interlock (the compare-and-swap loop), no 8-bit indices, no custom border colour, no fans, no logic ops, no dynamic rendering, no pipeline library, no extended dynamic state 3 (for MoltenVK's baked blend state), no BC7, no timeline semaphore, and the PC's exact combination (no D24S8, no rasterization-order access). It would test the *logic* of each fallback. It would not test the real driver's behaviour, speed, or a real absence (a masked feature is still present in the loader), and it cannot force barycentrics on. The mask belongs in the RHI, below N2-13 and N2-12, so every fallback is one code path with one test.

## 5. What the proven renderer asks of its graphics layer

Azahar's Vulkan renderer, condensed from the research note. Read at `5f3306d`; nothing was built or run, so every statement about behaviour is a reading of the code. This is the specification of what any option must cover. (Paths under `src/video_core/`.)

### 5.1 Hard requirements, and what is optional

- **Hard:** Vulkan 1.1 on the instance and the device (throws otherwise); a loader (`libvulkan.so.1`, `libvulkan.dylib` or a bundled `libMoltenVK.dylib`); a queue family with `GRAPHICS` (the *last* matching family wins; one queue; the graphics and present queues are the same `VkQueue`; `vkGetPhysicalDeviceSurfaceSupportKHR` is never called); `VK_KHR_swapchain` in practice; a swapchain format `R8G8B8A8_UNORM` or `B8G8R8A8_UNORM`; a host-visible memory type. `source: azahar@5f3306d renderer_vulkan/vk_platform.h:22`, `vk_instance.cpp:155-160, 481-511, 645-655`, `vk_swapchain.cpp:161-170`, `vk_stream_buffer.cpp:89-91`.
- **Copied, not required:** it enables `robustBufferAccess`, `geometryShader`, `logicOp`, `samplerAnisotropy`, `fragmentStoresAndAtomics` and `shaderClipDistance` by copying what the device reports (`vk_instance.cpp:520-529`). `geometryShader`, `logicOp` and `shaderClipDistance` have fallbacks. **`fragmentStoresAndAtomics` has no consumer and no fallback**: without it the shadow-write shader is invalid. `textureCompressionBC` is *not* enabled although BC1/3/5/7 images are created for the HD pack (what a driver does then is `unverified`).
- **Optional extensions, each with a fallback:** `VK_KHR_image_format_list`, `VK_EXT_shader_stencil_export` (else the depth-stencil blit is unavailable), `VK_EXT_external_memory_host`, `VK_KHR_timeline_semaphore` (else one fence per submit and a waiter thread), `VK_KHR_portability_subset` (`triangleFans`, stride alignment), `VK_EXT_extended_dynamic_state` (else state baked into pipelines), `VK_EXT_custom_border_color` (else emulated in the shader), `VK_EXT_index_type_uint8` (else widened), `VK_EXT_robustness2` (`nullDescriptor`), `VK_EXT_fragment_shader_interlock` (else the compare-and-swap loop), `VK_EXT_pipeline_creation_cache_control`, `VK_KHR_fragment_shader_barycentric`. `source: azahar@5f3306d renderer_vulkan/vk_instance.cpp:451-479`.
- **Driver rules, all of them:** NVIDIA proprietary: no interlock, no pipeline cache control. Qualcomm proprietary: no timeline semaphore, no extended dynamic state, no custom border colour; a flush after any pass of more than 20 draws (also ARM). Mesa Turnip: no timeline. ARM: no extended dynamic state. MoltenVK: no tooling info, no uint8 indices, no barycentric; cube images off when `shaderOutputLayer` is absent. Android (compile time): no geometry shaders. **AMD, Intel, lavapipe: no rule.** `source: azahar@5f3306d renderer_vulkan/vk_instance.cpp:455-479, 627-639`, `vk_render_manager.cpp:14, 134-137`, `vk_instance.h:119-127`.
- **Not used anywhere in `renderer_vulkan/`** (a search of the directory): dynamic rendering, push descriptors, descriptor indexing, `VK_KHR_present_id`/`present_wait`/`VK_GOOGLE_display_timing`, `VK_EXT_graphics_pipeline_library`, `VK_EXT_headless_surface`, `VK_KHR_synchronization2`, multiview, specialization constants, timestamp or occlusion queries, `vkCmdCopyBuffer`/`FillBuffer`/`UpdateBuffer`, indirect draws, instancing (the instance count is always 1), input attachments (`inputAttachmentCount = 0` in every render pass, `vk_render_manager.cpp:217-219`).

### 5.2 Where interlock is used

In exactly one place: the shadow-**write** fragment shader (fragment operation mode `Shadow`), only in the GLSL generator, with the pixel interlock bit (`fragmentShaderPixelInterlock`; the sample and shading-rate bits are not used). Without it the shader runs an `imageAtomicCompSwap` loop on the same image. The SPIR-V generator never writes shadows: `UsesSpirvIncompatibleConfig()` sends ShadowCube, shadow rendering and min/max blend emulation to GLSL and glslang. `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:844-870, 1215-1233`, `pica_fs_config.h:407-414`, `vk_instance.cpp:471-473, 582-587`. The generator declares no `pixel_interlock_*` layout; glslang defaults to `pixel_interlock_ordered` (`externals/glslang` at `fc9889c`, `ParseHelper.cpp:1968-1971`). Whether the compare-and-swap loop gives the same result as the interlock path is `unverified`: the loop is atomic but not primitive-ordered, and `UpdateShadow` is order dependent by the note's reading (`inference`).

### 5.3 The `R32_UINT` shadow design

- **Write:** the pass has **no attachments** (a render pass with zero colour and depth, a framebuffer sized from the colour surface); the colour surface is bound as a **storage image** through an `R32_UINT` view of the `RGBA8` image. The shader packs `d = uint(clamp(depth,0,1) * 0xFFFFFF)` and a stencil-like byte, calls `UpdateShadow(old, d, s)` and stores `(depth << 8) | s`. Hard dependencies: `fragmentStoresAndAtomics`, `STORAGE_IMAGE` on RGBA8, and an image created `MUTABLE_FORMAT` with an `R32_UINT` view (the format list gives `R8G8B8A8_UNORM` and `R32_UINT`). Every native-RGBA8 surface gets storage usage and the mutable flag at creation, "in case it is found out later that this is a shadow-source texture". `source: azahar@5f3306d renderer_vulkan/vk_texture_runtime.cpp:183-190, 770-789, 1491-1520`, `vk_rasterizer.cpp:748-751`.
- **Lookup (Shadow2D):** **a texel fetch with a manual compare, not hardware depth-compare sampling.** The shadow map is the colour target read through the `R32_UINT` view as a `usampler2D`; the shader calls `texelFetch`, compares in `CompareShadow`, fetches four texels and mixes them by hand. Every sampler Azahar creates has `compareEnable = false`. `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:1460-1498`, `renderer_vulkan/vk_texture_runtime.cpp:1570`, `vk_rasterizer.cpp:694-700`.
- **Out of range is lit.** Any texel with `uv < 0` or `uv >= size` returns **1.0** before the fetch, in both generators, so the PICA's white border colour needs **no sampler feature**: no `VK_EXT_custom_border_color`, no border-colour enum. (`metal.md`'s row on custom border colours is right for ordinary textures; for Shadow2D no border is involved.) `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:1475-1479`, `spv_fs_shader_gen.cpp:790-806`.
- **ShadowCube** (not seen in those scenes) looks unfinished on Vulkan: the descriptor set is created with `TEXTURE_BINDINGS<1>` (count 1 at binding 0), `BindShadowCube` writes array elements 0 to 5, and the heap choice `use_cube_heap ? Texture : Texture` is the same on both sides. The note infers it cannot work as committed; not run. `source: azahar@5f3306d renderer_vulkan/vk_pipeline_cache.cpp:69-77, 94`, `vk_rasterizer.cpp:651-654, 767-788`.

### 5.4 Geometry-shader draws are CPU draws

`PicaCore::DrawArrays` refuses acceleration when `use_gs == Yes` ("Geometry shaders cannot be accelerated due to register preservation"); those draws run the vertex shader and the PICA geometry shader on the CPU and are drawn from a streamed CPU vertex batch (`DrawTriangles`) with a trivial vertex shader. `PipelineRegs::UseGS` has only the values `No` and `Yes`, so the later checks in the Vulkan rasterizer are unreachable for a geometry-shader draw (`inference`). `source: azahar@5f3306d pica/pica_core.cpp:1052-1092`, `pica/regs_pipeline.h:164-167`, `renderer_vulkan/vk_rasterizer.cpp:423-451, 628-640`. The census saw the Point mode in 39 to 90 percent of the draws: those draws need only a streamed vertex buffer and a trivial vertex shader from the RHI. **The quaternion fix-up for those draws is done on the CPU on every host**: `AddTriangle` negates the quaternion of vertex 1 and 2 when its dot product with vertex 0's is negative (`source: azahar@5f3306d rasterizer_accelerated.cpp:85-90`). Only a lit draw shaded on the GPU by the translated PICA vertex shader needs one of the two GPU routes, in this order: fragment barycentrics (`pervertexEXT`), else a fixed geometry shader (`triangles` in, `triangle_strip` out, used only when lighting is on), else no fix-up (`source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:549-558, 1427-1433`, `glsl_shader_gen.cpp:384-431`, `vk_pipeline_cache.cpp:501-515`).

### 5.5 Gas is drawn by nobody

Both fragment generators, on fog mode `Gas`, log "Unimplemented gas mode" and write transparent black (the code comment: discarding "causes graphical issues in some MH games"); in the SPIR-V generator a gas fragment still writes depth, in the GLSL one it does not (`inference`). The software renderer's fog function acts only on `FogMode::Fog`, so it draws a gas fragment as an ordinary one. The 16 gas registers are `INSERT_PADDING_WORDS(0x10)` in Azahar: no state, no table, no dirty tracking. `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:171-181, 835-842`, `spv_fs_shader_gen.cpp:70-77, 197-207`, `renderer_software/sw_rasterizer.cpp:800-828`, `pica/regs_framebuffer.h:15-19, 298`.

**What 3dbrew says** (`GPU/Internal_Registers`, retrieved 2026-10-23, "last edited on 18 October 2026"): fog mode 7 is gas; `GPUREG_COLOR_OPERATION` fragment operation mode 1 is Gas (0 Default, 3 Shadow), and colour buffer format 0 is "RGBA8/Gas"; `GPUREG_GAS_ATTENUATION`, `GAS_ACCMAX`, `GAS_LIGHT_XY`, `GAS_LIGHT_Z`, `GAS_LIGHT_Z_COLOR`, `GAS_LUT_INDEX`/`GAS_LUT_DATA` (a 16-entry colour table), `GAS_DELTAZ_DEPTH`; `GPUREG_GAS_ACCMAX_FEEDBACK`: bits 16 to 31 hold "the maximum plain density (blue/alpha bytes of the color buffer) written by gas density rendering", "compared as unsigned 16-bit integers". The page does **not** say how the shading pass gets the density, nor the per-fragment accumulation formula (`unverified`; N2-10 or a hardware capture). *If* mode 1 combines each fragment's density with the value already in the colour buffer (`inference` from the register names and the running maximum), it needs a read-modify-write of the render target in the fragment stage: framebuffer fetch, raster-order access, interlock, or a copy per draw, plus a CPU-readable maximum. Open for N2-10; Azahar has no code to copy.

### 5.6 Layouts, barriers, passes

- **Every image lives in `VK_IMAGE_LAYOUT_GENERAL`** (initial transition `UNDEFINED` to `GENERAL` at creation). A transfer operation barriers `GENERAL` to `TRANSFER_{SRC,DST}_OPTIMAL` and back with per-surface stage and access masks; roughly two image barriers per image operation; the current layout is always known, so nothing is tracked. Depth reads in compute use `DEPTH_STENCIL_READ_ONLY_OPTIMAL`. Whether `GENERAL` costs bandwidth on the target GPUs is `unverified`. `source: azahar@5f3306d renderer_vulkan/vk_texture_runtime.cpp:127-148, 1170-1195`.
- **Render passes:** objects (not dynamic rendering), cached by (colour format, depth format, clear); one subpass, up to one colour and one depth-stencil attachment, `STORE` always, layouts `GENERAL`, no subpass dependencies, no input attachments, no resolve; a zero-attachment variant for the shadow pass. A pass is coalesced when framebuffer, pass, render area and clear request equal the current one; it ends with one pipeline barrier (a `GENERAL`-to-`GENERAL` image barrier per attachment, `BY_REGION`). `source: azahar@5f3306d renderer_vulkan/vk_render_manager.cpp:49-234`.
- **What breaks a pass:** clear, copy, blit, mip generation, texture upload, download, scale-up, surface creation, the feedback copy, both compute passes, queue submission, the start of the present pass. **A target that is also sampled** (`is_feedback_loop`) is bound through `Surface::CopyImageView()`, which ends the pass, copies **every mip level of the whole image** into a twin and lets the next draw start a new pass with `LOAD`; min/max blend emulation takes the same copy (`tex_color`). So: one pass break and one full-image copy per such draw. How often MH4U triggers it is not measured. `source: azahar@5f3306d renderer_vulkan/vk_rasterizer.cpp:718-732`, `vk_texture_runtime.cpp:1197-1299`.
- **Min/max blending (N3):** Vulkan's `MIN`/`MAX` ignore the factors, unlike the PICA's. Hardware blend is used only when both factors are One; otherwise the pipeline blends `ONE`/`ZERO`/`ADD` and the fragment shader computes the result from the copy of the colour target. GLSL generator only. This cost exists on the RX 6700 XT too. `source: azahar@5f3306d shader/generator/pica_fs_config.cpp:44-56`, `pica_fs_config.h:53-62`, `vk_rasterizer.cpp:725-732`.
- **Logic ops (N1):** hardware (all 16, through a table) when the `logicOp` feature exists and blending is off; otherwise the SPIR-V generator handles Clear, Set, Copy and NoOp (NoOp through the colour mask); CopyInverted's line is commented out; the other 11 log `Unhandled logic_op` and draw as Copy. `source: azahar@5f3306d renderer_vulkan/vk_graphics_pipeline.cpp:48-59, 204-205`, `pica_to_vk.h:106-129`, `spv_fs_shader_gen.cpp:82-102`.

### 5.7 Descriptors

Rasterizer pipeline layout: **three sets, 12 bindings (6 + 4 + 2), three dynamic offsets per draw, zero push-constant bytes.** Set 0 (written once): three `UNIFORM_BUFFER_DYNAMIC` (vertex data, vertex/geometry data, fragment data) and three `UNIFORM_TEXEL_BUFFER` (lookup tables). Set 1: four `COMBINED_IMAGE_SAMPLER` (tex0, tex1, tex2, `tex_color`), a new set and four writes **per draw**. Set 2: one `STORAGE_IMAGE` (the shadow buffer) and one combined sampler (the HD pack's normal map), a new set per draw. Descriptor types used in total: `UNIFORM_BUFFER_DYNAMIC`, `UNIFORM_TEXEL_BUFFER`, `COMBINED_IMAGE_SAMPLER`, `SAMPLED_IMAGE`, `STORAGE_IMAGE`, `STORAGE_BUFFER`; no separate samplers, no immutable samplers, no update-after-bind, no push descriptors. Sets come from pools in batches of 64 and are reused by tick; writes are queued (2,048) and flushed before a chunk is dispatched. Up to 12 vertex bindings plus one "fixed" binding (13), 16 attributes, all bound to one `VkBuffer` at different offsets. `source: azahar@5f3306d renderer_vulkan/vk_pipeline_cache.cpp:59-95`, `vk_rasterizer.cpp:115-141, 647-765`, `vk_resource_pool.cpp:18-64`, `vk_descriptor_update_queue.cpp:10-107`, `vk_graphics_pipeline.h:52-53`.

### 5.8 The abstract operations

| Operation | How often | Need |
|---|---|---|
| Persistently mapped ring buffers with a `Map`/`Commit`/wrapped contract and tick-based reuse (upload 512 MiB, download 16 MiB, stream: vertex/index 64 MiB, uniform 8 MiB, two texel rings up to 2 MiB) | every draw | G1, G5, G6, G8 |
| Three uniform-texel-buffer views over two rings | startup | G5, G6 |
| Bind vertex buffers (one buffer, many offsets), an index buffer (16-bit; 8-bit if available), three dynamic-offset uniform buffers | every draw | G8, N4, X5 |
| Create a 2D image (mips, 1 or 6 layers, format, usage, `MUTABLE_FORMAT`/`CUBE_COMPATIBLE`, format list) | per surface | G3, G4, G9, N6 |
| Views created lazily: sample, mip 0, storage (`R32_UINT`), depth, stencil | per surface | G3, G4 |
| Copy image regions (also within one image), blit (nearest/linear; also mip generation), clear (whole level by command, partial by a render pass), buffer-to-image and image-to-buffer (depth and stencil as two regions) | every transfer, copy, upload, download | G3, G9, R5 |
| Image barriers: `GENERAL` to transfer layouts and back with per-surface masks | about two per image operation | G9 |
| Deferred destruction by GPU tick | per replaced surface | all |
| Render pass: one subpass, colour + depth, load or clear, store; zero-attachment variant | per pass | G4, X4 |
| Pass coalescing, pass end with one barrier, pass break for the feedback copy | every draw | X6, N3 |
| Two compute passes: D24S8 to RGBA8 (depth read as colour) and depth/stencil to a buffer (readback of **every** D24S8 surface, followed by a CPU wait) | per transfer, per readback | G3, R5 |
| Full-screen-triangle passes: depth-stencil blit (needs stencil export); optional texture filters (an emulator user feature, not PICA behaviour) | rare | G3 |
| Graphics pipelines: shaders (vertex, fragment, optional geometry), vertex layout, blend, logic op, formats via the pass; depth/stencil/cull/topology baked unless extended dynamic state; viewport, scissor, stencil masks and reference, blend constants dynamic | on a new key | G8, G10, N1 |
| `VkPipelineCache` on disk (validated by header, vendor, device, UUID); two worker pools of `max(hardware threads, 2) / 2`; a draw whose pipeline is not ready is **skipped** when async compilation is on (default off) | start, shutdown, per new key | G10 |
| Timeline master semaphore, one tick per submit; command buffers and descriptor sets reused by tick; fences plus a waiter thread without timeline | every submit | R2 |
| Command recording into chunks on the emulation thread, replayed by a worker thread; `Flush` submits, `Finish` submits and waits | continuous | R2 |
| One queue, one `submit_mutex` shared by the worker and the present thread | every submit | R2 |
| An offscreen frame image, a present thread that acquires, blits or copies into the swapchain image and presents; FIFO/MAILBOX/IMMEDIATE; recreation on out-of-date, suboptimal, lost, resize or vsync change | per frame | R1, R3 |

### 5.9 Sizes

`source: azahar@5f3306d`, counted with a script (`python3 -I`) that removes `//` comments, `/* */` comments (string-aware) and whitespace and counts what is left; `.cpp` and `.h` only; 42 files; re-run in this work and reproduced.

| Group | Lines |
|---|---:|
| **Graphics-layer code**: `vk_instance` 836, `vk_platform` 411, `vk_common` 13, `vk_memory_util` 28, `vk_scheduler` 300, `vk_master_semaphore` 232, `vk_resource_pool` 210, `vk_descriptor_update_queue` 126, `vk_stream_buffer` 266, `vk_render_manager` 249, `vk_swapchain` 324, `vk_present_window` 530 | **3,525** |
| Renderer code: `vk_pipeline_cache` 641, `vk_graphics_pipeline` 504, `vk_rasterizer` 933, `vk_rasterizer_cache` 5, `vk_texture_runtime` 1,672, `vk_blit_helper` 882, `vk_shader_util` 222 | 4,859 |
| Neither: `vk_shader_disk_cache` 1,395, `pica_to_vk` 182, `renderer_vulkan` 1,405 (the two-screen compositor) | 2,982 |
| **Total of `renderer_vulkan/`** | **11,366** |
| `shader/generator/` (outside that directory) | 5,074 |

- **Where the primitives an RHI has to provide actually sit.** Inside the graphics-layer list: command chunks and worker replay (`vk_scheduler`, 300), ring buffers (266), tick-based pools (336), the present thread (530). **Outside it:** the image, copy, blit, clear, layout and barrier primitives are in `vk_texture_runtime.cpp` (1,672: 17 `ImageMemoryBarrier` literals and 21 `pipelineBarrier` calls) and `vk_blit_helper.cpp` (882); the descriptor and pipeline layouts are in `vk_pipeline_cache.cpp` (641) and pipeline creation in `vk_graphics_pipeline.cpp` (504). So the 3,525 is a floor for the RHI, not its size.
- **What the 11,366 includes that an RHI does not need:** the two-screen compositor (`renderer_vulkan.cpp/.h`, 1,405), the optional texture-filter passes (lines 633-904 of `vk_blit_helper.cpp`), the shader disk cache (1,395, which belongs to N2-16). `inference`.
- **Against `metal.md` 2.3, which says 11,413:** the figure is reproduced (9 of its 10 groups exactly, total 11,407) only when `/* */` block comments are *not* stripped; with them stripped it is 11,366. The two are the same size of thing; use 11.4k. [11](#11-corrections-to-earlier-documents-found-while-checking) lists it.
- **Licences (D6).** 6 of the 42 files are `GPLv3 or any later version` (`vk_graphics_pipeline.*`, `vk_pipeline_cache.*`, `vk_platform.h`, `vk_rasterizer.cpp`), 34 are `GPLv2 or any later version`, 2 carry SPDX `GPL-2.0-or-later` (`vk_master_semaphore.*`); all say "or any later version". `vk_master_semaphore.*`, `vk_scheduler.h`, `vk_stream_buffer.*` and `vk_resource_pool.*` also carry "yuzu Emulator Project" copyright lines (2019 or 2020), so a `native/THIRD_PARTY.md` entry for those must name yuzu as well as Azahar (`inference` from D6's rule). The files are not self-contained: they use Vulkan-Hpp, VMA, Boost.Container, fmt, Citra `common/` and `video_core/rasterizer_cache/`, `pica/`. The date of the licence switch in D6 (2026-10-03) could not be checked in a one-commit checkout.

## 6. plume against the needs

plume (https://github.com/renderbag/plume, MIT, commit `0ce184b`) is a C++17 render interface over Direct3D 12, Vulkan and Metal, "originally created to be used by RT64" (`source: plume@0ce184b README.md:3`). The research read the three public headers and `plume_vulkan.cpp` in the parts that matter, and only grepped `plume_d3d12.cpp` and `plume_metal.cpp`. In this section: `vk.cpp` is `plume_vulkan.cpp`, `vk.h` is `plume_vulkan.h`, `types.h` is `plume_render_interface_types.h`, `iface.h` is `plume_render_interface.h`, `metal.cpp` is `plume_metal.cpp`; all at `plume@0ce184b`. Verdicts: `exposes` (the API has it), `needs extending` (a patch makes it reachable), `no equivalent`, `not applicable`. Sizes are changed lines, counted for the Vulkan backend unless stated: `inference`, never measured. "LCD" is plume's stated design as "lowest common denominator between modern graphics APIs" (README.md:1): *fits* means the addition is a capability bit or something all three APIs have, *fights* that it is Vulkan-only.

### 6.1 The table

| ID | Verdict | plume element (file:line) | Patch points and size |
|---|---|---|---|
| G1 | exposes | buffer flags `STORAGE`, `VERTEX`, `INDEX` (types.h:418-430); heaps `DEFAULT`/`UPLOAD`/`READBACK`/`GPU_UPLOAD` (types.h:364-370); storage-buffer ranges (vk.cpp:520-527); every binding is `VK_SHADER_STAGE_ALL` (vk.cpp:1174) | none. Streaming rings are application work. `map()` never calls `vmaFlushAllocation` or `vmaInvalidateAllocation` (vk.cpp:904-917; both names absent), so it relies on coherent memory (`inference`) |
| G2 | geometry shader exposes; barycentrics need extending | `RenderGraphicsPipelineDesc::geometryShader` (types.h:1281), stage added vk.cpp:1440-1449, capability vk.cpp:4154; Metal asserts "Metal does not support geometry shaders!" (metal.cpp:1547); no barycentric extension anywhere | barycentrics: extension name (vk.cpp:75-92), a feature struct in both chains, a capability bool: about 20 lines + 6 for the other backends; fits LCD |
| G3 | needs extending | enum has `D16_UNORM`, `D32_FLOAT`, `D32_FLOAT_S8_UINT` only (types.h:126-127, 138); no D24 case in `toVk` (vk.cpp:166-191); depth+stencil copies use one `VkBufferImageCopy` with both aspects (vk.cpp:728-734, 3251) | D24S8 enum + size + `toVk`: about 15 lines (D3D12 and Metal 6 each; **Apple silicon has no D24**); a format-support query (X1): about 40 lines; the aspect split for depth+stencil readback (Vulkan allows one aspect per copy region, `memory`): about 20. **Neither real target has D24S8, so the missing enum value costs nothing there; the missing query does** |
| G4 | needs extending (interlock, mutable-format view, layouts); compare sampler and white border exposed | compare: `comparisonEnabled`/`comparisonFunc` (types.h:1399-1400, vk.cpp:1335-1336); `OPAQUE_WHITE` (types.h:327-332, vk.cpp:579-591); storage image `READ_WRITE_TEXTURE`/`STORAGE` (vk.cpp:516-517, 990) | **No interlock**: add the extension, a `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT` in the query and create chains, a capability: about 25 lines. **Images are never created `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`** (flags are set only for sample locations and cube, vk.cpp:992-998; no occurrence of `MUTABLE_FORMAT` in the file), so an `R32_UINT` view over an `RGBA8` target is invalid: about 5 lines. **The render pass and framebuffer hard-code `COLOR_ATTACHMENT_OPTIMAL`** as initial and final layout (vk.cpp:1701-1702, 2567-2568), while a storage write needs `GENERAL`: about 15 lines in two places (a `GENERAL` barrier layout exists, types.h:475). The compare-and-swap fallback needs no extension but the same two blockers. Azahar needs neither the compare sampler nor the border (5.3) |
| G5, G6 | exposes | `FORMATTED_BUFFER` texel buffers (types.h:345, vk.cpp:938-956); `TEXTURE_1D` | none |
| G7 | copy-and-barrier exposes; framebuffer fetch has no equivalent | `copyTexture`/`copyTextureRegion` (iface.h:143-145; vk.cpp:3207-3347); barriers (vk.cpp:2830-2892); no `INPUT_ATTACHMENT` descriptor type (types.h:342-355), no subpass dependency or input reference in either render-pass builder (vk.cpp:1678-1741, 2618-2634) | every `barriers()` call and every copy ends the active render pass (vk.cpp:2838, 3208). Input attachments: a descriptor type, an input reference and a self-dependency in two builders, `GENERAL`: about 70 lines, Vulkan-only, **fights LCD** (D3D12 has none, `memory`) |
| G8 | needs extending (blend constant; stencil reference) | blend factors, ops, write mask (types.h:1203-1235; vk.cpp:1563-1585); stencil, depth, cull, front face, depth bias (types.h:1283-1298, 204-215); viewport and scissor dynamic (vk.cpp:1613-1614), up to 8 render targets | **There is no blend-constant setter in any backend**: the command list has none (iface.h:114-152), the Vulkan pipeline has no `VK_DYNAMIC_STATE_BLEND_CONSTANTS` (the dynamic states are viewport, scissor and, if asked, depth bias, vk.cpp:1611-1618) and leaves `blendConstants` zero (vk.cpp:1580-1585), so a constant-colour blend gets (0,0,0,0): about 30 lines across three backends, fits LCD. The stencil reference is baked into the pipeline (vk.cpp:1602) while D3D12 and Metal apply it dynamically: about 25 lines. Vertex strides are baked into the pipeline (vk.cpp:1471), so a different stride is another pipeline (`inference`) |
| G9 | needs extending | `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `D16_UNORM` exist; **no RGB8, RGB565, RGB5A1, RGBA4, D24** (types.h:93-170) | new enum values, size entries (types.h:519-707) and `toVk` cases: about 40 lines; RGB8 has no portable 24-bit format on D3D12 or Metal, so RGB8 is an RGBA8 emulation in any design. No format-support query (X1) |
| G10 | needs extending (cache) | `vkCreateGraphicsPipelines(device->vk, VK_NULL_HANDLE, ...)` and the compute twin pass no `VkPipelineCache` (vk.cpp:1652, 1393); every graphics pipeline creates its own `VkRenderPass` (vk.cpp:1631) and every framebuffer another (2634); no dynamic rendering, pipeline library or extended dynamic state | a `VkPipelineCache` on `VulkanDevice` loaded from a blob and a `getPipelineCacheData()`: about 30 lines, Vulkan-only (**fights LCD**); a render-pass cache: about 30 more. The scratch vectors are `thread_local` (vk.cpp:1420, 1462-1464), which suggests pipeline creation was meant to run on several threads (`inference`). Without a patch the cache can only be added by replacing volk's global function pointer (hacky) |
| G11 | exposes | `BC7_UNORM`/`_SRGB` (vk.cpp:246-251); buffer-to-image `copyTextureRegion` with `PLACED_FOOTPRINT` (vk.cpp:3217-3237) | none. A worker-thread upload needs a queue: plume has **no queue-family ownership transfer** (images are `VK_SHARING_MODE_EXCLUSIVE`, vk.cpp:986; barriers use `VK_QUEUE_FAMILY_IGNORED`, 2874-2875), so upload on a second *direct* virtual queue, not on a transfer-only family |
| N1 | Vulkan exposes; Metal has no equivalent | `RenderLogicOperation`, all 16 (types.h:286-303), `logicOpEnabled`/`logicOp` (types.h:1305-1306, vk.cpp:1582-1583); D3D12 maps them (plume_d3d12.cpp:327-335); no occurrence of `logicOp` in `metal.cpp` | no capability bit: the Vulkan feature is enabled iff the device has it (vk.cpp:4081), and a pipeline asking for it on a device without it would be invalid (MoltenVK: `unverified`) |
| N2 | exposes | `TRIANGLE_FAN` (types.h:201), `triangleFan` capability (types.h:1806): **constant `true` off Apple, forced `false` on Apple** with the comment that MoltenVK "does so via compute shaders to translate to lists ... This results in renderpass restarts that can harm performance, so force disable native triangle fan support and rely on the game to emulate fans" (vk.cpp:4173-4180); Metal asserts (metal.cpp:652) | none for the Mac; the flag is not read from the device (`inference`: on another portability-subset device it would be wrong) |
| N3 | exposes | `MIN`/`MAX` (types.h:273-274; vk.cpp:400-417); Metal maps them (metal.cpp:723-725) | none; the PICA factor semantics are a shader matter (5.6) |
| N4 | needs extending | `toIndexType(R8_UINT)` returns `VK_INDEX_TYPE_UINT8_EXT` (vk.cpp:673-677), **but** `VK_EXT_index_type_uint8` is neither in the extension list nor in a feature chain | extension + feature struct + capability: about 20 lines; Metal and D3D12 have none (**fights LCD**); the widening in N2-05 is the portable route |
| N5 | exposes (no handling) | `stride` copied as is (vk.cpp:1469-1473) | none on a full device; the portability-subset repack stays ours |
| N6 | exposes (cube) | `RenderTextureFlag::CUBE` (types.h:444, vk.cpp:996-998); `TEXTURE_CUBE` views | none |
| R1 | needs extending (the structure is right) | `CreateVulkanInterface()` and `createDevice()` are separate (vk.cpp:4652-4680); the extension lists are `static const` sets (vk.cpp:46-69), so no extra instance extensions; the SDL option is **Linux-only** (CMakeLists.txt:20-21); the surface is created inside `VulkanSwapChain` from a `RenderWindow` (`HWND`, `ANativeWindow*`, `SDL_Window*`, **Xlib `{Display*, Window}` only** on Linux without SDL, or `{NSWindow*, view*}` on Apple, types.h:34-71; vk.cpp:2085-2153); no `VK_EXT_headless_surface` (no occurrence); the queue family is chosen by flags (vk.cpp:4012-4044) and present support is tested afterwards, failing with "Command queue does not support present." (vk.cpp:2155-2166) | (1) extra instance extensions: ~15; (2) an external `VkSurfaceKHR` in `RenderSwapChainDesc`: ~25; (3) headless surface: ~25; (4) a present-capable family: ~20: about **85 lines**, fits LCD. Around plume instead: build our own `VkSwapchainKHR` from plume's public handles and wrap images with the public `VulkanTexture(VulkanDevice*, VkImage)` constructor (vk.h:94): about 250 lines (`inference`), and extra instance extensions would still be impossible. **Headless works** (6.5) |
| R2 | timeline needs extending; the lock exposes | each native queue has a `std::unique_ptr<std::mutex>` (vk.h:392-396), taken in `executeCommandLists` (vk.cpp:3654-3657) and `present` (2290-2293); sync is fences and **binary** semaphores only (iface.h:68-74; vk.cpp:3533-3575); no timeline anywhere in the Vulkan backend | timeline semaphores: `VkPhysicalDeviceTimelineSemaphoreFeatures`, a signal value on submit, `VkTimelineSemaphoreSubmitInfo`: about 70 lines, fits LCD. A per-wait stage mask: ~10 (below). **All semaphore waits use the single stage `COLOR_ATTACHMENT_OUTPUT`** (vk.cpp:3633-3638), correct for swapchain acquire but not for "the compositor samples an image the renderer drew": the consuming stage is the fragment shader, which runs earlier, so the wait would not cover the sample (`inference`; a validation run would confirm). `pWaitDstStageMask` also points at one element whatever the number of waits (`waitStages`, vk.cpp:3633, 3638), which is invalid for more than one wait semaphore (`inference` from the Vulkan rule, `memory`) |
| R3 | exposes | `presentWait` capability = `presentId && presentWait` features (types.h:1796; vk.cpp:3880-3891, 3978-3985, 4164), **independent of a surface**; `present` attaches `VkPresentIdKHR` (vk.cpp:2278-2287); `wait()` calls `vkWaitForPresentKHR` with a **100 ms** timeout (vk.cpp:2309-2316); `displayTiming` = `VK_GOOGLE_display_timing` listed (vk.cpp:4165), used only by `getRefreshRate()` (2455-2464); **MAILBOX is detected (vk.cpp:2201) but not used**, FIFO or IMMEDIATE only (vk.cpp:2415-2425; commit 2a66aee, 2026-01-16, reverted it as the default "as it causes presentation issues"); resize returns false at 0x0 (vk.cpp:2321-2324) | `PresentWaitAvailable()` = `getCapabilities().presentWait`. On the RX 6700 XT plume would report both false (`inference` from the probe). The capability sits on the device while N2-21 says "with a surface". Plume's Metal backend sets `presentWait = true` unconditionally (metal.cpp:3876): what that measures is a Mac question |
| R4 | exposes | all backend types are public structs in `vk.h`: `VulkanInterface::instance` (:452), `VulkanDevice::vk`/`physicalDevice`/`queueFamilyIndices`/`queueFamilies[i].queues[j].vk`/`.mutex` (:392-412), `VulkanCommandList::vk` (:297), `VulkanFramebuffer::renderPass` (:269) | none. The experiment printed the instance, physical device, device, family indices and queue counts by `static_cast` |
| R5 | exposes | readback: `copyTextureRegion` into a `ReadbackBuffer` then `map` (iface.h:143, vk.cpp:3238-3262; commit #114 is the one at HEAD); timestamps `writeTimestamp` and `queryResults`, converted to nanoseconds (vk.cpp:2741-2749) | `queryResults` does not pass `VK_QUERY_RESULT_WAIT_BIT` (vk.cpp:2713), so the fence is waited first. `ReadImage` is about 40 lines of ours over `copyTextureRegion`. No image-to-image blit and no format conversion (no `vkCmdBlitImage`), so conversions between PICA formats and the Morton write-back are compute work of ours |
| R6 | exposes | `VOLK_IMPLEMENTATION` and `VMA_IMPLEMENTATION` at the top of `vk.cpp` (:8-9); `volkInitialize()` failure prints and `CreateVulkanInterface()` returns `nullptr` (vk.cpp:4498-4502, 4677-4680); the Vulkan headers are vendored (CMakeLists.txt:89-94) | **plume owns the one volk and the one VMA implementation**: `native/third_party/rhi/` must not compile a second one. Instance-level pointers are loaded, device-level ones are not (`volkLoadDevice` absent; only `volkLoadInstance`, vk.cpp:4623), so device calls go through the loader's dispatch (`inference`: a small per-call cost). The failure is `fprintf` plus a null return, not an error object |
| R7 | exposes | built and run headless on lavapipe (6.5) | a validation-layer run was not done |
| R8 | Vulkan exposes; Metal needs extending | 6.3 | run-time MSL for Metal: about 50 lines in `MetalShader` |
| R9 | no equivalent | zero matches for raster order or framebuffer fetch anywhere ([6.7](#67-d4s-row-checked-against-the-headers)); no logic ops, geometry, fans or 8-bit indices in `metal.cpp` | [7](#7-metal) |
| X1 | no equivalent | no `vkGetPhysicalDeviceFormatProperties` call in `vk.cpp` | about 40 lines + equivalents in the other two |
| X3 | no equivalent for `*_SSCALED`/`*_USCALED`; the integer formats exist (`R8G8B8A8_UINT`/`SNORM`/`SINT`, `R16G16B16A16_SINT`, ...) | no `SSCALED`/`USCALED` in the headers or `vk.cpp` | the integer fallback with shader conversion (Azahar's own fallback) works without a patch |

### 6.2 Device creation: what plume requires and what it takes

`source: plume@0ce184b plume_vulkan.cpp`.
- **Required instance extensions:** `VK_KHR_surface` and the platform's surface extension (`VK_KHR_win32_surface`, `VK_KHR_android_surface`, **`VK_KHR_xlib_surface` on Linux without SDL**, `VK_EXT_metal_surface` on Apple); `VK_EXT_debug_utils` in non-`NDEBUG` builds. A missing one prints "Missing required extension" and returns no instance (:46-69, 4577-4584). Optional: `VK_KHR_portability_enumeration` on Apple. The validation layer is enabled in debug builds if present.
- **Version.** It requests `apiVersion` 1.2 (:4509); no check of the device's version anywhere. Whether a 1.1-only driver works is `unverified`.
- **Required device extension:** `VK_KHR_swapchain` only (:71-73); without it `createDevice` returns `nullptr`.
- **Taken when offered (15 names, :75-92):** descriptor indexing, scalar block layout, robustness2, buffer device address, sampler mirror clamp, acceleration structure, ray-tracing pipeline, pipeline library, deferred host operations, sample locations, load/store op none, `present_id`, `present_wait`, `VK_GOOGLE_display_timing`, and `VK_KHR_portability_subset` ("the Vulkan spec requires this to be enabled if supported"). Missing optional ones only turn a capability off. The ray-tracing features are switched on wherever offered (`raytracing=true` on lavapipe, :3917-3928), which adds enabled features nobody uses.
- **Features:** the whole queried core `VkPhysicalDeviceFeatures` is passed as `pEnabledFeatures` (:3930-3933, 4081), so `geometryShader`, `logicOp`, `fragmentStoresAndAtomics` and the rest are on whenever the device has them. Extension feature structs are chained only for the optional extensions above. **There is no struct for interlock, barycentrics, index-uint8, custom border colours, timeline semaphores, synchronization2, dynamic rendering, extended dynamic state or the pipeline library**; none of those names appears in the file (the one hit, `VK_KHR_pipeline_library` at :83, is a ray-tracing dependency).
- **Selection.** The physical device is picked by type (discrete 4, integrated 3, virtual 2, CPU 1) or by exact name (:3783-3818); queue and present support are not part of the choice. Queues: one family per command-list type chosen by flags (direct = graphics+compute+transfer; compute; copy = transfer), up to 4 native queues per family, with virtual queues spread over them (:4012-4063, 3737-3758).
- **Consequences.** *Lavapipe:* all required extensions are present; the experiment ran (6.5). *The portability subset (MoltenVK):* plume chains the subset features and enables the extension (:3909-3915, 3999-4002); `triangleFan` is forced false on Apple; there is no `logicOp` capability bit. *Windows with the AMD driver:* nothing is required beyond the swapchain; everything the probe found useful (interlock, barycentrics, uint8, custom border, timeline, dynamic rendering, pipeline library) stays off until patched in, which is what the shadow write (G4) and the timeline (R2) need.

### 6.3 The shader story

- `RenderShaderFormat` is `DXIL`, `SPIRV` or `METAL`; each backend reports one: Vulkan `SPIRV` (vk.cpp:4626), D3D12 `DXIL`, Metal `METAL` (types.h:377-382). `createShader(data, size, entryPointName, format)` takes bytes (iface.h:229).
- **Vulkan:** the bytes go straight to `vkCreateShaderModule` (vk.cpp:1285-1305); plume contains no compiler, reflection or rewrite. Glslang's SPIR-V goes in unchanged. Specialization constants are `uint32` pairs (types.h:1244-1254).
- **The descriptor model has separate sampled textures and samplers and no combined image sampler** (`RenderDescriptorRangeType`, types.h:342-355; `COMBINED_IMAGE_SAMPLER` does not occur in the code). Azahar's GLSL uses combined samplers (`eCombinedImageSampler` for tex0..tex2, `vk_pipeline_cache.cpp:69-77`), so shaders written for it need `texture2D` + `sampler` pairs, or the RHI must lower a combined sampler to two descriptors (`inference`; affects D5's fragment generator, not D27's vertex shaders, which sample nothing).
- **Metal:** a **compiled Metal library (metallib) blob**: `MetalShader` asserts `format == METAL` and calls `newLibrary(dispatchData, ...)` (metal.cpp:1332-1352). plume's own examples reach it offline: HLSL, DXC, SPIR-V, SPIRV-Cross (MSL 2.1, argument buffers, push constants pinned to buffer index 8, `flip_vert_y`, a post-pass padding descriptor sets), `xcrun metal`, `xcrun metallib` (`examples/cmake/PlumeShaders.cmake:127-175`, `examples/cmake/tools/spirv_cross_msl.cpp:74-175`). Shaders generated at run time (D27, D5) cannot reach it without a patch (about 50 lines; the call has precedent: `metal.cpp:4097, 4154` compile MSL *text* for plume's own internal resolve and clear shaders, so the research note's "no MSL-text compile path" was true of `createShader` only). An end user's Mac has no `xcrun metal` without Xcode (`memory`).
- **D3D12:** DXIL only; nothing in our pipeline produces it; not applicable.

### 6.4 The barrier and layout model, for a renderer that samples what it just rendered

- Command lists own their pool, so recording on several threads is safe; per-list state (`targetFramebuffer`, `activeRenderPass`) is in the list (vk.h:296-310).
- **Layout and stage tracking live in the resource** (`VulkanTexture::textureLayout`, `barrierStages`) and are updated **while recording** (a texture's: vk.cpp:2883-2884; a buffer's `barrierStages`: 2862-2863). Two threads recording barriers for the same texture race, and a list recorded earlier but submitted later sees stale layouts (`inference`).
- `barriers()` ends the active render pass first (:2838), uses `srcStageMask = TOP_OF_PIPE | the last barrier's stages` and `dstStageMask = BOTTOM_OF_PIPE | the requested stages`, access masks `MEMORY_READ | MEMORY_WRITE` on both sides with a `// TODO` (:2842-2873), and covers the whole resource (all mips and layers). Stage groups are only graphics, compute and copy (types.h:450-460).
- Render passes: one subpass, no dependencies, `LOAD_OP_LOAD`/`STORE_OP_STORE`, fixed `COLOR_ATTACHMENT_OPTIMAL` (:1678-1741, 2618-2634). A pass starts at the first draw or clear after `setFramebuffer` and ends at the next barrier, copy, `setFramebuffer` or `end()`.
- **What it means (inference):** a draw that samples a surface rendered earlier in the frame needs a barrier to `SHADER_READ` and back, and each barrier ends the pass, so the next draw begins a new one with load and store (on a tile-based GPU such as the M2 that is a tile round trip; unmeasured). A draw that samples the surface it is drawing to is not expressible (the barrier ends the pass and the layouts are fixed): Azahar's feedback copy (5.6) would be needed, and plume has no whole-image copy-with-resume operation beyond `copyTexture`. For the capture player, which replays serially, this is workable; for a renderer thread plus a present thread sharing images it needs an owner-per-resource rule of ours. The README says exactly these parts, "barrier management and texture transitions", are still being refined.

### 6.5 The experiment: plume's Vulkan backend on lavapipe, headless

Built out of tree (plume's own `CMakeLists.txt` unmodified, plus a 110-line test program) with clang 18.1.3, Debug; nothing built inside the clone or the repository:

```
git clone --shallow-since=2025-09-01 --recurse-submodules --shallow-submodules https://github.com/renderbag/plume <dir>
cmake -S <test> -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DPLUME_SRC=<dir>
cmake --build <build> && <build>/plume_probe
```

The first link failed with `undefined reference to XGetWindowAttributes` (`vk.cpp:2479`); adding `X11` to the link fixed it, so the Linux build needs the Xlib library even for a program that opens no window. The whole configure, build and link took under a minute. Output (verbatim; the `image=`/`view=` pointers abbreviated):

```
interface shaderFormat=2 (1=DXIL 2=SPIRV 3=METAL)
device name listed: llvmpipe (LLVM 20.1.2, 256 bits)
description: name='llvmpipe (LLVM 20.1.2, 256 bits)' type=4 vendor=0x10005 driverVersion=0x6402008 dedicatedVideoMemory=16872804352
caps: geometryShader=true raytracing=true raytracingStateUpdate=false sampleLocations=false resolveRegion=true resolveModes=false
caps: descriptorIndexing=true scalarBlockLayout=true bufferDeviceAddress=true samplerMirrorClampToEdge=true presentWait=false displayTiming=false
caps: maxTextureSize=16384 preferHDR=true triangleFan=true dynamicDepthBias=true uma=false gpuUploadHeap=false queryPools=true
raw: VkInstance=0x559498fed2d0 VkPhysicalDevice=0x559499002130 VkDevice=0x559499041010 apiVersion=1.4.318 appInfo.apiVersion=1.2
raw: queueFamilyIndices direct=0 compute=0 copy=0; native queues per family: [0]=1
format R8G8B8A8_UNORM     vulkan: attachment=1 sampled=1 storage=1 | plume createTexture: image=... view=...
format B8G8R8A8_UNORM     vulkan: attachment=1 sampled=1 storage=1 | plume createTexture: image=... view=...
format R16_UINT           vulkan: attachment=1 sampled=1 storage=1 | plume createTexture: image=... view=...
format BC7_UNORM          vulkan: attachment=0 sampled=1 storage=0 | plume createTexture: image=... view=...
format D16_UNORM          vulkan: attachment=1 sampled=1 storage=0 | plume createTexture: image=... view=...
format D32_FLOAT          vulkan: attachment=1 sampled=1 storage=0 | plume createTexture: image=... view=...
format D32_FLOAT_S8_UINT  vulkan: attachment=1 sampled=1 storage=0 | plume createTexture: image=... view=...
BC7 as colour target: createTexture returned non-null object=1, VkImage=0x5646477e41f0 (null means failed; no exception, no fallback)
clear+readback: pixel0 = 255 128 64 255 (expected 255 128 64 255)
timestamps (ns): 913874360091 913874719156 delta=359065
```

- An interface and a device are made with no window, no SDL and no X server (exit 0 with `DISPLAY` unset); the Linux build still asks the loader for `VK_KHR_xlib_surface`, which this loader offers.
- Clear, copy and readback gave the expected bytes; two timestamps increased. The "attachment/sampled/storage" numbers come from the test program's own `vkGetPhysicalDeviceFormatProperties` call, not from plume.
- **plume does not check format support:** BC7 has no colour-attachment support on this device, yet `createTexture` returned a non-null image. For a format a device lacks the behaviour is "pass it on; if `vmaCreateImage` fails, print and leave the handle null" (vk.cpp:1012-1015): no capability, no fallback.
- plume's three capabilities agree with the project's probe on the same device: `triangleFan=true` with `triangle fans yes`, `geometryShader=true` with `geometry shaders yes`, `presentWait=false` with `present_wait no`. `triangleFan` is a constant, so that agreement says only that lavapipe is a full device.
- Not done: a Release build, a validation-layer run (the layer is not installed), a swapchain (no display), a multi-thread submission test, any pipeline (so geometry-shader, fan and logic-op pipelines are read from code, not run).

### 6.6 The project

- **Licences:** MIT, "Copyright (c) 2024 renderbag and contributors" (LICENSE:1-3). Vendored: volk (MIT), VulkanMemoryAllocator (MIT), Vulkan-Headers 1.4.335 (Apache-2.0 OR MIT), D3D12MemoryAllocator (MIT, Windows only), metal-cpp (Apache-2.0; committed into the tree, 1.4 MB, not a submodule). MIT and Apache-2.0 code can go into a GPL-3.0 work (`memory`: Apache-2.0 is compatible with GPLv3, not GPLv2; not checked against the FSF page).
- **Size:** `wc -l`: `plume_vulkan.cpp` 4,682; `plume_metal.cpp` 4,280; `plume_d3d12.cpp` 4,263; `plume_render_interface_types.h` 1,822; `plume_metal.h` 784; `plume_d3d12.h` 495; `plume_vulkan.h` 469; builders 281; interface 268; about 17,500 for the 11 top-level files. The Vulkan-only slice (cpp + h + the three shared headers) is **7,522**. Vendored: `volk.c` 3,577, `volk.h` 2,725, `vk_mem_alloc.h` 19,111.
- **Backends are chosen by platform, not by a CMake option:** Vulkan always; Metal `if(APPLE)`; D3D12 `elseif(WIN32)` (CMakeLists.txt:57-83). To ship Vulkan only on Windows we list the sources ourselves (`inference`). The SDL options are Linux-only and exclusive.
- **Activity:** 76 commits from 2025-02-28 to 2026-10-21, no tags, no releases. Commits per month for the last 12: 2025-11 0, 2025-12 17, 2026-01 5, 02 1, 03 0, 04 0, 05 4, 06 0, 07 1, 08 0, 09 3, 10 3: **34 in the 12 months**, 9 distinct author names (two of them, "Darío" and "Dario", are probably one person: `inference`); 14 of the 34 subjects name Metal or D3D12 (a case-insensitive search of the subjects; the research note said 12). Earlier months were busier (2025-03 8, 2025-07 13, 2025-08 20). Of those 34 commits, 11 touch `plume_vulkan.cpp`, 5 `plume_vulkan.h`, 6 `plume_render_interface_types.h`, 1 `plume_render_interface.h`. A public-API change in the window: `createSwapChain` now takes a `RenderSwapChainDesc` (2026-01-08, `dea63d3`).
- **Users, as far as the repository says:** RT64 only (README.md:3). The brief's mention of Unleashed Recompiled is `unverified`.
- **Tests: none.** No test directory; the only workflow, `validate.yml`, builds the library and the triangle example on five platforms and runs nothing. `RenderInterfaceTest` and `TestInitialize`/`TestDraw`/... in the interface header (iface.h:261-265) are example hooks.
- **README:** "While feature-rich and stable, it is not considered to be production-ready or to have stable API until a few crucial components such as barrier management and texture transitions are further refined" (README.md:3).
- **Contribution policy:** `CONTRIBUTING.md` line 1 reads "AI must not be used to generate any sort of contributions to this project, including code, issue reports or pull requests." Consequence for the options: patches written in AI-assisted sessions, which is how this repository is worked on (its `CLAUDE.md` says so), cannot go upstream, so a vendored copy with a patch queue stays ours.

### 6.7 D4's row checked against the headers

D4 says plume has "no interlock, raster order groups or framebuffer fetch exposed; unstable API". A case-insensitive search of every `plume_*.cpp`, `plume_*.h` and `plume_apple.mm` for `interlock`, `raster order`, `RasterizerOrdered`, `ROV`, `input attachment`, `INPUT_ATTACHMENT`, `subpassLoad`, `framebuffer fetch`, `programmableBlend` and `imageblock` finds **zero matches** (re-run in this work, 2026-10-23; the research note's list also holds `raster_order`, the same result). **Confirmed.** "Unstable API" is confirmed by README.md:3 (which also calls it "stable" in the same sentence) and by the `RenderSwapChainDesc` change above. Nothing contradicts the row; [11](#11-corrections-to-earlier-documents-found-while-checking) lists what the row lacks.

### 6.8 The cost in lines (inference throughout)

**As it is (option 1).** N2-21 gets for free: device and queues with a submit mutex, VMA, buffers, textures, views, samplers, pipelines, implicit render passes, copy and readback, timestamps, swapchain with FIFO/IMMEDIATE and present wait, volk loading, SPIR-V in, cube, BC7, D16/D32S8, logic ops, min/max, fans (Apple forced false), geometry shaders. Built around plume:

| Item | Size (our lines) | Consequence |
|---|---:|---|
| R1 surface from SDL3, extra extensions: own swapchain over plume's handles | 250 | duplicates plume's swapchain |
| R2 timeline values: one binary semaphore per producer-consumer pair, fences | 60 | the stage-mask problem stays, so likely an extra CPU wait |
| G4 interlock | n/a | not possible; the compare-and-swap loop needs the two blockers too: shadow writes lose pixel ordering (impact unmeasured) |
| G8 blend constant | 100+ | a pipeline per constant colour, or blending moved into the shader (needs a destination read) |
| G9, G3 narrow and 24-bit formats | 200 | readback needs conversion shaders anyway |
| G10 pipeline cache | 40 | the volk-pointer wrapper (hack), else none |
| X1 format queries: call `vkGetPhysicalDeviceFormatProperties` ourselves | 40 | fine |

About **700 to 900 lines of workaround code**; N2-21 stays M; N2-22, N2b-05 and N2b-09 inherit the weaker synchronisation. The census-seen needs that become fallbacks: G4, G8, G3/G9, G10.

**Extended (options 2a, 2b).** The patch queue from 6.1, Vulkan lines:

| Patch | Lines |
|---|---:|
| P1 instance extras, external and headless surface, present-capable family (R1) | 85 |
| P2 feature chain: interlock (G4), barycentrics (G2), uint8 (N4), timeline (R2), capability bits | 100 (+10 each in D3D12 and Metal) |
| P3 timeline-value submit API and a per-wait stage mask (R2) | 70 + 10 (+80 across D3D12 and Metal) |
| P4 pipeline cache and render-pass cache (G10) | 60 |
| P5 blend constants and a dynamic stencil reference (G8) | 12 + 25 (+30 across D3D12 and Metal) |
| P6 formats D24S8, RGB565, RGB5A1, RGBA4, (RGB8) and a format-support query (G3, G9) | 50 + 40 (+100 across D3D12 and Metal) |
| P7 mutable-format flag, `GENERAL` attachment layouts, optional input attachments (G4, G7) | 20 + 70 |
| P8 depth/stencil aspect split, MAILBOX, wait timeout (G3, R3) | 40 |
| P9 run-time MSL in the Metal backend (R8; Mac native only) | 50 |

About **500 Vulkan lines (7 percent of the 7,522-line slice)**, plus about 300 to keep D3D12 and Metal compiling and honest. The note's reading of D3D12 and Metal sources is that they are not referenced from the Vulkan files (the shared header has `// D3D12 only` members but no code dependency, `inference`), so deleting them leaves a Vulkan-only RHI of about 7.5k lines plus volk and VMA, but then a native Metal backend has nothing to extend. In plan units: the patch queue is about one S (150k tokens: writing and testing about 500 lines with a lavapipe test per patch); N2-21 shrinks because the 700 to 900 workaround lines are not needed, but stays M. P9 plus interlock and fetch in Metal is a separate M to L item that was not estimated, because the Metal render-pass code was not read. **Maintenance** (inference): upstream's last 12 months touched the functions we would patch (`VulkanDevice::VulkanDevice` at vk.cpp:3768-4188, `VulkanSwapChain`, `executeCommandLists`), so each touch is a rebase conflict; a pinned snapshot has none and gets no fixes; pinning `0ce184b` and re-basing only for specific upstream fixes is the cheapest regime, and a patch queue of numbered files keeps each conflict local.

## 7. Metal

The rule of [`metal.md`](metal.md) (3.2, stage 3.5): framebuffer fetch (`[[color(n)]]`) and raster order groups are items of a *native* Metal backend, unless the Vulkan-on-Metal driver already gives them. On the Mac the port runs on MoltenVK now ([`metal.md`](metal.md) 3.1); a native backend starts only on a trigger (3.3).

### 7.1 Which needs meet which mechanism

| Need | Seen in those scenes? | Metal mechanism it would use | MoltenVK 1.4.2 gives it? | KosmicKrisp gives it? |
|---|---|---|---|---|
| G4 shadow-write ordering | seen | **raster order groups** (8 per fragment function on Apple4 and later) | **yes**: `VK_EXT_fragment_shader_interlock` (pixel and sample) listed, mapped to raster order groups; not run | **no**: no interlock extension; the compare-and-swap loop is the only route |
| N1 all 16 logic ops | not seen | **framebuffer fetch** (no logic-op API in Metal) | **no**: `logicOp` false (private-API builds only); the shader path does 4 of 16 | yes: `logicOp` true, a shader-side blend lowering whose destination read is probably a fetch (`inference`) |
| N3 min/max blending | not seen | Metal has native `min`/`max`; framebuffer fetch only where the PICA factor semantics need a shader path (not established) | ops yes; the PICA factor case uses the copy path | not read |
| G7 gas | seen (cutscene) | **framebuffer fetch**, if the mode reads the destination colour (open, N2-10) | no framebuffer fetch (`inference`); local read listed, untraced | open |
| G2 quaternion fix-up | seen | neither: no geometry stage; `barycentric_coord` and `primitive_id` exist (Apple7); per-vertex access only on Apple10; the routes of `metal.md` 3.2 (a buffer written by the vertex stage and read by `primitive_id`; mesh functions) are untried | no: barycentrics listed but unusable; no geometry | no |
| N2 fans, N4 8-bit indices, N5 strides | not seen | neither: Metal has no fan type and no 8-bit index; strides are 1 B on Apple4 and later | yes (fans emulated, indices converted, alignment 1 on Apple8) | yes (fans emulated) |
| G1, G3, G5, G6, G8 to G11 | seen | neither: backend features or conversions | see [3.3](#33-the-m2-stand-in) | |

**What neither gives.** Inside a Metal render pass, Apple3 to Apple10 cannot make a fragment-stage memory barrier, so "copy and barrier" on a Mac is a pass break (X6), not a barrier (`FST p.6`, note 3).

### 7.2 What a later Metal backend would have to add, per option

| Option | What a native Metal backend adds | State of the base |
|---|---|---|
| 1 plume as it is | raster order groups, framebuffer fetch, logic ops, a run-time MSL path, and everything the Vulkan side needed for G4 | plume's Metal backend (4,280 lines) was read by grep only: it has no raster order groups, no fetch, no logic ops (`metal.cpp`: no occurrence), asserts on geometry (:1547) and fans (:652), maps only 16- and 32-bit indices, and takes only a metallib. How much of its render-pass and barrier code survives was not read |
| 2a plume extended | the same, as patches in `metal.cpp`: P9 (about 50 lines), raster order groups through SPIRV-Cross's output of `SPV_EXT_fragment_shader_interlock` (`memory`, `unverified`), fetch through `[[color(n)]]` inputs and pass configuration (not estimated) | as above, plus 300 lines of our patches to keep honest |
| 2b plume's Vulkan backend vendored | a Metal backend of our own against the interface that plume's types give us; plume's Metal source is deleted | the interface is Metal-shaped (argument-buffer style descriptors, no combined samplers) |
| 3 our own thin RHI | a Metal backend of the RHI: about **2,400 to 4,000 lines** (`inference`: the sum of `metal.md` 2.3's rows for device and capabilities 300 to 500, submission 400 to 600, stream buffers 150 to 250, render passes 200 to 300, pipelines and state 900 to 1,300 and the shader glue 900 to 1,500; the texture runtime, blit helper, rasterizer and presentation rows belong to the renderer above the RHI or to N2b). `metal.md`'s 6,000 to 9,000 lines is the scope of a native renderer for Azahar's `video_core`, not of an RHI backend | designed for it: the interface keeps the Vulkan idioms out ([9.4](#94-rhi-design-requirements-the-evidence-imposes)) |

### 7.3 New facts that bear on `metal.md`'s trigger rule

- **MoltenVK is deprecated** by Khronos on `main` (2026-10-13, README notice; the commit is `0aa76ad`): maintained "by the community on a best-effort basis"; Khronos recommends KosmicKrisp on Apple Silicon and keeps MoltenVK for Intel Macs and iOS. No release since 1.4.2 (2026-07-20). `source: MoltenVK@main README.md:6-14`. The "Platform" trigger of `metal.md` 3.3 (MoltenVK and KosmicKrisp both break on a macOS release and stay broken) is no nearer by itself, but the first of the two now has no maintainer commitment.
- **KosmicKrisp needs macOS 26 or later** (Metal 4; `Mesa@f3f63ef docs/drivers/kosmickrisp.rst:4-8`). `metal.md` 3.2 says macOS 15.
- **KosmicKrisp has no interlock and no barycentrics**, has `logicOp`, and offers custom border colours only behind `MESA_KK_EXPERIMENTAL=custom_border`. If the Mac follows Khronos' recommendation, the shadow write there is the compare-and-swap loop (atomic, not primitive-ordered; whether the result differs on the captured Arena scene is `unverified`). If that shows visibly, `metal.md`'s "Picture" trigger fires, and a native backend's raster order groups are the way back to ordered writes (`inference`).
- **MoltenVK's `present_wait` is released at command-buffer completion**, not at display. [D57](../native/DECISIONS.md) measures "p99 ≤ 8.3 ms" from displayed times; on MoltenVK `PresentWaitAvailable()` would say yes while measuring something earlier. `VK_GOOGLE_display_timing` (listed on MoltenVK, not on KosmicKrisp) is fed by the drawable's presented time. `source: MoltenVK@v1.4.2 MVKSwapchain.mm:246-285`, `MVKImage.mm:1677-1681, 1712-1724`. The RHI must report what its wait measures ([9.4](#94-rhi-design-requirements-the-evidence-imposes)).
- **MoltenVK does not give framebuffer fetch** (4 of 16 logic ops stay emulated): `metal.md` 3.2's "On Apple GPUs, MoltenVK can turn input attachments into framebuffer reads (my understanding)" is not found in 1.4.2's source ([11](#11-corrections-to-earlier-documents-found-while-checking)).
- **MoltenVK 1.4.x has fans and, on Apple8, byte strides**; the "cheap fixes in the shared Vulkan renderer" of `metal.md` 3.2 stage 2 that turn fans into lists are still right for speed (plume's own comment: the emulation "results in renderpass restarts"), but not for correctness.

## 8. The three options

The options are D4's: (1) plume as it is, (2) plume extended, (3) a thin RHI of our own over Vulkan, shaped so that a Metal backend fits later, with Azahar's Vulkan backend as a reference under D6. SDL_GPU is already ruled out (D4). Option 2 comes in two forms here, **2a** (D4's: a vendored copy with patches, all three backends kept) and **2b** (the variant the plume notes suggest and D4 does not name: vendor only plume's Vulkan backend, trimmed, MIT licence kept, as the starting point of our own RHI).

Plan units: S about 150k tokens, M about 400k, L about 1M. Every cost below is `inference`. The conversion used: the plume note sizes a patch queue of about 500 Vulkan lines, with a lavapipe test per patch, at one S; so about 3 lines of tested code per 1,000 tokens, which puts 4,000 to 5,500 lines at 1.2M to 1.8M. The plan's own N2-21 is an M and assumes the RHI is ready ("the RHI (or plume)").

### 8.1 What each option is

- **Option 1, plume as it is.** The pinned commit `0ce184b`, taken whole into `native/third_party/rhi/plume/` (with its `contrib/` volk, VMA and Vulkan-Headers), unmodified, driven through its public API; everything missing is built *around* it (6.8). Maintained by renderbag and contributors; no tags or releases; 34 commits in the last 12 months.
- **Option 2a, plume extended.** The same copy plus a numbered patch queue P1 to P9 (6.8), all three backends kept compiling. Maintained by us; upstream will not take the patches (6.6).
- **Option 2b, plume's Vulkan backend, trimmed.** Only `plume_vulkan.*` and the three shared headers (7,522 lines), volk, VMA and the Vulkan headers; D3D12 and Metal sources, `metal-cpp` and D3D12MA deleted; plume's licence kept beside the code and listed in `native/THIRD_PARTY.md`; the patch queue P1 to P8 applied and then free to reshape. **How it differs from 2a:** no obligation to keep D3D12 and Metal compiling (about 300 lines saved) and about 10,000 lines of D3D12 and Metal code and a 1.4 MB `metal-cpp` not carried; but a native Metal backend then has nothing of plume to extend. **How it differs from 3:** it starts from 7.5k lines of working code with plume's interface shape (`RenderInterface`, `RenderDevice`, `RenderCommandList`, descriptor-set builders) instead of an empty directory and Azahar's pattern; the sync and layout model is plume's until we replace it.
- **Option 3, our own thin RHI over Vulkan.** An interface at the level of Azahar's operation list (5.8), written in C++20 under `native/src/gpu/backend/`, with `native/third_party/rhi/` holding only volk and VMA; Azahar's graphics layer (3,525 lines, plus the image, barrier and descriptor code of the texture runtime and pipeline cache) as the reference to copy from under D6, with the provenance headers; plume (MIT) as a second reference (swapchain recreation, descriptor-layout building, headless use); a Metal backend later, as an RHI backend ([7.2](#72-what-a-later-metal-backend-would-have-to-add-per-option)).

### 8.2 Which needs each option meets

N = natively, as plume or the RHI stands; P = by a patch to plume (lines, 6.1); O = by our own code around plume (option 1) or inside our RHI (option 3); × = not met.

| ID | Option 1 | Option 2a / 2b | Option 3 |
|---|---|---|---|
| G1 | N | N | O |
| G2 | N (geometry shader); barycentrics ×; on the Mac the fix-up is skipped anyway | P barycentrics (~20) | O |
| G3 | O (D32S8 path and our own format query; plume's enum has no D24, which the real targets lack anyway) | P query (~40) and aspect split (~20) | O |
| G4 | **×**: no interlock, no mutable-format flag, fixed layouts; the compare-and-swap loop needs the last two | P interlock (~25), flag (~5), layouts (~15) | O (shadow write both ways, 5.3) |
| G5, G6 | N | N | O |
| G7 | N (copy and barrier) | N; P input attachments (~70) if N2-10 needs them | O (open) |
| G8 | O: blend constant only by a pipeline per colour or a shader destination read (100+); stencil reference baked | P blend constants and dynamic stencil reference (~55) | O |
| G9 | O: RGBA8 and D32S8 emulation, conversion shaders (~200) | P formats (~50) | O |
| G10 | O: a volk function-pointer wrapper (~40), no on-disk cache without it | P `VkPipelineCache` and render-pass cache (~60) | O |
| G11 | N (upload on a second direct queue, not a transfer family) | N | O |
| N1 | N (Vulkan hardware); Mac: O (shader emulation, 4 of 16) | same | O |
| N2, N3, N6 | N (fans: Apple forced false, so the rewrite is ours) | N | O |
| N4 | O (widen) | P (~20) or O (widen) | O |
| N5 | N | N | O |
| R1 | O (~250: our own swapchain over plume's handles) | P (~85) | O |
| R2 | O (~60: binary semaphore per pair; the stage-mask defect stays) | P timeline (~70) and stage mask (~10) | O |
| R3, R4, R5, R6, R7 | N (`ReadImage` and the Morton write-back are ours in every option) | N | O |
| R8 | N (Vulkan); Metal ×: metallib only | N; Metal P9 (~50) | O (SPIRV-Cross on a Metal backend) |
| R9 | ×; plume's Metal has neither | P in `plume_metal.cpp` (not estimated) for 2a; for 2b a new Metal backend | O in a later Metal backend |
| X1 | O (~40) | P (~40) | O |

### 8.3 Cost in plan units, by work package

| WP | Option 1 | Option 2a | Option 2b | Option 3 |
|---|---|---|---|---|
| N2-21 Backend foundation | stays M, with 700 to 900 lines of workaround code (6.8); **shadow writes degraded** | stays M, shrinks in risk; plus a new patch WP **N2-21p** of about S for the Vulkan patches and about a half S for the other two backends (about M) | N2-21 shrinks to about S (wrapping and policy); a new **N2-21p** of about M: vendor, trim, patch, a test per patch, because plume has none | **grows from M to about L or more** (4,000 to 5,500 lines plus about 1,500 of tests; or split into an **N2-21a RHI core** of L and an N2-21 of M on top) |
| N2-05 Vertex loading | unchanged | unchanged | unchanged | unchanged (spec text changes, 9.5) |
| N2-07 Fragment design | must design shadows without interlock (a destination-copy design that Azahar does not have) | unchanged | unchanged | unchanged |
| N2-12 Shadows | grows by about S (no valid path; copy per draw) | unchanged | unchanged | unchanged; both paths are built and tested through the mask |
| N2-13 Framebuffer operations | unchanged; blend constant needs the 100+ line workaround | unchanged | unchanged | unchanged (logic-op emulation of all 16 on the Mac either way) |
| N2-16 Pipeline cache | no on-disk cache without a hack | unchanged | unchanged | unchanged (cache API in the RHI) |
| N2-22, N2b-05, N2b-06 | inherit binary semaphores and a second swapchain (+250 lines) | unchanged | unchanged | unchanged |
| Total over the plan's M for N2-21 | about +S, with degraded G4 | about +M | about +M to +M/2 | about +L (`inference`), i.e. 1 to 2 M packages more than 2b |

### 8.4 Risks, what each leaves out, and the Mac path

| Option | Risks | Leaves out | The Mac path |
|---|---|---|---|
| 1 | G4 has no valid path; blend constant is (0,0,0,0) in every blend that uses it; no timeline; no on-disk pipeline cache; plume's `triangleFan` is a constant; state that races (6.4) | the census-seen shadow pass | plume's Vulkan backend runs on MoltenVK (portability subset handled); `triangleFan` false on Apple; interlock not enabled even where MoltenVK offers it |
| 2a | a permanent fork with no tests upstream and no upstream fixes; every upstream touch of a patched function is a rebase conflict; 300 lines to keep two unused backends honest; the sync model is plume's, which its README calls unrefined | nothing the notes found | patches add interlock for MoltenVK; native Metal later by patching `plume_metal.cpp` (not read end to end) |
| 2b | as 2a, minus the dead code; 7.5k lines of foreign code we must understand and test; the barrier and layout model must be replaced anyway if it is to share images across threads | the Metal route through plume | as 3, with the interface shaped by plume |
| 3 | the cost (about 1 to 2 M packages more than 2b); a larger new surface to test; mistakes in synchronisation that Azahar's design avoids by keeping everything in `GENERAL` | nothing; but it has no backend for D3D12 (D4: only if needed) | as 2b |

### 8.5 The options side by side

| | 1 plume as it is | 2a plume extended | 2b plume's Vulkan backend, trimmed | 3 own thin RHI |
|---|---|---|---|---|
| Meets the census-seen needs | **no** (G4 shadow write; G8 constant; G10 cache) | yes, after about 500 Vulkan lines | yes, after about 500 lines and a rework of sync | yes, by design |
| Code owned | about 800 workaround lines | 7.5k + 10k foreign lines and a patch queue | 7.5k foreign lines, reshaped | about 4 to 5.5k new lines, Azahar as reference |
| Cost, inference | N2-21 M, degraded | about +M | about +M (to +M/2) | about +L (1 to 2 M more than 2b) |
| Upstream | follows renderbag's public structs | permanent fork; patches cannot go back | permanent fork | none |
| Tests that exist for the base | none | none | none | ours from the start |
| Synchronisation model | plume's (one stage mask, resource-held layouts) | plume's, patched | plume's, replaced as needed | Azahar's proven pattern, hidden behind the interface |
| Metal later | plume's Metal lacks all three; metallib only | patches in `plume_metal.cpp` | new backend | new RHI backend, about 2.4k to 4k lines |
| Lavapipe (R7) | runs (headless experiment) | runs | runs | will run (a design requirement) |
| Evidence strength | read + one headless run | read | read | Azahar's measured sizes; no prototype |

## 9. Recommendation

### 9.1 The choice

**Option 3: a thin RHI of our own over Vulkan, shaped so that a Metal backend fits later, with Azahar's Vulkan backend as the reference to copy from under D6, and plume (MIT) as a second reference for the pieces Azahar lacks.** The provisional recommendation of D4 stands, with a stated price and stated conditions that would reverse it.

This is a test of D4's provisional recommendation, not a defence of it. What the evidence did to each option:
- **Option 1 is rejected by the evidence**, not by preference. The shadow write (seen in those scenes) needs a storage image on an `R32_UINT` view of the colour target, which needs an image created `MUTABLE_FORMAT`, layouts that allow `GENERAL`, and (for the ordered path) `VK_EXT_fragment_shader_interlock` enabled: plume has none of the three (6.1, G4). The blend constant (G8), the timeline (R2), an external surface (R1), the pipeline cache (G10) and the format query (X1) also fail. This holds for the PC and the M2 alike.
- **Option 2a collapses into 2b.** D3D12 is not needed (D4: "only if needed"), plume's Metal backend has no raster order groups, no fetch, no logic ops, takes only a metallib and asserts on fans and geometry, and upstream refuses AI-assisted patches (6.6): the multi-backend value that is the reason to prefer plume over our own code does not exist for us. Keeping two backends honest costs about 300 lines for nothing.
- **What remains is 2b against 3,** and the evidence does not make that a landslide. 2b saves about one to two M packages (inference). Three findings tip it to 3:
  1. The gaps in plume sit exactly where a PICA renderer works hardest: barriers (resource-held layout state mutated at record time, whole-resource `MEMORY_READ|MEMORY_WRITE` masks, a barrier that ends the render pass), render passes (one subpass, fixed layouts, one `VkRenderPass` per pipeline), submission (one wait stage for all semaphores, and a `pWaitDstStageMask` that points at one element) and device creation (whole-struct features, no format query). Of the nine patches P1 to P9, four (P2, P3, P4, P7) rework those areas, and the README itself says "barrier management and texture transitions" are still being refined (6.4, 6.6).
  2. The proven alternative is on disk and its demand on the graphics layer is narrow: 12 bindings in three sets, one subpass, everything in `GENERAL`, a timeline of ticks, one queue and one submit lock, no dynamic rendering, no push descriptors, no queries, no specialization constants (5.1, 5.6, 5.7). Azahar's graphics layer is 3,525 lines (a floor, 5.9: the image, barrier and descriptor code sits in other files) against 7,522 for plume's Vulkan slice, and the stability criterion of D56 ("stability, compatibility and the fewest errors for the average user") favours the pattern that is known to work on this game's renderer over one that is known to be unrefined at exactly its hardest part.
  3. The fork is permanent either way, so 2b buys no upstream, no tests and no maintenance relief; its only advantage is that the first 4,000 lines exist.
- **The price.** About one to two M packages more than 2b (inference, 8.3). The planner and Luis should weigh that against the risk in finding 1; the evidence does not prove that the risk would materialise (no validation run of plume was possible: the layer is not installed on the cloud machine).

### 9.2 What would change the recommendation

| Observation | Flips to |
|---|---|
| A validation-layer run (`VK_LAYER_KHRONOS_validation`) of plume with P2, P3 (stage mask), P4 and P7 applied, on lavapipe and on the RX 6700 XT, of three patterns (render then sample, a storage write on an `R32_UINT` view of an `RGBA8` target under interlock, a cross-queue sample through a timeline value) comes out clean, **and** the patches plus rework come to **600 lines or fewer** | **2b** |
| The planner's or a worker's estimate for our own RHI exceeds **about 1.5M tokens** (more than 1.5 L) | **2b** |
| A read of `plume_metal.cpp` end to end (render-pass and barrier code) shows that raster order groups, framebuffer fetch and run-time MSL come to about **300 lines or fewer**, **and** Luis decides a native Mac backend is wanted soon | **2a** |
| Luis wants plume's D3D12 backend (nothing in the evidence asks for it) | 2a |
| More captured scenes show that none of the needs beyond plain draw state matter | no flip: G4 is already seen |

What would *not* move it: the M2 probe run (it changes the Mac rows of section 4, not the RHI choice), the gas mode (it changes N2-10's design and whether input attachments are needed), the PC's missing `present_wait` (an OS-statistics question, D57). What would raise the weight of "shaped so that a Metal backend fits": the Mac moving to KosmicKrisp (no interlock) and the compare-and-swap loop showing visible shadow errors, which fires `metal.md`'s "Picture" trigger ([7.3](#73-new-facts-that-bear-on-metalmds-trigger-rule)).

### 9.3 Fallbacks the PC and the M2 each need

| Device | Fallbacks the RHI must provide (all from section 4) |
|---|---|
| RX 6700 XT | `D32_SFLOAT_S8_UINT` with conversions for D24S8; no `present_wait`, so the display time comes from the OS (D57); no rasterization-order access (irrelevant while interlock is `yes`) |
| M2, MoltenVK | no `logicOp` (shader emulation, 4 of 16 in Azahar's SPIR-V, or fetch on a native backend); no geometry shaders and no usable barycentrics (skip the fix-up; the GS draws fix up on the CPU anyway); D32S8; fans rewritten as lists (they work, but with pass restarts) and 8-bit indices widened (they work, converted by a compute pass; Azahar blacklists them on 1.4.1); `RGB8` as RGBA8; no graphics pipeline library; blend state baked into pipelines; primitive restart always on; FIFO and IMMEDIATE only; `present_wait` measures command-buffer completion |
| M2, KosmicKrisp | everything above, and **no interlock: the compare-and-swap loop for shadow writes**; `logicOp` yes; custom border colours not needed for Shadow2D |
| lavapipe | none of its own; it must run all of the above through the mask |

### 9.4 RHI design requirements the evidence imposes

Whatever the option; a list N2-21's planner can lift. Each carries its basis.
1. **Operation level, not Vulkan level.** The renderer-facing interface speaks Azahar's operations (5.8): surfaces, ring buffers, passes, pipelines keyed by state, bindings, submissions, readbacks. It must not expose `VkImageLayout`, `VkRenderPass`, `VkDescriptorSet`, `VkPipelineLayout` or Vulkan format enums, so a Metal backend can map to argument buffers, render pass descriptors and binary archives. Basis: 3.3 (Metal has no image layouts, barriers or descriptor sets), `metal.md` 2.3.
2. **A capability structure filled from device queries** (never constants: plume's `triangleFan` is one), **with a mask** (`MH4N_RHI_MASK` or a command-line option) applied after the query, for these bits: D24S8, interlock, 8-bit indices, custom border colours, fans, logic ops, dynamic rendering, pipeline library, extended dynamic state 3 blend bits, BC7, timeline semaphores, present wait, and "force staging copies" (to exercise the discrete-GPU path that lavapipe's single memory type hides). Basis: section 4; it cannot force barycentrics on.
3. **A format-support query and a chooser** (X1): attachment, sampled, storage and blit per format; D24S8 else D32S8, RGB8 always RGBA8, the narrow colour formats in their `R`-first variants, BC7 gated on `textureCompressionBC` and a run-time check. Azahar's `CreateFormatTable` is the pattern (`azahar@5f3306d renderer_vulkan/vk_instance.cpp:212-312`). Basis: G3, G9, G11; the `B5G6R5` family is sampled-only on the M2 (report 52371).
4. **Shadow support as a first-class path:** images that may become shadow sources are created with the mutable-format flag and the format list (`R8G8B8A8_UNORM`, `R32_UINT`), an `R32_UINT` storage view, a zero-attachment pass (X4), the interlock capability and the compare-and-swap fallback, both testable through the mask. `metal.md` 3.2 asks for storage usage only on surfaces that become shadow sources, so Apple GPUs can compress the rest; the interface should allow upgrading a surface on demand. Basis: 5.3, `metal.md` 1.2.
5. **One layout policy, hidden.** Azahar keeps everything in `GENERAL` with explicit barriers; it is proven on this renderer's workload and has no layout state to race (R2). Its bandwidth cost on Apple GPUs is unmeasured. The interface must not leak layouts so the policy can change. Basis: 5.6, 6.4.
6. **The feedback copy as an operation** (X6): end the pass, copy the target (the whole image in Azahar; a region would be cheaper, `inference`), resume with `LOAD`. Min/max emulation and any sampled render target use it; on Apple8 there is no fragment-stage barrier inside a pass, so it is also what Metal does. Basis: 5.6, `FST p.6`.
7. **Synchronisation as N2-21 specifies, with the defects of 6.1 avoided:** one submit lock per queue; every submission returns a timeline value; a wait carries its own stage mask; a binary-semaphore and fence fallback where `timelineSemaphore` is absent; deferred destruction by tick. Basis: 5.8, R2, plume's stage-mask defect.
8. **Pipelines:** a `VkPipelineCache` blob in and out of the device (header, vendor, device and UUID validated), a render-pass cache keyed by formats, creation callable from worker threads, an ubershader while a pipeline compiles; the pipeline key includes the blend state (MoltenVK cannot make it dynamic, X8) and the vertex strides if they are baked. The graphics pipeline library is optional (absent on both M2 drivers). Basis: G10, `report 52371`, `azahar@5f3306d renderer_vulkan/vk_pipeline_cache.cpp:192-298`.
9. **Presentation:** `PresentWaitAvailable()` *and* what it measures (`PresentWaitMeasuresDisplay()`: false on MoltenVK, whose wait is released at command-buffer completion); `VK_GOOGLE_display_timing` as a source where listed (MoltenVK); no MAILBOX on Apple (FIFO and IMMEDIATE); a headless surface for tests; a queue that can present chosen when a surface is given. Basis: R3, 3.3.
10. **Draw features:** a fragment depth output (X2, no early-Z); vertex attribute formats with the integer fallback and shader conversion (X3); strides taken from `minVertexInputBindingStrideAlignment` when the portability subset is present (1 on Apple8, 4 below Apple5), never assumed; 12 vertex bindings plus a fixed binding; primitive restart treated as always on on Apple (X7). Basis: Azahar's code, 3.3.
11. **Fans and 8-bit indices:** read from the capability, but on Apple devices rewrite fans as lists and widen 8-bit indices anyway (plume's own comment: the emulation "results in renderpass restarts"; Azahar blacklists uint8 on MoltenVK). Basis: 3.3, 6.1.
12. **Device selection by capability, not by index:** prefer a discrete device; on a Mac two `Apple M2` devices may answer (MoltenVK and KosmicKrisp) and the choice must be logged and overridable; request portability enumeration. Basis: 3.4.
13. **Enable only what the capabilities need** (plume turns on ray tracing wherever offered). Basis: 6.2.
14. **Loading:** volk, one volk and one VMA implementation in `native/third_party/rhi/`, a clear message and a null backend when there is no driver (R6). Basis: 6.1 (R6).
15. **Readback:** depth and stencil in two aspects (or Azahar's compute readback), the Morton write-back as a compute pass of ours, D24S8 ↔ D32S8 conversions as compute or CPU, `ReadImage` in the image's own format. Basis: 5.8, R5.
16. **Validation in CI:** run the RHI tests under `VK_LAYER_KHRONOS_validation` where it can be installed on the cloud machine (not installed now; whether the package can be installed there is `unverified`). Basis: 6.5.

### 9.5 What changes in the specs once D4 is decided

List only; the specs are not edited here. For option 3; for 2b the differences are named.
- **N2-21:** the size (M becomes L, or splits into an RHI-core WP and an M on top); "As D4 decided: the RHI (or plume)" names the RHI; `native/third_party/rhi/` holds volk and VMA only, and the RHI lives in `backend/`; "the fallbacks `rhi-spike.md` lists" becomes the list in 9.3 and 9.4; *Done when* gains the capability mask (every fallback run on lavapipe), the format query, `PresentWaitMeasuresDisplay()`, the Windows expectation that `PresentWaitAvailable()` is false on the RX 6700 XT, and "no occurrence of a Vulkan layout, render-pass or descriptor-set type in the renderer-facing header". (2b: N2-21 shrinks and an N2-21p appears.)
- **N2-05:** "which backends lack fans and 8-bit indices" has its answer in 3.3 and 4: none of the three Vulkan devices lacks them (MoltenVK and KosmicKrisp emulate fans, MoltenVK converts 8-bit indices by a compute pass); native Metal lacks both. So the rule becomes: rewrite on the capability bit, and always on Apple. Add strides from the device's alignment (X5 of the needs) and the attribute formats of X3.
- **N2-07:** shadow writes with interlock else compare-and-swap, and the lookup as a texel fetch with a manual compare and "out of range is lit" (no sampler feature); logic ops: hardware where `logicOp` is present, shader emulation of **all 16** elsewhere (Azahar does 4); min/max through X6; the shader writes depth (X2); the fix-up route per device (barycentrics on the PC only; the GPU geometry shader on the PC and lavapipe; skipped on the M2); the shadow write is GLSL-only in Azahar, so D5's option 2 needs both generators; pipeline counts on MoltenVK (baked blend state, no pipeline library); gas open.
- **N2-12:** the two shadow paths and their test through the mask; ShadowCube is unfinished in Azahar's Vulkan renderer and not seen in those scenes; KosmicKrisp is compare-and-swap only.
- **N2-16:** the cache blob API; no graphics pipeline library on the M2; Azahar skips a draw whose pipeline is not ready when async compilation is on (default off), which the ubershader replaces; MoltenVK's blob keeps converted MSL.
- **N2b-05:** the compositor waits on a timeline value with the fragment-shader stage; images are sampled in `GENERAL`; MoltenVK's baked blend state applies to its pipelines too.
- **N2b-06:** `PresentWaitAvailable()` and what it measures; MAILBOX absent on Apple; FIFO and IMMEDIATE; D57's second source on the RX 6700 XT; `VK_GOOGLE_display_timing` on MoltenVK; the headless surface for tests.

### 9.6 The decision requested of Luis

**Reply with one word: `3`** (our own RHI over Vulkan, Azahar as reference), **`2b`** (vendor plume's Vulkan backend, trimmed), **`2a`** (plume extended, all backends kept) or **`1`** (plume as it is; the evidence says it cannot do the shadow pass). The recommendation is `3`. Luis's Mac run ([3.4](#34-the-m2-pending-luiss-run)) does not need to come first: it changes the Mac rows, not this choice, but N2-21 should not start before D4 is `Decided`.

## 10. Questions only Luis's Mac run or more captured scenes can settle

Each with the command or capture that settles it and the part of the recommendation it would move.

| # | Question | What settles it | What it moves |
|---|---|---|---|
| 1 | Which Vulkan driver or drivers does Luis's Mac return (MoltenVK, KosmicKrisp, both), with which version and library path, and is his MoltenVK a private-API build (`logicOp`)? | [3.4](#34-the-m2-pending-luiss-run): `vkprobe-mac.json`, `loader.txt`, the `--loader` runs | the Mac columns of section 4; whether N1 is missing on the Mac; not the RHI choice |
| 2 | Does his line set equal the stand-in's (3.3 table)? Which lines differ? | the table in 3.4 | the Mac columns of section 4 |
| 3 | Is `VK_EXT_fragment_shader_interlock` listed on his MoltenVK, and does the shadow pass render correctly through it, and what does it cost against the compare-and-swap loop? | the interlock line of his run; then the shadow scenes replayed (N2-12) on both paths, with the mask forcing the loop | how much weight "shaped so that a Metal backend fits" carries; `metal.md`'s Picture trigger (7.3) |
| 4 | Does plume, patched, run the three patterns (render then sample; a storage write on an `R32_UINT` view of an `RGBA8` target under interlock; a cross-queue sample through a timeline value) with no validation errors, and how many lines do the patches and the rework come to? | a short program under `VK_LAYER_KHRONOS_validation` on lavapipe (needs the layer installed), the RX 6700 XT and the Mac | **9.2: the flip from option 3 to 2b** |
| 5 | What are the RX 6700 XT's extension list (198?), limits, queue families (is there a transfer-only family?), memory heaps and the 18 set bits of extended dynamic state 3? Are `VK_GOOGLE_display_timing` and `VK_EXT_present_timing` absent? | the missing `vulkaninfo --json` file for the PC (3.2), run through `mh4n_vkprobe --from-json` | the `unverified` statements of 3.2 and D57; the BC7 upload thread's queue (G11) |
| 6 | Does a newer AMD driver add `VK_KHR_present_id` and `present_wait`? | run the probe again after a driver update | R3, D57's second source |
| 7 | Does the PICA gas mode read the destination colour, and what exactly do modes 1 and 7 compute? | N2-10; more gas frames, or hardware tests | G7: whether input attachments (about 70 lines in plume, or the RHI's own) and Metal framebuffer fetch are needed |
| 8 | Do logic ops, fans, 8-bit indices, odd strides, min/max blending, cube and projection textures occur outside the four census scenes? | more captured scenes with the census counters (`pica-census.md` extended) | N1 to N6 rows; on MoltenVK 1.4.x only N1 would be missing without a native backend |
| 9 | How many draws per frame need the feedback copy or min/max emulation (pass breaks plus whole-image copies)? How many lit draws are hardware-vertex-shader draws (the only ones that need G2's GPU routes)? How often do a constant blend colour, a changing stencil reference and an RGB565, RGB5A1 or RGBA4 render target occur? | counters over the captures | plume's baked-state costs; the weight of X6; G2 on the M2 |
| 10 | Does the shadow result differ between pixel interlock and the compare-and-swap loop on the captured Arena scene? | run both paths on one capture on a device with interlock (the mask forces the loop) | whether a KosmicKrisp-only Mac is acceptable (7.3) |
| 11 | Does `vkWaitForPresentKHR` on MoltenVK return before the frame is on the display, and by how much, against `VK_GOOGLE_display_timing`'s `actualPresentTime`? | a small test program on the M2 | R3 and D57 on the Mac; `PresentWaitMeasuresDisplay()` |
| 12 | What does `MTLDevice.supportsBCTextureCompression` answer on his macOS (the HD pack's BC7)? | the BC7 line of his run | G11 on the Mac |
| 13 | Does the `pervertexEXT` route get anywhere on a MoltenVK built from `main`? (By source it will not.) | build and run Azahar's fragment-barycentric shader through MoltenVK `main` | G2 on the Mac |
| 14 | Does a BC image work when `textureCompressionBC` was not enabled (Azahar does not enable it)? | a validation-layer run on the PC and the Mac | G11, 5.1 |
| 15 | What do `GENERAL`-everywhere layouts and storage usage on every RGBA8 image cost on the M2's bandwidth? | `metal.md` 3.4 steps 4 to 6 with the storage-usage change | the layout policy (9.4, item 5) |
| 16 | Is the release tarball of MoltenVK that Azahar downloads a private-API build (`logicOp`)? | download the 1.4.x release tarball and probe it | N1 on the Mac; Azahar-as-reference notes |
| 17 | How much of `plume_metal.cpp`'s render-pass and barrier code would survive a Mac port that needs raster order groups, framebuffer fetch and run-time MSL? | a read of the file end to end (a researcher, not Luis) | **9.2: the flip to 2a**; the 7.2 estimates |
| 18 | How many distinct fragment-state keys and pipeline keys does a play session make? | N2-07's census numbers | G10: whether compile-on-first-use is tolerable without an ubershader |
| 19 | Does KosmicKrisp's `MESA_KK_EXPERIMENTAL=custom_border` work on an M2? | 3.4, step 4 | nothing for Shadow2D; ordinary textures' emulated border |

## 11. Corrections to earlier documents found while checking

Not edited here; the orchestrator or the owner of each file decides. Each as: file, line, what it says, what the evidence shows, source.

**`docs/research/metal.md`**

| Line | Says | Evidence | Source |
|---|---|---|---|
| 15, 68 (and D4 row) | MoltenVK has "No triangle fans"; the draw falls back to vertex shading on the CPU | MoltenVK 1.4.1 and 1.4.2 set `triangleFans = true` (hard-coded) and encode fans as an indexed indirect triangle list (since 1.2.5); reports 51106 and 52371 both say `triangleFans true`; Metal has no fan type (that part is right). Azahar reads the flag (`vk_instance.cpp:552-557`), so on 1.4.x its accelerated fan path should not be skipped (`inference`: both sources read, not run); plume's own comment says MoltenVK "supports triangle fans but does so via compute shaders" | `MoltenVK@v1.4.2 MVKDevice.mm:610` (v1.4.1: `:603`), `MVKCmdDraw.mm:182-186`, `Docs/Whats_New.md:599`; reports 51106, 52371; `plume@0ce184b plume_vulkan.cpp:4173-4180` |
| 21, 79 | MoltenVK compiles with Metal's fast math "by default" (`MVK_CONFIG_FAST_MATH_ENABLED=1`) | the default is `2` ("on demand": fast math unless a shader's `VK_KHR_shader_float_controls2` or `SignedZeroInfNanPreserve` limits it); identical at 1.4.1 and 1.4.2. Whether that protects Azahar's `isnan()` check depends on whether its SPIR-V carries those controls (`unverified`) | `MoltenVK@v1.4.2 Docs/MoltenVK_Configuration_Parameters.md:201-210`, `MVKEnvironment.h:183-184` |
| 74 | "Metal wants vertex strides in multiples of 4 (my understanding)" | 4 bytes for Apple2 and Apple3, 1 byte from Apple4; MoltenVK sets `vertexStrideAlignment` to 1 from Apple5; report 52371 says 1 for the M2 | `FST p.7`; `MoltenVK@v1.4.2 MVKDevice.mm:2433, 2568`; report 52371 |
| 13, 86 | "lit draws skip the per-triangle quaternion fix-up"; "On MoltenVK every lit draw runs without the flip" | only lit draws shaded on the GPU by the translated vertex shader. **A draw shaded on the CPU gets the flip on the CPU on every host**: every PICA-geometry-shader draw (39 to 90 percent of the draws in the census scenes) and every fan draw on a host without fans | `azahar@5f3306d rasterizer_accelerated.cpp:85-90`, `pica/pica_core.cpp:1052-1092` |
| 276 | KosmicKrisp "needs macOS 15" | "implemented on top of Metal 4, which requires macOS 26 and up" | `Mesa@f3f63ef docs/drivers/kosmickrisp.rst:4-8` |
| 50, 303 (and the trigger 3.3) | no mention that MoltenVK is deprecated | `main`'s README carries a deprecation notice since 2026-10-13: community best effort, Khronos recommends KosmicKrisp on Apple Silicon, MoltenVK stays for Intel Macs and iOS; no release after 1.4.2 | `MoltenVK@main README.md:6-14`, commit `0aa76ad` |
| 34, 117, 276 | KosmicKrisp: "custom border colours are not exposed yet"; "Trying it needs no code" | exposed by default: `logicOp` yes, **no interlock**, no barycentrics, no geometry; custom border colours exist behind `MESA_KK_EXPERIMENTAL=custom_border`; `maxUniformBufferRange` 65,536 | report 52372; `Mesa@f3f63ef kk_physical_device.c:174-177, 240` |
| 72 | Shadow row: interlock "supported through Metal raster order groups ... Expected to work" | right for MoltenVK. The row omits that Azahar blacklists interlock on NVIDIA's proprietary driver and that only the GLSL generator emits the shadow write | `azahar@5f3306d renderer_vulkan/vk_instance.cpp:471-473`, `shader/generator/pica_fs_config.h:407-414` |
| 165-166; no min/max row in 1.2 | min/max blending is something only Metal framebuffer fetch improves | in Azahar's Vulkan renderer min/max is emulated in the shader on **every** host except when both factors are One (Vulkan's `MIN`/`MAX` ignore factors), at the price of a whole-image copy and a pass break per draw; Metal has native `min`/`max` | `azahar@5f3306d shader/generator/pica_fs_config.cpp:44-56`, `vk_rasterizer.cpp:725-732`; `FST`/Apple `MTLBlendOperation` |
| 281 | "On Apple GPUs, MoltenVK can turn input attachments into framebuffer reads (my understanding)" | not found in 1.4.2: `use_framebuffer_fetch_subpasses` is never set, `VK_EXT_rasterization_order_attachment_access` is absent | `MoltenVK@v1.4.2 MVKPipeline.mm:2888` (serialiser only), `MVKExtensions.def` |
| 70 | 16-bit colour: "Not checked whether MoltenVK offers them" | now checked: `R5G6B5`, `R5G5B5A1`, `A1R5G5B5`, `R4G4B4A4` are colour attachments with blending; the `B5G6R5`, `B5G5R5A1`, `B4G4R4A4` variants are sampled only; `R8G8B8_UNORM` has no image features | report 52371 |
| 95 | gas: "work in the shader generators and helps every renderer" | may also need new render-target access (framebuffer fetch, interlock or a copy per draw) if mode 1 reads the destination colour | 5.5; 3dbrew `GPUREG_GAS_ACCMAX_FEEDBACK` |
| 187 (and 37) | Vulkan backend 11,413 lines "non-blank, non-comment" | 11,366 with `/* */` blocks also stripped; 11,413 is nearly reproduced (11,407, and 9 of 10 groups exactly) only if block comments are counted | 5.9 |

**`docs/research/vkprobe.md` and `vkprobe-results.md`**

| File:line | Says | Evidence | Source |
|---|---|---|---|
| `vkprobe.md`:61 | on a Mac "expect `no` for triangle fans" | for MoltenVK 1.4.x expect `yes` | as above |
| `vkprobe-results.md`:41-43 | "Expected ... `no` for triangle fans" | `yes` | as above |
| `vkprobe.md`:73 | "The site's own JSON export has a layout of its own, which `--from-json` does not read." | gpuinfo's `api/v3/getprofile.php` export **is** the Vulkan Profiles layout and `--from-json` reads it; three export gaps: no device name or type (`(unnamed)`), `D24_UNORM_S8_UINT` `unknown`, and 19 of 130 extensions missing; the probe prints `source: vulkaninfo` for such a file | `report 52371`/`52372` through the probe (3.3) |
| `vkprobe.md` table (barycentrics; present id/wait) | a `yes` is "extension and bit" | on MoltenVK `yes` for barycentrics means *listed*, not usable (SPIRV-Cross cannot translate per-vertex inputs); `yes` for present id/wait means the device's answer, with the wait released at command-buffer completion | 3.3 |
| `vkprobe-results.md`:9-11, 14, 22-24 | the run "checked against the SDK's `vulkaninfo --json`"; "198 device extensions"; driver file 2.0.353; no `VK_GOOGLE_display_timing` or `VK_EXT_present_timing` | the `vulkaninfo` file is not in the repository, and `vkprobe-pc.json` has only the probe's 24 lines: not checkable here | 3.2 |
| `vkprobe-results.md`:9 | "Run on 2026-10-21" | the commit `ba1ba93` is dated 2026-10-21 19:51 -0700, that is 2026-10-22 02:51 UTC; no date in the JSON | `git log` |

**`docs/native/DECISIONS.md`**

| Row | Says | Evidence |
|---|---|---|
| D4 | MoltenVK has "no triangle fans" among its known gaps | MoltenVK 1.4.x has them (emulated); remove from the list |
| D4 | plume "no interlock, raster order groups or framebuffer fetch exposed; unstable API" | **confirmed** (6.7). To add: no image `MUTABLE_FORMAT`, fixed `COLOR_ATTACHMENT_OPTIMAL` layouts, no `VkPipelineCache`, no blend-constant setter, binary semaphores only, no format query, no D24S8 or narrow colour formats, an Xlib-only Linux surface path, a metallib-only Metal shader path (6.1) |
| D4 | the Windows AMD driver's interlock was to be found out | answered: pixel and sample interlock `yes` on the RX 6700 XT with AMD driver 26.10.1 (3.2) |
| D4 | "Known already (`metal.md`)" | add: MoltenVK is deprecated on `main` (2026-10-13); KosmicKrisp needs macOS 26, has no interlock and no barycentrics |
| D5 | option 2: "they draw no gas, the SPIR-V one handles 4 of the 16 logic ops, and the first use of a pipeline stutters" | **omits** that Azahar's SPIR-V generator has no shadow write, no ShadowCube and no min/max blending (`UsesSpirvIncompatibleConfig`), so porting needs both generators (GLSL 1,515 and SPIR-V 1,346 counted lines, plus configuration and helpers) |
| D57 | the RX 6700 XT "has no `VK_KHR_present_id`, `present_wait`, `VK_GOOGLE_display_timing` or `VK_EXT_present_timing`" "from the Vulkan probe" | the probe has no line for the last two; that part rests on the `vulkaninfo` file that is not in the repository (3.2) |

## 12. What could not be checked

One list, merged from the four notes and this writing, each with what would settle it.

| Item | What would settle it |
|---|---|
| Luis's own M2: core count, macOS version, which MoltenVK or KosmicKrisp he has; every M2 number here is another M2's | his run ([3.4](#34-the-m2-pending-luiss-run)) |
| Who submitted reports 52371 and 52372, and whether they are one machine (inferred from OS and minute) | the site does not show it; unknowable |
| The build flags of MoltenVK's release tarball and of the SDK and Homebrew builds (private API, `logicOp`) | download and probe each (question 16) |
| Anything run on a Mac or a Metal device: no SPIR-V converted, no shadow pass rendered, no speed | Luis's run; `metal.md` 3.4 checklist |
| The RX 6700 XT beyond the 24 probe lines; whether the interlock works in a shader on that driver | the missing `vulkaninfo` file; a test shader (question 5, 4) |
| The `vulkaninfo` file the PC results say they compared with | it is not in the repository; Luis can supply it |
| The run date of the PC probe (2026-10-21 or 2026-10-22) | the commit's UTC date suggests one calendar-day difference of time zone (inference); the CI run 39710163710 page |
| Presentation on lavapipe (no surface, no display) | a run with a display or a virtual one |
| Whether any lavapipe or plume path *works* end to end (the probe reports capability only; the plume run had no pipeline, no validation layer, no multi-thread test, no swapchain) | validation-layer runs (question 4) |
| plume's Metal and D3D12 backends: read by grep and a few spots only | a full read (question 17) |
| plume's behaviour on MoltenVK, on AMD's Windows driver, with a Vulkan 1.1-only driver | runs on those |
| Whether the stage-mask defect and the resource-held layout state cause validation errors or artefacts | a validation-layer run |
| Unleashed Recompiled as a plume user | the project's page or its source |
| Azahar: nothing built or run; `vk_shader_disk_cache.cpp`, `renderer_vulkan.cpp`, `glsl_fs_shader_gen.cpp` and `rasterizer_cache.h` read in part; `custom_textures/`, `renderer_opengl/`, the CPU shader interpreter and `texture_decode.cpp` not read | reading them; N2-14 and N2-06 do |
| Azahar's "proven on MH4U" (D5) | not checkable from the code |
| Whether the Vulkan ShadowCube path in Azahar really fails; whether the compare-and-swap loop differs from the interlock path; whether `GENERAL` costs bandwidth; what a driver does with a BC image when `textureCompressionBC` is off | captures with a ShadowCube; both paths on one capture; M2 measurements; a validation run |
| The date of the licence switch in D6 (2026-10-03) | Azahar's history; our checkout has one commit |
| VMA's behaviour for `AUTO_PREFER_DEVICE`; the Vulkan rule that `R32_UINT` supports atomic storage (`memory`) | the VMA source and the Vulkan spec text |
| The per-fragment formula of gas modes 1 and 7 | N2-10; hardware tests |
| KosmicKrisp: the semantics of its present wait, whether `custom_border` works, how local-read compiles, the source lines of its limits, the manifest's installed name | a run on an M2 with the SDK |
| Apple documents: "Access to pre-raster per-vertex values" (Apple10) beyond its table row; the 16-bit formats on Apple GPUs and their Metal names; whether an M2 also answers the `mac2` family; the Khronos conformance page; which three usage options Apple's lossless-compression page excludes (the extraction lost them); "no public logic-op API" beyond the pages searched | reading those pages; Apple's documentation |
| MoltenVK issue #2059, the Dolphin, Cemu and Phoronix pages | not needed for D4 |
| The `VK_LOADER_DEBUG` and `VK_DRIVER_FILES` variable names and the Apple Feature Set Tables URL, written from memory | the loader's documentation; Apple's developer site |
| Whether `VK_LAYER_KHRONOS_validation` can be installed on the cloud machine | `apt` on a cloud session |
| All costs in plan units and line estimates (8.3, 6.8, 7.2): `inference` from line counts, with no prototype | a prototype of the first N2-21 pieces |
| `docs/research/pica-census.md` is not on this branch; only its four-scene numbers are used | the file, once merged |

## 13. Sources

Every pinned source and how to get it again. Downloaded files and clones are data: put each in its own new directory, never run code from them, and run any Python that reads them as `python3 -I`.

| Source | Pin | How to get it again |
|---|---|---|
| plume | `0ce184b19270e49236afbb74052a6b267494b86b` | `git clone --recurse-submodules https://github.com/renderbag/plume plume && git -C plume checkout 0ce184b19270e49236afbb74052a6b267494b86b && git -C plume submodule update --init --recursive` |
| Azahar | `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` | `git clone https://github.com/azahar-emu/azahar azahar && git -C azahar checkout 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` |
| MoltenVK v1.4.2 | tag, `db66022459ffb663aa2b50f6b018bc2e124f5edf` | `git clone --depth 1 --branch v1.4.2 https://github.com/KhronosGroup/MoltenVK mvk-1.4.2` |
| MoltenVK v1.4.1 | tag, `db445ff2042d9ce348c439ad8451112f354b8d2a` | `git clone --depth 1 --branch v1.4.1 https://github.com/KhronosGroup/MoltenVK mvk-1.4.1` |
| MoltenVK main | `5189f8de86ec365860f63bf1da736815cfaea5cd` (2026-10-13) | `git clone https://github.com/KhronosGroup/MoltenVK mvk-main && git -C mvk-main checkout 5189f8de86ec365860f63bf1da736815cfaea5cd`; tags: `git ls-remote --tags https://github.com/KhronosGroup/MoltenVK` |
| SPIRV-Cross | `6c09849fe88c48eaed08413aa022aaa136a3a057` (MoltenVK 1.4.2's pin, `ExternalRevisions/SPIRV-Cross_repo_revision`), `aa217aeb6c9f0ace7a0ab233b28807edf45eb165` (main's) | `git init spvcross && git -C spvcross fetch --depth 1 https://github.com/KhronosGroup/SPIRV-Cross <sha> && git -C spvcross checkout FETCH_HEAD` |
| Mesa (KosmicKrisp) | `f3f63efe9ca77e197210d9ece06f8e5cedc0f853` (2026-10-16) | `git clone --filter=blob:none --sparse https://gitlab.freedesktop.org/mesa/mesa.git mesa && git -C mesa sparse-checkout set src/kosmickrisp docs/drivers && git -C mesa checkout f3f63efe9ca77e197210d9ece06f8e5cedc0f853` (github.com/mesa3d/mesa asked for credentials) |
| gpuinfo reports | 52371 (`profile.json` sha256 `109720903309a10128de4d85ce62a71d771c389c436e14687d44ee4d3e1e759f`), 52372 (`f77ab407f6d796eddcd110fdcb141f230334d8fa0021f3d28b6f041fcf99fa92`) | `curl -o profile.json "https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=52371"`; the page: `https://vulkan.gpuinfo.org/displayreport.php?id=52371`; the probe: `mh4n_vkprobe --from-json profile.json` (add the gaps of 3.3 by hand if wanted). The site has no database download (`about.php`); the listing endpoints are internal |
| Apple Metal Feature Set Tables | 8-page PDF dated 2026-05-21, sha256 `9f31df15dd6827545702c5a0845f6e36e1889878cd0e534123bd70211e5c00a8` | the Metal page on developer.apple.com (the URL `https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf` is from `memory`); read with `pdftotext -layout` |
| Metal Shading Language Specification 4.1 | 383 pages, sha256 `41538b30d2f1140a5b2a0c84ce0a9f7b67bf0c707e224cfea0bfe5a44aa26cf5` | `https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf` (retrieved 2026-10-23) |
| Apple documentation pages | `mtlprimitivetype`, `mtlindextype`, `mtlsamplerbordercolor`, `mtlblendoperation`, `mtlblendfactor`, `mtlrenderpipelinecolorattachmentdescriptor`, `mtlrenderpipelinedescriptor`, `mtlgpufamily`, `mtlpixelformat/depth24unorm_stencil8`, `mtldevice/isdepth24stencil8pixelformatsupported`, `mtldevice/supportsbctexturecompression`, `optimizing-texture-data` | `https://developer.apple.com/tutorials/data/documentation/metal/<page>.json` (retrieved 2026-10-23) |
| 3dbrew | `GPU/Internal_Registers`, last edited 2026-10-18 | `https://www.3dbrew.org/wiki/GPU/Internal_Registers` (retrieved 2026-10-23) |
| lavapipe | Mesa 25.2.8-0ubuntu0.24.04.4, LLVM 20.1.2 | Ubuntu 24.04 packages `mesa-vulkan-drivers`, `vulkan-tools`; `build/native/bin/mh4n_vkprobe`, `vulkaninfo --json` |
| The probe | built from this repository at `69dd2c2` | see [`vkprobe.md`](vkprobe.md) |
| The RX 6700 XT report | [`vkprobe-pc.json`](vkprobe-pc.json), commit `ba1ba93` | in this repository; `mh4n_vkprobe --from-json docs/research/vkprobe-pc.json` |

Cross-references: [D4 and the decisions](../native/DECISIONS.md), [N2a work packages](../native/wp/N2a.md), [N2b work packages](../native/wp/N2b.md), [`metal.md`](metal.md), [`vkprobe.md`](vkprobe.md), [`vkprobe-results.md`](vkprobe-results.md).
