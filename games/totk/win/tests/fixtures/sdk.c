/* Synthetic "sdk" module: exports a function and a variable that a constructor sets. */
int sdk_ready;
int sdk_mul(int a, int b) { return a * b + sdk_ready; }
/* the asm keeps the compiler from evaluating the constructor at build time: it must run from .init_array */
__attribute__((constructor)) static void sdk_init(void) { __asm__ volatile("" ::: "memory"); sdk_ready = 42; }
