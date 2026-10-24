/* Implementations behind win_compat.h and the stand-in Apple headers in include/: Windows and POSIX. */
#include "win_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>

#include "guest_mem.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#include <sys/syscall.h>
#endif
#endif

/* ---- time, threads, randomness ---- */

/* The upstream runtime treats a pthread key of 0 as "not created yet" (Darwin never hands out key 0; glibc and
   winpthreads do). Take key 0 before anything else can, so every key the runtime creates is nonzero. */
__attribute__((constructor)) static void reserve_key_zero(void) {
    pthread_key_t k;
    if (pthread_key_create(&k, NULL) == 0 && k != 0) pthread_key_delete(k);
}

uint64_t compat_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((unsigned __int128)now.QuadPart * 1000000000u / (uint64_t)freq.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

uint64_t compat_thread_id(void) {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

/* A guest thread's stack when the runtime has one for the calling thread (host_runtime.c), else the host thread's. */
__attribute__((weak)) int host_runtime_guest_stack(uint64_t *lo, uint64_t *hi);

void *compat_stack_top(void) {
    uint64_t glo, ghi;
    if (host_runtime_guest_stack && host_runtime_guest_stack(&glo, &ghi)) return (void *)(uintptr_t)ghi;
#if defined(_WIN32)
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    return (void *)hi;
#else
    pthread_attr_t a;
    void *base = NULL;
    size_t size = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &base, &size); pthread_attr_destroy(&a); }
    return (char *)base + size;
#endif
}

size_t compat_stack_size(void) {
    uint64_t glo, ghi;
    if (host_runtime_guest_stack && host_runtime_guest_stack(&glo, &ghi)) return (size_t)(ghi - glo);
#if defined(_WIN32)
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    return (size_t)(hi - lo);
#else
    pthread_attr_t a;
    void *base = NULL;
    size_t size = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &base, &size); pthread_attr_destroy(&a); }
    return size;
#endif
}

void compat_random_bytes(void *p, size_t n) {
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) abort();
#elif defined(__linux__)
    for (size_t done = 0; done < n;) {
        ssize_t r = getrandom((char *)p + done, n - done, 0);
        if (r < 0 && errno != EINTR) abort();
        if (r > 0) done += (size_t)r;
    }
#else
    arc4random_buf(p, n);
#endif
}

void compat_locale(char *language, size_t language_size, char *region, size_t region_size) {
    char tag[86] = "";
#if defined(_WIN32)
    wchar_t w[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH) > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, sizeof tag, NULL, NULL);
#else
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LANG");
    if (e) snprintf(tag, sizeof tag, "%s", e);
#endif
    /* "ru-RU", "ru_RU.UTF-8", "zh-Hans-CN" -> language "ru"/"zh", region "RU"/"CN" */
    char *dot = strchr(tag, '.');
    if (dot) *dot = 0;
    for (char *c = tag; *c; c++) if (*c == '_') *c = '-';
    char *first = strchr(tag, '-'), *last = strrchr(tag, '-');
    if (first) *first = 0;
    snprintf(language, language_size, "%s", strcmp(tag, "C") && strcmp(tag, "POSIX") ? tag : "en");
    snprintf(region, region_size, "%s", last && strlen(last + 1) == 2 ? last + 1 : "");
}

int compat_fcntl(int fd, int cmd, ...) {
    if (cmd == F_FULLFSYNC) {
#if defined(_WIN32)
        return _commit(fd);
#else
        return fsync(fd);
#endif
    }
#if defined(_WIN32)
    errno = EINVAL;
    return -1;
#else
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
#undef fcntl
    return fcntl(fd, cmd, arg);
#endif
}

/* ---- memory queries ---- */

int compat_readable(uint64_t address, uint64_t size) {
    if (!size) return 1;
#if defined(_WIN32)
    uint64_t a = address, end = address + size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xEE))
            return 0;
        a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
#else
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int ok = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perm[5];
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perm) == 3 && address >= lo && address + size <= hi) {
            ok = perm[0] == 'r';
            break;
        }
    }
    fclose(f);
    return ok;
#endif
}

uint64_t compat_vm_page_size = 4096;

