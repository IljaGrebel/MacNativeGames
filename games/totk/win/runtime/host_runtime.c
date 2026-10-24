/* The Windows port's host runtime: what runtime/loader.c and runtime/hle_stack.c do on the Mac, for recompiled code.
 *
 * On the Mac the game's ARM64 code runs natively, so a guest thread is a host thread on a guest stack, host functions are
 * called directly and the host calls guest functions directly. Here the game's code is recompiled to x86-64 and runs on
 * a CPU state per thread (CPU, from the recompiler), so this file supplies:
 *   - the CPU state and Horizon thread-local region of every guest thread (TPIDRRO_EL0 -> the TLR, as ipc.c expects);
 *   - thread entry and exit for threads.c (dm_thread_enter / runtime_thread_leave);
 *   - SVC dispatch: Horizon calls to the upstream runtime_svc, host-function stubs to their bridge adapters;
 *   - host -> guest calls: runtime_symbol() hands out x86 thunks that run the guest function recompiled;
 *   - the module memory calls services.c makes (permissions, queries) over the module loader;
 *   - the boot: nn::init::Start with the host's callbacks, as the Mac loader's boot() does.
 */
#include "host_runtime.h"

#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <scarletnx/snx_recompiler.h>

#include "guest_mem.h"
#include "host_bridge.h"
#include "module_loader.h"
#include "recomp_host.h"
#include "runtime.h"

pthread_key_t runtime_keys[8];
static Loader g_loader;
static RecompHost g_rh;
static int g_loaded;

/* ---- host functions: one index per adapter, in the order the tables list them ---- */

typedef struct HostFn { const char *name; const void *fn; void (*adapter)(GuestCpu *c); } HostFn;
static HostFn *g_host;
static uint32_t g_nhost, g_cap_host;
static _Thread_local const HostFn *g_calling;   /* the host function the current SVC runs (for name-driven adapters) */

static uint32_t host_add(const char *name, const void *fn, void (*adapter)(GuestCpu *c)) {
    for (uint32_t i = 0; i < g_nhost; i++)
        if (g_host[i].fn == fn && g_host[i].adapter == adapter && (fn || !strcmp(g_host[i].name, name))) return i;
    if (g_nhost == g_cap_host) {
        g_cap_host = g_cap_host ? 2 * g_cap_host : 1024;
        g_host = realloc(g_host, g_cap_host * sizeof *g_host);
        if (!g_host) runtime_fail("host function table");
    }
    g_host[g_nhost] = (HostFn){name, fn, adapter};
    return g_nhost++;
}

uint32_t host_runtime_register(const char *name, const void *fn, void (*adapter)(GuestCpu *c)) {
    return host_add(name, fn, adapter);
}

const char *host_runtime_calling(void) { return g_calling ? g_calling->name : NULL; }

static int host_index_of_fn(const void *fn) {
    for (uint32_t i = 0; i < g_nhost; i++) if (g_host[i].fn == fn) return (int)i;
    return -1;
}

int host_runtime_index(const char *name) {
    for (uint32_t i = 0; i < g_nhost; i++) if (!g_host[i].fn && !strcmp(g_host[i].name, name)) return (int)i;
    for (uint32_t i = 0; i < g_nhost; i++) if (!strcmp(g_host[i].name, name)) return (int)i;
    return -1;
}

uint64_t host_runtime_stub(uint32_t index) { return loader_host_stub(&g_loader, index); }

/* The loader asks for every symbol: the upstream runtime decides, exactly as on the Mac (runtime_observer for PLT
   slots, then runtime_override), and the answer is mapped to the adapter generated for that host function. */
static int resolve(const char *name, int jump_slot, void *ctx) {
    (void)ctx;
    void *p = jump_slot ? runtime_observer(name) : NULL;
    if (!p) p = runtime_override(name);
    if (!p) return -1;
    int index = host_index_of_fn(p);
    if (index < 0) {
        fprintf(stderr, "[totk-win] %s: the host implements it but no adapter was generated\n", name);
        runtime_fail("host function without a bridge adapter");
    }
    return index;
}

/* ---- guest memory for host-made guest objects (thread-local regions, stacks for host calls) ---- */

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pool, g_pool_used, g_pool_size = 1ull << 30;

