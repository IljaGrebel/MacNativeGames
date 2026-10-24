/* The platform layer alone (no recompiler): guest memory and the module loader on the synthetic modules.
 * Builds for Linux and for Windows (MinGW), so the Windows memory calls are tested too (under Wine in CI).
 *
 * usage: platform_test main.nxm sdk.nxm
 */
#include <stdio.h>
#include <string.h>

#include "guest_mem.h"
#include "module_loader.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int is_read_only(uint64_t a) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) && mi.Protect == PAGE_READONLY;
}
#else
static int is_read_only(uint64_t a) {
    char line[256];
    FILE *f = fopen("/proc/self/maps", "r");
    int ro = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perm[5];
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perm) == 3 && a >= lo && a < hi) { ro = !strcmp(perm, "r--p"); break; }
    }
    if (f) fclose(f);
    return ro;
}
#endif

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static int host_index(const char *name, int jump_slot, void *ctx) { (void)ctx; (void)jump_slot; return strcmp(name, "host_add") ? -1 : 7; }

/* the GOT slot that holds `symbol`: the relocated word equal to its address */
static int points_to(const LoadedModule *m, uint64_t target) {
    for (uint64_t a = m->base + m->data_off; a + 8 <= m->base + m->image_size; a += 8)
        if (rd64(a) == target) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s main.nxm sdk.nxm\n", argv[0]); return 1; }

    /* guest memory */
    uint64_t g = gm_reserve_granularity(), p = gm_page_size();
    uint64_t base = gm_find_free(0x100000000ull, GM_ADDRESS_LIMIT, 16 * g, 0x10000000ull);
    CHECK(base && gm_reserve(base, 16 * g) == 0, "reserve 16 x %#llx at %#llx", (unsigned long long)g, (unsigned long long)base);
    CHECK(gm_reserve(base, 16 * g) != 0, "a second reservation of the same range is refused");
    CHECK(gm_commit(base + g, p, GM_READ | GM_WRITE) == 0 && rd64(base + g) == 0, "commit gives zero-filled memory");
    memset((void *)(uintptr_t)(base + g), 0xAB, (size_t)p);
    CHECK(gm_decommit(base + g, p) == 0 && gm_commit(base + g, p, GM_READ | GM_WRITE) == 0 && rd64(base + g) == 0,
          "decommit and commit again gives zero-filled memory");
    CHECK(gm_protect(base + g, p, GM_READ) == 0 && is_read_only(base + g), "protect to read-only");
    CHECK(gm_reserve(GM_ADDRESS_LIMIT - g, 2 * g) != 0, "a range crossing 2^39 is refused");
    CHECK(gm_release(base, 16 * g) == 0, "release");

    /* module loader */
    Loader l;
    const char *paths[] = {argv[1], argv[2]};
    if (loader_load(&l, paths, 2, 0, 8, host_index, NULL)) { printf("FAIL load: %s\n", l.error); return 1; }
    const LoadedModule *mainm = &l.modules[0], *sdk = &l.modules[1];
    CHECK(!strcmp(mainm->name, "main") && !strcmp(sdk->name, "sdk"), "module names main, sdk");
    CHECK(l.relocs_applied == 10 && l.imports_module == 3 && l.imports_host == 1, "10 relocations, 3 module and 1 host import");
    uint64_t sdk_mul = loader_symbol(&l, "sdk_mul"), sdk_ready = loader_symbol(&l, "sdk_ready");
    CHECK(sdk_mul > sdk->base && sdk_mul < sdk->base + sdk->text_size, "sdk_mul in sdk's text");
    CHECK(points_to(mainm, sdk_mul) && points_to(mainm, sdk_ready), "main's GOT and PLT slots hold sdk's addresses");
    uint64_t table = loader_symbol(&l, "table");
    CHECK(rd64(table) >= mainm->base && rd64(table) < mainm->base + mainm->text_size &&
          rd64(table + 8) >= mainm->base && rd64(table + 8) < mainm->base + mainm->text_size, "RELATIVE function pointers in main");
    CHECK(rd64(loader_symbol(&l, "imported_pointer")) == sdk_mul, "ABS64 against an import");
    uint64_t stub7 = loader_host_stub(&l, 7);
    CHECK(stub7 == l.stubs + 56 && rd32(stub7) == (0xD4000001u | ((LOADER_SVC_HOST + 7) << 5)) && rd32(stub7 + 4) == 0xD65F03C0u,
          "host stub 7: svc #0x1007; ret");
    CHECK(loader_host_stub(&l, 8) == 0, "no stub past the host functions");
    CHECK(points_to(mainm, stub7), "main's host_add slot holds the stub");
    uint64_t init = rd64(sdk->base + sdk->init_array_off);
    CHECK(sdk->init_array_count == 1 && init >= sdk->base && init < sdk->base + sdk->text_size, "sdk init array relocated (RELR)");
    CHECK(loader_seal(&l) == 0 && is_read_only(mainm->base) && is_read_only(l.stubs), "text and stubs read-only after sealing");
    LoaderCodeRange ranges[LOADER_MAX_MODULES + 1];
    uint64_t hints[64];
    size_t n = loader_code(&l, ranges, hints, 64);
    int has_init = 0, has_entry = 0;
    for (size_t i = 0; i < n; i++) { has_init |= hints[i] == init; has_entry |= hints[i] == loader_symbol(&l, "test_entry"); }
    CHECK(ranges[0].address == mainm->base && ranges[2].address == l.stubs && has_init && has_entry,
          "code ranges and hints (%zu) cover text, stubs, constructors and exports", n);
    loader_free(&l);

    /* a damaged manifest must be refused, not loaded */
    FILE *f = fopen(argv[1], "rb");
    static unsigned char buf[1 << 16];
    size_t size = fread(buf, 1, sizeof buf, f);
    fclose(f);
    buf[128] ^= 0xFF;   /* first relocation's offset */
    buf[133] ^= 0x7F;
    const char *bad = "damaged.nxm";
    f = fopen(bad, "wb");
    fwrite(buf, 1, size, f);
    fclose(f);
    const char *bad_paths[] = {bad, argv[2]};
    CHECK(loader_load(&l, bad_paths, 2, 0, 8, host_index, NULL) != 0, "damaged manifest refused: %s", l.error);
    remove(bad);

    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails != 0;
}