kern_return_t mach_vm_map(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, mach_vm_offset_t mask, int flags,
                          mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy, vm_prot_t cur,
                          vm_prot_t max, vm_inherit_t inh) {
    (void)t; (void)mask; (void)obj; (void)off; (void)copy; (void)max; (void)inh;
    if (cur != VM_PROT_NONE) return KERN_FAILURE;   /* the runtime only reserves address space this way */
    uint64_t g = gm_reserve_granularity();
    s = (s + g - 1) / g * g;
    if (!(flags & VM_FLAGS_ANYWHERE)) return gm_reserve(*a, s) == 0 ? KERN_SUCCESS : KERN_NO_SPACE;
    uint64_t at = gm_find_free(*a ? *a : 0x100000000ull, GM_ADDRESS_LIMIT, s, 2ull << 20);
    if (!at || gm_reserve(at, s)) return KERN_NO_SPACE;
    *a = at;
    return KERN_SUCCESS;
}

kern_return_t mach_vm_allocate(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, int flags) {
    if (mach_vm_map(t, a, s, 0, flags, 0, 0, 0, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT) != KERN_SUCCESS) return KERN_NO_SPACE;
    return gm_commit(*a, (s + 4095) & ~4095ull, GM_READ | GM_WRITE) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t mach_vm_deallocate(vm_map_t t, mach_vm_address_t a, mach_vm_size_t s) {
    (void)t;
    return gm_release(a, s) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

/* The first mapping at or after *a: its start and size (the runtime looks for free gaps between mappings). */
kern_return_t mach_vm_region(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t *s, vm_region_flavor_t flavor,
                             vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj) {
    (void)t; (void)flavor; (void)cnt;
    if (obj) *obj = 0;
    if (info) memset(info, 0, sizeof(vm_region_basic_info_data_64_t));
#if defined(_WIN32)
    uint64_t p = *a;
    MEMORY_BASIC_INFORMATION mi;
    while (VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) {
        uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress, end = base + mi.RegionSize;
        if (mi.State != MEM_FREE && end > *a) {
            *a = base > *a ? base : (uint64_t)(uintptr_t)mi.AllocationBase;
            uint64_t q = end;
            MEMORY_BASIC_INFORMATION more;
            while (VirtualQuery((void *)(uintptr_t)q, &more, sizeof more) && more.State != MEM_FREE &&
                   more.AllocationBase == mi.AllocationBase)
                q = (uint64_t)(uintptr_t)more.BaseAddress + more.RegionSize;
            *s = q - *a;
            return KERN_SUCCESS;
        }
        if (end <= p) break;
        p = end;
    }
    return KERN_INVALID_ADDRESS;
#else
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return KERN_FAILURE;
    char line[512];
    kern_return_t r = KERN_INVALID_ADDRESS;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && hi > *a) {
            *a = lo; *s = hi - lo; r = KERN_SUCCESS;
            break;
        }
    }
    fclose(f);
    return r;
#endif
}

kern_return_t vm_region_recurse_64(vm_map_t t, vm_address_t *a, vm_size_t *s, natural_t *depth,
                                   vm_region_recurse_info_t info, mach_msg_type_number_t *cnt) {
    (void)t; (void)a; (void)s; (void)depth; (void)info; (void)cnt;
    return KERN_FAILURE;   /* resident-page statistics (memory reports): not available here, they report 0 */
}

/* ---- SHA-256 ---- */

#if defined(_WIN32)
static BCRYPT_ALG_HANDLE sha256_provider(void) {
    static BCRYPT_ALG_HANDLE alg;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) abort();
        InitOnceComplete(&once, 0, NULL);
    }
    return alg;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    memset(c, 0, sizeof *c);
    BCRYPT_HASH_HANDLE h = NULL;
    if (BCryptCreateHash(sha256_provider(), &h, NULL, 0, NULL, 0, 0) != 0) abort();
    c->os = h;
    return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    if (BCryptHashData((BCRYPT_HASH_HANDLE)c->os, (PUCHAR)data, len, 0) != 0) abort();
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    if (BCryptFinishHash((BCRYPT_HASH_HANDLE)c->os, md, 32, 0) != 0) abort();
    BCryptDestroyHash((BCRYPT_HASH_HANDLE)c->os);
    c->os = NULL;
    return 1;
}
#else
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (int i = 0; i < 64; i++) {
        t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(c, 0, sizeof *c);
    memcpy(c->h, init, sizeof init);
    return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data;
    c->bytes += len;
    if (c->used) {
        unsigned take = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; len -= take;
        if (c->used == 64) { block(c->h, c->buf); c->used = 0; }
    }
    for (; len >= 64; p += 64, len -= 64) block(c->h, p);
    memcpy(c->buf, p, len);
    c->used = len;
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->bytes * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    CC_SHA256_Update(c, &pad, 1);
    while (c->used != 56) CC_SHA256_Update(c, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, len, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
#endif

unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, data, len);
    CC_SHA256_Final(md, &c);
    return md;
}
