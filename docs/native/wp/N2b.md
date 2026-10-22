# N2b · Integration: runtime, renderer and app

The native executable `mh4u-native` (D8, D12) runs the game on the native runtime of N1, draws it with the renderer of N2a, shows it in a window of its own through a compositor with the app's screen layouts, and puts the app's ImGui UI over it. The phase ends when **G1–G4 pass end to end, images included, and G3 runs at 120 fps on the reference PC within the budget of [`../ROADMAP.md`](../ROADMAP.md) §7** ([`LATER.md`](LATER.md), ROADMAP §5). The workflow is [`../AGENTS.md`](../AGENTS.md); the rules for code in `native/` are at the top of [`N1.md`](N1.md) and the shared rules (formats, Old 3DS runs, *(CI)* items, needs tags, the two ways of comparing) at the top of [`N0.md`](N0.md). They apply here, with the rules below.

**Where things stand (2026-10-21), and what N2b is written against.** N0's goldens G1, G2 and G6 are recorded (N0-09) and G3, G4, G7, G3-perf are not (N0-10, which needs the save). N1-21 (the first game boot) is being done by the local session; N1-26, N1-25 and N1-23 follow it. Of N2a, the capture player, the GX engine, the PICA command processor, textures and the HD pre-processor are done (N2-02, N2-03, N2-04, N2-14, N2-17); everything else, and D4 (the RHI) and D5 (the fragment pipeline), are open. So the WPs below are written against the contracts the N1 and N2a specs promise, not against code that exists:
- **N2a:** `--gpu native`, the renderer assembled, registered by `native/src/gpu/native/` (N2-08); the GPU wrapper `queue` (N2-22: a renderer thread, interrupts in completion order, at most one frame in flight); the surface cache with `--scale N` and write-back of a surface on demand (N2-15); the pipeline cache in the user folder (N2-16); HD streaming with `--hd ARCHIVE` and a VRAM budget (N2-18); the backend's images, command recording, submission, readback and timestamps (N2-21); glslang vendored by N2-06 (`native/third_party/glslang`); `GpuDebug::TakeFrameCounters` and `TakeFrameGpuTimeNs` (N2-02).
- **N1:** the game boots and passes G1 and G2 headless (N1-21) and G3 (N1-26); the deterministic mode (N1-25); `--perf-log` with the game thread's time per frame (N1-23); `NativePlatform::Find(rt)` with `TopScreenRows`, `TopScreenWidth`, `Overlay`, `LayoutRefreshes` and `RequestedRefreshRate` for the renderer (N1-20, `native/src/platform/platform_native.h`); `Runtime::SetRefreshRate` callable from any thread (D44).
- **The app:** `IRuntime` (`port/app/runtime.h`, N0-20, N0-24), still with the transitional `HostSystem()` and a `Window&` that is Azahar's `EmuWindow`, and the UI (`port/app/ui/`) still written against SDL2 and that window.

When an N2a or N1 WP merges with names other than these, the planner revises the *Spec* sections that use them in one short pass (as N2a does for D4 and D5); the goals, oracles and *Done when* lists stand.

## Rules for this phase

These add to the rules at the top of `N1.md` and `N0.md`.
1. **Two executables, one UI** (D12). The app's shared sources stay in `port/app/` and compile into both `mh4u` (Azahar, SDL2) and `mh4u-native` (SDL3). After N2b-02 they include no Azahar header and no SDL header: the window, its events and the few host services they need come through `port/app/app_window.h` (`App::IWindow`, `App::InputEvent`, `App::Host`), which each executable implements. A WP that changes a file under `port/app/` keeps the Azahar app green (`linux.yml`, `windows.yml` and `macos.yml` dispatched with `ref` set to its branch, *(CI)*) and the maintainer's smoke test of `docs/native/app-smoke.md` (`pc`).
2. **SDL3 only in `native/app/`.** The modules under `native/src/` are linked into every binary (the runner, the tools, every test), so none of them includes SDL or ImGui; they take what they need from the app through plain interfaces (a `VkSurfaceKHR`, an input state, a draw callback). `native/app/` is its own tree (N2b-03): its modules link SDL3, ImGui and the app's shared sources, and only `mh4u-native` and the tests under `native/app/tests/` link them. The executable's own options register in the `runner` scope from `native/app/` modules, so they never show in the runner's `--help`. The app build is the configure line of `N1.md` plus `-DMH4N_APP=ON`, in its own build folder, after `source ~/.mh4n_env &&`:
   ```
   source ~/.mh4n_env && cmake -S native -B build/native-app -G Ninja -DCMAKE_C_COMPILER=clang \
         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON -DMH4N_APP=ON
   cmake --build build/native-app && ctest --test-dir build/native-app --output-on-failure
   ```
