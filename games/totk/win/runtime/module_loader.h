/* Horizon module loader for the Windows port: places the game's modules (from nso_manifest.py's .nxm files) in the
 * guest address space, links them against each other and against the host, and lists their code for the recompiler.
 *
 * Imports a module cannot satisfy from another module are offered to the host: a host function gets a two-instruction
 * guest stub `svc #(LOADER_SVC_HOST + index); ret`, which the runtime's SVC handler sends to the bridge adapter of that
 * index. An import nobody provides gets a stub `svc #(LOADER_SVC_UNRESOLVED + k); ret`, so calling it stops with its
 * name instead of jumping to address 0. Weak undefined imports resolve to 0, as in ELF.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define LOADER_MAX_MODULES 8
#define LOADER_SVC_HOST 0x1000u        /* .. 0xEFFF: host functions through the bridge (bridge_abi.h's base) */
#define LOADER_SVC_UNRESOLVED 0xF000u  /* .. 0xFFFF: imports nothing provides */
#define LOADER_MAX_UNRESOLVED 0x1000u
#define LOADER_MODULE_ALIGN 0x200000ull /* modules start on 2 MiB boundaries, as on the console */

typedef struct LoaderSymbol { const char *name; uint32_t flags; uint64_t value; } LoaderSymbol;
enum { LOADER_SYM_DEFINED = 1, LOADER_SYM_WEAK = 2, LOADER_SYM_FUNCTION = 4 };

typedef struct LoadedModule {
    char name[64];
    uint64_t base;                 /* guest (= host) address of the module's offset 0 */
    uint64_t image_size, bss_size, size;   /* size: image + bss, page-rounded */
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t init_array_off, init_array_count;
    uint64_t init, fini;           /* DT_INIT / DT_FINI, absolute (valid when has_init / has_fini) */
    int has_init, has_fini;
    const LoaderSymbol *symbols; uint32_t nsym;
    const uint64_t *hints; uint32_t nhint;   /* module-relative */
    void *file; size_t file_size;            /* the .nxm contents, kept for the tables above */
} LoadedModule;

/* Asked first for every symbol a relocation names, defined or not, as the Mac loader does: the host's implementation
   replaces the game's own (nvnBootstrapLoader: the host's graphics instead of the SDK's NVN driver). jump_slot is set
   for PLT slots, where the host may also observe a call (log it and call the original). Returns the bridge index of a
   host function, or -1: then the symbol resolves to its own definition, another module's export, or a trap stub. */
typedef int (*LoaderHostResolve)(const char *name, int jump_slot, void *ctx);

typedef struct Loader {
    LoadedModule modules[LOADER_MAX_MODULES];
    unsigned count;
    uint64_t base, size;           /* the whole reservation: modules, then the stub page */
    uint64_t stubs, stubs_size;    /* guest stubs for host and unresolved imports */
    uint32_t nstub, host_count;
    const char *unresolved[LOADER_MAX_UNRESOLVED];
    uint32_t nunresolved;
    uint64_t relocs_applied, imports_host, imports_module, imports_weak_zero;
    char error[256];
} Loader;

/* Reads the .nxm files (in load order: main first), reserves guest memory at `base` (0: the lowest free range from
   0x8000000), copies and links the modules. Every host function index below `host_count` gets its stub at once, so
   function pointers the host hands out later (callbacks, NVN's GetProcAddress) are compiled ahead of time like the
   rest. 0 on success, -1 with loader->error set. */
int loader_load(Loader *l, const char *const *paths, unsigned count, uint64_t base, uint32_t host_count,
                LoaderHostResolve resolve, void *ctx);
/* Text ranges and function-start hints for the recompiler: `ranges` gets count+1 entries (every module's text, then
   the stub page), `hints` up to `max_hints` absolute addresses. Returns the hint count. */
typedef struct LoaderCodeRange { uint64_t address; const void *code; size_t size; } LoaderCodeRange;
size_t loader_code(const Loader *l, LoaderCodeRange *ranges, uint64_t *hints, size_t max_hints);
/* After loading: makes text and rodata read-only, data read-write. */
int loader_seal(Loader *l);
/* The absolute address of an exported symbol (searching modules in load order), or 0. */
uint64_t loader_symbol(const Loader *l, const char *name);
/* The guest address of host function `index`'s stub (svc #(LOADER_SVC_HOST + index); ret), or 0 past host_count. */
uint64_t loader_host_stub(const Loader *l, uint32_t index);
/* The name of unresolved stub k (svc immediate - LOADER_SVC_UNRESOLVED), or NULL. */
const char *loader_unresolved_name(const Loader *l, uint32_t k);
void loader_free(Loader *l);
