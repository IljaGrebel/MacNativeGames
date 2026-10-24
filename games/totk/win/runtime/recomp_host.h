/* The recompiler behind the Windows port: recompiles the loaded modules' code ahead of time and runs guest functions.
 *
 * This is the only file that talks to ScarletNX's recompiler library (an external GPL-3.0 dependency, see
 * games/totk/win/README.md); everything else sees the guest CPU state through bridge_abi.h.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "module_loader.h"

struct CPU;
typedef void (*RecompSvc)(struct CPU *cpu, uint32_t imm);

typedef struct RecompHost {
    struct snx_recompiler *r;
    struct snx_recompiled *m;
    void *rt;                 /* RtAPI, owned */
    size_t functions;
    double seconds;
    char error[256];
} RecompHost;

/* Recompiles every module's text and the loader's stub page. cache_dir (may be NULL) keeps the linked code, keyed by
   everything it is made from, so a second start loads it instead of compiling. */
int recomp_build(RecompHost *h, const Loader *l, const char *cache_dir, unsigned jobs);
/* Points a CPU state at the recompiled code with the given SVC handler. thread: the caller's per-thread context. */
int recomp_cpu_init(RecompHost *h, struct CPU *cpu, RecompSvc svc, void *thread);
/* Calls the guest function at `fn` with up to 8 integer arguments on the stack top `sp`, until it returns.
   Returns x0. */
uint64_t recomp_call(RecompHost *h, struct CPU *cpu, uint64_t fn, const uint64_t *args, unsigned nargs, uint64_t sp);
/* Runs every module's init array, dependencies first (the reverse of load order, so main runs last). */
void recomp_run_init(RecompHost *h, struct CPU *cpu, const Loader *l, uint64_t sp);
void recomp_free(RecompHost *h);
