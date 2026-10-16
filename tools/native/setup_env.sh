#!/usr/bin/env bash
# N0-02: make a fresh cloud container ready for native-port work.
# Idempotent; runs as root without sudo and without a terminal.
# Everything outside the system package directories lives under $HOME.
set -euo pipefail

SDL_TAG="release-3.4.18"
SDL_VERSION="3.4.18"
SDL_URL="https://github.com/libsdl-org/SDL"
AZAHAR_URL="https://github.com/azahar-emu/azahar.git"

APT_PACKAGES=(
  clang lld llvm cmake ninja-build pkg-config ccache
  libvulkan-dev vulkan-tools mesa-vulkan-drivers glslang-tools spirv-tools python3-pip
  libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libasound2-dev libpulse-dev
  libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev libxss-dev libxtst-dev
)

usage() {
  cat <<'USAGE'
Usage: tools/native/setup_env.sh [--no-sdl] [--with-azahar] [--azahar-only] [--help]

Prepares a cloud container for native-port work:
  apt packages, Python packages (requirements.txt), SDL3 3.4.18 built into ~/.local,
  and ~/.mh4n_env (CC, CXX, PKG_CONFIG_PATH, CMAKE_PREFIX_PATH, LD_LIBRARY_PATH).

Flags:
  --no-sdl        skip the SDL3 clone/build/install
  --with-azahar   also fetch Azahar's sources at port/azahar/COMMIT into ~/azahar
  --azahar-only   only fetch Azahar's sources (quick to run again)
  --help          print this text

The last line is "setup_env: done" only when every step succeeded.
USAGE
}

NO_SDL=0; WITH_AZAHAR=0; AZAHAR_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --no-sdl) NO_SDL=1 ;;
    --with-azahar) WITH_AZAHAR=1 ;;
    --azahar-only) AZAHAR_ONLY=1 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "setup_env: unknown flag: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JOBS="$(nproc)"
CACHE="$HOME/.cache/mh4n"
PREFIX="$HOME/.local"
ENV_FILE="$HOME/.mh4n_env"

log() { echo "setup_env: $*"; }

step_apt() {
  local missing=() p
  for p in "${APT_PACKAGES[@]}"; do
    dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
  done
  if [ "${#missing[@]}" -eq 0 ]; then
    log "apt: all packages already installed"
    return
  fi
  log "apt: installing ${missing[*]}"
  export DEBIAN_FRONTEND=noninteractive
  # DPkg::Lock::Timeout waits for the dpkg lock instead of failing
  apt-get -o DPkg::Lock::Timeout=600 update
  apt-get -o DPkg::Lock::Timeout=600 install -y --no-install-recommends "${missing[@]}"
}

step_pip() {
  log "pip: requirements.txt"
  python3 -m pip install --break-system-packages -q -r "$REPO_ROOT/requirements.txt"
}

step_sdl() {
  if PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}" \
       pkg-config --exact-version="$SDL_VERSION" sdl3 2>/dev/null; then
    log "sdl3: $SDL_VERSION already installed"
    return
  fi
  mkdir -p "$CACHE" "$PREFIX"
  local src="$CACHE/SDL"
  if [ ! -d "$src/.git" ]; then
    log "sdl3: cloning $SDL_TAG"
    rm -rf "$src"
    git clone --depth 1 --branch "$SDL_TAG" "$SDL_URL" "$src"
  fi
  log "sdl3: building with -j$JOBS"
  CC=clang CXX=clang++ cmake -S "$src" -B "$src/build" -G Ninja \
    -DSDL_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
  cmake --build "$src/build" -j"$JOBS"
  cmake --install "$src/build"
}

step_env_file() {
  cat > "$ENV_FILE" <<ENVEOF
# Written by tools/native/setup_env.sh
export CC=clang
export CXX=clang++
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig\${PKG_CONFIG_PATH:+:\$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$PREFIX\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
ENVEOF
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  log "wrote $ENV_FILE"
}

step_azahar() {
  local dir="$HOME/azahar" commit
  commit="$(tr -d '[:space:]' < "$REPO_ROOT/port/azahar/COMMIT")"
  mkdir -p "$dir"
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" = "$commit" ]; then
    log "azahar: already at $commit"
  else
    log "azahar: fetching $commit"
    [ -d "$dir/.git" ] || git -C "$dir" init -q
    git -C "$dir" remote get-url origin >/dev/null 2>&1 || git -C "$dir" remote add origin "$AZAHAR_URL"
    git -C "$dir" fetch --depth 1 origin "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
  fi
  git -C "$dir" submodule update --init --recursive --depth 1
  local head
  head="$(git -C "$dir" rev-parse HEAD)"
  if [ "$head" != "$commit" ]; then
    echo "setup_env: azahar HEAD $head does not match pinned $commit" >&2
    return 1
  fi
}

report() {
  log "versions:"
  local v
  v="$(clang --version | head -n1)"; echo "  clang: $v"
  v="$(cmake --version | head -n1)"; echo "  cmake: $v"
  v="$(ninja --version)"; echo "  ninja: $v"
  v="$(glslangValidator --version 2>&1 | head -n1 || true)"; echo "  glslangValidator: $v"
  v="$(pkg-config --modversion sdl3 2>/dev/null || echo 'not installed (--no-sdl?)')"; echo "  SDL3: $v"
  v="$(python3 -m pip list 2>/dev/null | grep -i -E '^(pyelftools|pytest|unicorn|capstone) ' | tr -s ' ' '=' | tr '\n' ' ')"
  echo "  python packages: $v"
  echo "  vulkan devices:"
  vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|deviceType' | sed 's/^ */    /' || echo "    (vulkaninfo found no devices)"
}

if [ "$AZAHAR_ONLY" -eq 1 ]; then
  step_azahar
  log "azahar: ok"
  echo "setup_env: done"
  exit 0
fi

step_apt
step_pip
if [ "$NO_SDL" -eq 0 ]; then step_sdl; fi
step_env_file
if [ "$WITH_AZAHAR" -eq 1 ]; then step_azahar; fi
report
echo "setup_env: done"
