/* Synthetic "main" module: imports from sdk (a call through the PLT, a variable through the GOT), calls a host
   function through the bridge, and keeps function pointers in data (RELATIVE / ABS64 relocations). */
extern int sdk_mul(int, int);
extern int sdk_ready;
extern long host_add(long, long);          /* resolved to a bridge stub, not to a module */
static long twice(long v) { return 2 * v; }
static long thrice(long v) { return 3 * v; }
long (*const table[2])(long) = { twice, thrice };
long *self_pointer;
int (*const imported_pointer)(int, int) = sdk_mul;   /* ABS64 against an imported symbol */
static long main_counter;
__attribute__((constructor)) static void main_init(void) {
    __asm__ volatile("" ::: "memory");
    self_pointer = &main_counter;
    main_counter = 5;
}
long test_entry(long x) {
    long r = sdk_mul((int)x, 3);           /* 3x + 42 */
    r = host_add(r, sdk_ready);            /* + 42 on the host */
    r += table[0](x) + table[1](x);        /* + 5x */
    r += imported_pointer(1, 1);           /* + 43 */
    return r + *self_pointer;              /* + 5: 8x + 132 in all */
}
