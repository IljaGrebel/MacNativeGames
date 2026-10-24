/* End to end: synthetic Horizon modules (tests/fixtures) -> nso_manifest.py -> module loader -> recompiler -> run.
 *
 * usage: loader_test main.nxm sdk.nxm [--no-host]
 * Prints one line per check and exits 0 when all pass. With --no-host the host function is withheld, so main's call
 * to it must stop at its unresolved stub with its name (exit 3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <scarletnx/snx_recompiler.h>

#include "bridge_abi.h"
#include "guest_mem.h"
#include "module_loader.h"
#include "recomp_host.h"
#include "test_protos.h"

static Loader loader;
static int no_host;

static long host_add(long a, long b) { return a + b; }
static void *host_function(const char *name) { return strcmp(name, "host_add") ? NULL : (void *)host_add; }
void bridge_missing(const char *name) { fprintf(stderr, "bridge: %s has no host function\n", name); exit(2); }

static int resolve(const char *name, int jump_slot, void *ctx) {
    (void)jump_slot;
    (void)ctx;
    if (no_host) return -1;
    for (unsigned i = 0; i < bridge_nvn_count; i++)
        if (!strcmp(bridge_nvn_table[i].name, name)) return (int)i;
    return -1;
}

static unsigned long long host_calls;
static void svc(CPU *c, uint32_t imm) {
    if (imm >= LOADER_SVC_UNRESOLVED) {
        const char *name = loader_unresolved_name(&loader, imm - LOADER_SVC_UNRESOLVED);
        printf("unresolved import called: %s\n", name ? name : "?");
        fflush(stdout);
        exit(3);
    }
    if (imm >= LOADER_SVC_HOST && imm - LOADER_SVC_HOST < bridge_nvn_count) {
        host_calls++;
        bridge_nvn_table[imm - LOADER_SVC_HOST].adapter(c);
        return;
    }
    fprintf(stderr, "svc #%#x is not implemented\n", imm);
    exit(4);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s main.nxm sdk.nxm [--no-host]\n", argv[0]); return 1; }
    no_host = argc > 3 && !strcmp(argv[3], "--no-host");
    int fails = 0;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

    if (bridge_nvn_bind(host_function)) { fprintf(stderr, "bridge bind\n"); return 1; }
    const char *paths[] = {argv[1], argv[2]};
    if (loader_load(&loader, paths, 2, 0, bridge_nvn_count, resolve, NULL)) { fprintf(stderr, "load: %s\n", loader.error); return 1; }
    CHECK(loader.base && loader.base + loader.size <= GM_ADDRESS_LIMIT, "modules at %#llx (+%#llx), below 2^39",
          (unsigned long long)loader.base, (unsigned long long)loader.size);
    CHECK(loader.modules[1].base % LOADER_MODULE_ALIGN == 0 && loader.modules[1].base > loader.modules[0].base,
          "sdk placed after main on a 2 MiB boundary");
    CHECK(loader.relocs_applied == 10, "10 relocations applied (%llu)", (unsigned long long)loader.relocs_applied);
    CHECK(loader.imports_module == 3, "3 imports bound to sdk (%llu)", (unsigned long long)loader.imports_module);
    CHECK(no_host ? loader.nunresolved == 1 : loader.imports_host == 1, "host import %s",
          no_host ? "left unresolved with a trap stub" : "bound to a bridge stub");
    uint64_t entry = loader_symbol(&loader, "test_entry");
    CHECK(entry > loader.modules[0].base && entry < loader.modules[0].base + loader.modules[0].text_size,
          "test_entry exported in main's text at %#llx", (unsigned long long)entry);
    if (loader_seal(&loader)) { fprintf(stderr, "seal: %s\n", loader.error); return 1; }

    /* a stack for the guest in its own reservation */
    uint64_t stack_size = 1 << 20, stack = gm_find_free(loader.base + loader.size, GM_ADDRESS_LIMIT, stack_size, 1 << 21);
    if (!stack || gm_reserve(stack, stack_size) || gm_commit(stack, stack_size, GM_READ | GM_WRITE)) {
        fprintf(stderr, "stack: %s\n", gm_error());
        return 1;
    }
    uint64_t sp = stack + stack_size - 64;

    RecompHost rh;
    if (recomp_build(&rh, &loader, NULL, 1)) { fprintf(stderr, "%s\n", rh.error); return 1; }
    printf("info recompiled %zu functions in %.2f s\n", rh.functions, rh.seconds);
    static CPU cpu;
    if (recomp_cpu_init(&rh, &cpu, svc, NULL)) { fprintf(stderr, "%s\n", rh.error); return 1; }

    recomp_run_init(&rh, &cpu, &loader, sp);
    int32_t ready;
    memcpy(&ready, (const void *)(uintptr_t)loader_symbol(&loader, "sdk_ready"), 4);
    CHECK(ready == 42, "sdk constructor ran (sdk_ready = %d)", ready);

    unsigned long long calls = 0;
    for (long x = -3; x <= 1000; x += 251, calls++) {
        uint64_t arg = (uint64_t)x;
        long got = (long)recomp_call(&rh, &cpu, entry, &arg, 1, sp);
        CHECK(got == 8 * x + 132, "test_entry(%ld) = %ld, want %ld", x, got, 8 * x + 132);
    }
    CHECK(host_calls == calls, "host function called through the bridge once per call (%llu of %llu)", host_calls, calls);
    recomp_free(&rh);
    loader_free(&loader);
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails != 0;
}
