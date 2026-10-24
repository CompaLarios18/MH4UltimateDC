# GPU fragment pipeline: the design for D5

## 0. Header

- **Status:** draft for review (N2-07). **D5 is `Open`; the maintainer decides it.** This document does not decide it and nothing in it is settled. It lays out the three options D5 lists and two variants of them ([3](#3-the-three-options-of-d5)), the evidence for and against each, what each does to the work packages that wait on it, and a **recommendation** (marked as one, [3.3](#33-recommendation)). Sections 4 and 5 then design the recommended family (option 1, or its variant 1b) in enough detail for the implementing packages to start from it, and [6](#6-what-changes-under-the-other-options) says what changes under 1b, 2', 2 and 3. Wherever the text says "the design", read "the design if D5 goes to the option-1 family". Proposed edits to shared files are in [8](#8-for-the-planner); none has been made.
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

**Recommendation (not a decision; D5 stays `Open`; it changed in review round 1).** The option-1 family of D5, ubershader first: our own key and generator modules, one module per feature, copying Azahar's state key (`FSConfig`) and generator semantics with provenance (D6), glslang as D27's tool, and an ubershader that the specialised shader is derived from, not written beside. Two ways of deriving it are on the table ([3.1](#31-the-options-and-the-evidence), [4.4.1](#441-one-source-two-modes)): **option 1**, a two-mode emitter in the generator, and **option 1b**, the ubershader written once against the configuration block with the specialised shader obtained by making the block's fields specialisation constants, built offline. The recommendation is **1b if a small spike shows that the drivers compile and fold it well, else 1**; everything else in this document is the same for both. Reasons, short ([3.3](#33-recommendation) has them with the numbers): (a) an ubershader is needed whichever option wins, because ROADMAP principle 5 asks for no visible stutter and Azahar has none; (b) option 2 is cheaper than D5's text says, since Azahar's SPIR-V generator is a user setting and the GLSL generator alone covers shadows, min/max and ShadowCube (variant 2'), but it leaves a hand-written ubershader as a second implementation to keep equal to the ported generator; (c) option 3 is a bet on a per-pixel cost nobody has measured: the `estimate` of [4.4.3](#443-cost-per-pixel-at-34401440) does not refute it on the PC at two fragments per pixel and does at four or with heavy draws; (d) building the ubershader first makes option 3 a measurable checkpoint, possible once N2-20 passes if N2-08 ships a force switch and a default pipeline provider ([4.7](#47-the-interface-between-the-command-processor-and-the-generator)), and otherwise once N2-16's `--ubershader-only` exists.

**The design in one paragraph.** A draw's fragment state is read from the command processor's `PicaState` into a `FragmentKey` (Azahar's `FSConfig` in a canonical, requested form, versioned by a struct hash, [4.3](#43-the-pipeline-key-and-how-many-keys-there-are)) and a `FragmentUniforms` block (Azahar's `FSUniformData`, 1,328 bytes). Fixed-function state (blend, depth, stencil, cull, formats, vertex layout) is not in the fragment key; it goes into the pipeline key as `rhi.h`'s `FixedFunctionState` whole, because the vendored plume bakes it on every device (D4, `rhi-spike` X8). Stencil reference, stencil masks and the blend constant are dynamic. The pipeline key is `(pass kind, vertex stage, fix-up stage, fragment key or ubershader, fixed-function state, logical formats)`. Specialisation constants are the mechanism of option 1b and otherwise unused. Each of the five feature directories contributes key fields, code, uniforms and requirements through one small interface ([4.8](#48-the-feature-interface)), and N2-08 builds the composition generically so that the later packages only fill their directories. The paths the RHI lacks are chosen by capability bits of N2-26 ([5](#5-paths-the-rhi-lacks-what-d4-fixed)).

**Numbers this document could not produce** (marked `unverified`; [7](#7-unverified-and-the-census-extension) says how to get them): how many unique pipeline keys the captures have; the ubershader's measured cost and, for 1b, the drivers' compile time and folded code quality; how many lit draws use the hardware vertex shader (the only ones that need a GPU quaternion fix-up); the blend factors in use. **Proposed edits to shared files** are listed once, each marked APPLY NOW or AFTER D5, in [8](#8-for-the-planner); none has been made, and no package may start on the strength of this document before D5 is decided.

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
| The capability structure and its mask (`MH4N_RHI_MASK`), filled from device queries. | `rhi-spike` 9.4 item 2; `N2a N2-26` | the device-dependent paths below are chosen by one of its bits ([5](#5-paths-the-rhi-lacks-what-d4-fixed)) |

Device facts that the paths depend on (`rhi-spike` 3.1 to 3.3 and 9.3): the RX 6700 XT has pixel and sample interlock, geometry shaders, `logicOp`, custom border colours and barycentrics, but no `D24_UNORM_S8_UINT` and no `present_wait`; the M2 under MoltenVK 1.4.2 has interlock but no `logicOp`, no geometry shaders, no custom border colours and no pipeline library; under KosmicKrisp it has `logicOp` but no interlock (so only the compare-and-swap shadow write); lavapipe reports more than the PC and must run every fallback through the mask. All M2 entries are `stand-in` until the maintainer's Mac run of `mh4n_vkprobe`.

### 2.3 What Azahar's generators are

`source: azahar@5f3306d`; `wc -l` of each file (counted here, `source`; not the code-line counter of `rhi-spike 5.9`):

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
- **The SPIR-V generator is a setting.** `Settings::values.spirv_shader_gen` (`common/settings.h:536`, default true) feeds `Profile.vk_use_spirv_generator` (`vk_pipeline_cache.cpp`); the choice is `use_spirv && !fs_config.UsesSpirvIncompatibleConfig()` (`vk_shader_disk_cache.cpp:162-163`). With the setting off every fragment shader is generated as GLSL and compiled with glslang, so the GLSL generator alone is a complete renderer ([3.1](#31-the-options-and-the-evidence), option 2').
- **That the renderer is "proven on MH4U" is D5's claim and is `unverified` here** (`rhi-spike 5`): nothing in the code shows it; Azahar's output on these scenes would.

## 3. The three options of D5

### 3.1 The options and the evidence

D5's text lists three options. Review round 1 added two variants that the evidence supports: **1b**, a single-source form of option 1, and **2'**, option 2 without Azahar's SPIR-V generator. What is common to the option-1 family (key, pipeline key, feature directories, lists, `Needs`) is in sections 4 and 5; 1 and 1b differ only in how the specialised shader is obtained.

| | Option 1 | Option 1b | Option 2' | Option 2 (D5's text) | Option 3 |
|---|---|---|---|---|---|
| **What it is** | Our own key and generator, a module per feature; the ubershader and the specialised shader are emitted from one C++ generator by a **two-mode emitter** (literals, or reads of the configuration block); glslang at run time for each specialised shader | The same key and modules; the ubershader GLSL is written **once** against the block's fields; the specialised shader is the same SPIR-V with those fields as specialisation constants, built offline (no glslang at run time for fragments); rhi.h must carry specialisation constants | Port `glsl_fs_shader_gen.cpp` (1,774 lines) with `pica_fs_config.*` and `shader_uniforms.h` (2,664 lines in all), lowered for separate samplers, with Azahar's SPIR-V generator off (the setting `spirv_shader_gen`, `common/settings.h:536`, default true, read at `vk_shader_disk_cache.cpp:162-163`: with it off every fragment shader goes through the GLSL generator and glslang); a hand-written ubershader beside it | 2' plus Azahar's SPIR-V generator (`spv_fs_shader_gen.cpp`, 1,631 lines, `sirit`) as the fast path for draws that are not shadow, min/max or ShadowCube | One big fragment shader reading the whole state from a block; no key, no specialised shader |
| **For** | Features in separate directories fit the parallel packages N2-09 to N2-12 and N2-23. One generator. All 16 logic ops, gas hooks and separate samplers designed in. One source for both shaders | As option 1, and no emitter (the main risk of option 1); features written once; no glslang at run time for fragments; going to option 3 is a change of constants, not a rewrite | The least new code of the generator family; Azahar's semantics; the one GLSL file covers shadows, min/max and ShadowCube. The feature methods of `FragmentModule` can be defined in each directory's own `.cpp` (C++ allows member definitions in separate files), so parallel packages are not blocked (`inference`) | As 2', and the SPIR-V generator compiles faster (no glslang) for most draws | No compile stutter from fragment shaders by construction; no key; the least code; one shader to debug |
| **Against** | The most design work; the two modes are different code in each helper, so drift is caught only by per-feature equality tests; glslang runs at run time for every specialised shader (milliseconds: N2-06 *Traps*; Azahar's SPIR-V generator exists to avoid it), so every miss stays on the ubershader longer | Driver compile time per pipeline of a large module with about 70 constants (the 272-byte `FSConfig` plus the user and gas words, 280 bytes), and whether drivers fold the constants into code as tight as literals (switches, the light loop), are both `unverified`; a change to N2-26's pipeline description (section 8); how MoltenVK and KosmicKrisp fold them is `unverified` | A hand-written ubershader is a second implementation to keep equal to the ported generator; `WriteLogicOp` has 5 case labels (Clear, Set, Copy, CopyInverted, NoOp; CopyInverted writes `~color` on a float vector, which does not look valid GLSL) and logs "Unhandled logic_op" for the rest; no gas; glslang at run time; about ten sampled lookups to lower | As 2', and a second generator with other semantics (its logic-op switch differs), the `sirit` dependency, 1,631 + 298 more lines to port, 4 of 16 logic ops | Per-pixel cost unmeasured against D19's 6 ms. The estimate does not refute it at k = 2 on the PC (rows (a) and (b) of [4.4.3](#443-cost-per-pixel-at-34401440) are 30% and 72% of the budget) but does at k = 4 or with draws like row (c), and it does not model register pressure. One large module is compiled into every pipeline |
| **Rough tokens (`inference`, ±50%, the unit of D4's evidence; an L package is about 1M)** | N2-08 about 1.4M to 1.9M: key, emitter, ubershader, TEV, stubs, generic composition and `--gpu native` do not fit one L; a split (08a key, uniforms, composition; 08b emitter, generator, ubershader, TEV, `--gpu native`) is recommended to the planner (`inference` from the token estimate; the split is not this document's call). Each of N2-09 to N2-12 and N2-23 also writes and tests both modes: +10% to +20% each | N2-08 about 1.1M to 1.5M (borderline L; the 08a/08b split of the composition is still advised). Features written once. N2-16 +0.1M for the constants plumbing | N2-08 about 1.0M to 1.4M (port, lower, stubs, hand ubershader). Features edit member functions in their own files | 2' +0.4M to +0.6M | N2-08 about 0.7M to 1.0M; N2-16 about 0.2M to 0.3M smaller |

**Against, under every option.** The fallback pipeline is not one per fixed-function state. The `PipelineKey` contains the vertex stage ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are)), so the ubershader pipeline exists per `(vertex stage, fix-up stage, fixed-function state, attachments, pass kind)`, and a new vertex program costs a glslang compile at its first use (D27; N2-06 *Traps*) as well as a pipeline compile. How [4.5](#45-when-the-specialised-pipeline-replaces-it) deals with it is the same under 1, 1b, 2', 2 and 3.

**How early the choice can be measured.** A forced-ubershader timing of G3 on the PC is possible once N2-20 passes (N2-20 renders the captures end to end and runs before N2-16), **provided N2-08 ships** a default `PipelineProvider` (a synchronous build on a miss, no list), a debug option `--force-ubershader` of its `--gpu native`, and the per-frame GPU time in the report ([8](#8-for-the-planner) (i)(2) and (4)); without those, the timing waits for N2-16's `--ubershader-only` (after N2-16 and N2-15); the M2 timing needs the Mac and N2-19. And the maintainer can say from his own play in Azahar whether "proven on MH4U" holds for the ported renderer, which no document here can settle.

### 3.2 Consequences for the waiting packages

| Package | Options 1 and 1b | Options 2' and 2 | Option 3 |
|---|---|---|---|
| **N2-08** (generator, TEV, alpha test, `--gpu native`; L) | builds `FragmentKey`, the generic composition slots ([4.1](#41-data-flow)), the generator (1: with the emitter; 1b: the single-source ubershader and the constants), the core stages, TEV, the ubershader, stubs for the five features and the prototype block of [4.8](#48-the-feature-interface); too big for one L, see [3.1](#31-the-options-and-the-evidence) | ports the GLSL generator (and for option 2 the SPIR-V one), lowers samplers, adds a hand-written ubershader and stubs; the *Spec*'s feature-directory structure is met by member definitions in separate files | builds the block layout, the ubershader and stubs; no key beyond the block |
| **N2-13** (framebuffer operations) | writes the output-stage GLSL functions of [4.7](#47-the-interface-between-the-command-processor-and-the-generator) in `fbops/` and tests them in its own shaders | the same functions, called from the ported generator (and, for option 2, from the SPIR-V one by hand in `sirit`) | the same functions, called in the block's branch |
| **N2-16** (pipeline cache, ubershader, background compilation; L) | as specified, with the changes of [8](#8-for-the-planner) (e); 1b adds the constants plumbing | as specified, but the ubershader is a hand-written second implementation | no specialised path: the cache and the list cover fixed-function pipelines only; its "switch when ready" loses its meaning |
| **N2-20** (integration on the captures) | a fix must hold in both modes; the equality tests of each feature (proposed in [8](#8-for-the-planner) (g)) localise a divergence | fixes land in the ported generator and in the ubershader | one shader path to fix |
| **N2-09** lighting, **N2-10** fog and gas, **N2-11** procedural textures, **N2-12** shadows, **N2-23** sampling | each writes its feature against [4.8](#48-the-feature-interface) in parallel after N2-23 (N2-10 after N2-08); under 1 each also writes the second mode | each defines its member functions of `FragmentModule` in its own directory | each writes its feature as a branch of the block's code |
| **N2-15** (surface cache) | unchanged | unchanged | unchanged |

### 3.3 Recommendation

**Recommendation (for the maintainer; D5 stays `Open`): the option-1 family, ubershader first; option 1b if a spike passes, else option 1.**

1. **An ubershader is needed under every option.** Principle 5 of the roadmap ("pipelines are compiled ahead of time and cached on disk, and an ubershader draws while a new one compiles") has no counterpart in Azahar. Options 2' and 2 port a generator and then add a hand-written ubershader: a second semantic implementation that has to be kept equal to the ported one. Option 1 generates both from one source and relies on per-feature equality tests to catch drift; option 1b has one source of GLSL.
2. **Option 2 is cheaper than D5's text says, and 2' is a legitimate choice.** Azahar's SPIR-V generator is a setting; with it off, the 1,774-line GLSL generator alone draws shadows (3 of 7 captures, 94 draws), min/max blending and ShadowCube. What stays against 2' is the second implementation, glslang at run time, 5 logic-op case labels with one invalid, no gas, and the sampler lowering. If the maintainer values the least new code over a structure of his own, 2' is the option; the cost is in N2-16 and in keeping two shaders equal.
3. **Option 3 is not refuted by the estimate, and is a bet on a number nobody has.** [4.4.3](#443-cost-per-pixel-at-34401440): on the PC at two fragments per pixel an ubershader as the only shader takes 30% and 72% of the 6 ms for rows (a) and (b); at four it takes 59% and 144%, and row (c) fails at either. The estimate leaves out register pressure and occupancy (the ubershader's register allocation is sized for its worst path and hurts every draw it shades; the ±2× range does not bound that), and the geometry-shader sprite pass (D5's term; Point-mode geometry-shader draws are 38.99 to 98.30 percent of the draws, blended like every draw) can push the number of fragments per pixel well above four locally (`inference`). So the risk sits on the PC for lit and overdraw-heavy frames, not on the M2, whose own target is 60 fps at 3x.
4. **1b against 1.** 1b removes the two-mode emitter (the one new idea of option 1 and its main risk) and the run-time glslang of fragments, and it makes option 3 a change of constants. Its costs are `unverified` and cheap to measure: the driver's compile time per pipeline for the large module, and whether the folded code is as tight as Azahar's literals (switch on TEV sources, the light loop). A spike (proposed, size S, not an existing package; defined in [8](#8-for-the-planner) (j)) compares the same keys built as 1b (one SPIR-V with about 70 constants) and as option 1 (literal GLSL, glslang, pipeline creation) on lavapipe and the RX 6700 XT: pipeline-creation time, and instruction and register counts of the folded code where the driver reports them. If it passes its proposed thresholds, 1b; if not, option 1. N2-26's frozen pipeline description does not carry specialisation constants today, so 1b needs a spec edit there first ([8](#8-for-the-planner) (a)).
5. **The ubershader-first order turns the choice between the family and option 3 into a measurement.** The forced-ubershader timing of G3 on the PC after N2-20 (with N2-08's force switch, [3.1](#31-the-options-and-the-evidence)), then N2-19's runs on both machines; if the ubershader is cheap enough the specialised path (and the part of N2-16 that switches) is not built.
6. **What would change this recommendation:** a spike that fails (1b to 1); a measurement of the ubershader inside the budget at k = 4 (option 3); the maintainer preferring the least new code (2'); or the emitter turning out harder than [4.4.1](#441-one-source-two-modes) says and the spike failing too (then 2', whose shape is a ported generator plus a hand-written ubershader).

What each choice leaves for the maintainer to weigh that no document here can settle: Azahar's unmodified semantics (2', 2) against parallel, tested feature packages (1, 1b); and whether he accepts that the first run on a fresh install runs on the ubershader until the list is warm (true under every option that has one; [4.5](#45-when-the-specialised-pipeline-replaces-it)).

## 4. The design (the option-1 family)

### 4.1 Data flow

```
GSP command lists ──> N2-04 command processor ──> pica::Backend::Draw(const DrawCall&)
                                                       │  DrawCall.state -> const PicaState*
                                                       ▼
 native/ (N2-08, the composition)   fragment::MakeKey(PicaState)     ──> FragmentKey      (requested, pre-profile)
                                    fragment::FillUniforms(...)      ──> FragmentUniforms (1,328 bytes) + LUT texel buffers
                                    fbops (N2-13): requested FixedFunctionState ──> blend, depth, stencil, cull, colour write mask
                                    N2-05/06: vertex layout, vertex program id, the 7 varyings
                                                       │
                                       PipelineKey = { pass kind, vertex stage, fix-up stage, fragment key id,
                                                       fixed-function state, logical formats }  (4.3)
                                                       ▼
 N2-16 pipeline cache:  key known and pipeline ready? ──yes──> specialised pipeline
                                  │no: queue compile on a worker thread
                                  ▼
                       fallback (ubershader) pipeline, 4.5 ──> draw now
```

The command processor hands over `const PicaState*` that is valid only during `Draw`; the dirty flags in it mean "changed since the previous draw returned" (`native/src/gpu/pica/backend.h`, `state.h`; `N2a N2-04` *As built*). The fragment extraction therefore runs on the thread that owns the processor (or on a copy of the roughly 105 KB `PicaState`, as N2-22 decides), re-extracts the key only when a dirty register of the fragment blocks is set, and hands the renderer thread a small `FragmentKey` and a uniform block. The 7 varyings between the vertex and the fragment stage are Azahar's fixed ones (`primary_color`, `texcoord0` to `texcoord2`, `texcoord0_w`, `normquat`, `view`, fixed locations; `glsl_fs_shader_gen.cpp:1267` `DefineInterface`); **core declares all seven** (N2-08, at the declarations point of [4.8](#48-the-feature-interface)), not `lighting/`, and the vertex stage (N2-06) writes all seven, zero where the program leaves one unwritten, as Azahar's does. N2-06's *Spec* does not mention this interface (its cache key has only an output map), so it is a proposed edit in [8](#8-for-the-planner) (h).

**The composition is built generically by N2-08.** The allowlists of N2-09 and N2-12 do not include `native/` (nor `pipeline/`), so every slot the later packages need is made by N2-08 and only filled by them: the pass kind (normal, or shadow write with no attachments and a storage image), the fix-up stage wiring ([5.5](#55-the-quaternion-fix-up-per-device)), the destination copy before a draw (`dst_copy`), the texel-buffer and storage-image bindings, and the generation of the reserved gas space ([5.11](#511-gas-stays-open)). A stub feature leaves its slot neutral.

### 4.2 Where each piece of state goes

Classes: **P** pipeline key (baked into the plume pipeline); **D** dynamic (set on the command list); **G** generated: a literal in the specialised shader, a field of the configuration block in the ubershader; **U** uniform data (same in both modes, no recompilation); **T** table (a texel buffer filled when its dirty bit is set).

| State | Class | Why | Azahar | In the captures |
|---|---|---|---|---|
| Blend enable, equations (rgb, alpha), the four factors, colour write mask | P | plume bakes blend on every device; MoltenVK cannot make it dynamic (X8) | `BlendingState` | blend on in all 9,645 draws; Add/Add only (`census`); the factors are not in the census (`unverified`) |
| Blend constant colour | D | patch 07 | `DynamicPipelineInfo.blend_color` | not in the census |
| Hardware logic op (16 values), only with the `logic_op` bit and blending off | P | N1 | `BlendingState.logic_op` | 0 draws |
| Logic op emulated in the shader (requested op; canonical form: zero when blending is on) and the destination copy | G (+ P: colour write mask for NoOp) | the `logic_op` bit is off | `FramebufferConfig.logic_op`, `requested_logic_op` | 0 draws |
| Requested blend equation and factors of a half whose equation is Min or Max (canonical form: zero otherwise) | G (+ P: the pipeline blends ONE, ZERO, ADD) | Vulkan `MIN`/`MAX` ignore the factors | `BlendConfig`, `BlendingState.rgb_blend_emulation` | 0 draws |
| Depth test, compare op, depth write | P | plume has no extended dynamic state | `DepthStencilState` | off in 58 to 99 percent of draws per capture, LessThanOrEqual or Always otherwise (`census`) |
| Stencil test, compare op, the three stencil actions | P | same | `DepthStencilState` | Always with Keep/Keep/Replace in 6 to 20 draws per capture |
| Stencil reference, compare mask, write mask | D | patch 10 | `DynamicPipelineInfo` | not in the census |
| Cull mode, front face, depth bias (the rest of `FixedFunctionState`), topology (lists, strips; fans rewritten by N2-05) | P | | `RasterizationState` | topologies Strip and Shader only |
| Attachment formats, as the logical PICA formats (the host formats come from `ChooseFormat` when the pipeline is built) | P | the pipeline needs the render pass formats | `AttachmentInfo` | RGBA8 and D24S8 in every draw, so D32S8 on the RX 6700 XT and both M2 drivers |
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
| Vulkan specialisation constants | none under option 1; **the mechanism under option 1b** | 1b: the key's fields and the device-profile switches (interlock or compare-and-swap, border emulation, logic-op emulation) are `uint32` constants (`rhi-spike 6.3`), so one SPIR-V serves every device and every key, **except the shadow-write modules** (`uber_shadow_write` and the specialised shaders of the shadow-write pass), which need two variants each (interlock and compare-and-swap): `FragmentShaderPixelInterlockEXT` is a capability of the whole SPIR-V module, and a module that declares it is invalid on a device without the extension even when the code using it is dead behind a specialisation constant (`memory`). Under option 1 the profile is applied when code is generated (the SPIR-V is per device profile, [4.4.1](#441-one-source-two-modes)). N2-26's frozen pipeline description lists none, so 1b needs a spec edit ([8](#8-for-the-planner) (a)). | `Profile` | |

On specialisation constants the answer therefore depends on the option, and an earlier draft of this document rejected the "constant ubershader" (every key field a constant, the driver folding them) with three reasons that do not hold: the texture-0 type needs no separate module (the three unit-0 bindings of [4.4.1](#441-one-source-two-modes) serve every type), the 280 bytes of key (the 272-byte `FSConfig` plus the user and gas words) are 70 `uint32` constants, and the large module compiled per pipeline is a background compile and the same cost the ubershader pays. The real costs are `unverified`: the driver's compile time and the tightness of the folded code. It is option 1b.

### 4.3 The pipeline key and how many keys there are

```
PipelineKey {            // the REQUESTED form: what the lists store; defined in native/src/gpu/native/pipeline_key.h (4.7)
  pass kind            : Normal | ShadowWrite
  vertex stage         : N2-06 program id + N2-05 vertex layout (or the trivial stage for CPU-assembled batches)
  fixup stage          : none | fixed geometry shader            (lighting on, hardware vertex shader, geometry_shader bit)
  fragment             : FragmentKey id | UberModule id
  fixed-function state : rhi.h's FixedFunctionState, whole (blend equations and factors, colour write mask, logic op,
                         depth test/write/compare, stencil ops, cull, front face, depth bias), as the PICA registers
                         request it, plus the topology
  attachments          : the logical PICA colour and depth formats (Azahar's AttachmentInfo)
}
FragmentKey = Azahar's FSConfig fields ([4.2]) in the canonical form below + the user word + the reserved gas word
```

N2-16 serialises exactly what `core::ToPlume` consumes (N2-26's `FixedFunctionState`) plus the topology, the vertex layout and the stage ids. One pipeline layout, the superset of the bindings of [5.6](#56-samplers) and [5.10](#510-uniforms), serves the specialised and the ubershader modules, so a switch from one to the other never changes the bindings.

**Which form the lists store, and where the device profile is applied (said once).** The key is the *requested* configuration: the lists store the `PipelineKey` as the registers request it. The device profile is applied afterwards, when code is generated (the logic-op and border emulation bits, the min/max emulation) and when the plume pipeline is built: the hardware logic op or its absence, the blend forced to ONE, ZERO, ADD for a min/max emulation, and the host formats from `ChooseFormat`. So a list moves between the PC and the Mac. The fragment key does not change with the device. The generated code does, and for a logic-op draw so does the pipeline that results (hardware op with the `logic_op` bit on; plain pipeline and an emulating shader with it off). Everything else is the same on every device, **including MoltenVK**, because plume bakes blend on all of them ([5.9](#59-pipeline-counts)).

**The canonical fragment key.** Azahar's `FramebufferConfig` stores `requested_rgb_blend` and `requested_alpha_blend` whenever blending is on and `requested_logic_op` always (`pica_fs_config.cpp`, the constructor). Every captured draw blends, so each blend-factor pair and each value left in the ignored logic-op register would make a new fragment key with identical code. The rule: **zero `requested_logic_op` when `alphablend_enable` is set, and zero a half's requested blend unless its equation is Min or Max.** This is device independent, because `ApplyProfile` uses the blend factors only for a Min or Max half (it reads them through `UsesNoOpFactors()` for any equation when blending is on, but the result matters only for Min or Max) and the requested logic op only when blending is off (`pica_fs_config.cpp`, `FramebufferConfig::ApplyProfile`). The factors that matter for the pipeline stay in the pipeline key's `FixedFunctionState`. The canonical rule changes none of the bounds below, which are counts of census groups, not of Azahar keys.

**Size of the fragment key.** Summing Azahar's structs by hand (`FramebufferConfig` 32 bytes, `TextureConfig` 136, `LightConfig` 76, `ProcTexConfig` 28) gives about 272 bytes (`inference`: `static_assert(std::has_unique_object_representations_v<...>)` rules out padding, the sizes are mine). The disk cache and the ubershader's configuration block hold this struct plus the user word and the reserved gas word ([5.11](#511-gas-stays-open)): 280 bytes, 70 `uint32`.

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

The specialised shader and the ubershader come from one source. There are two ways, and the choice between them is the 1 against 1b of [3.3](#33-recommendation); the rest of the document is the same for both.

**Option 1: a two-mode emitter.** The generator is written once, as C++ that emits GLSL text (Azahar's `FragmentModule` is the model). Every decision that Azahar's generator makes with `switch (config.x)` goes through an emitter helper with two modes. In *specialised* mode the helper evaluates the field at generation time and emits only the chosen branch, with the numbers as literals (Azahar's output). In *uber* mode it emits a GLSL `switch` or `if` on the same field read from the configuration block (`cfg.tev[2].sources`), and loops where the key holds a count (`cfg.lighting.src_num`). The two modes are different code inside each helper, so they can drift; what catches it is the equality test of each feature (the same synthetic draw in both modes must give the same image, and both must match the software renderer's formulas), which is a proposed addition to the packages' *Done when* lines ([8](#8-for-the-planner) (g)), not something they require today. glslang runs at run time for each specialised shader.

**Option 1b: one GLSL source against `cfg`.** The ubershader is written once; every field of the `FragmentKey` is read through an accessor that returns either the block's value or a specialisation constant (`layout(constant_id = N) const uint spec_x`, with one constant `spec_uber` choosing). The specialised shader is the *same SPIR-V* with the key's roughly 70 `uint32` fields passed as constants (plume's specialisation pairs, `rhi-spike 6.3`) and `spec_uber` zero; the SPIR-V is built offline (the committed `.inc` of `tools/native/spirv_inc.py`, N2-25), so fragments need no glslang at run time. The device-profile switches are constants too, so `uber_color` and the specialised colour shaders have one SPIR-V for every device; every shadow-write module (`uber_shadow_write` and the specialised shaders of the shadow-write pass) has two (interlock and compare-and-swap), because the interlock capability is declared by the whole module and a module that declares it is invalid on a device without the extension even when the code is dead behind a constant (`memory`). What 1b removes is the emitter; what it costs is the `unverified` pair of [3.1](#31-the-options-and-the-evidence) (driver compile time of a large module per pipeline, tightness of the folded code) and a field in N2-26's pipeline description ([8](#8-for-the-planner) (a)). Whether a driver folds a `switch` on a constant and unrolls a loop whose bound is a constant is exactly what the spike of [3.3](#33-recommendation) point 4 measures.

**Both.** The module count of the ubershader is **two**: `uber_color` (the normal pass) and `uber_shadow_write` (the shadow-write tail, [5.1](#51-shadow-writes-and-the-shadow-lookup)). Unit 0 declares its three sampler classes (2D, cube, shadow `usampler2D` equivalent) as separate bindings and the draw binds a one-texel dummy for the classes it does not use, so one module serves every texture-0 type; that costs two extra descriptor writes per draw and saves two module variants (`inference`; three modules by texture type is as valid). **Device profile:** under option 1 the profile switches are `#define`s of the generator, so the ubershader's SPIR-V is built once per device profile (`GenerateUber(PassKind, DeviceProfile)`) and cached; under 1b they are constants and one SPIR-V serves every device, except that the shadow-write modules (`uber_shadow_write` and the specialised shadow-write shaders) are built in two variants (interlock and compare-and-swap; the capability of the module, `memory`). It is never generated per draw.

Risk under option 1, stated plainly: the emitter is the one new idea in this design and its complexity is the argument against it. The fallback if it proves too hard is 1b, or a hand-written ubershader for the features of [2.1](#21-what-the-captures-use) only, tested against the generator's output, which is the shape of option 2' (two implementations) to keep equal.

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

All branches are on draw-uniform values (the block is the same for every fragment of a draw), so no divergence is introduced (`inference`: on the RX 6700 XT and the M2 uniform branches are scalar or cheap; not measured). The block is the `FragmentKey` struct itself, appended to the fragment uniforms in the same ring allocation ([5.10](#510-uniforms)) and written with every fragment-uniform write (the 280 bytes are always written, rather than decided per draw), so a draw makes one uniform write and one binding update.

#### 4.4.3 Cost per pixel at 3440×1440

**This is an `estimate`; every input below is an assumption, none is measured.** The measurement is the forced-ubershader timing of G3 on the PC (once N2-20 passes, with N2-08's force switch; [3.1](#31-the-options-and-the-evidence)) and N2-19's runs.

- **Two targets, not one.** The PC's is D19's: GPU at most 6 ms at 3440 × 1440 = 4,953,600 pixels (`derived`) with the HD pack. The M2's is its own: "M2 at 3x: steady 60 fps" (`docs/native/ROADMAP.md` §7 table), measured by N2-19 at `--scale 3` (`N2a N2-19`, *Spec*): since 3440 × 1440 is the widened top screen at 6x, 3x is a quarter of the pixels, 1,238,400 (`derived`), against a frame time of 16.7 ms (the M2's frame time at 60 fps; no GPU share is defined for the M2, so its percentages below are shares of the whole frame).
- **Fragments shaded per output pixel per frame (overdraw), k:** not in the census (it holds draws, not areas). Assumed 2 and 4, both tabulated.
- **Sustained throughput:** RX 6700 XT about 13.2 TFLOPS fp32 peak (`memory`), M2 about 2.8 to 3.6 TFLOPS depending on core count (`memory`; the cores of the maintainer's Mac are unknown; the larger figure is used), 30 percent sustained (assumed), one lane-instruction per FMA: 1,980 and 540 lane-instructions per nanosecond.
- **Instructions per fragment, by hand from the structure of Azahar's generated code, ±2×:** (a) unlit, 2 texture units, 3 active TEV stages: about 155 specialised, 355 ubershader; (b) 3 lights, fog, 2 units, 4 stages: 475 and 865; (c) 8 lights, fog, procedural texture, 3 units, 6 stages (nothing like it in the captures, which have 3 lights at most): 1,015 and 1,745.

Time if every fragment of the frame were shaded by that kind of draw (`derived`: pixels × k × instructions / rate), with the share of the target in brackets:

| Draw | PC, k = 2: specialised / ubershader (of 6 ms) | PC, k = 4 | M2 at 3x, k = 2 (of the 16.7 ms frame) | M2 at 3x, k = 4 |
|---|---|---|---|---|
| (a) | 0.78 ms (13%) / 1.78 ms (30%) | 1.55 ms (26%) / 3.55 ms (59%) | 0.71 ms (4%) / 1.63 ms (10%) | 1.42 ms (9%) / 3.26 ms (20%) |
| (b) | 2.38 ms (40%) / 4.33 ms (72%) | 4.75 ms (79%) / 8.66 ms (144%) | 2.18 ms (13%) / 3.97 ms (24%) | 4.36 ms (26%) / 7.93 ms (48%) |
| (c) | 5.08 ms (85%) / 8.73 ms (146%) | 10.16 ms (169%) / 17.46 ms (291%) | 4.66 ms (28%) / 8.00 ms (48%) | 9.31 ms (56%) / 16.01 ms (96%) |

What the table supports, and what it does not. **It does not refute option 3**: on the PC at k = 2 the ubershader takes 30% and 72% of the budget for the kinds of draws the captures have (rows (a) and (b)), and on the M2 at its own target every row at k = 2 takes at most 48% of the frame. It fails at k = 4 for row (b) on the PC (144%), and for row (c) at either k on the PC; the M2 at 3x stays within its frame in every cell (at most 96%). So the risk sits on the PC for lit and overdraw-heavy frames, not on the M2 (an earlier draft compared the M2 with the PC's 6 ms, which was the wrong target). The specialised shaders are 1.7 to 2.3 times cheaper per fragment, an `estimate` of the ratio only. Two things the table does not model, either of which can move it by more than the ±2×: **register pressure and occupancy** (the ubershader's register allocation is sized for its worst path and hurts every draw it shades), and **overdraw above four**, which the geometry-shader sprite pass (D5's term; 38.99 to 98.30 percent of the draws are Point-mode geometry-shader draws, blended like every draw, with the procedural texture on in 30.58 to 98.30 percent) can reach locally (`inference`). Neither mode has an early depth test ([5.4](#54-depth)), so occluded fragments cost the same in both.

Compile cost of the ubershader module is `unverified` (a very large module, one pipeline per combination of [4.5](#45-when-the-specialised-pipeline-replaces-it)); the lists ([4.6](#46-precompiled-pipeline-lists)) build these first.

### 4.5 When the specialised pipeline replaces it

Per draw, on the renderer thread (N2-16 owns the code):

1. Build the `PipelineKey`. Look it up in the pipeline table (a lock-free read of a published, immutable entry).
2. **Present and ready:** draw with it.
3. **Absent:** insert a placeholder (so no second request is made), queue "generate (option 1) or specialise (1b), create the pipeline" on a worker thread (the backend allows creation from several threads, `N2a N2-28`), and draw this call with the fallback pipeline of the rules below.
4. **Placeholder, not ready:** draw with the fallback. When the worker publishes the pipeline, the next draw with that key uses it. A switch happens between draws, never inside one.
5. The key goes into the list of [4.6](#46-precompiled-pipeline-lists) at its first sight, so the next start compiles it before it is needed.

Rules this implies:

- **The two shaders must give the same image** within the thresholds, or the switch shows as flicker. The sources of difference are float contraction and the order of operations; the PICA rounds to 8 bits at several points (`byteround` in Azahar's output), which hides most of it. N2-16's *Done when* requires the same image with forced misses; the feature packages' *Done when* lines do not yet require it per feature (proposed in [8](#8-for-the-planner) (g)).
- **The fallback pipeline is not one per fixed-function state.** The `PipelineKey` contains the vertex stage, so an ubershader pipeline exists per `(vertex stage, fix-up stage, fixed-function state, attachments, pass kind)`, and a new vertex program costs a glslang compile at its first use (D27) before its pipeline compile. This holds under every option. **Recommendation** (the maintainer or the planner decides): **(ii)**, the fallback uses a trivial vertex stage with the vertices shaded on the CPU, as the geometry-shader draws already are, so the fallback pipelines are bounded by fixed-function state, attachments and pass kind, with no fix-up stage (the CPU does the fix-up) and no translated vertex shader. It needs a CPU path that no package builds today ([5.5](#55-the-quaternion-fix-up-per-device)): a CPU shader runner (the vertex shader and the geometry shader), primitive assembly with the quaternion fix-up, and a streamed vertex batch. That is more than promoting N2-06's interpreter ("in the tests only", `N2a N2-06`, *Spec*), unless the path exists anyway for the geometry-shader draws (38.99 to 98.30 percent of the draws, `census`), in which case (ii) reuses it. It spends CPU only during misses. The alternative **(i)** accepts a counted synchronous build for each new combination, which is simpler to build and stalls once per vertex program and fixed-function combination on a new machine, against principle 5. (ii) is recommended because (i) puts dozens of stalls (19 to 31 vertex programs are in the captures, [4.3](#43-the-pipeline-key-and-how-many-keys-there-are)) in a fresh install's first run.
- **A fallback that does not exist is built synchronously, once.** Even with (ii) the fallback pipeline for a never-seen fixed-function state (a new blend factor pair, say) is itself a compile, because plume bakes that state. It is built on the render thread the first time, counted (`pipeline_sync_builds`) and listed so it never repeats on that machine. The alternative, skipping the draw, is Azahar's asynchronous behaviour and shows as a glitch. This is why N2-16's "never skipped, always through the ubershader" and the *Done when* "a cold cache never blocks on a compilation" cannot hold under any option with plume ([8](#8-for-the-planner) (e)).
- **Eviction:** a specialised pipeline stays for the run (the count is `unverified`, [4.3](#43-the-pipeline-key-and-how-many-keys-there-are); at least 23 distinct TEV configurations occur in one capture); the list is the only thing that persists.

### 4.6 Precompiled pipeline lists

**What they record.** Three things, in this order of importance:

1. **Keys** (authoritative, device independent): the `PipelineKey` in its requested form ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are): the canonical fragment key, the fixed-function state as the registers request it, the logical PICA pixel formats), with a use count and the program ids of the vertex stage. The file header holds the struct hash of the key types, as Azahar's `FILE_INFO` entry does (`vk_shader_disk_cache.cpp`, `CacheEntryType::FILE_INFO`, `FS_CONFIG`, `PL_CONFIG`): a change to a key type invalidates the list, and the list is rebuilt by play.
2. **Programs:** the PICA vertex and geometry programs (words and swizzle words) a key's vertex stage was translated from, by hash (Azahar's `VS_PROGRAM`). The translated SPIR-V is regenerable and may be cached beside them as a speed-up, dropped when the generator's version changes.
3. **The driver's blob:** `SavePipelineCache()` and `LoadPipelineCache()` of N2-28, one file per device (vendor, device, `pipelineCacheUUID`), refused when the header does not match.

**Where they live.** On the player's PC, under the user folder of D61 (`<user folder>/native/pipelines/`, a name N2-16 fixes), never in the repository and never in a release: programs and keys are derived from the game, and the driver blob is machine specific. ROADMAP's legal row says it outright ("pipeline lists and caches ... never enter the repository or the packages") and `docs/native/AGENTS.md` §8 rule 2 forbids anything derived from the game in the repository.

**How they are built.**

- **From captures (the maintainer's PC, and CI never):** the capture player with an empty list replays G1 to G4 and G9a to G9c; as in play, every key at its first sight is appended (N2-16's *Spec*), so the run itself builds the list for the census scenes. The count of [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) comes from the census extension, not from this.
- **From play:** the render thread appends each key at its first sight (N2-16's *Spec*), deduplicated by hash, written by the pipeline worker, never by the render thread.
- **At start:** the fallback pipelines for every combination in the list come first (they are what stops the first draw from stalling), then the specialised pipelines in decreasing use count on worker threads, while the game boots. How long the whole list takes on a given driver is `unverified`; N2-16's maintainer check (a warm cache, no frame of G3 over 8.3 ms because of a pipeline) measures it.
- **A new machine:** an empty list is valid. Every draw then takes the fallback (after the counted synchronous builds of [4.5](#45-when-the-specialised-pipeline-replaces-it) for each new fixed-function state, and, under (i), for each new vertex program) and the specialised pipelines arrive in the background. This is the case the ubershader exists for.

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
Generated GenerateUber(PassKind, const DeviceProfile&);                    // option 1: once per profile; 1b: once, offline

struct Uniforms;                // Azahar's FSUniformData layout (1,328 bytes) + the 'cfg' block
void FillUniforms(const pica::PicaState&, Uniforms&, UniformDirty&);       // writes only what a dirty bit says
void UploadTables(const pica::PicaState&, TableRings&);                    // lighting, fog, proctex LUTs
}
```

Two properties: `MakeKey` and `NeedsOf` are pure functions of the state and the profile (so the key tests run with no device and N2-16's list tests need no GPU), and nothing in this header names a Vulkan or plume type (`rhi.h` rules of N2-26). **Who owns the `PipelineKey` type, so that there is no dependency cycle:** the composition. `native/src/gpu/native/pipeline_key.h` (N2-08) defines the type from this module's key, `rhi.h`'s `FixedFunctionState` (declared by N2-26; filled from the registers by `fbops::MakeFixedFunctionState(const pica::PicaState&)`, N2-13's `fbops/`, a name proposed here, in its requested form; the device profile is applied to it by N2-13's `backend/state` when the pipeline is built) and N2-05/N2-06's vertex description, and declares the abstract interface the composition calls (`PipelineProvider`: look up a key, return a ready pipeline or the fallback, report a miss). `native/src/gpu/pipeline/` (N2-16) includes that header, implements the interface, serialises the key and owns the files; the composition receives the implementation by injection and does not link `pipeline/`. **No cycle:** `pipeline_key.h` is a header-only CMake `INTERFACE` target (declared by N2-08 in `native/src/gpu/native/`, depending only on the key and `rhi.h` headers), which `pipeline/` links; N2-16, whose allowlist has `native/src/gpu/native/**`, registers its provider through `mh4n/registry.h` and `--gpu native` picks it when registered. **N2-20 runs before N2-16, so N2-08 ships a default provider**: a synchronous build on a miss (counted), no list, and a debug option `--force-ubershader` of `--gpu native` that always takes the fallback; the per-frame GPU time is `FrameGpuTimeNs` (N2-28) in the capture-player report.

**The hook the output stage needs from N2-13 (`fbops/`) is plain GLSL, with signatures fixed here.** N2-08 depends on N2-13, so N2-13 runs first and cannot write against an emitter that does not exist yet; it writes embeddable GLSL source and tests it in its own shaders (SPIR-V arrays from `tools/native/spirv_inc.py`, as its *Traps* say). The functions: `vec4 mh4n_logic_op(uint op, vec4 src, vec4 dst)` (all 16 ops on the 8-bit rounded values; `dst` is only read for the eleven ops that need it, [5.2](#52-logic-ops)) and `vec4 mh4n_minmax_blend(uint eq_rgb, uint src_rgb, uint dst_rgb, uint eq_a, uint src_a, uint dst_a, vec4 src, vec4 dst, vec4 blend_color)` (`min` or `max` of `source × factor` and `dest × factor` per half; the equation and factor arguments are Azahar's enumerator values as `uint`). The generator calls them at the output point of [4.8](#48-the-feature-interface): in specialised mode (option 1) with literals, in the ubershader (and under 1b) with `cfg` fields. No allowlist changes: `fbops/` holds the source, `fragment/` includes it.

### 4.8 The feature interface

The generator calls each feature at fixed **points**, in Azahar's order (`FragmentModule::Generate`, `glsl_fs_shader_gen.cpp`):

| Point | What is emitted | Who |
|---|---|---|
| declarations | the 7 varyings (core), bindings, helper functions, and the **prototype block** (core): `vec4 sampleTexUnit0()` to `sampleTexUnit3()`, `vec4 shadowTexture(vec2 uv, float w)`, `vec4 shadowTextureCube(vec2 uv, float w)`, `vec4 ProcTex()`, emitted before any definition so the order of definitions does not matter | core (N2-08); each feature adds its own declarations |
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
| `sampling/` (N2-23) | texture 0 type, texture 2 coordinate source, requested wraps, border-emulation bits | `sampleTexUnit0` to `3`, wrap and border emulation, LOD, projection, cube, the normal map. `sampleTexUnit0` sends Shadow2D to `shadowTexture` and ShadowCube to `shadowTextureCube` (`shadow/`), and `sampleTexUnit3` sends the procedural slot to `ProcTex()` (`proctex/`), as `glsl_fs_shader_gen.cpp:1722-1755` | type and wrap read from the block; three sampler classes for unit 0 | `tex_lod_bias[3]`, `tex_border_color[3]` | texture and sampler per unit (two descriptors each); sampler objects from the PICA filter and wrap, clamp-to-edge where the border is emulated |
| `lighting/` (N2-09) | `LightConfig` | light loop, LUT lookups, bump, clamp; the bump and shadow terms call `sampleTexUnit{bump_selector}()` and `sampleTexUnit{shadow_selector}()` (`glsl_fs_shader_gen.cpp:506, 568`), which are `sampling/`'s; uses `normquat` and `view` (declared in core) | dynamic loop over `src_num` | `lighting_lut_offset`, `light_src[8]`, `lighting_global_ambient`; table `lut_lf` | `fixup_gs` where the device allows ([5.5](#55-the-quaternion-fix-up-per-device)); owns the generator of the fixed geometry shader, which the composition wires in |
| `fog/` (N2-10) | fog mode, fog flip; the fragment operation mode Gas; the **reserved gas key word** | `WriteFog`; the gas hook | one branch | `fog_color`, `fog_lut_offset`; table `lut_lf` (fog part); the **reserved gas uniform region** | gas may need `ordered_rmw` or `dst_copy` ([5.11](#511-gas-stays-open)) |
| `proctex/` (N2-11) | `ProcTexConfig` | `ProcTex()`, clamps, shifts, combiners, LUT lookups | read from the block | `proctex_*` offsets and noise parameters; tables `lut_rg`, `lut_rgba` | none |
| `shadow/` (N2-12) | shadow rendering, orthographic flag; texture-0 type is `sampling/`'s field, interpreted here for Shadow2D and ShadowCube | `shadowTexture(vec2 uv, float w)` and `shadowTextureCube(vec2 uv, float w)` (`CompareShadow`, the four-texel lookup), `UpdateShadow`, the write tail (interlock or compare-and-swap) | `uber_shadow_write`; the lookups in `uber_color` | `shadow_bias_constant`, `shadow_bias_linear`, `shadow_texture_bias` | `shadow_write_pass`: zero-attachment pass, storage image, the `interlock` bit |
| output (`fbops/`, N2-13, and core) | canonical requested logic op, requested blend of a Min or Max half, the emulation flags | `mh4n_logic_op` (all 16) and `mh4n_minmax_blend` ([4.7](#47-the-interface-between-the-command-processor-and-the-generator)) | read from the block | `blend_color` copy for min/max | `dst_copy`; the pipeline's blend and colour write mask |
| core (`fragment/`, N2-08) | alpha test function, scissor mode, depth-buffering mode, the six TEV stages, combiner-buffer masks, the user word | prologue, depth, scissor, TEV, alpha test | read from the block | `const_color[6]`, `tev_combiner_buffer_color`, `alphatest_ref`, `depth_scale`, `depth_offset`, scissor rectangle | none |

Shared fields and calls between features: `texture0_type` belongs to `sampling/` and `shadow/` reads it. A feature never calls another feature's internals or reads its key fields; it calls the functions of the prototype block, which core declares before any definition, so the order of emission does not matter. In Azahar the lighting bump and shadow terms call `sampleTexUnit{n}()`, so they belong to `sampling/`, which in turn calls `shadowTexture`, `shadowTextureCube` and `ProcTex()`. **N2-08's stubs define every function of the prototype block**, so N2-09, N2-11 and N2-12 can run in parallel after N2-23. A stub implements `Key` (so the key is complete), emits a neutral value, reports `Stubbed` and a `stubbed:<feature>` counter, as `N2a N2-08` specifies.

**Reserved room.** `FSConfig` has no gas fields and the gas registers are not in `FSUniformData`, so N2-08 reserves, owned by `fog/`, a gas key word in the `FragmentKey` and a gas uniform region in `FragmentUniforms` (the 16 raw gas register words and the 16-entry gas table of 3dbrew, `rhi-spike 5.5`; the table has no data source in `PicaState` today, [5.11](#511-gas-stays-open)). **Rule:** a feature that needs a key field or a uniform outside its reserved space (`fog/`'s gas, the user word, each feature's own sub-struct) reports `needs-decision`; it does not edit core's key or uniform files, which N2-08 owns.

## 5. Paths the RHI lacks (what D4 fixed)

The paths that depend on the device are chosen by a bit of the backend's capability structure (`N2a N2-26`), so lavapipe runs each of them through `MH4N_RHI_MASK`. Three rows of the table are the exceptions and say why: min/max blending is chosen by the draw's factors, not by the device; the depth write is the same everywhere; and the geometry-shader route has a value in N2-26 where a maskable bit is needed.

| Gap (device) | Bit | Mask name | The path without it | Section |
|---|---|---|---|---|
| no interlock (KosmicKrisp) | `interlock` | `interlock` | compare-and-swap loop | 5.1 |
| no hardware logic op (MoltenVK) | `logic_op` | `logic_op` | shader emulation of all 16 through the feedback copy | 5.2 |
| min/max ignores factors (every Vulkan device) | none: chosen by the factors | none | shader path through the feedback copy; runs on lavapipe unmasked | 5.3 |
| fragment depth write | none | none | no early depth test | 5.4 |
| no geometry shader (both M2 drivers) | `geometry_shader` is a value in N2-26, not a bit | none yet | no fix-up for hardware-vertex-shader lit draws | 5.5, [8](#8-for-the-planner) (a) |
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

**Chosen by the `logic_op` bit, and only with blending off.** **The fragment key carries the requested op (blending off, op not Copy) on every device**, which is what lets the lists move between the PC and the Mac ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are)). The `logic_op` bit changes only what is generated and built from it. With the bit on, the pipeline built from the requested key carries the hardware op (all 16 via plume's `logicOpEnabled`/`logicOp`, `rhi-spike 6.1` N1) and the shader writes the colour as it is. With it off (MoltenVK; lavapipe under `MH4N_RHI_MASK=logic_op`), the shader emulates **all 16** and the pipeline built has blending off, with the colour write mask the requested one except for NoOp, where it is 0 (Azahar's `GetFinalColorWriteMask`: the mask discards the colour while the depth write stays).

**How the destination is read.** Of the 16 ops, five need no destination (Clear, Copy, Set, CopyInverted, NoOp); eleven do (And, AndReverse, Invert, Nand, Or, Nor, Xor, Equiv, AndInverted, OrReverse, OrInverted; `derived` from Azahar's `LogicOp` enumeration). Those eleven read it from the **feedback copy** (X6): the pass ends, the target is copied to its twin, the next pass resumes with load, and the shader binds the twin as `tex_color` (`destFactor` in Azahar's GLSL). That is the RHI's one route to the destination colour. It is an `inference` that this covers logic ops: `rhi-spike` 9.4 item 6 names X6 (the feedback copy) without saying which features use it, and 5.6 and 9.5 (the N2-07 bullet) name it for min/max; none names it for logic ops. Framebuffer fetch and input attachments are not offered (plume exposes neither; the optional input-attachment patch is about 70 lines and Vulkan-only, `rhi-spike 6.8`). A storage-image read-modify-write of the colour target ordered by interlock would avoid the copy, but it needs storage usage and a mutable-format view on every colour surface, which D4's item 4 reserves for shadow sources so that Apple GPUs can compress the rest (`inference`): rejected here. The ops work on the 8-bit rounded values (`byteround` precedes them in Azahar's output); on the narrower formats (RGB565 and so on) the result is `unverified`, and none is in the census.

**Cost.** One pass break and one full-image copy per emulated draw that needs the destination (`rhi-spike 5.6`); with 0 such draws in the captures it is unmeasured.

### 5.3 Min/max blending

Hardware `MIN`/`MAX` only when **both factors are One** (`BlendConfig::UsesNoOpFactors`, per rgb and per alpha half): Vulkan's `MIN` and `MAX` ignore the factors, the PICA's do not. Otherwise the pipeline blends ONE, ZERO, ADD (the key's blend fields say so) and the shader computes `min` or `max` of `source × factor` and `dest × factor` from the feedback copy, with the blend colour from the uniform block (`WriteBlending`, `glsl_fs_shader_gen.cpp:897`; `rhi-spike 5.6`, N3). The choice is by the factors, not by a capability bit (`has_blend_minmax_factor` is false for Vulkan), so lavapipe runs both paths with no mask. Seen: no min/max equation in any capture (the equations are Add/Add in all 9,645 draws).

### 5.4 Depth

The fragment shader writes `gl_FragDepth` in every non-shadow draw (X2: the PICA depth is recomputed from `gl_FragCoord.z` with `depth_scale` and `depth_offset`, and from `1/w` in W-buffering mode, `WriteDepth` and `Generate` in `glsl_fs_shader_gen.cpp`). So **no early depth test exists for PICA draws** on any device, specialised or ubershader. How much that costs depends on how many draws could have used one: the census's "depth buffer in use" is 0.57 to 42.09 percent of the draws (G9c and G4), and the depth test is off in 77 percent of G3's draws (2,810 of 3,650) and 58 percent of G4's (805 of 1,390) (`derived`); alpha test, which also forces a late test, is GreaterThan in 3 to 315 draws where present (3, 140, 315 and 112 in G2, G3, G4 and G9a). The effect is that occluded fragments of the depth-tested draws are shaded in full: a cost that falls on the lit, fogged and shadowed draws as much as on the others, and which the estimate of [4.4.3](#443-cost-per-pixel-at-34401440) already includes, since it counts every fragment. Not designed away; a variant that skips the depth write when it provably equals the interpolated depth is not established and is not proposed (`unverified`).

### 5.5 The quaternion fix-up, per device

| Draw | Host | Route |
|---|---|---|
| The geometry-shader draws (CPU draws: `PicaCore::DrawArrays` refuses acceleration, Azahar's `AddTriangle` flags vertices and the `HardwareVertex` constructor negates the quaternion) | every host | **on the CPU**, in vertex assembly, `rhi-spike 5.4`. Nothing in the fragment stage. **But no package builds that path today**: N2-06's Goal translates the geometry shaders to SPIR-V, N2-05's `VertexLoader` port and N2-06's interpreter are "in the tests only" (`N2a.md:172, :189`), and the N2-07 *Spec*'s own "on the CPU for the geometry-shader draws" (`N2a.md:213`) rests on the same unowned path, for 38.99 to 98.30 percent of the draws (`census`). Who builds it is a question for the planner that holds under every option ([8](#8-for-the-planner) (k)) |
| Lit draws shaded by the translated vertex shader | RX 6700 XT, lavapipe (`geometryShader` on) | the **fixed geometry shader** (`triangles` in, `triangle_strip` out, used only with lighting on; Azahar's `PicaFixedGSConfig` in `shader_gen.h`, `GenerateFixedGeometryShader` in `glsl_shader_gen.cpp`, `ShaderDiskCache::UseFixedGeometryShader`): the key's `fixup stage` field is set; `lighting/` (N2-09) owns the GLSL generator of that geometry shader (a function of the vertex stage's output attributes, which N2-06 must expose, [8](#8-for-the-planner) (a), (h)), and N2-08's generic composition wires the stage into the pipeline, since N2-09's allowlist has no `native/` |
| the same | M2, MoltenVK and KosmicKrisp (`geometryShader` off) | **no fix-up** |
| the same | any host, by fragment barycentrics | **not offered**: the vendored plume does not enable the extension and N2-25 does not patch it |

The route is a pipeline-key field (`fixup stage`), set by the composition when `Needs.fixup_gs` holds and the `geometry_shader` capability value is on; `lighting/` uses the plain `normquat` varying, declared in core (not Azahar's `pervertex` array, which is the barycentric route). **How many lit draws take the GPU route is `unverified`**: the census has no joint table of lighting against geometry shader. The census does bound it from the other side in three captures: lit draws exceed the non-geometry-shader draws by at least 171 in G2 (297 − 126), 200 in G3 (1,290 − 1,090) and 14 in G9a (455 − 441) (`derived`), so at least that many lit draws are CPU draws there; G4 gives no bound (745 lit, 848 without a geometry shader). **Testing the M2's route on lavapipe is not possible through the mask today**, because N2-26 lists `geometry_shader` among the values, not the bits ([8](#8-for-the-planner) (a)); until then the "no fix-up" key is tested on synthetic values of the profile only.

### 5.6 Samplers

With plume there is no combined image sampler, so the generator emits `texture2D tex0;` and `sampler samp0;` as two bindings and writes each lookup as `texture(sampler2D(tex0, samp0), ...)`, `textureLod(sampler2D(tex1, samp1), ...)`, `textureProj(sampler2D(tex0, samp0), ...)` and `samplerCube(tex0, samp0)` for cube; `textureSize(tex0, 0)` and `texelFetch(tex0, ...)` take the texture alone, which on a separate `texture2D` or `utexture2D` needs the extension `GL_EXT_samplerless_texture_functions` in Vulkan GLSL (`memory`; the generator's extension block declares it). In Azahar's GLSL that is the unit-0 to unit-2 lookups in `sampleTexUnit0` to `2` (`glsl_fs_shader_gen.cpp:1716-1750`) and the normal-map lookup (`:510`); `tex_color` and the shadow map use `texelFetch` only. The texel buffers stay as `samplerBuffer` (in Vulkan GLSL a uniform texel buffer, not a combined image sampler: `memory`) bound as plume's formatted buffers. The descriptor layout: set 0 the uniform block(s) and the three texel buffers; set 1 texture and sampler per unit, `tex_color`; set 2 the shadow storage image. Samplers are objects made from the PICA filter and wrap state and cached by value (N2-23); a sampler with the border colour emulated is created with clamp-to-edge ([5.7](#57-border-colours)). No sampler ever sets `compareEnable` ([5.1](#51-shadow-writes-and-the-shadow-lookup)). One pipeline layout, the superset of these bindings and of [5.10](#510-uniforms), serves the specialised and the ubershader modules, so a switch never changes the bindings. The lowering is mechanical, about ten sampled-lookup sites (`source`); it is done once, in our generator, with no combined-sampler form anywhere.

### 5.7 Border colours

Chosen by the `custom_border_color` bit (patch 12, N2-27). **Bit on** (RX 6700 XT, lavapipe): a sampler with the unit's custom border colour. **Bit off** (both M2 drivers by default, and lavapipe under `MH4N_RHI_MASK=custom_border_color`): the key's border-emulation bits are set for a unit whose requested wrap is `ClampToBorder` on the s or t axis (Azahar's `TextureConfig::ApplyProfile`, `pica_fs_config.cpp:92-103`, applied at generation time, so the fragment key itself holds only the requested wraps); the shader returns the unit's `tex_border_color` uniform when the coordinate is out of range on that axis, and the sampler is created with clamp-to-edge. `ClampToBorder2` is mapped like `ClampToBorder` by Azahar's samplers but is not handled by its emulation (a TODO in `pica_to_vk.h`); the census counts both without separating them, so **which of the two the 90 and 21 draws use, and what the hardware does with `ClampToBorder2`, is `unverified`**; the design handles `ClampToBorder` and leaves `ClampToBorder2` to the software renderer's reading (`sw_texturing.cpp`), a check for N2-23. In the captures the only border colour seen belongs to the Shadow2D draws, which need none ([5.1](#51-shadow-writes-and-the-shadow-lookup)), so no ordinary texture in the seven captures exercises either path (`inference`).

### 5.8 The shadow write and min/max emulation are GLSL-only in Azahar

`FSConfig::UsesSpirvIncompatibleConfig()` sends ShadowCube, shadow rendering and min/max emulation to the GLSL generator and glslang (`vk_shader_disk_cache.cpp:162-168`; `rhi-spike 5.2`). The D4 revision of the N2-07 *Spec* (and `rhi-spike 9.5`) reads this as "if D5 ports Azahar's generators (option 2), both generators are needed". **That is not so.** Azahar's SPIR-V generator is a user setting, `spirv_shader_gen` (`common/settings.h:536`, default true), and the test at `vk_shader_disk_cache.cpp:162-163` is `use_spirv && !UsesSpirvIncompatibleConfig()`: with the setting off every fragment shader goes through the GLSL generator and glslang. So the fair option 2 is **2'**: port `glsl_fs_shader_gen.cpp` alone, which covers shadows, min/max and ShadowCube. Option 2 proper keeps the SPIR-V generator for the speed of its compile, at the price of a second set of semantics (the logic-op switches differ) and `sirit`. The shadow draws of the captures (94, in three of seven) are the draws the SPIR-V path could not draw. **Under the option-1 family the item does not arise**: one generator emits GLSL for glslang (option 1), or the one SPIR-V is built offline (1b), and Azahar's SPIR-V path is not ported.

### 5.9 Pipeline counts

Blend state is pipeline state on every device: plume bakes it (there is no dynamic blend state in the Vulkan backend) and MoltenVK cannot make it dynamic whatever the backend does (X8: `extendedDynamicState3ColorBlendEnable`, `ColorBlendEquation`, `ColorWriteMask` and `LogicOpEnable` are false there; 5 of 31 bits). Stencil reference, compare and write masks and the blend constant are dynamic (patches 10 and 07). So **the key and the count of unique keys include the blend state**, and the key holds the depth, stencil, cull and format state too (plume has no extended dynamic state).

**On MoltenVK the count of unique keys is the same as on the other devices** (`inference` from the key definition: the key is built from the requested state and differs by device only through the emulation paths of [4.3](#43-the-pipeline-key-and-how-many-keys-there-are)); what MoltenVK changes is the **cost of each key**: it has no graphics pipeline library (`rhi-spike 9.3`, `VK_EXT_graphics_pipeline_library` absent on both M2 drivers), so a pipeline cannot be linked from precompiled parts, and the vendored plume uses no library on any device anyway, so every key is a full pipeline compile through MoltenVK's SPIR-V to MSL path (`memory`; the time is `unverified`). The only defences are the ubershader and the lists ([4.5](#45-when-the-specialised-pipeline-replaces-it), [4.6](#46-precompiled-pipeline-lists)); `VkPipelineCache` is the plain pipeline-cache blob of N2-28. The number itself is `unverified` ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are), [7](#7-unverified-and-the-census-extension)). What the census does show is how few distinct fixed-function states occur: the `fragment_config` summary has 7 distinct strings in all seven captures, which come from 4 distinct depth-and-stencil combinations for the Default mode (stencil off with depth off; off with LessThanOrEqual and write; off with LessThanOrEqual and no write; stencil Always with depth Always and write), two of the strings differing only in the alpha test (a shader setting), plus the Shadow pass and the Gas mode (`derived` from the strings), with the blend factors unknown.

### 5.10 Uniforms

What the RHI offers (D4): uniform buffers at per-draw offsets over a ring (`Ring::Map(size, align)` and `Commit`), bound by a set update per draw because plume has no dynamic-offset descriptor type (X5; `N2a N2-27` `BindingArena`), texel buffers, a storage image. Azahar's layout is three uniform buffers (vertex data 32 bytes, vertex and geometry data 1,616 bytes, fragment data 1,328 bytes) and three texel buffers over two rings: `texture_lf_buffer` viewed as `R32G32_SFLOAT` (lighting and fog LUTs) and `texture_buffer` viewed as `R32G32_SFLOAT` and `R32G32B32A32_SFLOAT` (procedural noise, colour and alpha maps; colour and difference LUTs) (`source: azahar@5f3306d shader_uniforms.h`, `vk_rasterizer.cpp:88-110, 955-1030`; `rhi-spike 5.7` for the descriptor layout).

**The design:**

- **One fragment uniform buffer, one binding.** `FragmentUniforms` (Azahar's `FSUniformData` layout, 1,328 bytes, copied with provenance) followed by the `cfg` block (the 280-byte key: the 272-byte `FSConfig` plus the user and gas words; always written, not decided per draw) in one ring allocation, aligned to **256 bytes**, the largest alignment Vulkan allows for the offset (`memory`; the devices read so far give 16 and 64, `rhi-spike 3.3`, and the RX 6700 XT's is `unverified`). The vertex-side buffers are N2-06's; the composition passes the two buffers as two bindings in one set update.
- **Rewritten only when dirty.** Azahar's `fs_data_dirty` becomes the PicaState dirty bits the fields depend on; a draw whose fragment state did not change reuses the previous allocation and binding set. Whether that happens often is `unverified` (the census has draws, not state changes between them).
- **Tables in two rings, three views** as Azahar: `lut_lf` (lighting and fog), `lut_rg` and `lut_rgba` (procedural). The offsets of each table inside its buffer live in the uniform block (`lighting_lut_offset`, `fog_lut_offset`, `proctex_*_offset`), so a table upload does not rebuild a binding. Tables are uploaded when `PicaState.lighting.lut_dirty`, `fog.lut_dirty` or `proctex.table_dirty` say so, on the thread that owns the state (N2-22).
- **Per-draw bindings** (textures, samplers, `tex_color`, the storage image) come from the `BindingArena` of N2-27, one set per draw as Azahar; the count of descriptor writes is what [5.6](#56-samplers) lists.
- **Texel buffer size:** the smallest `maxTexelBufferElements` read is 67,108,864 (MoltenVK, `stand-in`, `rhi-spike` section 4, G5), against Azahar's tables of a few thousand entries: no constraint.

### 5.11 Gas stays open

Gas is drawn by nobody (`rhi-spike 5.5`): Azahar's two generators log "Unimplemented gas mode" and write transparent black, the GLSL one returning without writing depth. Seen: G4 only, 20 draws (1.44 percent of its draws, all ten frames): 10 draws with fog mode Gas and 10 with fragment operation mode Gas (`census`). This design **does not decide** the formula (N2-10 does, from 3dbrew's gas registers and a real-3DS check later) and leaves room:

- The key holds the fog mode Gas and the fragment operation mode Gas, and N2-08 reserves, owned by `fog/`, a gas key word and a gas uniform region (the 16 raw gas register words and the 16-entry gas table), so N2-10 edits neither core's key nor its uniform files ([4.8](#48-the-feature-interface)); the draw is a normal pass kind unless N2-10 says otherwise.
- `Needs` carries `ordered_rmw` and `dst_copy`, so N2-10 can ask for an ordered read-modify-write of the colour target (the `interlock` bit with a storage-image pass, or the feedback copy) without a change to this interface. `rhi.h` is frozen, so any new RHI operation N2-10 needs is a `needs-decision`.
- Until N2-10, a gas draw is counted `stubbed:gas` and draws Azahar's behaviour (transparent black); N2-20's thresholds exclude gas frames (`--skip-gas-frames`). The 16 gas registers are not named in Azahar's headers. `PicaState.regs` holds the plain registers (the last word written to each), so N2-10 can read those from there. It cannot read the 16-entry gas table: `GAS_LUT_DATA` is a data port, `regs` keeps only its last word, and neither N2-04's processor (`grep -i gas` in `native/src/gpu/pica` finds nothing) nor Azahar's `pica_core.cpp` has a gas side effect. A gas-table side effect in `gpu/pica/` or a `needs-decision` from N2-10 is needed ([8](#8-for-the-planner) (l)).

## 6. What changes under the other options

**Under option 1b** only [4.4.1](#441-one-source-two-modes) changes: no emitter, one GLSL source, the specialised shader is the same SPIR-V with constants, built offline; the profile switches become constants (one SPIR-V per module for every device, except the two variants of every shadow-write module); N2-26's pipeline description gains a field ([8](#8-for-the-planner) (a)); the feature interface of [4.8](#48-the-feature-interface) is the same with a single mode.

**Under option 2'** (port the GLSL generator, SPIR-V generator off): [4.2](#42-where-each-piece-of-state-goes) and [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) stay almost as they are (Azahar's `FSConfig` is the key, with the canonical rule of 4.3 applied, and its `StaticPipelineInfo` the pipeline key). [4.4](#44-the-ubershader) becomes a hand-written ubershader beside the generator, with the equality tests between them that the single source would have made free. [4.8](#48-the-feature-interface): the feature directories hold member definitions of `FragmentModule` in separate files, so the five packages are not blocked by sharing one file, but every function of the prototype block is a method of one class. [5.6](#56-samplers) is done once (GLSL). [5.8](#58-the-shadow-write-and-minmax-emulation-are-glsl-only-in-azahar) no longer applies. `WriteLogicOp` has 5 case labels, so N2-13 adds the rest as the functions of [4.7](#47-the-interface-between-the-command-processor-and-the-generator). Gas, the ubershader and the lists are ours in any case. [4.5](#45-when-the-specialised-pipeline-replaces-it), [4.6](#46-precompiled-pipeline-lists) and [5](#5-paths-the-rhi-lacks-what-d4-fixed) are unchanged.

**Under option 2** (2' plus the SPIR-V generator): as 2', and [5.6](#56-samplers) is done twice (GLSL and `sirit`); draws with a shadow pass, min/max or ShadowCube take the GLSL path (94 draws in the seven captures), the others the SPIR-V path, with 4 of 16 logic ops until N2-13's functions are added to both by hand; the equality burden grows to three implementations (GLSL generator, SPIR-V generator, ubershader).

**Under option 3** (ubershader only): `FragmentKey` shrinks to the configuration block (there is no specialised code to key); the pipeline key keeps the vertex stage, fixed-function state, pass kind and formats, so [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) counts pipelines by those only (a smaller number, `unverified`). [4.5](#45-when-the-specialised-pipeline-replaces-it) loses its switch and keeps its fallback rules, which are then the only pipelines. [4.6](#46-precompiled-pipeline-lists) stays for the pipelines (each costs more with the large module; `unverified`). [4.8](#48-the-feature-interface) keeps its table with "generated code" replaced by "code of the block". The cost question of [4.4.3](#443-cost-per-pixel-at-34401440) becomes the whole decision, and its PC columns at k = 4 are the risk. N2-16 and N2-08 shrink; N2-09 to N2-12 and N2-23 are unchanged in shape.

## 7. Unverified and the census extension

**What is `unverified` in this document, and what would check it:**

| Item | Check |
|---|---|
| Unique pipeline keys in the captures ([4.3](#43-the-pipeline-key-and-how-many-keys-there-are)) | the census extension below, run on the PC |
| Unique fragment keys across the seven, and in a play session | the same; a play-session count needs longer captures than 3 and 10 frames (`rhi-spike 2`, last list) |
| Blend factors in use; cull mode; wrap modes per unit; `ClampToBorder` against `ClampToBorder2`; scissor mode; W-buffering | the same (fields not decoded by the census today) |
| How many lit draws use the hardware vertex shader | a joint table: lighting on against geometry shader on |
| The procedural-texture configuration count ("one" in D5) | a `proctex_config` group in the census |
| The ubershader's cost, compile time, and the specialised/uber ratio | a forced-ubershader timing of G3 on the PC once N2-20 passes (N2-08's force switch) or with N2-16's `--ubershader-only`; N2-16 counters; N2-19 on the RX 6700 XT and the M2 |
| Option 1b: pipeline creation time and folded-code quality of the large module with constants, on lavapipe and the RX 6700 XT (MoltenVK and KosmicKrisp: `unverified`) | the spike of [3.3](#33-recommendation) point 4 |
| Whether the vertex-stage fallback (ii) of [4.5](#45-when-the-specialised-pipeline-replaces-it) is affordable on the CPU | the CPU path of [8](#8-for-the-planner) (k), which no package builds today, timed on G3 |
| Fragments per output pixel (overdraw) | a GPU measurement (N2-19); the census cannot give it |
| Whether the compare-and-swap shadow loop differs visibly from the interlock path | N2-12 on the Arena scene on both paths (D95) |
| Whether "proven on MH4U" holds for Azahar's renderer | the maintainer's own play in Azahar, or its output on the captured scenes |
| RT64's ubershader and pipeline-list implementation | only its README line was read (`https://github.com/rt64/rt64`, retrieved 2026-10-24: "Uses ubershaders to guarantee no stutters due to pipeline compilation"); its source was not |
| Logic-op results on narrow colour formats | none seen in the census; N2-13 |
| Driver compile time of a pipeline per device, and `minUniformBufferOffsetAlignment` on the RX 6700 XT | the maintainer's run of N2-30's `--rhi` report |

**The census extension that produces the number of unique pipeline keys.** `tools/native/pica_census.py` replays each capture into a register file and decodes `decode_state` per draw; its groups (`group_keys`) are per label, so no group is a key and none is a union across labels. To get the count, a small package (proposed, size S, needs `dump`, owning `tools/native/pica_census.py`, `tests/test_native_pica_census.py` with synthetic captures, and the generated `docs/research/pica-census.md`; [8](#8-for-the-planner) (d)) adds to the tool, with groups that do not presuppose any option:

1. In `decode_state`: the blend factors (`factor_source_rgb`, `factor_dest_rgb`, `factor_source_a`, `factor_dest_a`), the colour write mask bits, the cull mode, the scissor mode, `depthmap_enable`, the wrap modes of each unit separately, `fog_flip`, `texture2_use_coord1`, the shadow orthographic flag, the procedural-texture fields of Azahar's `ProcTexConfig`, and the lighting fields of `LightConfig` (per-light flags, LUT input, absolute flag, scale), all as Azahar's `FSConfig::FSConfig(regs)` reads them (`pica_fs_config.cpp`).
2. In `group_keys`: new groups `fs_key` (the canonical fragment key of [4.3](#43-the-pipeline-key-and-how-many-keys-there-are)), `fixed_function` (the blend factors and equations, colour write mask, logic op, depth, stencil, cull, front face, as the registers request them), `vertex_stage` (the vertex program hash, the vertex layout, the topology), `proctex_config`; plus a joint table of lighting on against geometry shader on. Any option's pipeline-key count is then the count of distinct tuples of these groups (plus the pass kind and the logical formats, which the census already has).
3. A **union across labels**: a new `--union` option that, after the per-label analysis, counts every group, and the joint tuple of `fs_key`, `fixed_function` and `vertex_stage`, over all labels together (the same register-value tuples, de-duplicated across captures), printing the count and the per-label contribution.

The command, on the PC with the seven captures staged under `build/golden/` (the paths of `docs/native/golden.md`):

```
python3 tools/native/pica_census.py \
    'G1=build/golden/G1/gpu-*.mgpu' 'G2=build/golden/G2/gpu-*.mgpu' \
    'G3=build/golden/G3/gpu-*.mgpu' 'G4=build/golden/G4/gpu-*.mgpu' \
    'G9a=build/golden/G9a/gpu-*.mgpu' 'G9b=build/golden/G9b/gpu-*.mgpu' 'G9c=build/golden/G9c/gpu-*.mgpu' \
    --union --json build/pica-census-union.json --md build/pica-census-union.md
```

(`--union` is the proposed option; the rest is the tool's documented form.) **The tool's `--md` default is `docs/research/pica-census.md`, the generated census, and the documented command of the tool writes there: run as written with that default, it would overwrite the generated document.** The proposed package must say so, keep the existing groups byte-identical, and write the extension to separate files, as the command above does. The output holds counts, register values, enumerator names and hashes only, as the census does.

## 8. For the planner

**Every proposed edit to a shared file is listed once here, with the file and the smallest correct change. None has been made, and no package may start on the strength of this document before D5 is decided.** Each item is marked **APPLY NOW** (it holds under any D5 outcome) or **AFTER D5** (it presupposes an option).

### APPLY NOW

- **(a) `docs/native/wp/N2a.md`, N2-26's text.** `rhi.h` does not exist yet (N2-26 is `todo` in `STATUS.md`). Items 1 and 2 are APPLY NOW: they hold under every D5 outcome (they follow from D4's fix-up route); item 3 is AFTER D5, or optional insurance: a hedge for option 1b. **All three touch N2-26's text. In this repository N2-26 waits on N2-25 and N2-24 and may be executed by a separate session, so they go to whoever owns N2-26, or become an orchestrator change to the frozen header after N2-26 merges: they are not edits for the batch PR that carries this document.** None touches N2-25's or N2-29's text.
  1. **`geometry_shader` becomes a maskable capability bit** (holds under every D5 outcome) (mask name `geometry_shader`; today `Capabilities` lists it among the values and the mask "only turns bits off"). Without it the M2's "no quaternion fix-up" route ([5.5](#55-the-quaternion-fix-up-per-device)) cannot be run on lavapipe through the mask, which N2-07's own *Spec* requires of every path.
  2. **`GraphicsPipelineDesc` takes an optional geometry stage** for the fixed geometry shader (holds under every D5 outcome). N2-26 says only "shaders". Plume's own description has one (`RenderGraphicsPipelineDesc::geometryShader`, `types.h:1281`, stage added at `vk.cpp:1440-1449`, `rhi-spike 6.1` G2); `native/third_party/rhi` does not exist in this tree (N2-25 is unmerged) and N2-25's spec does not remove it, so whether N2-26's wrapper description carries it is `unverified`. **Who builds that shader:** Azahar chooses it on the vertex side (`PipelineCache::CalcExtraConfig`, `vk_pipeline_cache.cpp`, `use_geometry_shader`, with `ExtraVSConfig`) and generates it in `glsl_shader_gen.cpp` (`GenerateFixedGeometryShader`). Proposal: `lighting/` (N2-09) owns the generator, as a function of the vertex stage's output attributes; the composition (N2-08, generic) wires it into the pipeline; N2-06 exposes the output-attribute map ((h) below).
  3. **An optional list of specialisation constants per stage** in the pipeline description (plume has the `uint32` pairs: `types.h:1244-1254`, `rhi-spike 6.3`). **Hedge, unused unless option 1b** (under option 1 the profile switches are `#define`s): AFTER D5, or optional insurance at the discretion of the session that executes N2-26; adding an unused optional field is cheap before the freeze and a frozen-header change after it.
- **(b) `docs/native/DECISIONS.md`, D5, "2026-10-23, evidence": correct two statements.** "826 draws of the Arena fight" is not wrong, it is stale: the 826 comes from the first census (commit `abaa1dd`, before G3 was recorded again); the regenerated census (`d4391dc`) has 800 for G3 (21.92 percent). "One procedural-texture configuration" was never in either census, which record only on or off for the procedural texture. The origin of the sentence is D5's own evidence (commit `a8369f7`, 2026-10-23 08:06 UTC), which predates `rhi-spike.md`'s first commit (`56d6cd6`, 09:10 UTC): the spike repeated D5 (section 2, first paragraph, marked `unverified`), not the reverse (checked with `git show`).
- **(c) The error in the N2-07 *Spec* itself** (`docs/native/wp/N2a.md`, the bullet "the shadow write and min/max emulation are GLSL-only in Azahar", which came from `rhi-spike` 9.5, N2-07 bullet): "if D5 ports Azahar's generators (its option 2), both generators are needed" should read: "Azahar's SPIR-V generator is a setting (`spirv_shader_gen`); with it off the GLSL generator alone draws shadows and min/max, so option 2 needs both generators only if the SPIR-V generator is kept as the fast path". D5's own option text is not affected.
- **(d) A census extension, as a small package** (S, needs `dump`; owns `tools/native/pica_census.py`, `tests/test_native_pica_census.py`, `docs/research/pica-census.md`). Content, groups and the command are in [7](#7-unverified-and-the-census-extension). It does not presuppose an option: its groups are `fs_key`, `fixed_function`, `vertex_stage`, `proctex_config`, the blend factors and a joint table of lighting against geometry shader. The package must state that the tool's documented command overwrites the generated `docs/research/pica-census.md`.

### AFTER D5

"After D5" means after the maintainer decides, not that every item presupposes an option: (e), (h), (k) and (l) hold under every option, and only the planner's or the orchestrator's call is awaited for them; (f) and (j) presuppose the option-1 family, (g) options 1 and 1b.

- **(e) N2-16, two sentences that cannot hold under any option with plume.** (1) The *Spec*: "such a draw always goes through the ubershader, never skipped". (2) The *Done when*: "(with the dump) G3's capture with a cold cache never blocks on a compilation, and passes the thresholds with and without the ubershader". Plume bakes blend, depth and stencil, so the fallback pipeline for a never-seen fixed-function state, and for a new vertex stage, is itself a compile ([4.5](#45-when-the-specialised-pipeline-replaces-it)). The smallest change: a fallback that does not exist is built synchronously once, counted (`pipeline_sync_builds`) and listed, and the *Done when* allows only those counted builds. With the recommended fallback (ii) of [4.5](#45-when-the-specialised-pipeline-replaces-it), the CPU path of (k) below must exist (more than N2-06's interpreter, today "in the tests only" (`N2a.md`, N2-06 *Spec*), promoted into the product: it also needs primitive assembly with the fix-up and a streamed batch, and no package builds it); with (i) nothing is built and the counted builds also include each new vertex program.
- **(f) `--ubershader-only`** beside N2-16's `--no-ubershader`, plus a per-frame GPU time with it forced (N2-28's `FrameGpuTimeNs` exists), for N2-16 and N2-19; needed only if D5 picks 1, 1b or 2'. It is the same switch as N2-08's `--force-ubershader` ([4.7](#47-the-interface-between-the-command-processor-and-the-generator)); N2-16 keeps that name.
- **(g) Both-modes equality requirements** (only under options 1 and 1b). The text of [3.1](#31-the-options-and-the-evidence), [4.4.1](#441-one-source-two-modes) and [4.5](#45-when-the-specialised-pipeline-replaces-it) depends on tests that are not in any *Done when* today: N2-16's *Done when* requires the same image with forced misses, but those of N2-08 (no ubershader-mode test), N2-09 to N2-12 and N2-23 require formulas on synthetic draws, and N2-20's thresholds on captures. Proposed added line for N2-08, N2-09, N2-10, N2-11, N2-12 and N2-23: "the same synthetic draws render equal, within the thresholds, in the specialised mode and in the ubershader mode" (under 1b: with the constants and with the block).
- **(h) N2-06.** Add the contract of the 7 varyings ([4.1](#41-data-flow)): the vertex stage writes `primary_color`, `texcoord0` to `texcoord2`, `texcoord0_w`, `normquat` and `view` at Azahar's fixed locations, zero where the program leaves one unwritten (N2-06's *Spec* gives only an output map in the cache key), and exposes the vertex stage's output-attribute map to the composition for the fixed geometry shader.
- **(i) N2-08, N2-13, N2-09, N2-12.** (1) N2-13's *Spec*: the output-stage functions are plain GLSL with the signatures of [4.7](#47-the-interface-between-the-command-processor-and-the-generator) (`mh4n_logic_op`, `mh4n_minmax_blend`), written in `fbops/` and tested in its own shaders; also `fbops::MakeFixedFunctionState(const pica::PicaState&)` (a name proposed here), which fills `rhi.h`'s `FixedFunctionState` in its requested form, with the device profile applied by `backend/state` when the pipeline is built ([4.7](#47-the-interface-between-the-command-processor-and-the-generator)); no allowlist change (the alternative of `fragment/output/**` is withdrawn). (2) N2-08's *Spec*: the prototype block and the stubs of every function in it ([4.8](#48-the-feature-interface)); the reserved gas key word and uniform region owned by `fog/`, with the `needs-decision` rule; the generic composition slots of [4.1](#41-data-flow) (pass kind, fix-up wiring, `dst_copy`, bindings), because N2-09 and N2-12 have no `native/` in their allowlists; the `PipelineKey` type and `PipelineProvider` in `native/src/gpu/native/pipeline_key.h` ([4.7](#47-the-interface-between-the-command-processor-and-the-generator)). (3) N2-08 may not fit one L ([3.1](#31-the-options-and-the-evidence)): a split as 08a (key, uniforms, composition) and 08b (generator, ubershader, TEV, `--gpu native`) is recommended to the planner (`inference` from the token estimate). (4) N2-08's *Spec* also gains the default `PipelineProvider` and the debug option `--force-ubershader` ([4.7](#47-the-interface-between-the-command-processor-and-the-generator)).
- **(j) A spike for option 1b** (S, not an existing package; only if D5 stays in the option-1 family; `Needs`: `pc` for the RX 6700 XT; the M2 leg waits for the Mac and is the remaining risk):
  - **Input.** A skeleton of `uber_color` written against `cfg`: the six TEV stages with their switches on sources, modifiers and operations, the light loop with its LUT fetches, and the output stage. It is a **lower bound** for the full module's compile time (it has no procedural texture, fog or sampling code), which is acceptable because it contains the two constructs whose folding is in doubt (a `switch` on a constant, a loop with a constant bound); the full module is re-measured when N2-08 has it.
  - **Keys.** About 24 synthetic keys whose TEV-stage count, light count (0 to 3) and LUT sets follow the census's mix (Config0 and Config1, the five LUT sets of [2.1](#21-what-the-captures-use)); no game data.
  - **Metrics and how they are read.** Pipeline-creation time around `vkCreateGraphicsPipelines` with a cold `VkPipelineCache` per run, median and p95 over the keys, three runs; instruction and register counts from `VK_KHR_pipeline_executable_properties` where the driver exposes it (`unverified` per driver, so time may be the only metric on some).
  - **Baseline.** Option 1's full miss latency for the same keys: literal GLSL, glslang, then pipeline creation.
  - **Proposed pass thresholds (proposals, for the planner to set):** folded code within about 10% of the literal version in instructions and registers, and 1b's miss latency (pipeline creation only) no worse than option 1's (glslang plus creation).
  - **Harness and files.** A standalone volk program using Vulkan's `VkSpecializationInfo` (what plume's `uint32` pairs map to, `inference`) before N2-26 exists, and the same through `rhi.h` after it; it would own `native/tools/spike_1b/**` and `docs/research/spike-1b.md`. **Dependencies:** volk comes from N2-25 (unmerged) or the system loader; the baseline needs glslang (apt `glslang-tools`, or N2-06's copy).
- **(k) Who builds the CPU path for the geometry-shader draws** (holds under every option). The path (a CPU shader runner for the vertex and geometry shaders, primitive assembly with the quaternion fix-up, a streamed vertex batch) is assumed by the N2-07 *Spec*'s "on the CPU for the geometry-shader draws" (`N2a.md:213`), by `rhi-spike 5.4`, and by fallback (ii) of [4.5](#45-when-the-specialised-pipeline-replaces-it), and affects 38.99 to 98.30 percent of the draws; no package builds it. N2-06's Goal translates the geometry shaders to SPIR-V, while the N2-07 *Spec* assumes CPU draws for them; N2-05's `VertexLoader` port and N2-06's interpreter are "in the tests only" (`N2a.md:172, :189`). The planner decides which of the two is true and which package owns it.
- **(l) The gas table has no data source** (holds under every option). `GAS_LUT_DATA` is a data port; `PicaState.regs` keeps its last word only, and neither N2-04's processor nor Azahar's `pica_core.cpp` has a gas side effect ([5.11](#511-gas-stays-open)). Either a gas-table side effect in `gpu/pica/` (N2-04's module, which is done, so the orchestrator's call) or a `needs-decision` from N2-10.

## 9. Coverage of the N2-07 *Spec*

| Spec bullet (`N2a N2-07`) | Section |
|---|---|
| which state goes into specialisation constants, uniforms, separate pipelines | [4.2](#42-where-each-piece-of-state-goes) (specialisation constants: the last row and the paragraph after it; option 1b in [4.4.1](#441-one-source-two-modes)) |
| the pipeline key, and how many unique keys the captures have | [4.3](#43-the-pipeline-key-and-how-many-keys-there-are) (the count is `unverified`; the bounds that can be derived; the command in [7](#7-unverified-and-the-census-extension)) |
| the ubershader: coverage (TEV, sampling, lighting, fog and gas, procedural textures, shadows), how it reads the state, cost per pixel at 3440×1440, when the specialised pipeline replaces it | [4.4.1](#441-one-source-two-modes), [4.4.2](#442-what-it-covers-and-how-it-reads-the-state), [4.4.3](#443-cost-per-pixel-at-34401440), [4.5](#45-when-the-specialised-pipeline-replaces-it) |
| precompiled pipeline lists: what they record, where they live, how they are built from captures and play | [4.6](#46-precompiled-pipeline-lists) |
| the interface between `DrawCall` and the fragment generator | [4.1](#41-data-flow), [4.7](#47-the-interface-between-the-command-processor-and-the-generator) |
| the feature interface (`sampling/`, `lighting/`, `fog/`, `proctex/`, `shadow/`): key, generated code, ubershader, uniforms | [4.8](#48-the-feature-interface) |
| logic ops, shadows, gas and any feature the RHI lacks, each path chosen by a capability bit | [5](#5-paths-the-rhi-lacks-what-d4-fixed) (the table at its head; the three rows that are not a plain bit) |
| D4: shadow writes (zero-attachment pass, `interlock`, compare-and-swap, texel fetch lookup, out of range lit) | [5.1](#51-shadow-writes-and-the-shadow-lookup) (`rhi-spike` 5.2, 5.3) |
| D4: logic ops (hardware with the `logic_op` bit and blending off; all 16 by emulation; the feedback copy) | [5.2](#52-logic-ops) (`rhi-spike` 5.6, 6.1 N1, 9.4 item 6, 9.5; 9.3 in [2.2](#22-what-d4-fixed)) |
| D4: min/max blending (hardware only when both factors are One; else the feedback copy) | [5.3](#53-minmax-blending) (`rhi-spike` 5.6) |
| D4: depth (the shader writes it; no early depth test) | [5.4](#54-depth) (`rhi-spike` X2 in section 2) |
| D4: the quaternion fix-up per device | [5.5](#55-the-quaternion-fix-up-per-device) (`rhi-spike` 5.4) |
| D4: samplers (separate texture and sampler) | [5.6](#56-samplers) (`rhi-spike` 6.3) |
| D4: border colours (the `custom_border_color` bit; shader emulation and clamp-to-edge; which state selects it) | [5.7](#57-border-colours) (`rhi-spike` 9.3, 9.4 item 2, in [2.2](#22-what-d4-fixed)) |
| D4: the shadow write and min/max emulation are GLSL-only; "option 2 needs both generators" (corrected: it does not, with the SPIR-V generator off) | [5.8](#58-the-shadow-write-and-minmax-emulation-are-glsl-only-in-azahar) (`rhi-spike` 5.2): the last clause of the *Spec* is corrected there and in [8](#8-for-the-planner) (c) |
| D4: pipeline counts (blend is pipeline state; the count on MoltenVK) | [5.9](#59-pipeline-counts) (`rhi-spike` 9.3, X8 in section 2) |
| D4: uniforms (per-draw offsets over a ring, texel buffers; Azahar's layout) | [5.10](#510-uniforms) (`rhi-spike` 5.7, X5) |
| D4: gas stays open | [5.11](#511-gas-stays-open) (`rhi-spike` 5.5) |
| D5's options, the evidence, the consequences for the waiting packages, a recommendation | [3](#3-the-three-options-of-d5) and [6](#6-what-changes-under-the-other-options) |
| `rhi-spike` sections named by the *Read first* | 2 (needs and X-ids): [2.2](#22-what-d4-fixed), [5.4](#54-depth), [5.9](#59-pipeline-counts), [7](#7-unverified-and-the-census-extension) (its last list); 5.2: 5.1, 5.8; 5.3: 5.1; 5.4: 5.5; 5.5: 5.11; 5.6: 5.2, 5.3; 5.7: 5.10; 6.3: 2.2, 4.4.1; 9.3: 2.2, 5.9; 9.4: 2.2 (items 2, 4, 6, 8), 5.2 (item 6) |

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
