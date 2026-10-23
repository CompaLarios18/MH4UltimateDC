# Evidence for `rhi-spike.md` 6.9: plume under the Vulkan validation layer

What this folder is: the program, the shaders, the patches and the result summary behind section 6.9 of [`../rhi-spike.md`](../rhi-spike.md) (N2-01, the RHI spike). It exists so that the numbers of 6.9 can be checked and repeated by someone who was not in the session. It holds no game data: it is a small Vulkan test program and seven patches to plume.

**Scope and limits (same as 6.9).** One run, on lavapipe (Mesa 25.2.8, LLVM 20.1.2, Vulkan 1.4.318) with `VK_LAYER_KHRONOS_validation` 1.3.275, on a machine with one queue and one memory type. Nothing was run on the RX 6700 XT or on MoltenVK. Lavapipe accepts invalid calls, so only the layer's messages show them. The effort figures quoted in 6.9 (tokens per patch) and "every patch compiled at the first build" are the experiment author's statement, not something these files show.

## Files

| Path | What |
|---|---|
| `test/main.cpp`, `test/CMakeLists.txt` | the test program (695 lines): patterns A1 to F, through plume's public API; `PLUME_PATCHED=ON` turns on the code that needs the patches |
| `shaders/*.vert`, `*.frag`, `build.sh` | the GLSL and the script that compiles it to the `test/gen/*.h` SPIR-V arrays (needs `glslangValidator`) |
| `patches/01` to `07` | seven patches to `renderbag/plume` at `0ce184b19270e49236afbb74052a6b267494b86b` (apply in order with `git apply`): 01 mutable format and zero-attachment passes, 02 fragment shader interlock, 03 per-wait stage mask, 04 `GENERAL` attachment layouts, 05 timeline semaphores, 06 pipeline cache and render-pass cache, 07 blend constants. 211 changed lines in all (`git diff --numstat`: 15, 19, 17, 9, 63, 76, 12) |
| `run.sh`, `summarize.py` | run one pattern with the layer on and write `logs/<label>-<pattern>.txt` and `.rc`; print one line per pattern |
| `env/`, `env-all/` | the layer settings used (`VK_LAYER_SETTINGS_PATH`): `env` limits duplicate messages to 3, `env-all` counts them all (the D2 counts) |
| `results-summary.txt` | `summarize.py` output of the original run, for the unpatched (`pristine`) and patched builds |
| `LICENSE-plume.txt` | plume's licence (MIT). The patches are changes to plume's files and keep it |
| `SHA256SUMS` | hashes of every file here, so 6.9 and section 13 of `rhi-spike.md` can quote them |

The test program, the scripts and the shaders are this repository's (GPL-3.0-or-later, D6). The patches modify plume (MIT, Copyright (c) 2024 renderbag and contributors).

## Repeating it

```
sudo apt-get install -y vulkan-validationlayers glslang-tools mesa-vulkan-drivers libvulkan-dev libx11-dev cmake ninja-build clang
git clone --recurse-submodules https://github.com/renderbag/plume plume
git -C plume checkout 0ce184b19270e49236afbb74052a6b267494b86b && git -C plume submodule update --init --recursive
sh shaders/build.sh                                       # writes test/gen/*.h
cp -r plume plume-patched
for p in patches/*.diff; do git -C plume-patched apply "$PWD/$p"; done       # in order
cmake -S test -B build-pristine -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DPLUME_SRC=$PWD/plume
cmake -S test -B build-patched  -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DPLUME_SRC=$PWD/plume-patched -DPLUME_PATCHED=ON
cmake --build build-pristine && cmake --build build-patched
mkdir -p logs
for p in A1 A2 B1 B2 B3 B4 B5 B6 B7 B8 C1 C2 C3 C4 C5 D1 D4 E F; do sh run.sh pristine build-pristine/plume_val $p; sh run.sh patched build-patched/plume_val $p; done
VAL_ENV=env-all sh run.sh pristine-all build-pristine/plume_val D2   # the race: thousands of messages, the count varies per run
python3 -I summarize.py pristine ; python3 -I summarize.py patched
```

Exit status of `plume_val`: 0 as expected, 1 pixels wrong, 2 setup failed, 3 bad usage. `D3` is the pattern with the owner rule (a mutex per texture around recording), which belongs to the program, not to a patch. Pass `LAYER_ENABLES=...` to change the layer's enables (the default is synchronization validation). `patched` builds use the compile definitions in `test/CMakeLists.txt`.
