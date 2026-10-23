# RHI spike: the evidence for D4 (render interface and backends)

## 0. Header

What this is: the evidence that settles [D4](../native/DECISIONS.md) (the render hardware interface and its backends) and a recommendation, the deliverable of N2-01 ([`N2a.md`](../native/wp/N2a.md)). It covers what the three target devices offer (RX 6700 XT, Apple M2, lavapipe), what the renderer asks of its graphics layer (read from Azahar's Vulkan renderer), how plume meets that, and what each of the three options costs.

- **Status:** `review`. D4 waits for Luis (the maintainer) and for his run of `mh4n_vkprobe` on his Mac ([3.4](#34-the-m2-pending-luiss-run)). The planner reviews the recommendation; Luis decides.
- **Date:** 2026-10-23. **Written by:** the cloud orchestrator's N2-01 session. Four research notes (lavapipe and the PC, plume, Azahar, the M2) were written first; this document turns them into the deliverable. Every claim carried over was either re-read in the source named next to it or is labelled; the re-checks that caught errors are in [11](#11-corrections-to-earlier-documents-found-while-checking) and [12](#12-what-could-not-be-checked).
- **The census.** `pica-census.md` (N0-13) is not on this branch. This document cites it by name and takes from the orchestrator's task text the number of scenes (four: title, character creation, one Arena fight, one gas cutscene) and of frames (3, 3, 10 and 10), and the census findings listed in section 2. **All of these are `unverified`** (`source`: the orchestrator's task text, until `pica-census.md` is on this branch). Every table about the census says *seen in those scenes* or *not seen in those scenes*, never "never".

**Labels.** Each point says where it comes from.

| Label | Meaning |
|---|---|
| `source: <ref>` | read in the named source in this work (2026-10-23) |
| `inference` | concluded from what was read, not run |
| `unverified` | could not be checked; the text says what would check it |
| `memory` | what the writer recalls about a library or a spec, not read here |
| `stand-in` | a number from another person's M2, from a published report, MoltenVK's source or Apple's tables |
| `pending Luis's run` | a number only the maintainer's own Mac can give |

**Reference forms.** `plume@0ce184b plume_vulkan.cpp:1234` (a file of that repository); `azahar@5f3306d renderer_vulkan/vk_instance.cpp:471` (paths under `src/video_core/` unless they start with `src/`, `externals/`, `CMake` or `.ci/`); `MoltenVK@v1.4.2 path:line` (MoltenVK files are under `MoltenVK/MoltenVK/<GPUObjects|Commands|Vulkan|Layers|API|Utility>/`: `MVKDevice.mm`, `MVKImage.mm`, `MVKSwapchain.mm`, `MVKPipeline.mm`, `MVKSync.h` in `GPUObjects/`, `MVKCmdDraw.mm` in `Commands/`, `mvk_datatypes.mm` in `Vulkan/`, `MVKExtensions.def` in `Layers/`, `MVKEnvironment.h` in `Utility/`; `Docs/` and `README.md` are at the repository root); `Mesa@f3f63ef path` (`kk_*.c` and `meson.build` are under `src/kosmickrisp/vulkan/`, `kosmickrisp.rst` under `docs/drivers/`); `plume-val <file>` (a file of the validation experiment: [`rhi-spike-evidence/`](rhi-spike-evidence/README.md), or a log of the original run in the session scratch folder, [6.9](#69-validation-layer-run-of-plume)); `report 52371`; `FST p.N` (Apple's Metal Feature Set Tables, page N of the 18-page file); `MSL p.N` (page of the footer "Page N of 383"); `vkprobe-pc.json features[id]`; `needs.md` is the orchestrator's list of needs for this spike (not in the repository), reproduced in [2](#2-what-the-renderer-needs).

**The versions every number comes from**

| What | Version | Note |
|---|---|---|
| plume | `0ce184b19270e49236afbb74052a6b267494b86b` (2026-10-21, 76 commits, no tags) | https://github.com/renderbag/plume |
| Azahar | `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` (2026-10-10) | the commit [`metal.md`](metal.md) pins; a one-commit checkout |
| MoltenVK | `v1.4.2` (released 2026-07-20, merge commit `db66022`), `v1.4.1` (`db445ff`) for reference, `main` at `5189f8d` (2026-10-13) | https://github.com/KhronosGroup/MoltenVK |
| SPIRV-Cross | `6c09849` (MoltenVK 1.4.2's pin), `aa217ae` (main's pin) | https://github.com/KhronosGroup/SPIRV-Cross |
| Mesa (KosmicKrisp) | `f3f63ef` (2026-10-16) | https://gitlab.freedesktop.org/mesa/mesa.git |
| gpuinfo reports | **52371**: Apple M2, MoltenVK 1.4.2, macOS 26.6.2, Vulkan 1.4.357, submitted 2026-10-07 21:50:02. **52372**: Apple M2, KosmicKrisp `vulkan-sdk-1.4.363`, macOS 26.6.2, submitted 2026-10-07 21:52:00. 51106 (MoltenVK 1.4.1) and 52375 (KosmicKrisp, macOS 27.0.1) only where named | https://vulkan.gpuinfo.org/displayreport.php?id=N |
| Apple | Metal Feature Set Tables dated 2026-05-21 (18 pages); Metal Shading Language Specification 4.1 (created 2026-06-04) | retrieved 2026-10-23 |
| lavapipe | Mesa 25.2.8-0ubuntu0.24.04.4, LLVM 20.1.2, device Vulkan 1.4.318, loader 1.3.275 | the cloud machine |
| RX 6700 XT | `docs/research/vkprobe-pc.json`, committed in `ba1ba93` | [3.2](#32-rx-6700-xt-windows-amds-own-driver) |
| the validation layer | `VK_LAYER_KHRONOS_validation` 1.3.275 (apt `vulkan-validationlayers 1.3.275.0-1`), on lavapipe | [6.9](#69-validation-layer-run-of-plume) |
| the probe | `mh4n_vkprobe` built from this repository at `69dd2c2` | `build/native/bin/mh4n_vkprobe`; the probe's source is unchanged in the next commit |
| 3dbrew | `GPU/Internal_Registers`, "last edited on 18 October 2026" | retrieved 2026-10-23 |

## 1. Summary

What each device offers, by the probe (`yes` means the extension is listed *and* the bit is set; the M2 rows are `stand-in`, from reports 52371 and 52372, MoltenVK and Mesa source and Apple's tables, [3.3](#33-the-m2-stand-in)):

| Device | Offers | Lacks |
|---|---|---|
| RX 6700 XT, AMD's own driver 26.10.1 | everything the probe asks for, **pixel and sample interlock**, geometry shaders, barycentrics, fans, logic ops | `D24_UNORM_S8_UINT` (use `D32_SFLOAT_S8_UINT`), rasterization-order access, `present_id`/`present_wait` |
| M2, MoltenVK 1.4.2 | interlock (raster order groups), fans (emulated), 8-bit indices, BC7 | `logicOp`, geometry shaders, custom border colours, D24, pipeline library; barycentric built-ins translate but per-vertex inputs (Azahar's G2 route) do not |
| M2, KosmicKrisp | `logicOp` | **no interlock**, no barycentrics, no geometry shaders, no D24; needs macOS 26 |
| M2, native Metal (Apple8) | framebuffer fetch, raster order groups | 24-bit depth, a logic-op API (in the pages searched), fans, 8-bit indices, geometry stage |
| lavapipe | all of it except barycentrics and `present_id`/`present_wait` | reports *more* than the PC (D24S8, rasterization-order access): it hides the PC's fallbacks, and it accepts every invalid call |

- **The Mac, first:** MoltenVK was deprecated by Khronos on `main` on 2026-10-13 (no release after 1.4.2); KosmicKrisp, the driver Khronos now recommends, needs macOS 26 and has no interlock, so there the shadow write is the unordered compare-and-swap loop; MoltenVK's `present_wait` is released at command-buffer completion, not at display ([3.3](#33-the-m2-stand-in), [7.3](#73-new-facts-that-bear-on-metalmds-trigger-rule)). Which Mac driver to target and whether native Metal is a goal are Luis's product calls, separate from D4 ([9.6](#96-the-decision-requested-of-luis)).
- **What plume lacks for MH4U, measured under the validation layer on lavapipe ([6.9](#69-validation-layer-run-of-plume)):** no interlock; no `MUTABLE_FORMAT`; no zero-attachment pass (three separate errors); one wait stage and a one-element stage mask; layout state held in the resource, which races between threads; no `VkPipelineCache`; 44 render passes for 42 pipelines; no blend-constant setter; no timeline; no format query; a descriptor set that makes its own layout and pool; Metal takes only a metallib. Render-then-sample basics are clean unpatched. Seven small Vulkan patches (211 changed lines) repair all but the layout race, which an owner rule in the caller covers.

| Option | Meets the needs seen in those scenes | Cost, tokens (inference, [8.3](#83-cost-in-tokens-and-on-the-critical-path)) | Critical path, unsplit / N2-21 split in three |
|---|---|---|---|
| 1 plume as it is | degraded: shadows unordered, blend constant by a workaround; 1,240 to 1,290 workaround lines | 0.67M to 0.83M | +0.27M to +0.43M / about 0 |
| 2a plume extended, all backends kept | yes, after patches (202 lines measured + 245 estimated) and rework | 0.71M to 0.95M | +0.31M to +0.55M / about 0 |
| 2b plume's Vulkan backend, trimmed, thick wrapper | as 2a without D3D12 and Metal | 0.62M to 0.86M (1,977 to 2,657 lines, 202 measured) | +0.22M to +0.46M / about 0 |
| 3 own thin RHI, from Azahar's pattern | yes once built; no prototype | 1.05M to 1.52M (3,680 to 5,320 lines, all estimated) | +0.65M to +1.12M / +0.09M to +0.27M |

- **Recommendation: option 2b, a close call against option 3: the gap is 0.55M at the centres (0.2M to 0.9M), +0.09M to +0.27M on the critical path if N2-21 is split in three.** This changes the first draft's recommendation (3), and the gap did not shrink: the first draft's "1 to 2 M packages" is 0.4M to 0.8M, and the corrected arithmetic gives the same order. What the plume run moved is the argument for paying it: render-then-sample is clean unpatched, the patches cost about what was estimated for the same items (about 20 lines saved), and the first draft's worry, a race on the layout state held in the resource, was confirmed and dealt with by a convention (the owner rule), not by rewriting plume. That convention is an invariant lavapipe cannot test well; the planner would lean 3, narrowly, if the decision were delegated. 2b is reversible (the wrapper is option 3's interface; reversing wastes about 0.33M to 0.46M). Whether to own 7.5k foreign lines in a fork with no upstream (plume refuses AI-assisted contributions) for about 0.55M less is Luis's preference to state, not a delegated call ([9.1](#91-the-choice-reassessed-from-the-start), [9.6](#96-the-decision-requested-of-luis)).
- **The schedule:** N2-21 heads the longest chain (N2-21, N2-06, N2-08, N2-23, N2-09, N2-20, N2-15, N2-16, N2b-12, ...); splitting it into a core (N2-06 and N2-22 need only that), a render-target slice and the rest, with `native/src/gpu/backend/` split by directory so the slices share no files, removes most of any option's delay; N2-05 and N2-07 are released by D4 itself ([8.3](#83-cost-in-tokens-and-on-the-critical-path), [9.5](#95-what-changes-in-the-specs-once-d4-is-decided)).
- **What would flip it to 3:** the gap measured below about 0.15M (the 2b total above 3,000 lines against my 1,977 to 2,657, or option 3 under 3,000 lines against my 3,680 to 5,320), a defect on the RX 6700 XT or MoltenVK that the patches and the owner rule cannot repair, or Luis declining the fork ([9.2](#92-what-would-change-the-recommendation)).
- **Pending:** Luis's Mac run ([3.4](#34-the-m2-pending-luiss-run)); the RX 6700 XT and MoltenVK legs of the plume run (they de-risk every option's shadow and sync features; I advise no further experiment before D4); more captured scenes (fans, logic ops, 8-bit indices and the other needs not seen in those scenes); the gas mode (N2-10); a read of `plume_metal.cpp`; the `vulkaninfo` file the PC results say they compared with is not in the repository.

## 2. What the renderer needs

The needs list is the orchestrator's (`needs.md`, written 2026-10-23), keyed by the IDs below. *Seen* means seen in the four census scenes (title, character creation, one Arena fight, one gas cutscene; 3, 3, 10 and 10 frames, counted independently twice). `pica-census.md`'s findings used here (all `unverified`, see the header): geometry shaders are used in Point mode in 39 to 90 percent of the draws and about 1 percent of the vertices; `D24S8` is the only depth format seen; shadow passes with a Shadow2D texture and a white border colour; fog; lighting Config0 and Config1 only; one procedural-texture configuration, always inside the geometry-shader sprite pass; gas in the cutscene only.

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
| R9 | Metal-only features (the rule of [`metal.md`](metal.md)) | rule | framebuffer fetch and raster order groups are native-Metal items (`metal.md` 2.2, lines 164-166; 3.2; the clause "unless the Vulkan-on-Metal driver already gives them" is this document's gloss): [7](#7-metal) |

**Needs the notes added** (renumbered here; the notes numbered theirs separately: plume's X1 is X1; Azahar's X1 to X5 are X2 to X6; the M2 note's X1 to X3 are X7 to X9; the M2 note's X4, the Vulkan-on-Metal driver choice, is not a need but a fact of the target matrix, [7](#7-metal)):

| ID | Need | Why it is needed |
|---|---|---|
| X1 | A format-support query: attachment / sampled / storage / blit per format | G3's D24S8-else-D32S8, G9's narrow formats, G11's BC7 and N1's fallback depend on it; plume answers none of them (`source: plume@0ce184b`, no call of `vkGetPhysicalDeviceFormatProperties` in `plume_vulkan.cpp`) |
| X2 | The fragment shader writes depth in every non-shadow draw (`gl_FragDepth`), so no early-Z for PICA draws | `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:186, 199-216` |
| X3 | Vertex attribute formats `*_SSCALED`/`*_USCALED` (8 and 16 bits, 1 to 4 components) with an integer-format fallback and shader conversion, and 3-component aliasing | `source: azahar@5f3306d renderer_vulkan/vk_instance.cpp:72-97, 350-395` |
| X4 | A render pass with zero attachments and a storage-image write target (the shadow write); in plume this needs three fixes: the framebuffer size, the pipeline's viewport and scissor count, and the render area | `source: azahar@5f3306d renderer_vulkan/vk_texture_runtime.cpp:1491-1499`; `plume-val logs/pristine-B1.txt` ([6.9](#69-validation-layer-run-of-plume)) |
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

Versions (`source: vulkaninfo --summary`, `dpkg -l`): device `llvmpipe (LLVM 20.1.2, 256 bits)`, `PHYSICAL_DEVICE_TYPE_CPU`, vendorID 0x10005, `apiVersion` 1.4.318, driver `Mesa 25.2.8-0ubuntu0.24.04.4 (LLVM 20.1.2)`, `driverID` MESA_LLVMPIPE, `conformanceVersion` 1.3.1.1. The loader is 1.3.275 (`libvulkan1 1.3.275.0-1build1`), so the device reports a higher version than the loader's instance version. One device. Instance layers: `VK_LAYER_INTEL_nullhw`, `VK_LAYER_MESA_device_select`, `VK_LAYER_MESA_overlay`; no validation layer at the time of the probe (it installs with apt, [6.9](#69-validation-layer-run-of-plume)).

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

**The report.** `report 52371` (https://vulkan.gpuinfo.org/displayreport.php?id=52371): Apple M2, integrated GPU, vendorID 0x106B, deviceID 0x1A060208, driver MoltenVK 1.4.2 (`driverVersion` 0.2.2210, which is `MVK_VERSION` decoded as a Vulkan version: `source: MoltenVK@v1.4.2 MoltenVK/MoltenVK/API/mvk_private_api.h:65-69`, `MVKDevice.mm:2412`), conformance 1.4.4.0, Vulkan 1.4.357, macOS 26.6.2 arm64, 130 device extensions. The newest MoltenVK *release* is 1.4.2 (`git ls-remote --tags` lists no later tag, retrieved 2026-10-23). It was submitted by a client application, not made with `mh4n_vkprobe`: the site's FAQ says data is submitted through "Vulkan Hardware Capability Viewer" (`source: https://vulkan.gpuinfo.org/about.php`, retrieved 2026-10-23); that this report came from that client is `inference`; its JSON profile (`api/v3/getprofile.php?id=52371`, 241,141 bytes) is the Vulkan Profiles layout the probe's `--from-json` reads. The "portability" download is byte-identical.

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
| barycentrics | **no** (neither the KHR nor the NV extension) | yes (listed; the built-ins translate, per-vertex inputs do not, below) |
| triangle fans | yes (a full device, no portability subset; emulated, `Mesa@f3f63ef src/kosmickrisp/vulkan/kk_cmd_draw.c:1842-1843`) | yes (portability subset) |
| extended dynamic state 3 | 7 of 31 bits | 5 of 31 bits |
| all other probe lines | equal (D24 no, D32S8 yes, BC7 yes, geometry no, custom border no, graphics pipeline library no, present id/wait yes) | |

Other differences read by hand on the pages: `maxUniformBufferRange` 65536 against 4294967295; `maxVertexInputBindingStride` 2048 against 1073741824; `maxPushConstantsSize` 256 against 4096; `minUniformBufferOffsetAlignment` 64 against 16; `maxBoundDescriptorSets` 32 against 8; `timestampPeriod` 41.6667 against 1; no `VK_KHR_portability_subset`; no `VK_GOOGLE_display_timing`. KosmicKrisp lists `VK_EXT_attachment_feedback_loop_layout`, `VK_EXT_provoking_vertex` and `VK_EXT_primitive_restart_index`, which MoltenVK 1.4.2 does not (`VK_KHR_dynamic_rendering_local_read` is on both: report 52371 lists it, but the profile export drops it). MoltenVK 1.4.1 (`report 51106`, the version Azahar pins) differs from 1.4.2 only in `VK_KHR_shader_quad_control` and the API version; its probe table is identical.

**MoltenVK, from its source** (`MoltenVK@v1.4.2`; nothing was compiled or run; what MoltenVK does on an M2 is only what the report shows):

| Item | What the source says | Source |
|---|---|---|
| `logicOp` | `_features.logicOp = getMVKConfig().useMetalPrivateAPI`; the build setting `MVK_USE_METAL_PRIVATE_API` is OFF by default. Upstream CI builds a macOS artifact with it. How the release tarball Azahar downloads is built could not be read (`unverified`) | `MVKDevice.mm:2765`; `CMakeLists.txt:70`; `.github/workflows/CI.yml:25-31, 48` |
| `geometryShader` | never assigned, always false; Metal has no geometry stage | `grep geometryShader MVKDevice.mm`: no match |
| interlock | listed only if `rasterOrderGroups` (Apple4 and later, so the M2); implemented through SPIRV-Cross's `SPV_EXT_fragment_shader_interlock` since 1.0.37. One known gap: `force_fragment_with_side_effects_execution = false`, because enabling it fails dEQP `pixel_ordered` discard tests ("Requires investigation") | `MVKDevice.mm:2632, 3530-3532`; `MVKPipeline.mm:1672-1673`; `Docs/Whats_New.md:1803, 1838` |
| `VK_EXT_rasterization_order_attachment_access` | **absent** at 1.4.1, 1.4.2 and main | `MoltenVK/Layers/MVKExtensions.def` (whole file) |
| barycentrics | listed when Metal reports support (the extensions are disabled otherwise) and the feature bit is then set unconditionally. SPIRV-Cross at both pins **translates the `gl_BaryCoord` built-ins** to MSL `[[barycentric_coord]]`, but **throws** on the per-vertex input form: "PerVertexKHR decoration is not supported in MSL." (pin `6c09849`; main's pin `aa217ae`: `:3926-3927`). `pervertexEXT` inputs are the form Azahar's G2 route uses | `MVKDevice.mm:575-577, 3548-3550, 2635-2645`; `SPIRV-Cross@6c09849 spirv_msl.cpp:14032-14033, 18470-18476, 4242, 4298` (built-ins), `:3920-3921` (throw) |
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
| framebuffer fetch | MoltenVK does not set SPIRV-Cross's option `use_framebuffer_fetch_subpasses` (it appears only in the pipeline-cache serialiser; the SPIRV-Cross default is false). With `VK_EXT_rasterization_order_attachment_access` absent, MoltenVK 1.4.2 gives no framebuffer fetch, and its own source says so: "Using memoryless as an input attachment requires shader framebuffer fetch, which MoltenVK does not support yet. TODO: support framebuffer fetch so VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT uses color(m)" (`MVKImage.mm:957-959`). `VK_KHR_dynamic_rendering_local_read` is listed; what it compiles to was not traced (`unverified`) | `MVKPipeline.mm:2888` (1.4.1: `:2848`; main: `:2820`); `MVKImage.mm:957-959` |
| fast math | `MVK_CONFIG_FAST_MATH_ENABLED` default **2** ("on demand": fast math unless a shader's float-controls limit it), not 1 | `Docs/MoltenVK_Configuration_Parameters.md:201-210`; `MVKEnvironment.h:183-184` |

**KosmicKrisp, from its source** (`Mesa@f3f63ef`): "implemented on top of Metal 4, which requires macOS 26 and up" (`docs/drivers/kosmickrisp.rst:4-8`); `.logicOp = true`, done by lowering the blend in the fragment shader (`src/kosmickrisp/vulkan/kk_physical_device.c:240`, `kk_shader.c:420-428`; that the destination read becomes a Metal framebuffer fetch is `inference`); no `geometryShader`; neither `EXT_fragment_shader_interlock` nor `KHR_fragment_shader_barycentric` in the extension table (`kk_physical_device.c:95-215`; a barycentric *property* line at `:896` is a leftover in the properties struct, not an exposure); `EXT_custom_border_color` is `KK_EXPERIMENTAL(CUSTOM_BORDER)`, off unless `MESA_KK_EXPERIMENTAL=custom_border` (`kk_physical_device.c:177`; whether it works on an M2 is `unverified`); present id/wait are in the table (`:150-166`), their semantics were not read.

**MoltenVK's status.** The README of `main` (`5189f8d`) carries a deprecation notice added on 2026-10-13 (`0aa76ad`): "Khronos has deprecated MoltenVK ... maintained by the community on a best-effort basis ... Khronos now recommends KosmicKrisp on Apple Silicon Macs. MoltenVK remains the recommended option for Intel-based Macs and for iOS" (`source: MoltenVK@main README.md:6-14`). The notice is not in the `v1.4.2` tag; `Docs/Whats_New.md` on main calls the next release "1.4.3 - Released TBD"; no release followed 1.4.2.

**Apple's tables, native Metal on an Apple8 GPU** (the M2: "M2-series | Metal 3 & 4 | Apple8", `FST p.2`; a feature is available from the family named in its row onward):

| Topic | Apple's row | M2 |
|---|---|---|
| Framebuffer fetch / programmable blending | "Programmable blending", family Apple2 (`FST p.3`); MSL fragment input `color(m)`, "macOS: Metal 2.3 and later" (`MSL p.146`). That the table row and the attribute are the same capability is the usual reading (and `metal.md`'s), not a sentence in either document (`inference`) | available |
| Raster order groups | "Raster order groups", family Apple4 (`FST p.4`); "Maximum number of raster order groups, per fragment function: 8" for Apple4 to Apple10 (`FST p.7`); `[[raster_order_group(index)]]` on textures and device buffers (`MSL p.137`) | available, 8 |
| 24-bit depth | no row; `Depth24Unorm_Stencil8` exists for macOS only and is deprecated in 27.0 (Apple's `mtlpixelformat/depth24unorm_stencil8` page); MoltenVK's `isDepth24Stencil8PixelFormatSupported` is false on Apple GPUs | absent |
| Logic operations | `MTLRenderPipelineColorAttachmentDescriptor` and `MTLRenderPipelineDescriptor` list no logic-op property; the MSL spec has no logic-op text. "No public API" means *in the pages searched* (`unverified` for any other document) | none found |
| Min/max blending | `MTLBlendOperation` has `min` and `max` | native |
| Triangle fans; 8-bit indices | `MTLPrimitiveType`: point, line, lineStrip, triangle, triangleStrip; `MTLIndexType`: uint16, uint32 | none |
| Geometry stage | none in the tables or the MSL spec; object/mesh functions "Mesh shading", family Apple7 (`FST p.5`; an earlier brief said Apple9, the 2026 table says Apple7) | none; mesh available |
| Barycentrics, primitive id | Apple7 (`FST p.4`); "Access to pre-raster per-vertex values" Apple10 (`FST p.6`) | barycentric and primitive id yes; per-vertex access not on Apple8 (that this row is the counterpart of `pervertexEXT` is not established) |
| Border colours | `opaqueWhite` among three; "Border color", family Apple7 (`FST p.3`) | white available |
| Vertex stride alignment | 4 B for Apple2, Apple3 and Apple4, 1 B from Apple5 (`FST p.7`: "4B 4B 4B 1B 1B 1B 1B 1B 1B" under Apple2 to Apple10) | 1 B |
| Barriers inside a render pass | Apple3 to Apple10 do not support memory barriers that include the fragment or tile stage or `renderTargets` scope in a render encoder (`FST p.6`, note 3) | not available: "copy and barrier" inside a pass is not a Metal option here (`inference` for the port) |
| BC formats | "BC pixel formats", family Apple9, note 2: some Apple7/Apple8 GPUs support them in iPadOS (`FST p.3, p.6`); the pixel-format table lists them as "Varies" for Apple7 and Apple8 with note 9: "All Apple7 and Apple8 macOS devices have support" (`FST p.14, p.15`) | **available on a Mac M2** (macOS); the property `supportsBCTextureCompression` is the run-time test; report 52371 shows `textureCompressionBC true` |
| Packed 16-bit colour formats | `B5G6R5Unorm`, `A1BGR5Unorm`, `ABGR4Unorm`, `BGR5A1Unorm`: filter, colour, MSAA, resolve and blend on every family (`FST p.12`); note 5: `A1BGR5Unorm` and `ABGR4Unorm` are incompatible with samplers using the `opaqueBlack` border colour (`FST p.15`) | available as colour targets with blending |
| `R32Uint` | colour, write, sparse, and atomic from Apple6 on, MSAA from Apple7 on (`FST p.12`; "Texture atomics", Apple6, `FST p.5`) | available with atomics (the shadow write's format) |
| Depth and stencil formats | the list is `Depth16Unorm`, `Depth32Float`, `Stencil8`, `Depth32Float_Stencil8` and `X32_Stencil8` (`FST p.15`): no 24-bit format | `D32S8` is the stand-in |
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
vulkaninfo --json                                   # writes VP_VULKANINFO_<device>_<driver>.json for the FIRST GPU only
vulkaninfo --json=1                                 # the second GPU (a second driver); tell the files apart by the driver in the name
for f in VP_VULKANINFO_*.json; do ./mh4n_vkprobe --from-json "$f"; done    # one file per call: two files in one call exit with status 2 ("unexpected argument")
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
- The `yes` for barycentrics on MoltenVK means "listed": the `gl_BaryCoord` built-ins translate, but the per-vertex inputs that Azahar's G2 route needs do not.
- The `yes` for present id and present wait is the device's answer only; the wait is released at command-buffer completion (above).
- Two drivers can answer on a Mac with the LunarG SDK, so two `Apple M2` devices may appear: tell them apart by the `driver:` line of the table (`MoltenVK 1.4.x` or `KosmicKrisp ...`); the `VP_VULKANINFO_<device>_<driver>.json` names say which file is which (`vkprobe.md` line 66). KosmicKrisp needs macOS 26 or later; MoltenVK 1.4.2 needs macOS 12 or later (`Docs/Whats_New.md:29`).
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
| fragment shader barycentrics | yes (listed; built-ins translate, per-vertex inputs do not) | no | |
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
| G2 quaternion fix-up | yes `[pc]` geometry shaders and barycentrics | fb: skip the fix-up. `geometryShader` no `[R]`; barycentric built-ins translate, but the per-vertex inputs of Azahar's route throw in SPIRV-Cross `[B]` | fb: skip. no geometry, no barycentric extension `[K]` | no. `barycentric_coord` yes (Apple7); per-vertex access only Apple10; buffer-by-`primitive_id` and mesh routes untried `[FST]` | yes by geometry shader; barycentrics no `[lp]` |
| G3 D24S8 | fb: `D32_SFLOAT_S8_UINT` `[pc]` | fb: D32S8 `[R]` | fb: D32S8 `[K]` | fb: no 24-bit depth `[FST]` | yes (D24S8 and D32S8) `[lp]` |
| G4 shadow write and lookup | yes: storage writes, pixel interlock `[pc]` | yes (interlock listed = raster order groups); not run `[R][B]` | fb: no interlock, so the compare-and-swap loop `[K]` | yes: raster order groups, 8 per fragment function `[FST]` p.4, p.7 | yes `[lp]` |
| G5, G6 lookup tables, procedural textures | `?` (not probed; core Vulkan, `memory`) | yes `[R]` `maxTexelBufferElements` 67,108,864 | yes `[K]` 268,435,456 | yes `[FST]` p.8 (256 M px) | yes `[lp]` 134,217,728 |
| G7 gas | open (N2-10); no rasterization-order access, interlock yes `[pc]` | open; no framebuffer fetch from MoltenVK (`inference` from [3.3](#33-the-m2-stand-in)); `VK_KHR_dynamic_rendering_local_read` listed, untraced | open; local-read and feedback-loop extensions listed `[K]`, untraced | open; framebuffer fetch available | open; rasterization-order access and interlock yes `[lp]` |
| G8 plain draw state | `?` (not probed) | yes, but blend state is baked into pipelines (X8) and primitive restart is always on (X7) `[R][B]` | as MoltenVK, blend baked `[K]` | yes (blend factors, constant colour, write mask) `[FST]` | `?` |
| G9 formats | fb for D24; colour formats not probed `[pc]` | RGB8 no (use RGBA8); `R5G6B5`, `R5G5B5A1`, `A1R5G5B5`, `R4G4B4A4` are attachments with blending; the `B5G6R5`, `B5G5R5A1`, `B4G4R4A4` variants are sampled only; D24 no `[R]` | not compared one by one | yes: `B5G6R5Unorm`, `A1BGR5Unorm`, `ABGR4Unorm`, `BGR5A1Unorm` are filter/colour/blend targets (`FST` p.12); no 24-bit depth; `R32Uint` has colour and atomics (p.12) | RGBA8 attachments yes (plume experiment, [6](#6-plume-against-the-needs)); D24 yes `[lp]` |
| G10 pipelines off-thread | yes: graphics pipeline library, dynamic rendering, extended dynamic state 1 to 3 (18 of 31 bits) `[pc]` | fb: plain `VkPipelineCache`; no pipeline library; dynamic state 3 only 5 bits `[R]` | fb: same; 7 bits `[K]` | yes: binary archives (Apple3), function specialization (Apple2) `[FST]` p.4, p.5 | yes: pipeline library yes, 16 of 31 bits `[lp]` |
| G11 BC7 | yes `[pc]` | yes, `textureCompressionBC` true `[R]` | yes `[K]` | yes on macOS: "All Apple7 and Apple8 macOS devices have support" (`[FST]` p.15 note 9); check `supportsBCTextureCompression` at run time | yes `[lp]` |
| N1 all 16 logic ops | yes `[pc]` | fb: `logicOp` false; shader emulation does 4 of 16 `[R][az]` | yes `[K]` (shader-side lowering `[KS]`) | fb: no pipeline API; framebuffer fetch makes all 16 possible `[FST]` | yes `[lp]` |
| N2 triangle fans | yes `[pc]` | yes: `triangleFans` true, emulated by an indexed indirect list `[R][B]` | yes, emulated `[K][KS]` | no: rewrite in the backend `[FST]` | yes `[lp]` |
| N3 min/max blending | ops yes; PICA factors need a shader path unless both factors are One, on every host `[az]` | same | same (`?` for the ops) | ops native `[FST]`; whether the PICA semantics need fetch is not established | same as the PC |
| N4 8-bit indices | yes `[pc]` | yes, converted by a compute pass at draw time `[R][B]` | yes `[K]` | no: widen `[FST]` | yes `[lp]` |
| N5 odd strides | yes (full device) | yes: alignment 1 on Apple8, 4 below Apple5 `[R][B]` | yes (no portability subset) `[K]` | yes: 1 B from Apple5 (4 B on Apple2 to Apple4) `[FST]` p.7 | yes `[lp]` |
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

Azahar's Vulkan renderer, condensed from the research note. Read at `5f3306d`; nothing was built or run, so every statement about behaviour is a reading of the code. This is the specification of what any option must cover. (Paths under `src/video_core/`.) That the renderer is "proven on MH4U" is D5's claim and is `unverified` here: nothing in the code shows it, and Azahar's behaviour on this game's scenes would verify it.

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

`PicaCore::DrawArrays` refuses acceleration when `use_gs == Yes` ("Geometry shaders cannot be accelerated due to register preservation"); those draws run the vertex shader and the PICA geometry shader on the CPU and are drawn from a streamed CPU vertex batch (`DrawTriangles`) with a trivial vertex shader. `PipelineRegs::UseGS` has only the values `No` and `Yes`, so the later checks in the Vulkan rasterizer are unreachable for a geometry-shader draw (`inference`). `source: azahar@5f3306d pica/pica_core.cpp:1052-1092`, `pica/regs_pipeline.h:164-167`, `renderer_vulkan/vk_rasterizer.cpp:423-451, 628-640`. The census saw the Point mode in 39 to 90 percent of the draws: those draws need only a streamed vertex buffer and a trivial vertex shader from the RHI. **The quaternion fix-up for those draws is done on the CPU on every host**: `AddTriangle` flags vertex 1 and 2 when the dot product of their quaternion with vertex 0's is negative (`source: azahar@5f3306d rasterizer_accelerated.cpp:85-90`) and the `HardwareVertex` constructor negates the quaternion of a flagged vertex (`:52-54`). Only a lit draw shaded on the GPU by the translated PICA vertex shader needs one of the two GPU routes, in this order: fragment barycentrics (`pervertexEXT`), else a fixed geometry shader (`triangles` in, `triangle_strip` out, used only when lighting is on), else no fix-up (`source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:549-558, 1427-1433`, `glsl_shader_gen.cpp:384-431`, `vk_pipeline_cache.cpp:501-515`).

### 5.5 Gas is drawn by nobody

Both fragment generators, on fog mode `Gas`, log "Unimplemented gas mode" and write transparent black (the code comment: discarding "causes graphical issues in some MH games"); in the SPIR-V generator a gas fragment still writes depth, in the GLSL one it does not (`inference`). The software renderer's fog function acts only on `FogMode::Fog`, so it draws a gas fragment as an ordinary one. The 16 gas registers are `INSERT_PADDING_WORDS(0x10)` in Azahar: no state, no table, no dirty tracking. `source: azahar@5f3306d shader/generator/glsl_fs_shader_gen.cpp:171-181, 835-842`, `spv_fs_shader_gen.cpp:70-77, 197-207`, `renderer_software/sw_rasterizer.cpp:800-828`, `pica/regs_framebuffer.h:15-19, 298`.

**What 3dbrew says** (`GPU/Internal_Registers`, retrieved 2026-10-23, "last edited on 18 October 2026"): fog mode 7 is gas; `GPUREG_COLOR_OPERATION` fragment operation mode 1 is Gas (0 Default, 3 Shadow), and colour buffer format 0 is "RGBA8/Gas"; `GPUREG_GAS_ATTENUATION`, `GAS_ACCMAX`, `GAS_LIGHT_XY`, `GAS_LIGHT_Z`, `GAS_LIGHT_Z_COLOR`, `GAS_LUT_INDEX`/`GAS_LUT_DATA` (a 16-entry colour table), `GAS_DELTAZ_DEPTH`; `GPUREG_GAS_ACCMAX_FEEDBACK`: bits 16 to 31 hold "the maximum plain density (blue/alpha bytes of the color buffer) written by gas density rendering", "compared as unsigned 16-bit integers". The page does **not** say how the shading pass gets the density, nor the per-fragment accumulation formula (`unverified`; N2-10 or a hardware capture). *If* mode 1 combines each fragment's density with the value already in the colour buffer (`inference` from the register names and the running maximum), it needs a read-modify-write of the render target in the fragment stage: framebuffer fetch, raster-order access, interlock, or a copy per draw, plus a CPU-readable maximum. Open for N2-10; Azahar has no code to copy.

### 5.6 Layouts, barriers, passes

- **Every surface image lives in `VK_IMAGE_LAYOUT_GENERAL`** (initial transition `UNDEFINED` to `GENERAL` at creation); the exceptions are the presentation images: the offscreen frame ends its pass in `TRANSFER_SRC_OPTIMAL` and the swapchain image goes through `TRANSFER_DST_OPTIMAL` to `PRESENT_SRC` (`azahar@5f3306d renderer_vulkan/vk_present_window.cpp:411-449, 537-538`). A transfer operation barriers `GENERAL` to `TRANSFER_{SRC,DST}_OPTIMAL` and back with per-surface stage and access masks; roughly two image barriers per image operation; the current layout is always known, so nothing is tracked. Depth reads in compute use `DEPTH_STENCIL_READ_ONLY_OPTIMAL`. Whether `GENERAL` costs bandwidth on the target GPUs is `unverified`. `source: azahar@5f3306d renderer_vulkan/vk_texture_runtime.cpp:127-148, 1170-1195`.
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

`source: azahar@5f3306d`, counted with a script (`python3 -I`) that removes `//` comments, `/* */` comments (string-aware) and whitespace and counts what is left; `.cpp` and `.h` only; 42 files; re-run in this work and reproduced. Raw string literals (the GLSL inside `R"( )"`) are counted as code lines and their content is not stripped; a counter that also strips `//` inside raw strings gets 5,071 for `shader/generator/` and 1,512 for `glsl_fs_shader_gen.cpp` instead of 5,074 and 1,515.

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

plume (https://github.com/renderbag/plume, MIT, commit `0ce184b`) is a C++17 render interface over Direct3D 12, Vulkan and Metal, "originally created to be used by RT64" (`source: plume@0ce184b README.md:3`). The research read the three public headers and `plume_vulkan.cpp` in the parts that matter, and only grepped `plume_d3d12.cpp` and `plume_metal.cpp`. In this section: `vk.cpp` is `plume_vulkan.cpp`, `vk.h` is `plume_vulkan.h`, `types.h` is `plume_render_interface_types.h`, `iface.h` is `plume_render_interface.h`, `metal.cpp` is `plume_metal.cpp`; all at `plume@0ce184b`. Verdicts: `exposes` (the API has it), `needs extending` (a patch makes it reachable), `no equivalent`, `not applicable`. Sizes are changed lines, counted for the Vulkan backend unless stated: `inference` unless marked **measured**, which means a patch written and run in [6.9](#69-validation-layer-run-of-plume). "LCD" is plume's stated design as "lowest common denominator between modern graphics APIs" (README.md:1): *fits* means the addition is a capability bit or something all three APIs have, *fights* that it is Vulkan-only.

### 6.1 The table

| ID | Verdict | plume element (file:line) | Patch points and size |
|---|---|---|---|
| G1 | exposes | buffer flags `STORAGE`, `VERTEX`, `INDEX` (types.h:418-430); heaps `DEFAULT`/`UPLOAD`/`READBACK`/`GPU_UPLOAD` (types.h:364-370); storage-buffer ranges (vk.cpp:520-527); every binding is `VK_SHADER_STAGE_ALL` (vk.cpp:1174) | none. Streaming rings are application work. `map()` never calls `vmaFlushAllocation` or `vmaInvalidateAllocation` (vk.cpp:904-917; both names absent), so it relies on coherent memory (`inference`) |
| G2 | geometry shader exposes; barycentrics need extending | `RenderGraphicsPipelineDesc::geometryShader` (types.h:1281), stage added vk.cpp:1440-1449, capability vk.cpp:4154; Metal asserts "Metal does not support geometry shaders!" (metal.cpp:1547); no barycentric extension anywhere | barycentrics: extension name (vk.cpp:75-92), a feature struct in both chains, a capability bool: about 20 lines + 6 for the other backends; fits LCD |
| G3 | needs extending | enum has `D16_UNORM`, `D32_FLOAT`, `D32_FLOAT_S8_UINT` only (types.h:126-127, 138); no D24 case in `toVk` (vk.cpp:166-191); depth+stencil copies use one `VkBufferImageCopy` with both aspects (vk.cpp:728-734, 3251) | D24S8 enum + size + `toVk`: about 15 lines (D3D12 and Metal 6 each; **Apple silicon has no D24**); a format-support query (X1): about 40 lines; the aspect split for depth+stencil readback (Vulkan allows one aspect per copy region, `memory`): about 20. **Neither real target has D24S8, so the missing enum value costs nothing there; the missing query does** |
| G4 | needs extending (interlock; zero attachments; a mutable-format view only if the RGBA8-viewed-as-`R32_UINT` design is kept); compare sampler and white border exposed | compare: `comparisonEnabled`/`comparisonFunc` (types.h:1399-1400, vk.cpp:1335-1336); `OPAQUE_WHITE` (types.h:327-332, vk.cpp:579-591); storage image `READ_WRITE_TEXTURE`/`STORAGE` (vk.cpp:516-517, 990); `R32_UINT` is in `RenderFormat` (types.h:129, vk.cpp:172-173) | **No interlock** (error IDs `VkShaderModuleCreateInfo-pCode-08740`/`-08742` under the layer): extension, feature struct in both chains, capability: **19 lines measured** (patch 02). **No zero-attachment pass**, in three places, not one: the framebuffer takes its extent only from its attachments (vk.cpp:2546-2553, 2595-2598), so width and height are 0; the pipeline's viewport and scissor count is `renderTargetCount`, which is 0 (vk.cpp:1501-1507); the render area is 0 (vk.cpp:3498-3513): **15 lines measured** (patch 01, with the flag below). A dummy attachment avoids all three (a workaround, not run). **No `MUTABLE_FORMAT`** (error `VkImageViewCreateInfo-image-01762`; no occurrence of the flag in the file): needed only if the shadow map stays an RGBA8 colour target viewed as `R32_UINT`, Azahar's design; a native `R32_UINT` texture with the `STORAGE` flag needs no view (vk.cpp:990), which is the no-patch route (8.2). **The fixed `COLOR_ATTACHMENT_OPTIMAL` layouts are not a blocker of the shadow write**: the pass has no attachment, a storage descriptor takes the layout the caller names (vk.cpp:1983-1984; `toImageLayout`, vk.cpp:694-722) and a barrier can set `GENERAL` (types.h:475); they bite only a pass that attaches an image in `GENERAL` (layer errors `initialLayout-00900`, `InvalidImageLayout`; patch 04, 9 lines, optional) and a renderer that samples what it renders (6.4). The compare-and-swap fallback needs no extension, only the zero-attachment fix (or a dummy attachment) and a storage format. Patched: right pixels and no layer message with and without interlock (6.9). Azahar needs neither the compare sampler nor the border (5.3) |
| G5, G6 | exposes | `FORMATTED_BUFFER` texel buffers (types.h:345, vk.cpp:938-956); `TEXTURE_1D` | none |
| G7 | copy-and-barrier exposes; framebuffer fetch has no equivalent | `copyTexture`/`copyTextureRegion` (iface.h:143-145; vk.cpp:3207-3347); barriers (vk.cpp:2830-2892); no `INPUT_ATTACHMENT` descriptor type (types.h:342-355), no subpass dependency or input reference in either render-pass builder (vk.cpp:1678-1741, 2618-2634) | every `barriers()` call and every copy ends the active render pass (vk.cpp:2838, 3208). Input attachments: a descriptor type, an input reference and a self-dependency in two builders, `GENERAL`: about 70 lines, Vulkan-only, **fights LCD** (D3D12 has none, `memory`) |
| G8 | needs extending (blend constant; stencil reference and masks) | blend factors, ops, write mask (types.h:1203-1235; vk.cpp:1563-1585); stencil, depth, cull, front face, depth bias (types.h:1283-1298, 204-215); viewport and scissor dynamic (vk.cpp:1613-1614), up to 8 render targets; dual-source factors `SRC1_COLOR`/`INV_SRC1_COLOR`/`SRC1_ALPHA`/`INV_SRC1_ALPHA` (types.h:262-265) | **There is no blend-constant setter in any backend**: the command list has none (iface.h:114-152), the Vulkan pipeline has no `VK_DYNAMIC_STATE_BLEND_CONSTANTS` (dynamic states are viewport, scissor and, if asked, depth bias, vk.cpp:1611-1618) and leaves `blendConstants` zero (vk.cpp:1580-1585); measured: a `BLEND_FACTOR` blend gave (0,0,0,0), and with `setBlendConstants(0.5)` 128 (**12 lines**, patch 07, Vulkan only; D3D12 and Metal about 14 more, `inference`). A no-patch route: dual-source blending carries the constant in a second fragment output (`dualSrcBlend` is on in the lavapipe listing and in report 52371; not probed on the PC). **The stencil reference, compare mask and write mask are members of the pipeline description in all three backends**, so a different value is a different plume pipeline everywhere: Vulkan bakes them (types.h:1292-1294; vk.cpp:1600-1608); D3D12 stores the reference in the pipeline (plume_d3d12.cpp:3149) and applies it through `checkStencilRef` (:2466-2473); Metal stores it in the pipeline state (metal.cpp:1609) and sets it at draw (:3447). Azahar sets the compare mask, write mask, reference and blend constants dynamically (`azahar@5f3306d renderer_vulkan/vk_graphics_pipeline.cpp:232-236`, `vk_pipeline_cache.cpp:416-432`). Making them dynamic: Vulkan about 30 lines (`inference`); D3D12 and Metal about 12 more for the reference, and the masks cannot be dynamic in D3D12 (pipeline state: **fights LCD**). Vertex strides are baked into the pipeline (vk.cpp:1471), so a different stride is another pipeline (`inference`) |
| G9 | needs extending | `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `D16_UNORM` exist; **no RGB8, RGB565, RGB5A1, RGBA4, D24** (types.h:93-170) | new enum values, size entries (types.h:519-707) and `toVk` cases: about 40 lines; RGB8 has no portable 24-bit format on D3D12 or Metal, so RGB8 is an RGBA8 emulation in any design. No format-support query (X1) |
| G10 | needs extending (cache) | `vkCreateGraphicsPipelines(device->vk, VK_NULL_HANDLE, ...)` and the compute twin pass no `VkPipelineCache` (vk.cpp:1652, 1393); every graphics pipeline creates its own `VkRenderPass` (vk.cpp:1631) and every framebuffer another (2634); no dynamic rendering, pipeline library or extended dynamic state | measured: `pipelineCache=(nil)` on all 42 calls and 44 `vkCreateRenderPass` for 42 pipelines and 2 framebuffers (20 identical pipelines own 20 passes); a `VkPipelineCache` on the device with `loadPipelineCache`/`savePipelineCache` and a render-pass cache keyed by formats: **76 lines measured** (patch 06; 3 passes for the same 42 pipelines; the saved blob was 32 bytes on lavapipe, header only). Vulkan-specific (**fights LCD**). The scratch vectors are `thread_local` (vk.cpp:1420, 1462-1464), which suggests pipeline creation was meant to run on several threads (`inference`). The compile time saved on a real driver is `unverified` |
| G11 | exposes | `BC7_UNORM`/`_SRGB` (vk.cpp:246-251); buffer-to-image `copyTextureRegion` with `PLACED_FOOTPRINT` (vk.cpp:3217-3237) | none. A worker-thread upload needs a queue: plume has **no queue-family ownership transfer** (images are `VK_SHARING_MODE_EXCLUSIVE`, vk.cpp:986; barriers use `VK_QUEUE_FAMILY_IGNORED`, 2874-2875), so upload on a second *direct* virtual queue, not on a transfer-only family |
| N1 | Vulkan exposes; Metal has no equivalent | `RenderLogicOperation`, all 16 (types.h:286-303), `logicOpEnabled`/`logicOp` (types.h:1305-1306, vk.cpp:1582-1583); D3D12 maps them (plume_d3d12.cpp:327-335); no occurrence of `logicOp` in `metal.cpp` | no capability bit: the Vulkan feature is enabled iff the device has it (vk.cpp:4081), and a pipeline asking for it on a device without it would be invalid (MoltenVK: `unverified`) |
| N2 | exposes | `TRIANGLE_FAN` (types.h:201), `triangleFan` capability (types.h:1806): **constant `true` off Apple, forced `false` on Apple** with the comment that MoltenVK "does so via compute shaders to translate to lists ... This results in renderpass restarts that can harm performance, so force disable native triangle fan support and rely on the game to emulate fans" (vk.cpp:4173-4180); Metal asserts (metal.cpp:652) | none for the Mac; the flag is not read from the device (`inference`: on another portability-subset device it would be wrong) |
| N3 | exposes | `MIN`/`MAX` (types.h:273-274; vk.cpp:400-417); Metal maps them (metal.cpp:723-725) | none; the PICA factor semantics are a shader matter (5.6) |
| N4 | needs extending | `toIndexType(R8_UINT)` returns `VK_INDEX_TYPE_UINT8_EXT` (vk.cpp:673-677), **but** `VK_EXT_index_type_uint8` is neither in the extension list nor in a feature chain | extension + feature struct + capability: about 20 lines; Metal and D3D12 have none (**fights LCD**); the widening in N2-05 is the portable route |
| N5 | exposes (no handling) | `stride` copied as is (vk.cpp:1469-1473) | none on a full device; the portability-subset repack stays ours |
| N6 | exposes (cube) | `RenderTextureFlag::CUBE` (types.h:444, vk.cpp:996-998); `TEXTURE_CUBE` views | none |
| R1 | needs extending (the structure is right) | `CreateVulkanInterface()` and `createDevice()` are separate (vk.cpp:4652-4680); the extension lists are `static const` sets (vk.cpp:46-69), so no extra instance extensions unless the SDL option is on, in which case plume adds SDL's instance extensions and creates the surface itself (vk.cpp:4534-4559, 2098-2110) — **but that option is Linux-only** (CMakeLists.txt:20-21); otherwise the surface is created inside `VulkanSwapChain` from a `RenderWindow`: `HWND` on Windows, `{NSWindow*, view*}` on Apple (types.h:43-68), **Xlib `{Display*, Window}` only without the SDL option on Linux**; no `VK_EXT_headless_surface` (no occurrence); the queue family is chosen by flags (vk.cpp:4012-4044) and present support is tested afterwards, failing with "Command queue does not support present." (vk.cpp:2155-2166) | (1) extra instance extensions: ~15; (2) an external `VkSurfaceKHR` in `RenderSwapChainDesc`: ~25; (3) headless surface: ~25; (4) a present-capable family: ~20: about **85 lines** (`inference`), fits LCD. Around plume instead: our own `VkSwapchainKHR` over plume's public handles, wrapping images with the public `VulkanTexture(VulkanDevice*, VkImage)` constructor (vk.h:94): about 250 lines (`inference`). That item exists for N2-21's external-surface and headless need, not for windowed presentation, which plume's own `HWND`/`NSWindow` paths already serve on Windows and macOS. **Headless works** (6.5) |
| R2 | timeline needs extending; the lock exposes | each native queue has a `std::unique_ptr<std::mutex>` (vk.h:392-396), taken in `executeCommandLists` (vk.cpp:3654-3657) and `present` (2290-2293); sync is fences and **binary** semaphores only (iface.h:68-74; vk.cpp:3533-3575); no timeline anywhere in the Vulkan backend | timeline semaphores: feature struct, capability, `createTimelineSemaphore`, `VkTimelineSemaphoreSubmitInfo` with wait and signal values: **63 lines measured** (patch 05, including the rework of patch 03), fits LCD. **All semaphore waits use the single stage `COLOR_ATTACHMENT_OUTPUT`** (`waitStages`, vk.cpp:3635, used at 3639): measured, a fragment-shader sample after a producer whose last barrier does not cover that stage gives `SYNC-HAZARD-READ-AFTER-WRITE` (C2, and C5 with a timeline at the default stage); clean with a per-wait stage mask (**17 lines**, patch 03); clean unpatched when the producer ends with a `GRAPHICS`-stage barrier (C1, A2). **`pWaitDstStageMask` points at that one local variable for any number of waits**: with two waits the call trace shows `[0]=0x400 [1]=0x4`, a read past the variable (undefined behaviour; the layer is silent). Cross-queue behaviour is `unverified` (lavapipe has one queue) |
| R3 | exposes | `presentWait` capability = `presentId && presentWait` features (types.h:1796; vk.cpp:3880-3891, 3978-3985, 4164), **independent of a surface**; `present` attaches `VkPresentIdKHR` (vk.cpp:2278-2287); `wait()` calls `vkWaitForPresentKHR` with a **100 ms** timeout (vk.cpp:2309-2316); `displayTiming` = `VK_GOOGLE_display_timing` listed (vk.cpp:4165), used only by `getRefreshRate()` (2455-2464); **MAILBOX is detected (vk.cpp:2201) but not used**, FIFO or IMMEDIATE only (vk.cpp:2415-2425; commit 2a66aee, 2026-01-16, reverted it as the default "as it causes presentation issues"); resize returns false at 0x0 (vk.cpp:2321-2324) | `PresentWaitAvailable()` = `getCapabilities().presentWait`. On the RX 6700 XT plume would report both false (`inference` from the probe). The capability sits on the device while N2-21 says "with a surface". Plume's Metal backend sets `presentWait = true` unconditionally (metal.cpp:3876): what that measures is a Mac question |
| R4 | exposes | all backend types are public structs in `vk.h`: `VulkanInterface::instance` (:452), `VulkanDevice::vk`/`physicalDevice`/`queueFamilyIndices`/`queueFamilies[i].queues[j].vk`/`.mutex` (:392-412), `VulkanCommandList::vk` (:297), `VulkanFramebuffer::renderPass` (:269) | none. The experiment printed the instance, physical device, device, family indices and queue counts by `static_cast` |
| R5 | exposes | readback: `copyTextureRegion` into a `ReadbackBuffer` then `map` (iface.h:143, vk.cpp:3238-3262; commit #114 is the one at HEAD); timestamps `writeTimestamp` and `queryResults`, converted to nanoseconds (vk.cpp:2741-2749) | `queryResults` does not pass `VK_QUERY_RESULT_WAIT_BIT` (vk.cpp:2713), so the fence is waited first. `ReadImage` is about 40 lines of ours over `copyTextureRegion`. No image-to-image blit and no format conversion (no `vkCmdBlitImage`), so conversions between PICA formats and the Morton write-back are compute work of ours |
| R6 | exposes | `VOLK_IMPLEMENTATION` and `VMA_IMPLEMENTATION` at the top of `vk.cpp` (:8-9); `volkInitialize()` failure prints and `CreateVulkanInterface()` returns `nullptr` (vk.cpp:4498-4502, 4677-4680); the Vulkan headers are vendored (CMakeLists.txt:89-94) | **plume owns the one volk and the one VMA implementation**: `native/third_party/rhi/` must not compile a second one. Instance-level pointers are loaded, device-level ones are not (`volkLoadDevice` absent; only `volkLoadInstance`, vk.cpp:4623), so device calls go through the loader's dispatch (`inference`: a small per-call cost). The failure is `fprintf` plus a null return, not an error object |
| R7 | exposes | built and run headless on lavapipe (6.5), and run under the validation layer (6.9) | the RX 6700 XT and MoltenVK legs were not run |
| R8 | Vulkan exposes; Metal needs extending | 6.3 | run-time MSL for Metal: about 50 lines in `MetalShader` |
| R9 | no equivalent | zero matches for raster order or framebuffer fetch anywhere ([6.7](#67-d4s-row-checked-against-the-headers)); no logic ops, geometry, fans or 8-bit indices in `metal.cpp` | [7](#7-metal) |
| X1 | no equivalent | no `vkGetPhysicalDeviceFormatProperties` call in `vk.cpp` | about 40 lines + equivalents in the other two |
| X3 | no equivalent for `*_SSCALED`/`*_USCALED`; the integer formats exist (`R8G8B8A8_UINT`/`SNORM`/`SINT`, `R16G16B16A16_SINT`, ...) | no `SSCALED`/`USCALED` in the headers or `vk.cpp` | the integer fallback with shader conversion (Azahar's own fallback) works without a patch |
| X2 | exposes | depth targets are ordinary (`D16_UNORM`, `D32_FLOAT`, `D32_FLOAT_S8_UINT`); the depth output is a shader matter | none |
| X4 | needs extending | see G4: no zero-attachment framebuffer, pipeline viewport count or render area | **15 lines measured** (patch 01); a dummy attachment is the workaround |
| X5 | no equivalent | `CONSTANT_BUFFER` maps to a plain `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` (vk.cpp:506-509); root descriptors assert "not supported in Vulkan" (vk.cpp:3008-3012); push constants exist (iface.h:125-132) | Azahar's three dynamic-offset uniform buffers over one ring have no plume equivalent and are in no total of 6.8 before this revision: a dynamic-offset constant-buffer type and an offset argument, about 80 to 120 lines (`inference`), or a descriptor-set update per draw |
| X5b descriptor sets per draw | no equivalent (pooled sets) | `VulkanDescriptorSet`'s constructor creates its own `VkDescriptorSetLayout` (`VulkanDescriptorSetLayout`), its own `VkDescriptorPool` and allocates one set; the destructor destroys both (plume_vulkan.cpp:1885-1944) | Azahar makes a fresh texture set (four writes) and a utility set per draw from pooled heaps recycled by tick (5.7); on plume a per-draw set means a layout, a pool and an allocation per draw. A set recycler in the wrapper or a shared-pool patch: about 100 to 150 lines (`inference`) |
| X6 | exposes | `copyTexture` plus the automatic render-pass restart: a copy ends the pass and the next draw begins a new one (`checkActiveRenderPass`, vk.cpp:3498-3513) | no whole-image copy-with-resume operation; ~80 to 120 lines of ours (`inference`) |
| X7 | not applicable | plume sets `primitiveRestartEnable = VK_TRUE` for strip topologies (vk.cpp:1497); there is no control for lists | none |
| X8 | exposes (nothing dynamic) | blend state is pipeline state in plume on every host; no dynamic blend state | none; the pipeline key includes the blend state |
| X9 | exposes | MAILBOX is detected (vk.cpp:2201) but not used; FIFO or IMMEDIATE (vk.cpp:2415-2425) | none; this matches MoltenVK's limits |

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
- **Layout and stage tracking live in the resource** (`VulkanTexture::textureLayout`, `barrierStages`) and are updated **while recording** (a texture's: vk.cpp:2883-2884; a buffer's `barrierStages`: 2862-2863). **Measured under the validation layer (6.9):** two threads recording barriers for one texture produce about 22,000 layer messages (`oldLayout-01197`, `InvalidImageLayout`) and ThreadSanitizer reports at plume_vulkan.cpp:2876 and 2882-2884; a list recorded in one order and submitted in the other gives `InvalidImageLayout`; a per-texture owner rule in the caller (one mutex around recording, lists recorded in submission order) gave 0 messages and 0 reports.
- `barriers()` ends the active render pass first (:2838), uses `srcStageMask = TOP_OF_PIPE | the last barrier's stages` and `dstStageMask = BOTTOM_OF_PIPE | the requested stages`, access masks `MEMORY_READ | MEMORY_WRITE` on both sides with a `// TODO` (:2842-2873), and covers the whole resource (all mips and layers). Stage groups are only graphics, compute and copy (types.h:450-460).
- Render passes: one subpass, no dependencies, `LOAD_OP_LOAD`/`STORE_OP_STORE`, fixed `COLOR_ATTACHMENT_OPTIMAL` (:1678-1741, 2618-2634). A pass starts at the first draw or clear after `setFramebuffer` and ends at the next barrier, copy, `setFramebuffer` or `end()`.
- **What it means:** render then sample is clean unpatched (6.9, A1, A2, C1, D1). A draw that samples a surface rendered earlier in the frame needs a barrier to `SHADER_READ` and back, and each barrier ends the pass (counted in the trace: two passes for two draws with a sample between), so the next draw begins a new one with load and store (on a tile-based GPU such as the M2 that is a tile round trip; unmeasured). A draw that samples the surface it is drawing to is not expressible (the barrier ends the pass and the layouts are fixed): Azahar's feedback copy (5.6) would be needed, and plume has no whole-image copy-with-resume operation beyond `copyTexture`. For the capture player, which replays serially, this is workable; for a renderer thread plus a present thread sharing images it needs the owner-per-resource rule above, which then binds the compositor too. The README says exactly these parts, "barrier management and texture transitions", are still being refined.

### 6.5 The experiment: plume's Vulkan backend on lavapipe, headless

Built out of tree (plume's own `CMakeLists.txt` unmodified, plus a 91-line test program and an 8-line `CMakeLists.txt`) with clang 18.1.3, Debug; nothing built inside the clone or the repository:

```
git clone --shallow-since=2025-09-01 --recurse-submodules --shallow-submodules https://github.com/renderbag/plume <dir>   # the 76-commit and per-month figures of 6.6 come from a full clone (history deepened afterwards)
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
- Not done in this first run: a Release build, a validation-layer run (the layer was not installed on the machine then; it installs with apt, and the run was made afterwards: [6.9](#69-validation-layer-run-of-plume)), a swapchain (no display), a multi-thread submission test, any pipeline (so geometry-shader, fan and logic-op pipelines are read from code, not run).
- The measured follow-up, with the layer, patches and threads, is [6.9](#69-validation-layer-run-of-plume).

### 6.6 The project

- **Licences:** MIT, "Copyright (c) 2024 renderbag and contributors" (LICENSE:1-3). Vendored: volk (MIT), VulkanMemoryAllocator (MIT), Vulkan-Headers 1.4.335 (Apache-2.0 OR MIT), D3D12MemoryAllocator (MIT, Windows only), metal-cpp (Apache-2.0; committed into the tree, 1.4 MB, not a submodule). MIT and Apache-2.0 code can go into a GPL-3.0 work (`memory`: Apache-2.0 is compatible with GPLv3, not GPLv2; not checked against the FSF page).
- **Size:** `wc -l`: `plume_vulkan.cpp` 4,682; `plume_metal.cpp` 4,280; `plume_d3d12.cpp` 4,263; `plume_render_interface_types.h` 1,822; `plume_metal.h` 784; `plume_d3d12.h` 495; `plume_vulkan.h` 469; builders 281; interface 268; about 17,500 for the 11 top-level files. The Vulkan-only slice (cpp + h + the three shared headers) is **7,522**. Vendored: `volk.c` 3,577, `volk.h` 2,725, `vk_mem_alloc.h` 19,111.
- **Backends are chosen by platform, not by a CMake option:** Vulkan always; Metal `if(APPLE)`; D3D12 `elseif(WIN32)` (CMakeLists.txt:57-83). To ship Vulkan only on Windows we list the sources ourselves (`inference`). The SDL options are Linux-only and exclusive.
- **Activity:** 76 commits from 2025-02-28 to 2026-10-21, no tags, no releases. Commits per month for the last 12: 2025-11 0, 2025-12 17, 2026-01 5, 02 1, 03 0, 04 0, 05 4, 06 0, 07 1, 08 0, 09 3, 10 3: **34 in the 12 months**, 9 distinct author names (two of them, "Darío" and "Dario", are probably one person: `inference`); 14 of the 34 subjects name Metal or D3D12 (a case-insensitive search of the subjects; the research note said 12). Earlier months were busier (2025-03 8, 2025-07 13, 2025-08 20). Of those 34 commits, 11 touch `plume_vulkan.cpp`, 5 `plume_vulkan.h`, 6 `plume_render_interface_types.h`, 1 `plume_render_interface.h`. A public-API change in the window: `createSwapChain` now takes a `RenderSwapChainDesc` (2026-01-08, `dea63d3`).
- **Users, as far as the repository says:** RT64 only (README.md:3). The brief's mention of Unleashed Recompiled is `unverified`.
- **Tests: none.** No test directory; the only workflow, `validate.yml`, builds the library and the triangle example on five platforms and runs nothing. `RenderInterfaceTest` and `TestInitialize`/`TestDraw`/... in the interface header (iface.h:261-265) are example hooks.
- **README:** "While feature-rich and stable, it is not considered to be production-ready or to have stable API until a few crucial components such as barrier management and texture transitions are further refined" (README.md:3).
- **Contribution policy:** `CONTRIBUTING.md` line 1 reads "AI must not be used to generate any sort of contributions to this project, including code, issue reports or pull requests." Consequence for the options: patches written in AI-assisted sessions, which is how this repository is worked on (its `CLAUDE.md` says so), cannot go upstream, so a vendored copy with a patch queue stays ours. The policy line goes on: "Usage of AI tools to communicate with the maintainers of this project is also disallowed." So agents never file an issue, a pull request or a message to the plume project (CONTRIBUTING.md line 1); vendoring the MIT code is unaffected.

### 6.7 D4's row checked against the headers

D4 says plume has "no interlock, raster order groups or framebuffer fetch exposed; unstable API". A case-insensitive search of every `plume_*.cpp`, `plume_*.h` and `plume_apple.mm` for `interlock`, `raster order`, `RasterizerOrdered`, `ROV`, `input attachment`, `INPUT_ATTACHMENT`, `subpassLoad`, `framebuffer fetch`, `programmableBlend` and `imageblock` finds **zero matches** (re-run in this work, 2026-10-23; the research note's list also holds `raster_order`, the same result). **Confirmed.** "Unstable API" is confirmed by README.md:3 (which also calls it "stable" in the same sentence) and by the `RenderSwapChainDesc` change above. Nothing contradicts the row; [11](#11-corrections-to-earlier-documents-found-while-checking) lists what the row lacks.

### 6.8 The cost in lines

Lines are our changed or added lines; the tokens that follow from them are in [8.3](#83-cost-in-tokens-and-on-the-critical-path), with one conversion. `inference` unless marked measured.

**As it is (option 1).** N2-21 gets for free: device and queues with a submit mutex, VMA, buffers, textures, views, samplers, pipelines, implicit render passes, copy and readback, timestamps, swapchain with FIFO/IMMEDIATE and present wait, volk loading, SPIR-V in, cube, BC7, D16/D32S8, logic ops, min/max, fans (Apple forced false), geometry shaders. Built around plume:

| Item | Size (our lines) | Consequence |
|---|---:|---|
| R1 surface from SDL3, extra extensions: own swapchain over plume's handles | 250 | duplicates plume's swapchain; needed for the external-surface and headless need |
| R2 timeline values: one binary semaphore per producer-consumer pair, fences; the producer ends with a `GRAPHICS`-stage barrier (clean in 6.9, C1) | 60 | never two waits in one submission; cross-queue `unverified` |
| G8 blend constant | 100 | dual-source blending (a second fragment output and a factor mapping), or a pipeline per constant colour |
| G9, G3 narrow and 24-bit formats | 200 | readback needs conversion shaders anyway |
| G10 pipeline cache | 40 | the volk-pointer wrapper (hack), else none; no render-pass cache |
| X1 format queries: call `vkGetPhysicalDeviceFormatProperties` ourselves | 40 | fine |
| **Subtotal (the first-draft table)** | **690** | |
| G4 shadow: a native `R32_UINT` texture with `STORAGE`, a dummy attachment, the compare-and-swap loop, conversion copies | 200 | unordered even where interlock exists; the `R32_UINT` surface handling is `unverified` |
| X5 dynamic-offset uniform buffers: a descriptor update per draw | 100 | |
| X5b a descriptor-set recycler (plume makes a layout, a pool and a set per set object) | 100 to 150 | |
| Combined samplers lowered to separate texture and sampler | 150 | shader side or wrapper |
| **Total** | **1,240 to 1,290** | |

N2-22, N2b-05 and N2b-09 inherit the stage-mask workaround. The census-seen needs that become fallbacks: G4, G8, G3/G9, G10.

**Extended (options 2a, 2b).** The patch queue, Vulkan lines. Where a patch was written in the run of 6.9 the figure is **measured** (added + removed lines of the diff, applied in order to `0ce184b`); the rest is `inference`.

| Patch | Contents | Lines |
|---|---|---:|
| 01 (P7) | `MUTABLE_FORMAT` flag; framebuffer width and height; viewport and scissor count at least 1 | **15** |
| 02 (P2) | `VK_EXT_fragment_shader_interlock` in the optional extensions, feature struct in both chains, capability | **19** |
| 03 (P3) | one stage mask per wait semaphore (`executeCommandListsEx`) | **17** |
| 04 (P7) | `GENERAL` attachment layout for a pass that attaches the image (optional) | **9** |
| 05 (P3) | timeline semaphores (feature, capability, `createTimelineSemaphore`, submit info with values), including the rework of 03's signature | **63** |
| 06 (P4) | one `VkPipelineCache` per device, load and save, a render-pass cache | **76** |
| 07 (P5) | blend constants dynamic, `setBlendConstants` | **12** |
| **Measured, 01 to 07** | | **211** (202 without 04; 113 net for 01 to 05, rework 10 lines) |
| P1 | external and headless surface, extra instance extensions, a present-capable family (R1) | 85 |
| P5, rest | stencil reference, compare mask and write mask dynamic (G8) | 30 |
| P6 | formats D24S8, RGB565, RGB5A1, RGBA4, (RGB8); the format query; the depth-stencil aspect split (G3, G9) | 110 |
| P8 | MAILBOX, wait timeout (R3) | 20 |
| optional | barycentrics (G2) 20, uint8 (N4) 20, input attachments (G7) 70 | 110 |
| other backends (2a only) | the `+` cells of the first-draft queue: D3D12 and Metal bits for P2 (20), P3 (80), P5 (30), P6 (100) | 230 |
| P9 (2a, Metal) | run-time MSL in `MetalShader` | 50 |

The first draft estimated 582 Vulkan lines (512 without the optional input attachments) for P1 to P8; the measured patches plus the estimated rest come to 202 + 245 = **447** without the optional items. The D3D12 and Metal sources are not referenced from the Vulkan files (the shared header has `// D3D12 only` members but no code dependency, `inference`), so deleting them leaves a Vulkan-only layer of about 7.5k lines plus volk and VMA, but then a native Metal backend has nothing to extend. **For the patterns tested, on one native queue under lavapipe, no patch needed to change `barriers()`; the layout race is handled by rework (the owner rule), counted in 8.3.** Cross-queue transitions, ownership transfers, the RX 6700 XT's several queue families and MoltenVK are untested (9.2). The rework above the patches (the owner rule, the feedback-copy operation, lowering combined samplers, dynamic-offset uniform buffers, the descriptor-set recycler) is counted in 8.3, not here. The lines of the patches are counted as `git diff --numstat` counts them, added plus removed, with no context lines: 211 over the seven diffs, of which 183 are non-blank and 173 are neither blank nor comment-only. **Maintenance** (inference): upstream's last 12 months touched the functions we would patch (`VulkanDevice::VulkanDevice` at vk.cpp:3768-4188, `VulkanSwapChain`, `executeCommandLists`), so each touch is a rebase conflict; a pinned snapshot has none and gets no fixes; pinning `0ce184b` and re-basing only for specific upstream fixes is the cheapest regime, and numbered patch files keep each conflict local.

### 6.9 Validation-layer run of plume

A second experiment ran the document's inferences about plume under the Khronos validation layer. The harness, the seven patches, the shaders, the layer settings and a summary of the results are in the repository, in [`rhi-spike-evidence/`](rhi-spike-evidence/README.md) (`README.md`, `test/main.cpp` of 695 lines, `patches/01..07-*.diff`, `shaders/`, `env/`, `env-all/`, `run.sh`, `summarize.py`, `results-summary.txt`, `LICENSE-plume.txt`; `SHA256SUMS` lists 23 files and has itself the sha256 `afd62dcc478c5af5b7adb9dbb46330fa5a14d3e76d4a407c38901feb8507fc52`). The folder holds the summary of the run, not its 184 log files: the call traces (the `pWaitDstStageMask` values, the render-pass and pipeline counts of E, the blend constant of F), the ThreadSanitizer output and the best-practices pass come from the original run's logs in the session scratch folder (`scratchpad/work/plume-val/logs/`, not in the repository), re-created by the README's commands. `source:` for the table below: `results-summary.txt` where it has the row, otherwise those logs.

**Setup.** plume `0ce184b` pristine and a copy with the seven numbered patches (replayed byte-identical on a clean copy); lavapipe (`llvmpipe`, Vulkan 1.4.318, one queue family with one queue); `VK_LAYER_KHRONOS_validation` 1.3.275 (apt `vulkan-validationlayers 1.3.275.0-1`), synchronization validation on, enabled by `VK_INSTANCE_LAYERS` and by plume's own Debug path; clang 18.1.3, Debug; `glslangValidator -V` for the shaders. plume registers no debug messenger, so the layer's messages go to stdout; the test program prints its own `[T]` lines on the same stream and a call trace of `vkQueueSubmit`, `vkCmdPipelineBarrier`, `vkCreateRenderPass`, `vkCreateGraphicsPipelines` and `vkCmdBeginRenderPass` through volk's global function pointers (no plume code touched). No version-gap warnings appeared (the layer validates the instance as 1.2 because plume requests 1.2). Each pattern ran as its own process; exit status 0 = expected behaviour, 1 = pixels wrong.

**Commands** (the README's): `sh shaders/build.sh`; `cmake -S test -B build-pristine -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DPLUME_SRC=$PWD/plume` and the same with `-DPLUME_SRC=$PWD/plume-patched -DPLUME_PATCHED=ON` (plume-patched is a copy of plume with the patches applied in order by `git apply`); `sh run.sh <label> <build>/plume_val <pattern>` for each pattern; `VAL_ENV=env-all sh run.sh pristine-all build-pristine/plume_val D2` to count every message of the race; `python3 -I summarize.py pristine` and `patched`. The call trace is `PLUME_VAL_TRACE=1`. The ThreadSanitizer build (g++ 13, because clang 18 has no `libclang_rt.tsan` here) is **not** a recipe in the folder; `results-summary.txt` records its counts.

**The table** (the rows B1, B2, C1, C2, C3, C5, A1, A2, D1, D2, D3, E and F were re-opened in the logs or the summary for this document). "Layer" lists distinct message IDs. A control is deliberate misuse.

| Pattern | What it does | Unpatched plume | Patched plume |
|---|---|---|---|
| A1, A2 | render into X, barrier to `SHADER_READ`, render Y sampling X; one list / two lists, two submits | clean; right pixels | clean; right |
| B1 | zero-attachment pass, `imageLoad`+`imageStore` on an `R32_UINT` view of an `RGBA8` image, no interlock | 6 IDs: `ImageViewCreateInfo-image-01762` (no `MUTABLE_FORMAT`), `FramebufferCreateInfo-width-00885` and `height-00887`, `PipelineViewportStateCreateInfo-viewportCount-04135` and `scissorCount-04136`, `RenderPassBeginInfo-None-08996`; **pixels wrong** (256 of 256 texels differ), exit 1 | clean; right |
| B2 | as B1 with `pixel_interlock_ordered` | the 6 plus `ShaderModuleCreateInfo-pCode-08740` and `-08742` (8); wrong | clean; right |
| B3, B4 | compare-and-swap loop, without / with interlock | the 6 / the 8; wrong | clean; right |
| B5 to B7 | `GENERAL` barrier between draws / no barrier / draws in separate lists with no barrier (controls) | the 6 (B7 plus `SYNC-HAZARD-WRITE-AFTER-WRITE`); wrong | clean (B7: the hazard, as a control should) |
| B8 | draw into a colour attachment in `GENERAL` | `initialLayout-00900` and `InvalidImageLayout` | clean (patch 04) |
| C1 | producer ends with a `GRAPHICS` barrier, binary semaphore, second submission samples | clean; right | clean |
| C2 | producer ends with a `NONE`-stage barrier; the semaphore alone carries the dependency | **`SYNC-HAZARD-READ-AFTER-WRITE`** on the fragment-shader sample | clean with a per-wait stage |
| C3 | two wait semaphores | layer silent; trace: `pWaitDstStageMask` `[0]=0x400 [1]=0x4` (a read past plume's local) | `[0]=0x7ce [1]=0x7ce` |
| C4, C5 | timeline semaphore with the right stage / left at the default (control) | not run (no timeline) | clean / `SYNC-HAZARD-READ-AFTER-WRITE` |
| D1 | thread 1 draws and submits, thread 2 waits on the fence, then samples | clean | clean |
| D2 | two threads record 20 barriers each on one texture, 2,000 rounds | the recorded run: 22,168 messages (`oldLayout-01197` 20,235, `InvalidImageLayout` 1,933; the unpatched build gave 17.7k to 23.5k over five runs); ThreadSanitizer: races at plume_vulkan.cpp:2876, 2882, 2883, 2884 | the same defect (patches do not touch it): 23,718 messages in its recorded run (21,761 + 1,957), outside the unpatched five-run range, so the counts vary by run |
| D3 | D2 with an owner rule in the test program (a mutex per texture around recording, lists recorded in submission order: 4 lines in the harness) | 0 messages, 0 overwrites, 0 TSan reports | the same |
| D4 | recorded 1, 2; submitted 2, 1 (control) | `InvalidImageLayout` | the same |
| E | 42 pipelines, 2 framebuffers | `pipelineCache=(nil)` on all 42 calls; 44 `vkCreateRenderPass`; blend constants (0,0,0,0) | 3 render passes (patch 06), cache accepted by a second device; blob 32 bytes (lavapipe: header only) |
| F | `BLEND_FACTOR` blend, source (1,1,1,1) | pixel 0 0 0 0 | pixel 128 128 128 128 with `setBlendConstants(0.5)` (patch 07) |

(The patched D2 split, 21,761 + 1,957, is 65,283 / 3 and 3,914 / 2 in `results-summary.txt`: each message prints its ID more than once.)

**The patches** (`rhi-spike-evidence/patches/*.diff`, Vulkan backend only; D3D12 and Metal not compiled): see the table in 6.8: 15, 19, 17, 9, 63, 76 and 12 lines, **211 changed lines** by `git diff --numstat` (added plus removed, no context lines; 183 non-blank), 202 without the optional 04; 113 net for 01 to 05 with 10 lines of rework (03's signature was rewritten when 05 added the timeline). The set the first draft named (P2, stage mask, P4, P7: patches 01, 02, 03, 04, 06) is 136 lines, 199 with the timeline. **The experiment author states** that every patch compiled at the first build and that the effort was about 2 to 5 thousand tokens per patch, about 30 thousand for the seven, about 70 thousand for the 695-line harness, about 50 thousand for reading plume's code, about 150 to 200 thousand tokens in all; the folder's own README says the effort figures and the compile statement are the author's, not something its files show. They are not used in the cost conversion of 8.0 (a different regime: no tests, a layer that gives the answer).

**What it confirms.** The stage mask (conditional: the hazard appears when the producer's last barrier does not itself cover the consumer's stage); the one-element stage mask (by the trace; the layer is silent); the resource-held layout state race (layer, ThreadSanitizer and the test); the whole-resource `MEMORY_READ|MEMORY_WRITE` masks (trace; the best-practices pass flags them, `ImageBarrierAccessLayout`); one `VkRenderPass` per pipeline and no `VkPipelineCache` (trace); no blend-constant setter; no `MUTABLE_FORMAT`; no interlock; the zero-attachment pass, which is **wider than the first draft said** (three errors, not one); and that hard-coded `COLOR_ATTACHMENT_OPTIMAL` is not needed for the shadow write itself. **What it adds:** render-then-sample is clean unpatched; the patches that were written cost about what was estimated for the same items (9.1); the one defect no diff repairs is the layout state, which an owner rule in the caller covers in the test.

**What it does not confirm or cannot test.** The RX 6700 XT (AMD's driver, several queue families, a different interlock, real compile times); MoltenVK and the M2; cross-queue behaviour (lavapipe has one queue, so the stage-mask defect is shown on one native queue and its cross-queue consequence is `unverified`); the Release build; a swapchain; more than one colour target, depth, stencil, vertex buffers, push constants, multisampling; a long soak of the two-thread patterns (2,000 rounds, one run each). The layer is 1.3.275, older than the device (1.4.318), so checks for newer features cannot exist. **Lavapipe accepts every invalid call** (a wrong-format view and a zero-size framebuffer returned handles; the draws then did nothing), so only the layer sees them: any RHI's CI must run under it. **The layer's synchronization validation did not flag storage-image hazards between draws inside one command buffer** (not even the no-barrier control B6) but did across two command buffers (B7), so silence on the compare-and-swap patterns B1, B3, B5 says nothing about their ordering; the interlock variants give the right texels on lavapipe, their ordering on a real GPU is unverified.

**What a follow-up could add.** The folder is evidence, not code of the port. If D4 picks option 2, the patches would move to `native/third_party/rhi/` as numbered patch files and the harness to `native/tools/` (with `run.sh` and the summariser); the RX 6700 XT and Mac legs of the same patterns, and a second queue, would be run with it. Nothing outside `docs/research/rhi-spike.md` was added by this document.

## 7. Metal

The rule of [`metal.md`](metal.md) (2.2, lines 164-166: reading the framebuffer in the fragment shader and "It pays only for Metal features that Azahar's Vulkan renderer doesn't use: reading the framebuffer ... and raster order groups"; 3.2 stage 2, line 281; line 29; and the list of what only Metal allows in stage 3.5, lines 287-290): framebuffer fetch (`[[color(n)]]`) and raster order groups are items of a *native* Metal backend. "Unless the Vulkan-on-Metal driver already gives them" is this document's gloss, which the tables below apply. On the Mac the port runs on MoltenVK now ([`metal.md`](metal.md) 3.1); a native backend starts only on a trigger (3.3).

### 7.1 Which needs meet which mechanism

| Need | Seen in those scenes? | Metal mechanism it would use | MoltenVK 1.4.2 gives it? | KosmicKrisp gives it? |
|---|---|---|---|---|
| G4 shadow-write ordering | seen | **raster order groups** (8 per fragment function on Apple4 and later) | **yes**: `VK_EXT_fragment_shader_interlock` (pixel and sample) listed, mapped to raster order groups; not run | **no**: no interlock extension; the compare-and-swap loop is the only route |
| N1 all 16 logic ops | not seen | **framebuffer fetch** (no logic-op API in Metal) | **no**: `logicOp` false (private-API builds only); the shader path does 4 of 16 | yes: `logicOp` true, a shader-side blend lowering whose destination read is probably a fetch (`inference`) |
| N3 min/max blending | not seen | Metal has native `min`/`max`; framebuffer fetch only where the PICA factor semantics need a shader path (not established) | ops yes; the PICA factor case uses the copy path | not read |
| G7 gas | seen (cutscene) | **framebuffer fetch**, if the mode reads the destination colour (open, N2-10) | no framebuffer fetch (MoltenVK's own TODO, `MVKImage.mm:959`); local read listed, untraced | open |
| G2 quaternion fix-up | seen | neither: no geometry stage; `barycentric_coord` and `primitive_id` exist (Apple7); per-vertex access only on Apple10; the routes of `metal.md` 3.2 (a buffer written by the vertex stage and read by `primitive_id`; mesh functions) are untried | no: the barycentric built-ins translate, the per-vertex inputs Azahar's route needs do not; no geometry | no |
| N2 fans, N4 8-bit indices, N5 strides | not seen | neither: Metal has no fan type and no 8-bit index; strides are 1 B from Apple5 (4 B on Apple2 to Apple4) | yes (fans emulated, indices converted, alignment 1 on Apple8) | yes (fans emulated) |
| G1, G3, G5, G6, G8 to G11 | seen | neither: backend features or conversions | see [3.3](#33-the-m2-stand-in) | |

**What neither gives.** Inside a Metal render pass, Apple3 to Apple10 cannot make a fragment-stage memory barrier, so "copy and barrier" on a Mac is a pass break (X6), not a barrier (`FST p.6`, note 3).

### 7.2 What a later Metal backend would have to add, per option

| Option | What a native Metal backend adds | State of the base |
|---|---|---|
| 1 plume as it is | raster order groups, framebuffer fetch, logic ops, a run-time MSL path, and everything the Vulkan side needed for G4 | plume's Metal backend (4,280 lines) was read by grep only: it has no raster order groups, no fetch, no logic ops (`metal.cpp`: no occurrence), asserts on geometry (:1547) and fans (:652), maps only 16- and 32-bit indices, and takes only a metallib. How much of its render-pass and barrier code survives was not read |
| 2a plume extended | the same, as patches in `metal.cpp`: P9 (about 50 lines), raster order groups through SPIRV-Cross's output of `SPV_EXT_fragment_shader_interlock` (`memory`, `unverified`), fetch through `[[color(n)]]` inputs and pass configuration (not estimated) | as above, plus about 330 lines of our patches to keep honest |
| 2b plume's Vulkan backend vendored | a Metal backend of our own behind the wrapper, plus the Metal compositor, presenter and ImGui path as above; plume's Metal source is deleted | the wrapper is ours; plume's descriptor model (argument-buffer style, no combined samplers) is Metal-friendly but is hidden below the wrapper |
| 3 our own thin RHI | a Metal backend of the RHI: about **2,850 to 4,450 lines** (`inference`: the sum of the six `metal.md` 2.3 rows for device and capabilities 300 to 500, submission 400 to 600, stream buffers 150 to 250, render passes 200 to 300, pipelines and state 900 to 1,300 and shaders and disk cache 900 to 1,500, taken whole; the texture runtime, blit helper, rasterizer and presentation rows belong to the renderer above the RHI or to N2b). `metal.md`'s 6,000 to 9,000 lines is the scope of a native renderer for Azahar's `video_core`, not of an RHI backend. **On top of it,** the Vulkan-free boundary is the renderer-facing interface only: N2-21's raw handles (R4), the compositor (N2b-05), the swapchain and present thread (N2b-06) and Dear ImGui stay Vulkan, so a Metal backend also needs a Metal compositor, presenter and ImGui path outside these lines | the interface keeps the Vulkan idioms out of the renderer's view ([9.4](#94-rhi-design-requirements-the-evidence-imposes)); "Metal-shaped" is not claimed beyond that |

### 7.3 New facts that bear on `metal.md`'s trigger rule

- **MoltenVK is deprecated** by Khronos on `main` (2026-10-13, README notice; the commit is `0aa76ad`): maintained "by the community on a best-effort basis"; Khronos recommends KosmicKrisp on Apple Silicon and keeps MoltenVK for Intel Macs and iOS. No release since 1.4.2 (2026-07-20). `source: MoltenVK@main README.md:6-14`. The "Platform" trigger of `metal.md` 3.3 (MoltenVK and KosmicKrisp both break on a macOS release and stay broken) is no nearer by itself, but the first of the two now has no maintainer commitment.
- **KosmicKrisp needs macOS 26 or later** (Metal 4; `Mesa@f3f63ef docs/drivers/kosmickrisp.rst:4-8`). `metal.md` 3.2 says macOS 15.
- **KosmicKrisp has no interlock and no barycentrics**, has `logicOp`, and offers custom border colours only behind `MESA_KK_EXPERIMENTAL=custom_border`. If the Mac follows Khronos' recommendation, the shadow write there is the compare-and-swap loop (atomic, not primitive-ordered; whether the result differs on the captured Arena scene is `unverified`). If that shows visibly, `metal.md`'s "Picture" trigger fires, and a native backend's raster order groups are the way back to ordered writes (`inference`).
- **MoltenVK's `present_wait` is released at command-buffer completion**, not at display. [D57](../native/DECISIONS.md) measures "p99 ≤ 8.3 ms" from displayed times; on MoltenVK `PresentWaitAvailable()` would say yes while measuring something earlier. `VK_GOOGLE_display_timing` (listed on MoltenVK, not on KosmicKrisp) is fed by the drawable's presented time. `source: MoltenVK@v1.4.2 MVKSwapchain.mm:246-285`, `MVKImage.mm:1677-1681, 1712-1724`. The RHI must report what its wait measures ([9.4](#94-rhi-design-requirements-the-evidence-imposes)).
- **MoltenVK does not give framebuffer fetch** (4 of 16 logic ops stay emulated): `metal.md` 3.2's "On Apple GPUs, MoltenVK can turn input attachments into framebuffer reads (my understanding)" is not found in 1.4.2's source, whose own comment says framebuffer fetch is not supported yet (`MVKImage.mm:957-959`) ([11](#11-corrections-to-earlier-documents-found-while-checking)).
- **MoltenVK 1.4.x has fans and, on Apple8, byte strides**; the "cheap fixes in the shared Vulkan renderer" of `metal.md` 3.2 stage 2 that turn fans into lists are still right for speed (plume's own comment: the emulation "results in renderpass restarts"), but not for correctness.

## 8. The three options

The options are D4's: (1) plume as it is, (2) plume extended, (3) a thin RHI of our own over Vulkan, shaped so that a Metal backend fits later, with Azahar's Vulkan backend as a reference under D6. SDL_GPU is already ruled out (D4). Option 2 comes in two forms here, **2a** (D4's: a vendored copy with patches, all three backends kept) and **2b** (the variant the plume notes suggest and D4 does not name: vendor only plume's Vulkan backend, trimmed, MIT licence kept, as the Vulkan layer under an operation-level wrapper of ours). After the validation-layer run ([6.9](#69-validation-layer-run-of-plume)) three further distinctions matter and are made below: whether the wrapper above plume is thick (the renderer never sees plume's types), whether plume's files stay a patch queue on a pristine copy or become our code, and whether option 3's Vulkan layer is written from Azahar's pattern or seeded from plume's code.

### 8.0 The cost basis

Every cost below is `inference` from line counts, with one conversion.

**Calibration from this repository.** Source lines (non-blank, non-comment, `.cpp` and `.h`, counted with the script of 5.9, which removes `//` and `/* */` comments and counts raw-string literals as code) and test lines of the merged packages sized M in `docs/native/STATUS.md`:

| Package (size, agent) | Source lines | Test lines |
|---|---:|---:|
| N2-24 Vulkan probe (M, native-worker): `native/src/vkprobe`, `native/tools/vkprobe`; `vkprobe_test` | 1,802 | 564 |
| N2-02 Capture player (M, high): `native/src/gpu_capture`, `native/tools/capture_player`, `gpu_debug.h`; `capture_player_test` | 3,300 | 2,113 |
| N2-03 GX engine (M, native-worker): `native/src/gpu/gx`; `gpu_gx_test` | 807 | 1,131 |
| N2-04 PICA command processor (M, high): `native/src/gpu/pica`; `gpu_pica_test` | 1,369 | 1,109 |
| N2-14 Textures (M, native-worker): `native/src/gpu/texture`; `gpu_texture_test` | 920 | 858 |
| N1-11 IPC replay (M, native-worker): `native/src/ipc_replay`, `native/tools/ipc_replay`; `ipc_replay_test` | 2,345 | 2,165 |
| N1-18 DSP (M, high): `native/src/services/dsp`, `native/src/audio`; `dsp_test` | 1,443 | 1,606 |

**Selection.** `STATUS.md` lists 57 done packages sized M; these seven are 12 percent of them, a convenience sample: the done M packages whose allowlist names source directories and a test file of their own, so that their lines can be counted without ambiguity. It is not random: three are GPU packages (N2-03, N2-04, N2-14) and two of those port Azahar code. The counter handles digit separators such as `1'000` (a plain count gives 814 for N2-03 and 2,191 test lines for N1-11); the spread below is wide, so another sample could move the central rate by a third; **the rate scales every option's absolute tokens together and leaves the ratio between options unchanged**. An M delivered a **median 1,443 source lines (mean 1,712, range 807 to 3,300)** and a median 1,131 test lines (range 564 to 2,165). At the plan's nominal M = 400k tokens that is a **median 3.6 source lines per 1,000 tokens (range 2.0 to 8.2)**, with the tests inside the package budget. The repository records no token spend per package, so the nominal size stands in for it (`inference`). **Conversion used throughout: 3.5 source lines of ours per 1,000 tokens, tests inside** (the budget of an M already pays for its tests, which run about as long as the source); tokens = lines ÷ 3.5. Spread: at 2.5 (hard code such as Vulkan synchronisation) every figure is 1.4 times larger; at 5 it is 0.7 times. Ports from Azahar (N2-03, N2-14) are inside the sample, so the rate already covers copying under D6. Units: S = 150k, M = 400k, L = 1M tokens; "+0.4M" is one M. (The plume run of 6.9 spent about 30k tokens on seven small patches, 6 to 7 lines per 1,000 tokens, but with no tests and with a layer that printed the answer: that is a different regime and is not used.)

### 8.1 What each option is

- **Option 1, plume as it is.** The pinned commit `0ce184b`, taken whole into `native/third_party/rhi/plume/` with its `contrib/` volk, VMA and Vulkan-Headers, unmodified, driven through its public API; everything missing is built *around* it (6.8). Maintained by renderbag and contributors; no tags or releases; 34 commits in the last 12 months. plume is in production in RT64 (README.md:3), which N2-07 and N2-16 already cite as their reference for the ubershader and the pipeline list. What that says: plume's Vulkan backend carries a real renderer on three APIs. What it does not say: RT64 is not a PICA renderer; whether it needs a storage write ordered by interlock, an image viewed in another format, a timeline across a renderer thread and a present thread, or a blend constant is `unverified` (a read of RT64's use of plume would verify it; the gaps of 6.1 are what *we* need that plume lacks).
- **Option 2a, plume extended.** The same copy plus a numbered patch queue (6.8), all three backends kept compiling. Maintained by us; upstream does not accept AI-assisted contributions (6.6).
- **Option 2b, plume's Vulkan backend, trimmed, under a thick wrapper.** Only `plume_vulkan.*` and the three shared headers (7,522 lines), volk, VMA and the Vulkan headers; the D3D12 and Metal sources, `metal-cpp` and D3D12MA deleted; plume's licence kept beside the code and listed in `native/THIRD_PARTY.md`; the numbered patches of 6.9 applied on a pristine copy; and **an operation-level wrapper of ours above it** (the interface requirements of 9.4) so the renderer, the compositor and the tests never see plume's types. **How it differs from 2a:** no obligation to keep D3D12 and Metal compiling (about 330 lines saved) and about 10,000 lines of other-backend code and a 1.4 MB `metal-cpp` not carried; a native Metal backend then has nothing of plume to extend. **How it differs from 3:** it starts from 7.5k lines of working code, built and run under the layer, instead of Azahar's pattern written afresh; the sync and layout model is plume's, held to an owner rule (6.9) instead of replaced.
- **2b-seeded, a variant of 2b.** The same files, but copied into our tree as our code, edited freely, with no patch queue and no tracking of upstream (the patches of 6.9 become ordinary commits). With a thick wrapper this is 2b with a different regime: the cost in lines is the same; it gains the freedom to replace plume's barrier tracking, loses the ability to diff against upstream. It is also the same thing as **option 3 whose Vulkan layer is seeded from plume's code**: that variant differs from 2b only in how much of plume we keep, and it ends up in the same place the moment we keep most of it. The estimates below treat 2b and 2b-seeded as one cost.
- **Option 3, our own thin RHI over Vulkan, written from Azahar's pattern.** An interface at the level of Azahar's operation list (5.8), in C++20 under `native/src/gpu/backend/`, with `native/third_party/rhi/` holding only volk and VMA. Azahar's lines exist but need extracting: they use Vulkan-Hpp, Boost.Container, fmt, Citra `common/` and the rasterizer-cache headers, and four files carry yuzu copyright lines (5.9); each ported piece takes the provenance header of D6. plume (MIT) is a second reference (swapchain recreation, descriptor-layout building, headless use, and now a tested set of fixes). A Metal backend later, as an RHI backend ([7.2](#72-what-a-later-metal-backend-would-have-to-add-per-option)).

### 8.2 Which needs each option meets

N = natively; P = by a patch to plume (lines: measured in 6.9 where a patch number is given, otherwise `inference`); O = by our own code around plume (option 1) or inside our RHI or wrapper (options 2b and 3); W = by a workaround that avoids a patch.

| ID | Option 1 | Option 2a / 2b | Option 3 |
|---|---|---|---|
| G1 | N | N | O |
| G2 | N (geometry shader); barycentrics not enabled; on the Mac the fix-up is skipped anyway | P barycentrics (~20, optional) | O |
| G3 | O (D32S8 path and our own format query; plume's enum has no D24, which the real targets lack anyway) | P query (~40) and aspect split (~20) | O |
| G4 | **degraded, workaround size S (~200 lines, `inference`)**: a native `R32_UINT` texture with the `STORAGE` flag (no view, so no `MUTABLE_FORMAT`), barriered to `GENERAL`, one dummy attachment (which also removes the other two zero-attachment errors, 6.9), and Azahar's compare-and-swap loop. **Unordered even on a device that has interlock**, because the extension cannot be enabled without a patch. Whether the rest of the PICA surface handling (fills, copies, display transfers) works with an `R32_UINT`-typed shadow surface is `unverified` (a lavapipe run of plume's `copyTexture` between `R32_UINT` and `RGBA8` and the surface-cache design of N2-15 would settle it) | P: interlock (patch 02, 19 lines measured), zero attachments (patch 01, 15), mutable format (in patch 01) only if the RGBA8-viewed-as-`R32_UINT` design is kept | O (both paths, 5.3) |
| G5, G6 | N | N | O |
| G7 | N (copy and barrier) | N; P input attachments (~70, optional) if N2-10 needs them | O (open) |
| G8 | W: blend constant through dual-source blending (plume exposes `SRC1_COLOR`/`INV_SRC1_COLOR`, types.h:262-265; `dualSrcBlend` is on in the lavapipe listing and in report 52371; not probed on the PC): a second fragment output and a factor mapping, ~100 lines; stencil reference and masks: a pipeline per value (~25 distinct pipelines multiply) | P blend constants (patch 07, 12 measured, Vulkan only) and dynamic stencil reference and masks (~30, `inference`); D3D12 and Metal need ~26 more (2a) | O |
| G9 | O: RGBA8 and D32S8 emulation, conversion shaders (~200) | P formats (~50) | O |
| G10 | O: a volk function-pointer wrapper (~40) and no render-pass cache | P `VkPipelineCache` and render-pass cache (patch 06, 76 measured) | O |
| G11 | N (upload on a second direct queue, not a transfer family) | N | O |
| N1 | N (Vulkan hardware); Mac: O (shader emulation, 4 of 16) | same | O |
| N2, N3, N6 | N (fans: Apple forced false, so the rewrite is ours) | N | O |
| N4 | O (widen) | P (~20) or O (widen) | O |
| N5 | N | N | O |
| R1 | O (~250: our own swapchain over plume's handles, for the external-surface and headless need of N2-21; windowed presentation needs it only without plume's SDL option) | P (~85, `inference`) | O |
| R2 | W/O: a producer that ends with a `GRAPHICS`-stage barrier is clean (C1, 6.9); never two waits in one submission; one binary semaphore per pair (~60); cross-queue `unverified` | P timeline (patch 05, 63 measured) and per-wait stage mask (patch 03, 17) | O |
| R3, R4, R5, R6, R7 | N (`ReadImage` and the Morton write-back are ours in every option) | N | O |
| R8 | N (Vulkan); Metal not met: metallib only | N; Metal P9 (~50) | O (SPIRV-Cross on a Metal backend) |
| R9 | not met; plume's Metal has neither | P in `plume_metal.cpp` (not estimated) for 2a; for 2b a new Metal backend | O in a later Metal backend |
| X1 | O (~40) | P (~40) | O |
| X4 | W (a dummy attachment) | P (patch 01, three fixes) | O |
| X5 | O: no dynamic-offset uniform buffers in plume; a descriptor update per draw (~100) | P (~80 to 120, `inference`) or wrapper | O |
| combined samplers | O: lower to separate texture and sampler (~100 to 200, shader or wrapper) | O (same) | N (Azahar's model has them) |

### 8.3 Cost in tokens, and on the critical path

All figures are `inference`: lines of ours from the component tables, divided by 3.5 lines per 1,000 tokens (8.0), plus **0.05M to 0.10M for reading and learning** where plume's code is involved (the run of 6.9 spent about 50k tokens reading it; reading is a recurring cost, not a one-time one: each upstream fix we cherry-pick and each new patch means reading the code around it again). Operations present in both options carry **one basis and one number in both**: the common readback core (the two depth and stencil compute passes about 300 to 400 lines, the Morton write-back about 150 to 250, `ReadImage` and timestamps about 80 to 120: **530 to 770**) and the capability mask with the format chooser (**250 to 350**) are the same lines under 2b and under 3; option 3 adds the generic readback plumbing that plume's `copyTextureRegion` provides (100 to 200). What is counted once: the chooser logic in the wrapper (all options); the format-query call itself in P6 (2a, 2b) or in X1 (option 1); the conversion compute passes in the readback core (all options), so option 1's "G9, G3 200" is only the emulation of the formats plume's enum lacks.

**Option 3, by component.** Source lines of ours; Azahar's counted lines (5.9) are the basis; tests are inside the rate. The present window (530 lines) and the swapchain (324) are **N2b-06's** work in this plan and are in no option's total.

| Component | Azahar basis (counted lines) | Added | Lines | Slice |
|---|---|---|---:|---|
| C1 Instance, device, queues, capabilities, volk loading, test helper | `vk_instance` 836, `vk_platform` 411, `vk_common` 13, `vk_memory_util` 28 = 1,288 | the Vulkan-Hpp and window-system code drop out; add the external-surface and headless instance API and device policy | 600 to 800 | a |
| C2 Memory (VMA), buffers, ring buffers | `vk_stream_buffer` 266 | hand-written allocation replaced by VMA | 200 to 300 | a |
| C3a Submission, fence, submit lock | `vk_scheduler` 300 (part) | | 250 to 350 | a |
| C4a Descriptors, minimal | `vk_descriptor_update_queue` 126, `vk_resource_pool` 210 (part) | | 250 to 350 | a |
| C5a Pipeline creation from SPIR-V (graphics and compute) | `vk_graphics_pipeline` 504 (part) | | 300 to 400 | a |
| C7a A minimal render pass and framebuffer, zero-attachment capable | `vk_render_manager` 249 (part) | N2-06's vertex stage needs a pass (A4 below) | 100 to 150 | a |
| C6 Images, views, copies, blits, clears, barriers under one layout policy, the feedback copy | the image primitives inside `vk_texture_runtime` 1,672 (17 barrier literals, 21 `pipelineBarrier` calls) and `vk_blit_helper` 882: 30 to 45 percent assumed | the feedback-copy operation | 500 to 800 | b |
| C7b The rest of render passes and framebuffers | `vk_render_manager` 249 (rest) | | 100 to 150 | b |
| C8 Capability mask, format table and chooser | the format table of `vk_instance` | mask, query | 250 to 350 | b |
| C4b Descriptors, the rest (pools, recycling by tick, dynamic offsets) | | | 150 to 200 | b |
| C3b Timeline values, per-wait stage mask, deferred destruction | `vk_master_semaphore` 232 | | 200 to 300 | c |
| C5b Pipeline cache blob API, render-pass cache | cache part of `vk_pipeline_cache` 641 | | 150 to 200 | c |
| C9 Readback to PICA layouts (compute), depth and stencil compute passes, `ReadImage`, timestamps | compute passes of `vk_blit_helper` (~400 of 882); Azahar has no timestamps | the common core 530 to 770 + generic readback plumbing 100 to 200 | 630 to 970 | c |
| **Total** | | | **3,680 to 5,320** | |

Slice a (the core that N2-06 needs: device, buffers, one submit path, SPIR-V pipelines, a minimal pass, buffer readback): 1,700 to 2,350 lines, **0.49M to 0.67M**. Slice b (render targets, passes, barriers, the mask): 1,000 to 1,500 lines, **0.29M to 0.43M**. Slice c: 980 to 1,470 lines, **0.28M to 0.42M**. Total **3,680 to 5,320 lines: 1.05M to 1.52M tokens, central (4,500 lines) 1.29M (L)**; at the slow rate (2.5 lines per 1,000 tokens) 1.5M to 2.1M, at the fast rate (5) 0.74M to 1.06M.

**Option 2b (and 2b-seeded), by component.** Which lines are measured and which are estimates is marked.

| Component | Lines | Basis |
|---|---:|---|
| Vendor, trim, build integration, licence and `THIRD_PARTY` entry | 200 | guess |
| Patches written and run in 6.9: 01 mutable format and zero attachments 15, 02 interlock 19, 03 per-wait stage mask 17, 05 timeline 63 (including the rework of 03), 06 pipeline cache and render-pass cache 76, 07 blend constants 12 (patch 04, 9 lines, optional, not counted) | 202 | **measured** (`git diff --numstat`, added + removed lines; 183 of the 211 over all seven are non-blank, 173 non-blank and not comment-only) |
| Patches not written: surface, headless, extra instance extensions, a present-capable family 85; dynamic stencil reference and masks 30; formats, format query and the depth-stencil aspect split 110; MAILBOX and the wait timeout 20 | 245 | first-draft estimates |
| Rework above the patches, each range a guess with its stated width: the owner rule with its debug assertions (guess 60 to 150; the tested mutex form in the harness is 4 lines); the feedback-copy operation (Azahar's `CopyImageView` is about 100 physical lines: 60 to 120); lowering combined samplers (guess 100 to 200); dynamic-offset uniform buffers (guess 80 to 120); a descriptor-set recycler or a shared-pool patch, because plume's `VulkanDescriptorSet` constructor creates its own layout, its own pool and one set (plume_vulkan.cpp:1885-1944) while Azahar makes two sets per draw from pooled heaps (guess 100 to 150) | 400 to 740 | guesses |
| The wrapper for what N2-21 asks and plume lacks: the common readback core 530 to 770 (same as option 3), the mask and format chooser 250 to 350 (same as option 3, C8), device policy and the test helper 150 | 930 to 1,270 | as option 3 where the operation is the same |
| **Total** | **1,977 to 2,657** | of which 202 measured |

At 3.5 lines per 1,000 tokens: 0.56M to 0.76M, plus 0.05M to 0.10M reading: **0.62M to 0.86M, central (2,317 lines) 0.74M**; at the slow rate 0.84M to 1.16M. **1,775 to 2,455 of 2b's 1,977 to 2,657 lines are estimates, and so are all of option 3's.**

**Option 2a:** 2b plus 230 lines for the other two backends (the `+` cells of the patch queue: 20 + 80 + 30 + 100), 50 for P9 and about 50 to keep a three-backend build: **+330 lines, +0.09M: 0.71M to 0.95M**, central 0.83M.

**Option 1:** the workaround lines of 6.8: 690 (R1 250, R2 60, G8 100, G9/G3 200, G10 40, X1 40) + the shadow workaround 200 + dynamic-offset descriptors 100 + the set recycler 100 to 150 + combined-sampler lowering 150 = 1,240 to 1,290, plus the same wrapper 930 to 1,270 = 2,170 to 2,560 lines: **0.67M to 0.83M, central 0.75M**. The cost is about 2b's; what differs is that these lines sit outside plume, where the defects of 6.9 (the single-element stage mask, the layout state held in the resource, no interlock) cannot be reached.

**The gap between 2b and 3: 0.55M at the centres (1.29M against 0.74M), 0.2M to 0.9M across the two ranges (1.05M against 0.86M at the closest, 1.52M against 0.62M at the widest), about 1.4 M packages.** At one rate the ranges are separate; if the rate differs between the options (2b at 2.5 is 0.84M to 1.16M, 3 at 5 is 0.74M to 1.06M) they overlap, which is the honest size of the uncertainty. This is the one gap figure used elsewhere in the document.

**Plan size of the WPs.**

| WP | Option 1 | Option 2a | Option 2b | Option 3 |
|---|---|---|---|---|
| N2-21 Backend foundation (plan: M, 0.4M) | the wrapper, 0.27M to 0.36M (930 to 1,270 lines), plus the workaround work (1,240 to 1,290 lines, 0.35M to 0.37M) | the wrapper plus a patch WP **N2-21p** (about 0.35M to 0.50M including reading), plus 0.09M for the other backends | N2-21 shrinks to the wrapper, 0.27M to 0.36M, plus **N2-21p** (vendor, trim, the patches, the rework, tests, reading) about 0.35M to 0.50M | **grows from M to L**: 1.05M to 1.52M |
| N2-05, N2-07 | unchanged in size; released by D4 itself | same | same | same |
| N2-12 Shadows | grows by about S (the 200-line workaround) | unchanged | unchanged | unchanged; both paths built, tested through the mask |
| N2-13 Framebuffer operations | grows (the dual-source workaround) | unchanged | unchanged | unchanged |
| N2-16 Pipeline cache | a hack | unchanged | unchanged | unchanged |
| N2b-05, N2b-06, N2-22 | inherit the stage-mask workaround and a second swapchain (+250) | unchanged | unchanged | unchanged |

**The critical path.** Derived from `docs/native/STATUS.md` (`Depends on` and `Size`; script in the scratch folder, run with `python3 -I`; each package weighted S 150k, M 400k, L 1M, done packages 0): the longest chain of packages not yet done is **N2-01 → N2-21 → N2-06 (L) → N2-08 (L) → N2-23 → N2-09 (L) → N2-20 → N2-15 (L) → N2-16 (L) → N2b-12 → N2b-13 → ...** and on into N2b-19 to N2b-23 and N3, about 11.2M tokens in all. N2-12 (M) is a parallel branch into N2-20 within one package of N2-09's. N2-21 has five direct dependents: N2-06, N2-22, N2-13, N2-08 and N2b-05. N2-05 and N2-07 depend on D4 (through N2-01), not on N2-21, so **deciding D4 releases both whichever option wins**. What each needs from N2-21 (`inference`): **N2-06** (run the translated *vertex* shader on lavapipe, outputs to a storage buffer): a vertex stage means a graphics pipeline, which in plume needs a framebuffer and a pass, with the zero-attachment fixes or a dummy attachment; so the minimal pass is in slice a (C7a), or N2-06 would have to run its shader as compute, which a translated vertex shader is not. **N2-22** (a renderer thread; ordered submissions): a, with the timeline values of c for ordering. **N2-13** (blend, logic ops, depth and stencil): render targets, passes, pipeline state = b. **N2b-05** (the compositor): a, b, the timeline values and `ReadImage` of c, and N2-06. **N2-08** depends on N2-21, N2-06 and N2-13.

*File ownership.* The plan's rule is that packages that run at the same time share no files. Every N2-21 slice, N2-13 and N2-12 list `native/src/gpu/backend/**` in their allowlists. For the slices to run in parallel with each other and with N2-13 and N2-12, `backend/` must be split into subdirectories, one owner each (the orchestrator's decision; the specs are not edited here): **`backend/core/`** (slice a: device, memory, submit, pipelines, the minimal pass), **`backend/targets/`** (slice b: images, passes, barriers, formats, the mask), **`backend/sync/`** (slice c: timeline values, stage masks, the pipeline-cache API) and **`backend/readback/`** (slice c: readback, compute passes, `ReadImage`, timestamps), with N2-13 owning **`backend/state/`** (the mapping of blend, logic-op, depth and stencil state to pipeline state) and N2-12 owning **`backend/shadow/`** (the zero-attachment storage pass, the interlock and compare-and-swap paths). The public interface header is written once by slice a, with every operation of 9.4 declared, and then changes only through the orchestrator; the other slices implement in their directories. Without that split, N2-13 waits for slices b and c in turn.

*Without a split of N2-21:* the delay on the whole chain is the estimate minus the plan's 0.4M. Option 3 **+0.65M to +1.12M** (1.6 to 2.8 M packages); option 2b **+0.22M to +0.46M**; option 1 +0.27M to +0.43M; option 2a +0.31M to +0.55M; the difference between 3 and 2b is the gap above, 0.55M at the centres. *With N2-21 split as a / b / c and `backend/` split by directory:* N2-06 and N2-22 start after **N2-21a** (1.2 to 1.7 M); **N2-21b** runs in parallel with N2-06 (L, 1.0M) and N2-13 follows it (b plus N2-13: 0.69M to 0.83M, shorter than N2-06, so it adds no delay); **N2-21c** runs in parallel with N2-22 and N2-06. The delay is then slice a's excess over 0.4M: option 3 **+0.09M to +0.27M**. For the others the first slice is derived, not assumed: option 2b, vendor and trim 200 + surface patch 85 + patch 01 (15) + device policy and test helper 150 + submit glue 100 (guess) = 550 lines, 0.16M, plus reading 0.05M to 0.10M = **0.21M to 0.26M**, inside one M, and what follows it (0.41M to 0.60M, then N2-13's 0.4M: 0.81M to 1.0M) is not longer than N2-06; option 2a 600 lines, 0.22M to 0.27M; option 1 (policy 150 + glue 100 + a dummy attachment 50 = 300 lines) 0.14M to 0.19M. So the others add **about 0** and option 3 **+0.09M to +0.27M**, a fraction of one M package. *With the slices split but `backend/` not split by directory:* N2-13 waits for b and c (0.97M to 1.25M against N2-06's 1.0M): option 3 adds 0 to 0.25M more, **+0.09M to +0.52M** in all. The model assumes that elapsed time on the chain is proportional to the tokens of its packages and that nothing parallel absorbs a slip (`inference`).

### 8.4 Risks, what each leaves out, and the Mac path

| Option | Risks the experiment did not remove | Leaves out | The Mac path |
|---|---|---|---|
| 1 | G4 unordered on the PC and on MoltenVK even where interlock exists; a blend constant only through a dual-source workaround; the single-element stage mask stays reachable (never wait on two semaphores); cross-queue hazards `unverified`; 1,240 to 1,290 workaround lines outside plume | the ordered shadow write | plume's Vulkan backend is written to handle the portability subset (plume_vulkan.cpp:3909-3915, 3999-4002) and forces `triangleFan` false on Apple (4174-4180); it has not run on MoltenVK (`inference`); interlock is not enabled even where MoltenVK offers it |
| 2a | a permanent fork with no upstream; every upstream touch of a patched function is a rebase conflict; ~330 lines to keep two unused backends honest | nothing the notes found | patches add interlock for MoltenVK; native Metal later by patching `plume_metal.cpp` (not read end to end) |
| 2b | 7.5k lines of foreign code with no tests of their own, whose understanding is a recurring cost; **the synchronisation invariant (the owner rule of 9.4 item 5) spans our wrapper and plume's internals and lavapipe cannot test it well** (one queue, every invalid call accepted, the layer missed storage-image hazards inside one command buffer); cross-queue transitions, ownership transfers, the RX 6700 XT's several queue families and MoltenVK are untested | the Metal route through plume | as option 1, with interlock patched in; native Metal later is a new backend behind the wrapper |
| 3 | the cost (about 0.55M more than 2b); no prototype, so the whole 3,680 to 5,320 lines are `inference`; new synchronisation code is written and validated from scratch (under the layer, which installs with apt) | D3D12 (D4: only if needed) | as 2b |

### 8.5 The options side by side

| | 1 plume as it is | 2a plume extended | 2b plume's Vulkan backend, trimmed, thick wrapper | 3 own thin RHI |
|---|---|---|---|---|
| Meets the seen needs | degraded: G4 unordered, blend constant by dual-source, R2 by a usage rule | yes, by patches (202 lines measured + 245 estimated) and rework | yes, as 2a without the other backends | yes once built; no prototype |
| Cost, tokens (8.3) | 0.67M to 0.83M | 0.71M to 0.95M | 0.62M to 0.86M | 1.05M to 1.52M |
| On the critical path (unsplit / N2-21 split in three with `backend/` split) | +0.27M to +0.43M / about 0 | +0.31M to +0.55M / about 0 | +0.22M to +0.46M / about 0 | +0.65M to +1.12M / +0.09M to +0.27M |
| Code owned | 1,240 to 1,290 workaround lines + 7.5k foreign | 7.5k + 10k foreign lines and a patch queue | 7.5k foreign lines, patched, with 1,977 to 2,657 of ours | 3.7k to 5.3k own lines |
| Upstream | follows renderbag's public structs | permanent fork; patches cannot go back | permanent fork, but replaceable under the wrapper (9.1) | none |
| Tests that exist for the base | none; our tests run it under the layer | none | none for plume; the harness of 6.9 (695 lines) is a start | ours from the start |
| Synchronisation model | plume's, held to a usage rule | plume's, patched, owner rule | plume's, patched, owner rule (the only route that avoids patching plume) | Azahar's pattern (`GENERAL`, explicit barriers, nothing tracked); that it is "proven on this game's renderer" is D5's claim, `unverified` |
| Metal later | plume's Metal lacks all three; metallib only | patches in `plume_metal.cpp` | a new backend behind the wrapper | a new RHI backend, about 2,850 to 4,450 lines plus the Metal compositor, presenter and Dear ImGui path (7.2) |
| Lavapipe (R7) | runs | runs | runs; clean under the layer for the patterns tested | will run (a design requirement) |
| Evidence strength | read + two runs | read + patches run | read + patches run under the layer | Azahar's measured sizes; no prototype |


## 9. Recommendation

### 9.1 The choice, reassessed from the start

**Option 2b: vendor plume's Vulkan backend, trimmed, apply the measured patches on a pristine copy, and put an operation-level wrapper of ours above it. It is a close call against option 3; the tie-break is a maintenance preference and a schedule trade, and both belong to Luis (9.6).** The first version of this document recommended option 3; **the recommendation has changed**, and the reason must be stated exactly, because the size of the cost gap did not change much.

**What moved the recommendation, and what did not.**
- *The gap.* The first draft said option 3 cost "1 to 2 M packages" more than 2b, that is 0.4M to 0.8M. The corrected arithmetic (8.3) gives 0.55M at the centres (0.2M to 0.9M), which sits inside the old range. **The gap did not shrink; it was re-costed, with the same order of size.** The experiment did not make it smaller.
- *What the experiment moved* is the argument for paying the gap. (a) The first draft said the work 2b needs would amount to a rewrite of plume's barrier model; the run found render-then-sample **clean unpatched** (A1, A2, C1, D1) and needed no patch to `barriers()` for the patterns tested. (b) The patches cost about what was estimated for the same items (like for like: the first draft's P2 less barycentrics and uint8 60, P3 80, P4 60, P5 blend constant 12, P7 flags and zero attachments 20 = 232 lines against 211 measured; the figure "582 estimated against 211 measured" compares two different sets, because P1, P6 and P8 were not written). So the run saved about 20 lines of patch cost (0.006M), not "a fraction of the estimate". (c) It **confirmed the first draft's central worry**: the layout and stage state held in the resource races between threads (about 22,000 layer messages, ThreadSanitizer reports), and it dealt with it **by a convention**, the owner rule, which the test program obeyed in 4 lines and which gave 0 messages and 0 reports in an adversarial test. (d) It added knowledge: the zero-attachment pass needs three fixes, the descriptor-set constructor creates its own layout and pool (A1 of the review), lavapipe hides every invalid call.
- *What remains.* 2b means owning 7.5k lines we did not write, with no upstream, no tests of their own, and a synchronisation invariant (the owner rule) that spans our wrapper and plume's internals (plume reads the recorded layout in `barriers()` at plume_vulkan.cpp:2876 and in its copy and resolve paths at 3236, 3261, 3294, 3324-3325 and 3363-3364, so the rule is the only route that avoids patching plume) and that **lavapipe cannot test well**: one queue, every invalid call accepted, and the layer missed storage-image hazards inside one command buffer. Nothing was run on the RX 6700 XT or MoltenVK. The "known defects are small and measured" holds **for lavapipe, one queue and the patterns tested**; cross-queue transitions, ownership transfers, several queue families and Apple's driver are untested.

**What each option comes to**, after the run (costs 8.3; all `inference` except the 202 measured patch lines):
- **Option 1** is not rejected as impossible: a native `R32_UINT` shadow surface with a dummy attachment and the compare-and-swap loop reaches the shadow write without a patch, the blend constant has a dual-source route and the stage mask a usage rule. It costs about what 2b costs (0.67M to 0.83M against 0.62M to 0.86M), but the lines sit outside plume where the defects cannot be reached, and shadows stay unordered even where the device has interlock. Not recommended on cost and risk.
- **Option 2a collapses into 2b.** D3D12 is not needed (D4: "only if needed"), plume's Metal backend has no raster order groups, no fetch, no logic ops, takes only a metallib and asserts on fans and geometry, and upstream does not accept AI-assisted contributions (6.6). Keeping two backends honest costs about 330 lines for nothing.
- **2b and 2b-seeded are one family** (8.1): the same lines; they differ in whether plume's files stay a patch queue on a pristine copy or become ours. Option 3 seeded from plume's code is the same thing the moment most of plume is kept.
- **2b against 3:** 3 costs **0.55M more at the centres** (1.29M against 0.74M; 0.2M to 0.9M), and on the critical path **+0.09M to +0.27M with N2-21 split in three and `backend/` split by directory, +0.65M to +1.12M against +0.22M to +0.46M without the split**; 2b costs less to start and carries foreign code and the invariant above; 3 costs more and is code written to Azahar's pattern (that this pattern is "proven on this game's renderer" is D5's claim, `unverified`), all ours, with no prototype. RT64 uses plume in production (8.1); what that says about a PICA renderer is `unverified`.

**2b is the reversible choice.** The wrapper is option 3's interface, so plume can later be replaced underneath it. Reversing after N2-21p is built wastes the plume-specific lines: vendor and trim 200, the patches 202 + 245, and the plume-specific rework (the owner rule 60 to 150, the set recycler 100 to 150, combined-sampler lowering 100 to 200, dynamic-offset buffers 80 to 120): **987 to 1,267 lines, 0.28M to 0.36M, plus 0.05M to 0.10M of reading: 0.33M to 0.46M**, and the call sites inside the wrapper would be rewritten against the new layer (not estimated). So "permanent fork of 7.5k foreign lines" means: a fork that is permanent only as long as the wrapper is not rebuilt on something else, at a stated price. (The permanent no-upstream consequence stands either way.)

**Dissent and nuance (the planner's second pass).** The planner judged 2b defensible and the document honest that it is a close call. It added that if the decision were delegated anyway it would lean **3, narrowly**, because the remaining risk sits in the synchronisation invariant (the owner rule) that lavapipe cannot test well. I do not resolve that here: it is the reason I put the choice to Luis and do not delegate it.

### 9.2 What would change the recommendation

What is measured, on lavapipe only: the patterns of 6.9. What remains unmeasured: the RX 6700 XT and MoltenVK legs, a second queue, real driver compile times, the 7.5k lines plume exercises that the patterns did not (swapchain, several attachments, push constants, multisampling), and every figure marked `inference`. **The RX 6700 XT and MoltenVK legs of the plume run de-risk the shadow and synchronisation features of every option** (the same patterns run against our own layer would need the same legs); they are not the tie-breaker between 2b and 3, and I advise no further experiment before D4 is decided: the critical-path difference with the split (+0.09M to +0.27M) is about what one S experiment costs. **The 3,000-line conditions below are checked when the first slices are built.**

The rule behind the thresholds: option 3 is preferred when the measured gap falls below about **0.15M** (a third of one M package); at the centres it is 0.55M.

| Observation | Flips to |
|---|---|
| The 2b total, measured by building its slices (patches, wrapper, rework), exceeds **3,000 lines** (my range is 1,977 to 2,657): 3,000 lines cost 0.93M with reading, 0.12M below option 3's low end (1.05M) | **3** |
| Slice a of option 3 is built and the three slices are measured or extrapolated at **under 3,000 lines** (my floor is 3,680): option 3 would cost 0.86M, 0.12M above 2b's centre (0.74M) | **3** |
| The patched plume run under the layer on the RX 6700 XT or on MoltenVK, or with a second queue, shows a defect that the patches and the owner rule cannot repair (for example a cross-queue hazard that needs `barriers()` itself changed, or interlock or a mutable-format view failing on that driver) | **3** (or a larger 2b, if the fix is small: re-cost) |
| Luis decides he will not own 7.5k foreign lines even with the wrapper making them replaceable | **3** |
| A read of `plume_metal.cpp` end to end shows raster order groups, framebuffer fetch and run-time MSL at about 300 lines or fewer, **and** Luis decides a native Mac backend is wanted soon | **2a** |

What would *not* move it: the M2 probe run (it changes the Mac rows of section 4), the gas mode, the PC's missing `present_wait` (an OS-statistics question, D57). The thresholds sit outside the ranges' centres (3,000 lines against 1,977 to 2,657 and 3,680 to 5,320), not on the recommendation's boundary.

### 9.3 Fallbacks the PC and the M2 each need

| Device | Fallbacks the RHI must provide (all from section 4) |
|---|---|
| RX 6700 XT | `D32_SFLOAT_S8_UINT` with conversions for D24S8; no `present_wait`, so the display time comes from the OS (D57); no rasterization-order access (irrelevant while interlock is `yes`) |
| M2, MoltenVK | no `logicOp` (shader emulation, 4 of 16 in Azahar's SPIR-V, or fetch on a native backend); no geometry shaders, and barycentric inputs in the per-vertex form that Azahar's G2 route needs do not translate (skip the fix-up; the GS draws fix up on the CPU anyway); D32S8; fans rewritten as lists (they work, but with pass restarts) and 8-bit indices widened (they work, converted by a compute pass; Azahar blacklists them on the MoltenVK driver); `RGB8` as RGBA8; no graphics pipeline library; blend state baked into pipelines; primitive restart always on; FIFO and IMMEDIATE only; `present_wait` measures command-buffer completion |
| M2, KosmicKrisp | everything above, and **no interlock: the compare-and-swap loop for shadow writes**; `logicOp` yes; custom border colours not needed for Shadow2D |
| lavapipe | none of its own; it must run all of the above through the mask |

All M2 entries are `stand-in` (3.3, 3.4).

### 9.4 RHI design requirements the evidence imposes

Whatever the option; a list N2-21's planner can lift. Each carries its basis.
1. **Operation level, not Vulkan level.** The renderer-facing interface speaks Azahar's operations (5.8): surfaces, ring buffers, passes, pipelines keyed by state, bindings, submissions, readbacks. It must not expose `VkImageLayout`, `VkRenderPass`, `VkDescriptorSet`, `VkPipelineLayout` or Vulkan format enums, and with option 2b it must not expose plume's types. **Boundary caveat:** this boundary is for the interface the renderer sees. N2-21's raw handles (R4: the instance, device, queue and the recording `VkCommandBuffer` for Dear ImGui), the compositor (N2b-05), the swapchain and present thread (N2b-06) and ImGui stay Vulkan, so a later Metal backend also needs a Metal compositor, presenter and ImGui path beyond the lines of 7.2. "Operation-level" is worth keeping; "shaped so that Metal fits" is not claimed beyond it. Basis: 3.3, `metal.md` 2.3.
2. **A capability structure filled from device queries** (never constants: plume's `triangleFan` is one), **with a mask** (`MH4N_RHI_MASK` or a command-line option) applied after the query, for these bits: D24S8, interlock, 8-bit indices, custom border colours, fans, logic ops, dynamic rendering, pipeline library, extended dynamic state 3 blend bits, BC7, timeline semaphores, present wait, and "force staging copies". Basis: section 4; it cannot force barycentrics on.
3. **A format-support query and a chooser** (X1): attachment, sampled, storage and blit per format; D24S8 else D32S8, RGB8 always RGBA8, the narrow colour formats in their `R`-first variants on MoltenVK, BC7 gated on `textureCompressionBC` and a run-time check. Azahar's `CreateFormatTable` is the pattern (`azahar@5f3306d renderer_vulkan/vk_instance.cpp:212-312`). Basis: G3, G9, G11; plume's unchecked `createTexture` reaches the layer as errors (6.9).
4. **Shadow support as a first-class path:** either images created with the mutable-format flag and the format list (`R8G8B8A8_UNORM`, `R32_UINT`) viewed through an `R32_UINT` storage view, or a native `R32_UINT` surface (option 1's route); a zero-attachment pass, which needs three fixes in plume (framebuffer size, viewport and scissor count, render area; 6.9); the interlock capability and the compare-and-swap fallback, both testable through the mask. `metal.md` 3.2 asks for storage usage only on surfaces that become shadow sources, so Apple GPUs can compress the rest. Basis: 5.3, 6.9.
5. **One layout policy, hidden, and an owner rule for who records.** Azahar keeps every *surface* image in `GENERAL` with explicit barriers (the offscreen frame and the swapchain image use transfer layouts and `PRESENT_SRC`); its policy has no layout state to race. That it is "proven on this game's renderer" is D5's claim, `unverified` here; Azahar's behaviour on MH4U would verify it. With plume underneath, the layout and stage state held in the resource is read while recording not only in `barriers()` (plume_vulkan.cpp:2876) but in the copy and resolve paths (3236, 3261, 3294, 3324-3325, 3363-3364), so it cannot be made stateless without patching plume: **the owner rule is the only route that avoids a patch**, stated as a checkable rule: (1) every image has one owning thread, and only that thread records barriers, copies or resolves on it; (2) images are handed between threads only by a timeline value with its own wait stage, and stay in `GENERAL`; (3) on each queue, lists are recorded in submission order; (4) debug builds assert all of this. It is an invariant that spans our wrapper and plume's internals, and lavapipe cannot test it well (one queue, every invalid call accepted, the layer missed storage-image hazards inside one command buffer); the harness tested a mutex-and-order form (6.9, D3), not the owning-thread form. Its bandwidth cost on Apple GPUs is unmeasured. The interface must not leak layouts. Basis: 5.6, 6.4, 6.9.
6. **The feedback copy as an operation** (X6): end the pass, copy the target (the whole image in Azahar), resume with `LOAD`. Basis: 5.6, `FST p.6`.
7. **Synchronisation as N2-21 specifies, with the defects of 6.9 avoided:** one submit lock per queue; every submission returns a timeline value; a wait carries its own stage mask; a binary-semaphore and fence fallback where `timelineSemaphore` is absent; deferred destruction by tick. Basis: 5.8, R2.
8. **Pipelines:** a `VkPipelineCache` blob in and out of the device, a render-pass cache keyed by formats (plume made 44 render passes for 42 pipelines), creation callable from worker threads, an ubershader while a pipeline compiles; the pipeline key includes the blend state (MoltenVK cannot make it dynamic, X8). The graphics pipeline library is optional. Basis: G10, 6.9.
9. **Presentation:** `PresentWaitAvailable()` *and* what it measures (`PresentWaitMeasuresDisplay()`: false on MoltenVK); `VK_GOOGLE_display_timing` as a source where listed; no MAILBOX on Apple; a headless surface for tests. Basis: R3, 3.3.
10. **Draw features:** a fragment depth output (X2); vertex attribute formats with the integer fallback (X3); strides from `minVertexInputBindingStrideAlignment` when the portability subset is present (1 on Apple8, 4 below Apple5); 12 vertex bindings plus a fixed binding; primitive restart treated as always on on Apple (X7). Basis: Azahar's code, 3.3.
11. **Fans and 8-bit indices:** read from the capability, but on Apple devices rewrite fans as lists and widen 8-bit indices anyway. Basis: 3.3, 6.1.
12. **Device selection by capability, not by index:** prefer a discrete device; on a Mac two `Apple M2` devices may answer and the choice must be logged and overridable; request portability enumeration. Basis: 3.4.
13. **Enable only what the capabilities need** (plume turns on ray tracing wherever offered). Basis: 6.2.
14. **Loading:** volk, one volk and one VMA implementation in `native/third_party/rhi/`, a clear message and a null backend when there is no driver (R6). Basis: 6.1.
15. **Readback:** depth and stencil in two aspects (or Azahar's compute readback), the Morton write-back as a compute pass of ours, `ReadImage` in the image's own format. Basis: 5.8, R5.
16. **Validation in CI:** run every RHI test under `VK_LAYER_KHRONOS_validation` (installs with apt as `vulkan-validationlayers` 1.3.275 on the cloud image) with synchronization validation on, and a ThreadSanitizer job for the multi-thread tests, because **lavapipe accepts every invalid call** (wrong-format views and zero-size framebuffers returned handles) and the layer's synchronization validation did not flag storage-image hazards between draws inside one command buffer, so silence on the no-interlock shadow patterns is weak evidence. The layer needs a messenger or stdout capture: plume registers none. The harness that does this for plume is in [`rhi-spike-evidence/`](rhi-spike-evidence/README.md). Basis: 6.9.

### 9.5 What changes in the specs once D4 is decided

List only; the specs are not edited here.
- **Split N2-21** (the orchestrator's decision; names are enough): **N2-21a** (1.2 to 1.7 M: device, queues, memory, buffers, one submit path with the lock, SPIR-V pipelines, a minimal zero-attachment-capable pass, buffer readback, the test helper; unblocks N2-06 and N2-22), **N2-21b** (render targets, images, passes, barriers, the format chooser and the mask, descriptor recycling; unblocks N2-13, N2-08 and N2b-05; runs in parallel with N2-06), **N2-21c** (timeline values and stage masks, the pipeline cache API, readback to the PICA layouts, `ReadImage`, timestamps; runs in parallel with N2-22). **The plan's rule that parallel packages share no files needs `native/src/gpu/backend/` split by directory** (`core/`, `targets/`, `sync/`, `readback/`, with N2-13 owning `state/` and N2-12 owning `shadow/`; 8.3), and the public interface header frozen by slice a. Delays with and without the split: option 3 +0.09M to +0.27M with it (+0.09M to +0.52M with slices but without the directory split), +0.65M to +1.12M without; option 2b about 0 and +0.22M to +0.46M (8.3). N2-05 and N2-07 are released by D4 itself.
- **N2-21:** "As D4 decided: the RHI (or plume)" names the RHI; with 2b a patch WP **N2-21p** (vendor, trim, the numbered patches, the layer-run harness) precedes or runs beside N2-21a, and `native/third_party/rhi/` holds plume's Vulkan files, volk and VMA; with 3 it holds volk and VMA only. "The fallbacks `rhi-spike.md` lists" becomes 9.3 and 9.4. *Done when* gains the capability mask (every fallback run on lavapipe), the format query, `PresentWaitMeasuresDisplay()`, the Windows expectation that `PresentWaitAvailable()` is false on the RX 6700 XT, "no Vulkan layout, render-pass or descriptor-set type (and with 2b no plume type) in the renderer-facing header", and "the RHI tests are clean under the validation layer".
- **N2-05:** "which backends lack fans and 8-bit indices" has its answer in 3.3 and 4: none of the three Vulkan devices lacks them (MoltenVK and KosmicKrisp emulate fans, MoltenVK converts 8-bit indices by a compute pass); native Metal lacks both. The rule becomes: rewrite on the capability bit, and always on Apple. Add strides from the device's alignment and the attribute formats of X3.
- **N2-07:** shadow writes with interlock else compare-and-swap, and the lookup as a texel fetch with a manual compare and "out of range is lit"; logic ops: hardware where `logicOp` is present, shader emulation of **all 16** elsewhere; min/max through X6; the shader writes depth (X2); the fix-up route per device; the shadow write is GLSL-only in Azahar, so D5's option 2 needs both generators; with plume underneath, GLSL written for combined samplers needs lowering; pipeline counts on MoltenVK; gas open.
- **N2-12:** the two shadow paths and their test through the mask; ShadowCube is unfinished in Azahar's Vulkan renderer and not seen in those scenes; KosmicKrisp is compare-and-swap only.
- **N2-16:** the cache blob API; no graphics pipeline library on the M2; Azahar skips a draw whose pipeline is not ready when async compilation is on (default off), which the ubershader replaces.
- **N2b-05:** the compositor waits on a timeline value with the fragment-shader stage (a wait at the default stage reproduced the hazard, 6.9); images are sampled in `GENERAL`; under 2b it must obey the owner rule when it records barriers on the renderer's images.
- **N2b-06:** `PresentWaitAvailable()` and what it measures; MAILBOX absent on Apple; the headless surface for tests; D57's second source on the RX 6700 XT; `VK_GOOGLE_display_timing` on MoltenVK.
- **N2-22** (the renderer thread) **and N2b-06** (the present thread): the owner rule of 9.4 item 5 constrains D56's thread design (renderer thread, present thread, compositor): an image is owned by one thread and handed over by a timeline value with its own wait stage; N2-22's queue wrapper and N2b-06's presenter assert it in debug builds.

### 9.6 The decision requested of Luis

**The question, in one sentence: do you want to build on plume's Vulkan code and own a replaceable fork of it (`2b`, about 0.55M tokens cheaper), or write the Vulkan layer ourselves from Azahar's pattern (`3`)? D4 should be decided by Luis and not delegated:** it turns on a maintenance preference and a schedule trade, and his usual delegation criterion (stability, compatibility, the fewest errors for the average user) does not settle it, because both options would pass the same oracles and the remaining difference is risk in a synchronisation invariant that lavapipe cannot test well. The planner's second pass said that if the decision were delegated anyway it would lean 3, narrowly (9.1).

**D4, in one word:** **`2b`** (vendor plume's Vulkan backend, trimmed, with the measured patches and a wrapper of ours; the recommendation), **`3`** (our own RHI over Vulkan, Azahar as reference), **`2a`** (plume extended, all backends kept) or **`1`** (plume as it is, with workarounds; not recommended). What each sets in motion: `2b` adds N2-21p and shrinks N2-21 (0.62M to 0.86M in all; about 0 on the critical path if N2-21 is split in three and `backend/` split by directory, +0.22M to +0.46M if not); `3` grows N2-21 to L (1.05M to 1.52M; +0.09M to +0.27M on the critical path with the split, +0.65M to +1.12M without); `2a` is `2b` plus about 0.09M; `1` adds workaround work in N2-12 and N2-13. Every answer except `1` releases N2-05 and N2-07 at once.

**Two consequences he should see before answering.** *The permanent fork (2a, 2b):* plume's `CONTRIBUTING.md` forbids AI-assisted contributions and, in the same line, "Usage of AI tools to communicate with the maintainers of this project is also disallowed", so agents never file an issue, a pull request or a message to the plume project; vendoring the MIT code is unaffected; every fix stays ours and every upstream fix must be cherry-picked by us (understanding plume's code is a recurring cost, 8.3). **Under 2b the fork is reversible:** the wrapper is option 3's interface, so plume can be replaced underneath it; reversing after N2-21p is built wastes about 0.33M to 0.46M (9.1). *The schedule:* N2-21 heads the longest chain of the plan, so what is added to it is added to the date of the first native frame unless N2-21 is split (8.3).

**Two product calls that are his, not D4's and not an agent's.** (a) **Which Mac driver to target, and whether a possibly visible shadow difference on a KosmicKrisp-only Mac is acceptable.** MoltenVK was deprecated by Khronos on `main` on 2026-10-13 (no release since 1.4.2) and Khronos recommends KosmicKrisp, which needs macOS 26 and has no interlock, so there the shadow write is the unordered compare-and-swap loop; whether that is visible is open question 10. This decides the supported macOS versions and how soon `metal.md`'s trigger could fire. (b) **Whether native Metal is a goal at all, and roughly when.** It decides how much of "shaped so that a Metal backend fits" is worth paying for now (9.4, item 1) and whether plume's Metal backend (2a) is worth a read. Neither needs an answer before D4; both should be answered before N2-21 is specified in detail.

## 10. Questions only Luis's Mac run or more captured scenes can settle

Each with the command or capture that settles it and the part of the recommendation it would move.

| # | Question | What settles it | What it moves |
|---|---|---|---|
| 1 | Which Vulkan driver or drivers does Luis's Mac return (MoltenVK, KosmicKrisp, both), with which version and library path, and is his MoltenVK a private-API build (`logicOp`)? | [3.4](#34-the-m2-pending-luiss-run): `vkprobe-mac.json`, `loader.txt`, the `--loader` runs | the Mac columns of section 4; whether N1 is missing on the Mac; not the RHI choice |
| 2 | Does his line set equal the stand-in's (3.3 table)? Which lines differ? | the table in 3.4 | the Mac columns of section 4 |
| 3 | Is `VK_EXT_fragment_shader_interlock` listed on his MoltenVK, and does the shadow pass render correctly through it, and what does it cost against the compare-and-swap loop? | the interlock line of his run; then the shadow scenes replayed (N2-12) on both paths, with the mask forcing the loop | how much weight "shaped so that a Metal backend fits" carries; `metal.md`'s Picture trigger (7.3) |
| 4 | Does the patched plume stay clean under the validation layer on the RX 6700 XT and on MoltenVK, with a second queue, and does the owner rule hold for the compositor's recording? (Lavapipe is done: [6.9](#69-validation-layer-run-of-plume); the layer installs with apt.) | the harness of [`rhi-spike-evidence/`](rhi-spike-evidence/README.md) (695 lines, seven patches) on the PC and the Mac, with `VK_LAYER_KHRONOS_validation`, and a two-queue test where the device has one | **9.2: de-risks the shadow and sync features of every option; flips 2b to 3 only if it finds a defect the patches and the owner rule cannot repair** |
| 5 | What are the RX 6700 XT's extension list (198?), limits, queue families (is there a transfer-only family?), memory heaps and the 18 set bits of extended dynamic state 3? Are `VK_GOOGLE_display_timing` and `VK_EXT_present_timing` absent? | the missing `vulkaninfo --json` file for the PC (3.2), read directly (it holds the extension list, limits, queue families, memory heaps, the 18 set extended-dynamic-state-3 bits and the display and present-timing extensions); run through `mh4n_vkprobe --from-json` it would only re-print the 24 probe lines | the `unverified` statements of 3.2 and D57; the BC7 upload thread's queue (G11) |
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
| 19 | Does KosmicKrisp's `MESA_KK_EXPERIMENTAL=custom_border` work on an M2? | 3.4, step 4 settles whether the extension is *listed*; whether it *works* needs a draw | nothing for Shadow2D; ordinary textures' emulated border |

## 11. Corrections to earlier documents found while checking

Not edited here; the orchestrator or the owner of each file decides. Each as: file, line, what it says, what the evidence shows, source.

**`docs/research/metal.md`**

| Line | Says | Evidence | Source |
|---|---|---|---|
| 15, 68 (and D4 row) | MoltenVK has "No triangle fans"; the draw falls back to vertex shading on the CPU | MoltenVK 1.4.1 and 1.4.2 set `triangleFans = true` (hard-coded) and encode fans as an indexed indirect triangle list (since 1.2.5); reports 51106 and 52371 both say `triangleFans true`; Metal has no fan type (that part is right). Azahar reads the flag (`vk_instance.cpp:552-557`), so on 1.4.x its accelerated fan path should not be skipped (`inference`: both sources read, not run); plume's own comment says MoltenVK "supports triangle fans but does so via compute shaders" | `MoltenVK@v1.4.2 MVKDevice.mm:610` (v1.4.1: `:603`), `MVKCmdDraw.mm:182-186`, `Docs/Whats_New.md:599`; reports 51106, 52371; `plume@0ce184b plume_vulkan.cpp:4173-4180` |
| 21, 79 | MoltenVK compiles with Metal's fast math "by default" (`MVK_CONFIG_FAST_MATH_ENABLED=1`) | the default is `2` ("on demand": fast math unless a shader's `VK_KHR_shader_float_controls2` or `SignedZeroInfNanPreserve` limits it); identical at 1.4.1 and 1.4.2. Whether that protects Azahar's `isnan()` check depends on whether its SPIR-V carries those controls (`unverified`) | `MoltenVK@v1.4.2 Docs/MoltenVK_Configuration_Parameters.md:201-210`, `MVKEnvironment.h:183-184` |
| 74 | "Metal wants vertex strides in multiples of 4 (my understanding)" | "multiples of 4" is right up to Apple4: 4 bytes for Apple2, Apple3 and Apple4, 1 byte from Apple5 (the M2, Apple8, is 1); MoltenVK sets `vertexStrideAlignment` to 1 from Apple5; report 52371 says 1 for the M2 | `FST p.7`; `MoltenVK@v1.4.2 MVKDevice.mm:2433, 2568`; report 52371 |
| 13, 86 | "lit draws skip the per-triangle quaternion fix-up"; "On MoltenVK every lit draw runs without the flip" | only lit draws shaded on the GPU by the translated vertex shader. **A draw shaded on the CPU gets the flip on the CPU on every host**: every PICA-geometry-shader draw (39 to 90 percent of the draws in the census scenes) and every fan draw on a host without fans | `azahar@5f3306d rasterizer_accelerated.cpp:85-90`, `pica/pica_core.cpp:1052-1092` |
| 276 | KosmicKrisp "needs macOS 15" | "implemented on top of Metal 4, which requires macOS 26 and up" | `Mesa@f3f63ef docs/drivers/kosmickrisp.rst:4-8` |
| 50, 303 (and the trigger 3.3) | no mention that MoltenVK is deprecated | `main`'s README carries a deprecation notice since 2026-10-13: community best effort, Khronos recommends KosmicKrisp on Apple Silicon, MoltenVK stays for Intel Macs and iOS; no release after 1.4.2 | `MoltenVK@main README.md:6-14`, commit `0aa76ad` |
| 34, 117, 276 | KosmicKrisp: "custom border colours are not exposed yet"; "Trying it needs no code" | exposed by default: `logicOp` yes, **no interlock**, no barycentrics, no geometry; custom border colours exist behind `MESA_KK_EXPERIMENTAL=custom_border`; `maxUniformBufferRange` 65,536 | report 52372; `Mesa@f3f63ef kk_physical_device.c:174-177, 240` |
| 72 | Shadow row: interlock "supported through Metal raster order groups ... Expected to work" | right for MoltenVK. The row omits that Azahar blacklists interlock on NVIDIA's proprietary driver and that only the GLSL generator emits the shadow write | `azahar@5f3306d renderer_vulkan/vk_instance.cpp:471-473`, `shader/generator/pica_fs_config.h:407-414` |
| 165-166; no min/max row in 1.2 | min/max blending is something only Metal framebuffer fetch improves | in Azahar's Vulkan renderer min/max is emulated in the shader on **every** host except when both factors are One (Vulkan's `MIN`/`MAX` ignore factors), at the price of a whole-image copy and a pass break per draw; Metal has native `min`/`max` | `azahar@5f3306d shader/generator/pica_fs_config.cpp:44-56`, `vk_rasterizer.cpp:725-732`; `FST`/Apple `MTLBlendOperation` |
| 281 | "On Apple GPUs, MoltenVK can turn input attachments into framebuffer reads (my understanding)" | not found in 1.4.2: `use_framebuffer_fetch_subpasses` is never set, `VK_EXT_rasterization_order_attachment_access` is absent | `MoltenVK@v1.4.2 MVKPipeline.mm:2888` (serialiser only), `MVKExtensions.def`; `MVKImage.mm:957-959` ("MoltenVK does not support [framebuffer fetch] yet") |
| 70 | 16-bit colour: "Not checked whether MoltenVK offers them" | now checked: `R5G6B5`, `R5G5B5A1`, `A1R5G5B5`, `R4G4B4A4` are colour attachments with blending; the `B5G6R5`, `B5G5R5A1`, `B4G4R4A4` variants are sampled only; `R8G8B8_UNORM` has no image features | report 52371 |
| 95 | gas: "work in the shader generators and helps every renderer" | may also need new render-target access (framebuffer fetch, interlock or a copy per draw) if mode 1 reads the destination colour | 5.5; 3dbrew `GPUREG_GAS_ACCMAX_FEEDBACK` |
| 187 (and 37) | Vulkan backend 11,413 lines "non-blank, non-comment" | 11,366 with `/* */` blocks also stripped; 11,413 is nearly reproduced (11,407, and 9 of 10 groups exactly) only if block comments are counted | 5.9 |

**`docs/research/vkprobe.md` and `vkprobe-results.md`**

| File:line | Says | Evidence | Source |
|---|---|---|---|
| `vkprobe.md`:61 | on a Mac "expect `no` for triangle fans" | for MoltenVK 1.4.x expect `yes` | as above |
| `vkprobe-results.md`:41-43 | "Expected ... `no` for triangle fans" | `yes` | as above |
| `vkprobe.md`:73 | "The site's own JSON export has a layout of its own, which `--from-json` does not read." | gpuinfo's `api/v3/getprofile.php` export **is** the Vulkan Profiles layout and `--from-json` reads it; three export gaps: no device name or type (`(unnamed)`), `D24_UNORM_S8_UINT` `unknown`, and 19 of 130 extensions missing; the probe prints `source: vulkaninfo` for such a file | `report 52371`/`52372` through the probe (3.3) |
| `vkprobe.md` table (barycentrics; present id/wait) | a `yes` is "extension and bit" | on MoltenVK `yes` for barycentrics means *listed*: the `gl_BaryCoord` built-ins translate, the per-vertex inputs of Azahar's G2 route do not; `yes` for present id/wait means the device's answer, with the wait released at command-buffer completion | 3.3 |
| `vkprobe-results.md`:9-11, 14, 22-24 | the run "checked against the SDK's `vulkaninfo --json`"; "198 device extensions"; driver file 2.0.353; no `VK_GOOGLE_display_timing` or `VK_EXT_present_timing` | the `vulkaninfo` file is not in the repository, and `vkprobe-pc.json` has only the probe's 24 lines: not checkable here | 3.2 |
| `vkprobe-results.md`:9 | "Run on 2026-10-21" | the commit `ba1ba93` is dated 2026-10-21 19:51 -0700, that is 2026-10-22 02:51 UTC; no date in the JSON | `git log` |

**`docs/native/DECISIONS.md`**

| Row | Says | Evidence |
|---|---|---|
| D4 | MoltenVK has "no triangle fans" among its known gaps | MoltenVK 1.4.x has them (emulated); remove from the list |
| D4 | plume "no interlock, raster order groups or framebuffer fetch exposed; unstable API" | **confirmed** (6.7). To add: no image `MUTABLE_FORMAT`, a zero-attachment pass that needs three fixes (framebuffer size, viewport and scissor count, render area), one `VkRenderPass` per pipeline and no `VkPipelineCache`, a single wait stage and a one-element stage mask, layout state held in the resource (races between threads); the fixed `COLOR_ATTACHMENT_OPTIMAL` layouts are not a blocker of the shadow write, no blend-constant setter, binary semaphores only, no format query, no D24S8 or narrow colour formats, an Xlib-only Linux surface path (without the SDL option), a metallib-only Metal shader path (6.1) |
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
| Whether any lavapipe or plume path *works* end to end on a real driver (the probe reports capability only; the plume runs were on lavapipe: first without the layer, then under it, 6.9, with no swapchain and one queue) | the RX 6700 XT and MoltenVK legs (question 4) |
| plume's Metal and D3D12 backends: read by grep and a few spots only | a full read (question 17) |
| plume's behaviour on MoltenVK, on AMD's Windows driver, with a Vulkan 1.1-only driver | runs on those |
| The stage-mask defect across two native queues (measured on one queue in 6.9) | a device with two queues, under the layer |
| Unleashed Recompiled as a plume user | the project's page or its source |
| Azahar: nothing built or run; `vk_shader_disk_cache.cpp`, `renderer_vulkan.cpp`, `glsl_fs_shader_gen.cpp` and `rasterizer_cache.h` read in part; `custom_textures/`, `renderer_opengl/`, the CPU shader interpreter and `texture_decode.cpp` not read | reading them; N2-14 and N2-06 do |
| Azahar's "proven on MH4U" (D5) | not checkable from the code |
| Whether the Vulkan ShadowCube path in Azahar really fails; whether the compare-and-swap loop differs from the interlock path; whether `GENERAL` costs bandwidth; what a driver does with a BC image when `textureCompressionBC` is off | captures with a ShadowCube; both paths on one capture; M2 measurements; a validation run |
| The date of the licence switch in D6 (2026-10-03) | Azahar's history; our checkout has one commit |
| VMA's behaviour for `AUTO_PREFER_DEVICE`; the Vulkan rule that `R32_UINT` supports atomic storage (`memory`) | the VMA source and the Vulkan spec text |
| The per-fragment formula of gas modes 1 and 7 | N2-10; hardware tests |
| KosmicKrisp: the semantics of its present wait, whether `custom_border` works, how local-read compiles, the source lines of its limits, the manifest's installed name | a run on an M2 with the SDK |
| Apple documents: "Access to pre-raster per-vertex values" (Apple10) beyond its table row; pages 1 to 18 of the Feature Set Tables are now all read (pages 9 to 18 for this revision), but the Metal API names of the 16-bit formats beyond the table were not; whether an M2 also answers the `mac2` family; the Khronos conformance page; which three usage options Apple's lossless-compression page excludes (the extraction lost them); "no public logic-op API" beyond the pages searched | reading those pages; Apple's documentation |
| MoltenVK issue #2059, the Dolphin, Cemu and Phoronix pages | not needed for D4 |
| The `VK_LOADER_DEBUG` and `VK_DRIVER_FILES` variable names and the Apple Feature Set Tables URL, written from memory | the loader's documentation; Apple's developer site |
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
| gpuinfo reports | 52371 (`profile.json` sha256 `109720903309a10128de4d85ce62a71d771c389c436e14687d44ee4d3e1e759f`), 52372 (`f77ab407f6d796eddcd110fdcb141f230334d8fa0021f3d28b6f041fcf99fa92`) | `curl -o profile.json "https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=52371"`; the page: `https://vulkan.gpuinfo.org/displayreport.php?id=52371`; the probe: `mh4n_vkprobe --from-json profile.json` (add the gaps of 3.3 by hand if wanted). The converter used for 3.3 follows three rules and nothing else: (1) `deviceName`, `deviceType` (as `VK_PHYSICAL_DEVICE_TYPE_<TYPE>`) and `deviceID` in `VkPhysicalDeviceProperties` are filled from the report page's property table; (2) every row of the page's device-extension table ("Extension / Version"; not the instance-extension table) that the profile lacks is added as an extension with that spec version; (3) every row of the page's first (optimal-tiling) format table whose cells are all empty is added to `formats` with empty `optimalTilingFeatures`, which the reader reads as `no`. The site has no database download (`about.php`); the listing endpoints are internal |
| Apple Metal Feature Set Tables | 18-page PDF dated 2026-05-21, sha256 `9f31df15dd6827545702c5a0845f6e36e1889878cd0e534123bd70211e5c00a8` | the Metal page on developer.apple.com (pages 9 to 18: pixel-format tables, `pdftotext -layout -f 9 -l 18`) (the URL `https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf` is from `memory`); read with `pdftotext -layout` |
| Metal Shading Language Specification 4.1 | 383 pages, sha256 `41538b30d2f1140a5b2a0c84ce0a9f7b67bf0c707e224cfea0bfe5a44aa26cf5` | `https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf` (retrieved 2026-10-23) |
| Apple documentation pages | `mtlprimitivetype`, `mtlindextype`, `mtlsamplerbordercolor`, `mtlblendoperation`, `mtlblendfactor`, `mtlrenderpipelinecolorattachmentdescriptor`, `mtlrenderpipelinedescriptor`, `mtlgpufamily`, `mtlpixelformat/depth24unorm_stencil8`, `mtldevice/isdepth24stencil8pixelformatsupported`, `mtldevice/supportsbctexturecompression`, `optimizing-texture-data` | `https://developer.apple.com/tutorials/data/documentation/metal/<page>.json` (retrieved 2026-10-23) |
| 3dbrew | `GPU/Internal_Registers`, last edited 2026-10-18 | `https://www.3dbrew.org/wiki/GPU/Internal_Registers` (retrieved 2026-10-23) |
| Validation layer and the plume run | `VK_LAYER_KHRONOS_validation` 1.3.275 (apt `vulkan-validationlayers 1.3.275.0-1`), `libvulkan1 1.3.275.0-1build1`, clang 18.1.3, glslang 15.1.0 | `apt install vulkan-validationlayers`; the harness (`test/main.cpp`, 695 lines), the seven patches, the layer settings and `results-summary.txt` are in [`rhi-spike-evidence/`](rhi-spike-evidence/README.md), with `SHA256SUMS` (23 files; itself `afd62dcc478c5af5b7adb9dbb46330fa5a14d3e76d4a407c38901feb8507fc52`); the 184 log files, the call traces and the ThreadSanitizer build are in the session scratch folder and are re-created by the README's commands |
| lavapipe | Mesa 25.2.8-0ubuntu0.24.04.4, LLVM 20.1.2 | Ubuntu 24.04 packages `mesa-vulkan-drivers`, `vulkan-tools`; `build/native/bin/mh4n_vkprobe`, `vulkaninfo --json` |
| The probe | built from this repository at `69dd2c2` | see [`vkprobe.md`](vkprobe.md) |
| The RX 6700 XT report | [`vkprobe-pc.json`](vkprobe-pc.json), commit `ba1ba93` | in this repository; `mh4n_vkprobe --from-json docs/research/vkprobe-pc.json` |

Cross-references: [D4 and the decisions](../native/DECISIONS.md), [N2a work packages](../native/wp/N2a.md), [N2b work packages](../native/wp/N2b.md), [`metal.md`](metal.md), [`vkprobe.md`](vkprobe.md), [`vkprobe-results.md`](vkprobe-results.md).
