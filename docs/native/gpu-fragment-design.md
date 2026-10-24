# GPU fragment pipeline: the design for D5

## 0. Header

- **Status:** draft for review (N2-07). **D5 is `Open`; the maintainer decides it.** This document does not decide it and nothing in it is settled. It lays out the three options D5 lists ([3](#3-the-three-options-of-d5)), the evidence for and against each, what each does to the work packages that wait on it, and a **recommendation** (marked as one, [3.3](#33-recommendation)). Sections 4 and 5 then design the recommended option (option 1) in enough detail for the implementing packages to start from it, and [6](#6-what-changes-under-options-2-and-3) says what changes if the maintainer picks option 2 or 3. Wherever the text says "the design", read "the design if D5 goes to option 1".
- **Date:** 2026-10-24. **Written by:** the N2-07 worker session, on `wp/N2-07-fragment-design`, base commit `51faacc`. Design only: nothing is built, copied or run. Azahar was read (read-only) in `~/azahar` at `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6`.
- **No game data.** Counts, register names, enumerator names and Azahar's file and function names only. The numbers come from `docs/research/pica-census.md`, which holds counts and hashes only.

**Labels.** Every claim carries its basis, the way [`rhi-spike.md`](../research/rhi-spike.md) does.

| Label | Meaning |
|---|---|
| `census` | a number printed in `docs/research/pica-census.md`, written by `tools/native/pica_census.py` from the seven MGPU captures (quoted exactly) |
| `source: <ref>` | read in the named file in this work (2026-10-24). `azahar@5f3306d path` is under `src/video_core/` |
| `derived` | arithmetic or set operations on `census` or `source` numbers, done here; the method is stated |
| `inference` | concluded from what was read, not run |
| `estimate` | an `inference` that rests on stated assumptions; the assumptions are listed next to it and are not measured |
| `memory` | what the writer recalls about a library, a spec or a chip, not read here |
| `unverified` | could not be checked from the repository; the text says what would check it |

**Reference forms.** `rhi-spike 5.3` is section 5.3 of `docs/research/rhi-spike.md`. `N2a N2-26` is the work package in `docs/native/wp/N2a.md`. The seven captures are named as the census names them: G1 the title, G2 character creation, G3 one Arena fight, G4 one gas cutscene (3, 3, 10 and 10 frames), and G9a the village during the caravan leader's talk, G9b the Arena's equipment screen, G9c the quest's first frames (3 frames each; `docs/native/golden.md`, `docs/native/wp/N0.md` N0-35). In all, 9,645 draws in 35 frames (`derived`: the sums of the census's capture table).

## 1. Summary

**What the game asks of the fragment stage** (`census`, seven captures, 9,645 draws). Every draw blends (`output_merger`: "blend" in all seven), with the equations Add/Add only and colour writes on. The features in use: lighting in four of seven (297 to 1,290 draws where present; 1 to 3 lights; only Config0 and Config1; five LUT sets, one of them none), fog in three (800, 390 and 336 draws), one shadow pass in three (50, 20 and 24 draws, 1.37 to 1.53 percent of the draws) with a Shadow2D texture and a white border colour, gas in one (G4, 20 draws), procedural textures in all seven (30.58 to 98.30 percent of the draws), and a geometry shader in Point mode in all seven (38.99 to 98.30 percent). Not seen in any of the seven: logic ops, min/max blending, cube and projection textures (ShadowCube included), triangle fans, 8-bit indices ([2.1](#21-what-the-captures-use)).

**Recommendation (not a decision; D5 stays `Open`).** Option 1 of D5, built **ubershader first**: our own generator, one module per feature, copying Azahar's state key (`FSConfig`) and generator semantics with provenance (D6); one GLSL generator, compiled with glslang (already D27's tool) instead of porting Azahar's two generators; and an ubershader that is **the same generator run in a second mode** (the key read from a per-draw configuration block instead of baked into the code), so the two cannot drift apart. The reasons, short ([3.3](#33-recommendation) has them with the numbers): (a) the ubershader is needed whichever option wins, because ROADMAP principle 5 asks for no visible stutter and Azahar has none; (b) the shadow write and min/max blending exist only in Azahar's GLSL generator, and shadows are in three of the seven captures, so option 2 would need both generators plus an ubershader of our own; (c) option 3 stakes the 6 ms GPU budget of D19 on a per-pixel cost nobody has measured, and the `estimate` of [4.4.3](#443-cost-per-pixel-at-34401440) says that cost is large on the Apple M2; (d) building the ubershader first makes option 3 a measurable checkpoint of option 1 instead of a separate bet: if the ubershader proves cheap enough, the specialised path can be dropped later.

**The design in one paragraph.** A draw's fragment state is read from the command processor's `PicaState` into a `FragmentKey` (Azahar's requested `FSConfig`, versioned by a struct hash) and a `FragmentUniforms` block (Azahar's `FSUniformData`, 1,328 bytes). Fixed-function state (blend, depth, stencil, cull, formats, vertex layout) is not in the fragment key; it goes into the pipeline key because the vendored plume bakes it on every device (D4, `rhi-spike` X8). Stencil reference, stencil masks and the blend constant are dynamic. The pipeline key is `(vertex stage id, fragment key id or ubershader, fixed-function state, pass kind)`. Nothing goes into Vulkan specialisation constants except the device-profile switches of the ubershader module. Each of the five feature directories contributes key fields, code (in both modes), uniforms and requirements through one small interface ([4.8](#48-the-feature-interface)). The paths the RHI lacks are chosen by capability bits of N2-26 ([5](#5-paths-the-rhi-lacks-what-d4-fixed)).

**Numbers this document could not produce** (marked `unverified`; [7](#7-unverified-and-the-census-extension) says how to get them): how many unique pipeline keys the captures have; the ubershader's measured cost; how many lit draws use the hardware vertex shader (the only ones that need a GPU quaternion fix-up); the blend factors in use. **Points for the planner** ([8](#8-for-the-planner)), the main three: `geometry_shader` is a value, not a maskable bit, in N2-26, so the M2's "no fix-up" path cannot be run on lavapipe through the mask; N2-16's "never skipped" cannot hold for a fixed-function state whose ubershader pipeline was never built; and two sentences of D5's evidence differ from the census (826 against 800 fog draws in G3; "one procedural-texture configuration" is not in the census).

## 2. The evidence

### 2.1 What the captures use

All `census` unless marked. Cells are draws, with the share of the capture's draws where the census gives one.

| Feature | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| Draws (frames) | 155 (3) | 794 (3) | 3,650 (10) | 1,390 (10) | 1,572 (3) | 1,025 (3) | 1,059 (3) |
| Geometry shader in use, Point mode | 140 (90.32%) | 668 (84.13%) | 2,560 (70.14%) | 542 (38.99%) | 1,131 (71.95%) | 1,007 (98.24%) | 1,041 (98.30%) |
| Procedural texture on | 140 (90.32%) | 668 (84.13%) | 2,560 (70.14%) | 425 (30.58%) | 1,127 (71.69%) | 1,007 (98.24%) | 1,041 (98.30%) |
| Lighting on | 0 | 297 (37.41%) | 1,290 (35.34%) | 745 (53.60%) | 455 (28.94%) | 0 | 0 |
| Fog (mode Fog) | 0 | 0 | 800 (21.92%) | 390 (28.06%) | 336 (21.37%) | 0 | 0 |
| Shadow pass (fragment operation mode Shadow) | 0 | 0 | 50 (1.37%) | 20 (1.44%) | 24 (1.53%) | 0 | 0 |
| Shadow texture on unit 0 (Shadow2D) | 0 | 0 | 90 | 0 | 21 | 0 | 0 |
| Border colour in use (always `0xffffffff`) | 0 | 0 | 90 | 0 | 21 | 0 | 0 |
| Gas (fog mode Gas, or fragment operation mode Gas) | 0 | 0 | 0 | 20 (1.44%) | 0 | 0 | 0 |
| Depth buffer in use | 6 (3.87%) | 69 (8.69%) | 840 (23.01%) | 585 (42.09%) | 361 (22.96%) | 9 (0.88%) | 6 (0.57%) |
| Alpha test GreaterThan (else off) | 0 | 3 | 140 | 315 | 112 | 0 | 0 |
| Three texture units | 0 | 21 | 120 | 130 | 30 | 0 | 0 |

More `census` detail the design uses:

- **Lighting:** 1 light in 297, 970, 310 and 299 draws (G2, G3, G4, G9a), 2 lights in 320 (G3) and 156 (G9a), 3 lights in 435 (G4). Configurations Config0 and Config1 only. LUT sets: none, FR+RR, DA, D0+RR, DA+FR+RR. Bump mode NormalMap in 50 draws (G3), None in the rest. The lighting shadow enable bit is on in 63, 800, 150 and 219 lit draws (G2, G3, G4, G9a). The other six configurations, the spot-attenuation LUT and D1 never appear. The same figure 800 turns up four times in G3 (fog, lighting shadow on, Config1, the FR+RR LUT set); that they are the same 800 draws is an `inference` (the census has no joint table).
- **Gas:** G4's 20 draws are 10 with fog mode Gas and 10 with fragment operation mode Gas, in the same 10 frames (the two tables, [5.11](#511-gas-stays-open)). That they are a density draw and a shading draw per frame is an `inference` from 3dbrew's description (`rhi-spike 5.5`).
- **Procedural textures:** the census records only on or off. In five captures the number of draws with the texture on equals the number of geometry-shader draws (140, 668, 2,560, 1,007, 1,041); in G4 (425) and G9a (1,127) it equals the draws of the one vertex layout with 8 attributes and stride 80 (the census's `vertex_layout` table). So the texture is used inside the sprite pass that runs the geometry shader (`inference`). D5's "one procedural-texture configuration" is not in this census (it has no procedural-texture group): `unverified` ([7](#7-unverified-and-the-census-extension)).
- **Draw state:** equations Add/Add in all 9,645 draws. Stencil test "Always" with Keep/Keep/Replace in 6, 6, 20, 20, 6, 9 and 6 draws. Depth test off, LessThanOrEqual or Always. Colour format RGBA8 and depth format D24S8 in every draw (one `render_target` configuration per capture).
- **Texture types and formats:** Texture2D and Shadow2D only. Border colour only with the Shadow2D draws (the counts 90 and 21 equal the Shadow2D counts; the same draws, `inference`). Ten texture formats (I8, A4, ETC1A4, ETC1, RGBA8, IA4, RGBA4, IA8, RGB5A1, I4); the CPU decodes them to RGBA8 (N2-14), so the fragment stage sees none of them.
- **Not seen in any of the seven captures:** effective logic ops, triangle fans, min/max blending, 8-bit indices, odd strides, cube textures, projection textures, ShadowCube. "Not seen" is not "never": no field, roar or heavy HUD scene is among the seven (D4's note).
- **Unique configurations per capture** (the census's table, `census`):

| Group | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| `fragment_config` (mode, blend equations, colour write, alpha test, stencil, depth: a summary, not a key) | 2 | 4 | 6 | 7 | 6 | 2 | 2 |
| `tev_config` (hash of the six stages and the update masks; an upper bound, Dot3_RGBA masking is not applied) | 5 | 15 | 22 | 23 | 23 | 5 | 5 |
| `lighting_config` (lights, configuration, LUTs, bump; "off" counts) | 1 | 3 | 5 | 6 | 5 | 1 | 1 |
| `vs_program` | 4 | 8 | 13 | 19 | 14 | 4 | 5 |
| `gs_program` | 1 | 1 | 1 | 4 | 3 | 1 | 1 |
| `vertex_layout` | 2 | 5 | 9 | 11 | 10 | 2 | 3 |
| `texture_config` | 7 | 21 | 35 | 30 | 38 | 12 | 9 |
| `render_target` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| `full_state` (everything the census decodes, texture sizes included) | 7 | 34 | 63 | 52 | 66 | 12 | 11 |

The census's `tev_config` is dominated by two hashes: in G1, 105 and 35 of 155 draws (`census`, the "Most used" table).

### 2.2 What D4 fixed

D4 is `Decided` (2026-10-23): option 2b, plume's Vulkan backend vendored and trimmed, numbered patches on a pristine copy, an operation-level wrapper of ours above it (`rhi-spike 9`; `N2a N2-25` to `N2-30`). What binds this design, with the section that says it:

| Fact | Where | Consequence here |
|---|---|---|
| Blend state is pipeline state on every device: plume bakes it, MoltenVK cannot make it dynamic (`extendedDynamicState3ColorBlendEnable`, `ColorBlendEquation`, `ColorWriteMask`, `LogicOpEnable` false, 5 of 31 bits). Stencil reference and masks and the blend constant are dynamic (patches 10 and 07). | `rhi-spike` X8, 6.1 G8, 6.8; `N2a N2-25` | blend is in the pipeline key; [4.3](#43-the-pipeline-key-and-how-many-keys-there-are), [5.9](#59-pipeline-counts) |
| The fragment shader writes depth in every non-shadow draw. | `rhi-spike` X2 | no early depth test for PICA draws; [5.4](#54-depth) |
| Shadow write: a pass with no attachments storing into the shadow map's `R32_UINT` view; `interlock` bit; Azahar's compare-and-swap loop otherwise; lookup by texel fetch, out of range is lit. | `rhi-spike` 5.2, 5.3, 9.4 item 4; `N2a N2-26`, `N2-27` | [5.1](#51-shadow-writes-and-the-shadow-lookup) |
| Hardware logic ops only where the `logic_op` bit is on, and then only with blending off. | `rhi-spike` 5.6, 9.3; `N2a N2-13` | [5.2](#52-logic-ops) |
| plume has no combined image sampler; texture and sampler are two descriptors. | `rhi-spike` 6.3, 8.2 | [5.6](#56-samplers) |
| Custom border colour only where the `custom_border_color` bit is on (patch 12). | `rhi-spike` 9.3, 9.4 item 2; `N2a N2-27`, `N2-29` | [5.7](#57-border-colours) |
| Uniform buffers at per-draw offsets over a ring, bound by a set update per draw (no dynamic-offset type); texel buffers; a storage image. | `rhi-spike` X5, 5.7, 6.1; `N2a N2-27` | [5.10](#510-uniforms) |
| The graphics pipeline library is not used and is absent on both M2 drivers; the pipeline cache blob API exists. | `rhi-spike` 9.3, 9.4 item 8; `N2a N2-28` | ubershader plus lists are the only stutter defence; [4.5](#45-when-the-specialised-pipeline-replaces-it) |
| The feedback copy (end the pass, copy the target, resume with load) is the RHI's one route to the destination colour. | `rhi-spike` X6, 5.6, 9.4 item 6 | logic ops, min/max, gas candidates |
| Fragment barycentrics are not enabled in the vendored plume (not patched). | `N2a N2-25` | the barycentric route for the quaternion fix-up is not offered; [5.5](#55-the-quaternion-fix-up-per-device) |
| The capability structure and its mask (`MH4N_RHI_MASK`), filled from device queries. | `rhi-spike` 9.4 item 2; `N2a N2-26` | every path below is chosen by one of its bits |

Device facts that the paths depend on (`rhi-spike` 3.1 to 3.3 and 9.3): the RX 6700 XT has pixel and sample interlock, geometry shaders, `logicOp`, custom border colours and barycentrics, but no `D24_UNORM_S8_UINT` and no `present_wait`; the M2 under MoltenVK 1.4.2 has interlock but no `logicOp`, no geometry shaders, no custom border colours and no pipeline library; under KosmicKrisp it has `logicOp` but no interlock (so only the compare-and-swap shadow write); lavapipe reports more than the PC and must run every fallback through the mask. All M2 entries are `stand-in` until the maintainer's Mac run of `mh4n_vkprobe`.

### 2.3 What Azahar's generators are

`source: azahar@5f3306d`; `wc -l` of each file (`measured` here, not the code-line counter of `rhi-spike 5.9`):

| File | Lines | What it is |
|---|---:|---|
| `shader/generator/pica_fs_config.h`, `.cpp` | 467, 222 | `FSConfig`: `FramebufferConfig`, `TextureConfig`, `LightConfig`, `ProcTexConfig`, each versioned by a consteval `StructHash`; `UsesSpirvIncompatibleConfig` |
| `shader/generator/glsl_fs_shader_gen.cpp`, `.h` | 1,774, 104 | the GLSL fragment generator (`FragmentModule`): the only one that writes shadows, min/max blending and ShadowCube |
| `shader/generator/spv_fs_shader_gen.cpp`, `.h` | 1,631, 298 | the SPIR-V generator, built on the `sirit` library (`externals/sirit`) |
| `shader/generator/shader_uniforms.h` | 97 | `FSUniformData` (0x530 = 1,328 bytes, `static_assert`), `VSUniformData` (32), `VSPicaUniformData` (1,616) |
| `renderer_vulkan/vk_shader_disk_cache.cpp` | 1,535 | the transferable disk cache: files `VS_CACHE`, `FS_CACHE`, `GS_CACHE`, `PL_CACHE`, entries `FS_CONFIG`, `FS_SPIRV`, `PL_CONFIG`, a `FILE_INFO` header carrying the struct hash |

What it does that this design keeps or changes:

- **The key is the requested configuration.** `ShaderDiskCache::UseFragmentShader` hashes `FSConfig{regs}` before any profile is applied; the generators apply the device profile inside their constructors (`config.ApplyProfile(profile_)`, `glsl_fs_shader_gen.cpp:107`, `spv_fs_shader_gen.cpp:24`). So one key means one shader id on every device while the generated code differs, and the disk cache is transferable between devices. The design keeps this.
- **What `ApplyProfile` does:** `FramebufferConfig::ApplyProfile` sets the emulated logic op when `!has_logic_op` and blending is off, and marks min/max emulation unless both factors are One (`has_blend_minmax_factor` is false in Azahar's Vulkan profile, `vk_pipeline_cache.cpp` where the profile is built); `TextureConfig::ApplyProfile` sets the border-emulation bits for `ClampToBorder` wraps when `!has_custom_border_color` (`pica_fs_config.cpp:100, 103`).
- **Pipeline state is separate.** `StaticPipelineInfo` holds `shader_ids[3]`, `BlendingState`, `AttachmentInfo`, `VertexLayout`, `RasterizationState` and `DepthStencilState`; `DynamicPipelineInfo` holds the blend colour, stencil reference and masks, scissor and viewport (`vk_graphics_pipeline.h`). Its `OptimizedHash` leaves rasterisation and depth-stencil out of the hash when extended dynamic state exists (`vk_graphics_pipeline.cpp:35`); plume has none, so ours keeps them.
- **Where it is incomplete (all `source`):** no ubershader and no list of keys to precompile (a draw whose pipeline is not ready is skipped when asynchronous compilation is on, which is off by default: `rhi-spike 5.8`); gas logs "Unimplemented gas mode" and writes transparent black (`glsl_fs_shader_gen.cpp:835`, `spv_fs_shader_gen.cpp:197`); the SPIR-V generator's logic-op switch handles Clear, Set, Copy and NoOp (CopyInverted's line is commented out) and logs "Unhandled logic_op" for the other 11; the GLSL generator's `WriteLogicOp` has the same five cases and writes `color = ~color;` for CopyInverted on a float vector, which does not look valid GLSL (`inference`; `glsl_fs_shader_gen.cpp:872`); shadow writes, min/max blending and ShadowCube are GLSL-only (`UsesSpirvIncompatibleConfig`).
- **That the renderer is "proven on MH4U" is D5's claim and is `unverified` here** (`rhi-spike 5`): nothing in the code shows it; Azahar's output on these scenes would.

## 3. The three options of D5

D5's text (`DECISIONS.md`) lists them. The table says what each is, what the evidence says for and against it, and what it costs the packages that wait.

### 3.1 The options and the evidence

| | Option 1: our own generator, with an ubershader while pipelines compile | Option 2: port Azahar's GLSL and SPIR-V generators as they are | Option 3: an ubershader only |
|---|---|---|---|
| **What it is** | One generator of ours with a module per feature; Azahar's `FSConfig` key and generator semantics copied into the feature modules with provenance (D6); an ubershader for first use; precompiled lists | `glsl_fs_shader_gen.cpp` and `spv_fs_shader_gen.cpp` with `pica_fs_config.*` and `shader_uniforms.h`, lowered for separate samplers; whatever else the stutter needs added beside them | One big fragment shader reading the whole state from a block; no key, no specialised shader |
| **For** | Features in separate directories fit the parallel packages N2-09 to N2-12 and N2-23 (their allowlists are `fragment/<feature>/**`). One generator instead of two. All 16 logic ops, gas hooks and separate samplers designed in from the start. The ubershader is generated from the same source as the specialised shader, so it cannot drift. | Least new code; the semantics are Azahar's and the generators are in use there (`unverified` on MH4U). The SPIR-V generator avoids glslang at run time. | No compile stutter from fragment shaders by construction; no key, no lists of fragment keys; the least code; one shader to debug. |
| **Against** | The most design work (the emitter with two modes, [4.4](#44-the-ubershader)); departs from proven code, so the oracles (the software renderer's formulas in every feature package, the captures in N2-20) carry the risk. | Two generators: shadow writes (3 of 7 captures, 94 draws), min/max blending and ShadowCube exist only in the GLSL one, so the GLSL one is needed for this game and the SPIR-V one adds a second set of semantics and the `sirit` dependency for the rest (`source`, [2.3](#23-what-azahars-generators-are)). Gas, 16 logic ops, the ubershader and the lists are all still ours to write. Monolithic classes: N2-09 to N2-12 would edit the same two files in parallel, which the plan's rule that parallel packages share no files forbids. About 3,400 lines (`wc -l` of the two generators) to port and lower for separate samplers. | Per-pixel cost unmeasured against D19's 6 ms at 3440×1440 and, by the `estimate` of [4.4.3](#443-cost-per-pixel-at-34401440), large on the M2. No early depth test either way. It does **not** remove pipeline creation: blend, depth, stencil, cull, formats and vertex layout stay in the pipeline (X8), so lists and background compilation are still needed for the fixed-function part. Compiling one very large module into every such pipeline costs more per pipeline (`unverified`). |
| **Gas** | designed for ([5.11](#511-gas-stays-open)); N2-10 fills it | none; N2-10 adds it inside Azahar's classes | none until N2-10; the block carries it |
| **Logic ops (all 16)** | in the emitter ([5.2](#52-logic-ops)) | must be added to both generators | in the block |
| **Samplers** | separate texture and sampler from the start | lowered by hand in about ten sampled lookup sites of the GLSL (`glsl_fs_shader_gen.cpp:510, 1716-1750`) and the SPIR-V equivalents (`source`) | separate from the start |
| **Size of N2-08** | L as specified; N2-16 as specified | L, mostly porting; N2-16 as specified but writes the ubershader by hand as a second implementation | L, smaller; N2-16 shrinks to the cache and the fixed-function list |

### 3.2 Consequences for the waiting packages

| Package | Option 1 | Option 2 | Option 3 |
|---|---|---|---|
| **N2-08** (generator, TEV, alpha test, `--gpu native`; L) | builds `FragmentKey`, the two-mode emitter, the core stages (prologue, depth, scissor, TEV, alpha test), the ubershader, stubs for the five features | ports the two generators and `FSConfig`, lowers samplers, adds stubs inside them; the feature-directory structure of its *Spec* has nothing to hold | builds the block layout, the ubershader and stubs; no `FragmentKey` beyond the block |
| **N2-13** (framebuffer operations) | writes the output-stage snippets (all 16 logic ops, min/max) in `fbops/` against the hook of [4.7](#47-the-interface-between-the-command-processor-and-the-generator); `state/` unchanged | must add the same emulation to two generators (the SPIR-V one by hand in `sirit`) or send those draws to the GLSL one | the same snippets, in the block's branch |
| **N2-16** (pipeline cache, ubershader, background compilation; L) | as specified: key, lists, switch from ubershader to the specialised pipeline | as specified, plus a hand-written ubershader that must stay equal to the ported generators | no specialised path: the cache and the list cover fixed-function pipelines only; its "switch when ready" and "same image with and without the ubershader" lose their meaning |
| **N2-20** (integration on the captures) | every fix must hold in both modes; the two-mode emitter makes one fix serve both | fixes land in two generators plus the ubershader | one shader path to fix |
| **N2-09** lighting, **N2-10** fog and gas, **N2-11** procedural textures, **N2-12** shadows, **N2-23** sampling | each writes its feature once against [4.8](#48-the-feature-interface), tested in both modes | each edits the ported classes: not parallel, so the five either run in sequence or the classes are first split (more work) | each writes its feature as a branch of the block's code |
| **N2-15** (surface cache) | unchanged | unchanged | unchanged |

### 3.3 Recommendation

**Recommendation (for the maintainer; D5 stays `Open`): option 1, built ubershader first.**

1. **The ubershader is needed under every option.** Principle 5 of the roadmap ("pipelines are compiled ahead of time and cached on disk, and an ubershader draws while a new one compiles") has no counterpart in Azahar. Option 2 would therefore port two generators and then add a hand-written ubershader beside them: a second semantic implementation that has to be kept equal to the ported ones, where option 1 generates both from one source.
2. **Option 2's saving is smaller than it looks.** The SPIR-V generator cannot draw shadows, min/max blending or ShadowCube, and shadow passes are in three of seven captures (50, 20 and 24 draws; 5 per frame in the Arena fight). So the GLSL generator (1,774 lines) is needed for this game, and the 1,631-line SPIR-V generator would serve the other draws only, with a `sirit` dependency and 4 of 16 logic ops (`source`).
3. **Option 3 rests on a number nobody has.** [4.4.3](#443-cost-per-pixel-at-34401440) shows the arithmetic: with the stated assumptions, an ubershader as the only shader would use a large share of the 6 ms on the RX 6700 XT for lit draws and more than the whole of it on the M2. Those are assumptions (`estimate`), not measurements, and a wrong assumption either way is expensive after the code is written.
4. **The two-mode emitter turns the choice between 1 and 3 into a measurement.** If the generator emits both the specialised shader and the ubershader from one source, the ubershader exists after N2-08 and N2-16's counters (`--no-ubershader`, plus an `--ubershader-only` switch proposed in [8](#8-for-the-planner)) and N2-19's budget runs can measure it on the RX 6700 XT and the M2. If it proves cheap enough, the specialised path can be dropped by not building the part of N2-16 that switches. Going the other way (starting from option 3 and adding a generator later) would mean rewriting the shader as a generator.
5. **What would change this recommendation:** a measurement of the ubershader (for instance G3 inside D19's 6 ms on the M2 with the ubershader forced would favour option 3); the maintainer preferring the least new code over structure (option 2); or the emitter turning out harder than [4.4.1](#441-one-source-two-modes) says (the fallback is a hand-written ubershader of a limited feature set, which is option 2's shape).

What each choice leaves for the maintainer to weigh that no document here can settle: how much he values Azahar's unmodified semantics (option 2) against parallel, tested feature packages (option 1); and whether he accepts that the first minutes of a fresh install, on a Mac above all, run on the ubershader until the list is warm (true under every option that has one; `estimate`, [4.4.3](#443-cost-per-pixel-at-34401440)).

## 4. The design (option 1)

### 4.1 Data flow

```
GSP command lists ──> N2-04 command processor ──> pica::Backend::Draw(const DrawCall&)
                                                       │  DrawCall.state -> const PicaState*
                                                       ▼
 native/ (N2-08, the composition)   fragment::MakeKey(PicaState)     ──> FragmentKey      (requested, pre-profile)
                                    fragment::FillUniforms(...)      ──> FragmentUniforms (1,328 bytes) + LUT texel buffers
                                    N2-13: FixedFunctionState   ──> blend, depth, stencil, cull, colour write mask
                                    N2-05/06: vertex layout, vertex program id, the 7 varyings
                                                       │
                                       PipelineKey = { pass kind, vertex stage id, fragment key id,
                                                       fixed-function state, attachment formats }
                                                       ▼
 N2-16 pipeline cache:  key known and pipeline ready? ──yes──> specialised pipeline
                                  │no: queue compile on a worker thread
                                  ▼
                       ubershader pipeline for the same fixed-function state ──> draw now
```

The command processor hands over `const PicaState*` that is valid only during `Draw`; the dirty flags in it mean "changed since the previous draw returned" (`native/src/gpu/pica/backend.h`, `state.h`; `N2a N2-04` *As built*). The fragment extraction therefore runs on the thread that owns the processor (or on a copy of the roughly 105 KB `PicaState`, as N2-22 decides), re-extracts the key only when a dirty register of the fragment blocks is set, and hands the renderer thread a small `FragmentKey` and a uniform block. The 7 varyings between the vertex and the fragment stage are Azahar's fixed ones (`primary_color`, `texcoord0` to `texcoord2`, `texcoord0_w`, `normquat`, `view`, fixed locations; `glsl_fs_shader_gen.cpp:1267` `DefineInterface`); the vertex stage (N2-06) writes all seven, zero where the program leaves one unwritten, as Azahar's does.

### 4.2 Where each piece of state goes

Classes: **P** pipeline key (baked into the plume pipeline); **D** dynamic (set on the command list); **G** generated: a literal in the specialised shader, a field of the configuration block in the ubershader; **U** uniform data (same in both modes, no recompilation); **T** table (a texel buffer filled when its dirty bit is set).

| State | Class | Why | Azahar | In the captures |
|---|---|---|---|---|
| Blend enable, equations (rgb, alpha), the four factors, colour write mask | P | plume bakes blend on every device; MoltenVK cannot make it dynamic (X8) | `BlendingState` | blend on in all 9,645 draws; Add/Add only (`census`); the factors are not in the census (`unverified`) |
| Blend constant colour | D | patch 07 | `DynamicPipelineInfo.blend_color` | not in the census |
| Hardware logic op (16 values), only with the `logic_op` bit and blending off | P | N1 | `BlendingState.logic_op` | 0 draws |
| Logic op emulated in the shader (requested op) and the destination copy | G (+ P: colour write mask for NoOp) | the `logic_op` bit is off | `FramebufferConfig.logic_op`, `requested_logic_op` | 0 draws |
| Min/max emulation flags and the requested equation and factors | G (+ P: the pipeline blends ONE, ZERO, ADD) | Vulkan `MIN`/`MAX` ignore the factors | `BlendConfig`, `BlendingState.rgb_blend_emulation` | 0 draws |
| Depth test, compare op, depth write | P | plume has no extended dynamic state | `DepthStencilState` | off in 58 to 99 percent of draws per capture, LessThanOrEqual or Always otherwise (`census`) |
| Stencil test, compare op, the three stencil actions | P | same | `DepthStencilState` | Always with Keep/Keep/Replace in 6 to 20 draws per capture |
| Stencil reference, compare mask, write mask | D | patch 10 | `DynamicPipelineInfo` | not in the census |
| Cull mode, topology (lists, strips; fans rewritten by N2-05), flip viewport | P | | `RasterizationState` | topologies Strip and Shader only |
| Attachment formats (the host formats `ChooseFormat` returns) | P | the pipeline needs the render pass formats | `AttachmentInfo` | RGBA8 and D24S8 in every draw, so D32S8 on the RX 6700 XT and both M2 drivers |
| Pass kind: normal, or shadow write (no attachments, a storage image) | P | the pass differs | `shadow_rendering` | shadow passes in G3, G4, G9a |
| Fixed geometry stage for the quaternion fix-up | P | a pipeline stage | `PicaFixedGSConfig` | [5.5](#55-the-quaternion-fix-up-per-device) |
| Vertex layout, vertex program id, trivial or translated vertex stage | P | | `VertexLayout`, `shader_ids` | N2-05, N2-06 |
| Viewport, scissor rectangle | D | | `DynamicPipelineInfo` | |
| PICA scissor test mode (disabled, include, exclude) | G | changes the code; the rectangle is U | `scissor_test_mode` | not in the census |
| Alpha test function (Never means discard everything) | G | the reference value is U | `alpha_test_func` | off, GreaterThan |
| Depth-buffering mode (W buffering) | G | `WriteDepth` | `depthmap_enable` | not in the census |
| The six TEV stages (sources, modifiers, operations, scales as raw words) and the combiner-buffer update masks | G | | `TextureConfig.tev_stages`, `combiner_buffer_input` | 5 to 23 hashes per capture (`census`) |
| Texture 0 type, texture 2 coordinate source, the requested wrap modes and the border-emulation bits | G | the type changes the sampler declaration; border emulation is off the device profile | `texture0_type`, `requested_wrap`, `texture_border_color` | Texture2D and Shadow2D only |
| Fog mode (None, Fog, Gas) and the flip bit | G | | `fog_mode`, `fog_flip` | Fog in 3 captures, Gas in 1 |
| Lighting: lights, configuration, bump, clamp, shadow bits, per-LUT enable, input, absolute flag, scale, per-light flags | G | | `LightConfig` | [2.1](#21-what-the-captures-use) |
| Procedural texture: coordinate, clamps, combiners, LUT filter, shifts, noise, LUT width and offsets, LOD range | G | | `ProcTexConfig` | on in all seven; the configuration count is not in the census |
| Shadow rendering, shadow texture orthographic flag | G (+ P pass kind) | | `shadow_rendering`, `shadow_texture_orthographic` | |
| The "user" word (HD pack normal map) | G | N2-17/N2-18; Azahar keeps it outside its cache (`UserConfig::IsCacheable`) | `UserConfig.use_custom_normal` | none |
| TEV constant colours (6), combiner-buffer colour, alpha reference, depth scale and offset, scissor rectangle, fog colour, LUT offsets, light data (8 lights), global ambient, LOD biases, border colours, blend colour for min/max emulation, shadow biases, procedural noise parameters | U | data, changes often, no recompilation | `FSUniformData` | |
| Lighting LUTs, fog LUT, procedural noise, colour and alpha maps and LUTs | T | filled when `PicaState.lighting.lut_dirty`, `fog.lut_dirty` or `proctex.table_dirty` says so | `texture_lf_buffer`, `texture_buffer` | |
| Vulkan specialisation constants | none in the key | the driver folds them but a pipeline is still created per value; the key stays one thing. Used for one purpose: the ubershader modules take the device-profile switches (interlock or compare-and-swap, border emulation, logic-op emulation) as plume's `uint32` specialisation pairs (`rhi-spike 6.3`), so their SPIR-V is identical on every device and can be built once and cached. | `Profile` | |

On specialisation constants the question has a second answer worth recording. A "constant ubershader" (every key field a specialisation constant, the driver folding them, no generator) is possible in principle. It does not fit here for three reasons that are `inference`: the texture-0 type changes the sampler declaration and the descriptor, which a constant cannot do; plume's specialisation constants are `uint32` pairs (`rhi-spike 6.3`), a poor fit for a structure of this size; and the driver compiles the whole large module per value, which is the cost the specialised path exists to avoid. It is not designed further.

### 4.3 The pipeline key and how many keys there are

```
PipelineKey {
  pass kind            : Normal | ShadowWrite
  vertex stage         : N2-06 program id + N2-05 vertex layout (or the trivial stage for CPU-assembled batches)
  fixup stage          : none | fixed geometry shader            (lighting on, hardware vertex shader, geometry_shader bit)
  fragment             : FragmentKey id | UberModule id
  fixed-function state : topology, cull, flip viewport; depth test/compare/write; stencil test/compare/3 actions;
                         blend enable, 2 equations, 4 factors, colour write mask, hardware logic op
  attachments          : colour Format, depth Format (host formats)
}
FragmentKey = Azahar's requested FSConfig fields ([4.2]) + the user word, trivially copyable, versioned by a struct hash
```

The `FragmentKey` is the requested configuration: the device profile is applied when code is generated, never when the key is made (Azahar's rule, [2.3](#23-what-azahars-generators-are)). That keeps the lists of [4.6](#46-precompiled-pipeline-lists) transferable between the PC and the Mac. The fixed-function part differs by device in two places only, both through emulation: with the `logic_op` bit on, a logic-op draw has the hardware op in its pipeline key and a common shader; with it off it has the plain pipeline and a different fragment key; and the min/max emulation forces the ONE, ZERO, ADD blend into the pipeline key. Everything else is the same on every device, **including MoltenVK**, because plume bakes blend on all of them ([5.9](#59-pipeline-counts)).

**Size of the fragment key.** Summing Azahar's structs by hand (`FramebufferConfig` 32 bytes, `TextureConfig` 136, `LightConfig` 76, `ProcTexConfig` 28) gives about 272 bytes (`inference`: `static_assert(std::has_unique_object_representations_v<...>)` rules out padding, the sizes are mine). The disk cache and the ubershader's configuration block hold exactly this struct.

**How many unique keys the captures have: `unverified`.** The census has no pipeline-key group, and none of its groups is a bound on one: `full_state` includes texture sizes and formats that no key holds and omits the blend factors, the cull mode, the wrap modes and the lighting details that keys hold. What can be said exactly (`derived` from the census's per-capture counts and its "Most used" tables, whose lists are complete wherever they read "top N of N"):

| Component | Per capture (G1, G2, G3, G4, G9a, G9b, G9c) | Across the seven | How |
|---|---|---|---|
| `fragment_config` strings | 2, 4, 6, 7, 6, 2, 2 | **7 distinct** (exact) | every capture's list is complete; G4 holds all seven |
| `gs_program` hashes | 1, 1, 1, 4, 3, 1, 1 | **4** (exact) | the lists are complete |
| `vertex_layout` | 2, 5, 9, 11, 10, 2, 3 | 13 to 14 | listed union 13; G4 lists 10 of 11 |
| `vs_program` | 4, 8, 13, 19, 14, 4, 5 | 19 to 31 | lower bound the largest capture; upper bound the listed union (15) plus the unlisted entries |
| `tev_config` | 5, 15, 22, 23, 23, 5, 5 | 23 to 63 | listed union 20, unlisted 5 + 12 + 13 + 13 |

The number of unique **fragment** keys is bounded by neither the TEV count (Dot3_RGBA masking can merge TEV hashes; other fields multiply them) nor `full_state`. The number of unique **pipeline** keys additionally multiplies by the blend factors in use and the host formats, neither of which the census records. The theoretical space of blend state is 1,265,625 equation-and-factor combinations (`derived`: (5 equations × 15 factors × 15 factors) squared for the rgb and alpha halves, from Azahar's `BlendEquation` and `BlendFactor`), of which the captures show one equation pair; so the list of [4.6](#46-precompiled-pipeline-lists) holds keys seen, never a product. The exact command and census extension that produce the number are in [7](#7-unverified-and-the-census-extension).

### 4.4 The ubershader

#### 4.4.1 One source, two modes

The generator is written once, as C++ that emits GLSL text (Azahar's `FragmentModule` is the model). Every decision that Azahar's generator makes with `switch (config.x)` goes through an emitter helper that has two modes:

- **Specialised mode:** the helper evaluates the field at generation time and emits only the chosen branch, with the numbers as literals. This is Azahar's output.
- **Uber mode:** the helper emits a GLSL `switch` or `if` on the same field read from the configuration block (`cfg.tev[2].sources`), and loops where the key holds a count (`cfg.lighting.src_num`).

A feature writes its code once with these helpers; its tests run the same synthetic draw in both modes and must give the same image, and both against the software renderer's formulas (the oracle of N2-09 to N2-12 and N2-23). The module count of the ubershader is **two**: `uber_color` (the normal pass) and `uber_shadow_write` (the shadow-write tail, [5.1](#51-shadow-writes-and-the-shadow-lookup)). Unit 0 declares its three sampler classes (2D, cube, shadow `usampler2D` equivalent) as separate bindings and the draw binds a one-texel dummy for the classes it does not use, so one module serves every texture-0 type; that costs two extra descriptor writes per draw and saves two module variants (`inference`; the alternative of three modules by texture type is as valid).

The ubershader's SPIR-V is built from the generator with a fixed key, once per device profile, and cached; it is not generated per draw.

Risk, stated plainly: the emitter is the one new idea in this design, and its complexity is the argument against it. The fallback if it proves too hard is an ubershader written by hand for the features of [2.1](#21-what-the-captures-use) only, tested against the generator's output; that is option 2's shape of two implementations to keep equal, and the recommendation of [3.3](#33-recommendation) point 5 names it.

#### 4.4.2 What it covers and how it reads the state

| Feature | In the ubershader | How the state is read | Cost driver |
|---|---|---|---|
| TEV (six stages) | all six, every source, modifier, operation and scale | `cfg.tev[i]` raw words; per-stage `switch` | the dynamic switches; a stage whose configuration is the identity could be skipped by a per-stage "active" bit computed at key time (`inference`) |
| Sampling | three units and the procedural slot; 2D, cube, projection, shadow; wrap modes; border emulation; LOD | `cfg.texture`, `tex_lod_bias`, `tex_border_color` | a texture fetch per active unit plus wrap arithmetic |
| Lighting | up to 8 lights, the LUTs D0, D1, SP, FR, RR, RG, RB and DA, bump, clamping, the shadow term | `cfg.lighting`, `light_src[8]`, `lighting_lut_offset`, the `lut_lf` texel buffer | a dynamic loop over `src_num` lights, two texel fetches per LUT |
| Fog | the fog LUT; the gas hook | `cfg.texture.fog_mode`, `fog_lut_offset`, `fog_color` | one texel fetch |
| Procedural textures | noise, clamps, shifts, the colour and alpha maps and LUTs | `cfg.proctex`, `proctex_*` offsets, `lut_rg` and `lut_rgba` buffers | the heaviest single block; on in 30.58 to 98.30 percent of the draws (`census`) |
| Shadows | the lookup (texel fetch, four texels mixed, out of range lit) in `uber_color`; the write tail in `uber_shadow_write` | `cfg.texture.texture0_type`, `shadow_*` uniforms | one fetch per tap |
| Output | alpha test, depth write, byte rounding, min/max emulation, logic-op emulation | `cfg.framebuffer` | a branch each; the destination copy is a draw-time decision, not a shader one |

All branches are on draw-uniform values (the block is the same for every fragment of a draw), so no divergence is introduced (`inference`: on the RX 6700 XT and the M2 uniform branches are scalar or cheap; not measured). The block is the `FragmentKey` struct itself, appended to the fragment uniforms in the same ring allocation ([5.10](#510-uniforms)), so a draw makes one uniform write and one binding update.

#### 4.4.3 Cost per pixel at 3440×1440

**This is an `estimate`; every input below is an assumption, none is measured.** The measurement is N2-16's forced-ubershader runs and N2-19's budget runs.

- Output size: 3440 × 1440 = 4,953,600 pixels (`derived`). D19: the GPU share is at most 6 ms at this size with the HD pack.
- **Fragments shaded per output pixel per frame (overdraw), k:** not in the census (it holds draws, not areas). Assumed 2 and 4.
- **Sustained throughput:** RX 6700 XT about 13.2 TFLOPS fp32 peak (`memory`), M2 about 2.8 to 3.6 TFLOPS depending on core count (`memory`; the cores of the maintainer's Mac are unknown), 30 percent sustained (assumed), one lane-instruction per FMA, so 1,980 and 540 lane-instructions per nanosecond with the larger M2 figure.
- **Instructions per fragment, by hand from the structure of Azahar's generated code, ±2×:**

| Draw | Specialised | Ubershader | Share of the 6 ms at k = 2, ubershader, RX 6700 XT / M2 |
|---|---:|---:|---|
| (a) unlit, 2 texture units, 3 active TEV stages | about 155 | about 355 | 1.8 ms (30%) / 6.5 ms (108%) |
| (b) 3 lights, fog, 2 units, 4 stages | about 475 | about 865 | 4.3 ms (72%) / 15.9 ms (264%) |
| (c) 8 lights, fog, procedural texture, 3 units, 6 stages (nothing like it in the captures: 3 lights at most) | about 1,015 | about 1,745 | 8.7 ms (146%) / 32.0 ms (533%) |

(`derived` from the assumptions: time = 4,953,600 × k × instructions / sustained rate.) The sentence the table supports is this: **as the only shader for a whole frame, the ubershader would use a large share of the 6 ms on the RX 6700 XT for lit draws and more than the whole budget on the M2.** The same arithmetic says the specialised shaders are 1.7 to 2.3 times cheaper per fragment (an `estimate` of the ratio only). And neither mode has an early depth test ([5.4](#54-depth)), so occluded fragments cost the same in both. On this estimate the ubershader is a bridge of seconds, not a steady state, on any device that is not much faster than the RX 6700 XT; this is also the argument against option 3 ([3.1](#31-the-options-and-the-evidence)).

Compile cost of the ubershader module is `unverified` (a very large module, one pipeline per fixed-function state seen); the lists ([4.6](#46-precompiled-pipeline-lists)) build these first.

### 4.5 When the specialised pipeline replaces it

Per draw, on the renderer thread (N2-16 owns the code):

1. Build the `PipelineKey`. Look it up in the pipeline table (a lock-free read of a published, immutable entry).
2. **Present and ready:** draw with it.
3. **Absent:** insert a placeholder (so no second request is made), queue "generate, compile with glslang, create the pipeline" on a worker thread (the backend allows creation from several threads, `N2a N2-28`), and draw this call with the ubershader pipeline for the same fixed-function state.
4. **Placeholder, not ready:** draw with the ubershader. When the worker publishes the pipeline, the next draw with that key uses it. A switch happens between draws, never inside one.
5. The key goes into the list of [4.6](#46-precompiled-pipeline-lists) at its first sight, so the next start compiles it before it is needed.

Rules this implies:

- **The two modes must give the same image** within the thresholds, or the switch shows as flicker. The sources of difference are float contraction and the order of operations; the PICA rounds to 8 bits at several points (`byteround` in Azahar's output), which hides most of it. N2-16's *Done when* already requires the same image with forced misses; the feature packages require it per feature.
- **The ubershader pipeline for a fixed-function state must exist** when a draw with a never-seen state arrives (a new blend factor pair, say). Plume bakes that state, so the ubershader pipeline for it is itself a compile, and N2-16's "such a draw always goes through the ubershader, never skipped" cannot hold for it. Proposed rule ([8](#8-for-the-planner), question 2): the ubershader pipeline is built **synchronously, once**, the first time a state is ever seen (counted and reported, and the state goes into the list so it never repeats on that machine); the alternative, skipping the draw, is Azahar's asynchronous behaviour and shows as a glitch.
- **Eviction:** a specialised pipeline stays for the run (the count is in the tens, [4.3](#43-the-pipeline-key-and-how-many-keys-there-are)); the list is the only thing that persists.

### 4.6 Precompiled pipeline lists

**What they record.** Three things, in this order of importance:

1. **Keys** (authoritative, device independent): the `PipelineKey` in its requested form (the fragment key before any profile, the fixed-function state, the logical PICA pixel formats rather than the host formats, so a list moves between the PC and the Mac), with a use count and the program ids of the vertex stage. The file header holds the struct hash of the key types, as Azahar's `FILE_INFO` entry does (`vk_shader_disk_cache.cpp`, `CacheEntryType::FILE_INFO`, `FS_CONFIG`, `PL_CONFIG`): a change to a key type invalidates the list, and the list is rebuilt by play.
2. **Programs:** the PICA vertex and geometry programs (words and swizzle words) a key's vertex stage was translated from, by hash (Azahar's `VS_PROGRAM`). The translated SPIR-V is regenerable and may be cached beside them as a speed-up, dropped when the generator's version changes.
3. **The driver's blob:** `SavePipelineCache()` and `LoadPipelineCache()` of N2-28, one file per device (vendor, device, `pipelineCacheUUID`), refused when the header does not match.

**Where they live.** On the player's PC, under the user folder of D61 (`<user folder>/native/pipelines/`, a name N2-16 fixes), never in the repository and never in a release: programs and keys are derived from the game, and the driver blob is machine specific. ROADMAP's legal row says it outright ("pipeline lists and caches ... never enter the repository or the packages") and `docs/native/AGENTS.md` §8 rule 2 forbids anything derived from the game in the repository.

**How they are built.**

- **From captures (the maintainer's PC, and CI never):** the capture player with N2-16's option `--record-keys FILE` (proposed here; N2-16's *Spec* has `--warm` and `--no-ubershader`) replays G1 to G4 and G9a to G9c and writes the keys it makes. This seeds a list for the census scenes and is the source of the `unverified` count of [4.3](#43-the-pipeline-key-and-how-many-keys-there-are).
- **From play:** the render thread appends each key at its first sight (N2-16's *Spec*), deduplicated by hash, written by the pipeline worker, never by the render thread.
- **At start:** the ubershader pipelines for every fixed-function state in the list come first (they are what stops the first draw from stalling), then the specialised pipelines in decreasing use count on worker threads, while the game boots. How long the whole list takes on a given driver is `unverified`; N2-16's maintainer check (a warm cache, no frame of G3 over 8.3 ms because of a pipeline) measures it.
- **A new machine:** an empty list is valid. Every draw then takes the ubershader (after the one-time synchronous build of [4.5](#45-when-the-specialised-pipeline-replaces-it) for each fixed-function state) and the specialised pipelines arrive in the background. This is the case the ubershader exists for.

### 4.7 The interface between the command processor and the generator

The command processor's `DrawCall` (`native/src/gpu/pica/backend.h`: `kind`, `sequence`, `vertex_base_address`, `num_vertices`, `vertex_offset`, `index_offset`, `index_u16`, `geometry_reconfigured`, `state`) carries no fragment data of its own; everything is in `*draw.state`. The fragment module exposes, in `native/src/gpu/fragment/` (a **proposal**: names are N2-08's to fix):

```cpp
namespace mh4n::gpu::fragment {

struct DeviceProfile {          // from rhi::Capabilities after the mask (N2-26); the Azahar Profile, reduced
  bool logic_op, custom_border_color, interlock, geometry_shader;
  bool has_blend_minmax_factor = false;   // false on every device with plume (Vulkan MIN/MAX ignore factors)
};

struct FragmentKey;             // [4.3]: requested FSConfig fields + user word; StructHash-versioned
FragmentKey MakeKey(const pica::PicaState&);                         // pure; no profile

struct Needs {                  // what a draw with this key needs from the renderer
  bool shadow_write_pass;       // zero-attachment pass + storage image + (interlock or CAS)
  bool dst_copy;                // FeedbackCopy before the draw: logic-op emulation (11 of 16 ops) or min/max
  bool fixup_gs;                // lighting on and the vertex stage is the translated one
  bool lut_lf, lut_rg, lut_rgba;            // texel buffers to bind
  u8   texture_units;                       // units with a texture or sampler to bind
  bool ordered_rmw;                         // reserved for gas (N2-10): needs interlock or a copy
};
Needs NeedsOf(const FragmentKey&, const DeviceProfile&);

struct Generated { std::vector<u32> spirv; std::string glsl_for_logs; };
Generated GenerateSpecialised(const FragmentKey&, const DeviceProfile&);   // worker thread
Generated GenerateUber(PassKind, const DeviceProfile&);                    // once per profile

struct Uniforms;                // Azahar's FSUniformData layout (1,328 bytes) + the 'cfg' block
void FillUniforms(const pica::PicaState&, Uniforms&, UniformDirty&);       // writes only what a dirty bit says
void UploadTables(const pica::PicaState&, TableRings&);                    // lighting, fog, proctex LUTs
}
```

Two properties: `MakeKey` and `NeedsOf` are pure functions of the state and the profile (so the key tests run with no device and N2-16's list tests need no GPU), and nothing in this header names a Vulkan or plume type (`rhi.h` rules of N2-26). The composition (`native/`) owns the `PipelineKey` and its assembly from this module's key, N2-13's `FixedFunctionState` and N2-05/N2-06's vertex description; N2-16 serialises it.

The hook the output stage needs from N2-13 (`fbops/`): `EmitLogicOp(Emitter&, LogicOp, Mode)`, `EmitMinMaxBlend(Emitter&, BlendConfig rgb, BlendConfig alpha, Mode)` and their `Needs` contribution, called by the generator at the output point of [4.8](#48-the-feature-interface).

### 4.8 The feature interface

The generator calls each feature at fixed **points**, in Azahar's order (`FragmentModule::Generate`, `glsl_fs_shader_gen.cpp`):

| Point | What is emitted | Who |
|---|---|---|
| declarations | varyings, bindings, helper functions | every feature, in directory order |
| prologue | primary colour rounding; discard everything when the alpha test is Never | core (N2-08) |
| scissor, depth | `WriteScissor`, `WriteDepth` | core |
| lighting | sets the primary and secondary fragment colours | `lighting/` |
| tev | six stages; a stage's source calls `Source(unit)` | core, with `sampling/` and `proctex/` supplying the sources |
| alpha test | | core |
| fog and gas | `WriteFog`; the gas hook | `fog/` |
| output | shadow-write tail, or: depth write, byte rounding, min/max emulation, colour, logic-op emulation | `shadow/`, or core with `fbops/` (N2-13) |

```cpp
struct FeatureModule {
  const char* name;                                       // "sampling" "lighting" "fog" "proctex" "shadow"
  void   Key(const pica::PicaState&, FragmentKey&);       // fills ITS sub-struct of the key only
  void   Emit(Point, EmitContext&);                       // ctx: key, profile, mode, the Emitter helpers of 4.4.1
  void   FillUniforms(const pica::PicaState&, Uniforms&, UniformDirty&);
  void   UploadTables(const pica::PicaState&, TableRings&);     // if the feature has LUTs
  Needs  NeedsOf(const FragmentKey&, const DeviceProfile&);     // ORed into the draw's Needs
  const char* Stubbed(const FragmentKey&);                // non-null when the feature is a stub that was needed
};
```

What each directory contributes:

| Directory (package) | Key fields | Generated code (both modes) | Ubershader | Uniforms and tables | Needs and pipeline effects |
|---|---|---|---|---|---|
| `sampling/` (N2-23) | texture 0 type, texture 2 coordinate source, requested wraps, border-emulation bits | `sampleTexUnit0` to `3`, wrap and border emulation, LOD, projection, cube, the normal map; routes unit 3 to `proctex/` | type and wrap read from the block; three sampler classes for unit 0 | `tex_lod_bias[3]`, `tex_border_color[3]` | texture and sampler per unit (two descriptors each); sampler objects from the PICA filter and wrap, clamp-to-edge where the border is emulated |
| `lighting/` (N2-09) | `LightConfig` | light loop, LUT lookups, bump, clamp, the shadow term (calls `shadow/` lookup); declares `normquat` and `view` | dynamic loop over `src_num` | `lighting_lut_offset`, `light_src[8]`, `lighting_global_ambient`; table `lut_lf` | `fixup_gs` where the device allows ([5.5](#55-the-quaternion-fix-up-per-device)); also provides the fixed geometry shader generator |
| `fog/` (N2-10) | fog mode, fog flip; the fragment operation mode Gas | `WriteFog`; the gas hook | one branch | `fog_color`, `fog_lut_offset`; table `lut_lf` (fog part) | gas may need `ordered_rmw` or `dst_copy` ([5.11](#511-gas-stays-open)) |
| `proctex/` (N2-11) | `ProcTexConfig` | `ProcTexSampler`, clamps, shifts, combiners, LUT lookups | read from the block | `proctex_*` offsets and noise parameters; tables `lut_rg`, `lut_rgba` | none |
| `shadow/` (N2-12) | shadow rendering, orthographic flag; texture-0 type is `sampling/`'s field, interpreted here for Shadow2D and ShadowCube | `CompareShadow`, the four-texel lookup, `UpdateShadow`, the write tail (interlock or compare-and-swap) | `uber_shadow_write`; the lookup in `uber_color` | `shadow_bias_constant`, `shadow_bias_linear`, `shadow_texture_bias` | `shadow_write_pass`: zero-attachment pass, storage image, the `interlock` bit |
| output (`fbops/`, N2-13, and core) | requested logic op, requested blend (rgb, alpha), the emulation flags | logic-op emulation of all 16, min/max emulation | read from the block | `blend_color` copy for min/max | `dst_copy`; the pipeline's blend and colour write mask |
| core (`fragment/`, N2-08) | alpha test function, scissor mode, depth-buffering mode, the six TEV stages, combiner-buffer masks, the user word | prologue, depth, scissor, TEV, alpha test | read from the block | `const_color[6]`, `tev_combiner_buffer_color`, `alphatest_ref`, `depth_scale`, `depth_offset`, scissor rectangle | none |

Shared fields: `texture0_type` belongs to `sampling/` and `shadow/` reads it; a feature that needs another's lookup (the lighting shadow term calls the shadow lookup) does so through a named function declared at the **declarations** point, never by reading the other feature's key fields. A stub implements `Key` (so the key is complete), emits a neutral value, reports `Stubbed` and a `stubbed:<feature>` counter, as `N2a N2-08` specifies.

## 5. Paths the RHI lacks (what D4 fixed)

The paths that depend on the device are chosen by a bit of the backend's capability structure (`N2a N2-26`), so lavapipe runs each of them through `MH4N_RHI_MASK`. Three rows of the table are the exceptions and say why: min/max blending is chosen by the draw's factors, not by the device; the depth write is the same everywhere; and the geometry-shader route has a value in N2-26 where a maskable bit is needed.

| Gap (device) | Bit | Mask name | The path without it | Section |
|---|---|---|---|---|
| no interlock (KosmicKrisp) | `interlock` | `interlock` | compare-and-swap loop | 5.1 |
| no hardware logic op (MoltenVK) | `logic_op` | `logic_op` | shader emulation of all 16 through the feedback copy | 5.2 |
| min/max ignores factors (every Vulkan device) | none: chosen by the factors | none | shader path through the feedback copy; runs on lavapipe unmasked | 5.3 |
| fragment depth write | none | none | no early depth test | 5.4 |
| no geometry shader (both M2 drivers) | `geometry_shader` is a value in N2-26, not a bit | none yet | no fix-up for hardware-vertex-shader lit draws | 5.5, question 1 of [8](#8-for-the-planner) |
| no combined sampler (plume) | none | none | lowered to two descriptors | 5.6 |
| no custom border colour (both M2 drivers by default) | `custom_border_color` | `custom_border_color` | shader emulation, clamp-to-edge | 5.7 |
| GLSL-only Azahar paths | none | none | one generator | 5.8 |
| blend baked (every device) | none | none | blend in the pipeline key | 5.9 |
| no dynamic-offset uniform buffer (plume) | none | none | a set update per draw | 5.10 |
| gas undrawn (everywhere) | none | none | open | 5.11 |

### 5.1 Shadow writes and the shadow lookup

**Write.** `fragment_operation_mode` Shadow (3) selects the pass kind `ShadowWrite` (`source: azahar@5f3306d pica_fs_config.cpp`, `shadow_rendering`): a pass with no attachments (N2-26's `BeginPass` with explicit width and height, patch 01) whose fragment shader stores into the shadow map through its `R32_UINT` storage view (N2-27: the colour image created with the mutable-format flag and the format list `R8G8B8A8_UNORM`, `R32_UINT`, `shadow_source`, storage usage on it alone). The shader packs `d = uint(clamp(depth, 0, 1) * 0xFFFFFF)` and a stencil-like byte from the combiner output, calls `UpdateShadow(old, d, s)` and stores. **Ordering:** where the `interlock` bit is on, the store sits between `beginInvocationInterlock` and `endInvocationInterlock` with `pixel_interlock_ordered` (Azahar's `WriteShadow` and `DefineExtensions`, `glsl_fs_shader_gen.cpp:844-870, 1215-1233`; the generator declares no `pixel_interlock_*` layout and glslang defaults to `pixel_interlock_ordered`, `rhi-spike 5.2`); where it is off the shader runs Azahar's `imageAtomicCompSwap` loop. The loop is atomic but not primitive-ordered, and `UpdateShadow` is order dependent by the reading of `rhi-spike 5.2`, so **whether the loop differs visibly from the interlock path on the Arena scene is `unverified`** (N2-12's and D95's test). KosmicKrisp has no interlock and so takes the loop; lavapipe runs both, the second under `MH4N_RHI_MASK=interlock` (`N2a N2-12`).

**Lookup.** A texel fetch with a manual compare, never a compare sampler: the shadow map is read through the `R32_UINT` view as an unsigned texture, `texelFetch` four texels, compare each in `CompareShadow`, mix by hand; a texel with `uv < 0` or `uv >= size` returns 1.0 (lit) before the fetch (`glsl_fs_shader_gen.cpp:1475-1498`, `spv_fs_shader_gen.cpp:790-806`; `rhi-spike 5.3`). So the white border colour the census saw (`0xffffffff`, 90 and 21 draws) needs **no sampler feature and no border-colour enum**. With the separate-sampler lowering of [5.6](#56-samplers) the shadow unit needs no sampler descriptor at all (`texelFetch` takes the texture only).

**In the key and the ubershader.** `shadow_rendering` and the pass kind are in the key; `uber_shadow_write` is the second ubershader module, sharing the body (lighting, TEV) with `uber_color` and differing in the tail. ShadowCube (not seen) follows the software renderer, not Azahar's Vulkan code, which looks unfinished (`rhi-spike 5.3`; `N2a N2-12`). The census's 94 shadow draws (50 + 20 + 24) are 1.37 to 1.53 percent of the draws of the three captures that have them.

### 5.2 Logic ops

**Seen:** 0 effective logic-op draws in 9,645 (`census`; the census counts a logic op as effective with blending off, an op other than Copy, colour writes on, not in shadow mode, and a colour buffer). The path exists for correctness and for N2-13's *Done when*.

**Chosen by the `logic_op` bit, and only with blending off.** With the bit on, the pipeline key carries the hardware op (all 16 via plume's `logicOpEnabled`/`logicOp`, `rhi-spike 6.1` N1) and the fragment key carries nothing. With it off (MoltenVK; lavapipe under `MH4N_RHI_MASK=logic_op`), the fragment key carries the requested op, the shader emulates **all 16**, and the pipeline has blending off, with the colour write mask the requested one except for NoOp, where it is 0 (Azahar's `GetFinalColorWriteMask`: the mask discards the colour while the depth write stays).

**How the destination is read.** Of the 16 ops, five need no destination (Clear, Copy, Set, CopyInverted, NoOp); eleven do (And, AndReverse, Invert, Nand, Or, Nor, Xor, Equiv, AndInverted, OrReverse, OrInverted; `derived` from Azahar's `LogicOp` enumeration). Those eleven read it from the **feedback copy** (X6): the pass ends, the target is copied to its twin, the next pass resumes with load, and the shader binds the twin as `tex_color` (`destFactor` in Azahar's GLSL). That is the RHI's one route to the destination colour. It is an `inference` that this covers logic ops: `rhi-spike` 9.4 item 6 and 9.5 name X6 for min/max and not for logic ops. Framebuffer fetch and input attachments are not offered (plume exposes neither; the optional input-attachment patch is about 70 lines and Vulkan-only, `rhi-spike 6.8`). A storage-image read-modify-write of the colour target ordered by interlock would avoid the copy, but it needs storage usage and a mutable-format view on every colour surface, which D4's item 4 reserves for shadow sources so that Apple GPUs can compress the rest (`inference`): rejected here. The ops work on the 8-bit rounded values (`byteround` precedes them in Azahar's output); on the narrower formats (RGB565 and so on) the result is `unverified`, and none is in the census.

**Cost.** One pass break and one full-image copy per emulated draw that needs the destination (`rhi-spike 5.6`); with 0 such draws in the captures it is unmeasured.

### 5.3 Min/max blending

Hardware `MIN`/`MAX` only when **both factors are One** (`BlendConfig::UsesNoOpFactors`, per rgb and per alpha half): Vulkan's `MIN` and `MAX` ignore the factors, the PICA's do not. Otherwise the pipeline blends ONE, ZERO, ADD (the key's blend fields say so) and the shader computes `min` or `max` of `source × factor` and `dest × factor` from the feedback copy, with the blend colour from the uniform block (`WriteBlending`, `glsl_fs_shader_gen.cpp:897`; `rhi-spike 5.6`, N3). The choice is by the factors, not by a capability bit (`has_blend_minmax_factor` is false for Vulkan), so lavapipe runs both paths with no mask. Seen: no min/max equation in any capture (the equations are Add/Add in all 9,645 draws).

### 5.4 Depth

The fragment shader writes `gl_FragDepth` in every non-shadow draw (X2: the PICA depth is recomputed from `gl_FragCoord.z` with `depth_scale` and `depth_offset`, and from `1/w` in W-buffering mode, `WriteDepth` and `Generate` in `glsl_fs_shader_gen.cpp`). So **no early depth test exists for PICA draws** on any device, specialised or ubershader. How much that costs depends on how many draws could have used one: the census's "depth buffer in use" is 0.57 to 42.09 percent of the draws (G9c and G4), and the depth test is off in 77 percent of G3's draws (2,810 of 3,650) and 58 percent of G4's (805 of 1,390) (`derived`); alpha test, which also forces a late test, is GreaterThan in 112 to 315 draws where present. The effect is that occluded fragments of the depth-tested draws are shaded in full: a cost that falls on the lit, fogged and shadowed draws as much as on the others, and which the estimate of [4.4.3](#443-cost-per-pixel-at-34401440) already includes, since it counts every fragment. Not designed away; a variant that skips the depth write when it provably equals the interpolated depth is not established and is not proposed (`unverified`).

### 5.5 The quaternion fix-up, per device

| Draw | Host | Route |
|---|---|---|
| The geometry-shader draws (CPU draws: `PicaCore::DrawArrays` refuses acceleration, Azahar's `AddTriangle` flags vertices and the `HardwareVertex` constructor negates the quaternion) | every host | **on the CPU**, in vertex assembly (N2-05/N2-06's path), `rhi-spike 5.4`. Nothing in the fragment stage |
| Lit draws shaded by the translated vertex shader | RX 6700 XT, lavapipe (`geometryShader` on) | the **fixed geometry shader** (`triangles` in, `triangle_strip` out, used only with lighting on; Azahar's `PicaFixedGSConfig` in `shader_gen.h`, `GenerateFixedGeometryShader` in `glsl_shader_gen.cpp`, `ShaderDiskCache::UseFixedGeometryShader`): the key's `fixup stage` field is set |
| the same | M2, MoltenVK and KosmicKrisp (`geometryShader` off) | **no fix-up** |
| the same | any host, by fragment barycentrics | **not offered**: the vendored plume does not enable the extension and N2-25 does not patch it |

The route is a pipeline-key field (`fixup stage`), set by the composition when `Needs.fixup_gs` holds and the `geometry_shader` capability value is on; `lighting/` declares the plain `normquat` varying (not Azahar's `pervertex` array, which is the barycentric route). **How many lit draws take the GPU route is `unverified`**: the census has no joint table of lighting against geometry shader. The census does bound it from the other side in two captures: lit draws exceed the non-geometry-shader draws by at least 171 in G2 (297 − 126), 200 in G3 (1,290 − 1,090) and 14 in G9a (455 − 441) (`derived`), so at least that many lit draws are CPU draws there; G4 gives no bound (745 lit, 848 without a geometry shader). **Testing the M2's route on lavapipe is not possible through the mask today**, because N2-26 lists `geometry_shader` among the values, not the bits ([8](#8-for-the-planner), question 1); until then the "no fix-up" key is tested on synthetic values of the profile only.

### 5.6 Samplers

With plume there is no combined image sampler, so the generator emits `texture2D tex0;` and `sampler samp0;` as two bindings and writes each lookup as `texture(sampler2D(tex0, samp0), ...)`, `textureLod(sampler2D(tex1, samp1), ...)`, `textureProj(sampler2D(tex0, samp0), ...)` and `samplerCube(tex0, samp0)` for cube; `textureSize(tex0, 0)` and `texelFetch(tex0, ...)` take the texture alone. In Azahar's GLSL that is the unit-0 to unit-2 lookups in `sampleTexUnit0` to `2` (`glsl_fs_shader_gen.cpp:1716-1750`) and the normal-map lookup (`:510`); `tex_color` and the shadow map use `texelFetch` only. The texel buffers stay as `samplerBuffer` (in Vulkan GLSL a uniform texel buffer, not a combined image sampler: `memory`) bound as plume's formatted buffers. The descriptor layout: set 0 the uniform block(s) and the three texel buffers; set 1 texture and sampler per unit, `tex_color`; set 2 the shadow storage image. Samplers are objects made from the PICA filter and wrap state and cached by value (N2-23); a sampler with the border colour emulated is created with clamp-to-edge ([5.7](#57-border-colours)). No sampler ever sets `compareEnable` ([5.1](#51-shadow-writes-and-the-shadow-lookup)). The lowering is mechanical, about ten sampled-lookup sites (`source`); it is done once, in our generator, with no combined-sampler form anywhere.

### 5.7 Border colours

Chosen by the `custom_border_color` bit (patch 12, N2-27). **Bit on** (RX 6700 XT, lavapipe): a sampler with the unit's custom border colour. **Bit off** (both M2 drivers by default, and lavapipe under `MH4N_RHI_MASK=custom_border_color`): the key's border-emulation bits are set for a unit whose requested wrap is `ClampToBorder` on the s or t axis (Azahar's `TextureConfig::ApplyProfile`, `pica_fs_config.cpp:92-103`, applied at generation time, so the fragment key itself holds only the requested wraps); the shader returns the unit's `tex_border_color` uniform when the coordinate is out of range on that axis, and the sampler is created with clamp-to-edge. `ClampToBorder2` is mapped like `ClampToBorder` by Azahar's samplers but is not handled by its emulation (a TODO in `pica_to_vk.h`); the census counts both without separating them, so **which of the two the 90 and 21 draws use, and what the hardware does with `ClampToBorder2`, is `unverified`**; the design handles `ClampToBorder` and leaves `ClampToBorder2` to the software renderer's reading (`sw_texturing.cpp`), a check for N2-23. In the captures the only border colour seen belongs to the Shadow2D draws, which need none ([5.1](#51-shadow-writes-and-the-shadow-lookup)), so no ordinary texture in the seven captures exercises either path (`inference`).

### 5.8 The shadow write and min/max emulation are GLSL-only in Azahar

`FSConfig::UsesSpirvIncompatibleConfig()` sends ShadowCube, shadow rendering and min/max emulation to the GLSL generator and glslang (`vk_shader_disk_cache.cpp:162-168`; `rhi-spike 5.2`). **If D5 ports Azahar's generators (option 2), both are needed:** the GLSL one for the 94 shadow draws of three captures (and min/max and ShadowCube when they appear), the SPIR-V one for the rest, with `sirit` as a dependency and a second set of semantics (the logic-op switch above is one place they already differ). **Under option 1 the item does not arise**: there is one generator, emitting GLSL for glslang, and the SPIR-V path of Azahar is not ported.

### 5.9 Pipeline counts

Blend state is pipeline state on every device: plume bakes it (there is no dynamic blend state in the Vulkan backend) and MoltenVK cannot make it dynamic whatever the backend does (X8: `extendedDynamicState3ColorBlendEnable`, `ColorBlendEquation`, `ColorWriteMask` and `LogicOpEnable` are false there; 5 of 31 bits). Stencil reference, compare and write masks and the blend constant are dynamic (patches 10 and 07). So **the key and the count of unique keys include the blend state**, and the key holds the depth, stencil, cull and format state too (plume has no extended dynamic state).

**On MoltenVK the count of unique keys is the same as on the other devices** (`inference` from the key definition: the key is built from the requested state and differs by device only through the emulation paths of [4.3](#43-the-pipeline-key-and-how-many-keys-there-are)); what MoltenVK changes is the **cost of each key**: it has no graphics pipeline library (`rhi-spike 9.3`, `VK_EXT_graphics_pipeline_library` absent on both M2 drivers), so a pipeline cannot be linked from precompiled parts, and the vendored plume uses no library on any device anyway, so every key is a full pipeline compile through MoltenVK's SPIR-V to MSL path (`memory`; the time is `unverified`). The only defences are the ubershader and the lists ([4.5](#45-when-the-specialised-pipeline-replaces-it), [4.6](#46-precompiled-pipeline-lists)); `VkPipelineCache` is the plain pipeline-cache blob of N2-28. The number itself is `unverified` ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are), [7](#7-unverified-and-the-census-extension)). What the census does show is how few distinct fixed-function states occur: the `fragment_config` summary has 7 distinct strings in all seven captures, which come from 4 distinct depth-and-stencil combinations for the Default mode (stencil off with depth off; off with LessThanOrEqual and write; off with LessThanOrEqual and no write; stencil Always with depth Always and write), two of the strings differing only in the alpha test (a shader setting), plus the Shadow pass and the Gas mode (`derived` from the strings), with the blend factors unknown.

### 5.10 Uniforms

What the RHI offers (D4): uniform buffers at per-draw offsets over a ring (`Ring::Map(size, align)` and `Commit`), bound by a set update per draw because plume has no dynamic-offset descriptor type (X5; `N2a N2-27` `BindingArena`), texel buffers, a storage image. Azahar's layout is three uniform buffers (vertex data 32 bytes, vertex and geometry data 1,616 bytes, fragment data 1,328 bytes) and three texel buffers over two rings: `texture_lf_buffer` viewed as `R32G32_SFLOAT` (lighting and fog LUTs) and `texture_buffer` viewed as `R32G32_SFLOAT` and `R32G32B32A32_SFLOAT` (procedural noise, colour and alpha maps; colour and difference LUTs) (`source: azahar@5f3306d shader_uniforms.h`, `vk_rasterizer.cpp:88-110, 955-1030`).

**The design:**

- **One fragment uniform buffer, one binding.** `FragmentUniforms` (Azahar's `FSUniformData` layout, 1,328 bytes, copied with provenance) followed by the `cfg` block (the 272-byte key; written only when the ubershader may draw this key, which is every draw before its specialised pipeline is ready) in one ring allocation, aligned to **256 bytes**, the largest alignment Vulkan allows for the offset (`memory`; the devices read so far give 16 and 64, `rhi-spike 3.3`, and the RX 6700 XT's is `unverified`). The vertex-side buffers are N2-06's; the composition passes the two buffers as two bindings in one set update.
- **Rewritten only when dirty.** Azahar's `fs_data_dirty` becomes the PicaState dirty bits the fields depend on; a draw whose fragment state did not change reuses the previous allocation and binding set. Whether that happens often is `unverified` (the census has draws, not state changes between them).
- **Tables in two rings, three views** as Azahar: `lut_lf` (lighting and fog), `lut_rg` and `lut_rgba` (procedural). The offsets of each table inside its buffer live in the uniform block (`lighting_lut_offset`, `fog_lut_offset`, `proctex_*_offset`), so a table upload does not rebuild a binding. Tables are uploaded when `PicaState.lighting.lut_dirty`, `fog.lut_dirty` or `proctex.table_dirty` say so, on the thread that owns the state (N2-22).
- **Per-draw bindings** (textures, samplers, `tex_color`, the storage image) come from the `BindingArena` of N2-27, one set per draw as Azahar; the count of descriptor writes is what [5.6](#56-samplers) lists.
- **Texel buffer size:** the smallest `maxTexelBufferElements` read is 67,108,864 (MoltenVK, `stand-in`, `rhi-spike` section 4, G5), against Azahar's tables of a few thousand entries: no constraint.

### 5.11 Gas stays open

Gas is drawn by nobody (`rhi-spike 5.5`): Azahar's two generators log "Unimplemented gas mode" and write transparent black, the GLSL one returning without writing depth. Seen: G4 only, 20 draws (1.44 percent of its draws, all ten frames): 10 draws with fog mode Gas and 10 with fragment operation mode Gas (`census`). This design **does not decide** the formula (N2-10 does, from 3dbrew's gas registers and a real-3DS check later) and leaves room:

- The key holds the fog mode Gas and the fragment operation mode Gas; the draw is a normal pass kind unless N2-10 says otherwise.
- `Needs` carries `ordered_rmw` and `dst_copy`, so N2-10 can ask for an ordered read-modify-write of the colour target (the `interlock` bit with a storage-image pass, or the feedback copy) without a change to this interface. `rhi.h` is frozen, so any new RHI operation N2-10 needs is a `needs-decision`.
- Until N2-10, a gas draw is counted `stubbed:gas` and draws Azahar's behaviour (transparent black); N2-20's thresholds exclude gas frames (`--skip-gas-frames`). The 16 gas registers are not named in Azahar's headers, but `PicaState.regs` holds all raw registers, so N2-10 reads them from there.

## 6. What changes under options 2 and 3

**Under option 2** (port Azahar's generators as they are): [4.2](#42-where-each-piece-of-state-goes) and [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) stay almost as they are (Azahar's `FSConfig` is the key and its `StaticPipelineInfo` the pipeline key). [4.4](#44-the-ubershader) becomes a hand-written ubershader beside the generators, with the equality tests between them that the two-mode emitter would have made free. [4.8](#48-the-feature-interface) changes most: Azahar's generators are two classes, so the feature directories hold nothing and N2-09 to N2-12 and N2-23 edit shared files; the planner would either split the ported classes first or sequence the five packages. [5.6](#56-samplers) is done twice (GLSL and `sirit`). [5.8](#58-the-shadow-write-and-minmax-emulation-are-glsl-only-in-azahar) is a standing fact: draws with a shadow pass, min/max or ShadowCube take the GLSL path (94 draws in the seven captures), the others the SPIR-V path with 4 of 16 logic ops until N2-13 adds the rest by hand. Gas, all 16 logic ops, the ubershader and the lists are ours in any case. The rest ([4.5](#45-when-the-specialised-pipeline-replaces-it), [4.6](#46-precompiled-pipeline-lists), [5](#5-paths-the-rhi-lacks-what-d4-fixed)) is unchanged.

**Under option 3** (ubershader only): `FragmentKey` shrinks to the configuration block (there is no specialised code to key); the pipeline key keeps the fixed-function state, the pass kind and the vertex stage, so [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) counts pipelines by fixed-function state and vertex layout only (a smaller number, `unverified`: the census's 4 depth-and-stencil combinations times the vertex layouts times the blend factors in use). [4.5](#45-when-the-specialised-pipeline-replaces-it) disappears; [4.6](#46-precompiled-pipeline-lists) stays for the fixed-function pipelines (the ubershader is large, so each costs more; `unverified`). [4.8](#48-the-feature-interface) keeps its table with "generated code" replaced by "code of the block". The cost question of [4.4.3](#443-cost-per-pixel-at-34401440) becomes the whole decision, and the M2 column of that table is the risk. N2-16 shrinks and N2-08 shrinks; N2-09 to N2-12 and N2-23 are unchanged in shape.

## 7. Unverified and the census extension

**What is `unverified` in this document, and what would check it:**

| Item | Check |
|---|---|
| Unique pipeline keys in the captures ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are)) | the census extension below, run on the PC |
| Unique fragment keys across the seven, and in a play session | the same; a play-session count needs longer captures than 3 and 10 frames (`rhi-spike 2`, last list) |
| Blend factors in use; cull mode; wrap modes per unit; `ClampToBorder` against `ClampToBorder2`; scissor mode; W-buffering | the same (fields not decoded by the census today) |
| How many lit draws use the hardware vertex shader | a joint table: lighting on against geometry shader on |
| The procedural-texture configuration count ("one" in D5) | a `proctex_config` group in the census |
| The ubershader's cost, compile time, and the specialised/uber ratio | N2-16 counters and a forced-ubershader switch; N2-19 on the RX 6700 XT and the M2 |
| Fragments per output pixel (overdraw) | a GPU measurement (N2-19); the census cannot give it |
| Whether the compare-and-swap shadow loop differs visibly from the interlock path | N2-12 on the Arena scene on both paths (D95) |
| Whether "proven on MH4U" holds for Azahar's renderer | Azahar's output on the captured scenes |
| RT64's ubershader and pipeline-list implementation | only its README line was read (`https://github.com/rt64/rt64`, retrieved 2026-10-24: "Uses ubershaders to guarantee no stutters due to pipeline compilation"); its source was not |
| Logic-op results on narrow colour formats | none seen in the census; N2-13 |
| Driver compile time of a pipeline per device, and `minUniformBufferOffsetAlignment` on the RX 6700 XT | the maintainer's run of N2-30's `--rhi` report |

**The census extension that produces the number of unique pipeline keys.** `tools/native/pica_census.py` replays each capture into a register file and decodes `decode_state` per draw; its groups (`group_keys`) are per label, so no group is a key and none is a union across labels. To get the count, a small package (proposed, size S, with the tool's usual synthetic test in `tests/test_native_pica_census.py`) adds to the tool:

1. In `decode_state`: the blend factors (`factor_source_rgb`, `factor_dest_rgb`, `factor_source_a`, `factor_dest_a`), the colour write mask bits, the cull mode, the scissor mode, `depthmap_enable`, the wrap modes of each unit separately, `fog_flip`, `texture2_use_coord1`, the shadow orthographic flag, the procedural-texture fields of Azahar's `ProcTexConfig`, and the lighting fields of `LightConfig` (per-light flags, LUT input, absolute flag, scale), all as Azahar's `FSConfig::FSConfig(regs)` reads them (`pica_fs_config.cpp`).
2. In `group_keys`: three new groups, `fs_key` (Azahar's requested `FSConfig` fields only), `pipeline_key` (the design's `PipelineKey`: `fs_key`, the vertex program hash and layout, the fixed-function state, the pass kind, the logical colour and depth formats) and `proctex_config`; plus a joint table of lighting on against geometry shader on.
3. A **union across labels**: a new `--union` option that, after the per-label analysis, counts the groups above over all labels together (the same register-value tuples, de-duplicated across captures) and prints the count and the per-label contribution.

The command, on the PC with the seven captures staged under `build/golden/` (the paths of `docs/native/golden.md`):

```
python3 tools/native/pica_census.py \
    'G1=build/golden/G1/gpu-*.mgpu' 'G2=build/golden/G2/gpu-*.mgpu' \
    'G3=build/golden/G3/gpu-*.mgpu' 'G4=build/golden/G4/gpu-*.mgpu' \
    'G9a=build/golden/G9a/gpu-*.mgpu' 'G9b=build/golden/G9b/gpu-*.mgpu' 'G9c=build/golden/G9c/gpu-*.mgpu' \
    --union --json build/pica-census.json --md docs/research/pica-census.md
```

(`--union` is the proposed option; the rest is the tool's own documented form.) The output holds counts, register values, enumerator names and hashes only, as the census does, so it is safe to commit; the captures stay on the PC.

## 8. For the planner

1. **`geometry_shader` is not a maskable bit.** `N2a N2-26` lists it with the values (`geometry_shader`, `stride_alignment`, ...), and the mask "only turns bits off" (D4 item 2's list has no geometry-shader entry). The M2's "no fix-up" route ([5.5](#55-the-quaternion-fix-up-per-device)) therefore cannot be run on lavapipe through the mask, against the *Spec* of N2-07 ("each path chosen by a bit ... so lavapipe runs every one through the mask"). Proposed: make `geometry_shader` a bit with the mask name `geometry_shader` (a change to the frozen `rhi.h` and N2-26's *Done when*, so the orchestrator's).
2. **N2-16's "never skipped" and the unseen fixed-function state.** [4.5](#45-when-the-specialised-pipeline-replaces-it): the ubershader pipeline for a state never seen does not exist and plume bakes the state. Proposed rule: build it synchronously once, count it, list it. If the planner prefers the other rule (skip the draw), N2-16's *Spec* sentence stands and a glitch frame is accepted.
3. **A switch for the measurement.** The recommendation's measurement ([3.3](#33-recommendation) point 4) needs `--ubershader-only` beside N2-16's `--no-ubershader`, plus a per-frame GPU time with it forced (N2-28's `FrameGpuTimeNs` exists).
4. **The census extension** of [7](#7-unverified-and-the-census-extension) is a small package that does not exist in `STATUS.md`; it needs a number and a place before N2-16's maintainer check.
5. **Two sentences of D5's evidence differ from the census** (`DECISIONS.md`, D5, "2026-10-23, evidence"): it says fog in "826 draws of the Arena fight", the census says 800 for G3 (21.92 percent); and "one procedural-texture configuration", which the census does not record. Not edited here (shared file).
6. **`fbops/` output-stage code is consumed by the generator in `fragment/`.** N2-13's allowlist is `fbops/` and `backend/state/`, N2-08's is `fragment/`; [4.7](#47-the-interface-between-the-command-processor-and-the-generator) puts the hook between them. If the planner prefers that N2-13 write these snippets inside `fragment/`, N2-13's allowlist needs `fragment/output/**`.
7. **Reference to the HD pack's normal map** (`UserConfig.use_custom_normal`, outside Azahar's cache): a `user` word is reserved in the key so N2-17 and N2-18 need not change it.

## 9. Coverage of the N2-07 *Spec*

| Spec bullet (`N2a N2-07`) | Section |
|---|---|
| which state goes into specialisation constants, uniforms, separate pipelines | [4.2](#42-where-each-piece-of-state-goes) |
| the pipeline key, and how many unique keys the captures have | [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) (the count is `unverified`; the bounds that can be derived; the command in [7](#7-unverified-and-the-census-extension)) |
| the ubershader: coverage (TEV, sampling, lighting, fog and gas, procedural textures, shadows), how it reads the state, cost per pixel at 3440×1440, when the specialised pipeline replaces it | [4.4.1](#441-one-source-two-modes), [4.4.2](#442-what-it-covers-and-how-it-reads-the-state), [4.4.3](#443-cost-per-pixel-at-34401440), [4.5](#45-when-the-specialised-pipeline-replaces-it) |
| precompiled pipeline lists: what they record, where they live, how they are built from captures and play | [4.6](#46-precompiled-pipeline-lists) |
| the interface between `DrawCall` and the fragment generator | [4.1](#41-data-flow), [4.7](#47-the-interface-between-the-command-processor-and-the-generator) |
| the feature interface (`sampling/`, `lighting/`, `fog/`, `proctex/`, `shadow/`): key, generated code, ubershader, uniforms | [4.8](#48-the-feature-interface) |
| logic ops, shadows, gas and any feature the RHI lacks, each path chosen by a capability bit | [5](#5-paths-the-rhi-lacks-what-d4-fixed) (the table at its head; the three rows that are not a plain bit) |
| D4: shadow writes (zero-attachment pass, `interlock`, compare-and-swap, texel fetch lookup, out of range lit) | [5.1](#51-shadow-writes-and-the-shadow-lookup) (`rhi-spike 5.2, 5.3`) |
| D4: logic ops (hardware with the `logic_op` bit and blending off; all 16 by emulation; the feedback copy) | [5.2](#52-logic-ops) (`rhi-spike 5.6, 9.4, 9.5`) |
| D4: min/max blending (hardware only when both factors are One; else the feedback copy) | [5.3](#53-minmax-blending) |
| D4: depth (the shader writes it; no early depth test) | [5.4](#54-depth) (`rhi-spike` X2) |
| D4: the quaternion fix-up per device | [5.5](#55-the-quaternion-fix-up-per-device) (`rhi-spike 5.4`) |
| D4: samplers (separate texture and sampler) | [5.6](#56-samplers) (`rhi-spike 6.3`) |
| D4: border colours (the `custom_border_color` bit; shader emulation and clamp-to-edge; which state selects it) | [5.7](#57-border-colours) (`rhi-spike 9.3`) |
| D4: the shadow write and min/max emulation are GLSL-only; option 2 needs both generators | [5.8](#58-the-shadow-write-and-minmax-emulation-are-glsl-only-in-azahar) (`rhi-spike 5.2`) |
| D4: pipeline counts (blend is pipeline state; the count on MoltenVK) | [5.9](#59-pipeline-counts) (`rhi-spike 9.3`, X8) |
| D4: uniforms (per-draw offsets over a ring, texel buffers; Azahar's layout) | [5.10](#510-uniforms) (`rhi-spike 5.7`, X5) |
| D4: gas stays open | [5.11](#511-gas-stays-open) (`rhi-spike 5.5`) |
| D5's three options, the evidence, the consequences for the waiting packages, a recommendation | [3](#3-the-three-options-of-d5), with [6](#6-what-changes-under-options-2-and-3) |
| `rhi-spike` sections named by the *Read first* (2, 5.2 to 5.7, 6.3, 9.3, 9.4) | cited in [2.2](#22-what-d4-fixed) and [5](#5-paths-the-rhi-lacks-what-d4-fixed); 2 in [2.2](#22-what-d4-fixed) and [7](#7-unverified-and-the-census-extension), 5.2 to 5.7 in 5.1 to 5.6 and 5.10, 6.3 in 5.6, 9.3 in 2.2 and 5.9, 9.4 in 2.2 and 4.7 |

## 10. Where to copy from (D6)

Copying is not part of this package. The implementing packages copy individual functions and tables from Azahar at `5f3306d` with the header D6 requires, opening each file to read its own licence line (some say GPLv3 or later, some GPLv2 or later; `native-wp` skill, "Rules that are never bent"):

| Our piece | Azahar source (under `src/video_core/`) |
|---|---|
| `FragmentKey`, `MakeKey`, the profile rules | `shader/generator/pica_fs_config.h`, `.cpp` (`FSConfig`, `FramebufferConfig`, `TextureConfig`, `LightConfig`, `ProcTexConfig`, `ApplyProfile`, `StructHash`), `shader/generator/profile.h` |
| Generator stages (prologue, depth, scissor, TEV, alpha test, output) | `shader/generator/glsl_fs_shader_gen.cpp` (`FragmentModule::Generate`, `WriteDepth`, `WriteScissor`, `WriteTevStage`, `GetSource`, `WriteAlphaTestCondition`, `DefineInterface`, `DefineBindingsVK`) |
| `lighting/` | the same file (`WriteLighting`, `DefineLightingHelpers`), `shader/generator/glsl_shader_gen.cpp` (the fixed geometry shader), `renderer_software/sw_lighting.cpp` (reference) |
| `fog/` | `glsl_fs_shader_gen.cpp` (`WriteFog`, `WriteGas`: a stub only), `renderer_software/sw_rasterizer.cpp` |
| `proctex/` | `glsl_fs_shader_gen.cpp` (`DefineProcTexSampler`, `AppendProcTex*`), `renderer_software/sw_proctex.cpp` |
| `shadow/` | `glsl_fs_shader_gen.cpp` (`WriteShadow`, `DefineShadowHelpers`, the lookups at `:1460-1498`), `renderer_software/sw_framebuffer.cpp` |
| `sampling/` | `glsl_fs_shader_gen.cpp` (`DefineTexUnitSampler`, `sampleTexUnit*`), `renderer_vulkan/pica_to_vk.h` (wrap and filter tables), `renderer_software/sw_texturing.cpp` |
| output (`fbops/`) | `glsl_fs_shader_gen.cpp` (`WriteLogicOp`, `WriteBlending`), `spv_fs_shader_gen.cpp` (the logic-op switch, for the cases only), `renderer_vulkan/vk_graphics_pipeline.cpp` (`GetFinalColorWriteMask`) |
| `FragmentUniforms` | `shader/generator/shader_uniforms.h` (`FSUniformData`), `renderer_vulkan/vk_rasterizer.cpp` (`UploadUniforms`, the LUT syncs) |
| the lists and their header | `renderer_vulkan/vk_shader_disk_cache.cpp` (`CacheEntryType`, the `FILE_INFO` struct-hash check), `vk_graphics_pipeline.h` (`StaticPipelineInfo`, `BlendingState`, `DepthStencilState`, `RasterizationState`) |
