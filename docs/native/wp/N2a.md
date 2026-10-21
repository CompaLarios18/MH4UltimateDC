# N2a · Native renderer from captures

The renderer is built and checked against N0's GPU captures (`MGPU`, N0-05 and N0-22), without the runtime, so this phase runs in parallel with N1. It implements `mh4n::gpu::Gpu` (`native/include/mh4n/gpu.h`, N1-01): the same interface the null GPU of N1-17 implements, so N2b only has to swap one for the other. The phase's exit is in [`../ROADMAP.md`](../ROADMAP.md#5-phases); the workflow is [`../AGENTS.md`](../AGENTS.md); the rules for code in `native/` are at the top of [`N1.md`](N1.md).

**Decisions.** N2-01 settles the RHI (D4) and N2-07 the fragment-pipeline design (D5); vertex shaders follow D27 (Recommended: Azahar's decompiler, compiled with glslang). A WP whose *Done when* ends in a decision stays `review`, with `waiting for D<n>` in its note, until DECISIONS.md marks it `Decided`, so nothing that depends on it starts early. Until then, the WPs after them are written against what the decisions must provide, not against an API. When each is decided, the planner revises the *Spec* sections that depend on it in one short pass; the goals, oracles and *Done when* lists stand.

**The oracle.** Every check in this phase is a capture replay (N2-02):
- per GX command, the memory the GPU wrote against the capture's `GPU_PAGES`;
- per command list, the register writes against `PICA_REG`;
- per frame, the displayed screens against the reference PPMs from Azahar's software renderer at 1x, with the thresholds of [`../ROADMAP.md`](../ROADMAP.md) §6: PSNR ≥ 40 dB, and no 16×16 block with a mean error above 8/255.

Azahar draws no gas at all (its shader generator logs "Unimplemented gas mode", and its software renderer handles only plain fog), so frames that use gas are left out of the image checks (`--skip-gas-frames`) and checked later against a real 3DS ([`LATER.md`](LATER.md), `3ds`).

**Where to run it:** in the cloud on lavapipe (a CPU Vulkan driver: slow, but exact enough for the image thresholds); timings only on the reference PC and the M2.

**Captures** are game data. They are `build/golden/<S>/gpu-<start>.mgpu` (N0-09, N0-10), in the session that recorded them or staged from the maintainer's PC (AGENTS §6). A session that runs these checks needs the dump staged and the captures recorded or staged; G3 and G4's need the save to record. "The G1–G4 captures" means every GPU capture of G1, G2, G3 and G4.

**Where the code goes.** Each part of the renderer is its own module under `native/src/gpu/`: `gx/` (N2-03), `pica/` (N2-04), `vertex/` (N2-05), `shader/` (N2-06), `backend/` (N2-21), `queue/` (N2-22), `fbops/` (N2-13), `texture/` (N2-14), `fragment/` (N2-08, with one directory per feature: `sampling/`, `lighting/`, `fog/`, `proctex/`, `shadow/`), `native/` (the composition registered as `--gpu native`, N2-08), `surface/` (N2-15), `pipeline/` (N2-16), `hd/` (N2-18). GPUs, GPU wrappers, capture-player options, checks and report columns register themselves (N1-01's registry), so no WP edits the capture player after N2-02. A tool's logic is a module under `native/src/` (`gpu_capture/`, `vkprobe/`, `hdpack/`), so tests can link it; its `native/tools/` directory holds only `main`.

---

### N2-01 · RHI spike
- **Size / agent:** M · native-worker-high, then the maintainer
- **Depends on:** N0-13, N1-01 · **Needs:** ci, pc, mac
- **Goal:** settle D4 with evidence: which of the features MH4U needs (from the PICA census) the RX 6700 XT (Windows driver), the M2 (MoltenVK, and Metal) and lavapipe offer, and what each RHI candidate costs: plume as it is, plume extended, or a thin RHI of our own.
- **Read first:** D4; `docs/research/pica-census.md` (N0-13) and `docs/research/metal.md`; plume's README and headers (https://github.com/renderbag/plume); Azahar's `src/video_core/renderer_vulkan/vk_instance.cpp` (the features and extensions its Vulkan renderer checks for, and its fallbacks).
- **Files (allowlist):** `native/src/vkprobe/**` (new: the probe and the JSON reader), `native/tools/vkprobe/**` (new: `main`), `native/tests/vkprobe_test.cpp`, `docs/research/rhi-spike.md` (new).
- **Spec:**
  - **`mh4n_vkprobe`** prints, for each Vulkan device, the features MH4U's rendering needs, each with its fallback when absent:
    - logic ops (`logicOp`);
    - fragment shader interlock (pixel and sample), and storage image writes from fragment shaders;
    - geometry shaders; fragment shader barycentrics (the quaternion fix-up without geometry shaders);
    - triangle fans (absent under the portability subset, so on MoltenVK); 8-bit indices (`VK_EXT_index_type_uint8`);
    - custom border colours; `shaderClipDistance`; `VK_EXT_shader_stencil_export`;
    - `VK_EXT_rasterization_order_attachment_access`;
    - depth-stencil formats (`D24_UNORM_S8_UINT` is missing on AMD; the fallback is `D32_SFLOAT_S8_UINT`);
    - BC7;
    - dynamic rendering; `VK_EXT_graphics_pipeline_library`; push descriptors; timeline semaphores; `VK_EXT_extended_dynamic_state` 1 to 3.
  - **`mh4n_vkprobe --from-json FILE`** reads `vulkaninfo --json` output instead of a live device, so a machine without our build can be evaluated.
  - **The target machines:** the maintainer runs `vulkaninfo --json` on the PC and the Mac; `rhi-spike.md` starts with the procedure for him. Until he does, use reports for the same GPU, OS and a recent driver from https://vulkan.gpuinfo.org, saying which.
  - **`rhi-spike.md`:**
    - the probe's output for lavapipe, the RX 6700 XT and the M2;
    - a table of the census's needs against plume's API: what it exposes, what needs extending, what has no equivalent;
    - for Metal: which needs framebuffer fetch and raster order groups meet (the rule of `metal.md`);
    - the options with their costs, and a recommendation.
  - The planner reviews the recommendation; the maintainer decides D4.
- **Done when:**
  - *(CI)* `mh4n_vkprobe` builds in CI and runs on lavapipe; `--from-json` reads a synthetic `vulkaninfo` file in a ctest;
  - `rhi-spike.md` has the three devices' outputs and a recommendation;
  - D4 is `Decided` in DECISIONS.md (the WP stays `review` until then).
- **Traps:** on Windows the AMD driver is AMD's own, not RADV; the spike exists to find out what that driver really exposes.
- **Out of scope:** building the RHI (N2-21).

### N2-02 · Capture player and comparison harness
- **Size / agent:** M · native-worker-high
- **Depends on:** N0-22, N1-01 · **Needs:** dump-check
- **Goal:** `mh4n_capture_player` replays an `MGPU` capture into any `mh4n::gpu::Gpu` implementation and checks it against the capture: memory per command, registers per command list, images per frame, with the thresholds, and times every frame. Later WPs plug their GPUs, options, checks and report columns into it without editing it.
- **Read first:** `docs/native/mgpu-format.md` and `tools/native/gcap.py` (N0-22); `native/include/mh4n/gpu.h`, `registry.h` and `options.h`; in Azahar: `src/video_core/renderer_software/renderer_software.cpp` (how a displayed screen is made from the framebuffer: formats, rotation).
- **Files (allowlist):** `native/tools/capture_player/**` (new), `native/src/gpu_capture/**` (the reader, the capture host and the check interface, shared with tests), `native/include/mh4n/gpu_debug.h` (new), `native/tests/capture_player_test.cpp`, `docs/native/capture-player.md` (new).
- **Spec:**
  - **Reading:** a C++ reader for `MGPU` that mirrors `gcap.py`, and a `CaptureHost` (`gpu::Host`) over a physical memory image of FCRAM and VRAM built from `MEM` records; its `Post` runs the work at once.
  - **`mh4n/gpu_debug.h`:** what a `Gpu` can also implement for the player (found with `dynamic_cast`):
    - `LoadInitState(const InitState&)`: the GPU state at the start of the capture, from the `INIT` record's blocks (registers, shader setups and uniforms, default attributes, the LUTs, the immediate-mode and geometry-pipeline state);
    - the register writes it decoded, in `PICA_REG` form, for `--check registers`;
    - its colour buffers written back to guest memory, for the image checks;
    - `TakeFrameCounters()`: named counters for the frame (draws through the ubershader, HD textures ready, …), printed as extra columns.
  - **Registry:** GPUs register a `gpu::GpuFactory` by name (`--gpu NAME`); GPU wrappers register a `gpu::GpuWrapper` (`std::function<std::unique_ptr<Gpu>(std::unique_ptr<Gpu>, Host&, const Options&)>`, declared in `gpu_debug.h`) by name, and the player applies each wrapper whose same-named flag is set (N2-22's `--queue`); checks register a `CaptureCheck` (`--check NAME`), which sees every record and every frame and adds its own columns; options register in the `capture-player` scope. So a WP adds one of these from its own module.
  - **Replay:** records in order. `INIT` goes to `LoadInitState` before the first `GX`; `CPU_PAGES` go into memory; `GX`, `MMIO`, `SWAP` and `FILL` go to the `Gpu`; `FLUSH` kinds 1, 3 and 4 go to `FlushRegion`, kind 2 to `InvalidateRegion`. `IRQ` records of PDC0 and PDC1 are VBlanks: the player calls `Gpu::VBlank(screen)` there. The other `IRQ` records are what the `Gpu` should signal, compared in order (ids only: the GSP ignores the delays, D25).
  - **Modes:**
    - `--isolated`: `GPU_PAGES` are applied after each command is checked, so every command starts from the reference inputs;
    - `--chained` (the default): they are only compared, so errors build up as they would in the game;
    - `--judge KINDS` (default `fill,transfer,copy,dma,cmdlist,frames`): what is judged. Commands of other kinds are still sent to the `Gpu`, then their `GPU_PAGES` are applied in place of its output and they are reported as skipped; without `frames`, the image checks are reported but not judged. So a GPU that does only part of the work can be checked on that part.
  - **Checks:**
    - after each `GX`, the pages the `Gpu` wrote against `GPU_PAGES`: bit-exact for fills and copies; for draws, through the image thresholds applied to the colour buffer;
    - `--check registers`: the decoded register writes against the `PICA_REG` records;
    - at each `FRAME`, the displayed top and bottom screens (from the `SWAP` information and memory) against the PPMs: PSNR ≥ 40 dB and the 16×16 block rule, both reported per frame;
    - `--skip-gas-frames`: frames whose command lists set fog mode 7 are reported but not judged.
  - **Output:** a table per frame (PSNR, worst block, pass or fail, CPU time spent in the `Gpu`, GPU time from timestamps when the backend has them, the counters and the checks' columns); `--dump-frames DIR` writes the native images and difference images as PPM; exit status 0 when everything passes.
  - **Two GPUs ship with the harness**, to test it: `replay` writes the `GPU_PAGES` itself and must pass; `none` does nothing and must fail at the first draw.
- **Done when:**
  - ctest passes with synthetic captures (made by a C++ writer in the test): `replay` passes; `none` fails at the expected record, and passes with `--judge none`; a 16×16 block uniformly off by 10/255 fails the block rule while PSNR stays above 40 dB; one fully wrong pixel passes both; the `INIT` state reaches the `Gpu` before the first command; each `FLUSH` kind reaches the right call; PDC records become `VBlank` calls; a GPU, a wrapper, a check, an option and a counter registered from another directory are found and shown;
  - (with the dump) `--gpu replay` passes on G1's capture, and the timing table prints.
- **Traps:** -
- **As built (N2-02), for the WPs that plug into the player** (the contracts are `native/include/mh4n/gpu_debug.h` and `native/src/gpu_capture/check.h`; `docs/native/capture-player.md` describes the tool):
  - The four members listed above are `LoadInitState`, `TakeRegisterWrites`, `WriteBackColorBuffers` and `TakeFrameCounters`; `GpuDebug` also has `TakeFrameGpuTimeNs`. A wrapper (N2-22) must implement `GpuDebug` and forward it to the `Gpu` it wraps. A WP that adds a GPU, wrapper or check lists `mh4n_gpu_capture` in its `DEPENDS` and includes `gpu_capture/check.h`; nothing in the player is edited to register one.
  - N2-04's `--check registers` needs only `GpuDebug::TakeRegisterWrites` (an empty optional means "not decoded").
  - Open assumptions, to confirm on the first real capture (dump check): command-list judging on RGB colour buffers, Azahar's off-by-one screen column, the frame and PPM numbering, and the timing columns.
- **Follow-up N2-03b (the capture player):** `GPU_PAGES` of a command that signals an interrupt holds the page of the GSP's shared memory that the GSP service changes inside `GPU::Execute` (the interrupt relay queue; the reference delivers the interrupt at once, D25), which no `Gpu` writes. The player does not judge such a page as the `Gpu`'s output when the capture's change of it, against the capture's memory before the command, is exactly the command's own non-VBlank interrupts pushed into one of the four queues (Azahar's `queue_interrupt`) and the `Gpu` did not touch it; it applies the page to the `Gpu`'s memory and counts it in the summary. It also prints, for a failing fill, transfer, copy or DMA, the decoded command, the output region, the pages and the offsets of differing bytes per page (never bytes), and lists the selected kinds the capture does not exercise (`docs/native/capture-player.md`). Every exact-judged `Gpu` to come (N2-15 on) relies on the same rule; a `Gpu` or wrapper must never write that page.
- **Out of scope:** any real GPU work (N2-03 onwards).

### N2-03 · GX engine
- **Size / agent:** M · native-worker
- **Depends on:** N2-02 · **Needs:** dump-check, save
- **Goal:** memory fill, display transfer (formats, scaling, tiling, flips), texture copy and DMA, bit-identical to Azahar's software implementation, on the CPU. This is the reference the surface cache's GPU versions (N2-15) are checked against.
- **Read first:** in Azahar: `src/video_core/gpu.cpp` (`Execute`, `MemoryFill`, `MemoryTransfer`), `src/video_core/renderer_software/sw_blitter.cpp` (`DisplayTransfer`, `TextureCopy`, `MemoryFill`), `src/video_core/pica/regs_external.h`; 3dbrew *GPU/External Registers*.
- **Files (allowlist):** `native/src/gpu/gx/**`, `native/tests/gpu_gx_test.cpp`.
- **Spec:**
  - The four operations from the GX commands, with every input and output format and flag the registers allow: the 2x downscales, tiled ↔ linear, the vertical flip, the texture-copy gaps.
  - The interrupts each one signals, with Azahar's delays.
  - Port `sw_blitter.cpp`'s logic (D6, with provenance).
  - Registers `--gpu cpu-gx` with the capture player: this engine with nothing for command lists.
- **Done when:**
  - ctest passes: each format pair and flag on synthetic images, against expected bytes worked out from the algorithm;
  - (with the dump) every fill, transfer and copy in the G1–G4 captures matches `GPU_PAGES` bit for bit: `--gpu cpu-gx --isolated --judge fill,transfer,copy,dma` passes.
- **Traps:** -
- **As built (N2-03):** `--gpu cpu-gx` is registered by `native/src/gpu/gx`. It follows Azahar's addresses for the register windows (LCD registers at `0x1ED02000`, GPU registers at `0x1EF00000` and `0x1EF01000`: the GSP base `0x1EB00000` plus the offsets of `WriteHWRegs`). Where Azahar reads or writes out of range or meets a pixel format that does not exist, the engine reads zeros, skips the write and logs, instead of crashing the host. It does nothing for command lists, the display (a buffer swap only updates the registers) or the cache. `GxEngine` is a `Gpu`, not a `GpuDebug`: a capture's INIT record (the GPU and LCD register blocks) is not loaded into it, so its registers start at zero and `--gpu cpu-gx` is only valid for captures driven by GX commands whose registers the capture itself sets (`pica-only` does load them).
- **Follow-up N2-03b:** the six `DISPLAY_TRANSFER` failures of G1 and G2 were not in the engine: `blitter.cpp` and `engine.cpp` were compared line by line with Azahar's `sw_blitter.cpp`, `gpu.cpp`, `regs_external.h` and `color.h` and nothing differs; the failing page is the GSP's relay queue (see N2-02's follow-up). Fill, copy and DMA are not exercised by the G1 and G2 captures: they are not verified. `GxEngine::Dma` for ranges the host cannot translate is still D41's open follow-up.
- **Out of scope:** running these on the host GPU (N2-15).

### N2-04 · PICA command processor
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-02 · **Needs:** dump-check, save
- **Goal:** the command-list decoder and register file, with every side effect of a register write, producing draw calls; its register writes are identical to Azahar's.
- **Read first:** in Azahar: `src/video_core/pica/pica_core.cpp` (command lists, the internal register writes and their side effects), `regs_internal.h` and the other `regs_*.h`, `shader_setup.cpp` (program and uniform uploads), `vertex_loader.cpp` (only to see where draws start); 3dbrew *GPU/Internal Registers* and *GPU/Commands*.
- **Files (allowlist):** `native/src/gpu/pica/**`, `native/tests/gpu_pica_test.cpp`.
- **Spec:**
  - **Decoding:** the header word (register id, parameter mask, extra parameter count, consecutive flag), byte masks, the alignment rule between commands.
  - **Side effects of writes,** as Azahar:
    - the vertex and geometry shader program and swizzle uploads (with their offset registers);
    - float uniforms (f24 in three-word mode, f32 in four-word mode), integer and boolean uniforms;
    - default and fixed attributes;
    - the lighting, fog and procedural-texture LUT uploads;
    - jumps between command lists (the command-buffer address, size and trigger registers);
    - the draw triggers (draw arrays and draw elements) and immediate-mode vertex submission;
    - the geometry-pipeline setup.
  - **The start state:** `LoadInitState` (N2-02's `gpu_debug.h`) fills the register file, shader setups, LUTs and attributes from a capture's `INIT` record.
  - **Output:** a `DrawCall` per draw: the register state and shader setups it needs, and where its vertices come from. A `Backend` interface receives them; this WP comes with a recording backend for tests. Decoded writes are reported through `gpu_debug.h`, and the module registers `--gpu pica-only` for `--check registers`.
- **Done when:**
  - ctest passes: synthetic command lists for each side effect, and a start state loaded from a synthetic `INIT`;
  - (with the dump) `--gpu pica-only --isolated --judge none --check registers` passes on every command list of the G1–G4 captures.
- **Traps:** -
- **As built (N2-04), for N2-05, N2-06, N2-08, N2-16 and N2-22** (the contracts live in the module, included as `gpu/pica/backend.h` and `gpu/pica/state.h`, with `DEPENDS mh4n_gpu_pica`; they are not in `native/include/mh4n`):
  - `Backend::Draw(const DrawCall&)`; a `DrawCall` is `{kind (Arrays, Indexed or Immediate), sequence, vertex_base_address (physical), num_vertices, vertex_offset, index_offset, index_u16, geometry_reconfigured, const PicaState* state}` and carries no vertex data: N2-05 loads vertices from `vertex_base_address` and the loader registers. The `state` pointer is valid only during `Draw`: a backend on another thread (N2-22) copies the `PicaState` (about 105 KB) or the processor runs on the renderer thread. The processor clears the dirty flags (registers, the lighting, fog and procedural-texture tables, the program, swizzle and uniforms of both setups) after `Draw` returns, so they mean "changed since the previous draw returned".
  - Immediate mode emits one `Immediate` draw per full vertex, with `state->immediate.input_vertex`. The geometry pipeline is reconfigured from the registers at every `Arrays` or `Indexed` draw and at the first immediate vertex after a write to `0x232`; its cursors (buffer index, vertex counts, attribute buffer) and the geometry shader's `b15` rule are left to whoever runs the vertex shaders (N2-06). The processor keeps no primitive-assembler state (Azahar's hardware renderers do not either).
  - `PICA_REG` entries are produced per register word written (a repeated-register command gives one entry per word, with the command's raw value; out-of-range ids are not recorded), as `mgpu-format.md` §3.5 describes. `--gpu pica-only` does only `SubmitCmdList` and the MMIO triggers (use it with `--judge none`); it counts draws as frame counters (`pica_draws`, `pica_immediate_draws`, `pica_cmdlists`).
  - Mirrored Azahar quirks, on purpose: handlers get the unmasked word while the register gets the masked one; a refused program or swizzle batch drops the whole batch; the extra words of a command come from the list as it is after the first write; a uniform batch gives up at the first out-of-range index; unit 3 mirrors the vertex unit only when `gs_unit_exclusive_configuration` and `use_gs` are 0. Deviations where Azahar crashes or reads out of bounds: a lighting LUT type of 24 or more is dropped; a fixed-attribute index above `0xF` empties the queue; a jump or list at an unmapped address ends the list; geometry-shader mode 3 and inconsistent register sets are logged and treated as no geometry shader.
  - Assumptions to confirm on the first real capture (dump check): the recorder lists one `PICA_REG` entry per register word, in order; like Azahar, a list whose last command ends on an odd word makes the loop read one more (value, header) pair from the 8 bytes after the list (real lists end with the finalize write, which stops the loop first).
- **Out of scope:** drawing (later WPs).

### N2-05 · Vertex loading
- **Size / agent:** M · native-worker
- **Depends on:** N2-04, N2-01 · **Needs:** dump-check, save
- **Goal:** vertex data from the PICA's vertex arrays into host vertex buffers, exactly as Azahar's `VertexLoader` feeds its vertex shader, in shapes every backend can draw.
- **Read first:** in Azahar: `src/video_core/pica/vertex_loader.cpp`, `src/video_core/pica/regs_pipeline.h`, and how its hardware renderers set up vertex input (`src/video_core/rasterizer_accelerated.cpp`); `docs/research/rhi-spike.md` (N2-01: which backends lack fans and 8-bit indices).
- **Files (allowlist):** `native/src/gpu/vertex/**`, `native/tests/gpu_vertex_test.cpp`.
- **Spec:**
  - The attribute loaders: up to 12 attributes in up to 12 buffers, byte, unsigned byte, short and float components, 1–4 per attribute, strides, the default and fixed attributes, index buffers of 8 and 16 bits, immediate mode.
  - **Shapes:** triangle fans become indexed triangle lists, and 8-bit indices become 16-bit, when the backend lacks them.
  - Output: host buffers and a vertex-input description for the backend, in a form that doesn't depend on D4 (offsets, formats, strides).
  - A CPU reference port of Azahar's `VertexLoader` (in the tests only, with provenance) checks the output.
- **Done when:**
  - ctest passes: synthetic arrays for every format, both index sizes, fans converted;
  - (with the dump) for every draw of the G1–G4 captures, the first 64 vertices loaded equal the CPU reference's.
- **Traps:** -
- **Out of scope:** the vertex shader (N2-06).

### N2-06 · Vertex shader translation
- **Size / agent:** L · native-worker-high
- **Depends on:** N2-04, N2-21 · **Needs:** dump-check, save
- **Goal:** PICA vertex shaders (and geometry shaders, if the census shows any) translated to SPIR-V with the PICA's float rules, compiled when first used and cached by program hash.
- **Read first:** D27; in Azahar: `src/video_core/shader/generator/glsl_shader_decompiler.cpp` (the PICA → GLSL decompiler; `sanitize_mul` near line 828) and `glsl_shader_gen.cpp`, `src/video_core/shader/shader_interpreter.cpp` (the reference semantics; MAX and MIN near line 265), `src/video_core/pica_types.h` (f24, and the multiply rule near line 93); 3dbrew *Shader Instruction Set*; `docs/research/pica-census.md` (geometry shaders).
- **Files (allowlist):** `native/src/gpu/shader/**`, `native/third_party/glslang/**`, `native/tests/gpu_shader_test.cpp`.
- **Spec:**
  - Port Azahar's decompiler (D27), with provenance; compile to SPIR-V with glslang, linked as a library. The cache key is the program and swizzle hashes plus the output map.
  - **The PICA's float rules** where they differ from IEEE: `MUL`, `DP3`, `DP4`, `DPH` and `MAD` give +0 for 0 × ±infinity, while a NaN factor stays NaN; `MAX` and `MIN` order as the hardware does (`src1 > src2 ? src1 : src2` for MAX, and the matching rule for MIN). f24 precision is approximated with f32, as Azahar does.
  - **Oracle:** a CPU port of Azahar's shader interpreter (in the tests only). For each captured draw, run the translated shader on lavapipe through the backend (N2-21) for the draw's uniforms and its first 64 vertices (writing the outputs to a storage buffer), and compare with the interpreter's: equal when both are NaN, or the same infinity, or `|a − b| ≤ 2^-16 × max(1, |a|, |b|)`.
- **Done when:**
  - ctest passes: each instruction against the interpreter on synthetic programs, flow control included (`IFU`, `IFC`, `LOOP`, `CALL*`, `BREAKC`, `JMP*`), and the special cases (0 × inf, NaN operands, MAX/MIN with NaN);
  - (with the dump) every vertex program in the G1–G4 captures passes the oracle.
- **Traps:** compiling with glslang at run time costs milliseconds: the pipeline cache (N2-16) has to hide it.
- **Out of scope:** the fragment pipeline (N2-07 onwards).

### N2-07 · Fragment pipeline design
- **Size / agent:** M · native-worker-high, reviewed by the planner
- **Depends on:** N0-13, N2-01 · **Needs:** none (the census data is in the docs)
- **Goal:** settle D5 in writing: how PICA fragment state becomes shaders and pipelines, the pipeline key, what the ubershader covers and what it costs, and the interface the fragment WPs implement.
- **Read first:** D4 (as decided), D5; `docs/research/pica-census.md`; in Azahar: `src/video_core/shader/generator/pica_fs_config.cpp` (the fragment state key), `src/video_core/shader/generator/glsl_fs_shader_gen.cpp` and `spv_fs_shader_gen.cpp`, `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`; the ubershader approach of RT64 (https://github.com/rt64/rt64).
- **Files (allowlist):** `docs/native/gpu-fragment-design.md` (new).
- **Spec:** the document answers, with the census's numbers:
  - which state goes into specialisation constants, which into uniforms, which into separate pipelines;
  - the pipeline key, and how many unique keys the captures have;
  - the ubershader: what it covers (TEV, sampling, lighting, fog and gas, procedural textures, shadows), how it reads the state, its estimated cost per pixel at 3440×1440, and when the specialised pipeline replaces it;
  - precompiled pipeline lists: what they record, where they live on the player's PC (never in the repository: they come from the game), how they are built from captures and play;
  - the interface between the command processor's `DrawCall` and the fragment generator, and the **feature interface**: what each feature directory (`sampling/`, `lighting/`, `fog/`, `proctex/`, `shadow/`) contributes to the key, the generated code, the ubershader and the uniforms, so N2-23 and N2-09 to N2-12 work in their own directories;
  - how logic ops, shadows, gas and any feature the RHI lacks (from N2-01) are done.
- **Done when:** the planner's review is addressed and the maintainer marks D5 `Decided` (the WP stays `review` until then).
- **Traps:** -
- **Out of scope:** code.

### N2-21 · Backend foundation
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-01, N2-02 · **Needs:** none
- **Goal:** the RHI D4 chose, standing: device and queue selection, memory allocation, render targets and their formats, command submission, readback of render targets into guest memory in the PICA's layouts, and GPU timestamps. Everything that draws builds on it.
- **Read first:** D4 and `docs/research/rhi-spike.md`; in Azahar: `src/video_core/renderer_vulkan/vk_instance.cpp`, `vk_scheduler.cpp` and `vk_texture_runtime.cpp` (what a PICA renderer needs from Vulkan), `src/video_core/texture/texture_decode.cpp` (the tiled layouts written back); `native/include/mh4n/gpu_debug.h`.
- **Files (allowlist):** `native/src/gpu/backend/**`, `native/third_party/rhi/**` (volk, the allocator and the RHI D4 chooses, one subdirectory each), `native/tests/gpu_backend_test.cpp`.
- **Spec:**
  - As D4 decided: the RHI (or plume) with the fallbacks `rhi-spike.md` lists for missing features. Vulkan is loaded at run time through volk (the rules at the top of `N1.md`): with no driver, creating the backend fails with a message, and nothing else does.
  - **Render targets** in the PICA's colour formats (RGBA8, RGB8, RGB5A1, RGB565, RGBA4) and depth formats (D16, D24, D24S8 with its fallback).
  - **Readback:** a render target written back into guest memory in the PICA's Morton-tiled layout and format, bit-exact for what was drawn: what the capture player's draw checks compare. Only on demand (checks, and the CPU reads N0-14 found).
  - **Timestamps** per frame for the capture player's timing table.
  - Runs on lavapipe in the cloud.
- **Done when:** ctest passes on lavapipe: clear-and-readback of every colour and depth format against bytes worked out from the formats; a timestamp query returns increasing values.
- **Traps:** -
- **Out of scope:** shaders and drawing state (N2-06, N2-08 onwards).

### N2-22 · Renderer thread and synchronisation
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-21, N2-03, N2-04 · **Needs:** dump-check, save
- **Goal:** principles 4 and 6 for the renderer: GX commands are processed on a renderer thread of their own; interrupts are signalled in order; flushes and invalidates are honoured; anything the renderer writes into guest memory happens at the scheduler's safe points.
- **Read first:** [`../ROADMAP.md`](../ROADMAP.md) §3, principles 4 and 6; `native/include/mh4n/gpu.h` (`Host::Post`, `SignalInterrupt`); `docs/research/flat-memory-check.md` (N0-14), if it exists; `docs/research/system-services.md` (the engine's one frame of lag).
- **Files (allowlist):** `native/src/gpu/queue/**`, `native/tests/gpu_queue_test.cpp`.
- **Spec:**
  - A `Gpu` that wraps another: `Execute`, `WriteReg`, `SetBufferSwap`, `SetColorFill`, `FlushRegion` and `InvalidateRegion` queue work for the renderer thread and return at once.
  - Interrupts go back through `Host::SignalInterrupt` in the order the commands complete.
  - `InvalidateRegion` (the CPU is about to read) waits until earlier commands touching the range are done and written back; `FlushRegion` marks cached data stale before later commands run.
  - Writes into guest memory (fills, transfers, readbacks) are handed to the scheduler with `Host::Post`, except into buffers the request lent (principle 6).
  - At most one frame in flight beyond the one being built (the engine's lag); more blocks the queue, not the game thread, and the counter of such waits is reported (`TakeFrameCounters`).
  - Registered as the GPU wrapper `queue`: the capture player's `--queue` wraps the chosen GPU in it.
- **Done when:**
  - ctest passes: command order and interrupt order under a fake host; an invalidate that waits for an earlier transfer; writes arriving only through `Post`;
  - (with the dump) the capture player in `--chained` mode gives the same results with `--queue` as without it on the G1–G4 captures.
- **Traps:** -
- **Out of scope:** the game runtime's use of it (N2b).

### N2-13 · Framebuffer operations
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-07, N2-21 · **Needs:** none
- **Goal:** depth and stencil tests and writes, blending (equations, factors, the constant colour), logic ops, colour and depth masks, and the PICA's depth range and offset, as the software renderer does them.
- **Read first:** `docs/native/gpu-fragment-design.md`; in Azahar: `src/video_core/renderer_software/sw_framebuffer.cpp`, `src/video_core/pica/regs_framebuffer.h`, how its Vulkan renderer maps them (`src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp`, `pica_to_vk.h`).
- **Files (allowlist):** `native/src/gpu/fbops/**`, `native/src/gpu/backend/**`, `native/tests/gpu_fbops_test.cpp`.
- **Spec:** as the design document says, including logic ops on backends without them (N2-01).
- **Done when:** ctest passes on lavapipe: each test, blend mode and logic op on synthetic draws, against expected pixels from the software renderer's formulas. (The captures check them in N2-23 and N2-20.)
- **Traps:** -
- **Out of scope:** -

### N2-14 · Textures: decoding, hashes, cache
- **Size / agent:** M · native-worker
- **Depends on:** N2-02 · **Needs:** dump-check, save, pc
- **Goal:** every PICA texture format decoded from its tiled layout, bit-identical to Azahar; textures hashed exactly as Azahar hashes them for custom textures, so the community's HD pack finds its textures; a texture cache keyed by address, format and contents.
- **Read first:** in Azahar: `src/video_core/texture/texture_decode.cpp` and `etc1.cpp`; `src/video_core/rasterizer_cache/rasterizer_cache.h` (`ComputeHash`, near line 1076); `src/video_core/custom_textures/custom_tex_manager.cpp` (file names `tex1_<w>x<h>_<hash>_<format>`, `pack.json`, `use_new_hash`; no `pack.json` means the old hash, near lines 99 and 322); `src/common/cityhash.cpp`; `docs/research/flat-memory-check.md` (N0-14), if it exists.
- **Files (allowlist):** `native/src/gpu/texture/**`, `native/third_party/xxhash/**` (only for the cache's own content hash), `native/tests/gpu_texture_test.cpp`, `tools/native/hd_hash_check.py` (new), `tests/test_native_hd_hash_check.py` (new).
- **Spec:**
  - Decoders for RGBA8, RGB8, RGB5A1, RGB565, RGBA4, IA8, RG8, I8, A8, IA4, I4, A4, ETC1 and ETC1A4, from the Morton-tiled layout. Port Azahar's (D6, with provenance).
  - **The custom-texture hash:** CityHash64 in both modes (port `src/common/cityhash.cpp` exactly, with provenance). With `use_new_hash`, over the raw uploaded bytes; otherwise (and when the pack has no `pack.json`), over the texture decoded with `convert = false`.
  - **The cache:** by address, format and size, with a content hash checked when a flush or invalidate says the memory may have changed (principle 2), and as the safety net N0-14 recommends (until it reports, on every flush and invalidate). This content hash may be XXH3 (vendored, BSD-2-Clause).
  - **`--check tex-hashes`** (a capture-player check registered here) prints, for each texture a capture's draws use, `<hash> <w>x<h> <format>` in both hash modes.
  - **`hd_hash_check.py HASHES PACK_DIR`:** given that output and a pack folder, reports how many of the captured textures have a file in the pack.
- **Done when:**
  - ctest passes: each format on synthetic textures, against bytes worked out from the formats; CityHash64 against its published test vectors; both hash modes fed the right bytes; the check's output on a synthetic capture;
  - `python3 -m pytest tests/test_native_hd_hash_check.py` passes;
  - (with the dump) every texture in the G1–G4 captures decodes equal to the CPU reference;
  - (the maintainer, with the pack) `hd_hash_check.py` on G3's capture finds pack files for the textures the pack replaces.
- **Traps:** -
- **As built (N2-14):**
  - Module `mh4n_gpu_texture` (DEPENDS `mh4n_gpu_capture`, `mh4n_gpu_pica`); the content hash of the cache is XXH3 from the vendored xxHash 0.8.3 (`native/third_party/xxhash/`, BSD-2-Clause). `cityhash.{h,cpp}` are MIT (Google's notice only), not GPL.
  - `--check tex-hashes` writes a file (option `--tex-hashes-out FILE`, default `<capture>.tex-hashes.txt`) with the sections `# use_new_hash=1` and `# use_new_hash=0` and lines `<16 hex digits> <w>x<h> <format number>`. It runs its own `gpu/pica` command processor over the capture's GX and MMIO records on the capture host's memory, so it works with any `--gpu` and needs nothing from the Gpu. Level 0 only; shadow textures are skipped; cube faces are hashed as six textures; an old-mode hash of a texture whose size is not a multiple of 8 is refused, not guessed; a texture memory that a command list draws into is hashed as it is after that list.
  - `docs/research/flat-memory-check.md` (N0-14) does not exist yet, so the cache's safety net (every flush and invalidate marks every texture for rehash, `Config::check_all_on_notify`) is on by default.
- **Out of scope:** loading HD textures (N2-17, N2-18).

### N2-08 · Fragment generator, TEV and alpha test
- **Size / agent:** L · native-worker-high
- **Depends on:** N2-03, N2-05, N2-06, N2-13, N2-14, N2-21 · **Needs:** none
- **Goal:** the fragment generator and the ubershader as the design document describes them, with the six TEV stages (sources, operands, operations, scales, the combiner buffer) and the alpha test, the feature directories with stubs for the features that come next, and `--gpu native`, the renderer assembled.
- **Read first:** `docs/native/gpu-fragment-design.md`; in Azahar: `src/video_core/renderer_software/sw_rasterizer.cpp` (reference semantics), `src/video_core/shader/generator/glsl_fs_shader_gen.cpp` and `spv_fs_shader_gen.cpp` (how its renderers generate them); `src/video_core/pica/regs_texturing.h`.
- **Files (allowlist):** `native/src/gpu/fragment/**`, `native/src/gpu/native/**` (new), `native/src/gpu/backend/**` (what drawing needs), `native/tests/gpu_tev_test.cpp`.
- **Spec:**
  - As the design document says for the generator, the ubershader, TEV and the alpha test.
  - **Feature directories** `fragment/{sampling,lighting,fog,proctex,shadow}/`, each with a stub that implements the design's feature interface with the feature off, and reports a draw that needed it (a `stubbed:<feature>` counter, `TakeFrameCounters`).
  - **`--gpu native`** (registered from `native/src/gpu/native/`): N2-03's GX engine, N2-04's command processor, N2-05's vertex loading, N2-06's shaders, this fragment pipeline, N2-13's framebuffer operations and N2-14's textures, on N2-21's backend.
- **Done when:** ctest passes: synthetic untextured draws per TEV operation, source and scale, and per alpha-test function, rendered on lavapipe, against expected pixels from the software renderer's formulas; a draw that needs a stubbed feature is counted.
- **Traps:** -
- **Out of scope:** the features (N2-23, N2-09 to N2-12).

### N2-23 · Texture sampling and the first captured frames
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-08 · **Needs:** dump-check
- **Goal:** the three texture units (2D, cube and projection lookups; wrap and clamp modes, border colour, filters, LOD and bias; the procedural slot's routing), so the first captured frames draw end to end.
- **Read first:** `docs/native/gpu-fragment-design.md` (the feature interface); in Azahar: `src/video_core/renderer_software/sw_texturing.cpp`, the texturing parts of `src/video_core/shader/generator/glsl_fs_shader_gen.cpp`; `src/video_core/pica/regs_texturing.h`.
- **Files (allowlist):** `native/src/gpu/fragment/sampling/**`, `native/src/gpu/backend/**` (samplers), `native/tests/gpu_sampling_test.cpp`.
- **Spec:** as the design document says for sampling; shadow lookups are N2-12's.
- **Done when:**
  - ctest passes: synthetic draws per texture type, wrap mode, filter and LOD setting on lavapipe, against expected pixels from the software renderer's formulas;
  - (with the dump) on G1's capture with `--gpu native`, the frames with no draw counted as `stubbed:*` pass the image thresholds; at least one frame qualifies (if none does, report which features block them).
- **Traps:** -
- **Out of scope:** lighting, fog, procedural textures, shadows (N2-09 to N2-12).

### N2-09 · Fragment lighting
- **Size / agent:** L · native-worker-high
- **Depends on:** N2-08, N2-23 · **Needs:** none
- **Goal:** the PICA's per-fragment lighting: up to 8 lights, the LUTs (D0, D1, Fresnel, reflection RGB, spot, distance attenuation), bump and tangent mapping, clamping, the quaternion interpolation and its fix-up, as the software renderer does it.
- **Read first:** `docs/native/gpu-fragment-design.md`; in Azahar: `src/video_core/renderer_software/sw_lighting.cpp`, `sw_clipper.cpp` (`FlipQuaternionIfOpposite`, the fix-up), the lighting part of `src/video_core/shader/generator/glsl_fs_shader_gen.cpp`, `src/video_core/pica/regs_lighting.h`.
- **Files (allowlist):** `native/src/gpu/fragment/lighting/**`, `native/tests/gpu_lighting_test.cpp`.
- **Spec:** as the design document says; the quaternion fix-up as Azahar's renderers do it.
- **Done when:** ctest passes: synthetic lit draws against the software renderer's formulas. (Captures check lighting in N2-20.)
- **Traps:** -
- **Out of scope:** shadows (N2-12).

### N2-10 · Fog and gas
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-08 · **Needs:** none
- **Goal:** depth fog with its LUT as the software renderer does it, and the gas mode (the density accumulation pass and the shading pass) as far as public documentation describes it, for a real-3DS check later.
- **Read first:** `docs/native/gpu-fragment-design.md`; in Azahar: the fog part of `src/video_core/renderer_software/sw_rasterizer.cpp` and of `src/video_core/shader/generator/glsl_fs_shader_gen.cpp` (whose gas case only logs "Unimplemented gas mode"); `src/video_core/pica/regs_texturing.h` (the fog registers; Azahar defines no gas registers); 3dbrew *GPU/Internal Registers* (the gas registers); `docs/research/pica-census.md` (how the game uses gas).
- **Files (allowlist):** `native/src/gpu/fragment/fog/**`, `native/tests/gpu_fog_test.cpp`, `docs/research/gas.md` (new: what is known, from where, what is open, and how it will be checked).
- **Spec:** fog as the software renderer. Gas from 3dbrew's description; `gas.md` writes down, for a synthetic draw, the values the density pass and the shading pass should produce, worked out by hand from that description, and marks what the description leaves open.
- **Done when:** ctest passes: fog on synthetic draws against the software renderer's formulas; the gas passes on `gas.md`'s synthetic draw give its values. (Captures check fog in N2-20; gas waits for the real-3DS capture player.)
- **Traps:** -
- **Out of scope:** the real-3DS capture player (`LATER.md`); listing the captures' gas frames (N2-20).

### N2-11 · Procedural textures
- **Size / agent:** M · native-worker
- **Depends on:** N2-08, N2-23 · **Needs:** none
- **Goal:** the procedural texture unit (noise, the U/V and colour LUTs, the shift and clamp modes, the LOD) as the software renderer does it.
- **Read first:** `docs/native/gpu-fragment-design.md` (the feature interface); in Azahar: `src/video_core/renderer_software/sw_proctex.cpp`, the proctex part of `src/video_core/shader/generator/glsl_fs_shader_gen.cpp`; `docs/research/pica-census.md` (whether and where the game uses it).
- **Files (allowlist):** `native/src/gpu/fragment/proctex/**`, `native/tests/gpu_proctex_test.cpp`.
- **Spec:** as the design document says.
- **Done when:** ctest passes: synthetic procedural textures against the software renderer's formulas. (Captures check them in N2-20.)
- **Traps:** -
- **Out of scope:** -

### N2-12 · Shadows
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-08, N2-13, N2-23 · **Needs:** none
- **Goal:** the shadow pass (the fragment operation mode that writes depth to a shadow map) and shadow lookups (2D and cube), as the software renderer does them.
- **Read first:** `docs/native/gpu-fragment-design.md` (how shadows are done with what the RHI offers); in Azahar: `src/video_core/renderer_software/sw_framebuffer.cpp` (the shadow write), `sw_rasterizer.cpp` (the shadow lookups, near lines 495–569), the shadow parts of `src/video_core/shader/generator/glsl_fs_shader_gen.cpp` (which uses image atomics and interlock where available); `docs/research/pica-census.md`.
- **Files (allowlist):** `native/src/gpu/fragment/shadow/**`, `native/src/gpu/backend/**`, `native/tests/gpu_shadow_test.cpp`.
- **Spec:** as the design document says.
- **Done when:** ctest passes: a synthetic shadow pass and its 2D and cube lookups. (Captures check them in N2-20.)
- **Traps:** -
- **Out of scope:** -

### N2-20 · Fragment integration on the captures
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-09, N2-10, N2-11, N2-12, N2-23 · **Needs:** dump, save
- **Goal:** with every fragment feature in, the G1–G4 captures pass the image thresholds (frames with gas left out), so the renderer is complete enough for the surface cache, the pipeline cache and the budgets.
- **Read first:** `docs/native/capture-player.md`; the reports of N2-08 to N2-13 and N2-23; `docs/research/pica-census.md`.
- **Files (allowlist):** `native/src/gpu/{gx,pica,vertex,shader,backend,fbops,texture,fragment,native}/**`, `native/tests/gpu_{gx,pica,vertex,shader,backend,fbops,texture,tev,sampling,lighting,fog,proctex,shadow}_test.cpp` (fixes; list each one in the report), `docs/research/gas.md` (the gas frames found).
- **Spec:** run the capture player with `--gpu native --skip-gas-frames` on every G1–G4 capture, in `--isolated` and then `--chained` mode; fix what fails, smallest fix first, each with a unit test. If more than three features need fixes, stop after listing them with the evidence, and report `blocked`.
- **Done when:** every G1–G4 capture passes in both modes, no draw is counted as `stubbed:*`, and the gas frames are listed in `gas.md`.
- **Traps:** -
- **Out of scope:** resolution scaling (N2-15); timing (N2-19).

### N2-15 · Surface cache and resolution scaling
- **Size / agent:** L · native-worker-high
- **Depends on:** N2-20, N2-22, N0-14 · **Needs:** dump-check, save
- **Goal:** render targets and display buffers live on the host GPU at the chosen internal resolution; transfers between them run there; guest memory is updated only when the CPU will read it (N0-14's findings) or when a capture check asks for it.
- **Read first:** `docs/research/flat-memory-check.md` (N0-14); `docs/native/gpu-fragment-design.md`; in Azahar: `src/video_core/rasterizer_cache/` (what a surface cache has to handle: overlaps, formats, partial copies, invalidation).
- **Files (allowlist):** `native/src/gpu/surface/**`, `native/src/gpu/gx/**` (GPU paths of fills, transfers and copies), `native/src/gpu/{native,backend}/**` (render targets and readback through the surface cache), `native/tests/gpu_surface_test.cpp`.
- **Spec:**
  - A surface is (physical address, size, format, dimensions, scale). Lookups for render targets, textures and transfers; overlap rules; a surface written back to guest memory (N2-21's readback) when its range is invalidated, read by a transfer the GPU can't do, or checked by the capture player.
  - Fills, display transfers and texture copies between surfaces run on the host GPU, and are checked against N2-03's CPU versions at 1x.
  - Resolution scale 1x to 10x for render targets; transfers and copies scale with them.
  - **`--scale N`** (a capture-player option registered here) renders at N× and compares at 1x (downscaled).
- **Done when:**
  - ctest passes: overlap and invalidation cases on synthetic surfaces; GPU transfers equal to the CPU versions at 1x;
  - (with the dump) the G1–G4 captures pass at 1x in `--chained` mode, and at 3x and 6x against the 1x reference within the thresholds.
- **Traps:** -
- **Out of scope:** presenting to a window (N2b).

### N2-16 · Pipeline cache, ubershader and background compilation
- **Size / agent:** L · native-worker-high
- **Depends on:** N2-07, N2-20, N2-15 · **Needs:** dump-check, save, pc
- **Goal:** no visible stutter from shaders (principle 5): pipelines are compiled ahead of time from a list kept on the player's PC, cached on disk, and while a missing one compiles in the background, the ubershader draws.
- **Read first:** `docs/native/gpu-fragment-design.md`; in Azahar: `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` and `vk_shader_disk_cache.cpp`; RT64's ubershader and pipeline-list approach.
- **Files (allowlist):** `native/src/gpu/pipeline/**`, `native/src/gpu/{native,fragment}/**` (the draw path through the pipeline cache), `native/tests/gpu_pipeline_test.cpp`.
- **Spec:**
  - The pipeline key from the design; a disk cache in the user folder, with the driver's `VkPipelineCache` data and our list of keys; precompiling the list at start-up on worker threads.
  - On a miss: draw with the ubershader at once, compile the specialised pipeline in the background, switch when it is ready.
  - Keys seen while playing are appended to the list, so the next start compiles them.
  - **In the capture player** (options and counters registered here): `--no-ubershader`, `--warm`; per frame, how many draws used the ubershader, and the frame time with a cold and a warm cache.
- **Done when:**
  - ctest passes: the key's stability, the disk cache's round trip, the ubershader fallback (forced misses give the same image within the thresholds);
  - (with the dump) G3's capture with a cold cache never blocks on a compilation, and passes the thresholds with and without the ubershader;
  - (the maintainer) on the reference PC, with a warm cache, no frame of G3's capture takes over 8.3 ms because of a pipeline.
- **Traps:** -
- **Out of scope:** pipelines for the app's own UI (N2b).

### N2-17 · HD pack pre-processor
- **Size / agent:** M · native-worker
- **Depends on:** N2-14 · **Needs:** pc
- **Goal:** (D14) the HD pack, downloaded at setup from its author's own links, is converted on the player's PC into a form that loads fast: BC7 with mipmaps, in one indexed archive keyed by Azahar's hash.
- **Read first:** D14; `tools/mods_catalog.json` and `docs/MODS.md` (how the pack is found and downloaded today); in Azahar: `src/video_core/custom_textures/custom_tex_manager.cpp` and `custom_format.cpp` (file names, PNG and DDS, `pack.json` options, `flip_png_files`).
- **Files (allowlist):** `native/src/hdpack/**` (new: the conversion), `native/tools/hdpack/**` (new: `main`), `native/third_party/bc7/**` (bc7enc_rdo or another encoder under MIT or a public-domain dedication; check and report the licence), `native/third_party/png/**` (a PNG decoder, same rule), `native/tests/hdpack_test.cpp`, `docs/native/hdpack.md` (new).
- **Spec:**
  - `mh4n_hdpack PACK_DIR -o OUT [--threads N]`: reads every texture the pack has (PNG or DDS, both hash modes, `pack.json`'s options), builds the mip chain, encodes to BC7, and writes one archive: an index (hash → offset, size, format, dimensions, mip count) and the data.
  - Resumable: an interrupted run continues where it stopped. A progress line for the setup to show.
  - Never part of a package: it runs on the player's PC, after the download.
- **Done when:**
  - ctest passes: synthetic PNGs (made in the test) of a few sizes become an archive whose index and BC7 blocks decode back within BC7's error;
  - (the maintainer) the real pack converts on the reference PC; the time and the archive's size go into `hdpack.md`.
- **Traps:** -
- **Out of scope:** the setup's UI (N3); streaming (N2-18).

### N2-18 · HD texture streaming and the VRAM budget
- **Size / agent:** M · native-worker-high
- **Depends on:** N2-17, N2-15 · **Needs:** dump-check, save, pc
- **Goal:** HD textures replace the originals as they arrive, loaded in the background (principle 5), within a VRAM budget that evicts what isn't used.
- **Read first:** `docs/native/hdpack.md`; [`../ROADMAP.md`](../ROADMAP.md) §7 (VRAM under the 6700 XT's 12 GB with margin); the texture cache (N2-14).
- **Files (allowlist):** `native/src/gpu/texture/**`, `native/src/gpu/hd/**`, `native/tests/gpu_hd_test.cpp`.
- **Spec:**
  - On a texture-cache miss with a hash in the archive, the original texture is used at once and the HD one is read and uploaded on a worker thread; the next draw after the upload uses it.
  - A VRAM budget (default 8 GB, an option) with least-recently-used eviction of HD textures, which fall back to the originals.
  - **In the capture player** (registered here): `--hd ARCHIVE`, and per frame the HD textures requested, ready and evicted, and the VRAM in use.
- **Done when:**
  - ctest passes with a synthetic archive: the original first, the HD texture a few frames later, eviction under a small budget;
  - (the maintainer, with the real archive) G3's capture with `--hd` on the reference PC: no frame over 50 ms after warm-up, VRAM under the budget, and the picture matching Azahar's with the pack in side-by-side screenshots he takes (automated HD references are in `LATER.md`).
- **Traps:** -
- **Out of scope:** -

### N2-19 · GPU budget measurement
- **Size / agent:** S · native-worker, then the maintainer
- **Depends on:** N2-15, N2-16, N2-18, N1-22, N1-23 · **Needs:** ci, pc, mac
- **Goal:** N2a's part of the exit: do translation and the GPU fit their budgets (3 ms and 6 ms at 120 fps) at 3440×1440 with the HD pack, on the RX 6700 XT? And how does the M2 do at 3x?
- **Read first:** [`../ROADMAP.md`](../ROADMAP.md) §7; `docs/native/perf-procedure.md`; `docs/native/capture-player.md`; `docs/native/golden.md` (G3-perf); `.github/workflows/native.yml` (the Windows and macOS jobs of N1-22).
- **Files (allowlist):** `docs/native/perf-procedure.md` (a capture-player section), `tools/native/perf_report.py` (the capture player's table), `tests/test_native_perf_report.py`, `.github/workflows/native.yml` (upload the capture player with the Windows and macOS artifacts).
- **Spec:** the procedure: the capture player on G3-perf's capture (30 frames at the monitor's 21:9 aspect, N0-10) at `--scale 6` (the widened top screen at 6x is about 3440×1440), with `--hd` and a warm cache, replayed ten times (300 frames), three runs; the same on the M2 at `--scale 3`. The report gives p50, p95 and p99 of the translation time (CPU) and the GPU time.
- **Done when:**
  - `python3 -m pytest tests/test_native_perf_report.py` passes; *(CI)* the artifacts carry the capture player;
  - the numbers are in `docs/research/perf-native-n2.md` (the orchestrator writes it) with the verdict against the budget.
- **Traps:** -
- **Out of scope:** optimising.
