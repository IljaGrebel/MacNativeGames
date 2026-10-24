/* Guest register access for the generated guest -> host bridge (Windows / x86-64 port).
 *
 * The generated adapters (bridge_nvn.c) read arguments and write results only through these macros, so the bridge does
 * not depend on one recompiler's CPU struct. A build defines GuestCpu as its recompiler's CPU state; the default below
 * is a plain struct with the same field names, used by the tests.
 *
 * Guest memory: the port reserves the guest's address ranges at the same host addresses (below 2^39), so a guest
 * pointer is a host pointer and BR_G2H / BR_H2G are identities. A build with a separate guest base overrides them.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef BRIDGE_GUEST_CPU_DEFINED
typedef struct GuestCpu {
    uint64_t x[32];          /* x0..x30, x[31] = SP */
    uint8_t v[32][16];       /* q0..q31 */
} GuestCpu;
#endif

#ifndef BR_G2H
#define BR_G2H(addr) ((void *)(uintptr_t)(addr))
#endif
#ifndef BR_H2G
#define BR_H2G(ptr) ((uint64_t)(uintptr_t)(ptr))
#endif

#define BR_X(c, i) ((c)->x[(i)])
#define BR_SET_X(c, i, val) ((c)->x[(i)] = (uint64_t)(val))

/* Stack-passed arguments: AAPCS64 gives each scalar its own 8-byte slot from SP upwards. */
static inline uint64_t br_stack64(const GuestCpu *c, unsigned slot) {
    uint64_t v; memcpy(&v, BR_G2H(c->x[31] + 8u * slot), sizeof v); return v;
}
static inline float br_stack_f32(const GuestCpu *c, unsigned slot) {
    float v; memcpy(&v, BR_G2H(c->x[31] + 8u * slot), sizeof v); return v;
}
static inline double br_stack_f64(const GuestCpu *c, unsigned slot) {
    double v; memcpy(&v, BR_G2H(c->x[31] + 8u * slot), sizeof v); return v;
}
#define BR_STACK64(c, slot) br_stack64((c), (slot))
#define BR_STACK_F32(c, slot) br_stack_f32((c), (slot))
#define BR_STACK_F64(c, slot) br_stack_f64((c), (slot))

/* Integer-only composites of at most 16 bytes travel in one or two consecutive GPRs (AAPCS64 B.4 / C.10),
   or in consecutive 8-byte stack slots. `buf` must hold 16 bytes. */
static inline void *br_load_struct(const GuestCpu *c, unsigned reg, uint64_t *buf, unsigned size) {
    buf[0] = c->x[reg];
    buf[1] = size > 8 ? c->x[reg + 1] : 0;
    return buf;
}
static inline void *br_load_struct_stack(const GuestCpu *c, unsigned slot, uint64_t *buf, unsigned size) {
    memcpy(buf, BR_G2H(c->x[31] + 8u * slot), size);
    return buf;
}
static inline void br_store_struct(GuestCpu *c, const void *src, unsigned size) {
    uint64_t buf[2] = {0, 0};
    memcpy(buf, src, size);
    c->x[0] = buf[0];
    if (size > 8) c->x[1] = buf[1];
}

/* Homogeneous floating-point aggregates (up to four float or double members) travel one member per V register
   (AAPCS64 B.6 / C.4): member k in the low bits of v[first + k]. */
static inline void *br_load_hfa(const GuestCpu *c, unsigned first, unsigned count, unsigned lane, uint64_t *buf) {
    for (unsigned k = 0; k < count; k++) memcpy((uint8_t *)buf + k * lane, &c->v[first + k], lane);
    return buf;
}
static inline void br_store_hfa(GuestCpu *c, const void *src, unsigned count, unsigned lane) {
    for (unsigned k = 0; k < count; k++) {
        memset(&c->v[k], 0, sizeof c->v[k]);
        memcpy(&c->v[k], (const uint8_t *)src + k * lane, lane);
    }
}

/* Floating-point arguments and results live in the low bits of v0..v7. */
static inline float br_s(const GuestCpu *c, unsigned i) { float v; memcpy(&v, &c->v[i], sizeof v); return v; }
static inline double br_d(const GuestCpu *c, unsigned i) { double v; memcpy(&v, &c->v[i], sizeof v); return v; }
static inline void br_set_s(GuestCpu *c, unsigned i, float v) { memset(&c->v[i], 0, sizeof c->v[i]); memcpy(&c->v[i], &v, sizeof v); }
static inline void br_set_d(GuestCpu *c, unsigned i, double v) { memset(&c->v[i], 0, sizeof c->v[i]); memcpy(&c->v[i], &v, sizeof v); }
#define BR_S(c, i) br_s((c), (i))
#define BR_D(c, i) br_d((c), (i))
#define BR_SET_S(c, i, v) br_set_s((c), (i), (v))
#define BR_SET_D(c, i, v) br_set_d((c), (i), (v))

typedef struct BridgeEntry {
    const char *name;               /* the guest import name */
    void (*adapter)(GuestCpu *c);   /* reads AAPCS64 arguments, calls the host, writes the result */
    int has_callback_args;          /* a parameter is a guest function address: the host must call it through the CPU */
} BridgeEntry;

/* Called by an adapter whose host function is not implemented. Supplied by the runtime. */
void bridge_missing(const char *name);

extern const BridgeEntry bridge_nvn_table[];
extern const unsigned bridge_nvn_count;
unsigned bridge_nvn_bind(void *(*resolve)(const char *name));

#define BRIDGE_SVC_BASE 0x1000u
/* The two words of guest stub `index`: svc #(BRIDGE_SVC_BASE + index); ret. */
static inline void bridge_stub_words(unsigned index, uint32_t out[2]) {
    out[0] = 0xD4000001u | ((BRIDGE_SVC_BASE + index) << 5);
    out[1] = 0xD65F03C0u;
}
