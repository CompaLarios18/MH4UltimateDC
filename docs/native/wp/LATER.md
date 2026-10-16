# Later phases (outlines)

N2b to N7 and Track D, outlined. The planner (`native-planner`) details each phase into a `wp/<phase>.md` in the spec format of [`../AGENTS.md`](../AGENTS.md#4-work-packages) when the phase before it nears its exit, using what that phase learned (STATUS, research notes, measurements). The IDs below are provisional; the planner may split, merge or reorder them. The phases themselves are in [`../ROADMAP.md`](../ROADMAP.md#5-phases).

## N2b · Integration: runtime, renderer and app

Depends on N1's exit and N2a. The native executable `mh4u-native`, on SDL3 (D8, D12), runs the game with the native renderer and the app's UI.

| ID | Work package | Notes |
|---|---|---|
| N2b-01 | The native executable on SDL3 | Window, Vulkan surface, event loop; the app's ImGui UI with ImGui's SDL3 and Vulkan backends |
| N2b-02 | `IRuntime` for the native runtime | Lifecycle, input, options, perf stats, paths: the methods N0-20 and N0-24 defined |
| N2b-03 | The renderer in the runtime | The null GPU swapped for the renderer; VBlank drives presentation |
| N2b-04 | Compositor | The app's layouts (widened top screen, bottom screen as overlay or side by side, the touch overlay) scaled to the window; pacing at 120 fps on a 240 Hz display (two refreshes per frame) |
| N2b-05 | The hooks' visual features | Widescreen, HUD at the edges, the screen filters, the touch overlay, on the native renderer |
| N2b-06 | End to end with images | The native runner takes screenshots; G1–G4 against the goldens, images included |
| N2b-07 | Frame pacing measured | On the reference PC: G3 at 120 fps, p99 frame time, stutters over 50 ms |

**Exit:** G1–G4 pass end to end; 120 fps in G3 on the reference PC within the budget of [`../ROADMAP.md`](../ROADMAP.md) §7.

## Renderer oracles still to build

Two references N2a can't produce from Azahar's software renderer. The planner places them where they're needed (gas before N3's alpha; the HD references before N5's parity check).

| ID | Work package | Notes |
|---|---|---|
| N2o-01 | Real-3DS capture player | A homebrew (`3ds`) that replays an `MGPU` capture on the console and dumps the framebuffers, for gas and for anything else Azahar draws wrong |
| N2o-02 | HD-pack reference images | The Azahar build with the pack, on the maintainer's PC (`pc`): screenshots at the frames of the captures, as the reference N2-18 compares with by hand today |

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
| N3-03 | Real input | SDL3 controllers, keyboard and mouse with the app's mapping; touch with the mouse; the Circle Pad Pro protocol on `ir:USER` for the right stick and ZL/ZR (D10) |
| N3-04 | Software keyboard and error applets with UI | The app's keyboard; error messages |
| N3-05 | Saves | Azahar's layout (from N1-13); import and export of real 3DS saves |
| N3-06 | Native setup | `prepare_game.py` from the launcher, the code library built with `--native`, the HD pack step preselected (D14: download from the author's links, `mh4n_hdpack`) |
| N3-07 | LayeredFS and mods | `romfs_ext/`, the mod manager's load order, data mods on the native loader |
| N3-08 | 60 vs 120 fps audit | The same sessions at both rates: the same state checkpoints |
| N3-09 | All 30 hooks verified | Each hook point exercised on the native runtime |
| N3-10 | Packages | The native executable next to the Azahar one in the Windows, Linux and macOS packages; the launcher offers it as experimental |
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
