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
