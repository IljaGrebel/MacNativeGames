/* Guest address space for the Windows port (also builds on Linux, where the tests run).
 *
 * Guest addresses equal host addresses: the guest's ranges are reserved at fixed host addresses below 2^39 (the SDK
 * encodes IPC buffer addresses in 39 bits), so the host services read guest pointers directly. A range is reserved
 * first (address space only, no memory), then committed in pieces as the guest needs them.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define GM_ADDRESS_LIMIT (1ull << 39)

enum { GM_NONE = 0, GM_READ = 1, GM_WRITE = 2, GM_EXEC = 4 };

/* The host page size the commit and protect calls round to (4 KiB on x86-64), and the reservation granularity
   (64 KiB on Windows, the page size elsewhere). */
size_t gm_page_size(void);
size_t gm_reserve_granularity(void);

/* Reserves [base, base+size) exactly, or fails (returns -1) if any part is taken. base and size: multiples of the
   reservation granularity; the range must end at or below GM_ADDRESS_LIMIT. */
int gm_reserve(uint64_t base, uint64_t size);
/* The lowest base at or above `from` where a reservation of `size` fits below `limit`, stepping by `align`; 0 if none.
   It only probes: call gm_reserve afterwards (another thread may take the range in between). */
uint64_t gm_find_free(uint64_t from, uint64_t limit, uint64_t size, uint64_t align);
/* Commits zero-filled memory with the given protection inside a reservation. */
int gm_commit(uint64_t addr, uint64_t size, unsigned prot);
int gm_protect(uint64_t addr, uint64_t size, unsigned prot);
/* Returns committed memory to the reservation (contents lost, address space kept). */
int gm_decommit(uint64_t addr, uint64_t size);
/* Releases a whole reservation made by gm_reserve. */
int gm_release(uint64_t base, uint64_t size);
/* The last error as text (thread-local, for logs). */
const char *gm_error(void);
