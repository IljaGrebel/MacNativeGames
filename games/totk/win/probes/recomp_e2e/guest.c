/* Guest code: compiled for AArch64 (Switch-like: cortex-a57, no LSE) and, for reference, natively for x86-64. */
#include <stdint.h>
typedef struct { uint64_t a, b; } H16;
typedef long (*HostFn)(int, float, H16, long);

long guest_calls(HostFn f, long n, long *counter) {
    long acc = 0;
    for (long i = 0; i < n; i++) {
        H16 s = { (uint64_t)i, (uint64_t)(2 * i) };
        acc += f((int)i, (float)i * 0.5f, s, acc);
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);   /* LDAXR/STLXR loop on A57 */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);            /* DMB ISH */
    }
    return acc + *counter;
}

float guest_fp(const float *x, const float *y, long n, long reps) {
    float s = 0.0f;
    for (long r = 0; r < reps; r++)
        for (long i = 0; i < n; i++)
            s += x[i] * y[i] + x[i] / (y[i] + 1.0f);
    return s;
}

uint64_t guest_int(const uint32_t *d, long n, long reps) {
    uint64_t h = 1469598103934665603ull;
    for (long r = 0; r < reps; r++)
        for (long i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return h;
}
