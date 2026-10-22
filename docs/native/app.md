# mh4u-native: the app tree

`mh4u-native` is the native port's app (D8, D12): a window of its own, the game on the native runtime and the app's ImGui UI over it. Its code is in `native/app/`, apart from the runner's and the tools', so that nothing under `native/src/` ever needs SDL3 or Dear ImGui. This page says how to build and run it and where its code lives; the CI part is N2b-27's, the rest comes with the later work packages of [`wp/N2b.md`](wp/N2b.md).

## Building it

SDL3 3.4.18 is a build dependency of the app only (`tools/native/setup_env.sh` installs it into `~/.local`; `~/.mh4n_env` makes CMake find it). The configure line is the runner's plus `-DMH4N_APP=ON` (default `OFF`: the runner, the tools and their tests never need SDL3), in a build folder of its own:

```
source ~/.mh4n_env
cmake -S native -B build/native-app -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON -DMH4N_APP=ON
cmake --build build/native-app
ctest --test-dir build/native-app --no-tests=error --output-on-failure
```

The executable is `build/native-app/bin/mh4u-native`. In this stage it only answers `--help` (the usage with every option the linked modules register in the `runner` scope, the runtime's and the app's) and `--version` (`mh4u-native <git revision>`); anything else exits with status 2 and the usage. N2b-10 replaces `main` with the call to the shell.

## Where the code lives

| Folder | What |
|---|---|
| `native/app/CMakeLists.txt` | the app tree's build: `mh4n_app_module`, the targets `mh4n_imgui` and `mh4n_inih`, and `mh4n::app` (every app module, every native module, SDL3, ImGui, inih) |
| `native/app/<path>/` | an app module: a `CMakeLists.txt` with `mh4n_app_module(SOURCES … [DEPENDS mh4n_app_x …] [THIRD_PARTY …])`, found at any depth except under `program/`, `tests/` and `third_party/`; its library is `mh4n_app_<path with _ for />` and is linked whole, so options and registrations in it work without being named |
| `native/app/shared/` | the one module that compiles the non-UI shared sources of `port/app/` (`config.cpp`, `input.cpp`, `kbm.cpp`, `invite.cpp`, `util/*.cpp`) and `native_rooms.cpp`, the stubs of `App::Multiplayer` and `Launcher::Lobby` (rooms are N4's: `Available()` is false and no rooms are listed). Later work packages add their own files in other modules and never compile these sources again |
| `native/app/host/` | `App::Host` (`port/app/app_window.h`) on SDL3: ticks, clipboard, cursor, mouse position, key names, message boxes, controllers, the window icon |
| `native/app/program/` | `mh4u-native`'s `main` |
| `native/app/tests/` | one doctest binary per `<name>_test.cpp`, linked against `mh4n::app`; `app_shared_test` reads a synthetic `mh4u-app.ini`, runs `App::Host` through SDL3's `dummy` video driver (no display) and checks the rooms stubs |
| `native/app/third_party/imgui/backends/` | Dear ImGui's SDL3 platform backend (`imgui_impl_sdl3.{cpp,h}`), the unmodified files of ImGui's tag `v1.92.9b`; the core files are the ones the Azahar app builds, in `port/app/third_party/imgui` |
| `native/third_party/inih/` | inih (`inih/ini.{c,h}`, `inih/cpp/INIReader.{cpp,h}`), laid out as Azahar's `externals/inih` |

The shared UI sources and the window of the Azahar app stay in `port/app/` and compile into both executables (rule 1 of `wp/N2b.md`); `tools/native/check_native_deps.py` checks `native/app/**` and every `port/app/` source a `CMakeLists.txt` there names: no Azahar include, no SDL2 include, no httplib or JSON include. It also keeps SDL and ImGui out of `native/src/`. SDL2 and SDL3 export the same function names, so nothing in `native/` links SDL2.

## Without a display

SDL3's `dummy` and `offscreen` video drivers need no display: set `SDL_VIDEO_DRIVER=dummy` (or `offscreen`) in the environment, or the hint `SDL_HINT_VIDEO_DRIVER` before `SDL_Init`, as `app_shared_test` does.
