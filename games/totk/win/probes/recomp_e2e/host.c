#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <scarletnx/snx_recompiler.h>
#include "snx_guestcpu.h"
#include "bridge_protos.h"
#include "bridge_abi.h"

/* native references: the same guest.c compiled for x86-64 with ref_ prefixes */
long ref_calls(NVNT_host_fn f, long n, long *counter);
float ref_fp(const float *x, const float *y, long n, long reps);
uint64_t ref_int(const uint32_t *d, long n, long reps);

#define GUEST_BASE 0x40000000ull          /* guest addresses == host addresses (cpu.mem = NULL) */
#define GUEST_SIZE (64ull << 20)
static uint64_t OFF_CALLS, OFF_FP, OFF_INT;   /* entry offsets in the .text image, from the symbol table (run.sh) */

static long host_fn(int i, float x, H16 s, long acc) { return i + (long)(x * 4.0f) + (long)s.a + (long)s.b + (acc & 7); }
static void *resolve(const char *name) { return strcmp(name, "host_fn") ? NULL : (void *)host_fn; }
void bridge_missing(const char *name) { fprintf(stderr, "missing %s\n", name); exit(2); }

static unsigned long long svc_count;
static void my_svc(CPU *c, uint32_t imm) {
    if (imm >= BRIDGE_SVC_BASE && imm - BRIDGE_SVC_BASE < bridge_nvn_count) { svc_count++; bridge_nvn_table[imm - BRIDGE_SVC_BASE].adapter(c); return; }
    fprintf(stderr, "unexpected svc %u\n", imm); exit(3);
}
static void my_undef(CPU *c, uint64_t pc, uint32_t w) { fprintf(stderr, "undef %llx %08x\n", (unsigned long long)pc, w); c->pc = 0; }
static void my_brk(CPU *c, uint64_t pc, uint32_t imm) { (void)pc; (void)imm; c->pc = 0; }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s guest.bin off_calls off_fp off_int\n", argv[0]); return 1; }
    OFF_CALLS = strtoull(argv[2], NULL, 16); OFF_FP = strtoull(argv[3], NULL, 16); OFF_INT = strtoull(argv[4], NULL, 16);
    FILE *f = fopen(argv[1], "rb"); static uint8_t code[1 << 16]; size_t code_size = fread(code, 1, sizeof code, f); fclose(f);
    uint8_t *g = mmap((void *)GUEST_BASE, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g != (uint8_t *)GUEST_BASE) { perror("reserve guest range"); return 1; }
    size_t stub_off = (code_size + 15) & ~15ull;
    memcpy(g, code, code_size);
    uint32_t stub[2]; bridge_stub_words(0, stub); memcpy(g + stub_off, stub, sizeof stub);
    uint64_t stub_addr = GUEST_BASE + stub_off;
    if (bridge_nvn_bind(resolve) != 0) return 1;

    struct snx_recompiler *r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM); if (getenv("KEEP_IR")) snx_recompiler_set_keep_ir(r, getenv("KEEP_IR"));
    struct snx_code_range range = { GUEST_BASE, g, stub_off + sizeof stub };
    uint64_t hints[] = { GUEST_BASE + OFF_CALLS, GUEST_BASE + OFF_FP, GUEST_BASE + OFF_INT, stub_addr };
    double t0 = now();
    struct snx_recompiled *m = snx_recompile_ranges(r, &range, 1, hints, 4);
    if (!m) { fprintf(stderr, "recompile: %s\n", snx_recompiler_error(r)); return 1; }
    printf("recompiled %zu functions in %.2f s, fallbacks %zu\n", snx_recompiled_functions(m), now() - t0, snx_recompiled_fallbacks(m));

    RtAPI rt; snx_runtime_defaults(&rt); rt.svc = my_svc; rt.undef = my_undef; rt.brk = my_brk;
    static CPU cpu; memset(&cpu, 0, sizeof cpu);
    cpu.mem = NULL; cpu.rt = &rt;
    if (snx_recompiled_bind(m, &cpu) != 0) { fprintf(stderr, "bind failed\n"); return 1; }
    uint64_t stack_top = GUEST_BASE + GUEST_SIZE - 0x1000;
    int fails = 0;

    /* 1. host calls through the bridge, atomics, barriers */
    long n = 2000000;
    uint64_t counter_addr = GUEST_BASE + 0x100000; *(long *)counter_addr = 0;
    memset(cpu.x, 0, sizeof cpu.x);
    cpu.x[0] = stub_addr; cpu.x[1] = n; cpu.x[2] = counter_addr; cpu.x[30] = 0; cpu.x[31] = stack_top;
    t0 = now(); snx_recompiled_run(m, &cpu, GUEST_BASE + OFF_CALLS, 0); double t_rc = now() - t0;
    long got = (long)cpu.x[0];
    long ref_counter = 0; t0 = now(); long want = ref_calls(host_fn, n, &ref_counter); double t_nat = now() - t0;
    printf("calls: %s (got %ld want %ld, svc %llu)  recompiled %.1f ns/call, native %.1f ns/call\n", got == want ? "OK" : "FAIL",
           got, want, svc_count, t_rc / n * 1e9, t_nat / n * 1e9);
    fails += got != want;

    /* 2. FP (vectorised NEON with fdiv) */
    long len = 4096, reps = 2000;
    float *x = (float *)(GUEST_BASE + 0x200000), *y = (float *)(GUEST_BASE + 0x300000);
    for (long i = 0; i < len; i++) { x[i] = (float)(i % 97) * 0.25f + 0.1f; y[i] = (float)(i % 89) * 0.5f + 0.3f; }
    memset(cpu.x, 0, sizeof cpu.x);
    cpu.x[0] = (uint64_t)x; cpu.x[1] = (uint64_t)y; cpu.x[2] = len; cpu.x[3] = reps; cpu.x[30] = 0; cpu.x[31] = stack_top;
    t0 = now(); snx_recompiled_run(m, &cpu, GUEST_BASE + OFF_FP, 0); t_rc = now() - t0;
    float gf; memcpy(&gf, &cpu.v[0], 4);
    t0 = now(); float wf = ref_fp(x, y, len, reps); t_nat = now() - t0;
    printf("fp:    %s (got %.6g want %.6g)  recompiled %.3f s, native %.3f s, ratio %.2fx\n", gf == wf ? "OK" : "DIFF", gf, wf, t_rc, t_nat, t_rc / t_nat);

    /* 3. integer */
    uint32_t *d = (uint32_t *)(GUEST_BASE + 0x400000);
    for (long i = 0; i < len; i++) d[i] = (uint32_t)(i * 2654435761u);
    memset(cpu.x, 0, sizeof cpu.x);
    cpu.x[0] = (uint64_t)d; cpu.x[1] = len; cpu.x[2] = reps; cpu.x[30] = 0; cpu.x[31] = stack_top;
    t0 = now(); snx_recompiled_run(m, &cpu, GUEST_BASE + OFF_INT, 0); t_rc = now() - t0;
    uint64_t gi = cpu.x[0];
    t0 = now(); uint64_t wi = ref_int(d, len, reps); t_nat = now() - t0;
    printf("int:   %s  recompiled %.3f s, native %.3f s, ratio %.2fx\n", gi == wi ? "OK" : "FAIL", t_rc, t_nat, t_rc / t_nat);
    fails += gi != wi;
    fails += gf != wf;
    return fails != 0;
}
