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
