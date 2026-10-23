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

## Getting `mh4u-native.exe` (the Windows build)

The maintainer does not build it. `.github/workflows/native-app.yml` ("Native app CI", [`CI.md`](CI.md)) builds `mh4u-native` on Linux, Windows and macOS on every push to `native` and to a `wp/**` branch, and the Windows job publishes the artifact **`mh4u-native-windows`** (kept 30 days; pushes and dispatches only, never a pull request). To get it: on GitHub, **Actions** → **Native app CI** → the run the orchestrator names (a push, or a dispatch on the batch branch) → **Artifacts** at the bottom of the run's page → `mh4u-native-windows`. The orchestrator gives the run's link with every procedure that needs the executable.

The artifact is a zip of one folder with these files, all ours (no game data in it):

| File | What |
|---|---|
| `mh4u-native.exe` | the app (in this stage `--help` and `--version` only; the window comes with N2b-10) |
| `SDL3.dll` | SDL3 3.4.18, built from its release tag with llvm-mingw |
| `libc++.dll`, `libunwind.dll` | the two llvm-mingw runtime DLLs the programs import (the step "Package mh4u-native and its DLLs" prints each program's imports; Windows' own DLLs are not copied). With them the folder has six files, not five |
| `mh4u_native_runner.exe` | the headless runner (the same program as the one in `mh4n-runner-windows`, from a different build) |
| `mh4n_hdpack.exe` | the HD pack pre-processor, without debug information |

### Smoke test of the downloaded artifact (`pc`)

About **10 minutes**. It checks that the files in the zip are enough on their own: unpacked in a folder that is not on `PATH`, on a PC without llvm-mingw, each program starts and no "missing DLL" dialog appears. Run it for the first artifact of every batch that gives you one, and whenever the orchestrator asks. Use PowerShell. It comes before *Setting up mh4u-native on the reference PC* (N2b-11, not written yet). None of the steps needs the repository checkout: step 2's `Set-Location C:\NoJales\mh4pcport\repo` is only a starting folder, and if there is no checkout yet, skip that line (the zip comes from the browser, and everything else runs in the scratch folder).

1. **Download the zip** (2 min). Open the run the orchestrator named, scroll to **Artifacts**, click `mh4u-native-windows`. *Expect:* a file `mh4u-native-windows.zip` in `Downloads` (tens of MB: `mh4u-native.exe` and `mh4u_native_runner.exe` keep their debug information; `mh4n_hdpack.exe` is stripped). *Fails if:* the artifact is not listed (the run was a pull request, it is older than 30 days, or the Windows job failed): tell the orchestrator the run's link and stop.
2. **Unzip into a scratch folder** (1 min).
   ```
   Set-Location C:\NoJales\mh4pcport\repo
   $scratch = Join-Path $env:TEMP 'mh4u-native-smoke'
   Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
   Expand-Archive -Path "$HOME\Downloads\mh4u-native-windows.zip" -DestinationPath $scratch
   Get-ChildItem $scratch | Select-Object Name, Length
   ```
   *Expect:* the six files of the table above: `mh4u-native.exe`, `SDL3.dll`, `libc++.dll`, `libunwind.dll`, `mh4u_native_runner.exe`, `mh4n_hdpack.exe`, nothing else (if the run's log lists another llvm-mingw DLL, that one too). *Fails if:* a file is missing or the list has something else (send the list).
3. **Start a PowerShell with no compiler on the path** (1 min). Open a **new** PowerShell window (so it has your usual `PATH`), then in it:
   ```
   $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
   $scratch = Join-Path $env:TEMP 'mh4u-native-smoke'
   Set-Location $scratch
   ```
   *Expect:* no output. This only changes this window's `PATH`: it has the Windows folders and not llvm-mingw, Git, MSYS or the scratch folder's DLLs (the programs find those next to themselves). Close the window afterwards.
4. **`mh4u-native.exe --version`** (under 1 min).
   ```
   .\mh4u-native.exe --version; "exit $LASTEXITCODE"
   ```
   *Expect:* one line `mh4u-native <revision>` (the 10-character revision of the run's commit: a longer git hash starts with it, and a trailing `-dirty` is also fine; the orchestrator gives you the full hash with the run link) and `exit 0`, and **no dialog** ("The code execution cannot proceed because ... .dll was not found", or "was not found" in the console, or an exit code such as `-1073741515`). *Fails if:* a dialog or any of these appears: write down the DLL name it gives and do the last section.
5. **`mh4u-native.exe --help`** (under 1 min).
   ```
   $o = & .\mh4u-native.exe --help 2>&1; "exit $LASTEXITCODE"; $o | Select-Object -First 4
   ```
   *Expect:* the first line is `usage: mh4u-native [options]`, then `options:` and the first option lines; `exit 0`.
6. **`mh4u_native_runner.exe --help`** (under 1 min).
   ```
   $o = & .\mh4u_native_runner.exe --help 2>&1; "exit $LASTEXITCODE"; $o | Select-Object -First 3
   ```
   *Expect:* the first line starts `usage: mh4u_native_runner`; `exit 0`.
7. **`mh4n_hdpack.exe --help`** (under 1 min).
   ```
   .\mh4n_hdpack.exe --help; "exit $LASTEXITCODE"
   ```
   *Expect:* this tool has no `--help` option, so it says `mh4n_hdpack: unknown option --help`, then `usage: mh4n_hdpack PACK_DIR -o OUT [options]` and its option list, and `exit 2`. That is the correct result: a missing DLL gives a dialog or a different exit code instead, and no usage line.
8. **No dialog at any step** (0 min). *Expect:* none of steps 4 to 7 opened a message box. *Fails if:* one did.

**What you send back** (in the batch PR or the chat; text only, never screenshots or game files): the run's link, the output of step 2's `Get-ChildItem` list, and the output lines of steps 4 to 7 with their `exit` lines, or "all as expected".

**When a step fails:** stop there. Send the step's number, the text of the dialog or the console message (the DLL's name above all), and the last 50 lines of the run's step "The packaged programs start" from the Actions page (the same four checks, run by the job on `windows-latest`; if that step is green and your PC fails, the difference is your PC, and the DLL name is what the orchestrator needs). Do not copy DLLs from your llvm-mingw install by hand and call it fixed.

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