uint64_t host_runtime_guest_alloc(uint64_t size) {
    size = (size + 0xFFFF) & ~0xFFFFull;
    pthread_mutex_lock(&g_pool_lock);
    if (!g_pool) {
        g_pool = gm_find_free(0x200000000ull, GM_ADDRESS_LIMIT, g_pool_size, 1ull << 21);
        if (!g_pool || gm_reserve(g_pool, g_pool_size)) runtime_fail("guest pool reservation");
    }
    uint64_t at = g_pool + g_pool_used;
    if (g_pool_used + size > g_pool_size || gm_commit(at, size, GM_READ | GM_WRITE)) {
        pthread_mutex_unlock(&g_pool_lock);
        runtime_fail("guest pool exhausted");
    }
    g_pool_used += size;
    pthread_mutex_unlock(&g_pool_lock);
    return at;
}

/* ---- guest threads ---- */

#define HOST_CALL_STACK (1u << 20)   /* a guest stack for host -> guest calls made when no guest frame is active */

typedef struct GuestThread {
    CPU cpu;
    uint64_t tls, tlr, dummy, call_stack_top;
    uint64_t stack_lo, stack_hi;         /* the main thread's guest stack (QueryMemory reports it as the stack) */
    unsigned depth;                      /* guest code active on this thread (nested host -> guest calls) */
} GuestThread;

static pthread_key_t g_thread_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static void make_key(void) { if (pthread_key_create(&g_thread_key, NULL)) runtime_fail("guest thread key"); }

static GuestThread *current(void) {
    pthread_once(&g_key_once, make_key);
    return pthread_getspecific(g_thread_key);
}

static void svc_entry(CPU *c, uint32_t imm);
void runtime_describe_address(uint64_t a, char *out, size_t n);
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);

/* threads.c (guest_thread_init) and boot(): the calling host thread becomes a guest thread. */
void runtime_thread_init(void) {
    if (current()) runtime_fail("duplicate guest thread environment");
    GuestThread *t = calloc(1, sizeof *t);
    if (!t) runtime_fail("guest thread");
    uint64_t region = host_runtime_guest_alloc(8192 + 0x200 + 0x400 + HOST_CALL_STACK);
    t->tls = region;
    t->tlr = region + 8192;
    t->dummy = t->tlr + 0x200;
    t->call_stack_top = region + 8192 + 0x200 + 0x400 + HOST_CALL_STACK - 64;
    memcpy((void *)(uintptr_t)(t->tlr + 0x1f8), &t->dummy, 8);   /* the TLR's thread-type slot, as the Mac sets it */
    if (g_loaded) {
        if (recomp_cpu_init(&g_rh, &t->cpu, svc_entry, t)) runtime_fail(g_rh.error);
    }
    t->cpu.tpidrro = t->tlr;
    pthread_setspecific(g_thread_key, t);
    if (pthread_setspecific(runtime_keys[0], NULL) || pthread_setspecific(runtime_keys[1], (void *)(uintptr_t)t->tls) ||
        pthread_setspecific(runtime_keys[2], (void *)(uintptr_t)t->tlr))
        runtime_fail("thread slots");
}

static int is_guest_code(uint64_t pc) {
    if (!g_loaded) return 0;
    for (unsigned i = 0; i < g_loader.count; i++) {
        const LoadedModule *m = &g_loader.modules[i];
        if (pc >= m->base && pc < m->base + m->text_size) return 1;
    }
    return pc >= g_loader.stubs && pc < g_loader.stubs + g_loader.stubs_size;
}

static void run_guest(GuestThread *t, uint64_t fn, uint64_t sp, const uint64_t *args, unsigned nargs) {
    for (unsigned i = 0; i < 8; i++) t->cpu.x[i] = i < nargs ? args[i] : 0;
    t->cpu.x[30] = 0;   /* the run ends when the function returns to 0 */
    t->cpu.x[31] = sp;
    t->depth++;
    snx_recompiled_run(g_rh.m, &t->cpu, fn, 0);
    t->depth--;
}

extern int (*host_thread_guest_entry)(uint64_t entry, uint64_t arg, uint64_t sp);   /* thread_enter.c */

/* threads.c starts a guest thread through dm_thread_enter (thread_enter.c), which asks here first */
static int guest_entry(uint64_t entry, uint64_t arg, uint64_t sp) {
    if (!is_guest_code(entry)) return 0;
    GuestThread *t = current();
    if (!t) runtime_fail("guest thread entered without an environment");
    run_guest(t, entry, sp, &arg, 1);
    return 1;
}

