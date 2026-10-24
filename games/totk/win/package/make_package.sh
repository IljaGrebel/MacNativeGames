#!/usr/bin/env bash
# Builds the prebuilt Windows package of totk-win on Linux: totk-win.exe cross-built with llvm-mingw against a static
# LLVM and ScarletNX's recompiler library, a portable CPython with the extraction scripts' wheels, the extraction
# scripts and First-Run.ps1. No game content: the player's dump is prepared on the player's machine.
#
#   games/totk/win/package/make_package.sh OUT_DIR
#
# Environment (all have defaults that download or build into WORK):
#   WORK          working folder (default ~/.cache/totk-win-package)
#   LLVM_MINGW    llvm-mingw toolchain folder (default: downloaded, release 20250924)
#   LLVM_WIN      LLVM for x86_64-w64-mingw32 with lib/cmake/llvm (default: built from 20.1.8, X86 only; hours)
#   SNX_SRC       ScarletNX checkout (default: cloned at SNX_COMMIT)
#   LLVM_AS       an llvm-as of LLVM 20 (default: llvm-as-20)
#   SNX_DEPS      folder with xxhash/ and enet/ clones for ScarletNX's CPM (default: CPM downloads them)
set -euo pipefail
OUT=${1:?usage: make_package.sh OUT_DIR}
HERE=$(cd "$(dirname "$0")/.." && pwd)          # games/totk/win
TOTK=$(cd "$HERE/.." && pwd)                     # games/totk
REPO=$(cd "$TOTK/../.." && pwd)
WORK=${WORK:-$HOME/.cache/totk-win-package}
SNX_COMMIT=5889bb1
LLVM_VERSION=20.1.8
MINGW_RELEASE=20250924
PY_RELEASE=20251014
PY_VERSION=3.12.12
mkdir -p "$WORK"
step() { printf '\n== %s\n' "$*"; }

step "llvm-mingw"
if [ -z "${LLVM_MINGW:-}" ]; then
  LLVM_MINGW=$WORK/llvm-mingw-$MINGW_RELEASE-ucrt-ubuntu-22.04-x86_64
  if [ ! -d "$LLVM_MINGW" ]; then
    curl -fL -o "$WORK/llvm-mingw.tar.xz" \
      "https://github.com/mstorsjo/llvm-mingw/releases/download/$MINGW_RELEASE/llvm-mingw-$MINGW_RELEASE-ucrt-ubuntu-22.04-x86_64.tar.xz"
    tar -xJf "$WORK/llvm-mingw.tar.xz" -C "$WORK"
  fi
fi
export PATH=$LLVM_MINGW/bin:$PATH
CROSS=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64
       -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++
       -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres -DCMAKE_AR="$LLVM_MINGW/bin/llvm-ar"
       -DCMAKE_RANLIB="$LLVM_MINGW/bin/llvm-ranlib" -DCMAKE_FIND_ROOT_PATH="$LLVM_MINGW/x86_64-w64-mingw32"
       -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
       -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH)

step "ScarletNX source ($SNX_COMMIT)"
if [ -z "${SNX_SRC:-}" ]; then
  SNX_SRC=$WORK/ScarletNX
  [ -d "$SNX_SRC" ] || git clone https://github.com/scarletnx/ScarletNX "$SNX_SRC"
  git -C "$SNX_SRC" checkout -q "$SNX_COMMIT"
fi

step "LLVM $LLVM_VERSION for Windows (static, X86)"
if [ -z "${LLVM_WIN:-}" ]; then
  LLVM_WIN=$WORK/llvm-win
  if [ ! -f "$LLVM_WIN/lib/cmake/llvm/LLVMConfig.cmake" ]; then
    src=$WORK/llvm-src
    if [ ! -d "$src/llvm" ]; then
      curl -fL -o "$WORK/llvm-src.tar.xz" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/llvm-project-$LLVM_VERSION.src.tar.xz"
      mkdir -p "$src" && tar -xJf "$WORK/llvm-src.tar.xz" -C "$src" --strip-components=1 \
        "llvm-project-$LLVM_VERSION.src/llvm" "llvm-project-$LLVM_VERSION.src/cmake" "llvm-project-$LLVM_VERSION.src/third-party"
    fi
    cmake -S "$src/llvm" -B "$WORK/llvm-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LLVM_WIN" \
      "${CROSS[@]}" -DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu -DLLVM_TARGETS_TO_BUILD=X86 \
      -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF \
      -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_BINDINGS=OFF \
      -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
      -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_TERMINFO=OFF \
      -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=clang;-DCMAKE_CXX_COMPILER=clang++"
    cmake -S "$SNX_SRC/scripts/llvm-libraries" -B "$WORK/llvm-libs-list" -DLLVM_BUILD="$WORK/llvm-build" \
      -DOUTPUT="$WORK/llvm-libraries.txt"
    cmake "$WORK/llvm-build" -DLLVM_DISTRIBUTION_COMPONENTS="$(cat "$WORK/llvm-libraries.txt");llvm-headers;cmake-exports"
    cmake --build "$WORK/llvm-build" --target install-distribution
  fi
fi

