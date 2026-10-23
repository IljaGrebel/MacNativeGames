#!/bin/sh
# Builds the guest code for AArch64 (Switch-like: Cortex-A57, no LSE), extracts its .text and entry offsets, builds
# the host harness against the installed recompiler library and runs it. Linux x86-64; Clang/LLVM 20 or newer.
# usage: run.sh <ScarletNX recompiler install prefix> [llvm suffix, default -20]
set -eu
PREFIX=$1; V=${2:--20}
HERE=$(cd "$(dirname "$0")" && pwd); B=${BUILD:-$HERE/build}
mkdir -p "$B"
clang$V --target=aarch64-linux-gnu -mcpu=cortex-a57 -ffreestanding -fno-pic -O2 -c "$HERE/guest.c" -o "$B/guest_a64.o"
test -z "$(llvm-readelf$V -r "$B/guest_a64.o" | grep -v 'no relocations')" || { echo "guest code has relocations"; exit 1; }
llvm-objcopy$V -O binary --only-section=.text "$B/guest_a64.o" "$B/guest_a64.bin"
off() { llvm-nm$V "$B/guest_a64.o" | awk -v s="$1" '$3 == s { print $1 }'; }
cmake -S "$HERE" -B "$B/host" -G Ninja -DCMAKE_C_COMPILER=clang$V -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B/host" >/dev/null
"$B/host/recomp_e2e" "$B/guest_a64.bin" "$(off guest_calls)" "$(off guest_fp)" "$(off guest_int)"
