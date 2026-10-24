# Windows / x86-64 port (branch `windows`)

Goal: Tears of the Kingdom and Breath of the Wild built from the player's own files into a native Windows program that
holds a steady 30 FPS at Switch-docked quality on 13-16 W handhelds (ROG Ally X, ROG Ally, OneXPlayer 3), on AMD,
Intel and NVIDIA GPUs.

The full plan and the optimization catalog live outside the repository; this file records the decisions the code
depends on.

## What changes from the Mac port

| Part | Mac (upstream) | Windows (this branch) |
| --- | --- | --- |
| Game code | ARM64, runs as it is after `patch_nso.py` | Recompiled ahead of time to x86-64 on the player's machine |
| Host calls | Imports resolve to host C functions (same ABI) | Imports resolve to guest stubs `svc #(0x1000+i); ret`; a generated adapter moves AAPCS64 registers into a host C call (`bridge/`) |
| Graphics | NVN on Metal 4 | NVN on Vulkan 1.3 |
| Shaders | Ryujinx -> SPIR-V -> SPIRV-Cross -> MSL | Ryujinx -> SPIR-V, used by Vulkan directly |
| ASTC textures | Native on Apple GPUs | Transcoded to BC7 once, at build time |
| Window, input, audio | AppKit, GameController, Core Audio | SDL3, WASAPI |

Everything that can be translated is translated at build time. While the game runs, the only layer between the game
and the hardware is NVN -> Vulkan.

## Rules this branch follows

- New code lives in new directories (`games/totk/win/`, later `gfx/vk/` and a `platform/` layer). Shared upstream files
  change only where a seam is needed, so `main` can be merged regularly.
- No game content in the repository, as upstream requires.
- Guest addresses equal host addresses: the guest's ranges are reserved below 2^39 at the same host addresses, so the
  host services written for the Mac read guest pointers unchanged.

## Departures from upstream's CONTRIBUTING.md (cannot go upstream)

- **CPU translation.** Upstream forbids JIT, dynamic translation and CPU emulators: on Apple silicon the code runs as
  it is. On x86-64 it cannot, so this branch recompiles it ahead of time. Code the static pass misses is compiled on
  first use and recorded, so the next build compiles it ahead of time (the goal for a release: no JIT at run time).
