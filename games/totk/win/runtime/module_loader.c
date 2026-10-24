/* Horizon module loader: see module_loader.h. The .nxm format is written by games/totk/win/tools/nso_manifest.py. */
#include "module_loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"

#define NXM_HEADER_SIZE 128u
#define NO_SYMBOL 0xFFFFFFFFu
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

#pragma pack(push, 1)
typedef struct NxmHeader {
    char magic[4];
    uint32_t version, name_off, reserved;
    uint64_t image_size, bss_size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t init_array_off, init_array_count;
    uint32_t nreloc, nsym, nhint, strtab_size;
    uint64_t image_offset;
} NxmHeader;
typedef struct NxmReloc { uint64_t where; uint32_t type, symbol; int64_t addend; } NxmReloc;
typedef struct NxmSymbol { uint32_t name, flags; uint64_t value; } NxmSymbol;
#pragma pack(pop)

typedef struct Parsed {
    const NxmHeader *h;
    const NxmReloc *relocs;
    const NxmSymbol *syms;
    const uint64_t *hints;
    const char *strtab;
    const uint8_t *image;
    LoaderSymbol *symbols;   /* names resolved, owned */
} Parsed;

static int err(Loader *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->error, sizeof l->error, fmt, ap);
    va_end(ap);
    return -1;
}

static uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static int read_file(const char *path, void **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    void *p = malloc(n ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return -1; }
    fclose(f);
    *data = p;
    *size = (size_t)n;
    return 0;
}

/* Every table must lie inside the file and every offset inside the module: the file is the player's build output,
   but a damaged one must stop the loader, not write outside the guest's memory. */
static int parse(Loader *l, const char *path, const uint8_t *f, size_t n, Parsed *p) {
    if (n < NXM_HEADER_SIZE) return err(l, "%s: too small", path);
    const NxmHeader *h = (const NxmHeader *)f;
    if (memcmp(h->magic, "NXM1", 4) || h->version != 1) return err(l, "%s: not an NXM1 file", path);
    uint64_t off = NXM_HEADER_SIZE;
    uint64_t rel_size = (uint64_t)h->nreloc * sizeof(NxmReloc), sym_size = (uint64_t)h->nsym * sizeof(NxmSymbol);
    uint64_t hint_size = (uint64_t)h->nhint * 8u;
    uint64_t tables_end = off + rel_size + sym_size + hint_size + h->strtab_size;
    if (tables_end > n || h->image_offset < tables_end || h->image_offset > n || h->image_size > n - h->image_offset)
        return err(l, "%s: tables or image outside the file", path);
    if (h->strtab_size == 0 || f[off + rel_size + sym_size + hint_size + h->strtab_size - 1] != 0)
        return err(l, "%s: string table not terminated", path);
    uint64_t module_end = h->image_size + h->bss_size;
    if (h->text_off != 0 || h->text_off + h->text_size > h->ro_off || h->ro_off + h->ro_size > h->data_off ||
        h->data_off + h->data_size != h->image_size || h->text_size % 4 || module_end < h->image_size)
        return err(l, "%s: bad segment layout", path);
    if (h->init_array_off + h->init_array_count * 8 > h->image_size || h->init_array_off % 8)
        return err(l, "%s: init array outside the image", path);
    p->h = h;
    p->relocs = (const NxmReloc *)(f + off);
    p->syms = (const NxmSymbol *)(f + off + rel_size);
    p->hints = (const uint64_t *)(f + off + rel_size + sym_size);
    p->strtab = (const char *)(f + off + rel_size + sym_size + hint_size);
    p->image = f + h->image_offset;
    if (h->name_off >= h->strtab_size) return err(l, "%s: bad name", path);
    for (uint32_t i = 0; i < h->nreloc; i++) {
        const NxmReloc *r = &p->relocs[i];
        if (r->where < h->data_off || r->where % 8 || r->where + 8 > module_end)
            return err(l, "%s: relocation %u at %#llx outside the data segment", path, i, (unsigned long long)r->where);
        if (r->symbol != NO_SYMBOL && r->symbol >= h->nsym) return err(l, "%s: relocation %u: bad symbol", path, i);
        if (r->type != R_RELATIVE && r->type != R_ABS64 && r->type != R_GLOB_DAT && r->type != R_JUMP_SLOT)
            return err(l, "%s: relocation %u: type %u not supported", path, i, r->type);
        if ((r->type == R_RELATIVE) != (r->symbol == NO_SYMBOL))
            return err(l, "%s: relocation %u: symbol does not match its type", path, i);
    }
    p->symbols = calloc(h->nsym ? h->nsym : 1, sizeof *p->symbols);
    if (!p->symbols) return err(l, "out of memory");
    for (uint32_t i = 0; i < h->nsym; i++) {
        const NxmSymbol *s = &p->syms[i];
        if (s->name >= h->strtab_size) return err(l, "%s: symbol %u: bad name", path, i);
        if ((s->flags & LOADER_SYM_DEFINED) && s->value > module_end)
            return err(l, "%s: symbol %u outside the module", path, i);
        p->symbols[i] = (LoaderSymbol){p->strtab + s->name, s->flags, s->value};
    }
    for (uint32_t i = 0; i < h->nhint; i++)
        if (p->hints[i] >= h->text_size || p->hints[i] % 4) return err(l, "%s: hint %u outside text", path, i);
    return 0;
}