/* compat_stack_top / compat_stack_size (pthread_get_stackaddr_np in services.c's QueryMemory): on the Mac the main
   thread's guest code runs on the host thread's own stack, which QueryMemory reports as the stack region. Here the
   main thread's guest stack is guest memory: report that one. */
int host_runtime_guest_stack(uint64_t *lo, uint64_t *hi) {
    GuestThread *t = current();
    if (!t || !t->stack_hi) return 0;
    *lo = t->stack_lo;
    *hi = t->stack_hi;
    return 1;
}

/* ---- host -> guest calls ---- */

uint64_t host_runtime_guest_call(uint64_t fn, const uint64_t *args, unsigned nargs) {
    GuestThread *t = current();
    if (!t) { runtime_thread_init(); t = current(); }
    CPU saved;
    memcpy(&saved, &t->cpu, sizeof saved);
    /* below the guest's own stack pointer when guest code is active on this thread (a host function it called is
       calling back), else on the thread's host-call stack */
    uint64_t sp = t->depth ? ((saved.x[31] - 512) & ~15ull) : t->call_stack_top;
    run_guest(t, fn, sp, args, nargs);
    uint64_t result = t->cpu.x[0];
    memcpy(&t->cpu, &saved, sizeof saved);
    return result;
}

/* runtime_symbol: upstream host code calls guest functions through plain C function pointers (the Mac's guest code is
   host code). Each guest function asked for gets one of these thunks: an x86 function taking up to eight integer
   arguments that runs the guest function recompiled. Floating-point arguments are not passed (none of the calls the
   runtime makes this way has one). */
#define THUNKS 128
static uint64_t g_thunk_target[THUNKS];
static const char *g_thunk_name[THUNKS];
static unsigned g_nthunk;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;
/* thunk_<g>_<k> is thunk number g*8+k */
#define THUNK(g, k)                                                                                                 \
    static uint64_t thunk_##g##_##k(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,   \
                                    uint64_t a6, uint64_t a7) {                                                   \
        uint64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};                                                        \
        return host_runtime_guest_call(g_thunk_target[(g) * 8 + (k)], args, 8);                                     \
    }
#define T8(g) THUNK(g, 0) THUNK(g, 1) THUNK(g, 2) THUNK(g, 3) THUNK(g, 4) THUNK(g, 5) THUNK(g, 6) THUNK(g, 7)
T8(0) T8(1) T8(2) T8(3) T8(4) T8(5) T8(6) T8(7) T8(8) T8(9) T8(10) T8(11) T8(12) T8(13) T8(14) T8(15)
#define P8(g) thunk_##g##_0, thunk_##g##_1, thunk_##g##_2, thunk_##g##_3, thunk_##g##_4, thunk_##g##_5, thunk_##g##_6, \
              thunk_##g##_7
