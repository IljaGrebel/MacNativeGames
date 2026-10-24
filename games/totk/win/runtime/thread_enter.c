/* Guest thread entry and exit for the upstream thread runtime (runtime/threads.c), replacing the Mac's
 * thread_enter.S: dm_thread_enter runs the thread's entry and never returns; ExitThread ends in runtime_thread_leave,
 * which jumps back into dm_thread_enter's frame and finishes the thread on the host side. The recompiled frames of the
 * thread are abandoned, not unwound (__builtin_longjmp restores registers only), so this works through code that has
 * no unwind information. host_thread_guest_entry (host_runtime.c) runs entries that are guest code; any other entry
 * is a host function (the runtime's own thread tests). */
#include <stdint.h>
#include <stdlib.h>

_Noreturn void runtime_fail(const char *message);
void dm_thread_returned(void);

int (*host_thread_guest_entry)(uint64_t entry, uint64_t arg, uint64_t sp);

typedef struct ThreadExit { void *jump[5]; void (*fn)(void *); void *arg; } ThreadExit;
static _Thread_local ThreadExit g_exit;

/* After the long jump nothing of dm_thread_enter's frame or registers is trusted: on Win64 a register holding the
   thread-local block's address is not restored (found under Wine: g_exit read as zeros), so a separate function
   computes the thread-local address afresh. */
__attribute__((noinline)) static _Noreturn void finish_exit(void) {
    ThreadExit *e = &g_exit;
    e->fn(e->arg);
    runtime_fail("thread exit returned");
}

void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp, uint64_t *host_context) {
    host_context[0] = (uint64_t)(uintptr_t)g_exit.jump;   /* nonzero: threads.c then leaves through us */
    if (__builtin_setjmp(g_exit.jump) == 0) {
        if (!(host_thread_guest_entry && host_thread_guest_entry(entry, arg, sp)))
            ((void (*)(uint64_t))(uintptr_t)entry)(arg);
        dm_thread_returned();
    }
    finish_exit();
}

_Noreturn void runtime_thread_leave(const uint64_t *host_context, void *arg, void (*fn)(void *)) {
    g_exit.fn = fn;
    g_exit.arg = arg;
    __builtin_longjmp((void **)(uintptr_t)host_context[0], 1);
}