3. **Threads** (principles 4 and 6). `mh4u-native` has four host threads of its own: the **main** thread (SDL's events, the UI's frames, every `IRuntime` call), the **game** thread (`Runtime::Boot`: the scheduler and the guest fibers), the **renderer** thread (N2-22) and the **present** thread (N2b-06); plus the renderer's workers (pipeline compiles, HD streaming). Only the game thread touches guest memory and kernel objects; the others hand work over with `Kernel::Post`, `gpu::Host::Post` or `Runtime::SetRefreshRate`. **The game never waits for presentation:** the present thread takes the newest displayed frame from a mailbox and drops older ones. The main thread never blocks on the game thread, except `IRuntime::SetGameOption`'s bounded wait (N2b-09).
4. **Headless first.** Every presentation feature has a form that runs without a display: the compositor draws into an offscreen image, `mh4u-native --headless --present-dump DIR` writes the composed window as PPM, SDL3's `offscreen` video driver makes windows and Vulkan surfaces through `VK_EXT_headless_surface` (verified in SDL 3.4.18's `src/video/offscreen/SDL_offscreenvulkan.c`), and lavapipe runs it in the cloud and in Linux CI. What needs a real display (vsync, the monitor's refresh, the AMD driver, how it looks) is a named maintainer procedure on the reference PC (`pc`).
5. **Thresholds** are never changed here: images, ROADMAP §6 (PSNR ≥ 40 dB and no 16×16 block with a mean error above 8/255, the rule `native/src/gpu_capture/image.h` implements); scene timing across timing models, D50 (4 frames, a 60-frame input margin); the DSP capture, D51; `fs:` requests, D52; time, ROADMAP §7 and D19 (game thread ≤ 4 ms, translation ≤ 3 ms, GPU ≤ 6 ms at 3440×1440 with the HD pack, 120 fps, no stutter over 50 ms). A check that cannot meet one stops the WP with `blocked` (or `needs-decision`, with the table), never with a looser number.
6. **Images across timing models** follow the decision N2b-17 settles (proposed D58): in the `cross` profile the screenshots are gated only at calibrated frames, with the thresholds above; frames in which the native run drew gas are reported and not judged (Azahar draws no gas, N2a's preamble). The `same` profile stays exact.
7. **Game data.** Screenshots, present dumps, GPU logs and present logs of the game are game data: they go to `build/`, never into the repository, a test fixture or a report. Reports quote frame numbers, counts, PSNR values and times only. Fixtures are synthetic (our own 3DS programs, images made by the tests).
8. **Nothing the player sees changes by default without a decision:** the frame rate (the `fps` of `mh4u.ini`), the layouts, the HD pack's default, the window's behaviour and the executable the launcher starts are the maintainer's (see *Decisions*).

## Where the code goes

| Path | What | WP |
|---|---|---|
| `port/app/app_window.h`, `port/app/host_sdl2.cpp` | the window, its events and the host services the shared UI uses; the Azahar app's implementation | N2b-01, N2b-02 |
| `native/app/` | the app tree: `CMakeLists.txt` (discovery), `program/` (`main` of `mh4u-native`), `tests/` | N2b-03 |
| `native/app/runtime/` | `mh4n_app_runtime`: `NativeRuntime`, the `IRuntime` of the native runtime | N2b-09 |
| `native/app/window/`, `native/app/shell/` | the SDL3 window and its events; the start-up, run loop and shutdown of `mh4u-native` | N2b-10 |
| `native/app/ui/` | the shared UI compiled for SDL3, ImGui's SDL3 backend, ImGui drawn on the renderer's backend | N2b-11 |
| `native/src/screen_layout/` | the app's layouts and touch mapping (pure arithmetic) | N2b-04 |
| `native/include/mh4n/present.h`, `native/src/present/` | the presentation contract; the compositor | N2b-05 |
| `native/src/swapchain/` | the swapchain, the offscreen target and the present thread | N2b-06 |
| `native/src/display_mem/` | `--gpu null-display`: the null GPU with screens built from guest memory | N2b-07 |
| `native/src/input_live/` | `--input-source live`: input latched at frame boundaries, `--record-input` | N2b-08 |
| `native/src/gpu/runtime/` | `--gpu renderer`: the renderer as the runtime's GPU, its runner options, the displayed screens | N2b-12, N2b-13 |
| `native/src/shots/` | the native runner's screenshots | N2b-14 |
| `port/tests/gfx_*.c`, `port/tests/gsp.h` | our own GX and PICA test programs | N2b-15, N2b-16 |
| `native/src/pacing/` | the display-locked VBlank | N2b-23 |

Documents: `docs/native/app-smoke.md` (N2b-01), `app.md` (N2b-03), `presentation.md` (N2b-05), `renderer-runtime.md` (N2b-12), `gfx-tests.md` (N2b-15), `e2e.md` (N2b-19), `visual-check.md` (N2b-22).

## Decisions

**Rests on:** D4 (Open: the RHI; N2b uses "the backend" of N2-21, whatever D4 chose), D5, D8 (SDL3 3.4.18), D10 (Old 3DS), D12 (two executables sharing the UI through `IRuntime`), D14 (HD on by default, set up by N3-06), D15 (120 fps at 3440×1440 on a 240 Hz monitor), D19 (the budget split), D20 (`mh4n/gpu.h`), D25 and D41 (interrupts at once with Azahar's delays handed over; DMA as `CopyBlock`, with `FlushRegion` and `InvalidateRegion` around it), D26 (`--io-latency azahar`), D31 and D35 (profiles; the reference backend is `recomp`), D37 (one user folder for both executables), D39 (`ir:USER` connects nothing until N3), D43, D44 (refresh-rate calls; always `--user`), D45 (UTF-8 paths), D50–D52.

**Proposed by this plan** (the planner's report has the options; the orchestrator adds the rows; a WP whose *Done when* ends in one stays `review` with `waiting for D<n>` until it is `Decided`). The numbers are the next free ones at the time of writing:
- **D55** (Recommended) the app tree `native/app/`, outside `mh4n::all`; the shared app sources free of SDL and Azahar types (rules 1 and 2).
- **D56** (Recommended) the threads of rule 3, and the present thread's newest-wins mailbox.
- **D57** (the maintainer's, settled by N2b-23) frame pacing: the VBlank locked to the display's refresh, or the host clock alone.
- **D58** (settled by the orchestrator at the maintainer's request, as D50, from N2b-17's measurements) images across timing models: gated at calibrated frames; amends D31.
- **D59** (Recommended; latency is the maintainer's to weigh) live input latched at game-frame boundaries, recordable as a frame-keyed script.
- **D60** (the maintainer's) the window: borderless desktop full screen, keys released on focus loss, the game running on while minimised (as `mh4u` does today); no pause in the background.
- **D61** (Recommended; N3-06 confirms) where `mh4u-native` finds its files: the user folder as Azahar resolves it, the game folder `<user>/native/game/`, the code library `<user>/recomp/<program id>.native.<dll|so|dylib>`, the HD archive `<user>/load/textures/<program id>.mh4hd`.
- **D62** (the maintainer's) the layouts `mh4u-native` offers in N2b (overlay, stacked, single, side; separate windows later), its window title, and that it runs without the launcher until N3-10.

## Oracles

| WP | Oracle |
|---|---|
| N2b-01, N2b-02 | the Azahar app's CI builds on three systems; a compile test of the shared sources without SDL or Azahar headers; the maintainer's smoke test |
| N2b-03 | ctest and CI of the app tree on three systems; `check_native_deps.py`; the layout test (app modules stay out of the runner) |
| N2b-04 | layouts and touch points against values worked out from Azahar's `framebuffer_layout.cpp`, the port's patch and `emu_window.cpp` |
| N2b-05, N2b-06 | composed images read back on lavapipe against pixel positions from N2b-04; mailbox and resize behaviour under a fake clock |
| N2b-07, N2b-14 | `port/tests/io_test.c`'s known picture, as the reference runner's own test checks it (`tests/test_port.py`) |
| N2b-08 | a recorded live session replayed as a script gives the same input samples |
| N2b-09 to N2b-11 | `IRuntime` driven by a fake window; `mh4u-native --headless` on `io_test` (input, touch, layout, menu) |
| N2b-12, N2b-13 | the conformance programs with `--gpu renderer`, synthetic GPU sequences on lavapipe; on the game, the golden checks of N1-21 and the displayed screens against guest memory (`--display-check`) |
| N2b-15, N2b-16 | our GX and PICA programs: exact expected pixels, and the reference runner's screenshots within ROADMAP §6 |
| N2b-17 | jit against recomp screenshots (the calibration); synthetic PPMs for the tool |
| N2b-18 to N2b-21 | `golden.py check` in the `cross` profile, images included (G1–G4, G3-perf) |
| N2b-22 | the compositor and touch tests with the hooks' layout values; the maintainer's visual check against the Azahar build |
| N2b-23, N2b-24 | the pacer's arithmetic on synthetic present timestamps; the maintainer's 120-fps procedure on the reference PC |

## Order

- **Critical path:** N2a's renderer (N2-21 → … → N2-20 → N2-15 → N2-16, N2-18) → **N2b-12 → N2b-13 → N2b-19 → N2b-20 → N2b-21 → N2b-24**; and N1-21 → N1-26 → N1-25 → **N2b-18** → N2b-12 (N2b-12 exposes a header of `gpu_null`, which N2b-18 may fix). N2-19 and N1-23 (the budgets of N2a and N1) also feed N2b-24.
- **Shared files:** `wp_status.py overlaps --all` reports one pair from this file: N2b-17 and N0-11 both change `tools/native/golden.py`, `tests/test_native_golden.py` and `docs/native/golden.md`. N2b-17 does not wait for G5 (two players, N4's oracle), so the orchestrator runs the two one after the other, whichever is ready first.
- **Can start now** (their dependencies are done): N2b-01, N2b-03, N2b-04, N2b-08, N2b-15. Then N2b-02 and N2b-16; N2b-05 once N2-21 and N2-06 are merged; N2b-17 once N1-21, N0-10, N0-30 and N0-32 are.
- **In parallel with the renderer's long tail:** the whole app side (N2b-01 to N2b-11) finishes on `--gpu null-display` and our own programs, long before N2b-12 can start; N2b-22 and N2b-23 then need the renderer only for the maintainer's checks.
- **Build-heavy** (one at a time, AGENTS §10): every WP that builds `native/` (all but N2b-01, N2b-02, N2b-15, N2b-16, N2b-17, N2b-24); N2b-01 and N2b-02 build the Azahar app, which is heavier still (tens of minutes; run in the background).
- **LATER's provisional IDs:** N2b-01 (the executable) became N2b-03 and N2b-10; N2b-02 (`IRuntime`) N2b-09 (with N2b-01, N2b-02 and N2b-08 around it); N2b-03 (the renderer in the runtime) N2b-12 and N2b-13; N2b-04 (compositor) N2b-04 to N2b-06; N2b-05 (the hooks' visual features) N2b-13 (the widened top screen) and N2b-22; N2b-06 (end to end with images) N2b-14 to N2b-21; N2b-07 (frame pacing) N2b-23 and N2b-24.

---

### N2b-01 · The app's UI without Azahar's window and system
- **Size / agent:** M · native-worker-high (builds the Azahar app)
- **Depends on:** N0-24 · **Needs:** ci, pc
- **Goal:** the last Azahar types leave the app's shared sources: the UI talks to an `App::IWindow` instead of Azahar's `EmuWindow`, `IRuntime` loses `HostSystem()`, and the UI's two remaining uses of `port/src` code that sits on Azahar (net sync, the options file) go through `IRuntime`. The Azahar app behaves exactly as before.
- **Read first:** `port/app/runtime.h` and the *As built* notes of N0-20 and N0-24 in [`N0.md`](N0.md); `port/app/window.h`, `window.cpp`; `port/app/ui/ui.cpp`, `ui.h`, `layer.h`, `menu.cpp`, `hint.cpp`; `port/app/launcher/screen_settings.cpp`; `port/app/main.cpp`; `port/src/frontend.h` (`OptionsPath`, `OptionsTemplate`) and `port/src/netsync.h` (`EndSkipPending`); `docs/native/azahar-deps.md`.
- **Files (allowlist):** `port/app/app_window.h` (new), `port/app/runtime.h`, `port/app/runtime_azahar/**`, `port/app/window.h`, `port/app/{window.cpp,window_gl.cpp,window_vk.cpp,window_sw.cpp,main.cpp,multiplayer.cpp,multiplayer.h,CMakeLists.txt}`, `port/app/ui/{ui.cpp,ui.h,layer.h,menu.cpp,hint.cpp}`, `port/app/launcher/screen_settings.cpp`, `port/app/util/tests/**`, `port/src/options_template.h` (new), `port/src/options_template.cpp` (new), `port/src/frontend.cpp`, `port/CMakeLists.txt`, `docs/native/azahar-deps.md`, `docs/native/app-smoke.md` (new).
- **Spec:**
  - **`App::IWindow`** (`port/app/app_window.h`), what the UI uses of a window today, and no more: `IsOpen`, `RequestClose`, `SetFullscreen(bool)`, `IsFullscreen`, `RequestInputReload`, `KeyboardMouseActive`, `ReleaseTouch` (the UI's `TouchReleased` call), `SizeInPoints` and `SizeInPixels` (each `{int w, h}`), and `SDL_Window* SdlWindow()` (an opaque pointer here; each executable's own SDL defines it). `App::Window` (Azahar's) implements it; `Ui::Layer`, `Ui::Start` and `menu.cpp` take `IWindow&` and no longer include `window.h`.
  - **`IRuntime`:** `HostSystem()` goes; `Load(const LoadRequest&, IWindow& window, IWindow* secondary)` (the Azahar runtime casts to `App::Window&`, which it made); new `std::vector<std::string> EndSkipPending()` (the Azahar runtime forwards to `Port::NetSync::EndSkipPending`, `hint.cpp` calls it). `Ui::Start` and `Multiplayer::Init` lose their unused `Core::System&`. `main.cpp` keeps the concrete `AzaharRuntime` and gets the system the windows need from it (a method of `AzaharRuntime`, not of `IRuntime`).
  - **The options file:** `Port::OptionsTemplate()`'s text moves to `port/src/options_template.{h,cpp}` (`Port::OptionsTemplateText()`, standard library only), which `frontend.cpp` calls; `screen_settings.cpp` uses `App::Runtime().GameOptionsPath()` instead of `Port::OptionsPath()` and no longer includes `frontend.h`.
  - The fake runtime of the app's tests (`port/app/util/tests/fake_runtime.h`) follows the interface.
  - **`docs/native/app-smoke.md`:** the maintainer's smoke test of the Azahar app, as exact steps: start `mh4u` from the Windows package of the branch (`windows.yml` dispatched on it), launcher → play → Esc menu → change an option in each tab → the frame-rate counter (F3), full screen (F11) → host a room and join it from a second instance → name a character with the keyboard → quit; what to look at in `mh4u_log.txt`.
- **Done when:**
  - `grep -lE '#include "(window\.h|frontend\.h|netsync\.h|core/|video_core/|common/)' port/app/ui/*.cpp port/app/ui/*.h port/app/launcher/*.cpp | grep -v 'ui/render_'` prints nothing (the `render_*.cpp` files are the Azahar app's own renderers and keep their includes), and `grep -n HostSystem port/app/runtime.h` prints nothing;
  - `python3 -m pytest tests/test_native_app_util.py` passes (the app's helper tests, with the fake runtime);
  - *(CI)* `linux.yml`, `windows.yml` and `macos.yml` dispatched on this WP's branch are green, and so is the runner job of `native.yml` (it compiles `frontend.cpp`);
  - the maintainer's smoke test of `docs/native/app-smoke.md` passes on the reference PC.
- **Traps:** move one concern per commit (N0-20's advice); keep the threading of `ui.h` (frames built on the emulation thread, the draw copy taken by the renderer's thread); `ui.h` still declares the OpenGL and software drawing functions the Azahar windows call: leave them for N2b-02.
- **Out of scope:** SDL in the shared sources (N2b-02); anything native.

### N2b-02 · The app's UI without SDL2
- **Size / agent:** M · native-worker-high (builds the Azahar app)
- **Depends on:** N2b-01 · **Needs:** ci, pc
- **Goal:** the shared app sources compile with no SDL header at all: they receive window and input events as `App::InputEvent`, and the few host services they call (time, clipboard, cursor, key names, message boxes, controllers, the window icon) come through `App::Host`. The Azahar app translates SDL2's events and implements the services; N2b-10 does the same with SDL3.
- **Read first:** `port/app/app_window.h` (N2b-01); `port/app/ui/ui.cpp` (`Layer::OnEvent`, the ImGui SDL2 calls), `ui/menu.cpp`, `ui/keyboard.cpp`, `ui/key_guides.cpp`, `ui/multiplayer_tab.cpp`; `port/app/input.cpp` and `input.h`; `port/app/icon.cpp`, `dialog.cpp`; `port/app/window.cpp` (`PollEvents`); SDL3's migration guide (https://github.com/libsdl-org/SDL/blob/release-3.4.18/docs/README-migration.md; confirm there and in SDL3's headers that scancode, mouse-button and gamepad-button numbers are the same as SDL2's, and translate any that are not in the window).
- **Files (allowlist):** `port/app/app_window.h`, `port/app/host_sdl2.cpp` (new), `port/app/{input.cpp,input.h,input_sdl2.cpp,icon.cpp,icon.h,dialog.cpp,dialog.h,config.cpp,window.cpp,window.h,window_gl.cpp,window_vk.cpp,window_sw.cpp,main.cpp,CMakeLists.txt}`, `port/app/ui/**`, `port/app/launcher/{launcher.cpp,pick.cpp,screen_mods.cpp,screen_online.cpp,updater.cpp}`, `port/app/util/**`, `tests/test_native_app_portable.py` (new), `docs/native/azahar-deps.md`, `docs/native/app-smoke.md`.
- **Spec:**
  - **`App::InputEvent`** (in `app_window.h`): a plain struct with a `type` (key down, key up, text, mouse motion, mouse button, wheel, pad button, pad axis, pad added, pad removed, focus lost, resized, close requested, quit) and the fields those need, in SDL2's numbering (each executable's window translates into it): `scancode`, `repeat`, `mods` (bits for shift, ctrl, alt, gui), `text` (UTF-8), `x`, `y`, `dx`, `dy` (window points), `button`, `down`, `from_touch` (SDL's touch mouse id), `wheel_x`, `wheel_y`, `pad` (instance id), `axis`, `value`. The few SDL constants the shared code names (Escape, Delete, Return, the Alt modifier, the mouse buttons, the gamepad's Guide, Back and Start) become named constants there, with SDL2's values.
  - **`App::Host`** (free functions declared in `app_window.h`): `Ticks()` (ms), `GetClipboard`/`SetClipboard`, `ShowCursor(bool)`, `MousePosition()`, `ScancodeFromName`/`ScancodeName`, `ShowMessageBox(title, text, error, IWindow*)`, `ListControllers()` (the `ControllerInfo`s `input.cpp` builds today), `SetWindowIcon(IWindow&)`. `port/app/host_sdl2.cpp` implements them for the Azahar app; nothing else under `port/app/` calls SDL.
  - **ImGui's platform backend leaves the UI:** `Layer::OnEvent(const InputEvent&)` decides as today whether the menu takes the event; feeding ImGui (`ImGui_ImplSDL2_ProcessEvent`, `NewFrame`, `InitForOther`) moves to the window, through `IWindow::ImGuiInit()`, `ImGuiNewFrame()` and `ImGuiForwardCurrentEvent()` (the window remembers the SDL event it is translating). `ui.h` keeps only what both executables use; the Azahar-only drawing entry points (`InitOpenGL`, `DrawOpenGL`, `DrawSoftware`, …) move to `ui/render.h`, used by `window_*.cpp` and `ui/render_*.cpp` only.
  - `input.cpp` keeps the key-name table and the profile logic; its controller enumeration moves to `input_sdl2.cpp` behind `Host::ListControllers`. The settings file's comment that points at SDL2's scancode page stays (names are the same).
  - The launcher's files (`launcher/`) are Azahar-app-only in N2b (D62): they may keep SDL; list them as such in `azahar-deps.md`. Only the files `tests/test_native_app_portable.py` lists must be SDL-free.
  - **`tests/test_native_app_portable.py`:** compiles with `clang++ -std=c++20 -fsyntax-only` every shared source (`config.cpp`, `input.cpp`, `kbm.cpp`, `invite.cpp`, `multiplayer.cpp`, `ui/` except `render_*.cpp`, `util/*.cpp`) with include paths for `port/app`, `port/src`, ImGui and `native/third_party/fmt` only (no SDL, no Azahar), and fails listing the files that do not compile or that include `SDL` or an Azahar header; skips without clang.
  - `app-smoke.md` gains the steps that exercise what moved: the menu's keys and mouse, the keyboard's text and clipboard paste, the key guides, a controller's Guide and Back+Start, full screen and the window icon.
- **Done when:**
  - `python3 -m pytest tests/test_native_app_portable.py tests/test_native_app_util.py` passes;
  - `grep -rlE '#include <SDL|SDL_[A-Za-z]+\(' port/app/{config.cpp,input.cpp,kbm.cpp,invite.cpp,multiplayer.cpp} port/app/ui/{ui,menu,keyboard,key_guides,multiplayer_tab,hint,options,fps_counter}.cpp` prints nothing;
  - *(CI)* `linux.yml`, `windows.yml` and `macos.yml` dispatched on this WP's branch are green;
  - the maintainer's smoke test of `docs/native/app-smoke.md` passes on the reference PC.
- **Traps:** SDL2's `event.key.keysym.scancode` is `event.key.scancode` in SDL3, and SDL3's `SDL_GetWindowSize` returns `bool`: the translation lives in the window, so the shared code never sees either. A key event that echoes a controller (`EchoOfController`) is filtered in the window, as now. Room callbacks still arrive on the network thread (N0-24).
- **Out of scope:** SDL3 and the native window (N2b-10); the launcher in `mh4u-native` (N3-10).

### N2b-03 · SDL3 and the app tree in the native build
- **Size / agent:** M · native-worker (build-heavy)
- **Depends on:** N1-22, N1-01 · **Needs:** ci
- **Goal:** `native/app/` builds `mh4u-native` against SDL3 3.4.18 (D8) and ImGui, on Linux, Windows and macOS in CI, without changing what the runner, the tools and the existing tests link; and the guard against Azahar code covers the new tree and the app sources it compiles.
- **Read first:** `native/CMakeLists.txt` and `native/tests/CMakeLists.txt` (module discovery, `mh4n_module`, `mh4n_program`, `mh4n::all`); `tools/native/setup_env.sh` and `docs/native/ENV.md` (SDL3 in the cloud); `.github/workflows/native.yml` and `docs/native/CI.md` (the three native jobs, llvm-mingw, the macOS toolchain notes of N1-22's *As built*); `tools/native/check_native_deps.py`; `port/app/CMakeLists.txt` (how ImGui is built for `mh4u`).
- **Files (allowlist):** `native/CMakeLists.txt`, `native/app/CMakeLists.txt` (new), `native/app/tests/CMakeLists.txt` (new), `native/app/program/**` (new), `port/app/third_party/imgui/backends/imgui_impl_sdl3.cpp` (new), `port/app/third_party/imgui/backends/imgui_impl_sdl3.h` (new), `.github/workflows/native-app.yml` (new), `tools/native/check_native_deps.py`, `tests/test_native_check_native_deps.py`, `tests/test_native_cmake_layout.py`, `docs/native/CI.md`, `docs/native/app.md` (new).
- **Spec:**
  - **`MH4N_APP`** (cache string `AUTO`, `ON`, `OFF`; default `AUTO`): `find_package(SDL3 3.4.18 CONFIG)`; `AUTO` builds the app tree when SDL3 is found and says so in one status line either way; `ON` without SDL3 is a configure error. The existing jobs and builds do not change.
  - **`native/app/CMakeLists.txt`:** every `native/app/<name>/CMakeLists.txt` (at any depth, except `program/` and `tests/`) is an app module made with `mh4n_app_module(SOURCES … [DEPENDS …] [THIRD_PARTY …])`, library `mh4n_app_<path>`, linked whole like `mh4n_module`; `mh4n::app` links every app module and `mh4n::all`, SDL3 and the ImGui core library `mh4n_imgui` (ImGui's core files from `port/app/third_party/imgui`, v1.92.9b, with `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, and the SDL3 platform backend). The program `mh4u-native` is `native/app/program/main.cpp`; each `native/app/tests/<name>_test.cpp` is a doctest binary linked against `mh4n::app`. Nothing under `native/src/` may depend on an app module (configure error).
  - **`mh4u-native`** in this WP is a placeholder `main`: `--help` (the `runner` scope's usage, which includes the app modules' options) and `--version`, anything else exits 2 with the usage. N2b-10 replaces it with the call to the shell.
  - **`imgui_impl_sdl3.{cpp,h}`** from ImGui's tag `v1.92.9b` (MIT), unmodified; propose its `native/THIRD_PARTY.md` row.
  - **`native-app.yml`**, on pushes to `native` and `wp/**` and on PRs into `native` that touch `native/**`, `port/app/**`, `port/src/**` or the workflow: three jobs with the toolchains of `native.yml`'s native jobs, each building SDL3 3.4.18 from its release tag (cached by tag), configuring with `-DMH4N_APP=ON`, building, running ctest (tests that need a Vulkan device skip without one; lavapipe is installed on Linux) and `mh4u-native --version`. The Windows job uploads `mh4u-native-windows` (the executable, `SDL3.dll`, the llvm-mingw DLLs) on pushes, 30 days.
  - **`check_native_deps.py`** also checks `native/app/**` and every `port/app/` source that a `native/app/` `CMakeLists.txt` names: no Azahar include or target (as today), and no SDL2 include (`<SDL.h>`, `"SDL.h"`, `<SDL2/…>`); and no SDL or ImGui include under `native/src/`.
  - **`tests/test_native_cmake_layout.py`** gains: with `-DMH4N_APP=OFF` the tree configures and builds as before; with SDL3 present (skip otherwise), an app module that registers a `runner` option shows it in `mh4u-native --help` and not in `mh4u_native_runner --help`.
  - **`docs/native/app.md`:** how to build and run `mh4u-native` (rule 2's configure line), where its code lives, its CI job and artifact; the later WPs fill in the rest.
- **Done when:**
  - the app build of rule 2 succeeds in the cloud and `build/native-app/bin/mh4u-native --version` prints the revision;
  - the configure line of `N1.md` without `-DMH4N_APP` still builds, ctest passes, and `mh4u_native_runner --help` is unchanged;
  - `python3 -m pytest tests/test_native_check_native_deps.py tests/test_native_cmake_layout.py` passes;
  - *(CI)* the three jobs of `native-app.yml` are green and the Windows artifact exists.
- **Traps:** SDL2 and SDL3 export the same function names (ROADMAP §4): nothing in `native/` may link SDL2. On macOS keep Xcode's clang (N1-22's *As built*: Homebrew's clang breaks the vendored {fmt}); build SDL3 with the job's own compiler. On Windows, SDL3 built with llvm-mingw, not MXE.
- **Out of scope:** the window and everything that runs (N2b-10 on).

### N2b-04 · Screen layouts and touch mapping
- **Size / agent:** M · native-worker
- **Depends on:** N1-20 · **Needs:** none
- **Goal:** the arithmetic of the app's screen layouts and of touches on them, as the Azahar build has it with the port's patch: where the top and bottom screens go in a window of any size, with the widened top screen, the aspect-ratio option and the touch overlay (corner, opacity, crop, extra views), and which bottom-screen pixel a click is.
- **Read first:** in Azahar: `src/core/frontend/framebuffer_layout.cpp` and `.h`, `src/core/frontend/emu_window.cpp` (`TouchPressed`, `TouchMoved`, `TouchReleased`, `ClipToTouchScreen`); the hunks of `port/azahar/azahar.patch` for those files and `core/frontend/top_screen.h`; `port/app/runtime_azahar/azahar_options.cpp` (`LayoutOptionFor`: overlay is Azahar's custom layout, stacked its default, single and side); `port/src/platform.h` (`ScreenOverlay`); `docs/research/widescreen.md` (the widened top screen) and `second-screen.md`.
- **Files (allowlist):** `native/src/screen_layout/**` (new), `native/tests/screen_layout_test.cpp`.
- **Spec:**
  - **`mh4n::screen_layout`:** `enum class Mode { Overlay, Stacked, Single, Side }` with the settings names `overlay`, `stacked`, `single`, `side` (`windows`, any other name: `Overlay`, and the caller logs it, D62); `struct Inputs { u32 window_w, window_h; Mode mode; u32 top_width; std::string aspect_ratio; Port::ScreenOverlay overlay; }` (`top_width` 0 means 400; `aspect_ratio` one of the hooks' names: `default`, `stretch`, `4:3`, `16:9`, `16:10`, `21:9`); `struct Layout` with the top and bottom rectangles in window pixels, whether the bottom is shown, its opacity, its crop and up to `Port::ScreenOverlay::MAX_VIEWS` extra views (rectangle and crop each).
  - `Layout Compute(const Inputs&)`: Azahar's layout functions for the four modes with the patch's changes (`TopScreenWidth`, the custom overlay layout, the clamps of `SetScreenOverlay`), ported with provenance (D6).
  - **Touch:** a `TouchTracker` with `Press(layout, x, y)`, `Move(layout, x, y)`, `Release()` and the touch point in bottom-screen pixels (0..319, 0..239) or none, following `emu_window.cpp` with the patch: a press on an extra view maps through that view's crop and keeps to it while held; a press on the bottom screen maps through its crop; a move outside the touch screen is clipped to it.
  - Pure arithmetic: no GPU, no SDL.
- **Done when:** ctest passes (`screen_layout_test`): each mode at 800×480, 1920×1080, 3440×1440 and 1280×1024, with top widths 400, 427 and 573, against rectangles worked out by hand from Azahar's formulas and the patch (written in the test with the working); each aspect-ratio option in single mode; the overlay in each corner, with an `x`/`y` placement, a crop and two views; touches on the bottom screen, on a crop, on a view, dragged off the screen and released.
- **Traps:** Azahar's layouts work in a rotated frame (`is_rotated`); the 3DS's framebuffers are stored rotated, the layout's rectangles are not. Keep the patch's clamps exactly (a crop of at least 1% each way). The copied code needs the provenance header and a `native/THIRD_PARTY.md` row (AGENTS §8 rule 6; `tests/test_native_third_party.py`).
- **Out of scope:** drawing (N2b-05); where the values come from (the platform, N2b-22).

### N2b-05 · Compositor and the presentation contract
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2-21, N2-06, N2b-04 · **Needs:** none
- **Goal:** the contract by which a GPU hands its displayed screens to whoever shows them (`mh4n/present.h`), and the compositor that draws one such frame into a target with a layout, with the app's filters and opacity, and with a UI layer on top.
- **Read first:** [`../ROADMAP.md`](../ROADMAP.md) §3 (principles 4 and 6); `native/include/mh4n/gpu.h` and `gpu_debug.h`; N2-21's backend (`native/src/gpu/backend/`) and `docs/research/rhi-spike.md` (N2-01); `native/src/gpu_capture/image.h` (`DisplayShadow::BuildScreen`: what a displayed screen is, its size and orientation); `native/src/screen_layout/` (N2b-04); `port/app/ui/layer.h` (the UI's draw copy).
- **Files (allowlist):** `native/include/mh4n/present.h` (new), `native/src/present/**` (new), `native/tests/present_test.cpp`, `docs/native/presentation.md` (new).
- **Spec:**
  - **`mh4n/present.h`** (`namespace mh4n::present`):
    - The header is self-contained (it includes only `mh4n/` headers): the backend's types appear as opaque pointers that the producers and the compositor, which both link N2-21's backend, cast back.
    - `RgbImage { u32 width, height; std::vector<u8> rgb; }`: 3 bytes a pixel, top row first (the layout of `gpu_capture::Image`, which converts to it).
    - `ScreenImage`: either the backend's image (`std::shared_ptr<const void>`, an object of N2-21's image type, kept alive while the frame is) with the region that holds the screen, the scale (image pixels per displayed pixel) and the storage orientation (the 3DS's framebuffers are stored rotated by 90°), plus the backend timeline value after which it holds the pixels; or CPU pixels (RGBA8, top row first, already upright); and in both cases the displayed size at 1x (top: 240 high and 400 wide, or the hooks' width; bottom: 320×240).
    - `DisplayFrame { u64 sequence; u32 game_frame; u64 tick; std::optional<ScreenImage> top, bottom; bool gas; }`: what a VBlank of the top screen shows (`nullopt`: that screen is off or not yet set up); `gas`: a draw since the previous frame used the gas mode (fog mode 7).
    - `ScreenSink { virtual void Publish(const DisplayFrame&) = 0; }`: thread-safe, returns at once.
    - `Display`, which a `gpu::Gpu` that can show its screens also implements (found with `dynamic_cast`, like `GpuDebug`): `AddScreenSink(ScreenSink*)`, `RemoveScreenSink(ScreenSink*)` (before and after the run), and `ReadScreen(const ScreenImage&, std::function<void(RgbImage)> done)`, the screen at 1x as RGB for screenshots (downscaled from the internal scale with a box filter, `done` on any thread).
    - `UiLayer { virtual void Record(void* recorder, u32 target_width, u32 target_height) = 0; }` (`recorder`: the backend's command recorder, opaque): called by the compositor after the screens, on the thread that composes.
  - **`present::Compositor`** (the backend of N2-21, shaders in GLSL compiled with N2-06's glslang at first use and kept): `Compose(recorder, target, const DisplayFrame&, const screen_layout::Layout&, const ComposeOptions&)` clears the target (black), draws each shown screen into its rectangle (rotated upright, the region only), the bottom screen with the layout's opacity and crop and each extra view with its crop, sampling nearest or linear (`ComposeOptions::linear_filter`, the app's `linear_filter`), uploads CPU-pixel screens itself, and records the UI layer last. A target is a backend image of any size (offscreen) or a swapchain image (N2b-06).
  - Its own pipelines are made at start-up (the app's own UI and blits are not in N2-16's cache).
  - **`docs/native/presentation.md`:** the contract, the compositor, how a frame travels from VBlank to the window (completed by N2b-06 and N2b-23).
- **Done when:** ctest passes on lavapipe (`present_test`): synthetic `DisplayFrame`s (backend images and CPU pixels, at scale 1 and 3, both orientations) composed into offscreen targets of the sizes of N2b-04's test for each mode and read back: each screen's pixels land in N2b-04's rectangles (corners and centre exact with nearest filtering); the overlay's opacity blends as `src × a + dst × (1 − a)` within 1/255; a crop and two views show the right parts; a widened top screen (573×240) fills its rectangle; a missing screen leaves black; the UI layer's draw lands over the screens; `ReadScreen` of a 3x image equals the box-filtered 1x pixels.
- **Traps:** the device and queue are N2-21's: if its backend cannot give a second user (the compositor) what it needs (recording, submission from another thread, images it did not make), stop with `needs-decision` naming the missing piece rather than editing `native/src/gpu/backend/`, which N2a's WPs still change. Vulkan is loaded at run time (volk): tests skip without a device.
- **Out of scope:** swapchains and the present thread (N2b-06); where frames come from (N2b-07, N2b-13).

### N2b-06 · Swapchain and the present thread
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-05 · **Needs:** none
- **Goal:** principle 4 for presentation: a present thread takes the newest `DisplayFrame` from a mailbox, composes it with the current layout and the UI layer, and presents it to a swapchain (or writes it to an offscreen target), handling vsync, resizes, minimising and shutdown, and logs when each frame was shown. Publishing a frame never blocks the renderer or the game.
- **Read first:** `docs/native/presentation.md` and `native/include/mh4n/present.h` (N2b-05); the Vulkan specification's chapters on `VK_KHR_swapchain`, present modes, `VK_KHR_present_id`/`VK_KHR_present_wait` and `VK_EXT_headless_surface`; how Azahar's Vulkan renderer handles an out-of-date swapchain (`src/video_core/renderer_vulkan/vk_swapchain.cpp`, `vk_present_window.cpp`).
- **Files (allowlist):** `native/src/swapchain/**` (new), `native/tests/swapchain_test.cpp`, `docs/native/presentation.md`.
- **Spec:**
  - **`swapchain::Presenter`**, made with the backend, a target (a `VkSurfaceKHR` the caller made, or an offscreen size) and a `LayoutSource` (a callback the present thread calls per frame for the current `screen_layout::Layout` and `ComposeOptions`):
    - is a `present::ScreenSink`: `Publish` stores the frame in a one-slot mailbox (a newer frame replaces an older one that was not shown, counted as dropped) and wakes the present thread;
    - `RequestRedraw()`: compose the last frame again (the UI changed while the game shows nothing new);
    - `SetVsync(bool)`: FIFO when on; when off, MAILBOX if the surface has it, else IMMEDIATE;
    - `Resize(w, h)` and the swapchain's own out-of-date and suboptimal answers recreate it on the present thread; a zero extent (minimised) skips presenting, the frames meanwhile counted as dropped (D60);
    - `Stop()`: the thread finishes the frame it has and exits; destruction waits for the device to be idle.
  - **Present log** (`SetPresentLog(path)`; the app's option, N2b-10): a CSV `# present v1 <date>` header, then one row per presented frame: `sequence,game_frame,published_us,composed_us,presented_us,displayed_us,dropped_before` (`displayed_us` from `VK_KHR_present_wait` when the device has it, else empty), host times from the steady clock.
  - **Offscreen target** (`--present-dump`, N2b-10): `DumpTo(dir, every_n)` writes each n-th composed image and, at `Stop`, the last one, as `presented_<sequence>.ppm` and `last.ppm`.
  - **Rests on N2-21** for a graphics queue that can present, submission from the present thread, and instance extensions for surfaces (`VK_KHR_surface`, the platform's, and `VK_EXT_headless_surface` when the loader has it): if the merged backend lacks one, stop with `needs-decision` naming it (the planner's report proposes adding them to N2-21).
- **Done when:** ctest passes (`swapchain_test`): on lavapipe, a `VK_EXT_headless_surface` swapchain (skipped with a message when the driver lacks the extension; the offscreen cases always run) presents frames in order; three frames published faster than they are presented show only the newest and count two dropped; `Publish` returns in under 1 ms while the present thread is held inside a composition (a test hook); a resize recreates the swapchain and the next frame has the new size; a zero extent presents nothing; `Stop` with a frame pending returns; the present log's rows match the frames shown; vsync off picks MAILBOX or IMMEDIATE as the surface offers.
- **Traps:** a swapchain's images belong to the presentation engine until acquired: never compose into one that was not acquired. One `VkQueue` is shared with the renderer thread: every submission goes through the backend's submission path (its lock), never around it.
- **Out of scope:** the window and its events (N2b-10); locking the VBlank to the display (N2b-23).

### N2b-07 · Displayed screens from guest memory (`--gpu null-display`)
- **Size / agent:** S · native-worker (build-heavy)
- **Depends on:** N2b-05, N1-17 · **Needs:** none
- **Goal:** a GPU that shows something without the renderer: the null GPU, with each VBlank's screens built from guest memory the way Azahar's software renderer shows them, so the app, the shots and the CI can run on our own programs (whose pictures the CPU draws) before the renderer is in the runtime.
- **Read first:** `native/src/gpu_null/null_gpu.h`; `native/src/gpu_capture/image.h` (`DisplayShadow`); `native/include/mh4n/present.h` (N2b-05); `port/tests/io_test.c` (a picture written by the CPU into the top screen's default framebuffer, with no GSP call).
- **Files (allowlist):** `native/src/display_mem/**` (new), `native/tests/display_mem_test.cpp`.
- **Spec:**
  - Registers the `gpu::GpuFactory` `null-display`: the `null` GPU (found in the registry, not copied) wrapped in a `Gpu` that forwards every call and also implements `present::Display`.
  - It keeps a `gpu_capture::DisplayShadow`: loaded at start from the inner GPU's register file (`ReadReg` over the GPU and LCD register windows, which the null GPU initialises as Azahar's `PicaCore::InitializeRegs`), then updated by `WriteReg`, `SetBufferSwap` and `SetColorFill` as the capture player updates it.
  - At `VBlank(0)` (the top screen; the bottom screen is built at the same time) it builds both screens with `BuildScreen` from the runtime host's physical memory, on the scheduler thread (principle 6: reading only), and publishes a `DisplayFrame` of CPU pixels to its sinks; `game_frame` is the frame counter when the program maps it, else 0. `ReadScreen` returns the CPU pixels.
  - The top screen's height follows the hooks' rows when `NativePlatform::Find(rt)` reports them (non-zero `TopScreenRows`), as the reference's hooks write the framebuffer height register.
- **Done when:** ctest passes (`display_mem_test`): through the runtime's `gpu::Host` with synthetic memory, a top framebuffer in VRAM at the initial register values and an RGB8 bottom one after a `SetBufferSwap` are published with the expected pixels and orientation; a colour fill shows the fill; a sink added and removed receives frames only in between. (`io_test` through the runner is N2b-14's check.)
- **Traps:** the null GPU skips display transfers and texture copies, so the game's own screens stay as the CPU left them under `null-display`: it is a tool for our programs and for the app's plumbing, not a picture of the game.
- **Out of scope:** the renderer's displayed screens (N2b-13).

### N2b-08 · Live input, latched at frame boundaries
- **Size / agent:** S · native-worker (build-heavy)
- **Depends on:** N1-16, N1-09 · **Needs:** none
- **Goal:** an input source the app can feed from any thread, which the game sees change only between its frames (proposed D59), so that any live session can be written down as a frame-keyed script and replayed on both runners.
- **Read first:** `native/include/mh4n/host.h` (`InputState`, `InputSource`, `InputSourceFactory`); `native/src/input_script/` (the script source); `port/src/schedule.h` and `schedule.cpp` (the script grammar: buttons, `TOUCH:x:y`, `CIRCLE:x:y`, holds in frames); `docs/native/runner.md`.
- **Files (allowlist):** `native/src/input_live/**` (new), `native/tests/input_live_test.cpp`.
- **Spec:**
  - Registers the `InputSourceFactory` `live`. `input_live::Find(Runtime&)` gives the source; `Set(const InputState&)` is thread-safe (the main thread calls it).
  - **Latching** (`--input-latch frame|none`, default `frame`): at each frame change (`Runtime::OnFrame`), the state set last becomes the one `Sample` returns until the next frame change; before the game's frame counter has been seen (and in programs that never map it) `Sample` returns the state set last.
  - **`--record-input FILE`:** every change of the latched state is written as script lines that `--input-file` reads back (`f<N>:<BUTTON>:f<held>`, `f<N>:TOUCH:<x>:<y>:f<held>`, `f<N>:CIRCLE:<x>:<y>:f<held>`, in the grammar of `schedule.h`; milliseconds of game time instead of frames before the frame counter is seen), flushed at the end of the run. The C-stick, ZL/ZR and motion are not written (the script has no syntax for them; N3-03).
- **Done when:** ctest passes (`input_live_test`): states set between two frame changes are seen only from the second; with `--input-latch none` at once; a recorded sequence (buttons, touch, circle pad, overlapping holds) replayed through the script source gives the same `Sample` at every frame of a fake frame counter; a program without a frame counter is recorded in milliseconds.
- **Traps:** `Set` runs on the main thread and `Sample` on the scheduler thread: no lock may be held across the frame callback; a seqlock or a mutex-guarded copy is enough.
- **Out of scope:** mapping keys and controllers to buttons (N2b-09 for the keyboard; N3-03 for the rest).

### N2b-09 · `IRuntime` for the native runtime
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-01, N2b-03, N2b-06, N2b-07, N2b-08, N1-21 · **Needs:** ci, dump-check
- **Goal:** `NativeRuntime` implements `App::IRuntime` on the native runtime, so the app's start-up, options, paths, perf stats and input reach it the way they reach Azahar: the game runs on a game thread of its own and is shown through the present thread, and the methods that need something N2b does not have answer as documented.
- **Read first:** `port/app/runtime.h` (N2b-01) and `port/app/runtime_azahar/` (what each method does there); `native/include/mh4n/runtime.h` (`Boot`, boot steps, `RequestExit`, `SetRefreshRate`, `OnFrame`); `native/src/platform/platform_native.h`; `docs/native/runner.md` (the runner's options, exit statuses); `port/app/config.h`; D12, D37, D43, D44, D45; in Azahar: `src/common/file_util.cpp` (`GetUserPath`: how the user folder is found).
- **Files (allowlist):** `native/app/runtime/**` (new), `native/app/tests/app_runtime_test.cpp`, `docs/native/app.md`.
- **Spec:**
  - **Start-up:** `Load` builds the runner's option list (rule 2: the `runner` scope) from the request (PROGRAM: a game folder or an ELF; `--code`), the settings (`--user` always, D44; `--scale`, `--hd`, `--perf-log`, `--gpu`, `--input-source live`), and the command line the shell passes through; parses it; starts the game thread with `Runtime::Boot`; and returns once a boot step of phase `Started`, registered by this module, has run (`LoadResult::Ok`) or `Boot` has returned (status 2: `Failed` with `StatusDetails` from the run's reason; a game folder without `exheader.bin`: `NotAGame`). The step then holds the game thread until `Start`. `--gpu` is `renderer` when it is registered (N2b-12), else `null-display`, with an Info line; an explicit `--gpu` wins. The input source is `live` (N2b-08) unless the command line has `--input-source`, `--input` or `--input-file` (then the script's, logged).
  - **The game:** `request.game` is a game folder made by `tools/native/prepare_game.py`, or an ELF; a `.3ds`, `.cci` or `.cxi` (what `mh4u-app.ini`'s `game` names for `mh4u`) is replaced by the folder of D61 when it exists, else `Load` answers `NotAGame` with details that name `prepare_game.py` (N3-06 makes the folder from the launcher). The code library: `request.code_library`, else `MH4U_CODE`, else the file of D61; one that does not match the game is refused (D13) and `CodeNotice` says so.
  - **Presentation:** `Load` makes the `swapchain::Presenter` on a Vulkan surface of the main window (`SDL_Vulkan_CreateSurface` on `IWindow::SdlWindow()`; a window without one, a test's or `--present-dump`'s, gets the offscreen target), registers it as a sink of the GPU's `present::Display` (a `Devices`-phase step named after `runtime.devices`), and gives it a `LayoutSource` built from the layout setting and the window's size (N2b-22 adds the hooks' values).
  - **Run:** `Start` releases the game thread. `RunStep` waits until the present thread shows a frame or 16 ms pass, and returns `Running`, `ShutdownRequested` (the game ended with status 0: the program exited, `CloseApplication`, D43) or `Failed` (2, 3 or 4, with the reason). `Stop` posts `RequestExit(0)` through the kernel, joins the game thread (releasing the start hold if `Start` never came), stops the presenter and the renderer. `WaitForBackgroundTasks` joins the renderer's workers. `TakePerfStats`: game frames per second (frame changes), VBlanks per second (frames published), emulation speed (game ticks over host time; 1.0 unless the clock was held).
  - **Options:** `ApplyConfig` maps `resolution` (0: the window's height over 240, rounded, 1 to 10), `vsync`, `linear_filter`, `screens` (the layout; `windows` becomes overlay with a Warning, D62), `custom_textures` (the HD archive of D61 when it exists, else an Info line), `perf_log`; `renderer` other than `vulkan`, `cpu_clock`, `new_3ds` and `volume` are logged once and have no effect (D10; ROADMAP principle 3; sound is N3-02). `SetVsync`, `SetLinearFilter`, `ApplyLayout` take effect on the next frame; `SetResolution` at the next `Load` unless the renderer reports a live change (logged). `SetGameOption` posts `Hooks::SetOption` to the scheduler and waits up to 500 ms for its answer (false after that, logged). `SetLayoutEditing` posts the hooks' call as the Azahar runtime makes it. `ActiveRenderer` is Vulkan.
  - **Input:** `BuildInputProfile` keeps, per 3DS button and circle-pad direction, the codes of the settings' key lists (`Input::KeyCodes`); `KeyDown`, `KeyUp` and `ReleaseAllKeys` update the pressed set and hand the resulting `InputState` to the live source (N2b-08); `SetInputMuted` as the interface says; the touch point comes from the shell (N2b-10). Controllers, the mouse as the camera (`SetMouseStick`, `CameraTurnActive` false) and the Circle Pad Pro are N3-03: the methods log once.
  - **Paths:** the user folder resolved as Azahar's `GetUserPath(UserDir)` at the pinned commit (ported with provenance), so one user folder serves both executables (D37, D61); `SetUserDir`; `GameOptionsPath` (`<user>/mh4u.ini`) and `GameOptionsTemplate` (`Port::OptionsTemplateText()`, N2b-01). Paths are UTF-8 (D45).
  - **What N2b does not have** answers as documented in `app.md`: rooms (`RoomsAvailable` false; `JoinRoom` reports `RoomError::Unknown` once; `OpenRoom` not ok; N4), the app's keyboard (`SetSoftwareKeyboard` kept, the runtime's own keyboard answers, `FinishKeyboard` returns `KeyboardError::None` and does nothing; N3-04), titles (`ReadGameHeader` of a game folder reads `exheader.bin`; `FindInstalledTitle` not found; `CheckCia` and `InstallCia` `Invalid`; N3-06), `EndSkipPending` empty (N4), button and stick setting names as Azahar's (`util/buttons.h`), bindings empty.
  - **`app.md`** gets the method table: what each does natively, and the WP that completes it.
- **Done when:**
  - `ctest --test-dir build/native-app -R app_runtime` passes: with a fake `IWindow` and a test ELF whose main thread is a C++ function (`Runtime::SetMainThreadEntry` through a test hook of `NativeRuntime`, as `runtime_test.cpp` boots one), `Load` → `Start` → `RunStep` until `ShutdownRequested`, the exit status mapped; a failing load gives `Failed` with the reason; `Stop` before `Start` returns; the options list built from a synthetic `Config` (each key above); `SetGameOption`'s answer and its timeout; keys to buttons through the live source; the user folder rules on synthetic folders (portable `user/` next to the executable first);
  - *(CI)* `native-app.yml` is green;
  - (with the dump) `MH4N_GAME=build/native-game MH4N_CODE=<the --native library> ctest --test-dir build/native-app -R app_runtime_game` passes: a test (skipped without the two variables) loads the game folder through `NativeRuntime` with `--gpu null-display`, a fake window and G1's script (`--input-file tools/native/golden/G1.txt`), reaches game frame 1450 (G1's title) and stops cleanly.
- **Traps:** every `IRuntime` call comes from the main thread: anything that touches guest memory or kernel objects is posted (rule 3). The `Started` step must not block the scheduler's own host thread once `Stop` asks it to end.
- **Out of scope:** the window and the run loop (N2b-10); the UI (N2b-11).

### N2b-10 · `mh4u-native`: window, events and the run loop
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-09, N2b-02 · **Needs:** ci
- **Goal:** `mh4u-native` opens an SDL3 window (or none, headless), turns its events into `App::InputEvent`s, keys and touches, and runs the game through `IRuntime` from start to clean exit: resize, full screen, minimise, focus loss and close behave as in `mh4u` (D60), and our own programs run end to end through it in CI.
- **Read first:** `port/app/main.cpp` (the Azahar app's flow, the order of start-up and shutdown) and `window.cpp` (its events, full screen, focus, mouse and touch); `port/app/app_window.h` (N2b-01, N2b-02); `native/app/runtime/` (N2b-09); `native/src/screen_layout/` (touch); SDL 3.4.18's headers (`SDL_video.h`, `SDL_events.h`, `SDL_vulkan.h`).
- **Files (allowlist):** `native/app/window/**` (new), `native/app/shell/**` (new), `native/app/program/**`, `native/app/tests/app_window_test.cpp`, `tests/test_native_app.py` (new), `docs/native/app.md`.
- **Spec:**
  - **`native/app/window/`:** `NativeWindow` (`App::IWindow` on SDL3: Vulkan, high-DPI, resizable, minimum size half the 3DS's, the icon through `App::Host`), the SDL3 implementation of `App::Host`, and the translation of SDL3 events into `App::InputEvent` (rule 1). Full screen is SDL3's borderless desktop full screen, toggled by `fullscreen_key` (F11) and by `--fullscreen`/`--windowed` (D60).
  - **`native/app/shell/`** defines `mh4n::app::Main`: parse the options (`runner` scope; the app's own: `--windowed`, `--fullscreen`, `--window-size WxH`, `--headless`, `--present-dump DIR`, `--present-dump-every N`, `--present-log FILE`, `--replay-events FILE`, and `--no-launcher` accepted and ignored for N3-10), resolve the user folder, load `mh4u-app.ini` (`App::Load`) and write `mh4u.ini` from the template when there is none (as `main.cpp`), make the window, then `ApplyConfig`, `PrepareCode`, `Load`, `Start`, and loop: pump events → keys to `IRuntime::KeyDown`/`KeyUp`, the mouse's left button and motion to the touch tracker of N2b-04 on the presenter's current layout (the touch point to the live source), window events to the presenter (`Resize`, minimise) → `RunStep`; until the window closes, `Quit`, or `RunStep` says otherwise; then `Stop` and the shutdown order of `main.cpp`. Focus loss calls `ReleaseAllKeys` (as `window.cpp`); the game keeps running while minimised (D60).
  - **Headless** (`--headless`): SDL3's `offscreen` video driver; with `--present-dump`, the presenter's offscreen target instead of a swapchain (`--window-size`, default 800×480). `--replay-events FILE` injects window events at game frames or milliseconds (`f<N>:` or `<ms>:` then `key <scancode> down|up`, `mouse <x> <y> down|up|move`, `resize <w> <h>`, `close`), for tests.
  - **The program** (`native/app/program/main.cpp`): converts the Windows command line from UTF-16 (D45, as the runner) and calls `Main`.
  - **`tests/test_native_app.py`** (skips without `MH4N_APP_EXE` or the built test programs, which it builds as `tests/test_conformance.py` does): `io_test` run by `mh4u-native --headless --present-dump` with the reference runner's input script of `tests/test_port.py` gives the same printed events as that test asserts, and `last.ppm` has `io_test`'s green band, red half and blue half inside the top screen's rectangle of the overlay layout at 800×480; a `--replay-events` mouse press inside the bottom screen's rectangle makes `io_test` print the touch point N2b-04's mapping gives; a `close` event ends the run with status 0.
  - `app.md`: the options, the start-up order, the threads, the files it reads and writes.
- **Done when:**
  - `ctest --test-dir build/native-app -R app_window` passes (with `SDL_VIDEO_DRIVER=offscreen`: events pushed with `SDL_PushEvent` come out as the expected `InputEvent`s; full screen toggles; a resize reaches the presenter);
  - `MH4N_APP_EXE=build/native-app/bin/mh4u-native python3 -m pytest tests/test_native_app.py` passes;
  - *(CI)* `native-app.yml` is green, and its Linux job runs `tests/test_native_app.py` (the job is N2b-03's; if a step must be added, propose it in the report).
- **Traps:** SDL3's video subsystem lives on the main thread (`SDL_Init` there; events pumped there). `SDL_Vulkan_CreateSurface` needs the instance the backend made with SDL's instance extensions (N2b-06's *Spec*). A key event that is a controller's echo (Steam Input) is dropped, as `window.cpp` does.
- **Out of scope:** the UI (N2b-11); controllers (N3-03); the separate-windows layout (D62).

### N2b-11 · The app's UI in `mh4u-native`
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-10 · **Needs:** ci, pc
- **Goal:** the app's in-game menu, keyboard, hint, frame-rate counter and key guides run in `mh4u-native` from the same sources as in `mh4u`: built on the main thread, drawn by the present thread over the game through the compositor's UI layer.
- **Read first:** `port/app/ui/ui.h`, `layer.h`, `ui.cpp`, `render_vk.cpp` (how the Azahar app draws the copy with ImGui's Vulkan backend); `port/app/third_party/imgui/backends/imgui_impl_vulkan.h` (`IMGUI_IMPL_VULKAN_USE_VOLK`) and `imgui_impl_sdl3.h`; `native/include/mh4n/present.h` (`UiLayer`) and `docs/native/presentation.md`.
- **Files (allowlist):** `native/app/ui/**` (new), `native/app/shell/**`, `native/app/window/**`, `native/app/tests/app_ui_test.cpp`, `tests/test_native_app.py`, `docs/native/app.md`, `port/app/ui/**` (fixes found on SDL3, each kept working in `mh4u`: rule 1).
- **Spec:**
  - `native/app/ui/` compiles the shared UI sources (the list of `tests/test_native_app_portable.py`) and ImGui's SDL3 and Vulkan backends; `NativeWindow` implements `ImGuiInit`, `ImGuiNewFrame` and `ImGuiForwardCurrentEvent` with the SDL3 backend.
  - **Drawing:** a `present::UiLayer` that draws the layer's draw copy with `imgui_impl_vulkan` (with volk) into the compositor's command buffer, holding the layer's lock as `render_vk.cpp` does, when D4's backend exposes its Vulkan handles; otherwise a renderer of `ImDrawData` on the compositor's pipelines (the worker reports which). The present thread calls `Presenter::RequestRedraw` whenever the UI is visible, so the menu stays live when the game shows nothing new.
  - **The shell:** `Ui::Start` after the window and before `Load`, `Ui::OnEvent` before the game gets an event, `Ui::Frame` each loop, `Ui::Stop` after `IRuntime::Stop`; `fps_counter_key` and `key_guides_key` as `window.cpp`.
  - `app.md` gets the maintainer's smoke test of `mh4u-native`, as exact steps on the reference PC with the Windows artifact: start with the game folder and the `--native` library, Esc menu over the game, change `fps` from 60 to 120 in the options tab (the log shows `gsp: vblank period 2234265 ticks`), the frame-rate counter, the key guides, full screen, quit.
- **Done when:**
  - `ctest --test-dir build/native-app -R app_ui` passes: with a fake window, an Escape `InputEvent` opens the menu (`Ui::Visible()`), a frame composed offscreen with the UI layer differs from the frame without it inside the menu window's rectangle (as ImGui reports it);
  - `tests/test_native_app.py` gains a case that passes: `io_test` headless with `--replay-events` pressing Escape at 1000 ms, and `last.ppm` shows the menu (its window rectangle differs from `io_test`'s picture);
  - *(CI)* `native-app.yml` is green; if a file under `port/app/` changed, `linux.yml`, `windows.yml` and `macos.yml` dispatched on this branch are green too;
  - the maintainer's smoke test of `app.md` passes on the reference PC.
- **Traps:** ImGui's Vulkan backend uploads its font texture on first use: on the present thread, under the layer's lock. Keep one ImGui context, as `mh4u` has.
- **Out of scope:** the app's keyboard answering the game (N3-04); the launcher (N3-10).

### N2b-12 · The renderer as the runtime's GPU
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N1-21, N2-15, N2-16, N2-18, N2-20, N2-22, N2b-18 (`gpu_null`, below) · **Needs:** dump-check
- **Goal:** `--gpu renderer` puts N2a's renderer behind the runtime's GSP in place of the null GPU: on its own thread, signalling the same interrupts with Azahar's delays, doing DMA as D41 says, with its settings (scale, HD archive, pipeline cache) as runner options, and a per-frame log for the budget.
- **Read first:** `native/src/gpu_null/null_gpu.h` (what the GSP and the game expect of a GPU: interrupts, delays, DMA, the register file); D25 and D41; `native/include/mh4n/gpu.h` (`Host::GuestPointer`); N2-08's, N2-15's, N2-16's, N2-18's and N2-22's specs and reports (`--gpu native`, the `queue` wrapper, the options each reads); `docs/native/capture-player.md`; `docs/native/runner.md`.
- **Files (allowlist):** `native/src/gpu/runtime/**` (new), `native/src/gpu/gx/**`, `native/src/gpu/surface/**`, `native/src/gpu_null/**` (only to expose the delay functions in a header, behaviour unchanged), `native/tests/gpu_runtime_test.cpp`, `tests/test_conformance.py` (`MH4N_RUNNER_ARGS`: extra arguments for the native runner), `docs/native/renderer-runtime.md` (new).
- **Spec:**
  - **Registers the `gpu::GpuFactory` `renderer`:** N2-08's `native` GPU from the registry, wrapped by N2-22's `queue` wrapper from the registry, with the runtime's `gpu::Host`.
  - **Interrupts:** the ones the null GPU signals for each command, with the delays the null GPU computes, handed to `Host::SignalInterrupt` when the renderer completes the command (D25, D41). The delay computation (Azahar's `DelayGenerator`, private in `NullGpu` today) moves to a header of `native/src/gpu_null/` that both GPUs call; `gsp_test` and the null GPU's behaviour stay as they are.
  - **DMA (D41's open follow-ups):** `GxEngine::Dma` copies between any two mapped ranges through `Host::GuestPointer`, with `CopyBlock`'s rules as the null GPU has them (pieces of at most a page walked forwards; an unmapped source page zero-fills; an unmapped destination page is skipped; `memmove` inside a piece); the renderer's DMA path calls its caches' `FlushRegion` on the source and `InvalidateRegion` on the destination, as Azahar's `RequestDma`.
  - **Runner options** (`runner` scope), the same names the renderer's modules read in the capture player's scope, so they are read unchanged: N2-15's `--scale N` (default 1), N2-18's `--hd ARCHIVE` and its VRAM budget option, N2-16's options; the pipeline cache in `<user>/cache/pipelines/` (none without `--user`, with an Info line). `renderer-runtime.md` lists them.
  - **`--gpu-log FILE`:** a CSV `# gpulog v1 <date>` header, then one row per top-screen VBlank: `game_frame,translate_us,gpu_us,queue_wait_us,` then the GPU's frame counters (`TakeFrameCounters`: ubershader draws, HD textures ready and evicted, VRAM in use…), `translate_us` being the renderer thread's busy time and `gpu_us` the timestamps' (`TakeFrameGpuTimeNs`).
- **Done when:**
  - ctest passes (`gpu_runtime_test`, on lavapipe): a DMA between heap and linear memory, a DMA from an unmapped page (zero fill) and to one (skipped), each with the caches flushed and invalidated; the interrupts and delays of a fill, a transfer and a command list equal the null GPU's for the same commands; the options reach the factory;
  - `MH4N_RUNNER=build/native/bin/mh4u_native_runner MH4N_RUNNER_ARGS="--gpu renderer" python3 -m pytest tests/test_conformance.py` passes (the test passes `MH4N_RUNNER_ARGS` to the native runner, split like a shell; without it, as before);
  - (with the dump) `golden.py check G1 --candidate-runner <native runner> --candidate-program build/native-game --candidate-code <the --native library> --candidate-args "--gpu renderer --scale 1" --require checkpoints,ipc,svc,hid` passes (the `cross` profile, as N1-21 with the null GPU); a difference that needs a change outside this WP's files stops it with `blocked` and the evidence.
- **Traps:** the GSP pops the interrupt queue with LDREX/STREX (N1-17): the renderer never writes it; interrupts go through the host. A `ReadReg` must answer from the wrapper's register mirror without waiting for the renderer thread.
- **Out of scope:** what is displayed (N2b-13); presentation (N2b-05, N2b-06).

### N2b-13 · Displayed screens from the renderer
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-12, N2b-05 · **Needs:** dump-check
- **Goal:** VBlank drives presentation: at each VBlank the renderer finds the surfaces the display registers point at (the top screen at the hooks' widened height) and publishes them as a `DisplayFrame`, without a copy through guest memory; and a check shows they are what guest memory would show.
- **Read first:** `native/include/mh4n/present.h` and `docs/native/presentation.md` (N2b-05); N2-15's surface cache; `native/src/gpu_capture/image.h` (`DisplayShadow`: the reference for what is displayed); `docs/research/widescreen.md` (*How a frame reaches the screen*, the top screen's height of 400 that the hooks change); `native/src/platform/platform_native.h` (`TopScreenRows`).
- **Files (allowlist):** `native/src/gpu/runtime/**`, `native/src/gpu/native/**`, `native/src/gpu/surface/**`, `native/tests/gpu_display_test.cpp`, `docs/native/renderer-runtime.md`.
- **Spec:**
  - The `renderer` GPU implements `present::Display`. On the renderer thread, at `VBlank(0)` (after the `SetBufferSwap`s the GSP applied), for each screen: the active framebuffer's address, format, stride and size from the display registers, the top screen's height replaced by `NativePlatform::Find(rt)->TopScreenRows()` when non-zero (the reference's hooks write that register, N1-20); the colour fill when it is on; a surface-cache lookup; when no surface holds the range, an upload from guest memory into a cached image. It publishes a `DisplayFrame` with `game_frame` and `tick` from the GSP's VBlank, the images' ready timeline values, and `gas` set when a draw since the previous frame had fog mode 7.
  - `ReadScreen`: N2-21's readback of the region, downscaled to 1x (box filter), rotated upright, RGB.
  - **`--display-check N`** (a runner option): every N-th top VBlank, write the displayed surfaces back to guest memory (N2-15's write-back, posted to the scheduler, principle 6), build the screens with `DisplayShadow::BuildScreen` from memory, read the published ones with `ReadScreen` at scale 1, and count the screens that differ in any byte; the count is logged at the end and is a column of `--gpu-log`.
- **Done when:**
  - ctest passes (`gpu_display_test`, on lavapipe): a synthetic sequence (a fill, a draw into a colour buffer, a display transfer to a display buffer, a buffer swap) publishes a frame whose `ReadScreen` equals `DisplayShadow::BuildScreen` over the written-back memory, at scale 1 and at scale 3 downscaled; the hooks' rows widen the top screen; a buffer no surface holds is uploaded; a draw with fog mode 7 sets `gas`;
  - (with the dump) `mh4u_native_runner` on G1 to frame 1700 and on G2 to its end, with G1's and G2's scripts and `--gpu renderer --scale 1 --display-check 60`, logs 0 differing screens.
- **Traps:** the GSP's VBlank comes from the scheduler thread through the queue: the lookup runs on the renderer thread in queue order, after the commands submitted before it. Azahar's software renderer shows a screen one column off at one edge (N2-02's open assumption): `DisplayShadow` decides what "displayed" means, so follow it and report any such column.
- **Out of scope:** screenshots (N2b-14); composing (N2b-05).

### N2b-14 · Screenshots in the native runner
- **Size / agent:** S · native-worker (build-heavy)
- **Depends on:** N2b-07 · **Needs:** none
- **Goal:** the native runner saves screenshots with the reference runner's options, names and timing, from any GPU that implements `present::Display`, so `golden.py` can compare them.
- **Read first:** `port/README.md` (the paragraph on `--shots`, `--shots-from`, `--shot-every` and the frame-keyed names); `port/src/schedule.h` (`When`); `tests/test_port.py` (`test_runner_scripted_input_and_screenshots`); `native/include/mh4n/present.h`; `docs/native/runner.md` (*Parity with the reference runner*).
- **Files (allowlist):** `native/src/shots/**` (new), `native/tests/shots_test.cpp`, `tests/test_native_shots.py` (new), `docs/native/runner.md`.
- **Spec:**
  - Options (`runner` scope) with the reference's syntax: `--shots DIR`, `--shots-from MS|fN`, `--shot-every MS|fN` (default 1000 ms).
  - A shot is taken at the first moment the frame counter reads that frame or more (frame-keyed), or the game clock passes that time: the module waits on the scheduler thread for the next published frame (a `ScreenSink` it adds) and reads both screens with `ReadScreen`; names `f%07u_top.ppm` and `f%07u_bottom.ppm`, or `%07u_top.ppm` in milliseconds, as the reference's. A frame the counter skipped has no shot.
  - `DIR/gas.txt` lists, one per line, the frames whose shot follows a published frame with `gas` set since the previous shot.
  - Without a `present::Display` GPU, `--shots` exits 2 with a message.
  - `runner.md`'s parity table: the three options move from the reference-only list to the shared ones.
- **Done when:**
  - ctest passes (`shots_test`): frame-keyed and millisecond schedules on a fake frame counter and clock; names; `gas.txt`;
  - `MH4N_RUNNER=build/native/bin/mh4u_native_runner python3 -m pytest tests/test_native_shots.py` passes: `io_test` with `--gpu null-display --shots DIR` writes `0001000_top.ppm` (400×240) with `tests/test_port.py`'s pixel checks (green band, red and blue halves), and `--shots-from 500 --shot-every 500` gives `0000500_top.ppm`.
- **Traps:** the reading blocks the scheduler thread until the renderer answers: acceptable for a debugging option (the reference stops too), never in a run without `--shots`.
- **Out of scope:** comparing images (N2b-17).

### N2b-15 · Synthetic GX programs
- **Size / agent:** M · native-worker-high
- **Depends on:** N0-26 · **Needs:** none
- **Goal:** our own 3DS programs that drive the GPU through the GSP the way a game does (memory fills, display transfers, texture copies, buffer swaps, VBlank waits), with pictures whose pixels are known exactly: a public-CI oracle for the GX path, the display registers and presentation, on the reference runner now and on the native runner with the renderer later.
- **Read first:** `port/tests/io_test.c`, `svc.h`, `build_svc_test.py`; `tests/test_port.py` (how a program is built, run and its screenshots checked); 3dbrew *GSP Services*, *GSP Shared Memory* and *GPU/External Registers*; libctru's `gx.h` and `gspgpu.c` (https://github.com/devkitPro/libctru, for the command layouts only; nothing copied).
- **Files (allowlist):** `port/tests/gsp.h` (new), `port/tests/gfx_gx.c` (new), `tests/test_native_gfx.py` (new), `docs/native/gfx-tests.md` (new).
- **Spec:**
  - **`port/tests/gsp.h`:** for bare test programs: `gsp::Gpu` from `srv:`, `AcquireRight`, `RegisterInterruptRelayQueue` with an event, the shared memory mapped, the linear heap (`svcControlMemory` with the linear flag), GX commands queued into the shared command queue and `TriggerCmdReqQueue`, waiting for a given interrupt, `FlushDataCache`, `SetBufferSwap` (or `WriteHWRegs` of the framebuffer registers), waiting for PDC0.
  - **`gfx_gx.c`:** draws, each step printing a line (`OutputDebugString`): a memory fill of a 240×400 RGBA8 buffer with a 32-bit colour and of a second one with a 24-bit pattern; a display transfer of each to the top screen's display buffer in RGB8 and in RGB565 (one with the 2x downscale, one with the vertical flip); a texture copy with a gap into the bottom screen's buffer; buffer swaps of both screens; then it keeps swapping between two top buffers on PDC0 for 120 VBlanks and prints the VBlank count.
  - **`tests/test_native_gfx.py`:** builds the program (`build_svc_test.build`), runs it on `MH4U_RUNNER` (the reference; skipped without it) and on `MH4N_RUNNER` with `MH4N_GFX_GPU` as `--gpu` (skipped without them), each with `--shots` in milliseconds at the moments the program's steps are stable, and checks: the printed lines; each screenshot equal, pixel for pixel, to the image the test computes from the program's parameters and the formats (Azahar's colour conversions as `gpu_capture/image.h` documents them); and, when both runners ran, the two runs' shots identical.
  - **`gfx-tests.md`:** what each program draws and checks, how to run the test against either runner (the reference runner from the CI artifact, `docs/native/CI.md`).
- **Done when:** `MH4U_RUNNER=<the reference runner of native.yml's artifact> python3 -m pytest tests/test_native_gfx.py` passes (the native part skipped until N2b-12 to N2b-14), and `python3 -m pytest tests/test_native_gfx.py` without runners skips cleanly.
- **Traps:** the GX queue and the interrupt relay queue live in shared memory that the GSP and the program both write: follow 3dbrew's header layout exactly; Azahar processes a queued command at `TriggerCmdReqQueue`. A program without the game's frame counter has only millisecond shots.
- **Out of scope:** drawing with the PICA (N2b-16).

### N2b-16 · A synthetic PICA draw
- **Size / agent:** M · native-worker-high
- **Depends on:** N2b-15 · **Needs:** none
- **Goal:** one of our own programs draws with a PICA command list (a hand-encoded vertex shader, attribute buffers, TEV, depth test and blending), so that the whole draw path, from command list to the displayed screen, has a public-CI oracle with known pixels.
- **Read first:** `port/tests/gsp.h` and `gfx_gx.c` (N2b-15); 3dbrew *GPU/Internal Registers*, *GPU/Commands*, *Shader Instruction Set* (the encodings of `MOV`, `END`, the operand descriptors), *GPU/Shader Setup*; `native/src/gpu/pica/regs.h` (the register numbers N2-04 uses); `tests/test_native_gfx.py`.
- **Files (allowlist):** `port/tests/gfx_draw.c` (new), `port/tests/gsp.h`, `tests/test_native_gfx.py`, `docs/native/gfx-tests.md`.
- **Spec:**
  - `gfx_draw.c` builds in linear memory a command list that sets a 240×400 RGBA8 colour buffer and a D24S8 depth buffer, the viewport, a vertex shader of `MOV o0, v0`, `MOV o1, v1`, `END` (position and colour; the output map, the input count and permutation, the program and swizzle uploads written word by word with the encodings in comments), the attribute buffer format (float positions, unsigned-byte colours), TEV stage 0 as the primary colour with the other stages passing it through, and draws: a red triangle; a green triangle that overlaps it, nearer, with the depth test `LESS`; a blue quad (two triangles) with alpha 128 blended `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` over both. It then display-transfers the buffer to the top screen and swaps.
  - The test computes the expected image: pixels whose centres lie more than one pixel inside a shape's edges get the shape's colour (the blend in integer arithmetic, `(s × a + d × (255 − a)) / 255` rounded as Azahar's software renderer, to within 1/255), and the pixels within one pixel of an edge are left out of the exact check. The whole image is also compared, when both runners run, between them with ROADMAP §6's thresholds (PSNR ≥ 40 dB, no 16×16 block over 8/255).
- **Done when:** `MH4U_RUNNER=<the reference runner> python3 -m pytest tests/test_native_gfx.py -k draw` passes (the native part skipped until N2b-12 to N2b-14).
- **Traps:** PICA floats in the shader's uniforms are f24 in three-word mode; this program uses no uniforms on purpose. The colour buffer is stored rotated and in 8×8 Morton tiles: the display transfer untiles it.
- **Out of scope:** textures, lighting and the other fragment features (the captures check them, N2-20).

### N2b-17 · Image checks across timing models
- **Size / agent:** M · native-worker-high
- **Depends on:** N1-21, N0-10, N0-30, N0-32 · **Needs:** dump, save
- **Goal:** decide, with measurements, which screenshots a native run can be held to: two backends whose clocks differ show different pictures at the same game frame wherever something moves (N0-28 found 19 of 48, 57 of 114 and 140 of 292 screenshots different on G1, G2 and G6, jit against recomp), so the `cross` profile gates the images only at frames where jit and recomp already agree within ROADMAP §6's thresholds; and build that gate into `golden.py`.
- **Read first:** `tools/native/golden.py` and `docs/native/golden.md` (*Who runs, and what is compared*; `images` is info in `cross`, D31); `docs/native/cross-model.md` and `tools/native/cross_model_report.py` (N0-28, N0-30); `native/src/gpu_capture/image.h` and `image.cpp` (`CompareImages`: the thresholds as code); ROADMAP §6; D31, D50.
- **Files (allowlist):** `tools/native/golden.py`, `tests/test_native_golden.py`, `tools/native/imgdiff.py` (new), `tests/test_native_imgdiff.py` (new), `tools/native/cross_model_report.py`, `tests/test_native_cross_model_report.py`, `docs/native/golden.md`, `docs/native/cross-model.md`.
- **Spec:**
  - **`imgdiff.py A.ppm B.ppm`**: PSNR over R, G and B and the 16×16 block rule, exactly as `CompareImages` (blocks at the edges over the pixels they have); prints `psnr=<dB> worst_block=<0-255> at=<x>,<y> pass|fail`; exit 0, 1, or 2 for unreadable or different-size images. A module function the other tools import.
  - **Calibration:** `cross_model_report.py --images` reads a recording and a `--candidate-args "--cpu jit"` run of the same session (the screenshots every 60 frames from frame 300), and lists per frame and screen PSNR, worst block and pass, and proposes the image frames: those where both screens pass. Run it on G1, G2, G6, G3, G4 and G3-perf; the proposal goes to `build/cross-model/image_frames.txt` (`<session> <frame>` per line), and `cross-model.md` gets the tables (frames and numbers only).
  - **The gate** (`golden.py check`, `cross` profile): `images` compares, at each frame `--image-frames FILE` lists for the session (default `config/native/image_frames.txt`, which the orchestrator writes from the proposal), the candidate's top and bottom screenshots with the recording's using `imgdiff`'s rule; a frame listed in the candidate's `shots/gas.txt` is reported `skipped (gas)` and not judged; the other screenshots stay `info`. No listed frame for the session, or no candidate screenshots: `images` skipped (exit 3 under `--require images`, D34). The `same` profile is unchanged (SHA-256).
  - **Shots for a native candidate:** `golden.py` passes `--shots`, `--shots-from` and `--shot-every` to a `--candidate-runner` whose `--help` lists `--shots` (N2b-14), in the recording's frame-keyed form.
  - **Enough frames:** every session needs at least 3 image frames, and at least one in each scene that lasts more than 300 frames. If calibration finds fewer, the WP ends `needs-decision` with the tables (which frames, how far off), and no threshold is changed.
  - The decision (proposed D58) is recorded from the tables: the WP stays `review` until it is `Decided`.
- **Done when:**
  - `python3 -m pytest tests/test_native_imgdiff.py tests/test_native_golden.py tests/test_native_cross_model_report.py` passes (synthetic PPMs: a 16×16 block off by 10/255 fails the block rule with PSNR above 40 dB, one wrong pixel passes both, as in `capture_player_test`; the gate on listed frames only; `gas.txt`; skips and exit statuses; the shot options passed only to a candidate that has them);
  - (with the dump; the save for G3, G4, G3-perf) the calibration runs on the six sessions and `cross-model.md` has the tables, with at least the frames the rule above asks for in each session;
  - D58 is `Decided` in DECISIONS.md.
- **Traps:** the thresholds live in one place (`imgdiff.py`) and equal `image.h`'s constants: the test checks them against that header. Calibrate on the recordings made with the N0-29 runner (N0-09, N0-10).
- **Out of scope:** native runs (N2b-19 on); editing `config/**` (the orchestrator does it).

### N2b-18 · G4 and G3-perf headless
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N1-26, N1-25, N0-10, N0-30 · **Needs:** dump, save
- **Goal:** the two sessions N1 does not run pass headless with the null GPU before the renderer meets them: G4 (a cutscene that uses gas) and G3-perf (G3 with the widened 21:9 top screen and the HUD at the edges), so that N2b-21 and N2b-20 only meet renderer problems.
- **Read first:** N1-26's spec and report; `docs/native/golden.md` (G4, G3-perf); `docs/native/cross-model.md` (N0-30's tables for G4).
- **Files (allowlist):** `native/src/{memory,loader,kernel,runtime,ipc,services,applets,input_script,gpu_null,audio,platform}/**`, `native/src/cpu/*` (the code host, not `interp/`), `native/runner/**`, `native/tests/{memory,loader,cpu,kernel_objects,scheduler,threads,svc,runtime,ipc,srv,fs_romfs,fs_save,apt,cfg,hid,gsp,dsp,other_services,nwm,platform}_test.cpp` (fixes; list each one in the report).
- **Spec:** as N1-26, for G4 and G3-perf, on copies of the save (`golden.py` makes them), with the default `--io-latency azahar`. A difference that depends on time is proposed for `ipc_time_dependent.txt` with its reason. If more than three modules need fixes, stop after listing them, `blocked`.
- **Done when:** `golden.py check G4 --candidate-runner <native runner> --candidate-program build/native-game --candidate-code <the --native library> --require state,checkpoints,ipc,svc,hid` passes in the `cross` profile, and the same for G3-perf.
- **Traps:** never write into the staged save. G3-perf's options (`widescreen`, `wide_render`) come from its recording's `--set`; `golden.py` passes them to the candidate.
- **Out of scope:** rendering.

### N2b-19 · G1 and G2 end to end
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-13, N2b-14, N2b-16, N2b-17, N2b-18 · **Needs:** dump
- **Goal:** with the renderer in the runtime, G1 and G2 pass every check N1-21 passed with the null GPU, and their screenshots pass at the calibrated frames: the first two sessions end to end.
- **Read first:** N1-21's spec and report; `docs/native/renderer-runtime.md`; `docs/native/golden.md` (the image gate, N2b-17); `docs/native/gfx-tests.md`.
- **Files (allowlist):** `native/src/{memory,loader,kernel,runtime,ipc,services,applets,input_script,gpu_null,audio,platform,gpu,shots}/**`, `native/src/cpu/*` (the code host), `native/runner/**`, `native/tests/{memory,loader,cpu,kernel_objects,scheduler,threads,svc,runtime,ipc,srv,fs_romfs,fs_save,apt,cfg,hid,gsp,dsp,other_services,nwm,platform,shots,gpu_runtime,gpu_display,gpu_gx,gpu_pica,gpu_vertex,gpu_shader,gpu_backend,gpu_fbops,gpu_texture,gpu_tev,gpu_sampling,gpu_lighting,gpu_fog,gpu_proctex,gpu_shadow,gpu_surface,gpu_pipeline,gpu_hd,gpu_queue}_test.cpp` (fixes; list each one in the report), `docs/native/e2e.md` (new).
- **Spec:** first our own programs (`tests/test_native_gfx.py` with `MH4N_GFX_GPU=renderer`), then G1, then G2, each with `--candidate-args "--gpu renderer --scale 1"`; fix what fails, smallest fix first, each with a unit test in its module's test file; if more than three modules need fixes, stop after listing them, `blocked`. `e2e.md` gets, per session, the checks run, the image frames judged and skipped, their PSNR and worst block (numbers only), and the run's wall time on lavapipe.
- **Done when:**
  - `MH4U_RUNNER=<the reference runner> MH4N_RUNNER=build/native/bin/mh4u_native_runner MH4N_GFX_GPU=renderer python3 -m pytest tests/test_native_gfx.py` passes (both runners, so the shots are also compared with each other);
  - `golden.py check G1 --candidate-runner <native runner> --candidate-program build/native-game --candidate-code <the --native library> --candidate-args "--gpu renderer --scale 1" --require checkpoints,ipc,svc,hid,state,dsp,images` passes in the `cross` profile (D50 to D52 and D58 apply), and the same for G2 without `dsp`;
  - `--display-check 60` logs 0 differing screens on both runs (N2b-13).
- **Traps:** with the renderer, interrupts arrive when the work completes instead of at once (the null GPU), so waits on P3D and PPF take real time: a scene shift within D50's 4 frames is expected; beyond it, `needs-decision` with the table, never a wider tolerance. Don't touch `golden.py` here: a comparator problem goes back to N2b-17's owner through the orchestrator.
- **Out of scope:** G3, G4 (N2b-20, N2b-21); timing on real hardware (N2b-24).

### N2b-20 · G3 and G3-perf end to end
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-19 · **Needs:** dump, save
- **Goal:** the Arena hunt with the renderer, at the 3DS's top-screen width (G3) and at 21:9 with the HUD at the edges (G3-perf), passes end to end: the busy scene the 120-fps measurement uses is right before it is timed, and the widened top screen is checked against the Azahar build's.
- **Read first:** `docs/native/e2e.md` (N2b-19); N1-26's report; N2b-18's report; `docs/research/widescreen.md`.
- **Files (allowlist):** `native/src/{memory,loader,kernel,runtime,ipc,services,applets,input_script,gpu_null,audio,platform,gpu,shots}/**`, `native/src/cpu/*` (the code host), `native/runner/**`, `native/tests/{memory,loader,cpu,kernel_objects,scheduler,threads,svc,runtime,ipc,srv,fs_romfs,fs_save,apt,cfg,hid,gsp,dsp,other_services,nwm,platform,shots,gpu_runtime,gpu_display,gpu_gx,gpu_pica,gpu_vertex,gpu_shader,gpu_backend,gpu_fbops,gpu_texture,gpu_tev,gpu_sampling,gpu_lighting,gpu_fog,gpu_proctex,gpu_shadow,gpu_surface,gpu_pipeline,gpu_hd,gpu_queue}_test.cpp` (fixes; list each one in the report), `docs/native/e2e.md`.
- **Spec:** as N2b-19, for G3 and G3-perf on copies of the save. G3 at `--set 0:fps=120` too (as N1-26): the quest loads and the sequence of scenes equals the 60-fps native run's (`state_diff.py --order-only`).
- **Done when:** `golden.py check G3 … --candidate-args "--gpu renderer --scale 1" --require state,checkpoints,ipc,svc,hid,images` passes in the `cross` profile, and the same for G3-perf; the 120-fps run's scene order equals the 60-fps run's; `--display-check 60` logs 0 differing screens on G3-perf.
- **Traps:** pressing B right after the quest is accepted leaves it unposted (`golden.md`); never write into the staged save.
- **Out of scope:** G4 (N2b-21).

### N2b-21 · G4 end to end
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-20 · **Needs:** dump, save
- **Goal:** the gas cutscene with the renderer passes end to end, its gas frames reported and left out of the image gate (the reference draws no gas), so that the last of G1–G4 passes.
- **Read first:** `docs/native/e2e.md`; `docs/research/gas.md` (N2-10, N2-20: the gas frames); `docs/research/screen-filters.md` (depth of field and gas in cutscenes).
- **Files (allowlist):** `native/src/{memory,loader,kernel,runtime,ipc,services,applets,input_script,gpu_null,audio,platform,gpu,shots}/**`, `native/src/cpu/*` (the code host), `native/runner/**`, `native/tests/{memory,loader,cpu,kernel_objects,scheduler,threads,svc,runtime,ipc,srv,fs_romfs,fs_save,apt,cfg,hid,gsp,dsp,other_services,nwm,platform,shots,gpu_runtime,gpu_display,gpu_gx,gpu_pica,gpu_vertex,gpu_shader,gpu_backend,gpu_fbops,gpu_texture,gpu_tev,gpu_sampling,gpu_lighting,gpu_fog,gpu_proctex,gpu_shadow,gpu_surface,gpu_pipeline,gpu_hd,gpu_queue}_test.cpp` (fixes; list each one in the report), `docs/native/e2e.md`.
- **Spec:** as N2b-19, for G4 on a copy of the save. `e2e.md` lists G4's frames whose shots `gas.txt` names, and the image frames judged.
- **Done when:** `golden.py check G4 … --candidate-args "--gpu renderer --scale 1" --require state,checkpoints,ipc,svc,hid,images` passes in the `cross` profile; at least one image frame of G4 was judged (not every frame skipped for gas); `--display-check 60` logs 0 differing screens.
- **Traps:** a gas frame that differs from the reference is expected and not a failure; one the reference also draws without gas, and that differs, is.
- **Out of scope:** checking gas itself against a real 3DS (N2o-01, [`LATER.md`](LATER.md)).

### N2b-22 · The hooks' visual features in `mh4u-native`
- **Size / agent:** M · native-worker-high (build-heavy)
- **Depends on:** N2b-11, N2b-13 · **Needs:** ci, pc
- **Goal:** what the port's hooks change on the screen works in `mh4u-native` as in `mh4u`: the widened top screen and the aspect-ratio option, the HUD at the edges, the screen filters, and the touch overlay with its crop, its quest-HUD views, its opacity and the quest-map key; checked automatically with the hooks' values and by the maintainer against the Azahar build.
- **Read first:** `native/src/platform/platform_native.h` (what the hooks set: rows, width, overlay, aspect ratio, layout refreshes); `port/src/hooks.cpp` (where it calls `SetTopScreenWidth`, `SetScreenOverlay`, `RefreshScreenLayout`, `SetOption("aspect_ratio")`, and `ToggleQuestMap`); `port/app/window.cpp` (the touch-screen keys); `docs/research/widescreen.md`, `second-screen.md`, `screen-filters.md`; `native/src/screen_layout/`.
- **Files (allowlist):** `native/app/shell/**`, `native/app/runtime/**`, `native/src/screen_layout/**`, `native/app/tests/app_visual_test.cpp`, `tests/test_native_app.py`, `docs/native/app.md`, `docs/native/visual-check.md` (new).
- **Spec:**
  - **The layout source** (N2b-09's) reads, each frame, `NativePlatform::Find(rt)`'s `TopScreenWidth`, `Overlay` and the `aspect_ratio` option, besides the layout setting and the window size; a change of `LayoutRefreshes` makes the next frame use the new values (the hooks' `RefreshScreenLayout`).
  - **Touch:** the shell's touch tracker uses the same layout, so a click on a quest-HUD view or a cropped overlay reaches the part of the bottom screen it shows.
  - **The quest-map key** (`touch_screen_key`, F12 | M): posts the hooks' `ToggleQuestMap` to the scheduler (as `window.cpp` calls it for `mh4u`).
  - **`visual-check.md`:** the maintainer's procedure, as exact steps on the reference PC with the Windows artifact and the game: `mh4u` and `mh4u-native` side by side with the same `mh4u.ini`, at 3440×1440: `widescreen = 21:9` with `wide_render = 1` and `0`, `hud_edges = 1`, each screen filter off and on (`bloom`, `depth_of_field`, `motion_blur`, `radial_blur`, `haze`), the touch overlay in the village and in a quest (its corner, opacity, the map alone and the whole screen with the key, a click on the map), the four layouts; for each, what to look at and what counts as the same.
- **Done when:**
  - `ctest --test-dir build/native-app -R app_visual` passes: with the platform's values set as the hooks set them (a `NativePlatform` made in the test), the composed frame and the touch mapping follow the width, the aspect ratio, the overlay's corner, crop and two views, and a layout refresh;
  - *(CI)* `native-app.yml` is green;
  - the maintainer's procedure of `visual-check.md` passes on the reference PC (his notes go into `e2e.md` through the orchestrator).
- **Traps:** the hooks call the platform on the scheduler thread and the present thread reads it: use the platform's getters (they lock), never its fields.
- **Out of scope:** the other 25 hooks (N3-09).

### N2b-23 · Frame pacing at 120 fps on a 240 Hz display
- **Size / agent:** M · native-worker-high (build-heavy), then the maintainer
- **Depends on:** N2b-22, N2b-06 · **Needs:** pc
- **Goal:** at 120 fps on a 240 Hz display every game frame is shown for exactly two refreshes (ROADMAP §7): the VBlank that drives the game is kept at half the display's measured refresh, and its phase away from the refresh boundary, through `Runtime::SetRefreshRate`; and the maintainer chooses between that and the plain host clock with measurements (proposed D57).
- **Read first:** ROADMAP §3 (principle 3: "the runtime generates VBlank at the presentation rate") and §7; D15, D44 (who may call `SetRefreshRate`, from which thread, and the log line of each change); `docs/native/presentation.md` (N2b-06's present log); `native/src/platform/platform_native.h` (`RequestedRefreshRate`).
- **Files (allowlist):** `native/src/pacing/**` (new), `native/src/swapchain/**`, `native/app/runtime/**`, `native/tests/pacing_test.cpp`, `docs/native/presentation.md`, `docs/native/app.md`.
- **Spec:**
  - **`pacing::Pacer`** (pure logic, fed by the present thread): inputs are the display's refresh (SDL3's display mode, refined by the present timestamps), the present log's times, and the hooks' requested rate. Modes (`--pacing display|clock`, the app's option; the default is D57's, `clock` until it is decided):
    - `clock`: never calls `SetRefreshRate` (the GSP's VBlank is the hooks' rate on the host clock).
    - `display`: when vsync is on and the display's rate is within 0.5% of an integer multiple n of the requested rate, the VBlank rate is the display's rate over n; the phase of the game's VBlank against the refresh is measured from the present times, and when it comes within 15% of a refresh period of a boundary, the rate is nudged by 0.1% until it is back in the middle. At most one `SetRefreshRate` call per 10 s (each logs `gsp: vblank period`); a change of the requested rate restarts the lock.
  - **Glue:** `NativeRuntime` owns the pacer and calls `Runtime::SetRefreshRate` from the present thread (D44: posted, newest wins).
  - **The maintainer's procedure** (a section of `presentation.md`, *Pacing on the reference PC*): on the reference PC at 3440×1440, 240 Hz, full screen, vsync on, `fps = 120`, G3's fight for 5 minutes in each mode: the present log's count of frames shown for other than two refreshes per minute, and the `gsp: vblank period` lines.
- **Done when:**
  - ctest passes (`pacing_test`): on synthetic present timestamps of a 239.96 Hz display with jitter, `display` mode settles at 119.98 Hz, keeps the phase out of the boundary bands over 10 simulated minutes with at most one call per 10 s, and restarts on a new requested rate; a 144 Hz display at 120 fps leaves the rate alone (not a multiple); `clock` mode never calls;
  - the maintainer has run the procedure *Pacing on the reference PC* of `presentation.md` in both modes, and the numbers are in `docs/research/perf-native-n2b.md` (the orchestrator writes it);
  - D57 is `Decided` (the WP stays `review` until then).
- **Traps:** each call changes the game's VBlank period by a fraction of a percent; the game's logic runs on delta time (`docs/research/system-services.md`, *Frame pacing*), so its speed does not change, but never call it per frame. Headless runs and goldens have no pacer.
- **Out of scope:** 180 and 240 fps (N6); variable refresh.

### N2b-24 · Exit: G1–G4 end to end and 120 fps in G3
- **Size / agent:** S · native-worker, then the maintainer
- **Depends on:** N2b-21, N2b-23, N2-19, N1-23 · **Needs:** dump, save, pc
- **Goal:** prove N2b's exit on the final batch: G1–G4 (and G3-perf) pass end to end with images, and `mh4u-native` runs G3's fight at 120 fps on the reference PC within ROADMAP §7's budget.
- **Read first:** ROADMAP §7 and D19; `docs/native/perf-procedure.md` (N0-15, N1-23 and N2-19 sections); `tools/native/perf_report.py`; `docs/native/e2e.md`; `docs/native/presentation.md` (present log) and `renderer-runtime.md` (`--gpu-log`).
- **Files (allowlist):** `docs/native/perf-procedure.md`, `tools/native/perf_report.py`, `tests/test_native_perf_report.py`, `docs/native/e2e.md`.
- **Spec:**
  - **`perf_report.py`** reads, besides its current inputs, N1-23's `--perf-log`, N2b-12's `--gpu-log` and N2b-06's present log of one run, joins them by game frame, keeps the frames from `--from-frame N` (the hunt's checkpoint frame of G3-perf in `golden.md`) and drops the first 10 s after it (warm-up), and prints p50, p95 and p99 of: the game thread's time per frame, the translation time, the GPU time, the presented-frame interval; the number of presented intervals over 50 ms; the largest VRAM in use; and one verdict line per budget: game thread p99 ≤ 4 ms, translation p99 ≤ 3 ms, GPU p99 ≤ 6 ms (D19), the presented interval, counted in refreshes of the measured display rate (from `displayed_us` when the log has it, else `presented_us`, rounded to the nearest refresh), at most two in 99% of frames (ROADMAP §7's "p99 ≤ 8.3 ms" is 1/120 s, two refreshes at 240 Hz), 0 intervals over 50 ms, VRAM within N2-18's budget.
  - **The procedure** (a section of `perf-procedure.md`, *N2b: mh4u-native at 120 fps*), exact steps: the Windows artifact of `native-app.yml` from the final batch; a copy of G3's user folder with the save; `mh4u-native <game folder> --code <the --native library> --user <the copy> --fullscreen --input-file tools/native/golden/G3-perf.txt --set 0:fps=120 --hd <the archive> --perf-log … --gpu-log … --present-log …` at 3440×1440 and 240 Hz (the monitor's real mode is written down: M-2), pacing as D57 decided, a warm pipeline cache (one run first), three runs; then `perf_report.py` on each.
- **Done when:**
  - `python3 -m pytest tests/test_native_perf_report.py` passes (synthetic CSVs of the three logs: the join, the warm-up cut, each verdict line both ways);
  - (with the dump; the save for G3, G4, G3-perf) on the final batch, the checks of N2b-19, N2b-20 and N2b-21 pass again, and `e2e.md` records them;
  - (the maintainer, `pc`) the procedure's three runs pass every verdict line; the numbers and the verdict are in `docs/research/perf-native-n2b.md` (the orchestrator writes it). A budget that is missed ends the WP `blocked` with the table: no threshold is changed, and the orchestrator opens the work (optimisation, or N1's per-core parallelism moved forward, ROADMAP §5) or a decision.
- **Traps:** a run that drops frames because of a cold pipeline cache or HD textures still streaming is a failure of N2-16 or N2-18's promise (principle 5), not noise: report it as such.
- **Out of scope:** the Mac (N2c); four players (N5).
