/* Synthetic "main" for the boot test: a constructor, a value the "sdk" asks for, and a guest thread entry that
   reports through memory and leaves with ExitThread. */
#include <stdint.h>

int main_constructed;
__attribute__((constructor)) static void main_ctor(void) { __asm__ volatile("" ::: "memory"); main_constructed = 1; }

long boot_main_value(void) { return 1234; }

void worker(uint64_t arg) {
    *(volatile uint64_t *)arg = 0xC0FFEE;
    __asm__ volatile("svc #0x0a" ::: "memory");   /* ExitThread: does not return */
    for (;;) {}
}
