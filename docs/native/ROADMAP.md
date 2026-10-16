# Native port roadmap (no Azahar)

**Status:** design agreed with the maintainer on 2026-10-15; work happens on branch `native`, starting after the v0.1.x launch week. This file is the canonical plan for the branch. The tasks themselves are the work packages in [`wp/`](wp/), tracked in [`STATUS.md`](STATUS.md); decisions are logged in [`DECISIONS.md`](DECISIONS.md); how agents do the work is [`AGENTS.md`](AGENTS.md).

**Decided so far** ([`DECISIONS.md`](DECISIONS.md)):
- GPL-3.0-or-later, as the rest of the repository.
- The runtime presents itself as an Old 3DS, the mode the HD texture pack is made for.
- HD textures are on by default. They are downloaded at setup from the pack author's own links and never shipped in our packages.
- **v1.0 target:** 120 fps stable on the reference PC, with the HD pack on. 180 and 240 fps come after 1.0.
  - The reference PC is the maintainer's: Ryzen 5 5600X + Radeon RX 6700 XT (12 GB).
  - It drives a 1440p ultrawide at 240 Hz, most likely 3440×1440 (to confirm).
- All native-port work lives on branch `native`.
- Sessions run on Sonnet 5.5 in ultracode, with many subagents at once; Opus is kept for planning at phase boundaries.

## 1. What "100 % native" means

| Level | What it is | What is left of the 3DS | When |
|---|---|---|---|
| **N-1 · No emulator** | No Azahar module linked and none of its architecture: not its kernel, its memory system, its clock or its patch. The game's code (recompiled, with hooks and decompiled functions) runs on a runtime of our own. Individual pieces of Azahar's code may be copied, with a compatible licence and a provenance note ([D6](DECISIONS.md)) | Only the interfaces: the game still talks to a 3DS (SVCs, services, PICA commands), but native code answers, with no emulated clock | v1.0 (N5) |
| **N-2 · Native inside** | Replacements higher up, where the 3DS interface gets in the way: engine-level rendering (native-resolution UI, shadows, interpolation), SDK-level audio and files, one screen | Only the game's data formats | N6 onwards, piece by piece |
| **N-3 · Source code** | The game's functions as readable, verified C/C++; the recompiler covers the rest | Nothing, at 100 % | Track D: long and optional |

N-1 is the level the community already calls "native":
- Zelda64Recomp runs recompiled code on a reimplemented libultra, and RT64 interprets display lists.
- Unleashed Recompiled reimplements the Xbox 360 kernel and D3D API.
- ReXGlue keeps Xenia's command processor inside its runtime.

