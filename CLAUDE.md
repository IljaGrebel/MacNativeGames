# CLAUDE.md

Fork of MacNativeGames (Tears of the Kingdom / Breath of the Wild running natively on Apple silicon). The work in this
fork is the **Windows / x86-64 port on branch `windows`**: the game's ARM64 code recompiled ahead of time to x86-64
(ScarletNX's `libscarletnx-recompiler`), the Mac runtime (`games/totk/runtime`) compiled unchanged on Windows, NVN on
Vulkan later. Targets: 13-16 W handhelds (ROG Ally / Ally X, OneXPlayer 3), AMD, Intel and NVIDIA GPUs.

The design record is `games/totk/win/README.md`: read it before changing anything under `games/totk/win/`.

## Rules

- No game content in the repository, ever (NSO, RomFS, keys, decompiled game code, shaders from the game). Everything
  derived from the player's dump is made on the player's machine.
- New code lives in `games/totk/win/` (later `gfx/vk/`, `platform/`). Upstream files change only for a seam, guarded
  so the Mac build is unchanged (example: `SHARED_FONT_ROWS_AT_RUN_TIME` in `runtime/shared_font.c`), so `main` can be
  merged regularly.
- This branch departs from upstream `CONTRIBUTING.md` (CPU translation; GPL-3.0 when linked with ScarletNX). It cannot
  go upstream. ScarletNX stays an external dependency at a pinned commit (`5889bb1`), never vendored.
- The player has only Windows and installs nothing: the deliverable is the prebuilt package
  (`games/totk/win/package/`), built on Linux.
- Every claim of "works" needs a test that ran. Say what was tested on what (synthetic modules, Wine, real game).

## Layout (`games/totk/win/`)

| Path | What |
| --- | --- |
| `bridge/` | `gen_bridge.py` (NVN adapters from prototypes, AAPCS64), `gen_host_bridge.py` (adapters for the runtime's `*_override` tables from Clang's AST) |
| `tools/` | `nso_manifest.py` (NSO -> `.nxm` load manifest), `prepare_game.py` (runs upstream extraction), `first_run.sh` (Linux/WSL path) |
| `runtime/` | `module_loader`, `guest_mem`, `recomp_host` (ScarletNX), `host_runtime.c` (threads, TLR, SVC dispatch, boot), `thread_enter.c`, `platform_m0.c`, `game_config.c` |
| `compat/` | force-included `win_compat.h` and stand-in Apple/POSIX headers so the Mac runtime builds unchanged |
| `package/` | `make_package.sh` (cross-builds `totk-win.exe` with llvm-mingw + static LLVM 20 + ScarletNX), `First-Run.ps1/.cmd`, `README.txt` |
| `probes/recomp_e2e/` | recompiler verification and performance; `snx-relaxed-fp.patch` (relaxed-FP experiment, not applied anywhere yet) |
| `tests/` | C test programs and synthetic Horizon modules (`fixtures/`: written for the tests, no game code) |

## Build and test (Linux x86-64)

Tools: clang/lld/llvm 20, cmake, ninja, python3 (+ `cryptography`, `lz4`), aarch64 clang target, Wine 64-bit,
x86_64-w64-mingw32-gcc (some Wine tests).

ScarletNX recompiler for the host (needed by most tests):

```sh
git clone https://github.com/scarletnx/ScarletNX && git -C ScarletNX checkout 5889bb1
cmake -S ScarletNX -B snx-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF \
  -DSNX_BUILD_3DS=OFF -DSNX_BUILD_VITA=OFF -DBUILD_TESTING=OFF -DCMAKE_C_COMPILER=clang-20 \
  -DCMAKE_CXX_COMPILER=clang++-20 -DSNX_LLVM_CONFIG=$(command -v llvm-config-20)
cmake --build snx-build --target scarletnx-recompiler
cmake -DCMAKE_INSTALL_PREFIX=$PWD/snx-install -P snx-build/host-tools/recompiler-library/cmake_install.cmake
```

If CPM cannot download xxhash/enet behind a proxy, clone them and pass `-DCPM_xxhash_SOURCE=` / `-DCPM_enet_SOURCE=`.

Tests (20, all must pass; the ones whose prerequisites are missing skip, so check the skip count):

```sh
SNX_RECOMPILER_PREFIX=$PWD/snx-install TOTK_WIN_PACKAGE_EXE=<pkg>/bin/totk-win.exe \
  python3 -m unittest -v tests.test_win_loader tests.test_win_bridge
```

The rest of `tests/` is upstream's; 18 of its tests fail off macOS and did so before this branch.

Windows package (first time: downloads llvm-mingw and LLVM source, builds LLVM for Windows, about 1.5 h on 2 cores;
cached in `$WORK`):

```sh
games/totk/win/package/make_package.sh OUT_DIR    # then: cd OUT_DIR/.. && zip -qr9 TotK-Win-M0.zip OUT_DIR
```

Under Wine the packaged exe is checked by `WindowsPackageTests`. Windows PowerShell does not run under Wine here, so
`First-Run.ps1` is only parsed (pwsh) and reviewed for Windows PowerShell 5.1.

## Status (2026-10-24)

- M0 (boot without graphics) implemented. Synthetic SDK startup passes on Linux and as the Windows `.exe` under Wine
  (`BOOTTEST ok=7f`, then from the code cache).
- Package `TotK-Win-M0.zip` (commit 42dd2fc) sent to the player. **Waiting for `first-run.log` from the first run on
  the real game** (never run on real TotK modules yet).

## Next

1. From the player's log: implement whatever SVCs / imports / services the real boot reaches until the first NVN
   calls (`TOTK_WIN_TRACE_SVC=1` traces SVCs; unresolved imports stop with their name). Rebuild and resend the package.
2. M1: NVN on Vulkan 1.3 (`gfx/vk/`), SDL3 window and input, WASAPI audio, frame limiter, ASTC -> BC7 at build time.
3. Relaxed FP in ScarletNX (`probes/recomp_e2e/snx-relaxed-fp.patch`, 5.5-7x on FP loops), barrier cost, BotW
   preemption (`poll` hook).
4. Port settings as entries in the game's own options menu (upscalers FSR/XeSS/DLSS, frame generation, language),
   via RomFS UI layout edits and the decompilation pipeline (README, from M2).

## Pitfalls found so far

- The runtime treats pthread key 0 as "not created" (Darwin never returns it): `compat.c` takes key 0 at start.
- Win64: after `__builtin_longjmp` a register holding a thread-local address is not restored; `thread_enter.c` reads
  thread-local state in a separate `noinline` function.
- Guest = host addresses below 2^39: the Windows exe links with `--disable-high-entropy-va`.
- CMake drops repeated `-include` flags: use `"SHELL:-include file"`.
- llvm-mingw's Clang (21) is newer than the LLVM linked (20): ScarletNX needs `-DSNX_LLVM_AS=` an llvm-as 20.
- Static LLVM: `find_package(LLVM)` before ScarletNX's package and link `totk-win` with the C++ driver.
- llvm-mingw defaults to the Windows 7 API: `_WIN32_WINNT=0x0A00`.

## External documents

- Plan: https://claude.ai/artifact/WLny8QXsdY7E7z593YDAym
- Optimization catalog for handhelds: https://claude.ai/artifact/Msq3WskAsgVyYhabxAsGTt
