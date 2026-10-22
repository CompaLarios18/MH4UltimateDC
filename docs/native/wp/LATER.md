# Later phases (outlines)

N2c, N4 to N7, Track D and the renderer oracle N2o-02, outlined (N2b and N3 are detailed in [`N2b.md`](N2b.md) and [`N3.md`](N3.md); pointers are kept below). The planner (`native-planner`) details each phase into a `wp/<phase>.md` in the spec format of [`../AGENTS.md`](../AGENTS.md#4-work-packages) when the phase before it nears its exit, using what that phase learned (STATUS, research notes, measurements). The IDs below are provisional; the planner may split, merge or reorder them. The phases themselves are in [`../ROADMAP.md`](../ROADMAP.md#5-phases).

## N2b · Integration: runtime, renderer and app

Detailed in [`N2b.md`](N2b.md) (27 work packages, N2b-01 to N2b-27; its *Order* section maps the provisional IDs that were here).

**Exit:** G1–G4 pass end to end; 120 fps in G3 on the reference PC within the budget of [`../ROADMAP.md`](../ROADMAP.md) §7.

## Renderer oracles still to build

| ID | Work package | Notes |
|---|---|---|
| N2o-01 | Real-3DS capture player | **Moved to N3** as N3-23 (the capture trimmer, a Python tool whose oracle is the capture player), N3-24 (the homebrew on libctru's GX calls, built in GitHub Actions in the `devkitpro/devkitarm` container and checked on the reference runner) and N3-25 (the gas check on the console, `3ds`), because the alpha waits for it (proposed D74). The N2b planner's notes (three packages; devkitPro's package servers answered 403 from the cloud and Docker Hub answered; G1's 3-frame capture is 144 MB) are in those specs. Still not verified: how much memory a homebrew gets on the maintainer's console (N3-24's homebrew prints it, N3-25 records it) |
| N2o-02 | HD-pack reference images | The Azahar build with the pack, on the maintainer's PC (`pc`): screenshots at the frames of the captures, as the reference N2-18 compares with by hand today. **Needed before N5-01** (the parity checklist): detailed at the N5 boundary (N3 keeps the HD check by hand, as N2b). The reference runner in CI is built without Vulkan (`docs/native/CI.md`), so the references come from the Windows package's Vulkan renderer with custom textures, at the scale of the native run |

## N2c · The Mac

| ID | Work package | Notes |
|---|---|---|
| N2c-01 | MoltenVK on the M2 | Builds, passes the captures, runs end to end; then `mh4u-native` joins the macOS package as N3-18 did for Windows and Linux (proposed D68: the N3 alpha has no macOS build) |
| N2c-02 | Mac performance | M2 at 3x, steady 60 fps |
| N2c-03 | Native Metal backend (if needed) | Only if N2c-02 misses its target, through the RHI (D4), under the rule of `docs/research/metal.md` |

## N3 · Playable end to end at 120 fps (opt-in alpha)

Detailed in [`N3.md`](N3.md) (30 work packages, N3-01 to N3-30; its *Order* section maps the provisional IDs that were here, and lists what was left out of N3).

**Exit:** G1–G4, G6 and G7 pass end to end natively; alpha builds in the packages; testers' reports triaged.

## N4 · Native multiplayer (beta)

| ID | Work package | Notes |
|---|---|---|
| N4-01 | Room client | Azahar's room protocol (ENet, `network_version` 4): join, host, members, chat (D7). In `mh4u-native`: the room methods of `IRuntime` in `NativeRuntime`, `App::Multiplayer` and `Launcher::Lobby` replacing the stubs of N2b-03 (`native/app/shared/native_rooms.cpp`), chat added to `IRuntime` with its UI (D29); the launcher that hosts and joins stays `mh4u`'s until N5-07 (proposed D66) |
| N4-02 | `nwm::UDS` over rooms | Beacons, connections, data frames on channel 243, node ids, the EAPoL-Logoff fix of the port's patch |
| N4-03 | Net sync on the game clock | The design of `port/src/netsync.cpp`, with the clock's `Hold`; it replaces the native platform's local-only net sync and end-skip vote (N1-20) and fills `IRuntime::EndSkipPending` (empty since N2b-25). While the clock is held the DSP makes no frames and the audio stream plays its fade and silence (proposed D63): nothing to add for sound |
| N4-04 | Our own room server | Replaces `azahar-room` in the packages; official Azahar clients still join |
| N4-05 | UPnP | For hosting |
| N4-06 | Mixed sessions | Native at 120, the Azahar build at 60, official Azahar at 30, another native; freeze tests (G5, recorded by N0-11, and four-player variants) |

**Exit:** four mixed players go hall → quest → hunt → back, and survive the freeze tests.

## N5 · Parity and cutover: v1.0 native at 120 fps

| ID | Work package | Notes |
|---|---|---|
| N5-01 | Parity checklist | Every feature of the Azahar build: options, mods, multiplayer, overlay, HD pack, the launcher's flows |
| N5-02 | The v1.0 targets measured | The KPI table of [`../ROADMAP.md`](../ROADMAP.md) §7 |
| N5-03 | Azahar leaves the packages | With `azahar-room`; the CPU-clock and `new_3ds` settings go; the launcher (by then `mh4u-native`'s, N5-07) defaults to native; the setup no longer installs the update through Azahar's AM: `tools/native/setup_native.py` takes the `.cia` (N3-17); the alpha's packaging (N3-18) becomes the release packaging |
| N5-04 | Two weeks of community testing | No blocking regressions; a triage routine |
| N5-05 | Retire the Azahar backend from the product | `mh4u`'s Azahar runtime (`port/app/runtime_azahar/`, the window and renderer files) leaves the app; the shared files (hooks, fiber, schedule, platform) move to their final place. The reference runner (`port/azahar/`, `arm_recomp.*`, `frontend.cpp`, `mh4u_runner`) stays buildable in CI as the oracle factory for as long as it is useful (D11, ROADMAP §4): it is retired only by a decision of its own |
| N5-06 | `native` into `main` | The maintainer's decision, after N5-04 |
| N5-07 | The launcher in `mh4u-native` | Before N5-03: `port/app/launcher/` on SDL3 in `native/app/` (setup, mods, online, settings, the updater), with an HTTP client for the updater, the lobby and the catalog chosen then (D55 keeps httplib and JSON out of the shared sources); until then `mh4u`'s launcher starts `mh4u-native` (proposed D66) |
| N5-08 | The C-stick, ZL and ZR in input scripts | The script grammar (`port/src/schedule.h`) and both runners' input paths, so a session can drive the camera and be recorded by `--record-input` (N2b-08 and N3 leave them out); only if N5-01's checklist needs it |

**Exit:** v1.0 released.

## N6 · Beyond 120, native inside (after 1.0)

| ID | Work package | Notes |
|---|---|---|
| N6-01 | Frame-counted logic at 180 and 240 | The audit of N3-22 (`tools/native/fps_audit.py`, with N3-21's observables) at higher rates; hooks for what breaks |
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