- **Licence.** The recompiler planned for this (ScarletNX's `libscarletnx-recompiler`) is GPL-3.0. It stays an external
  dependency fetched at build time and is never copied into this MIT repository; a program linked with it is
  distributed under GPL-3.0. The bridge talks to it only through `bridge/bridge_abi.h`, so the recompiler can be
  replaced.

## Recompiler check (2026-10-24, ScarletNX 5889bb1)

Verified in its source and with `probes/recomp_e2e` (ARM64 code recompiled and run against the same C compiled
natively):

| Claim | Result |
| --- | --- |
| LLVM, ahead of time, code for the host CPU (AVX2, AVX-512 detected) | Yes |
| Exclusives without a global lock | Yes: `STXR` is a seq_cst compare-and-swap of the loaded value; `STXP` a 16-byte `cmpxchg16b` (their fix for TotK's lock-free lists) |
| Guest addresses = host addresses (`cpu.mem = NULL`) | Works in the probe. Their own Switch runtime uses a base pointer, so this mode is ours to keep tested |
| Barriers | Every `DMB`/`DSB`/`ISB` is a seq_cst fence (`mfence`), also `DMB ISHLD`/`ISHST`, which x86 does not need. Not measured yet |
| `WFE` / `YIELD` | `WFE` is a hook the runtime supplies (ours: `WaitOnAddress`); `YIELD` is `pause` |
| Code the static pass misses | Compiled on first use, recorded as a hint for the next ahead-of-time build |
| Windows | COFF linker and loader in the source; the library was built and tested on Linux only here |
| Integer code | 1.04-1.12x native |
| Host calls through the bridge | about 3-8 ns over a native call |
| Floating point | Exact ARM semantics, **5.5-7x slower** than native on a vectorised loop; a relaxed-FP experiment brings it to about 1x, bit-identical on that loop (details in `probes/recomp_e2e/README.md`) |
| Licence | GPL-3.0-or-later; parts ported from QEMU (LGPL-2.1-or-later) |

Relaxed floating point is the largest single performance item found so far for the 13-16 W target.

## Status

| Step | State | Tested by |
| --- | --- | --- |
| Bridge generator for NVN (`bridge/gen_bridge.py`) | done: 472 of 477 functions | `tests/test_win_bridge.py` |
| Host-function bridge (`bridge/gen_host_bridge.py`) | done: every `*_override` / `runtime_observer` entry of the boot set (135) from Clang's AST | boot test |
| Recompiler check (ScarletNX) | done | `probes/recomp_e2e` |
| NSO -> load manifest (`tools/nso_manifest.py`) | done: RELA, JMPREL, RELR, DT_INIT/DT_FINI, eh_frame_hdr hints | `tests/test_win_loader.py` |
| Guest memory, module loader | done: host implementations first (as the Mac loader), one stub per host function | Linux, and the Windows build under Wine |
| Upstream runtime on the port (`compat/`) | done: 20 runtime files build unchanged on Linux and for Windows (MinGW) | thread probe on Linux and under Wine; boot test |
| Threads, TLS, SVCs, host <-> guest calls, boot (`runtime/host_runtime.c`) | done | boot test: `BOOTTEST ok=7f` |
| M0 platform: NVN call trace, no window, silent audio (`runtime/platform_m0.c`) | done | boot test |
| Prebuilt Windows package (`package/`): `totk-win.exe` cross-built with llvm-mingw against a static LLVM 20 and ScarletNX for Windows, a portable CPython for the extraction scripts, `First-Run.cmd` | done | boot test of the `.exe` under Wine (recompile, then the code cache); upstream container tests and `nso_manifest` output on the bundled Python under Wine |
| **First boot on real TotK modules** | **needs the player's dump: `First-Run.cmd` (Windows) or `tools/first_run.sh` (Linux)** | |

The synthetic modules (`tests/fixtures`) are C written for the tests, linked like Horizon modules (MOD0, three
page-aligned segments, LZ4-compressed NSO): no game content. The boot set (`bootsdk.c`, `bootmain.c`) walks the SDK's
startup: `nn::init::Start` with the host's callbacks, module constructors, GetInfo / SetHeapSize / CreateThread /
StartThread / WaitSynchronization / ExitThread through the upstream dispatcher, a host replacement and the NVN entry.

## How the upstream runtime runs here

The Mac runtime (`games/totk/runtime`) is compiled as it is. `compat/win_compat.h` is force-included and
`compat/include` (and `compat/include_win` on Windows) stands in for the Apple and POSIX headers it uses: Mach VM calls
over guest_mem, CommonCrypto SHA-256 over CNG, CoreFoundation's locale, AudioToolbox's output unit (a silent clock in
M0), Darwin's `stat` fields, `struct tm` with `tm_gmtoff`, `pread`, `flock`, directories with `d_type`. One finding:
the runtime treats pthread key 0 as "not created" (Darwin never returns 0); `compat.c` takes key 0 at start.

What the Mac does in `loader.c`, `hle_stack.c`, `thread_enter.S`, `frontend.m` and the Metal layer is in `runtime/`:
`host_runtime.c` (loading, CPU state and Horizon TLR per thread, SVC dispatch, x86 thunks for host code that calls
game functions, module memory calls, the boot), `thread_enter.c` (thread entry / ExitThread without unwinding through
recompiled frames), `platform_m0.c` (the M0 platform).

## First run (M0: boot without graphics)

On Windows: unpack the package (`package/make_package.sh OUT_DIR` builds it on Linux) and double-click
`First-Run.cmd`. It asks for the base game 1.0.0 (XCI or NSP) and `prod.keys` dumped from your own console, prepares
them with the bundled Python, boots and writes `first-run.log`. Nothing is installed; see `package/README.txt`.

The prebuilt program reads `romfs_config.h` / `shared_fonts_config.h` at start (`TOTK_WIN_RUNTIME_CONFIG`,
`runtime/game_config.c`) instead of compiling them in, so one `totk-win.exe` serves every dump; `shared_font.c` has a
two-line seam for it. `TOTK_WIN_TRACE_SVC=1` logs every Horizon SVC with its arguments and result.

On Linux x86-64 (or WSL2), building everything from source:

```sh
git clone -b windows https://github.com/IljaGrebel/MacNativeGames && cd MacNativeGames
games/totk/win/tools/first_run.sh /mnt/c/Games/TotK.xci /mnt/c/Switch/prod.keys
```

It installs the tools, builds the recompiler library at the pinned commit, extracts and verifies ExeFS, RomFS (about
17 GB, a few minutes) and the fonts with the upstream scripts, builds `totk-win` and boots. The log is
`games/totk/win-first-run.log`. The goal of M0: the game starts and the log shows its first NVN calls.

## Settings in the game's own menu

The port's settings (upscaler, frame generation, frame limit, language, device profile) will appear as entries in
TotK's own options menu, not in a separate overlay. The Mac port already reads and edits the game's UI archives
(`.blarc` / `.bflyt` / `.bflan`, `scripts/ultrawide_layout.py`, `scripts/make_romfs_patches.py`) and serves edited
RomFS files at run time; the new entries need rows in the options layout, their labels in every language's message
files, and code that handles them - the first target of the decompilation pipeline. Until the game runs, the
settings are a configuration file; an overlay exists only for debugging before the game's menu is reachable.

## Decompilation pipeline (from M2)

The recompiled game is the base and plays in full from the start. On top of it, functions move to native C++ one at
a time, each proven equivalent before it replaces the original:

1. A profile of the running game names the hottest functions; render submission first, then physics and loading.
2. The function is decompiled to C/C++.
3. A differential test runs the new function and the recompiled original on the same recorded inputs (the method of
   `probes/recomp_e2e`) and compares results and memory effects.
4. Only a match replaces the original. Render-submission functions may then talk to Vulkan directly, skipping NVN.

The share of native code grows for as long as the profile shows a gain; carried far enough this becomes a full
decompilation, with the game playable at every step. Decompiled code derives from the game's code: it stays on the
player's machine, generated from their own files like everything else, and never enters this repository.

Graphics features (upscalers, frame pacing, device profiles) and shader preparation do not depend on it: shaders are
Maxwell binaries in the RomFS, translated to SPIR-V at build time, with pipelines built once at first launch.

## The bridge (`bridge/`)

`gen_bridge.py` writes one adapter per host function from its C prototype (`nvn_protos.h` for NVN). On the current
headers it generates 472 of 477 NVN functions. The five it leaves for a person:

| Function | Why |
| --- | --- |
| `nvnDeviceGetProcAddress`, `nvnCommandBufferGetMemoryCallback` | Return a function pointer: the host must hand back a guest stub address, not a host address |
| `nvnBufferBuilderGetMemoryPool`, `nvnTextureBuilderGetMemoryPool`, `nvnTextureGetMemoryPool` | `nvn_protos.h` declares them as returning `NVNmemoryPool` by value; the NVN API returns a pointer. Harmless on the Mac (the host function is called directly), wrong through a bridge. Fix the prototype generator upstream |

Three generated functions take callbacks (`nvnDeviceInstallDebugCallback`, `nvnCommandBufferSetMemoryCallback`,
`nvnDeviceWalkDebugDatabase`); their host side must call the guest address through the recompiler.

Tests: `python -m unittest tests.test_win_bridge`. The register allocation is checked against clang targeting
aarch64 (standard AAPCS64, as on the Switch), and the generated C is compiled and run with a fake guest CPU.
