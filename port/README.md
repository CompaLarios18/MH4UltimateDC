# port/

The PC port: [Azahar](https://github.com/azahar-emu/azahar)'s 3DS core (kernel, services, GPU, audio) with the game's recompiled code as the CPU. Architecture and plan: [`docs/PORT_ARCHITECTURE.md`](../docs/PORT_ARCHITECTURE.md).

| Path | What |
|---|---|
| `src/arm_recomp.*` | CPU backend (`Core::ARM_Interface`) that runs recompiled code: one host fiber per guest thread, system calls into Azahar's kernel, Azahar's page table as guest memory, cycle accounting through Azahar's timers, and a check that the loaded program is the one that was recompiled. Code that wasn't recompiled (a missed entry point, code made at run time) runs in Azahar's interpreter until it returns, and its address is logged for the next recompilation |
| `src/fiber.*` | Host fibers (ucontext on Linux/macOS, Win32 fibers on Windows) |
| `src/frontend.*` | `Port::PrepareBackendFor()`: called by a frontend before loading a game; installs the recompiled backend when there is a code library for that game. Azahar's Qt frontend calls it when built with this directory (see `azahar/azahar.patch`) |
| `src/runner.cpp` | `mh4u_runner`: boots a 3DS executable with no window, with the recompiled code (`--cpu recomp --code LIB`) or Azahar's own JIT/interpreter; for tests and for comparing backends |
| `tests/svc_test.c` | Freestanding 3DS test program (threads, events, mutexes, atomics, VFP short vectors, Thumb) |
| `azahar/` | The Azahar commit this builds against, and a small patch: a hook to replace the CPU backend, and a hook to build this directory inside Azahar's tree |

The runner contains no game code. The game's recompiled code is a separate shared library, generated and compiled on the user's machine from their own dump and loaded at run time.

## Building (Linux; Windows and macOS later)

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
# 1. Recompile your dump and build the code library (-O1 takes a while; -O0 is quick)
python3 tools/recompile.py --exheader baserom/update_v130/exheader.bin --code baserom/update_v130/code.bin -o build/recomp
(cd tools && python3 -m recomp.build ../build/recomp --code-library --opt O1)    # build/recomp/mh4u_code.so

# 2. Without the RomFS the game starts up and stops when it first opens its files, which
#    is enough to compare backends:
python3 tools/make_cxi.py --exheader baserom/update_v130/exheader.bin --exefs baserom/update_v130/exefs.bin -o build/mh4u_norom.cxi
../azahar/build/bin/Release/mh4u_runner build/mh4u_norom.cxi --code build/recomp/mh4u_code.so --cpu recomp --log "*:Info"
../azahar/build/bin/Release/mh4u_runner build/mh4u_norom.cxi --cpu jit --log "*:Info"
```

Everything under `build/` contains game code: it never goes into the repository.

## With Azahar's normal interface

Built with `-DENABLE_QT=ON` (and the same `-DMH4U_PORT_DIR`), Azahar's Qt frontend runs a game on recompiled code when it finds a code library for it: either the path in the `MH4U_CODE` environment variable, or `recomp/<program id>.so` (`.dll` on Windows) in Azahar's user directory, e.g. `recomp/0004000000126300.so` for MH4U. Everything else (settings, controls, multiplayer rooms, texture packs, screen layouts) is Azahar's. If the loaded code isn't the code that was recompiled (another version), the backend logs it and interprets everything instead.

## Licence

Azahar is GPL-3.0-or-later, so executables built from this directory are too.
