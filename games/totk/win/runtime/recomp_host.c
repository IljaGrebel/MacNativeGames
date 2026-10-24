/* The recompiler behind the Windows port: see recomp_host.h. */
#include "recomp_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <scarletnx/snx_recompiler.h>

static double now_seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void stop_undef(CPU *c, uint64_t pc, uint32_t word) {
    fprintf(stderr, "guest: undefined instruction %08x at %#llx\n", word, (unsigned long long)pc);
    c->pc = 0;
}
static void stop_brk(CPU *c, uint64_t pc, uint32_t imm) {
    fprintf(stderr, "guest: brk #%u at %#llx\n", imm, (unsigned long long)pc);
    c->pc = 0;
}

int recomp_build(RecompHost *h, const Loader *l, const char *cache_dir, unsigned jobs) {
    memset(h, 0, sizeof *h);
    LoaderCodeRange ranges[LOADER_MAX_MODULES + 1];
    size_t max_hints = 0;
    for (unsigned i = 0; i < l->count; i++)
        max_hints += l->modules[i].nhint + l->modules[i].nsym + l->modules[i].init_array_count;
    max_hints += l->nstub + 1;
    uint64_t *hints = malloc(max_hints * sizeof *hints);
    struct snx_code_range *sr = calloc(l->count + 1, sizeof *sr);
    if (!hints || !sr) { free(hints); free(sr); snprintf(h->error, sizeof h->error, "out of memory"); return -1; }
    size_t nhints = loader_code(l, ranges, hints, max_hints);
    for (unsigned i = 0; i <= l->count; i++) sr[i] = (struct snx_code_range){ranges[i].address, ranges[i].code, ranges[i].size};

    h->r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!h->r || !snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM)) {
        snprintf(h->error, sizeof h->error, "the recompiler cannot target this host");
        free(hints); free(sr);
        return -1;
    }
    if (jobs) snx_recompiler_set_jobs(h->r, jobs);
    if (cache_dir) snx_recompiler_set_cache(h->r, cache_dir);
    double t0 = now_seconds();
    h->m = snx_recompile_ranges(h->r, sr, l->count + 1, hints, nhints);
    h->seconds = now_seconds() - t0;
    free(hints); free(sr);
    if (!h->m) { snprintf(h->error, sizeof h->error, "recompile: %s", snx_recompiler_error(h->r)); return -1; }
    h->functions = snx_recompiled_functions(h->m);
    RtAPI *rt = calloc(1, sizeof *rt);
    if (!rt) { snprintf(h->error, sizeof h->error, "out of memory"); return -1; }
    snx_runtime_defaults(rt);
    rt->undef = stop_undef;
    rt->brk = stop_brk;
    h->rt = rt;
    return 0;
}

int recomp_cpu_init(RecompHost *h, CPU *cpu, RecompSvc svc, void *thread) {
    RtAPI *rt = h->rt;
    rt->svc = svc;
    memset(cpu, 0, sizeof *cpu);
    cpu->mem = NULL;          /* guest addresses are host addresses */
    cpu->rt = rt;
    cpu->thread = thread;
    if (snx_recompiled_bind(h->m, cpu)) { snprintf(h->error, sizeof h->error, "bind: CPU hooks missing"); return -1; }
    return 0;
}

uint64_t recomp_call(RecompHost *h, CPU *cpu, uint64_t fn, const uint64_t *args, unsigned nargs, uint64_t sp) {
    for (unsigned i = 0; i < 8; i++) cpu->x[i] = i < nargs ? args[i] : 0;
    cpu->x[30] = 0;           /* returning to 0 ends the run */
    cpu->x[31] = sp;
    snx_recompiled_run(h->m, cpu, fn, 0);
    return cpu->x[0];
}

void recomp_run_init(RecompHost *h, CPU *cpu, const Loader *l, uint64_t sp) {
    for (unsigned i = l->count; i-- > 0;) {
        const LoadedModule *m = &l->modules[i];
        for (uint64_t k = 0; k < m->init_array_count; k++) {
            uint64_t f;
            memcpy(&f, (const void *)(uintptr_t)(m->base + m->init_array_off + 8 * k), 8);
            if (f) recomp_call(h, cpu, f, NULL, 0, sp);
        }
    }
}

void recomp_free(RecompHost *h) {
    if (h->m) snx_recompiled_free(h->m);
    if (h->r) snx_recompiler_free(h->r);
    free(h->rt);
    memset(h, 0, sizeof *h);
}