static const LoaderSymbol *find_export(const Loader *l, const char *name, const LoadedModule **owner) {
    for (unsigned m = 0; m < l->count; m++) {
        const LoadedModule *mod = &l->modules[m];
        for (uint32_t i = 0; i < mod->nsym; i++) {
            const LoaderSymbol *s = &mod->symbols[i];
            if ((s->flags & LOADER_SYM_DEFINED) && s->name[0] && !strcmp(s->name, name)) {
                if (owner) *owner = mod;
                return s;
            }
        }
    }
    return NULL;
}

uint64_t loader_symbol(const Loader *l, const char *name) {
    const LoadedModule *owner;
    const LoaderSymbol *s = find_export(l, name, &owner);
    return s ? owner->base + s->value : 0;
}

const char *loader_unresolved_name(const Loader *l, uint32_t k) { return k < l->nunresolved ? l->unresolved[k] : NULL; }

/* A stub: svc #imm; ret. The stub page is written before the recompile and never changes afterwards. */
static uint64_t add_stub(Loader *l, uint32_t imm) {
    uint64_t at = l->stubs + 8ull * l->nstub;
    if (8ull * (l->nstub + 1) > l->stubs_size) return 0;
    uint32_t words[2] = {0xD4000001u | (imm << 5), 0xD65F03C0u};
    memcpy((void *)(uintptr_t)at, words, sizeof words);
    l->nstub++;
    return at;
}

typedef struct StubCache { const char *name; uint64_t address; } StubCache;

static uint64_t resolve_import(Loader *l, const char *name, uint32_t flags, LoaderHostResolve resolve, void *ctx,
                               StubCache *cache, uint32_t *ncache, uint32_t cap, int *error) {
    const LoadedModule *owner;
    const LoaderSymbol *s = find_export(l, name, &owner);
    if (s) { l->imports_module++; return owner->base + s->value; }
    for (uint32_t i = 0; i < *ncache; i++)
        if (!strcmp(cache[i].name, name)) { l->imports_host++; return cache[i].address; }
    int index = resolve ? resolve(name, ctx) : -1;
    uint64_t address;
    if (index >= 0) {
        if ((uint32_t)index > LOADER_SVC_UNRESOLVED - LOADER_SVC_HOST - 1) { *error = err(l, "%s: host index too large", name); return 0; }
        address = add_stub(l, LOADER_SVC_HOST + (uint32_t)index);
        l->imports_host++;
    } else if (flags & LOADER_SYM_WEAK) {
        l->imports_weak_zero++;
        return 0;
    } else {
        if (l->nunresolved >= LOADER_MAX_UNRESOLVED) { *error = err(l, "more than %u unresolved imports", LOADER_MAX_UNRESOLVED); return 0; }
        address = add_stub(l, LOADER_SVC_UNRESOLVED + l->nunresolved);
        l->unresolved[l->nunresolved++] = name;
    }
    if (!address) { *error = err(l, "stub page full"); return 0; }
    if (*ncache < cap) cache[(*ncache)++] = (StubCache){name, address};
    return address;
}

