# Later phases (outlines)

N2c to N7, Track D and the renderer oracles N2o, outlined (N2b is detailed in [`N2b.md`](N2b.md); a pointer is kept below). The planner (`native-planner`) details each phase into a `wp/<phase>.md` in the spec format of [`../AGENTS.md`](../AGENTS.md#4-work-packages) when the phase before it nears its exit, using what that phase learned (STATUS, research notes, measurements). The IDs below are provisional; the planner may split, merge or reorder them. The phases themselves are in [`../ROADMAP.md`](../ROADMAP.md#5-phases).

## N2b · Integration: runtime, renderer and app

Detailed in [`N2b.md`](N2b.md) (24 work packages, N2b-01 to N2b-24; its *Order* section maps the provisional IDs that were here).

**Exit:** G1–G4 pass end to end; 120 fps in G3 on the reference PC within the budget of [`../ROADMAP.md`](../ROADMAP.md) §7.

## Renderer oracles still to build

Two references N2a can't produce from Azahar's software renderer. Neither is needed for N2b's exit (N2b's image gate leaves gas frames out and compares the HD pack by hand, N2-18), so both stay here and are detailed with the phase that needs them.

| ID | Work package | Notes |
|---|---|---|
| N2o-01 | Real-3DS capture player | A homebrew (`3ds`) that replays an `MGPU` capture on the console and dumps the framebuffers, for gas and for anything else Azahar draws wrong. **Needed before N3's alpha** (N3's exit has G4 end to end, gas frames included): the planner details it at the N3 boundary, or earlier if the orchestrator asks, since it depends only on N2-20 and N0-10's G4 capture. See the notes below |
| N2o-02 | HD-pack reference images | The Azahar build with the pack, on the maintainer's PC (`pc`): screenshots at the frames of the captures, as the reference N2-18 compares with by hand today. **Needed before N5-01** (the parity checklist): detailed at the N5 boundary. The reference runner in CI is built without Vulkan (`docs/native/CI.md`), so the references come from the Windows package's Vulkan renderer with custom textures, at the scale of the native run |

**What the planner found about N2o-01 (2026-10-21), for whoever details it:**
- **Three packages, not one.** (a) A PC tool that cuts a capture down to what one frame needs and relocates it: G1's 3-frame capture is 144 MB (N0-05) because it holds memory images, more than an Old 3DS application can hold, and its command lists, registers and GX commands name physical addresses the homebrew cannot have. The PICA command processor of N2-04 knows which register fields are addresses; the relocated result is itself an `MGPU` file, so its oracle runs in the cloud: the capture player with `--gpu native` gives the same images for the relocated capture as for the original, bit for bit. (b) The homebrew: libctru's `GX_ProcessCommandList`, `GX_MemoryFill`, `GX_DisplayTransfer`, `GX_TextureCopy` and `GX_RequestDma` are enough (no citro3d), the package loaded from the SD card into the linear heap and VRAM; checked first on the reference runner with a synthetic package, then on the console. (c) The gas check: the native renderer's gas frames against the console's dumps, with ROADMAP §6's thresholds (`3ds`, the maintainer).
- **Toolchain.** From this cloud session `apt.devkitpro.org` and `pkg.devkitpro.org` answered 403 and Docker Hub answered, so a cloud worker cannot be assumed to install devkitARM; the homebrew builds in GitHub Actions in the `devkitpro/devkitarm` container image (a workflow of its own), and a cloud worker writes it against that job.
- **Not verified:** how much memory a homebrew gets on the maintainer's console and loader (it bounds the package's size).

## N2c · The Mac

| ID | Work package | Notes |
|---|---|---|
| N2c-01 | MoltenVK on the M2 | Builds, passes the captures, runs end to end |
| N2c-02 | Mac performance | M2 at 3x, steady 60 fps |
| N2c-03 | Native Metal backend (if needed) | Only if N2c-02 misses its target, through the RHI (D4), under the rule of `docs/research/metal.md` |

## N3 · Playable end to end at 120 fps (opt-in alpha)

