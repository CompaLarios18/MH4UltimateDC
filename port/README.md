# port/

The PC port: [Azahar](https://github.com/azahar-emu/azahar)'s 3DS core (kernel, services, GPU, audio) with the game's recompiled code as the CPU. Architecture and plan: [`docs/PORT_ARCHITECTURE.md`](../docs/PORT_ARCHITECTURE.md).

| Path | What |
|---|---|
| `src/arm_recomp.*` | CPU backend (`Core::ARM_Interface`) that runs recompiled code: one host fiber per guest thread, system calls into Azahar's kernel, Azahar's page table as guest memory, cycle accounting through Azahar's timers, and a check that the loaded program is the one that was recompiled. Code that wasn't recompiled (a missed entry point, code made at run time) runs in Azahar's interpreter until it returns, and its address is logged for the next recompilation |
| `src/fiber.*` | Host fibers (ucontext on Linux/macOS, Win32 fibers on Windows) |
| `src/frontend.*` | `Port::PrepareBackendFor()`: called by a frontend before loading a game; installs the recompiled backend when there is a code library for that game and the code Azahar is about to load (the installed update's, with the code mods in its mods folder) is the code the library was recompiled from; otherwise Azahar's own CPU emulation runs the game at its usual speed (the interpreter the recompiled code falls back to would be far too slow). `Port::BackendNotice()` then tells the player why MH4U runs without the port, or that the library lacks some hook points, and how to build it; Azahar's Qt frontend calls both when built with this directory and shows the notice in a message box (see `azahar/azahar.patch`); its *Tools → MH4U Port Options...* opens `Port::OptionsPath()`, made from `Port::OptionsTemplate()` when missing, and its hotkey *MH4U Quest Map On/Off* (F12) calls `Port::ToggleQuestMap()` |
| `src/runner.cpp` | `mh4u_runner`: boots a 3DS executable with no window, with the recompiled code (`--cpu recomp --code LIB`) or Azahar's own JIT/interpreter; can install a CIA first, play an input script and save screenshots; for tests and for comparing backends |
| `src/schedule.cpp`, `src/schedule.h` | the runner's script parsers (times, `--input` events, scheduled options); standard library only, shared with the native runner |
| `src/hooks.cpp` | The port's hooks on game functions (hook points from `config/hooks.txt`) and its options (`mh4u.ini`): frame rate, widescreen, HUD, touch screen, fixes, traces. It includes nothing of Azahar: everything it needs from the emulator is behind `Port::Platform` (below), and `port/CMakeLists.txt` compiles it in the paged memory mode (`RC_PAGED`) for the Azahar build, while the native runtime will compile it flat (N1-20; nothing under `native/` compiles it yet, and `tests/test_port_hooks_platform.py` checks that it can be) |
| `src/platform.h`, `src/platform_azahar.cpp` | `Port::Platform`, the pure interface the hooks use (refresh rate, top-screen rows and width, game seconds, whether the emulation runs, the screen layout and overlay as a plain struct, string options, the user directory, log, net sync and the end-skip vote), and the Azahar implementation that holds every Azahar call the hooks used to make |
| `app/` | The port's own app, `mh4u` (SDL2): opens the game directly in a window of its own on Azahar's core, with the recompiled code and the hooks, Vulkan/OpenGL/software renderers, keyboard and controller at once, the touch overlay, rooms from its settings (`mh4u-app.ini`), an in-game menu and the 3DS software keyboard drawn with Dear ImGui (`app/ui/`, vendored in `app/third_party/imgui/`); built with this directory when Azahar has SDL2 (`MH4U_APP`) |
| `src/netsync.*` | Net sync: the port's consoles in a network room keep their emulated clocks together, so that one whose PC stalls isn't dropped |
| `windows/` | For the Windows package: `Build code library.bat` and `build-code-library.ps1`, which build the game's code library from the player's update with the tools the package carries in `mh4u-tools`, then install the update in Azahar with `mh4u_runner --install` |
| `unix/` | For the Linux and macOS packages: `build-code-library.sh` (`Build code library.command` in the Mac package), the same build with a file dialog (zenity, kdialog or the Mac's own) |
| `tests/` | The freestanding 3DS test programs (no game code): `conf_*.c`, the kernel conformance programs, each with Azahar's expected output in `tests/expected/`; `svc.h`, the system-call wrappers and helpers they all share (`io_test.c` and `bench.c` include it too, so a change there must keep every program compiling); `build_svc_test.py`, which builds any program by name (`--program NAME`, `--list`); `io_test.c` (HID input and a known picture in the top framebuffer, for the runner's `--input` and `--shots`); `bench.c`; `svc_test.c` (threads, events, mutexes, atomics, VFP short vectors, Thumb); `codehost_test.c` (the native code host's test program, built by `native/src/cpu/CMakeLists.txt`) |
| `azahar/` | The Azahar commit this builds against, and a small patch: a hook to replace the CPU backend, a hook to build this directory inside Azahar's tree, hooks for the port's options (refresh rate, system-call observer, the top screen's width in every screen layout, the custom layout as a touch-screen overlay that follows the window's size and can show just a part of the touch screen), a switch for the headless runner to skip drawing (vertex shaders included), a fix to Azahar's local-wireless emulation (two consoles joining a network at the same moment got the same node id, and the game dropped one), a hook at the start of each run of the emulation loop (for net sync), an observer of every event CoreTiming runs (the runner's `--trace-timing`; none unless one is set), a switch that fixes the console's Wi-Fi MAC address (only the runner's default mode sets it), a frontend's own drawing over the Vulkan renderer's screens (the app's menu; none unless one is registered), *Create Room* offering an unlisted room when there is no lobby server (Azahar has none of its own, and a public room then fails to start), a room's host rights only for the host's checked name or the host's own client on the same machine (a nickname alone is anyone's to type, and a lobby shows a dedicated room's name as its owner), LayeredFS reading `romfs_ext/`'s sub-folders (MH4U's archives are in `data/` and `eng/data/`), and two fixes to Azahar's software renderer that the game needs: a data race on physical-memory lookups between the renderer's worker threads and the emulation thread (it sent texture reads gigabytes off and crashed both CPU backends on the game's first textured frame), and textures outside of memory read as black instead of crashing |

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

# The same runner also runs the kernel conformance programs and the port's runner tests
MH4U_RUNNER=../azahar/build/bin/Release/mh4u_runner python3 -m pytest tests/test_conformance.py tests/test_port.py
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
the rules.

`--trace-ipc FILE [--trace-ipc-full]` writes every IPC request with its reply, using Azahar's own
IPC recorder (`Kernel().GetIPCRecorder()`; no patch is needed). A line, written when the reply
is recorded (so a service that answers later comes after the lines of requests that other
threads sent in between), is `<thread> seq=<n> session=<name> cmd=<header> in=<words> desc=<...>
out=<words> outdesc=<...> t=<ticks>`: `seq` numbers the requests in the order they were sent,
`session` is the service's port name (`fs:USER`) or, for a session a reply handed out,
`<service>/<kind>#<k>` (`fs:USER/file#3`; never the file's name), `in` is every word of the
command as the client wrote it and `out` every word of the reply as the client reads it (its
second word is the result), `desc` and `outdesc` list `<kind>:<index>:<size>:<token>` for the
static buffers (CRC-32 of the bytes), the mapped buffers (`mapped-r|w|rw`; the CRC-32 of those
the service may read in `desc`, and of those it may write, after the reply, in `outdesc`) and
the handles (`copy`/`move`, with the names of the handles). The header and the `# thread` lines
are those of `--trace-svc` (`# ipctrace v1 ...`); the words are `0x` and 8 lowercase digits, not
--trace-svc's bare uppercase hex. `--trace-ipc-full` adds
`data=<index>:<hex>` with the bytes of every input buffer, for the replay harness (N1-11). A
request that never got a reply ends with `out=-`. The file holds game data (file names, save
bytes): keep it in `build/` and never paste lines from it. `tools/compare_ipc_traces.py ref
cand [--strict] [--ignore FILE]... [--prefix] [--context N]` compares two traces thread by thread
after the normalisation of `tools/native/tracefmt.py` (which also names the handles of an
SVC trace that came in IPC replies: `tracefmt.py svc.txt --ipc ipc.txt`), and prints only
session names, headers, result codes, descriptor indices, sizes, CRCs and counts. Limits: for a
session served by the game itself (not by an HLE service) the output CRCs are `-`, because
Azahar records its reply before it writes the buffers.

`--trace-timing FILE` writes every event Azahar's CoreTiming runs (the shared page's clock, the HID
updates, and whatever else the services and the GPU schedule), to find where two runs part (N0-29;
[Determinism](#determinism)). After a header
(`# timingtrace v1 <date> game=<title id> cpu=<cpu> new3ds=<on|off>`) a line is
`tick=<decimal> event=<name> core=<n> late=<cycles> from=<emu|host>`, written just before the event's
callback runs: `tick` is that core's clock since the program loaded (the `t=` of `--trace-svc`, so a
tick finds the system calls of the thread that was running), `event` is the name the event was
registered with (a space in it is written as `_`: `Y2R_Completion_Signal_Event`), `core` is the core
whose timer ran it, `late` how many cycles after its time (`tick` minus `late` is when it was due),
and `from=host` marks an event scheduled from a host thread other than the emulation thread (the
thread that loaded the game; recorded when it was scheduled, so a host thread that cannot know the
emulated time is told apart from the loop). The observer is a pair of function pointers in the patch
(`Core::SetTimingObserver`); with none set, `ScheduleEvent` and `Advance` each do one atomic load and
nothing else changes. The file holds event names and ticks, no game data, but a long run writes a
line per event: keep it in `build/`.

`--capture-dsp START:FRAMES:FILE` and `--capture-hid FILE` write binary captures of the audio and
the input (`MDSP` and `MHID`: magic, `u32` version 1, then records `{u32 type, u32 frame, u64
ticks, u32 size, payload}`, plus a sibling `FILE.json` manifest). `frame` is the game's frame
counter (`0x010571d4`) when the record is written. `--capture-dsp` starts at the first DSP audio
frame at or after game frame START and records FRAMES audio frames (160 samples, 1,310,720 ticks,
32,728 Hz): per frame, type 1 the two DSP shared-memory regions (0x8000 bytes each) as they are
before the frame is generated, then type 3 for each sample buffer a source read (physical
address, size, CRC-32, and the bytes only the first time that address, size and CRC appear), and
type 2 the 160 stereo `s16` samples of the mixed output, taken before the (null) sink. It needs
three observer hooks in Azahar's HLE DSP (`AudioCore::HLE::g_capture_hooks`, in `azahar.patch`);
buffers are noted when the DSP dequeues them, extends an embedded buffer, and each audio frame it
re-reads the unconsumed part of a looping buffer from guest memory (the application may rewrite it
while it plays; the (address, size, CRC) rule stores bytes only when they change). `--capture-hid` writes,
on every change of the frame counter, type 1 the 0x2b0 bytes of HID shared memory as the game has
it mapped (zeros until it does) and, once a game has handed a shared-memory block to `ir:USER`
(the runner turns Azahar's IPC recorder on to see it), type 2 the first 0x200 bytes of that block
(the Circle Pad Pro header and receive-buffer info). Captures are game data: keep them in
`build/`, never commit or paste them. `tools/native/capfmt.py` has `summary FILE`, `check FILE`,
`pcm-to-wav FILE OUT.wav`, `diff-hid REF CAND` (buttons, circle pad and touch per game frame,
ticks ignored) and `diff-dsp REF CAND [--frames N]` (every source's status block and the DSP's
own frame counter, keyed on that counter rather than on game frames); they print counts, frame
numbers and statuses, never samples. The native runtime's captures (N1-16, N1-18) use the same format.

The capture holds the block weakly (N0-35): it never keeps it alive, so a game that finalizes `ir:USER`, closes its handle and opens it again at the same address (the game does, with the Circle Pad Pro option on) gets its second `CreateMemoryBlock` as it does without `--capture-hid` (the capture used to hold the first block, and that call failed with `0xE0A01BF5`); type-2 records are written only while a block is alive, none between a block's release and the next `Initialize`, and from that `Initialize` on the new block's (the first block is kept while it lives, so a game that opens `ir:USER` once records what it did before). `port/tests/ir_reopen.c` is the program that shows it.

`--capture-gpu START:FRAMES:FILE` writes an `MGPU` capture (format: `docs/native/mgpu-format.md`; `tools/native/gcap.py`
has `summary` and `check`) of everything that crosses Azahar's `VideoCore::GPU` interface for FRAMES game frames
(at most 30) from the first frame at or after START (`f<N>` is accepted too). The run reaches START as usual, then, between
two slices of emulation, the runner writes `INIT` (the GPU's registers, the LCD registers, the PICA register file, both
shader setups, the default attributes, the lighting, fog and procedural-texture tables, the immediate-mode state and the
geometry pipeline) and `MEM` (all of FCRAM, 128 MiB on the Old 3DS and 256 MiB with `--new-3ds on`, then VRAM, in pieces
of 16 MiB), keeps a copy of both as the shadow, and writes the first `FRAME`. From then on, before every GX command,
register write (`GPU::WriteReg`), buffer swap and colour fill it compares FCRAM and VRAM with the shadow page by page and
writes the pages the CPU changed (`CPU_PAGES`), then the event (`GX`, `MMIO`, `SWAP`, `FILL`); after a GX command or a
register write it compares again and writes what the GPU changed (`GPU_PAGES`), and the shadow is brought up to date after
each comparison, so the GPU's writes never show up as the CPU's. A command list's `PICA_REG` record holds every internal
register write the command processor made, in order, one entry per register written (a repeated write of one register,
as a shader upload makes, is one entry per word) and follows its `GX` at once; the interrupts the command signalled
(`IRQ`: the id and the delay it was given) come after that and after `GPU_PAGES`. `FLUSH` records the game's
FlushDataCache, InvalidateDataCache and StoreDataCache requests to `gsp::Gpu` (read from Azahar's IPC recorder, which
the runner turns on) and the regions of each CacheFlush GX command. A `FRAME` record, with the reference images
`FILE.frames/<n>_top.ppm` and `<n>_bottom.ppm` (`<n>` the frame counter in decimal; the software renderer at 1x, as it
holds the screens when the record is written), is
written when the frame counter changes: at the first GPU event of the new frame, or after the slice if none came.
The capture ends at the first event of frame START+FRAMES (a capture of 3 frames has 3 `FRAME` records and 6 images);
the run goes on. `FILE.json` is the manifest: game, format version, the frames covered, the renderer, the memory sizes, the
record counts and the file size. The observers (`Pica::g_capture_hooks` in `azahar.patch`: `GPU::Execute`,
`GPU::WriteReg`, `SetBufferSwap`, `SetColorFill`, the interrupt handler, and the four places in `PicaCore` that write an
internal register) are empty unless the option is given, and then cost one test each. Notes:
drawing stays on for the whole run, whatever `--draw-window` says (the memory at START has to hold the render targets of the
frames before it), and `disable_right_eye_render` is set off; a command list started by a register write
(`GPU::WriteReg` to a trigger register, which `gsp::Gpu`'s WriteHWRegs allows) gets its `MMIO` record but no `PICA_REG`, because the format
only has one after a command-list `GX`: the manifest counts such writes as `unrecorded_register_writes`; the geometry pipeline block
holds 0 for the fields a backend has not set up (`main_vertex_num`, `total_vertex_num` and the buffer index of the
variable-primitive backend while it waits for its index). The memory is not compared again after the last event, so the
last GPU change before the end of the capture is in the file and later CPU changes are not. A capture is about 140 MB
before its first event and the diffs take a while (a pass over 134 MB per GX command, twice): keep FRAMES small. Captures are game
data: keep them in `build/`, never commit or paste them. The recording logic (everything but the reading of Azahar's
state) is the region `BEGIN gpu-capture-core` of `runner.cpp`, free of Azahar types; `tests/test_native_gcap_recorder.py`
compiles it alone and makes captures from a scripted game. What only the CI runner and a run with the game exercise: the
hooks, the INIT blocks built from Azahar's state, the cache-flush match on `gsp::Gpu`, the reference images.

Every time in a script is emulated milliseconds or a game frame: `f600` is the first moment the
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
starts the screenshots late, so a close look at one moment doesn't image the whole run.
`--shot-every MS` (default 1000) is in emulated milliseconds and names the files by the time
(`0001500_top.ppm`, `0001500_bottom.ppm`); `--shot-every f<N>` is every N game frames, the first at
`--shots-from f<M>` (frame M; with `--shots-from` in ms, at the first frame after that time; without it,
at frame N), each taken at the first moment the frame counter reads that frame or more, and named
by it: `f0000100_top.ppm` and `f0000100_bottom.ppm` (7 digits, as `mem_f0000100_...` for a frame dump).
A frame the counter skipped over has no shot, and `--draw-window` has no effect with frame-keyed
shots (the screens are drawn all the time). `--print-schedule` shows the interval and the first
frame (`shot-every every=f60 first=f100`). With `--shots-from f<M>` and a millisecond `--shot-every`
the first shot is `f<M>` and the next ones follow every interval of emulated time. The golden
sessions use the frame-keyed form (N0-09): a screenshot taken at a frame does not depend on the
millisecond that frame falls at;
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
screens. It also sends sound nowhere, so it needs no sound card. `--init-time SECONDS[.FRACTION]` (N0-35) sets the start date of the emulated clock (seconds since the Unix epoch, UTC; default 1767268800, 2026-01-01 12:00 UTC; see *The start date with a fraction* in the Determinism section). `--room-host PORT` makes
a network room inside the runner (Azahar's multiplayer) and joins it, `--room-join HOST:PORT`
joins one, so two runners play the game's local wireless modes together (a copy of a user
folder needs `--new-console-id`: a room turns away a console it already has, and every folder
made by a default-mode run now carries the same console ID, so any two such folders need it). Give runners on
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

`--perf-log FILE` writes one CSV row per frame (a vertical blank of the emulated GPU to the next):
the frame time (`present_us`) and the host time in the recompiled code, in system calls and in the
GPU's command processing (`recomp_us`, `svc_us`, `gpu_us`; each microsecond counted once, in the
innermost of the three). The timers are in `src/perf.h` (a clock read at each end and an atomic
add; rows go to the file from a writer thread, nothing is logged or allocated per frame); the
scopes sit in `ARM_Recomp::Execute` and `ARM_Recomp::Svc`, the GPU's on the hooks of
`Pica::g_capture_hooks` (azahar.patch), so `--perf-log` and `--capture-gpu` cannot run together.
`tools/native/perf_report.py FILE [--fps N] [--skip N]` prints p50 / p95 / p99 / max per column
as a markdown table, with the frames over the budget. The app does the same with `perf_log = FILE`
under `[Debug]` in `mh4u-app.ini`. The numbers say nothing of the game, so the CSV can be shared.
The maintainer's steps for the baseline are in `docs/native/perf-procedure.md`.

Everything under `build/` contains game code: it never goes into the repository.

## Determinism

The reference runner is meant to repeat itself (D30): two runs with the same arguments, in fresh copies of one
user folder, give the same traces, captures, screenshots and user folders (the `log/` folder of the user folder
aside, by D49: its file is stamped with the host's clock and holds the run's directory path, it is the runner's own
diagnostic log and not state of the game or of the emulated console, so a comparison of two user folders skips it;
see N0-29, iteration 0, below). Without `--async-io` (its default mode)
it already read files in place, started at a fixed date and kept host time out of the delay of a file read; N0-26
fixes the two values that were still drawn anew in every run, and gives `--trace-timing` to look for what is left;
N0-29 removes the sources it found, except the runner's log (source 6; the list is below), and measures the result on the three gate sessions: with
the game, five pairs of runs are identical in every artifact compared (the runner's own log is left out of the
comparison, and differs: D49), see the last subsection.

What each mode keeps of Azahar's behaviour:
- `--async-io` keeps all of it: none of the values below is fixed.
- A room (`--room-host`, `--room-join`) and `--new-console-id` keep Azahar's Wi-Fi MAC address and console unique ID
  (a room hands out its own address; a console that asked for a new identity gets a drawn one). Everything else in the
  table below is as in the default mode: the start date and tick, the reads of files, the time zone, the zeroed
  unwritten bytes of reply words and the zeroed padding of the metadata files are set in every mode but `--async-io`.

### What the game sees, for the native runtime to start from

These are the values the default mode fixes, and so the values the native runtime's deterministic mode starts from
(the follow-up that D30 asks for is opened from this list). Each is a constant in the runner, set through Azahar's own
setting where it has one.

| Value | Azahar | Default mode of the runner | Where the game sees it |
|---|---|---|---|
| Start date | the host's date (`init_clock` SystemTime) | `init_clock` FixedTime, `init_time` 1767268800 (2026-01-01 12:00:00 UTC) unless `--init-time SECONDS[.FRACTION]` gives another date, up to microseconds (N0-35: the whole seconds go to `init_time`, the fraction to the shared page's handler through the patch's `SharedPage::SetInitTimeFraction`; a value without a fraction, or with an all-zero one, is exactly the integer); the runner sets `TZ=UTC0` itself, because the shared page converts it through the host's time zone: see the time zone row | the shared page's date records (0x20, 0x40), and what derives from them: `ptm`'s console time, the news database, what the game reads through the records. **Not** `svcGetSystemTick`, the records' update ticks (0x28, 0x48), `kFixedStartTicks` or the time zone handling, which a fraction leaves as they are |
| Starting tick of the emulated clock | a random 32-bit number drawn per run (`Timing::GenerateBaseTicks`: `std::mt19937` seeded from `std::random_device`, so any value from 0 to 2^32 - 1) | `init_ticks_type` Fixed, `init_ticks_override` **2,000,000,000** (0x77359400, 7.4596 s of the 268,111,856 Hz clock); every core's timer starts there | `svcGetSystemTick` (the running core's timer: this plus the ticks emulated so far); the update ticks in the shared page's date records (0x28 and 0x48); the date in those records, which Azahar computes as `init_time` plus the whole clock, starting tick included, so at the first update the date is `init_time` + 7459 ms |
| Console Wi-Fi MAC address | the folder's `mac.txt`, or a random address from a list of 16 Nintendo-like ranges, written to `mac.txt`, when there is none | **02:00:00:00:00:01** (locally administered), not read from or written to `mac.txt` | the shared page's bytes 0x60 to 0x65 (the local wireless service puts it there when it starts, and the runner sets the page once the game has loaded, so the page has it either way), `cfg`, and what the local wireless service and `nwm::SOC` take from it |
| Reads of files | on host threads, finishing at a host-dependent emulated time | `deterministic_async_operations`; the patch keeps host time out of the delay of a read | when a read wakes the game |
| Console unique ID of a config savegame that the run makes (N0-29) | drawn per run from the host's random source (`CryptoPP::AutoSeededRandomPool`): 16 random bits and, in a folder without `nand/rw/sys/LocalFriendCodeSeed_B`, a random 34-bit friend code seed | **0x4D480001A2B3C4D5** (random number **0x4D48**, friend code seed 0x1A2B3C4D5; `kFixedConsoleId`, `kFixedConsoleRandomNumber`), under the MAC address's condition (no room, no `--new-console-id`); a config savegame that already exists keeps its ID | `cfg:u` command `0x00030040` (GetTransferableId), whose reply for a salt is the last 8 bytes of SHA-256 of the ID (8 bytes, little endian) and the salt (`& 0xFFFFF`, 4 bytes): **salt 0: words 0x61B8087C, 0xB904EFF1; salt 0x1263 (the game's): words 0x9592177D, 0xFE13E102**; what the game derives from those and stores (the data show this and no more: in G2 the game's save file is equal in two runs with this ID, where 16 bytes of it differed with the previous runner; the ID was fixed together with the other changes, so the measurement does not show that the save holds anything derived from the ID: see the gates measurement, last subsection); the config savegame's blocks 0x90000 and 0x90001 (bytes `D5 C4 B3 A2 01 00 48 4D`) and 0x90002 (`48 4D 00 00`) |
| Unwritten bytes of reply words (N0-29) | whatever the host's heap held (the command buffer of a request is not initialised) | 0 (`SetZeroedIpcReplies`), except in a word whose index the request also had: the request's words are copied into the same buffer first, and a reply word the service does not fully write keeps those bytes there (the guest's own bytes, the same in every run, as before this change) | the bytes of a reply word that the service did not write: bytes 1 to 3 after a `u8`, an enum of `u8` or a `bool`, bytes 2 and 3 after a `u16`, all four of a word it declares and does not push (seen: `APT:A` 0x01010000, `cfg:u` 0x00020000, `y2r:u` 0x00280000) |
| Duplicate flag of an extdata created by the obsolete command (N0-29) | `FS_USER::ObsoletedCreateExtSaveData` (command 0x0830) never sets `duplicate_data`: stack residue, which `GetFormatInfo` returns | false (with `deterministic_async_operations`) | `fs:USER` `GetFormatInfo` after command 0x0830 (and byte 12 of the archive's `metadata` file); not reached by G1, G2 or G6, so not measured |
| Time zone (N0-29) | the host's (`TZ`) | `UTC0`, set by the runner | the date of the shared page's records (and what `ptm` and the news database take from it) |

Notes for the mirror:
- The starting tick 2,000,000,000 is also when the clock's high word changes: `svcGetSystemTick` reaches 2^32 after
  2,294,967,296 more ticks (8.56 s of emulated time), at the same moment in every run (the random start used to put
  it at a different moment in every run, from the first tick to 16 s in).
- The traces' `t=` and `tick=` are relative to the load of the program (`g_start_ticks`), so the starting tick shows
  in the game's own clock, not in those fields.
- The MAC address is fixed only when the default mode has no room and no `--new-console-id`. In a room the room hands
  out an address (the local wireless service uses the room's, whatever `mac.txt` says), and a console that asked for
  a new identity keeps `mac.txt`'s.
- The console unique ID follows the same rule (default mode, no room, no `--new-console-id`) and only decides a config
  savegame that the run makes. A folder that already has a config savegame keeps the ID in it, and nothing rewrites it.
  Fixed 0x4D480001A2B3C4D5 is this port's own choice (any pair of Azahar's shape would do). The native runtime's
  decision D37 (N1, in the batch of PR #19 when this was written) gives a fresh user folder a random ID and says that
  goldens and replays use folders that already hold a config savegame: that fits the reference only if the folder a
  golden starts from (`user-before`) holds the config savegame that a default-mode reference run makes, with this ID;
  a golden recorded from a folder without one holds a config savegame made during the run (this ID) that a native
  run, drawing its own, would not reproduce. N0-09 has to record from a folder that holds it (its spec says so), and
  the native runtime's deterministic mode must use this ID where it makes one.
- Why one fix covers the shared page's date: its update callback (`SharedPage::Handler::UpdateTimeCallback`) writes
  `date_time` from `init_time` plus the clock (`GetGlobalTimeUs`) and `update_tick` from the timer's own ticks, so
  both carried the random starting tick and nothing else of the host (the update is an event scheduled at tick 0 of
  core 0, and then once an hour). Read from the sources; the measurements below find the page identical in two runs
  (N0-26: the dumps at frames 100 and 800; the gates: the dump at frame 300, in every pair).

### The start date with a fraction: `--init-time` (N0-35)

`--init-time SECONDS[.FRACTION]` is the start date of the emulated clock: seconds since the Unix epoch, UTC. The
grammar is exactly this and nothing else: SECONDS is 1 to 10 ASCII digits, FRACTION a `.` followed by 1 to 6 ASCII
digits (microseconds); no sign, no space, no exponent, no hex, no `.5`, no `5.`, no second dot, no trailing text. The
value in microseconds is SECONDS * 1000000 plus the fraction padded on the right with zeros to 6 digits, and it must
lie in the 3DS clock's range, both ends included: 946684800 (2000-01-01 00:00:00 UTC) to 4102444799.999999
(2099-12-31 23:59:59.999999 UTC). The default is 1767268800, and a value without a fraction, or with an all-zero one
(`1767268800.000000`), behaves exactly like the integer, and the integer 1767268800 exactly like no option: the
shared page, every trace and the program's and the runner's output are byte for byte what N0-29 measured, and no line is added to a trace
header or to an output unless the option is given (`--print-schedule` then ends with an `init time:` line).
`--init-time` together with `--async-io` (which starts at the host's time and would ignore the date) is a usage error.
A repeated `--init-time` takes the last value, and a bad value in either occurrence is an error. `--print-schedule` is the one output that differs when the option is given (even `--init-time 1767268800` prints the extra `init time:` line): "byte for byte" is about runs, the shared page, the program's output and the runner's stdout.

Exit status: a bad value (or a missing one, or the combination with `--async-io`) is a message on stderr that names
`--init-time` and the offending text, and **exit status 2**. The other bad options of the runner exit with 1 (usage
text) and 2 is also what a program that failed to load gives; the specification of the option says 2 for the
reference runner and the native runner alike, so this follows the specification.

What the fraction moves, and why it needs a hunk of the patch. Azahar's shared page writes its date in
`SharedPage::Handler::UpdateTimeCallback` (once at tick 0 and then hourly) from `GetSystemTimeSince1900()`, which
is `init_time` (a whole number of seconds: `Settings::values.init_time`) plus the milliseconds of the global time
(`Timing::GetGlobalTimeUs()`, the timers' ticks from `kFixedStartTicks`), together with `update_tick =
timing.GetTicks()`; the game reads the date record plus (its tick minus `update_tick`) over the rate. The only
other way to move the date is the starting tick, and that would also move `svcGetSystemTick` and the records'
update ticks (and with them everything the game derives from its own tick); `init_time` cannot carry a fraction. So
the whole seconds go to `Settings::values.init_time` and the fraction, in microseconds, to a process-wide setter
that the patch adds to `shared_page.h`/`.cpp` (`SharedPage::SetInitTimeFraction`, default 0, not serialized: the
runner does not use save states), which `GetSystemTimeSince2000` adds to the global time before it cuts the sum
to milliseconds: date = floor((init_time * 10^6 + fraction + global time) / 1000) ms. With fraction 0 that is
`init_time` plus the milliseconds of the global time, the unpatched sum (integer arithmetic, `init_time` has whole
seconds), so a run without a fraction is Azahar's and N0-29's.

| | moved by the fraction | left alone |
|---|---|---|
| the date | the shared page's date records (0x20 and 0x40), hence what derives from them: what the game reads through the records, `ptm`'s console time, the news database | |
| the ticks | | `svcGetSystemTick`, the records' update ticks (0x28 and 0x48), `kFixedStartTicks` (2,000,000,000) |
| the host | | the `TZ=UTC0` handling |

The date shifts by exactly the fraction in whole milliseconds (`.5` is 500 ms later than the integer), and a fraction below
a millisecond by 0 or 1 ms, depending on the clock's microseconds at the update. The option does not move the other host-clock stamps either (file times of the file system services, the runner's log, `err_f`), which the integer `init_time` never moved. The tests (`tests/test_port.py`,
`port/tests/time_probe.c`) read the records and the tick with a program and check this without the game. The
goldens use it to put the game's first clock reading in the middle of a second (D83).

### Finding what is left

Two runs with the same arguments, each with `--trace-timing` and `--trace-svc` (and `--state-trace`): the first line
where the two timing traces differ names the event and its tick; that tick is a `t=` of the SVC trace, whose lines
around it show which thread was running and what it was doing. `from=host` lines are events scheduled from host
threads (asynchronous work that a host thread finished, say): their emulated time cannot depend on the host if the
runs are to repeat, so they are the first suspects. `tools/compare_svc_traces.py`, `tools/compare_ipc_traces.py` and
`tools/native/state_diff.py` compare the other traces. Anything that reads the host's clock or a random source in
code the game reaches (`std::chrono::system_clock`, `steady_clock`, `Common::Timer`, `time(`, `std::random_device`,
`rand`) is a candidate; the two this section started with were found that way in `core_timing.cpp` and `cfg.cpp`.

### Measurement with the game (N0-26)

Measured on 2026-10-21 on the maintainer's PC, idle: two runs (30 s and 31 s) of the runner built from this branch
(the Windows CI build), the code library built from the update at `e6cd840`, `TZ=UTC0`, a fresh copy of one user
folder per run (update 1.3.0 installed, no save), golden session G1 as `--input-file`, `--cpu recomp --new-3ds off
--frames 800` with `--state-trace --trace-svc --trace-ipc --trace-timing` and `--dump f100:1FF81000:0x1000
--dump f800:1FF81000:0x1000`. This is the measurement N0-29 starts from, not a gate.
"Measured on 2026-10-21" is the UTC date written in the headers of the traces.

Identical in the two runs (apart from line 1, the header with the date):

| What | Size | Result |
|---|---|---|
| Shared page, 4 dumps (frames 100 and 800, two runs) | 0x1000 bytes each | one SHA-256 for all four: the page does not change between frame 100 and frame 800; bytes 0x60 to 0x65 are `02 00 00 00 00 01` |
| State trace (`--state-trace`) | 802 lines | identical, so the tick `t` of every record too |
| SVC trace (`--trace-svc`) | 489,775 lines | identical; `compare_svc_traces.py --normalize --outputs` reports "all threads match": 18 threads, equal call counts in every thread, 460,699 calls in the main thread in both runs |
| Timing trace (`--trace-timing`) | 33,963 lines | identical |

Not identical: the IPC trace (`--trace-ipc`, 7,171 lines in both runs). Three lines differ, all in the reply words
(`out=`) and nowhere else (the session, the command header, the result code, the descriptors and the CRCs are the
same in every line):

| Line | Session | Command header | What differs |
|---|---|---|---|
| 236 (the first) | `APT:A` | `0x01010000` | the third reply word |
| 243 | `APT:A` | `0x01010000` | the third reply word |
| 245 | `cfg:u` | `0x00030040` | the last two reply words (together a 64-bit value) |

What the comparators say:
- `compare_ipc_traces.py` reports "all threads match" for the two runs (8 threads, with 6083, 2, 4, 163, 3, 39, 31
  and 827 requests). That is expected and does not mean the three lines are equal: without `--strict` it compares,
  per thread and in order, the session, the command header, the result code (the first reply parameter) and the
  descriptors, and never the other reply words. With `--strict` it also compares every word and names the words that
  differ, so it would report these three lines. The measurement did not run it with `--strict`; the three lines were
  found by comparing the files line by line. (The control below did run it with `--strict`.)
- `compare_svc_traces.py --normalize --outputs`: "all threads match" (above). The earlier recompiler pairs of
  2026-10-17 differed in the main thread's call count only; at 800 frames with the fixed starting tick that
  difference is gone.

What this shows: with the fixed starting tick (`kFixedStartTicks`) and the fixed MAC address, two 800-frame G1 runs
repeat exactly in the state trace, the SVC trace, the timing trace and the shared page, and differ in three IPC reply
lines. What this measurement alone does not show is why the earlier runs differed; the control below is the
comparison with the previous runner.

#### Control: the previous runner on the same setup

This control was taken on 2026-10-21 (the UTC date of the trace headers) with the previous runner: the Windows
CI build of `e6cd840` (`windows.yml` run 39075253705), which draws the starting tick and the MAC address at random in
every run. Everything else is as above: the same code library, the same G1 script, `--frames 800`, `TZ=UTC0`, a fresh
user folder per run, `--state-trace --trace-svc --trace-ipc` and the dumps at frames 100 and 800. There is no
`--trace-timing`, because that runner has none. Two runs of each runner are compared with each other:

| What | Previous runner (random values), run 1 against run 2 | This branch's runner (fixed values), run 1 against run 2 |
|---|---|---|
| Shared page (0x1000 bytes at frames 100 and 800) | the two runs differ by SHA-256 (frames 100 and 800 are equal within a run); 12 bytes differ, at 0x40-0x41, 0x48-0x4B and 0x60-0x65 (the second date record's date and update tick, and the MAC address) | one SHA-256 for all four dumps |
| State trace (802 lines in both) | `t` differs in 7 of 801 records, the first at frame 468, by -562 to +8 ticks, and is equal again at the last frame; the game's own frame delta (`smain_delta`, marked `ignore` in `config/native/observable.txt`) differs in 471 of 801 records; `scene`, `frame_counter`, `quest_id` and `stage_loader_state` are equal in all records; by raw lines, 472 of 802 differ | identical |
| SVC trace | 489,728 lines against 492,180; `compare_svc_traces.py --normalize --outputs`: one thread differs, the main thread, at call 82233, in the loop that polls `GetSystemTick` (svc 0x28), the returned tick differing by 8 | 489,775 lines in both; all threads match |
| IPC trace (7,171 lines in both) | 271 lines differ (raw comparison, line 1 equal); `compare_ipc_traces.py` without `--strict`: "all threads match" (8 threads, 6083, 2, 4, 163, 3, 39, 31 and 827 requests); with `--strict`: only the main thread differs, first at request 54 (`fs:USER`, command `0x08170000`), in reply word 2 | 3 lines differ (above) |

Two caveats, both of the control and not of the runs above:
- One other emulator process (another job of the same session) was running on the PC during the control, so the PC
  was not fully idle for it; the runs above were taken on an idle PC.
- Both fixed values changed at once between the control and the runs above. The control does not say which of the two
  removed which difference. The MAC address is not a clock, which makes the starting tick the likelier source of the
  tick differences (the state trace's `t`, the polling loop in the main thread), but that was not measured separately:
  there was no run with only one of the two values fixed.

What the control does show: with the previous runner, two runs differ in the shared page, in the state trace's ticks,
in the main thread's polling and in 271 IPC lines; with this branch's runner, on the same setup, they do not, except
for three IPC reply lines. How many of the 271 lines are the same kind as the three was not looked at.

Where N0-29 starts: the three IPC reply words. They are leads, nothing more is known about them. For both the lead is
"a random source or a stale reply word" (a 64-bit value in `cfg:u` command `0x00030040` that varies from run to run,
and a reply word of `APT:A` command `0x01010000` that varies), and the data cannot tell the two apart, so the choice
is left to N0-29. Neither is in `config/native/ipc_time_dependent.txt`, which has no entries yet.

Not measured: runs of 1700 and 9000 frames, golden sessions G2 and G6, a run under load, the captures (HID, DSP,
GPU), the screenshots and the user folders after the run (these are N0-29's gates), which of the two fixed values
removed which difference, and why the three reply words differ.

### Sources found by N0-29, iteration 0

Iteration 0 measured the N0-26 runner (the Windows CI build of `wp/N0-26` at `6fa80b1`) on the three gate sessions,
two runs each on an idle PC, with every recorder (`--trace-svc`, `--trace-ipc`, `--trace-ipc-full`, `--state-trace`,
`--trace-timing`, `--count-unmapped`, the HID and DSP captures, `--capture-gpu`, `--shots`): G1 to 1700 frames, G6 to
3700, G2 to 9000. In every pair the state, SVC and timing traces, `unmapped.txt`, the HID and DSP captures and every
screenshot were identical, and so were the GPU capture's manifest and reference images in the sessions that have one
(G1, captured at frame 1500, and G2, at frame 2500; G6 has none). What still differed (the first three
rows are game state or what the game sees; the last is not, and is not removed):

| What | Where | How much |
|---|---|---|
| Reply words (`out=`) of five kinds of IPC request: `APT:A` 0x01010000, `cfg:u` 0x00030040 and 0x00020000, `y2r:u` 0x00280000, and, in G2, the CRC of the buffer of one `fs:USER` file write | the IPC traces | 7, 15 and 18 lines in G1, G6 and G2 (the five kinds are the union over the three sessions) |
| The GPU capture file | `.mgpu` | 8 consecutive bytes |
| The user folder after the run | the config savegame of `cfg` (every session), a metadata byte of the game's extdata (G1 and G6), the game's own save file (G2) | 16 bytes of the config savegame, 1 byte, 16 bytes |
| The runner's log file, in the same user folder | `log/mh4u_runner_log.txt` (every session) | every line: Azahar stamps each line with the host's clock (`steady_clock`, `common/logging/backend.cpp`), and some lines hold the absolute path of the run's directory; with the stamp and that path removed, the lines are equal |

How each was found: the first line is the comparison of the two runs' files; for an IPC line, which words differ
(`compare_ipc_traces.py --strict` names them; the plain comparison does not look at reply words) and what the service's
code writes into them; for a file, the offsets that differ against the layout of the file (the config savegame's block
table, the 16-byte `ArchiveFormatInfo`); for the GPU capture, the record the bytes sit in, which names the guest
address, and then the same value in the IPC trace's reply. The sources, by what they are:

1. **The console unique ID of a new config savegame (random).** The user folder the sessions start from has no config
   savegame, so every run makes one (`Module::FormatConfig`), and `Module::GenerateConsoleUniqueId` draws its ID from
   the host's random source: a 16-bit number and, because the folder has no local friend code seed, 34 random bits of
   seed, stored in the blocks 0x90000, 0x90001 (the ID) and 0x90002 (the number). That is exactly the 16 bytes of the
   config savegame that differ (the low 34 bits and the top 16 bits of each copy of the ID, and the number: the bits
   between are always 0). `cfg:u` GetTransferableId answers from the ID, so its reply words differed in every run; the
   game keeps that reply in its memory, which is the 8 bytes of the GPU capture (the capture records the guest's
   memory). In G2 the game's own save file also differed in 16 bytes, and so did the CRC of the buffer of the
   `fs:USER` write that stores it. With this ID fixed (together with the other changes of the iteration: sources 2
   and 3) the two save files, that CRC and the GPU capture are equal in the gate runs (last subsection). That is what
   the data show and no more: they do not show that the 16 bytes of the save are derived from the transferable ID,
   nor which of the changes removed which difference. **Removed:** the patch adds
   `Service::CFG::SetFixedConsoleUniqueId(random_number, console_id)` (a switch like `SetFixedConsoleMac`: it refuses a
   pair that is not in the shape Azahar makes, and nothing else changes) and `Module::GenerateConsoleUniqueId`, which
   a config savegame that is made calls, returns that pair when it is set; the runner sets `kFixedConsoleId` under the
   condition of the MAC address (no `--async-io`, no room, no `--new-console-id`: a console that asked for a new
   identity gets one drawn, as before). A user folder that already has a config savegame keeps its ID (nothing
   rewrites it), so two runs in copies of one such folder repeat as they did. The values the game sees are in the
   table above; the native runtime's deterministic mode starts from the same ID (N1-15 has an open question about the
   ID of a fresh user folder: this is the answer for the reference runner's default mode).
2. **Unwritten bytes of reply words.** `HLERequestContext::cmd_buf` is never initialised (only its first word is), and
   the contexts are made on the host's heap for every request. A service that pushes a `u8`, a `u16` or a `bool` writes
   one or two bytes of its word (`RequestBuilder::Push<u8>` copies one byte), and the rest of the word, copied to the
   guest with the reply, was whatever the block held before: in every differing line of the three kinds the low
   byte (the value) was equal in both runs and only bits 8 to 31 differed. Seen in `APT:A` GetTargetPlatform,
   `cfg:u` GetRegion and `y2r:u` IsBusyConversion in these sessions, and in `fs:USER` IsSdmcDetected in N0-26's
   control: the same for every handler that pushes less than a word or declares a word it does not fill. The state, SVC
   and timing traces were identical, so the game did not act on the residue in these sessions. **Removed:** the patch adds
   `Kernel::SetZeroedIpcReplies(bool)`; with it set, the constructor of the context zeroes the whole 256-byte buffer
   instead of its first word; the runner sets it in the default mode (no `--async-io`). Cost: one 256-byte fill per
   HLE request (not measured). The unwritten bytes of a reply word are now 0, with one exception that was already
   deterministic: `PopulateFromIncomingCommandBuffer` copies the request's words (1 plus its normal parameters) into
   the same buffer before the handler runs, so a reply word whose index the request also had and that the service does
   not fully write keeps the request word's bytes there (`dsp::DSP` 0x001000c0, for one, has a four-word request and
   replies a `u16` in word 2: bytes 2 and 3 of the reply word are bytes 2 and 3 of the request word). That is the
   guest's own data and the same in every run. The mirror starts its reply buffer from the request's words and zeros
   the rest, so the same bytes come out. The native runtime's decision D47 (N1, in the batch of PR #19) said a `u8`, `u16` or `bool` reply writes the
   whole word, zero-extended, which would differ from the reference exactly in the words described above (a reply
   word whose index the request had, and whose bytes the service does not write). D53 (2026-10-21) amended it:
   the native runtime keeps the request word's bytes in the bytes a service does not write (and zero where the
   request had no word at that index), as the reference does; N1-10b implements it in the IPC framework's `Reply`,
   so the strict IPC comparison finds those words equal.
3. **Padding of the metadata of an extdata or save (uninitialised).** `FS_USER::CreateExtSaveData` (and
   `FormatSaveData`, `FormatThisUserSaveData`) declare an `ArchiveFormatInfo` without initialising it, set its four
   fields, and the archive writes the whole 16 bytes, the three padding bytes after `duplicate_data` included, to the
   `metadata` file: byte 13 of the game's extdata metadata was stack residue (1 in one run, 2 in the other; in G2 the
   two runs happened to agree). **Removed:** in the patch, both writers (`FormatAsExtData` for the extdata,
   `ArchiveSource_SDSaveData::Format` for the save) write a zeroed 16-byte buffer with the four fields copied in, when
   `deterministic_async_operations` is set (the runner sets it without `--async-io`). The game never sees the padding
   (`GetFormatInfo` returns the four fields), so no value it sees changes; the file of the user folder does.
   One byte of this struct is seen by the game: `FS_USER::ObsoletedCreateExtSaveData` (command 0x0830) never sets
   `duplicate_data` (byte 12 of the metadata), and `GetFormatInfo` returns `duplicate_data != 0`. **Removed:** with
   `deterministic_async_operations` set, the patch sets it to false there, so byte 12 of that metadata and the
   duplicate flag `GetFormatInfo` then returns are 0 (the other three callers already set it). The three sessions
   make no request of command 0x0830, so no measurement shows it; it is the same class as the padding.
4. **The host's time zone.** Not a difference between runs on one PC: `SharedPage::Handler::GetSystemTimeSince2000`
   converts the year 2000 with `std::mktime`, a local time, so the date in the shared page depends on `TZ`; until now
   the instructions asked for `TZ=UTC0` and the measurement script set it. **Removed:** in the default mode the runner
   sets `TZ` to `UTC0` itself before it loads anything (`_putenv_s`/`_tzset` on Windows, `setenv`/`tzset` elsewhere),
   so a run from another shell or on another PC gives the same date. Nothing changes where `TZ=UTC0` was set.
5. **Padding of `mcu.dat` (uninitialised, not seen to differ).** `Module::SaveMCUConfig` writes the 24-byte `MCUData`
   raw; its last 4 bytes are tail padding, which the struct does not define. They were zero in every run so far, which
   shows nothing about the next, so the same cure as for source 3: **Removed:** with `deterministic_async_operations`
   set, the patch copies the 20 bytes of fields into a zeroed 24-byte buffer and writes that. The game only sees
   `clock_sequence` (`cfg:u` GetUUIDClockSequence), so no value it sees changes.
6. **The runner's log (not removed).** The log file in the user folder (`log/mh4u_runner_log.txt`) differs in every
   line between two runs: Azahar stamps each with the host's clock, and a few hold the run's directory path. It is not
   game state, the game cannot read it and nothing reads it back, so it is left as Azahar writes it, and D49 (the
   orchestrator's decision at the maintainer's request, 2026-10-21) makes the comparison of two user folders skip
   `log/`: the exact comparison of the user folder is of every other file, byte by byte (the orchestrator's comparison
   script does this; no script in this repository compares user folders). Zeroing the stamp would need a switch in
   Azahar's logging backend, and the paths would still differ unless both runs used the same directory; the log is
   also what shows what a run did when something is wrong, which a silenced log would not.

What was measured with the runner of this iteration (the next subsection has the setup and the numbers): the IPC
traces repeat in all five kinds of request (`APT:A` 0x01010000, `cfg:u` 0x00030040 and 0x00020000, `y2r:u` 0x00280000,
and in G2 the CRC of the `fs:USER` write of the game's save), the config savegame and the extdata metadata are equal
in every run (from a folder without a config savegame), the GPU capture file is equal, and the game's save in G2 is
equal; in none of the five pairs of runs did one of those differ. The runner's log (source 6) stays different, as
predicted. The contributions of the individual fixes were not separated, since they were applied together (the ID,
the zeroed reply bytes, the padding of the metadata), so the measurement does not say which fix removed which
difference; the previous paragraphs say which source each difference was traced to, and that is from the code and
the record of the differing bytes, not from a run with one fix alone.

Found by reading the code and not removed, because no session reaches it or it has not differed (a lead for the next
iteration, none of it measured):
- `ps:ps` GenerateRandomBytes and `ssl:C` GenerateRandomData (OpenSSL `RAND_bytes`), `am`'s export of a wrapped ticket
  (OpenSSL `RAND_bytes`, `am.cpp`), the key and signature code in `core/hw/ecc.cpp` and `core/hw/rsa/rsa.cpp` and the
  random tag contents of `nfc` (`nfc_device.cpp`) (all three `CryptoPP::AutoSeededRandomPool`), and
  `svcGetSystemInfo` type 0x20000 (`HOST_TICK`, the host's clock): none of the sessions opens those services or calls
  that SVC.
- Environment variables `MH4U_*` and `<user dir>/mh4u.ini` are read by the port's hooks when the code library loads
  (`hooks.cpp`: the frame rate, widescreen, injected buttons, overlays and so on). The runner does not clear them: its
  own test sets one on purpose (`MH4U_NET_SYNC` in `tests/test_port.py`), so unsetting every `MH4U_*` in the default
  mode would break that. A reference run has to start without them and without an
  `mh4u.ini` in the user folder (the measurement scripts set none).
- The vertex shader JIT picks instructions by the host CPU (reciprocal and reciprocal square root), which could change
  low bits of the pixels between two machines (not between two runs on one); `lle_applets` is on by default but the
  user folder has no applet titles; the order of a thread's held mutexes when it exits holding more than one (one mutex
  in the sessions); the order of host directory listings (`DiskDirectory` does not sort); the host's network answers
  (`ac:u`, `soc:U`). None differed or is reached by the three sessions.
- `arm_recomp.cpp` and `arm_recomp.h` are not changed in this iteration, so there is no `--perf-log` comparison to
  make; the timing and SVC traces were already identical in every pair.

### Measurement with the game (N0-29, gates)

Measured on 2026-10-21 (the UTC date written in the headers of the traces) on the maintainer's Windows 11 PC (AMD
Ryzen 5 5600X, 6 cores and 12 threads, 32 GB), idle except where stated. The runs are kept under the git-ignored
`build/` of the maintainer's checkout; this section holds numbers and names only.

**Setup**
- Runner: the Windows CI build of `wp/N0-29-runner-exact` at `3aef9b8` (`windows.yml` run 39582006178; Native CI
  run 39581995274, all six jobs green, `runner`, `native-windows` and `native-macos` among them; `native-guard` run
  39581995229 green), `mh4u_runner.exe` with a SHA-256 starting `FDD639F5F2EEAE7C`. This is the runner of the
  iteration that applied the fixes of sources 1 to 5 together; it is not the N0-26 runner of iteration 0.
- Code library: the paged library built from the update at `e6cd840` (the port's hooks are unchanged since).
- User folder: a fresh copy of one folder per run (update 1.3.0 installed, no save and no config savegame), so every
  run makes its own config savegame. `--cpu recomp --new-3ds off`.
- `TZ=UTC0` was set by the measurement script as well, so this measurement does not isolate the runner's own
  setting of the time zone (source 4).
- Sessions: G1 to 1700 frames, G6 to 3700, G2 to 9000, each as `--input-file` (the golden sessions of the N0-09
  work, `G1.txt`, `G6.txt` and `G2.txt`). Runs: G1 three times (62, 63 and 95 s), G6 twice (301 and 300 s), G2 twice
  (1180 and 1197 s). All exited 0.
- Recorders, in every run: `--state-trace` (with `config/native/observable.txt`), `--trace-svc`, `--trace-ipc`
  with `--trace-ipc-full`, `--trace-timing`, `--capture-hid`, `--count-unmapped`, `--shots` from frame 300 every 60
  frames, and `--dump f300:1FF81000:0x1000` (the shared page; the dump is written into the shots folder). Besides
  that: G1 with `--capture-dsp 1240:300` and `--capture-gpu 1500:3`, G2 with `--capture-gpu 2500:3`, G6 with
  neither (its session file lists none).
- In parallel: nothing, except in the third G1 run. It ran while a second `mh4u_runner` process ran G2 on the same PC
  (with `--shots` only, no other recorder): the script started that process 15 s before the run and stopped it after
  the run, so it was there for the whole of the run (95 s against 62 and 63 s).
- What was compared (the orchestrator's comparison script, which is not in the repository; it only reads): the text
  traces line by line, line 1 left out when both start with `#` (the header, with the date); the binary captures,
  their manifests, every screenshot, the frames of the GPU capture and every file of the user folder after the run
  (`log/` left out, D49) by SHA-256, with the same file names on both sides. Line 1 of each text trace, left out of
  that comparison, is byte-identical in every run as well, so the whole files are identical. Line counts below are
  lines (newline characters; the traces end their lines with CRLF), line 1 included.

**G1** (1700 frames; three runs, compared in all three pairs: 1-2, 1-3, 2-3; run 3 under load)

| What | Size | Result |
|---|---|---|
| State trace | 1,702 lines | identical in all three pairs |
| SVC trace | 1,082,077 lines | identical |
| IPC trace | 18,954 lines (21 comment lines and 18,932 requests besides line 1) | identical, including every reply word |
| Timing trace | 78,579 lines | identical |
| Unmapped-access count | 1 line (the header only) | identical: no unmapped access was counted in any run |
| HID capture | 1,204,316 bytes (manifest 164) | identical |
| DSP capture | 20,149,224 bytes (manifest 165) | identical |
| GPU capture | 144,248,896 bytes (manifest 550); frames folder: 6 PPM files (frames 1500 to 1502, both screens), 1,555,290 bytes | identical, the 8 bytes of iteration 0 included |
| Shots | 49 files: 48 PPM and the 4,096-byte dump of the shared page | identical |
| User folder after the run, without `log/` | 25 files, 51,788,928 bytes (the config savegame and the extdata metadata among them) | identical file by file |
| Runner's log (`log/mh4u_runner_log.txt`) | 42 lines, 6,506 bytes in each run (and an empty `.old` file) | SHA-256 differs in all three runs, as predicted (left out of the comparison, D49) |

**G6** (3700 frames; two runs, one pair)

| What | Size | Result |
|---|---|---|
| State trace | 3,702 lines | identical |
| SVC trace | 3,711,176 lines | identical |
| IPC trace | 47,780 lines (25 comment lines and 47,754 requests besides line 1) | identical, including every reply word |
| Timing trace | 188,524 lines | identical |
| Unmapped-access count | 1 line (the header only) | identical: no unmapped access was counted in any run |
| HID capture | 2,620,316 bytes (manifest 164) | identical |
| DSP and GPU captures | none requested | not applicable |
| Shots | 115 files: 114 PPM and the 4,096-byte dump of the shared page | identical |
| User folder after the run, without `log/` | 25 files, 51,788,928 bytes | identical file by file |
| Runner's log | 42 lines, 6,506 bytes in each run | SHA-256 differs |

**G2** (9000 frames; two runs, one pair)

| What | Size | Result |
|---|---|---|
| State trace | 9,002 lines | identical |
| SVC trace | 7,007,565 lines | identical |
| IPC trace | 97,094 lines (25 comment lines and 97,068 requests besides line 1) | identical, including every reply word and the CRC of the `fs:USER` write of the save |
| Timing trace | 398,679 lines | identical |
| Unmapped-access count | 1 line (the header only) | identical: no unmapped access was counted in any run |
| HID capture | 6,372,716 bytes (manifest 164) | identical |
| DSP capture | none requested | not applicable |
| GPU capture | 145,694,492 bytes (manifest 550); frames folder: 6 PPM files (frames 2500 to 2502, both screens), 1,555,290 bytes | identical |
| Shots | 293 files: 292 PPM and the 4,096-byte dump of the shared page | identical |
| User folder after the run, without `log/` | 26 files, 51,870,336 bytes (the game's save file, 81,408 bytes, among them) | identical file by file |
| Runner's log | 51 lines, 8,717 bytes in each run | SHA-256 differs |

**What the comparators of the repository say**, run on the saved runs (they only read). The same three commands were
run on every pair of the table above, and all of them report a match:
- `tools/native/state_diff.py --tolerance 0`: exit 0 on all five pairs. G1 (all three pairs): "traces agree: 4 scene
  changes, within 0 frames; 2 real scene changes"; G6: 5 and 3; G2: 10 and 8. For G1 run 1 against run 2 it was run
  with and without `--observable config/native/observable.txt` (same result).
- `tools/compare_ipc_traces.py --strict` (every word of every request): "all threads match" on all five pairs. G1:
  9 threads, 16,092 requests in the main thread and 2, 294, 168, 3, 576, 39, 31 and 1,727 in the others; G2: 13
  threads, 84,634 requests in the main thread (the others: 2, 1,544, 563, 224, 3, 16, 576, 65, 65, 151, 147 and 9,078).
  This is the comparison that reported differing reply words on the N0-26 runner.
- `tools/compare_svc_traces.py --normalize --outputs`: "all threads match" on all five pairs. G1: 21 threads, 1,017,886
  calls in the main thread; G6: 3,564,667 in the main thread; G2: 25 threads, 6,687,785 calls in the main thread.

**Compared with iteration 0** (the N0-26 runner, the Windows CI build at `6fa80b1`, two runs of each of G1, G6 and
G2, one pair per session; the table in "Sources found by N0-29, iteration 0" above). The state, SVC and timing
traces, the unmapped-access count, the HID and DSP captures, every screenshot and the GPU capture's manifest and
reference images were identical in iteration 0 and are identical now, so they do not appear below.

| What differed in iteration 0 | Iteration 0 (3 pairs) | This runner (5 pairs: G1 x3, G6, G2) |
|---|---|---|
| Reply words of five kinds of IPC request: `APT:A` 0x01010000, `cfg:u` 0x00030040 and 0x00020000, `y2r:u` 0x00280000, and in G2 the CRC of the `fs:USER` write of the save | 7, 15 and 18 lines (G1, G6, G2) | none differs |
| GPU capture file (`.mgpu`) | 8 bytes (G1, G2) | none differs (144,248,896 and 145,694,492 bytes) |
| Config savegame in the user folder | 16 bytes, every session | none differs |
| Extdata metadata file in the user folder | 1 byte (G1, G6) | none differs |
| The game's save file in the user folder (G2) | 16 bytes | none differs (81,408 bytes) |
| Runner's log | every line, every session | differs (SHA-256 of the file), as predicted and left out by D49 |

What this shows: with the runner of this iteration, in the five pairs of the three gate sessions, including a
G1 run under the load of a second runner, no artifact compared differs except the runner's own log. It shows
the result of the whole set of changes: the contributions of the individual fixes were not separated.

**Not measured**
- The contribution of each fix (they were applied together: the ID, the zeroed reply bytes, the metadata padding).
- The runner's own time zone setting on its own (the script set `TZ=UTC0` too).
- The cost of zeroing the 256-byte command buffer for every HLE request (no `--perf-log` run; `arm_recomp.cpp` is not
  changed, so no `--perf-log` comparison is due).
- The metadata fix in `ObsoletedCreateExtSaveData` (no session calls command 0x0830).
- Other machines or operating systems (the Linux and macOS runners), and a second run of the measurement on another
  day.
- `--cpu jit`; `--async-io`, rooms and `--new-console-id` (they keep Azahar's behaviour).
- Sessions with a save or quests (G3, G4, G7).
- A user folder that already has a config savegame (it keeps its ID).
- A user folder other than the one copied for every run: that one has no `nand/rw/sys/LocalFriendCodeSeed_B`, the
  case in which Azahar draws the friend code seed too (a folder with that file was not tried).
- Runs longer than 9000 frames.
- The app (it does not use these switches).
- A load other than one second runner (G1 only); G6 and G2 were measured on an idle PC.

## With Azahar's normal interface

Built with `-DENABLE_QT=ON` (and the same `-DMH4U_PORT_DIR`), Azahar's Qt frontend runs a game on recompiled code when it finds a code library for it: either the path in the `MH4U_CODE` environment variable, or `recomp/<program id>.so` (`.dll` on Windows) in Azahar's user directory, e.g. `recomp/0004000000126300.so` for MH4U. Everything else (settings, controls, multiplayer rooms, texture packs, screen layouts) is Azahar's. If the loaded code isn't the code that was recompiled (another version), the backend logs it and interprets everything instead.

## Licence

GPL-3.0-or-later, like the rest of the repository ([LICENSE](../LICENSE)) and like Azahar, whose core the executables contain. The Windows workflow publishes builds only with that licence and the matching source (this repository at the built commit, Azahar at `azahar/COMMIT` plus `azahar/azahar.patch`).
