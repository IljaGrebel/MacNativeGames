/* The Windows port's host runtime (host_runtime.c): loading, guest threads, SVC dispatch, host <-> guest calls, boot. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "bridge_abi.h"
#include "module_loader.h"

/* Registers a host function (before host_runtime_load). fn identifies it (the pointer the upstream override tables
   return) and may be NULL for functions told apart by name; adapter moves the guest's arguments. Returns its index. */
uint32_t host_runtime_register(const char *name, const void *fn, void (*adapter)(GuestCpu *c));
/* Platform functions the port adds (graphics, window, ...): called by host_runtime_load before the modules load. */
void host_runtime_register_platform(void);
/* The name of the host function the current SVC runs (for adapters shared by many names), or NULL. */
const char *host_runtime_calling(void);
/* The index of a registered host function by name, or -1; and the guest address of its stub. */
int host_runtime_index(const char *name);
uint64_t host_runtime_stub(uint32_t index);

/* Loads the .nxm modules (load order: main first), links them, recompiles their code. cache_dir may be NULL. */
int host_runtime_load(const char *const *paths, unsigned count, const char *cache_dir);
const Loader *host_runtime_loader(void);
/* Runs the game: the calling thread becomes the guest main thread and calls start_symbol (nn::init::Start) with the
   main thread handle and the host's three startup callbacks, on a main stack of the given size. */
int host_runtime_boot(const char *start_symbol, uint64_t main_stack_size);

/* Calls a guest function from host code with up to eight integer arguments; returns x0. */
uint64_t host_runtime_guest_call(uint64_t fn, const uint64_t *args, unsigned nargs);
/* An x86 function pointer that calls guest function guest_fn (for host code that calls game functions directly). */
void *host_runtime_thunk(uint64_t guest_fn, const char *name);
/* Committed, zeroed guest memory below 2^39 (64 KiB granules, never freed). */
uint64_t host_runtime_guest_alloc(uint64_t size);
void host_runtime_register_boot_callbacks(void);
