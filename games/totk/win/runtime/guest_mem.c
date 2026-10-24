/* Guest address space: Windows (VirtualAlloc) and POSIX (mmap) implementations of guest_mem.h. */
#include "guest_mem.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define GM_TLS __declspec(thread)
#else
#define GM_TLS _Thread_local   /* C11; MinGW and clang-cl map it to the PE TLS directory */
#endif

static GM_TLS char gm_last_error[160];

static int fail(const char *what, uint64_t addr, uint64_t size) {
#if defined(_WIN32)
    snprintf(gm_last_error, sizeof gm_last_error, "%s %#llx+%#llx: Windows error %lu", what, (unsigned long long)addr,
             (unsigned long long)size, (unsigned long)GetLastError());
#else
    snprintf(gm_last_error, sizeof gm_last_error, "%s %#llx+%#llx: %s", what, (unsigned long long)addr,
             (unsigned long long)size, strerror(errno));
#endif
    return -1;
}

const char *gm_error(void) { return gm_last_error; }

static int bad_range(uint64_t addr, uint64_t size, uint64_t align) {
    return size == 0 || addr % align || size % align || addr + size < addr || addr + size > GM_ADDRESS_LIMIT;
}

#if defined(_WIN32)

size_t gm_page_size(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwPageSize; }
size_t gm_reserve_granularity(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwAllocationGranularity; }

static DWORD win_prot(unsigned prot) {
    /* Guest code is recompiled, never executed in place: no guest page needs PAGE_EXECUTE. */
    if (prot & GM_WRITE) return PAGE_READWRITE;
    if (prot & (GM_READ | GM_EXEC)) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

int gm_reserve(uint64_t base, uint64_t size) {
    if (bad_range(base, size, gm_reserve_granularity())) return fail("reserve: bad range", base, size);
    void *p = VirtualAlloc((void *)(uintptr_t)base, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    if (p == (void *)(uintptr_t)base) return 0;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    return fail("reserve", base, size);
}

uint64_t gm_find_free(uint64_t from, uint64_t limit, uint64_t size, uint64_t align) {
    if (limit > GM_ADDRESS_LIMIT) limit = GM_ADDRESS_LIMIT;
    for (uint64_t a = (from + align - 1) / align * align; a + size <= limit && a + size > a; a += align) {
        MEMORY_BASIC_INFORMATION mi;
        uint64_t p = a, ok = 1;
        while (p < a + size) {
            if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi) || mi.State != MEM_FREE) { ok = 0; break; }
            p = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        }
        if (ok) return a;
    }
    return 0;
}

int gm_commit(uint64_t addr, uint64_t size, unsigned prot) {
    if (bad_range(addr, size, gm_page_size())) return fail("commit: bad range", addr, size);
    if (!VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)size, MEM_COMMIT, win_prot(prot))) return fail("commit", addr, size);
    return 0;
}

int gm_protect(uint64_t addr, uint64_t size, unsigned prot) {
    DWORD old;
    if (bad_range(addr, size, gm_page_size())) return fail("protect: bad range", addr, size);
    if (!VirtualProtect((void *)(uintptr_t)addr, (SIZE_T)size, win_prot(prot), &old)) return fail("protect", addr, size);
    return 0;
}

int gm_decommit(uint64_t addr, uint64_t size) {
    if (bad_range(addr, size, gm_page_size())) return fail("decommit: bad range", addr, size);
    if (!VirtualFree((void *)(uintptr_t)addr, (SIZE_T)size, MEM_DECOMMIT)) return fail("decommit", addr, size);
    return 0;
}

int gm_release(uint64_t base, uint64_t size) {
    (void)size;
    if (!VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE)) return fail("release", base, size);
    return 0;
}

#else /* POSIX */

size_t gm_page_size(void) { return (size_t)sysconf(_SC_PAGESIZE); }
size_t gm_reserve_granularity(void) { return gm_page_size(); }

static int posix_prot(unsigned prot) {
    int p = 0;
    if (prot & (GM_READ | GM_EXEC)) p |= PROT_READ;
    if (prot & GM_WRITE) p |= PROT_READ | PROT_WRITE;
    return p ? p : PROT_NONE;
}

int gm_reserve(uint64_t base, uint64_t size) {
    if (bad_range(base, size, gm_reserve_granularity())) return fail("reserve: bad range", base, size);
#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap((void *)(uintptr_t)base, (size_t)size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#else
    void *p = mmap((void *)(uintptr_t)base, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (p == (void *)(uintptr_t)base) return 0;
    if (p != MAP_FAILED) munmap(p, (size_t)size);
    return fail("reserve", base, size);
}

uint64_t gm_find_free(uint64_t from, uint64_t limit, uint64_t size, uint64_t align) {
    if (limit > GM_ADDRESS_LIMIT) limit = GM_ADDRESS_LIMIT;
    for (uint64_t a = (from + align - 1) / align * align; a + size <= limit && a + size > a; a += align) {
        if (gm_reserve(a, size) == 0) { gm_release(a, size); return a; }
    }
    return 0;
}

int gm_commit(uint64_t addr, uint64_t size, unsigned prot) {
    if (bad_range(addr, size, gm_page_size())) return fail("commit: bad range", addr, size);
    /* a fresh anonymous mapping over the reservation: zero-filled, as committed Windows memory is */
    void *p = mmap((void *)(uintptr_t)addr, (size_t)size, posix_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != (void *)(uintptr_t)addr) return fail("commit", addr, size);
    return 0;
}

int gm_protect(uint64_t addr, uint64_t size, unsigned prot) {
    if (bad_range(addr, size, gm_page_size())) return fail("protect: bad range", addr, size);
    if (mprotect((void *)(uintptr_t)addr, (size_t)size, posix_prot(prot))) return fail("protect", addr, size);
    return 0;
}

int gm_decommit(uint64_t addr, uint64_t size) {
    if (bad_range(addr, size, gm_page_size())) return fail("decommit: bad range", addr, size);
    void *p = mmap((void *)(uintptr_t)addr, (size_t)size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (p != (void *)(uintptr_t)addr) return fail("decommit", addr, size);
    return 0;
}

int gm_release(uint64_t base, uint64_t size) {
    if (munmap((void *)(uintptr_t)base, (size_t)size)) return fail("release", base, size);
    return 0;
}

#endif