int loader_load(Loader *l, const char *const *paths, unsigned count, uint64_t base, LoaderHostResolve resolve, void *ctx) {
    memset(l, 0, sizeof *l);
    if (count == 0 || count > LOADER_MAX_MODULES) return err(l, "module count %u", count);
    Parsed parsed[LOADER_MAX_MODULES] = {{0}};
    uint64_t page = gm_page_size(), offset = 0;
    uint64_t imports_total = 0;
    for (unsigned i = 0; i < count; i++) {
        LoadedModule *m = &l->modules[i];
        if (read_file(paths[i], &m->file, &m->file_size)) return err(l, "%s: cannot read", paths[i]);
        if (parse(l, paths[i], m->file, m->file_size, &parsed[i])) { loader_free(l); return -1; }
        const NxmHeader *h = parsed[i].h;
        snprintf(m->name, sizeof m->name, "%s", parsed[i].strtab + h->name_off);
        m->image_size = h->image_size; m->bss_size = h->bss_size;
        m->size = round_up(h->image_size + h->bss_size, page);
        m->text_off = h->text_off; m->text_size = h->text_size; m->ro_off = h->ro_off; m->ro_size = h->ro_size;
        m->data_off = h->data_off; m->data_size = h->data_size;
        m->init_array_off = h->init_array_off; m->init_array_count = h->init_array_count;
        m->symbols = parsed[i].symbols; m->nsym = h->nsym;
        m->hints = parsed[i].hints; m->nhint = h->nhint;
        m->base = offset;   /* relative until the reservation is placed */
        offset = round_up(offset + m->size, LOADER_MODULE_ALIGN);
        for (uint32_t s = 0; s < h->nsym; s++) imports_total += !(parsed[i].symbols[s].flags & LOADER_SYM_DEFINED);
    }
    l->count = count;
    l->stubs_size = round_up((imports_total + 1) * 8, page);
    l->size = round_up(offset + l->stubs_size, gm_reserve_granularity());
    if (!base) base = gm_find_free(0x8000000ull, GM_ADDRESS_LIMIT, l->size, LOADER_MODULE_ALIGN);
    if (!base || gm_reserve(base, l->size)) { loader_free(l); return err(l, "cannot reserve %#llx bytes for the modules: %s", (unsigned long long)l->size, gm_error()); }
    l->base = base;
    l->stubs = base + offset;
    for (unsigned i = 0; i < count; i++) {
        LoadedModule *m = &l->modules[i];
        m->base += base;
        if (gm_commit(m->base, m->size, GM_READ | GM_WRITE)) { loader_free(l); return err(l, "%s: %s", m->name, gm_error()); }
        memcpy((void *)(uintptr_t)m->base, parsed[i].image, (size_t)m->image_size);   /* bss stays zero */
    }
    if (gm_commit(l->stubs, l->stubs_size, GM_READ | GM_WRITE)) { loader_free(l); return err(l, "stubs: %s", gm_error()); }

    uint32_t cap = (uint32_t)(imports_total ? imports_total : 1), ncache = 0;
    StubCache *cache = calloc(cap, sizeof *cache);
    if (!cache) { loader_free(l); return err(l, "out of memory"); }
    int error = 0;
    for (unsigned i = 0; i < count && !error; i++) {
        LoadedModule *m = &l->modules[i];
        const Parsed *p = &parsed[i];
        for (uint32_t r = 0; r < p->h->nreloc && !error; r++) {
            const NxmReloc *rel = &p->relocs[r];
            uint64_t value;
            if (rel->type == R_RELATIVE) {
                value = m->base + (uint64_t)rel->addend;
            } else {
                const LoaderSymbol *s = &m->symbols[rel->symbol];
                uint64_t sym = (s->flags & LOADER_SYM_DEFINED)
                                   ? m->base + s->value
                                   : resolve_import(l, s->name, s->flags, resolve, ctx, cache, &ncache, cap, &error);
                value = sym ? sym + (uint64_t)rel->addend : 0;   /* a weak import that stays 0 stays 0 */
            }
            memcpy((void *)(uintptr_t)(m->base + rel->where), &value, 8);
            l->relocs_applied++;
        }
    }
    free(cache);
    if (error) { loader_free(l); return -1; }
    return 0;
}

int loader_seal(Loader *l) {
    uint64_t page = gm_page_size();
    for (unsigned i = 0; i < l->count; i++) {
        LoadedModule *m = &l->modules[i];
        uint64_t ro_end = m->data_off / page * page;   /* data may share rodata's last page: that page stays writable */
        if (ro_end && gm_protect(m->base, ro_end, GM_READ)) return err(l, "%s: %s", m->name, gm_error());
    }
    if (gm_protect(l->stubs, l->stubs_size, GM_READ)) return err(l, "stubs: %s", gm_error());
    return 0;
}

size_t loader_code(const Loader *l, LoaderCodeRange *ranges, uint64_t *hints, size_t max_hints) {
    size_t n = 0;
    for (unsigned i = 0; i < l->count; i++) {
        const LoadedModule *m = &l->modules[i];
        ranges[i] = (LoaderCodeRange){m->base + m->text_off, (const void *)(uintptr_t)(m->base + m->text_off), (size_t)m->text_size};
        for (uint32_t h = 0; h < m->nhint && n < max_hints; h++) hints[n++] = m->base + m->hints[h];
        for (uint32_t s = 0; s < m->nsym && n < max_hints; s++) {
            const LoaderSymbol *sym = &m->symbols[s];
            if ((sym->flags & LOADER_SYM_DEFINED) && (sym->flags & LOADER_SYM_FUNCTION) && sym->value < m->text_size &&
                sym->value % 4 == 0)
                hints[n++] = m->base + sym->value;
        }
        /* constructors (relocated: the slots hold absolute addresses now) */
        for (uint64_t k = 0; k < m->init_array_count && n < max_hints; k++) {
            uint64_t f;
            memcpy(&f, (const void *)(uintptr_t)(m->base + m->init_array_off + 8 * k), 8);
            if (f >= m->base && f < m->base + m->text_size && f % 4 == 0) hints[n++] = f;
        }
    }
    ranges[l->count] = (LoaderCodeRange){l->stubs, (const void *)(uintptr_t)l->stubs, (size_t)(8ull * (l->nstub ? l->nstub : 1))};
    for (uint32_t s = 0; s < l->nstub && n < max_hints; s++) hints[n++] = l->stubs + 8ull * s;
    return n;
}

void loader_free(Loader *l) {
    if (l->base && l->size) gm_release(l->base, l->size);
    for (unsigned i = 0; i < LOADER_MAX_MODULES; i++) {
        free((void *)l->modules[i].symbols);
        free(l->modules[i].file);
        l->modules[i].symbols = NULL;
        l->modules[i].file = NULL;
    }
    l->base = l->size = 0;
}