typedef uint64_t (*ThunkFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static const ThunkFn g_thunks[THUNKS] = {P8(0), P8(1), P8(2), P8(3), P8(4), P8(5), P8(6), P8(7),
                                         P8(8), P8(9), P8(10), P8(11), P8(12), P8(13), P8(14), P8(15)};

void *host_runtime_thunk(uint64_t guest_fn, const char *name) {
    if (!guest_fn) return NULL;
    pthread_mutex_lock(&g_thunk_lock);
    for (unsigned i = 0; i < g_nthunk; i++)
        if (g_thunk_target[i] == guest_fn) { pthread_mutex_unlock(&g_thunk_lock); return (void *)g_thunks[i]; }
    if (g_nthunk == THUNKS) runtime_fail("out of guest-call thunks");
    g_thunk_target[g_nthunk] = guest_fn;
    g_thunk_name[g_nthunk] = name;
    void *t = (void *)g_thunks[g_nthunk++];
    pthread_mutex_unlock(&g_thunk_lock);
    return t;
}

void *runtime_symbol(const char *name) { return host_runtime_thunk(loader_symbol(&g_loader, name), name); }

/* ---- SVCs ---- */

static void svc_entry(CPU *c, uint32_t imm) {
    if (imm >= LOADER_SVC_UNRESOLVED) {
        const char *name = loader_unresolved_name(&g_loader, imm - LOADER_SVC_UNRESOLVED);
        char where[96];
        runtime_describe_address(c->x[30], where, sizeof where);
        fprintf(stderr, "[totk-win] the game called %s, which nothing implements (from %s)\n", name ? name : "?", where);
        runtime_fail("unresolved import called");
    }
    if (imm >= LOADER_SVC_HOST) {
        uint32_t i = imm - LOADER_SVC_HOST;
        if (i >= g_nhost) runtime_fail("bad host function stub");
        const HostFn *previous = g_calling;
        g_calling = &g_host[i];
        g_host[i].adapter(c);
        g_calling = previous;
        return;
    }
    const char *module = "?";
    uint64_t offset = c->pc;
    runtime_module_offset(c->pc, &module, &offset);
    static int trace = -1;   /* TOTK_WIN_TRACE_SVC=1: every Horizon SVC with its arguments and result */
    if (trace < 0) trace = getenv("TOTK_WIN_TRACE_SVC") != NULL;
    if (trace)
        fprintf(stderr, "[svc] %#x(%#llx, %#llx, %#llx, %#llx) from %s+%#llx\n", imm, (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], module,
                (unsigned long long)offset);
    runtime_svc(imm, c->x, module, offset);
    if (trace)
        fprintf(stderr, "[svc] %#x -> %#llx, %#llx\n", imm, (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
}

/* ---- module memory (services.c) ---- */

static pthread_mutex_t g_perm_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_perm[LOADER_MAX_MODULES];   /* per 4 KiB data page: Horizon permission bits, 16 = locked */

static unsigned page_permission(unsigned i, uint64_t off) {
    const LoadedModule *m = &g_loader.modules[i];
    if (off < m->data_off) return off < ((m->text_size + 4095) & ~4095ull) ? 5 : 1;
    return g_perm[i] ? g_perm[i][(off - m->data_off) / 4096] : 3;
}

static int data_range(uint64_t address, uint64_t size, unsigned *index) {
    for (unsigned i = 0; i < g_loader.count; i++) {
        const LoadedModule *m = &g_loader.modules[i];
        if (address < m->base + m->data_off || address >= m->base + m->size || size > m->base + m->size - address) continue;
        if (!g_perm[i]) {
            size_t n = (size_t)((m->size - m->data_off + 4095) / 4096);
            g_perm[i] = malloc(n);
            if (!g_perm[i]) runtime_fail("module permission table");
            memset(g_perm[i], 3, n);
        }
        *index = i;
        return 1;
    }
    return 0;
}

uint64_t runtime_module_permission(uint64_t address, uint64_t size, unsigned permission) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (permission != 0 && permission != 1 && permission != 3) return 0xd801;
    unsigned i;
    pthread_mutex_lock(&g_perm_lock);
    if (!data_range(address, size, &i)) { pthread_mutex_unlock(&g_perm_lock); return 0xd401; }
    const LoadedModule *m = &g_loader.modules[i];
    uint64_t first = (address - m->base - m->data_off) / 4096;
    for (uint64_t p = 0; p < size / 4096; p++)
        if (g_perm[i][first + p] & 16) { pthread_mutex_unlock(&g_perm_lock); return 0xd401; }
    memset(g_perm[i] + first, (int)permission, size / 4096);
    unsigned prot = (permission & 1 ? GM_READ : 0) | (permission & 2 ? GM_WRITE : 0);
    if (gm_protect(address, size, prot)) runtime_fail(gm_error());
    pthread_mutex_unlock(&g_perm_lock);
    fprintf(stderr, "[totk] %s data permission +%#llx size=%#llx perm=%u\n", m->name,
            (unsigned long long)(address - m->base), (unsigned long long)size, permission);
    return 0;
}

uint64_t runtime_module_lock_permission(uint64_t address, uint64_t size, unsigned mask, unsigned attribute) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (mask != 16 || attribute != 16) return 0xd801;   /* the SDK's only module attribute: the RELRO lock */
    unsigned i;
    pthread_mutex_lock(&g_perm_lock);
    if (!data_range(address, size, &i)) { pthread_mutex_unlock(&g_perm_lock); return 0xd401; }
    const LoadedModule *m = &g_loader.modules[i];
    uint64_t first = (address - m->base - m->data_off) / 4096;
    for (uint64_t p = 0; p < size / 4096; p++) g_perm[i][first + p] |= 16;
    pthread_mutex_unlock(&g_perm_lock);
    return 0;
}

int runtime_module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission,
                         unsigned *attribute) {
    for (unsigned i = 0; i < g_loader.count; i++) {
        const LoadedModule *m = &g_loader.modules[i];
        if (address < m->base || address >= m->base + m->size) continue;
        uint64_t off = (address - m->base) & ~4095ull, lo = off, hi = off + 4096;
        pthread_mutex_lock(&g_perm_lock);
        unsigned perm = page_permission(i, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && page_permission(i, lo - 4096) == perm) lo -= 4096;
        while (hi < end && page_permission(i, hi) == perm) hi += 4096;
        pthread_mutex_unlock(&g_perm_lock);
        *base = m->base + lo; *size = hi - lo; *state = data ? 4 : 3; *permission = perm & 7; *attribute = perm & 16;
        return 1;
    }
    return 0;
}

