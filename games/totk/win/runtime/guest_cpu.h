/* The bridge's guest CPU is the recompiler's CPU state. Force-included (-include) into the generated bridge and the
   runtime, so bridge_abi.h sees GuestCpu as ScarletNX's CPU (x[32], v[32] with the same layout). */
#pragma once
#include <scarletnx/common/cpu/aarch64/cpu.h>
typedef CPU GuestCpu;
#define BRIDGE_GUEST_CPU_DEFINED 1