step "ScarletNX recompiler library ($SNX_COMMIT) for Windows"
SNX_WIN=$WORK/snx-win
# llvm-mingw's Clang is newer than the LLVM the recompiler links: its runtime-library bitcode is assembled from text
# with an llvm-as of that LLVM's major version (any 20.x reads 20.1.8's format)
LLVM_AS=${LLVM_AS:-$(command -v llvm-as-20 || echo /usr/lib/llvm-20/bin/llvm-as)}
if [ ! -f "$SNX_WIN/lib/libscarletnx-recompiler.a" ]; then
  deps=()
  if [ -n "${SNX_DEPS:-}" ]; then deps=(-DCPM_xxhash_SOURCE="$SNX_DEPS/xxhash" -DCPM_enet_SOURCE="$SNX_DEPS/enet"); fi
  cmake -S "$SNX_SRC" -B "$WORK/snx-win-build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CROSS[@]}" \
    -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_3DS=OFF -DSNX_BUILD_VITA=OFF -DBUILD_TESTING=OFF \
    -DSNX_A64_DISASM=OFF -DLLVM_DIR="$LLVM_WIN/lib/cmake/llvm" -DSNX_LLVM_AS="$LLVM_AS" "${deps[@]}"
  cmake --build "$WORK/snx-win-build" --target scarletnx-recompiler
  # the library's own install (the whole project's would also want scarletnx.exe, which a tools-only build lacks)
  cmake -DCMAKE_INSTALL_PREFIX="$SNX_WIN" -P "$WORK/snx-win-build/host-tools/recompiler-library/cmake_install.cmake" >/dev/null
  test -f "$SNX_WIN/lib/libscarletnx-recompiler.a"
fi

step "totk-win.exe"
BUILD=$WORK/totk-win-build
cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CROSS[@]}" \
  -DCMAKE_PREFIX_PATH="$SNX_WIN;$LLVM_WIN" -DLLVM_DIR="$LLVM_WIN/lib/cmake/llvm" -DTOTK_WIN_RUNTIME_CONFIG=ON \
  -DPython3_EXECUTABLE="$(command -v python3)" -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build "$BUILD" --target totk-win

step "portable Python $PY_VERSION and the extraction scripts' wheels"
PYDIR=$WORK/python-$PY_VERSION
if [ ! -f "$PYDIR/python.exe" ]; then
  rm -rf "$WORK/py-stage" && mkdir -p "$WORK/py-stage"
  curl -fL -o "$WORK/python.tar.gz" "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/cpython-$PY_VERSION+$PY_RELEASE-x86_64-pc-windows-msvc-install_only.tar.gz"
  tar -xzf "$WORK/python.tar.gz" -C "$WORK/py-stage"
  ( cd "$WORK/py-stage/python" && rm -rf -- *.pdb DLLs/*.pdb tcl include libs Lib/test Lib/idlelib Lib/tkinter \
      Lib/turtledemo DLLs/_tkinter.pyd DLLs/tcl*.dll DLLs/tk*.dll )
  python3 -m pip download -q --only-binary=:all: --platform win_amd64 --python-version 3.12 --implementation cp \
    -d "$WORK/wheels" 'cryptography==50.0.2' 'lz4==4.4.5'
  for w in "$WORK"/wheels/*.whl; do
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$w" \
      "$WORK/py-stage/python/Lib/site-packages"
  done
  mv "$WORK/py-stage/python" "$PYDIR"
fi

step "package in $OUT"
rm -rf "$OUT" && mkdir -p "$OUT/bin" "$OUT/prep/games/totk" "$OUT/licenses"
cp "$BUILD/totk-win.exe" "$OUT/bin/"
cp -r "$PYDIR" "$OUT/python"
P=$OUT/prep/games/totk
mkdir -p "$P/scripts" "$P/win/tools" "$P/win/tests/config" "$P/third_party/d2r"
cp "$TOTK"/scripts/{extract_exefs,extract_romfs,prepare_romfs_backend,extract_shared_fonts,prepare_shared_fonts}.py "$P/scripts/"
cp "$HERE"/tools/{prepare_game,nso_manifest}.py "$P/win/tools/"
cp "$HERE"/tests/config/shared_fonts_config.h "$P/win/tests/config/"
cp "$TOTK"/third_party/d2r/relr.py "$P/third_party/d2r/"
cp "$HERE"/package/First-Run.ps1 "$HERE"/package/First-Run.cmd "$OUT/"
cp "$HERE"/package/README.txt "$OUT/"
cp "$REPO/LICENSE" "$OUT/licenses/MacNativeGames-MIT.txt"
cp "$SNX_SRC/LICENSE" "$OUT/licenses/ScarletNX-GPL-3.0.txt"
cp "$WORK/llvm-src/llvm/LICENSE.TXT" "$OUT/licenses/LLVM-Apache-2.0-with-LLVM-exception.txt" 2>/dev/null || true
cp "$LLVM_MINGW/LICENSE.TXT" "$OUT/licenses/llvm-mingw.txt" 2>/dev/null || true
cp "$PYDIR/LICENSE.txt" "$OUT/licenses/Python.txt"
cat >"$OUT/licenses/SOURCES.txt" <<EOF
totk-win.exe is linked with ScarletNX's libscarletnx-recompiler (GPL-3.0-or-later) and is distributed under GPL-3.0.
Its complete corresponding source:
  MacNativeGames, branch windows, commit $(git -C "$REPO" rev-parse HEAD): https://github.com/IljaGrebel/MacNativeGames
  ScarletNX commit $SNX_COMMIT: https://github.com/scarletnx/ScarletNX
  LLVM $LLVM_VERSION: https://github.com/llvm/llvm-project/releases/tag/llvmorg-$LLVM_VERSION
  llvm-mingw $MINGW_RELEASE (toolchain, libc++/libunwind/mingw-w64 runtime): https://github.com/mstorsjo/llvm-mingw
Built with games/totk/win/package/make_package.sh. The Python packages' licences are in python/Lib/site-packages/*.dist-info.
EOF
( cd "$OUT" && find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt )
echo "package: $OUT ($(du -sh "$OUT" | cut -f1))"