uint8_t *runtime_module_base(const char *name) {
    for (unsigned i = 0; i < g_loader.count; i++)
        if (!strcmp(g_loader.modules[i].name, name)) return (uint8_t *)(uintptr_t)g_loader.modules[i].base;
    return NULL;
}

int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    for (unsigned i = 0; i < g_loader.count; i++) {
        const LoadedModule *m = &g_loader.modules[i];
        if (pc >= m->base && pc < m->base + m->size) { *name = m->name; *off = pc - m->base; return 1; }
    }
    return 0;
}

int runtime_guest_code_pc(uint64_t pc) { return is_guest_code(pc); }

uint64_t runtime_image_bytes(void) {
    uint64_t total = 0;
    for (unsigned i = 0; i < g_loader.count; i++) total += g_loader.modules[i].size;
    return total;
}

void runtime_describe_address(uint64_t a, char *out, size_t n) {
    const char *name;
    uint64_t off;
    if (runtime_module_offset(a, &name, &off)) { snprintf(out, n, "%s+0x%llx", name, (unsigned long long)off); return; }
    if (a >= g_loader.stubs && a < g_loader.stubs + g_loader.stubs_size) {
        uint64_t k = (a - g_loader.stubs) / 8;
        snprintf(out, n, "host:%s", k < g_nhost ? g_host[k].name : "?");
        return;
    }
    snprintf(out, n, "0x%llx", (unsigned long long)a);
}

void runtime_trace_guest(uint64_t fp, uint64_t lr) {
    for (unsigned n = 0; n < 16; n++) {
        char where[96];
        runtime_describe_address(lr, where, sizeof where);
        fprintf(stderr, "[totk] caller %s\n", where);
        if (!fp || fp % 8 || !compat_readable(fp, 16)) break;
        uint64_t f[2];
        memcpy(f, (const void *)(uintptr_t)fp, 16);
        lr = f[1];
        if (f[0] <= fp || f[0] - fp > (64u << 20)) break;
        fp = f[0];
    }
}

/* hle_stack.c on the Mac: host functions there run on a separate host stack. Recompiled code already runs host
   functions on the thread's own host stack (the guest stack is guest memory), so these are identities. */
void *runtime_hle_wrap(void *fn) { return fn; }
void *runtime_hle_wrap_named(void *fn, const char *name) { (void)name; return fn; }
void runtime_hle_trace_set(int mode) { (void)mode; }
void runtime_hle_describe(unsigned index, char *out, size_t cap) { snprintf(out, cap, "host#%u", index); }
void runtime_hle_stack_init(uint64_t tsd_offset) { (void)tsd_offset; }

uint64_t runtime_now_ns(void) { return compat_now_ns(); }

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[totk-win] FATAL %s\n", message);
    fflush(stderr);
    fflush(stdout);
    exit(70);
}

void runtime_startup_checkpoint(uintptr_t caller) { (void)caller; }

/* WFE: the SDK's spinlocks wait with it. A spinning thread keeps its virtual core, so the thread it waits for may never
   get one; give the core up, yield the host CPU and take a core again (what ScarletNX's own Switch runtime does). The
   thread sleeps instead of spinning, which also saves power. */
static void guest_wfe(CPU *c) {
    (void)c;
    int away = runtime_core_away();
    sched_yield();
    runtime_core_back(&away);
}

/* ---- loading and boot ---- */

