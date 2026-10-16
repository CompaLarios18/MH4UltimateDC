# port/

The PC port: [Azahar](https://github.com/azahar-emu/azahar)'s 3DS core (kernel, services, GPU, audio) with the game's recompiled code as the CPU. Architecture and plan: [`docs/PORT_ARCHITECTURE.md`](../docs/PORT_ARCHITECTURE.md).

| Path | What |
|---|---|
| `src/arm_recomp.*` | CPU backend (`Core::ARM_Interface`) that runs recompiled code: one host fiber per guest thread, system calls into Azahar's kernel, Azahar's page table as guest memory, cycle accounting through Azahar's timers, and a check that the loaded program is the one that was recompiled. Code that wasn't recompiled (a missed entry point, code made at run time) runs in Azahar's interpreter until it returns, and its address is logged for the next recompilation |
| `src/fiber.*` | Host fibers (ucontext on Linux/macOS, Win32 fibers on Windows) |
| `src/frontend.*` | `Port::PrepareBackendFor()`: called by a frontend before loading a game; installs the recompiled backend when there is a code library for that game and the code Azahar is about to load (the installed update's, with the code mods in its mods folder) is the code the library was recompiled from; otherwise Azahar's own CPU emulation runs the game at its usual speed (the interpreter the recompiled code falls back to would be far too slow). `Port::BackendNotice()` then tells the player why MH4U runs without the port, or that the library lacks some hook points, and how to build it; Azahar's Qt frontend calls both when built with this directory and shows the notice in a message box (see `azahar/azahar.patch`); its *Tools → MH4U Port Options...* opens `Port::OptionsPath()`, made from `Port::OptionsTemplate()` when missing, and its hotkey *MH4U Quest Map On/Off* (F12) calls `Port::ToggleQuestMap()` |
| `src/runner.cpp` | `mh4u_runner`: boots a 3DS executable with no window, with the recompiled code (`--cpu recomp --code LIB`) or Azahar's own JIT/interpreter; can install a CIA first, play an input script and save screenshots; for tests and for comparing backends |
| `src/schedule.cpp`, `src/schedule.h` | the runner's script parsers (times, `--input` events, scheduled options); standard library only, shared with the native runner |
| `src/hooks.cpp` | The port's hooks on game functions (hook points from `config/hooks.txt`) and its options (`mh4u.ini`): frame rate, widescreen, HUD, touch screen, fixes, traces |
| `app/` | The port's own app, `mh4u` (SDL2): opens the game directly in a window of its own on Azahar's core, with the recompiled code and the hooks, Vulkan/OpenGL/software renderers, keyboard and controller at once, the touch overlay, rooms from its settings (`mh4u-app.ini`), an in-game menu and the 3DS software keyboard drawn with Dear ImGui (`app/ui/`, vendored in `app/third_party/imgui/`); built with this directory when Azahar has SDL2 (`MH4U_APP`) |
| `src/netsync.*` | Net sync: the port's consoles in a network room keep their emulated clocks together, so that one whose PC stalls isn't dropped |
| `windows/` | For the Windows package: `Build code library.bat` and `build-code-library.ps1`, which build the game's code library from the player's update with the tools the package carries in `mh4u-tools`, then install the update in Azahar with `mh4u_runner --install` |
| `unix/` | For the Linux and macOS packages: `build-code-library.sh` (`Build code library.command` in the Mac package), the same build with a file dialog (zenity, kdialog or the Mac's own) |
| `tests/svc_test.c` | Freestanding 3DS test program (threads, events, mutexes, atomics, VFP short vectors, Thumb) |
| `azahar/` | The Azahar commit this builds against, and a small patch: a hook to replace the CPU backend, a hook to build this directory inside Azahar's tree, hooks for the port's options (refresh rate, system-call observer, the top screen's width in every screen layout, the custom layout as a touch-screen overlay that follows the window's size and can show just a part of the touch screen), a switch for the headless runner to skip drawing (vertex shaders included), a fix to Azahar's local-wireless emulation (two consoles joining a network at the same moment got the same node id, and the game dropped one), a hook at the start of each run of the emulation loop (for net sync), a frontend's own drawing over the Vulkan renderer's screens (the app's menu; none unless one is registered), *Create Room* offering an unlisted room when there is no lobby server (Azahar has none of its own, and a public room then fails to start), a room's host rights only for the host's checked name or the host's own client on the same machine (a nickname alone is anyone's to type, and a lobby shows a dedicated room's name as its owner), LayeredFS reading `romfs_ext/`'s sub-folders (MH4U's archives are in `data/` and `eng/data/`), and two fixes to Azahar's software renderer that the game needs: a data race on physical-memory lookups between the renderer's worker threads and the emulation thread (it sent texture reads gigabytes off and crashed both CPU backends on the game's first textured frame), and textures outside of memory read as black instead of crashing |

The runner contains no game code. The game's recompiled code is a separate shared library, generated and compiled on the user's machine from their own dump and loaded at run time.

## Building (Linux; Windows, Linux and macOS in CI)

The repository's workflows build the port's Azahar for Windows (MXE, `.github/workflows/windows.yml`), Linux (an AppImage, `.github/workflows/linux.yml`) and macOS (an app for Apple silicon, `.github/workflows/macos.yml`) on every change to `port/`, and package the port's own app (`app/`, built with Azahar when SDL2 is on) with it: `mh4u.exe` next to `azahar.exe`, a second AppImage `mh4u.AppImage`, and `MH4U.app` (with MoltenVK in its `Contents/Frameworks`). To build it yourself on Linux:

```bash
# Azahar at the pinned commit, with the patch
git clone https://github.com/azahar-emu/azahar.git && cd azahar
git checkout $(cat ../MH4UltimateDC/port/azahar/COMMIT)
git submodule update --init --recursive --depth 1
git apply ../MH4UltimateDC/port/azahar/azahar.patch

# Configure and build the runner (no Qt; software renderer)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DENABLE_QT=OFF -DENABLE_TESTS=OFF -DENABLE_ROOM=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_SCRIPTING=OFF \
  -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF -DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF \
  -DENABLE_SDL2=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF -DMH4U_PORT_DIR=$PWD/../MH4UltimateDC/port
cmake --build build --target mh4u_runner
cd ../MH4UltimateDC

# Check the backend against Azahar's JIT with the test program (no game data needed)
MH4U_RUNNER=../azahar/build/bin/Release/mh4u_runner python3 -m pytest tests/test_port.py
```

## Running the game's code

```bash
# 1. Your dump to a code library in one step (extract, check version, recompile, compile);
#    --install puts it where the port's Azahar build looks for it
python3 tools/build_game.py --dump "MH4U v1.3.0 update.cia" --install ~/.local/share/azahar-emu

#    or step by step (-O1 takes a while; -O0 is quick)
python3 tools/recompile.py --exheader baserom/update_v130/exheader.bin --code baserom/update_v130/code.bin -o build/recomp
(cd tools && python3 -m recomp.build ../build/recomp --code-library --opt O1)    # build/recomp/mh4u_code.so

# 2. Without the RomFS the game starts up and stops when it first opens its files, which
#    is enough to compare backends:
python3 tools/make_cxi.py --exheader baserom/update_v130/exheader.bin --exefs baserom/update_v130/exefs.bin -o build/mh4u_norom.cxi
../azahar/build/bin/Release/mh4u_runner build/mh4u_norom.cxi --code build/recomp/mh4u_code.so --cpu recomp --log "*:Info"
../azahar/build/bin/Release/mh4u_runner build/mh4u_norom.cxi --cpu jit --log "*:Info"
```

With the whole game (your decrypted cartridge dump, plus the update, which Azahar takes
from its virtual SD card), the runner can also drive it without a window:

```bash
../azahar/build/bin/Release/mh4u_runner game.cxi --code build/recomp/mh4u_code.so \
    --user build/run/user --install "MH4U v1.3.0 update.cia" --ms 60000 \
    --shots build/run/shots --shot-every 1000 --input "20000:A,25000:START:200,30000:TOUCH:160:120"
```

`--new-3ds on|off` (default `off`) sets Azahar's New 3DS mode before the game loads; the
reference runs are Old 3DS runs, because the mode changes the cores, the FCRAM size and what
the cfg, ptm and APT services answer.

`--state-trace FILE [--observable FILE]` writes the game's observable state: after the header
(`# statetrace v1 <date> game=<title id> cpu=<cpu> new3ds=<on|off>`), a line
`f=<frame> t=<ticks> <name>=<0x value | ->` whenever the game's frame counter (`0x010571d4`)
changes, with the values listed in `config/native/observable.txt` (the default, relative to the
current directory; an address or a pointer chain such as `[[0x0104dea0]+0x30]`, a size, a name
and `eq` or `ignore`; `-` is a null pointer in the chain or unreadable memory). The values are
read page by page after checking that the pages are mapped, so the trace adds no unmapped
accesses and writes nothing. `tools/native/state_diff.py reference candidate [--tolerance N]
[--order-only]` aligns two traces on their scene changes and exits 0 if each happens within N
frames (default 2) of its counterpart with equal `eq` values (`--order-only`: only the order of
the scenes). `--count-unmapped FILE` counts the accesses to unmapped memory that Azahar logs as
`unmapped ...` (scalar reads and writes, and block reads and writes by pages) and writes
`count=<n> addr=<hex> size=<n> rw=<read|write> lr=<hex>` lines, the most frequent first, at the
end of the run; the LR is the CPU's `r14` at the access (with `--cpu recomp`, the recompiled
code's context, which a leaf function does not update).

`--trace-svc FILE` writes every system call with its thread, caller and arguments; the
same run on `--cpu jit` gives the reference, and the first difference per thread points at
the recompiled function that went wrong (`tools/compare_svc_traces.py jit.txt recomp.txt`).
A line is `<thread> <svc> pc=<hex> lr=<hex> <r0> <r1> <r2> <r3> t=<ticks> in4=<r4>,<r5>,<r6>
out=<r0>,<r1>,<r2>,<r3>` (8 uppercase hex digits, no `0x`): `in4=` is r4 to r6 as the call was
made (timeouts and permissions live there for some calls), `out=` is r0 to r3 when the call
completed for its thread, which for a call that blocks it (a wait, a sleep, a request with a
deferred reply) is when the thread next ran, so its line can come after other threads' later
calls; within a thread the lines stay in the order of the calls. `out=-` marks a call that was
still blocked when the run ended. The first line is a header
(`# svctrace v1 <date> game=<title id> cpu=<jit|recomp|interp> new3ds=<on|off>`) and every
thread gets a line when it is created
(`# thread <id> parent=<id> entry=<hex> arg=<hex> stack=<hex> prio=<n> core=<requested>/<resolved>`;
the main thread has `parent=0`). To compare two runtimes whose thread ids and handle values
differ, add `--normalize` (threads become `main` or `<entry>#<k>`, handles `<kind>#<k>`, times
are dropped) and `--outputs` (compare the results too); `tools/native/tracefmt.py` describes
the rules. Every time in a script is emulated milliseconds or a game frame: `f600` is the first moment the
frame counter (the u32 at `0x010571d4`) reads 600 or more, so a script means the same on every CPU backend and
runtime (`--input "f600:START,f900:A:f3"`; a hold may be `f<N>` frames, default 100 ms; `--dump`, `--replace`,
`--set`, `--freeze`, `--call-counts`, `--shots-from` and `--ms` take the same). `--frames N` (or `--ms f<N>`) stops the
run at frame N, exit status 0, with no time limit unless `--ms` is also given. `--input-file PATH` reads
one event per line, `#` comments allowed, and `--print-schedule` prints the parsed schedule and exits (no game
needed). Keep the default of reading files in place: with `--async-io` frame numbers vary between runs.
`--input` presses buttons, touches the bottom screen (pixels) or moves the circle pad
(-1..1) at emulated times; `--shots` saves both screens as PPM images, and
`--draw-window 100` draws only the 100 ms before each screenshot, which makes long runs
many times faster (the software renderer, and the vertex shaders the GPU runs on the CPU, take
nearly all the time), and `--shots-from MS`
starts the screenshots late, so a close look at one moment doesn't image the whole run;
`--dump MS:ADDR:SIZE` writes guest memory at an emulated time, to diff the game's state
between two runs that differ in one input, and `--replace MS:FROM:TO` changes every copy of a
byte string in the game's writable memory at an emulated time; with a code library built with
`-D RC_CALL_COUNTS` (`python3 -m recomp.build build/recomp --code-library -D RC_CALL_COUNTS
-o mh4u_code_counts.so`), `--call-counts MS` writes how often each function has run, so
the difference between two snapshots, or two runs, shows the code an input ran; `--cpu-clock 200` is
Azahar's CPU Clock Speed setting. The runner reads files in place, so a run repeats
exactly: with Azahar's default (`--async-io`) a read finishes on a host thread and wakes
the game at an emulated time that depends on how fast the host was, which moved loading
screens by seconds between identical runs and let time-based input scripts land on other
screens. It also sends sound nowhere, so it needs no sound card. `--room-host PORT` makes
a network room inside the runner (Azahar's multiplayer) and joins it, `--room-join HOST:PORT`
joins one, so two runners play the game's local wireless modes together (a copy of a user
folder needs `--new-console-id`: a room turns away a console it already has). Give runners on
one machine `--lockstep FILE:COUNT:INDEX` as well: it holds each runner's emulated clock within
a few milliseconds of the others'. The emulated clock stands still while the GPU works, so a
runner drawing a screenshot falls silent for the others for the second or so that takes, and
MH4U's local play gives up on a silent player within about a second (`--stalls MS` prints the
steps that took longer than MS of host time). In lockstep the other runners wait for it, and
two runners stay together in a Gathering Hall. Between machines the port's net sync does the same
over the room (`src/netsync.cpp`, on by default): `--freeze MS:HOST_MS` stops a runner for a while, as a
PC that stalls would, to try it. `--set MS:KEY=VALUE` changes one of the port's options at an
emulated time (those the hooks read as the game runs: `fps`, `refresh_rate`, `dither`, `kick`,
`target`, `quest_overlay`, `hud_edges`, ...), e.g. one player at 120 fps once a session's input
scripts, written for 30 fps menus, have brought everyone into a quest. `--overlay-layout SPEC
WxH[:TOP]` prints the screens' rectangles the `touch_overlay` option gives a window; with a
`quest_overlay` value and the map's panel slot after them, also the part of the touch screen a
quest shows and where a click on it touches (through Azahar's own touch code).
`--quest-overlay-frames FPS KIND:SECONDS,...` runs a made-up quest's frames (panels up with the
map in a slot, panels gone for a change of area, the quest menu, ...) through the choice of what
that corner shows and prints each change, at any frame rate. The port's hooks
take options from the environment: `MH4U_FPS=60` raises the engine's frame-rate cap,
`MH4U_TRACE_FRAMES=60` logs the engine's frame timing every 60 frames, and
`MH4U_QUEST_OVERLAY_TEST=0` makes the touch overlay show the map's part of the touch screen (slot
0) all the time, to look at the drawing outside a quest (checked with the Qt frontend and OpenGL
under Xvfb).

Everything under `build/` contains game code: it never goes into the repository.

## With Azahar's normal interface

Built with `-DENABLE_QT=ON` (and the same `-DMH4U_PORT_DIR`), Azahar's Qt frontend runs a game on recompiled code when it finds a code library for it: either the path in the `MH4U_CODE` environment variable, or `recomp/<program id>.so` (`.dll` on Windows) in Azahar's user directory, e.g. `recomp/0004000000126300.so` for MH4U. Everything else (settings, controls, multiplayer rooms, texture packs, screen layouts) is Azahar's. If the loaded code isn't the code that was recompiled (another version), the backend logs it and interprets everything instead.

## Licence

GPL-3.0-or-later, like the rest of the repository ([LICENSE](../LICENSE)) and like Azahar, whose core the executables contain. The Windows workflow publishes builds only with that licence and the matching source (this repository at the built commit, Azahar at `azahar/COMMIT` plus `azahar/azahar.patch`).
