# port/

The PC port: [Azahar](https://github.com/azahar-emu/azahar)'s 3DS core (kernel, services, GPU, audio) with the game's recompiled code as the CPU. Architecture and plan: [`docs/PORT_ARCHITECTURE.md`](../docs/PORT_ARCHITECTURE.md).

| Path | What |
|---|---|
| `src/arm_recomp.*` | CPU backend (`Core::ARM_Interface`) that runs recompiled code: one host fiber per guest thread, system calls into Azahar's kernel, Azahar's page table as guest memory, cycle accounting through Azahar's timers |
| `src/fiber.*` | Host fibers (ucontext on Linux/macOS, Win32 fibers on Windows) |
| `src/headless.cpp` | Boots a 3DS executable with no window; for tests and comparing backends |
| `tests/svc_test.c` | Freestanding 3DS test program (threads, events, mutexes, atomics, VFP short vectors, Thumb) |
| `azahar/` | The Azahar commit this builds against, and a small patch: a hook to replace the CPU backend, and a hook to build this directory inside Azahar's tree |

## Building (Linux; Windows and macOS later)

```bash
# Azahar at the pinned commit, with the patch
git clone https://github.com/azahar-emu/azahar.git && cd azahar
git checkout $(cat ../MH4UltimateDC/port/azahar/COMMIT)
git submodule update --init --recursive --depth 1
git apply ../MH4UltimateDC/port/azahar/azahar.patch

# Test program (no game data needed)
python3 ../MH4UltimateDC/port/tests/build_svc_test.py -o ../MH4UltimateDC/build/port-test

# Configure and build (no Qt; software renderer)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DENABLE_QT=OFF -DENABLE_TESTS=OFF -DENABLE_ROOM=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_SCRIPTING=OFF \
  -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF -DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF \
  -DENABLE_SDL2=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DMH4U_PORT_DIR=$PWD/../MH4UltimateDC/port \
  -DMH4U_TEST_RECOMP_DIR=$PWD/../MH4UltimateDC/build/port-test \
  -DMH4U_RECOMP_DIR=$PWD/../MH4UltimateDC/build/recomp      # your own dump's recompiled code
cmake --build build --target mh4u_test_headless

# Same program, recompiled vs Azahar's JIT: the debug output must match
build/bin/Release/mh4u_test_headless ../MH4UltimateDC/build/port-test/svc_test.elf --cpu recomp
build/bin/Release/mh4u_test_headless ../MH4UltimateDC/build/port-test/svc_test.elf --cpu jit
```

## Licence

Azahar is GPL-3.0-or-later, so executables built from this directory are too.
