# Native CI

The workflow is `.github/workflows/native.yml` ("Native CI"). It never uses game data: the runner is built and started without a game, and the tests use synthetic data. The existing `ci.yml` (pytest and the game-data guard on every PR) stays as it is.

## Triggers

| Trigger | What runs |
|---|---|
| `push` to `native` or to a `wp/**` branch | `tools`; `native`; `changes`, then `runner` if it says so |
| `pull_request` into `native` | `native`; `changes`, then `runner` if it says so (the Python tests are `ci.yml`'s job on PRs) |
| `workflow_dispatch` | `tools`, `native`, and `runner` always |

A newer push to a `wp/` branch (or to a PR) cancels the older run of the same ref; runs on `native` are never cancelled.

## Jobs

- **`tools`** (pushes and dispatch): the steps of `ci.yml`: Python 3.12, `clang lld llvm`, `pip install -r requirements.txt`, `python -m pytest -q`, and the guard that fails if `git ls-files` lists game data.
- **`native`** (N1-01; on every trigger, `ubuntu-24.04`): installs `clang lld llvm cmake ninja-build libvulkan-dev mesa-vulkan-drivers glslang-tools spirv-tools` and `pip install -r requirements.txt` (the ARM test programs are built with `tools/difftest.py`), then configures `native/` with the one configure line of `wp/N1.md`, builds, runs `ctest`, and runs `python3 tools/native/check_native_deps.py` (nothing under `native/` may use Azahar).
- **`changes`**: decides whether `runner` is needed, from the files changed under `port/`, `tools/`, `config/` and `.github/workflows/native.yml`:
  - dispatch: always;
  - PR: the files of the PR (`base...head`);
  - push to `wp/**`: the files changed since the merge base with `origin/native`;
  - push to `native`: the files of the pushed commits (`before..HEAD`); a new branch or a force push, where `before` is unusable, builds.
- **`runner`**: the reference runner, in the container image `linux.yml` uses (`opensauce04/azahar-build-environment:latest`).
  1. Azahar is cloned and checked out at `port/azahar/COMMIT`, submodules fetched, `port/azahar/azahar.patch` applied.
  2. ccache through `actions/cache`, keyed on the hash of `COMMIT` and the patch (plus the commit sha; the restore key is the hash alone).
  3. Configure with the flags of `port/README.md`'s Linux runner build (no Qt, no Vulkan or OpenGL, no SDL2, software renderer), build target `mh4u_runner`.
  4. Package `mh4u_runner`, every shared library `ldd` resolves except the C library's own, and `run.sh` (sets `LD_LIBRARY_PATH` to the bundled `lib/`) into `mh4u-runner-linux.tar.gz`; a step unpacks it and checks `run.sh --help` prints `usage: mh4u_runner`.
  5. Upload as artifact `mh4u-runner-linux`, kept 30 days.

## Fetching the runner (cloud session)

The newest runner that still has its artifact, for a branch (`<branch>` is `native` or a `wp/...` name). From a cloud session this needs the environment to allow the host GitHub serves artifacts from, `*.blob.core.windows.net` (the download fails with `Forbidden` otherwise; see `AGENTS.md` §7):

```bash
id=$(gh api 'repos/CompaLarios18/MH4UltimateDC/actions/artifacts?name=mh4u-runner-linux&per_page=50' \
  --jq '[.artifacts[] | select(.expired | not) | select(.workflow_run.head_branch == "<branch>")][0] | .workflow_run.id')
case "$id" in ""|null) echo "no runner artifact for <branch>"; exit 1 ;; esac
mkdir -p build/runner
gh run download "$id" -n mh4u-runner-linux -D build/runner
tar -xzf build/runner/mh4u-runner-linux.tar.gz -C build/runner
build/runner/run.sh --help        # prints the runner's usage
```

A push that changed none of the watched paths builds no runner, so the newest artifact of a branch may come from an earlier push; for a branch without one, use `native`'s.

## Building it when there is none

```bash
gh workflow run native.yml --ref native
gh run list --workflow native.yml --branch native --limit 1     # then: gh run watch <id>
```

GitHub accepts a dispatch for a workflow that is not on the default branch once it has run at least once. Dispatch always builds the runner. Artifacts expire after 30 days, so a quiet branch needs this now and then.

## Not covered

Windows and macOS builds (`windows.yml`, `macos.yml` on `main`; dispatch them with `ref` set to any branch). The game-data guard of N0-19 is its own workflow, `native-guard.yml`: it runs `tools/native/check_no_game_data.py --range` over the pushed or proposed commits, on the same triggers (pushes to `native` and `wp/**`, PRs into `native`). On a PR it runs the script from the PR's own checkout, so it is a check, not a barrier: review changes to `tools/native/check_no_game_data.py` and `.github/workflows/` like code that guards the repository.