**Out of scope:** other games, a general 3DS emulator, save states (they don't exist today either) and byte-matching decompilation.

## 2. Starting point

**Already native:**
- the game's code: 40,566 functions, 3.26 M instructions, every jump table resolved ([`../RECOMPILER.md`](../RECOMPILER.md));
- 30 hook points and 3 tunables;
- the `mh4u` app (launcher, menu, touch overlay, multiplayer) and the mod catalog;
- the lobby (`server/lobby`) and the scripts and Docker files for dedicated rooms (the room server itself is still `azahar-room`);
- the map of how the game talks to the OS ([`../research/system-services.md`](../research/system-services.md)): 692 `svc` sites, 350 IPC request functions, 23 services.

**What Azahar provides today, and what replaces it.** The last column is the size of Azahar's equivalent module, in non-blank lines, at the pinned commit `5f3306d`.

| Piece | Today (Azahar) | Native | Azahar |
|---|---|---|---|
| Memory | `PageTable` of 4 KiB pages; pages watched by the GPU cache take the slow path; unmapped reads return 0, unmapped writes are dropped | One flat 4 GiB reservation (`ctx->mem + A`, already used by the tests), with an explicit policy for unmapped memory (principle 2) | `memory.cpp` 1.1 k |
| Time | `Core::Timing`: cycles charged per block (`RC_TICK`), CPU clock 50–400 % | Game clock = host clock, pausable (net sync, pause, replays) | — |
| Threads and kernel | One fiber per guest thread, all on the emulation thread; Azahar answers the SVCs | Fibers with our own scheduler per virtual core ([D1](DECISIONS.md)); about 25 distinct SVCs | kernel 9.9 k |
| Code that wasn't recompiled | Azahar's interpreter runs missed entry points; with a code library that doesn't match the code (an unrebuilt code mod), the whole game runs on Azahar's CPU emulation | A minimal interpreter as a safety net ([D13](DECISIONS.md)); with a mismatched library the game doesn't start and the launcher offers to rebuild | — |
| Services | About 37 HLE services | The 23 the game uses; almost half are stubs | services 55.8 k (fs 3.7 k, apt 5.1 k, nwm 3.2 k, cfg 2.2 k, gsp 1.7 k, ir 1.3 k, hid 1.0 k, dsp 0.6 k) |
| System data | Built-in replacements (bad-word list, shared font, Miis, countries) | Our own replacements for what the game opens (e.g. the name filter `nNgc::ProfanityFilter` reads the bad-word list) | — |
| GPU | Emulated PICA200: rasterizer cache, shader JIT and generators, Vulkan/GL/software renderers; command lists processed synchronously; custom textures loaded as they come | A specialised PICA → Vulkan/Metal translator on its own thread, precompiled pipelines, the HD pack pre-processed | video_core 42.2 k |
| Audio | HLE DSP | Our own HLE DSP → mixer → SDL | audio_core/hle 2.6 k |
| Network | `network`: room client and server (`Network::Room` inside the app, `azahar-room`), and `nwm::UDS` | UDS over IP with a room client and server compatible with Azahar's | network 3.8 k + nwm 3.2 k |
| Files | `file_sys`: NCCH, RomFS, LayeredFS, saves; the setup installs the update with Azahar's AM (`port/app/launcher/checks.cpp`) | Extracted RomFS (`tools/extract_ncch.py` exists), LayeredFS, folders for saves and extdata | file_sys 10.0 k, loader 2.6 k |
| Frontend | The app links `citra_common`, `citra_core`, `input_common`, `network`, `video_core`, `web_service`; the hooks call `SetRefreshRate`, `PicaCore().regs.framebuffer_config` and `CoreTiming` | The app on `IRuntime`; the hooks on a platform interface | — |
| Integration | A 1,387-line patch on Azahar | Nothing | — |

One game doesn't need all of that. The order of magnitude is **40 to 55 thousand lines** of our own:
- kernel ~5 k;
- services 10–15 k;
- GPU 15–20 k;
- audio ~3 k;
- network with the room server ~5 k;
- files and loader ~3 k;
- plus moving the app to SDL3 and a new compositor.

**Kept:**
- `tools/` (recompiler, `build_game.py`, extractors, `mods.py`, `find_services.py`);
- the setup from the player's own dump;
- code mods: `.text` changes still go in at recompile time, and data-only mods are applied by the native loader (Azahar's loader does it today);
- the hooks, moved onto the platform interface;
- the app's UI, moved onto `IRuntime`;
- `server/`, `docs/research/`, the agent workflow and the legal rules.

**Retired at the cutover:** `port/src/arm_recomp.*` (with its interpreter fallback), `port/azahar/`, `port/src/frontend.cpp`. The runner is rewritten with the same command line.

## 3. Target architecture

```
 mh4u.exe ──────────────────────────────────────────────────────────────────────────────
  app (today's port/app, on SDL3): launcher · Esc menu · touch overlay · multiplayer
     │ IRuntime: load/run/pause · input · screens · options · rooms · saves
  ───┼──────────────────────────────────────────────────────────────────────────────────
     ▼ native runtime (native/)
  cpu/      code library (built on the player's PC) in RC_NATIVE mode: flat memory, no
            cycle charging · hooks · decompiled functions · minimal fallback interpreter
  kernel/   fibers per virtual core · event, mutex, semaphore, timer, address arbiter,
            shared memory, sessions · handles · the game's SVCs
  ipc/      SendSyncRequest → service and command (buffer at TLS+0x80, descriptors)
  services/ srv fs apt cfg hid ir gsp dsp nwm::UDS ptm ndm ac
            stubs: am nim act frd cecd boss soc http cam y2r err:f · system data
  gpu/      GX queue → PICA command processor → Vulkan / Metal backend (own thread)
            precompiled pipelines + cache + ubershader · HD pack in BC7, by hash
  audio/    HLE DSP → mix at 32,728 Hz → SDL (paced by the audio device)
  net/      UDS over IP · Azahar-compatible room client and server · net sync · UPnP
  fs/       extracted RomFS + LayeredFS (romfs_ext/) · saves and extdata in folders
  time/     game clock (host, pausable) · synthetic VBlank at the presentation rate
```

**Principles:**
1. **The game keeps talking to a 3DS.** We replace what sits under the SDK's interfaces (SVC, IPC, PICA commands), not the SDK. The surface is already mapped, and none of the game changes. Higher-level replacements (N-2) come later, only where profiling or a feature asks for them.
2. **Flat memory, direct pointers.**
   - The GPU learns what changed from the flushes the SDK already does (`FlushDataCache`, `FlushCacheRegions`), with content hashing as a safety net. The N0 census must confirm that the game never writes GPU-visible data without a flush, and never reads GPU output with the CPU.
   - **Unmapped memory:** Azahar returns 0 on such reads and drops such writes, and the game does read address 0 (in the quest-loading race). Release builds map a zero-filled low region that behaves like Azahar; debug builds use guard pages to find the bug. Apple silicon hosts have 16 KiB pages, not 4 KiB.
3. **The clock is the host's.**
   - `GetSystemTick` comes from the host's monotonic clock, scaled to 268,111,856 Hz.
   - The runtime generates VBlank at the presentation rate; there is no CPU clock setting any more.
   - The clock can be held (net sync, pause). Release builds of the code library check a preemption flag in each block (`RC_NATIVE`); in deterministic mode the clock advances by executed blocks instead, with builds that charge cycles (`RC_NATIVE_DETERMINISTIC`).
4. **The GPU never stops the game.** The renderer runs on its own thread, and P3D/PPF interrupts are signalled in order. A thread that waits for the GPU waits alone; the others keep running.
5. **No visible stutter.**
   - With an asynchronous GPU, a shader stall no longer freezes the whole console's clock (today it does, and a silent player is dropped online after about a second), but it still shows.
   - So pipelines are compiled ahead of time and cached on disk, and an ubershader draws while a new one compiles.
   - HD textures stream in the background: the original texture shows until the HD one arrives.
6. **One writer of guest memory per core.** Host threads (renderer, I/O, audio, network) never write guest memory while it runs. They queue events (interrupts, replies, audio frames), and the core's scheduler applies them at safe points. The only exception is a buffer that a request in progress lent to the service (the target of a file read, like a DMA). This keeps the GSP interrupt queue, which the game pops with LDREX/STREX, safe from the renderer.
7. **Every piece has an oracle** (the rule of [`../AGENT_WORKFLOW.md`](../AGENT_WORKFLOW.md)): nothing merges without an automatic comparison against the current build, with thresholds fixed in advance (section 6).
8. **120 fps stable is a v1.0 requirement, not a later improvement.** Every phase measures its share of the frame budget on the reference PC (section 7).

## 4. Strategy: the current build is the oracle

- **The Azahar build is not thrown away: it becomes the oracle factory.** N0 adds recordings to it (IPC, GPU captures, DSP frames, input and game state) and records golden sessions.
- **The renderer is developed from captures, without the runtime.**
  - A capture (our own `MGPU` format, D23) holds everything that crosses Azahar's GPU interface: the GX commands, the register writes, the memory the GPU can see, what the GPU wrote, and the reference image. The native renderer implements the same interface (D20).
  - A `capture_player` replays them into the new renderer and compares, and also times each frame. So N2 starts alongside N1 instead of after it.
- **The runtime is developed headless.** `mh4u_native_runner` has `mh4u_runner`'s command line, so existing tools work with two adjustments:
  - traces are compared with handles and thread ids normalised by creation order;
  - input scripts are keyed to the game's frame counter (`0x010571d4`) or to scenes, not to emulated milliseconds.
- **The cutover comes with parity.**
  - During the transition there are two executables: the Azahar one with SDL2 and the native one with SDL3, because SDL2 and SDL3 export the same function names and don't coexist well in one process. They share the UI through `IRuntime`, and the launcher picks one ([D12](DECISIONS.md)).
  - The native runtime ships as an experimental option in N3 and becomes the default in N5, when Azahar leaves the packages. The Azahar build stays as a development reference as long as it's useful.

```
N0 ──┬── N1 headless runtime ───────┐
     │                              ├── N2b integration ── N3 alpha ── N4 online ── N5 v1.0 ── N6
     ├── N2a renderer from captures ┘                      (opt-in)                (default)
     └── Track D: decompilation, always in parallel
```

## 5. Phases

Sizes are relative; there are no dates. Each phase starts with a pilot that measures cost, and never sets its own thresholds.

**N0 · Foundations, oracles, baseline.** All inside the current build; it also helps v0.x.
- Recorders in `mh4u_runner` (IPC, GPU, DSP, HID, game state, unmapped accesses) and frame-keyed input.
- Golden sessions G1–G7.
- Censuses: PICA features, flush coverage, threads.
- A performance baseline on the reference PC and the M2.
- The seams: an inventory of Azahar dependencies, `IRuntime`, a platform interface for the hooks, `RC_NATIVE`.
- A cloud environment script and native CI.
- **Exit:** goldens recorded and replayable; censuses and inventory in `docs/research/` and `docs/native/`; baseline numbers; the UI running through `IRuntime` with no visible change. Work packages: [`wp/N0.md`](wp/N0.md).

**N1 · Headless runtime: the game boots without Azahar.**
- Loader and guest memory.
- The code-library host.
- Kernel objects, the scheduler, the game clock, the SVC table.
- IPC and the services in headless form: fs, apt, cfg, hid driven by scripts, gsp with a null GPU, dsp without sound.
- A swkbd that answers by itself.
- The native runner, and CPU-budget measurement.
- **Exit:**
  - G1 and G2 pass the full-game and IPC thresholds headless;
  - in G3 the game thread fits its budget on the reference CPU (otherwise per-core parallelism moves forward to N3);
  - CI proves no Azahar module is linked.

  Work packages: [`wp/N1.md`](wp/N1.md).

**N2 · Native renderer.**
- **N2a (from captures, in parallel with N1):**
  - an RHI spike ([D4](DECISIONS.md)), the GX engine and the PICA command processor;
  - vertex loading and the vertex-shader translator;
  - the fragment pipeline: TEV, lighting with LUTs and the quaternion fix-up, fog and gas, procedural textures, shadows;
  - textures, with Azahar's custom-texture hashes;
  - the pipeline cache with an ubershader;
  - the HD pack pre-processor (BC7) with streaming.
- **N2b:** integration with the runtime and the app's compositor.
- **N2c:** the Mac (MoltenVK first; native Metal when measurements call for it).
- **Exit:**
  - golden captures pass the GPU thresholds, and with the HD pack the picture matches the Azahar build with the pack;
  - G1–G4 pass end to end;
  - translation and GPU fit their budgets at 3440×1440 with the HD pack on the RX 6700 XT.

  Work packages: [`wp/N2a.md`](wp/N2a.md); N2b and N2c in [`wp/LATER.md`](wp/LATER.md).

**N3 · Playable end to end at 120 fps (opt-in alpha).**
- Our DSP, real input with the Circle Pad Pro protocol, the swkbd and error applets.
- Saves in Azahar's layout, plus import/export of real 3DS saves.
- A native setup that extracts with `tools/extract_ncch.py` and has the HD pack step preselected.
- The A/B audit 60 vs 120 fps.
- All 30 hooks on the platform interface.
- The native executable shipped next to the Azahar one.

**N4 · Native multiplayer (beta).**
- Our own `nwm::UDS`.
- Azahar's room protocol: client, and a server of our own (until then `azahar-room` stays as a separate binary).
- Net sync on the game clock, and UPnP.
- **Exit:** four mixed players (native at 120, the Azahar build at 60, official Azahar at 30, another native) go hall → quest → hunt → back, and survive freeze tests.

**N5 · Parity and cutover: v1.0 native at 120 fps.**
- The parity checklist and the v1.0 targets of section 7.
- Azahar and `azahar-room` leave the packages; the CPU-clock and `new_3ds` settings go away.
- Two weeks of community testing without blocking regressions.

**N6 · Beyond 120, native inside (after 1.0).**
- 180 and 240 fps: an audit of frame-counted logic, fixed with hooks, and optional visual interpolation.
- Per-core host threads with LDREX/STREX as host CAS and barriers as fences.
- PGO where setup time allows.
- Engine-level graphics, and one screen.

**N7 · Optional.** The original Online mode through a NEX server (Pretendo's MH4U server is open source, AGPL, in beta), StreetPass through the lobby, other regions.

**Track D · Decompilation (N-3).**
- In this order: hook targets, the SDK boundaries, MT core, what mods need, the rest.
- One source builds for `difftest` (ARM) and for the host, using `gptr<T>` 32-bit guest pointers.
- Link-time override of `f_XXXXXXXX`.
- Oracles: `difftest` plus an A/B in the game.

## 6. Oracles and thresholds

| Subsystem | Oracle | Source of truth | Public CI? |
|---|---|---|---|
| Kernel/SVC | `port/tests/svc_test.c` and new tests; per-thread SVC traces, normalised | Azahar | Yes (our own programs) |
| Services | Replay of the golden sessions' IPC | Azahar build | No (derived from the game) |
| GPU | Capture replay → image diff and frame time | Azahar's software renderer at 1x; with the HD pack, its Vulkan renderer with the pack at the same scale; for what Azahar doesn't draw (gas), a homebrew capture player on a real 3DS | Partly: our own PICA test programs with reference images |
| Audio | DSP frames → PCM diff | Azahar HLE; LLE (teakra) | No |
| Input | Frame-keyed scripts → state checkpoints | Azahar build | No |
| Network | Mixed sessions native/Azahar | The game itself | No |
| Whole game | Golden sessions: scenes, state hashes, screenshots | Azahar build | No |
| High frame rate | The same sessions at 60 and 120 fps (180 and 240 in N6) → state checkpoints | The same runtime at 60 fps | No |
| Decompilation | `difftest` + A/B in the game | The original binary | No |
| Performance | Reference scenes, p50/p99 frame time | N0 baseline and section 7 | No |

**Initial thresholds.** They are fixed in N0, before any pilot. If a piece can't meet one, the change is argued in writing in a decision record; it is never lowered silently.
- **GPU:** per frame, PSNR ≥ 40 dB against the reference and no 16×16 block with a mean error above 8/255 (so a missing effect can't hide in the average). What Azahar doesn't draw (gas) is compared against real-3DS captures.
- **Audio:** decoding and interpolation bit-identical to Azahar's HLE; the final mix at SNR ≥ 60 dB.
- **Whole game:** the same sequence of scenes as the golden session, each within ±2 frames, with the observable values (`config/native/observable.txt`) equal at each checkpoint. In deterministic mode, two runs also give identical state hashes at every checkpoint.
- **IPC:** each thread's sequence equals the golden one after normalising handles and thread ids, except time-dependent commands.
- **High frame rate:** the same state checkpoints as at 60 fps; positions and times within the tolerance the audit sets, never a different scene.

**Determinism.** Exact comparisons need a deterministic mode:
- the scheduler checks for preemption every N executed blocks;
- I/O completes at fixed virtual times;
- virtual clock (the build that charges cycles).

Real-time mode is the normal one. To flush out races like the 120-fps quest-loading one, the scheduler can also shuffle its check points with a seed.

## 7. Performance targets

**Reference PC (Ryzen 5 5600X + RX 6700 XT) and monitor (1440p ultrawide, 240 Hz), HD pack on.**
- A 1440p ultrawide is almost always 3440×1440 (21:9); a 32:9 one would be 5120×1440. The port already draws true 21:9 with the HUD at the edges, and 3440×1440 is 6x the widened top screen.
- At 240 Hz, 120 fps divides evenly: every frame lasts two refreshes, so pacing stays even without FreeSync.

**Frame budget at 120 fps** (8.3 ms; a proposal the N0 baseline confirms or corrects):

| Part | Budget | Runs |
|---|---|---|
| Game thread: logic and building command lists | ≤ 4 ms | Virtual core 0 |
| PICA → Vulkan translation | ≤ 3 ms | Its own thread, in parallel |
| GPU at 3440×1440 with the HD pack | ≤ 6 ms | In parallel, with the engine's existing one-frame lag |
| Margin for waits and presentation | the rest | — |

**v1.0 (N5):**

| KPI | Target |
|---|---|
| Busy hunt, 4 players, 3440×1440, HD pack | 120 fps, p99 ≤ 8.3 ms |
| Stutters over 50 ms with a warm cache, HD textures streaming included | 0 |
| VRAM with the HD pack | under the 6700 XT's 12 GB, with margin |
| M2 at 3x | steady 60 fps |
| Quest load | no slower than the Azahar build |
| Windows package | no Qt, ICU or FFmpeg |

**N6:**

| KPI | Target |
|---|---|
| Busy hunt, 4 players, 3440×1440, HD pack | 180 and 240 fps: native where the logic holds up, visual interpolation where it doesn't |
| Steam Deck at 2x | steady 60 fps |

## 8. Risks

| Risk | Mitigation |
|---|---|
| A long tail of GPU accuracy details (gas, shadows, procedural textures, LUT precision) | Per-frame captures against the software renderer, the real 3DS as final reference, the census to bound the scope, the Azahar build as the product until parity |
| The performance hypothesis is wrong (the cost is in the game, not in the emulation) | The N0 baseline measures it before N1, and N1 measures the game thread headless; if it doesn't fit, per-core parallelism moves forward to N3 |
| Races that native speed exposes (the 120-fps quest-loading race is a sample) | Deterministic mode, a seeded shuffling scheduler, an I/O-latency option, hooks like the `startQuest` one |
| Data races between host threads and the game | Principle 6 |
| Unmapped reads that pass silently today | The zero-filled low region in release builds, and the N0 count |
| Scheduling semantics (priorities, cores) | The thread census, strict priority per core as on the 3DS, conformance tests |
| Visible stutter | Precompiled pipelines with an ubershader, HD textures in the background; net sync holds the clock so players aren't dropped |
| Long stretches with nothing visible | N0 improves v0.x, the `capture_player` shows the renderer early, N3 ships the runtime as an option |
| Hidden Azahar dependencies in the app, launcher, hooks or servers | The N0 inventory and N1's CI rule |
| Community content (HD pack, mods) | Texture hashes identical to Azahar's and LayeredFS with sub-folders, with compatibility tests in N2 and N5 |
| The HD pack author's Dropbox link | Dropbox pauses all of an account's shared links for 24 hours after its daily limit (20 GB on the free plan, 1 TB on paid ones; the pack is 12 GB). Work with the author on mirrors under their control; the catalog takes several sources with one checksum; if all fail, the game starts without the pack and fetches it later |
| VRAM with the HD pack | BC7 with mipmaps (a quarter of RGBA8), background streaming, a VRAM budget that evicts what isn't used |
| Mac performance | MoltenVK first; native Metal through the RHI when measurements call for it |
| How a "native port" is perceived legally | The usual rules: no game data, built from the player's own dump, no monetisation. The executable contains no game code, the HD pack comes from its author, and everything derived from the game (pipeline lists and caches, the pre-processed pack, PGO profiles, captures, golden sessions) is made on each player's PC and never enters the repository or the packages |
| Lots of AI-written code to maintain | Oracles everywhere, reviewers who didn't write the code, decision records, one document per subsystem |
| Copied Azahar pieces going stale | Copied with their commit and a provenance note, listed in `native/THIRD_PARTY.md` |

## References

- **Our research:** `docs/research/system-services.md`, `metal.md`, `widescreen.md`, `second-screen.md`, `screen-filters.md`, `mt-classes.md`; `docs/RECOMPILER.md`, `docs/PORT_ARCHITECTURE.md`, `docs/AGENT_WORKFLOW.md`, `docs/MODS.md`, `tools/mods_catalog.json`.
- **N64Recomp + N64ModernRuntime + RT64:** https://github.com/N64Recomp/N64ModernRuntime · https://github.com/rt64/rt64
- **Unleashed Recompiled** (kernel and D3D reimplemented on plume): https://github.com/hedge-dev/UnleashedRecomp · plume: https://github.com/renderbag/plume
- **ReXGlue** (recompilation + Xenia's command processor): https://github.com/rexglue/rexglue-sdk
- **TriAevum** (native OoT3D, alpha; its own runtime, Azahar's shader decompiler and audio, libultraship, NRI/Vulkan): https://github.com/coccofresco/TriAevum
- **Zakuro + 3dsrecomp** (MIT; their list says MH4U is "Playable" on recompiled code on their HLE runtime, which keeps a fallback interpreter): https://github.com/fearkov/zakuro · https://github.com/fearkov/3dsrecomp
- **Dusklight** (Twilight Princess: decomp + aurora on SDL3): https://github.com/TwilitRealm/dusklight
- **SDL3 GPU** (3.4.18): https://github.com/libsdl-org/SDL/blob/release-3.4.18/include/SDL3/SDL_gpu.h
- **Fragment shader interlock on AMD:** RADV since Mesa 23.2 (https://www.phoronix.com/news/RADV-Adds-Frag-Shader-Interlock), AMDVLK since 2024.Q4.1 (https://www.phoronix.com/news/AMDVLK-2024.Q4.1-Released)
- **Dropbox shared-link limits:** https://help.dropbox.com/files-folders/share/banned-links
- **Azahar, licence and AI policy:** https://github.com/azahar-emu/azahar · https://github.com/azahar-emu/azahar/blob/master/AI-POLICY.md
- **Pretendo's MH4U NEX server:** https://github.com/PretendoNetwork/monster-hunter-4-ultimate
- **3dbrew** (SVCs, services, PICA200): https://www.3dbrew.org
