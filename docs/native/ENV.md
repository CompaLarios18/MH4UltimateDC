# Cloud development environment

`tools/native/setup_env.sh` makes a fresh cloud container ready for native-port work. It is idempotent, runs as root without `sudo` and without a terminal, and writes outside the system package directories only under `$HOME`. Start it in the background and wait for its last line:

```bash
mkdir -p build/logs && nohup bash -c 'bash tools/native/setup_env.sh --with-azahar; echo "setup exit=$?"' > build/logs/setup_env.log 2>&1 &
tail -3 build/logs/setup_env.log     # wait for "setup_env: done"
source ~/.mh4n_env
```

## Flags

| Flag | Effect |
|---|---|
| (none) | apt packages, Python packages, SDL3, `~/.mh4n_env` |
| `--no-sdl` | skip the SDL3 clone, build and install |
| `--with-azahar` | also fetch Azahar's sources at `port/azahar/COMMIT` into `~/azahar` (read only; the patch is not applied) |
| `--azahar-only` | only the Azahar step |
| `--help` | print the flags |

The last line is `setup_env: done` only when every step succeeded; any failing step exits non-zero without it.

## Steps

1. **apt** (skipped when every package is already installed; waits up to 10 minutes for the dpkg lock via `DPkg::Lock::Timeout`): `clang lld llvm cmake ninja-build pkg-config ccache libvulkan-dev vulkan-tools mesa-vulkan-drivers vulkan-validationlayers glslang-tools spirv-tools python3-pip libclang-rt-18-dev`, and the headers SDL3 needs to build without a display: `libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libasound2-dev libpulse-dev` plus `libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev libxss-dev libxtst-dev`. The second group is an addition to the spec: SDL3's X11 backend refuses to configure without Xcursor (and wants the other X extension headers), so the first group alone fails. `vulkan-validationlayers` (1.3.275 on the cloud image, N2-25) is the Khronos validation layer that `tools/native/vk_layer_run.py` runs the RHI tests under (`docs/native/rhi.md`); the harness fails, it does not skip, when the layer is missing. `libclang-rt-18-dev` (`1:18.1.3-1ubuntu1` on Ubuntu 24.04, N2-29) is clang 18's runtime library set (`libclang_rt.tsan-x86_64.a` among them): without it `clang -fsanitize=thread` fails to link (the cloud image had `clang` 18.1.3 but not this package), and the `native-tsan` job of `.github/workflows/native.yml` and the ThreadSanitizer build of `docs/native/rhi.md` need it. It goes with clang 18, the `clang` of Ubuntu 24.04; another compiler version needs the package of its own version (`libclang-rt-<N>-dev`).
2. **pip**: `pip install --break-system-packages -r requirements.txt`.
3. **SDL3 3.4.18**: shallow clone of tag `release-3.4.18` into `~/.cache/mh4n/SDL`, CMake + Ninja with clang (`-DSDL_TESTS=OFF -DCMAKE_BUILD_TYPE=Release`, `-j$(nproc)`), installed into `~/.local`. Skipped when `pkg-config` already reports 3.4.18 there.
4. **`~/.mh4n_env`**: exports `CC=clang`, `CXX=clang++`, `PKG_CONFIG_PATH`, `CMAKE_PREFIX_PATH` and `LD_LIBRARY_PATH` for `~/.local`; the script sources it.
5. **Azahar** (`--with-azahar` / `--azahar-only`): `git init`, `remote add origin`, `fetch --depth 1 origin <sha>`, `checkout FETCH_HEAD`, `submodule update --init --recursive --depth 1`, then checks `HEAD` equals the pinned commit.
6. **Report**: one line per tool (clang, cmake, ninja, glslangValidator, whether `clang -fsanitize=thread` links, SDL3, Python packages) and the Vulkan devices (`llvmpipe` in a container).

## Measured times

Measured on a 4-core, 16 GB container (the spec assumes 2 cores and 7 GB, so expect roughly double on that size), Ubuntu 24.04 packages, with apt packages already present:

| Run | Time |
|---|---|
| Fresh `HOME` (clone and build SDL3, pip, env file) | 48 s |
| Second run, same `HOME` | 1.7 s |
| `--azahar-only` on an existing checkout at the pinned commit | 1 min 55 s (the recursive submodule update dominates) |

apt itself was not timed from scratch: the container already had most packages.

## Checks

```bash
source ~/.mh4n_env && pkg-config --modversion sdl3    # 3.4.18
echo $CXX                                              # clang++
vulkaninfo --summary | grep -i llvmpipe
python3 -m pytest tests/test_native_setup_env.py
```