int host_runtime_load(const char *const *paths, unsigned count, const char *cache_dir) {
    for (unsigned i = 0; i < 8; i++)
        if (pthread_key_create(&runtime_keys[i], NULL)) runtime_fail("thread keys");
    for (const HostBridgeEntry *const *t = host_bridge_tables; *t; t++)
        for (const HostBridgeEntry *e = *t; e->name; e++) host_add(e->name, e->fn, e->adapter);
    host_runtime_register_platform();
    host_runtime_register_boot_callbacks();
    if (loader_load(&g_loader, paths, count, 0, g_nhost, resolve, NULL)) {
        fprintf(stderr, "[totk-win] %s\n", g_loader.error);
        return -1;
    }
    fprintf(stderr, "[totk-win] %u modules at %#llx: %llu relocations, %llu host, %llu cross-module, %u unresolved imports\n",
            g_loader.count, (unsigned long long)g_loader.base, (unsigned long long)g_loader.relocs_applied,
            (unsigned long long)g_loader.imports_host, (unsigned long long)g_loader.imports_module, g_loader.nunresolved);
    for (uint32_t k = 0; k < g_loader.nunresolved && k < 64; k++)
        fprintf(stderr, "[totk-win]   unresolved: %s\n", g_loader.unresolved[k]);
    if (loader_seal(&g_loader)) { fprintf(stderr, "[totk-win] %s\n", g_loader.error); return -1; }
    if (recomp_build(&g_rh, &g_loader, cache_dir, 0)) { fprintf(stderr, "[totk-win] %s\n", g_rh.error); return -1; }
    fprintf(stderr, "[totk-win] recompiled %zu functions in %.1f s\n", g_rh.functions, g_rh.seconds);
    ((RtAPI *)g_rh.rt)->wfe = guest_wfe;
    g_loaded = 1;
    host_thread_guest_entry = guest_entry;
    return 0;
}

const Loader *host_runtime_loader(void) { return &g_loader; }

/* nn::init::Start's callbacks (host functions handed to the game as guest stubs) */
static void cb_loader_ready(void) { fprintf(stderr, "[totk] rtld callback: modules linked\n"); }
static void cb_module_inits(void) {
    fprintf(stderr, "[totk] SDK startup returned; running module constructors\n");
    for (unsigned i = 0; i < g_loader.count; i++) {
        const LoadedModule *m = &g_loader.modules[i];
        fprintf(stderr, "[totk] ENTER %s %s\n", m->name, m->has_init ? "DT_INIT" : "init_array");
        if (m->has_init) {
            host_runtime_guest_call(m->init, NULL, 0);
        } else {
            for (uint64_t k = 0; k < m->init_array_count; k++) {
                uint64_t f;
                memcpy(&f, (const void *)(uintptr_t)(m->base + m->init_array_off + 8 * k), 8);
                if (f) host_runtime_guest_call(f, NULL, 0);
            }
        }
        fprintf(stderr, "[totk] RETURN %s\n", m->name);
    }
}
static void cb_module_finis(void) { runtime_fail("guest exited: module finalizers not yet implemented"); }
static void void_adapter(GuestCpu *c) { (void)c; ((void (*)(void))g_calling->fn)(); }

int host_runtime_boot(const char *start_symbol, uint64_t main_stack_size) {
    runtime_thread_init();
    uint32_t main_handle = svc_thread_main_init();
    uint64_t start = loader_symbol(&g_loader, start_symbol);
    if (!start) { fprintf(stderr, "[totk-win] no %s in the modules\n", start_symbol); return -1; }
    uint32_t ready = host_add("__host_loader_ready", (const void *)cb_loader_ready, void_adapter);
    uint32_t inits = host_add("__host_module_inits", (const void *)cb_module_inits, void_adapter);
    uint32_t finis = host_add("__host_module_finis", (const void *)cb_module_finis, void_adapter);
    if (finis >= g_loader.host_count) {
        fprintf(stderr, "[totk-win] the boot callbacks were registered after loading\n");
        return -1;
    }
    uint64_t stack = host_runtime_guest_alloc(main_stack_size);
    current()->stack_lo = stack;
    current()->stack_hi = stack + main_stack_size;
    uint64_t args[5] = {main_handle, 0, host_runtime_stub(ready), host_runtime_stub(inits), host_runtime_stub(finis)};
    fprintf(stderr, "[totk] ENTER original nn::init::Start\n");
    GuestThread *t = current();
    run_guest(t, start, stack + main_stack_size - 64, args, 5);
    fprintf(stderr, "[totk] nn::init::Start returned\n");
    return 0;
}

void host_runtime_register_boot_callbacks(void) {
    host_add("__host_loader_ready", (const void *)cb_loader_ready, void_adapter);
    host_add("__host_module_inits", (const void *)cb_module_inits, void_adapter);
    host_add("__host_module_finis", (const void *)cb_module_finis, void_adapter);
}
