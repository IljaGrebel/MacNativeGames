#!/usr/bin/env bash
# First run of the Windows port's M0 (boot without graphics), on Linux x86-64 or WSL2 on Windows.
#
#   games/totk/win/tools/first_run.sh /path/to/TotK-1.0.0.xci /path/to/prod.keys [--skip-romfs]
#
# Installs the build tools (apt, Ubuntu 24.04), builds ScarletNX's recompiler library at a pinned commit, builds
# totk-win, prepares your own dump (prepare_game.py) and boots it. Everything stays in this checkout and in
# ~/.cache/totk-win; the log goes to games/totk/win-first-run.log. Send that log back.
set -euo pipefail

GAME=${1:?usage: first_run.sh GAME.xci|GAME.nsp prod.keys [--skip-romfs]}
KEYS=${2:?usage: first_run.sh GAME.xci|GAME.nsp prod.keys [--skip-romfs]}
EXTRA=${3:-}
SNX_COMMIT=5889bb1   # ScarletNX revision checked with games/totk/win/probes/recomp_e2e
HERE=$(cd "$(dirname "$0")/.." && pwd)          # games/totk/win
TOTK=$(cd "$HERE/.." && pwd)                     # games/totk
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/totk-win
LOG=$TOTK/win-first-run.log
mkdir -p "$CACHE"

step() { printf '\n== %s\n' "$*"; }

step "build tools"
if command -v apt-get >/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y -qq git cmake ninja-build clang-20 lld-20 llvm-20-dev python3 python3-venv pkg-config nasm \
    libzstd-dev zlib1g-dev
fi
CC=clang-20
command -v $CC >/dev/null || CC=clang
python3 -m venv "$CACHE/venv"
"$CACHE/venv/bin/pip" install -q cryptography lz4
PY="$CACHE/venv/bin/python"

step "ScarletNX recompiler library ($SNX_COMMIT)"
if [ ! -f "$CACHE/snx/lib/libscarletnx-recompiler.a" ]; then
  [ -d "$CACHE/ScarletNX" ] || git clone https://github.com/scarletnx/ScarletNX "$CACHE/ScarletNX"
  git -C "$CACHE/ScarletNX" fetch -q origin && git -C "$CACHE/ScarletNX" checkout -q "$SNX_COMMIT"
  LLVMCFG=$(command -v llvm-config-20 || command -v llvm-config)
  cmake -S "$CACHE/ScarletNX" -B "$CACHE/snx-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSNX_TOOLS_ONLY=ON \
    -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_3DS=OFF -DSNX_BUILD_VITA=OFF -DBUILD_TESTING=OFF \
    -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=${CC/clang/clang++} -DSNX_LLVM_CONFIG="$LLVMCFG"
  cmake --build "$CACHE/snx-build" --target scarletnx-recompiler
  cmake --install "$CACHE/snx-build" --prefix "$CACHE/snx" >/dev/null 2>&1 || true
  test -f "$CACHE/snx/lib/libscarletnx-recompiler.a"
fi

step "your dump: ExeFS, RomFS, fonts, module manifests"
"$PY" "$HERE/tools/prepare_game.py" "$GAME" --keys "$KEYS" $EXTRA

step "totk-win"
CONFIG="$TOTK/build"
[ -f "$CONFIG/romfs_config.h" ] || CONFIG="$HERE/tests/config"
cmake -S "$HERE" -B "$CACHE/totk-win-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$CC \
  -DCMAKE_PREFIX_PATH="$CACHE/snx" -DTOTK_WIN_CONFIG_DIR="$CONFIG" -DPython3_EXECUTABLE="$PY"
cmake --build "$CACHE/totk-win-build" --target totk-win

step "boot (log: $LOG)"
NXM="$TOTK/extracted/v0/nxm"
export TOTK_ROMFS="$TOTK/extracted/v0/romfs" TOTK_PROFILE_DIR="$CACHE/profile" TOTK_WIN_RECOMP_VERBOSE=1
echo "The first start recompiles the game's code for this CPU (can take 10-40 minutes); later starts load it from the cache."
mkdir -p "$TOTK_PROFILE_DIR"
set +e
( cd "$TOTK" && timeout 5400 "$CACHE/totk-win-build/totk-win" --cache "$CACHE/code-cache" \
    "$NXM/main.nxm" "$NXM/subsdk0.nxm" "$NXM/sdk.nxm" ) 2>&1 | tee "$LOG" | grep --line-buffered -E "recompil|units|\[totk-win\]|ENTER|FATAL" || true
rc=${PIPESTATUS[0]}
set -e
echo "exit code $rc" >>"$LOG"
step "result"
grep -E "\[totk-win\]|ENTER|RETURN|nninit|nnMain|\[nvn\] #(1|2|3|4|5) |FATAL|unimplemented|unresolved" "$LOG" | head -60 || true
echo
echo "NVN calls logged: $(grep -c '^\[nvn\]' "$LOG" || true)   exit code: $rc"
echo "Full log: $LOG"