| ID | Work package | Notes |
|---|---|---|
| N3-01 | The full HLE DSP | Decoders, interpolation, mixing and effects replace N1-18's model behind the same service. Oracle: MDSP captures (bit-identical decoding, mix at SNR ≥ 60 dB) |
| N3-02 | Audio output | SDL3 audio, resampling from 32,728 Hz to the device's rate, latency |
| N3-03 | Real input | SDL3 controllers, keyboard and mouse with the app's mapping; touch with the mouse; the Circle Pad Pro protocol on `ir:USER` for the right stick and ZL/ZR (D10). Builds on N2b-08's live input source and N2b-09's `IRuntime` input methods, which in N2b map only the keyboard's keys and the mouse's touches |
| N3-04 | Software keyboard and error applets with UI | The app's keyboard (`IRuntime::SetSoftwareKeyboard` and `FinishKeyboard` in `mh4u-native`, stubs in N2b-09); error messages |
| N3-05 | Saves | Azahar's layout (from N1-13); import and export of real 3DS saves |
| N3-06 | Native setup | `prepare_game.py` from the launcher, the code library built with `--native`, the HD pack step preselected (D14: download from the author's links, `mh4n_hdpack`) |
| N3-07 | LayeredFS and mods | `romfs_ext/`, the mod manager's load order, data mods on the native loader |
| N3-08 | 60 vs 120 fps audit | The same sessions at both rates: the same state checkpoints |
| N3-09 | All 30 hooks verified | Each hook point exercised on the native runtime |
| N3-10 | Packages | The native executable next to the Azahar one in the Windows, Linux and macOS packages; the launcher offers it as experimental and starts `mh4u-native` with the arguments of `docs/native/app.md` (D12); the separate-windows layout, which N2b leaves out (D62) |
| N3-11 | Reports from testers | Logs and crash reports a tester can send |

**Exit:** G1–G4, G6 and G7 pass end to end natively; alpha builds in the packages; testers' reports triaged.

## N4 · Native multiplayer (beta)

| ID | Work package | Notes |
|---|---|---|
| N4-01 | Room client | Azahar's room protocol (ENet, `network_version` 4): join, host, members, chat (D7) |
| N4-02 | `nwm::UDS` over rooms | Beacons, connections, data frames on channel 243, node ids, the EAPoL-Logoff fix of the port's patch |
| N4-03 | Net sync on the game clock | The design of `port/src/netsync.cpp`, with the clock's `Hold` |
| N4-04 | Our own room server | Replaces `azahar-room` in the packages; official Azahar clients still join |
| N4-05 | UPnP | For hosting |
| N4-06 | Mixed sessions | Native at 120, the Azahar build at 60, official Azahar at 30, another native; freeze tests (G5 and four-player variants) |

**Exit:** four mixed players go hall → quest → hunt → back, and survive the freeze tests.

## N5 · Parity and cutover: v1.0 native at 120 fps

| ID | Work package | Notes |
|---|---|---|
| N5-01 | Parity checklist | Every feature of the Azahar build: options, mods, multiplayer, overlay, HD pack, the launcher's flows |
| N5-02 | The v1.0 targets measured | The KPI table of [`../ROADMAP.md`](../ROADMAP.md) §7 |
| N5-03 | Azahar leaves the packages | With `azahar-room`; the CPU-clock and `new_3ds` settings go; the launcher defaults to native |
| N5-04 | Two weeks of community testing | No blocking regressions; a triage routine |
| N5-05 | Retire the Azahar backend | `port/azahar/`, `arm_recomp.*`, `frontend.cpp` go; `mh4u_runner` becomes the native runner; shared files (hooks, fiber, schedule, platform) move to their final place |
| N5-06 | `native` into `main` | The maintainer's decision, after N5-04 |

**Exit:** v1.0 released.

## N6 · Beyond 120, native inside (after 1.0)

| ID | Work package | Notes |
|---|---|---|
| N6-01 | Frame-counted logic at 180 and 240 | The audit of N3-08 at higher rates; hooks for what breaks |
| N6-02 | Visual interpolation | Optional, for what can't run natively at the higher rate |
| N6-03 | One host thread per core | LDREX/STREX as host compare-and-swap, barriers as fences; a change to `recomp.h` and the memory model |
| N6-04 | PGO | Where the setup's time allows |
| N6-05 | Engine-level graphics | Native-resolution UI, shadows, one screen; piece by piece |
| N6-06 | Steam Deck | 2x at a steady 60 fps |

**Exit:** the N6 targets of [`../ROADMAP.md`](../ROADMAP.md) §7.

## N7 · Optional

- The original Online mode through a NEX server (Pretendo's MH4U server is open source, AGPL, in beta). Connecting to a server doesn't mix licences; copying its code would.
- StreetPass through the lobby.
- Other regions (EUR, JPN): their own addresses, code libraries and goldens.

## Track D · Decompilation (N-3)

Always in parallel, with the workflow of [`../../AGENT_WORKFLOW.md`](../../AGENT_WORKFLOW.md).
- In this order: hook targets, the SDK boundaries, MT core, what mods need, the rest.
- One source builds for `difftest` (ARM) and for the host, using `gptr<T>` 32-bit guest pointers.
- Link-time override of `f_XXXXXXXX`.
- Oracles: `difftest`, plus an A/B in the game.
