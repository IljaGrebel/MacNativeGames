/* Synthetic "sdk" for the boot test: a stand-in for nn::init::Start that walks the same host paths the real SDK's
   startup takes - the host's callbacks, module constructors, Horizon SVCs (GetInfo, SetHeapSize, CreateThread,
   StartThread, WaitSynchronization, OutputDebugString), a host replacement (nn::oe::GetDesiredLanguage) and the NVN
   entry (nvnBootstrapLoader, then nvnDeviceGetProcAddress). It reports one bit per step through OutputDebugString. */
#include <stdint.h>

typedef void (*Fn)(void);
extern long boot_main_value(void);                 /* main */
extern int main_constructed;                       /* main, set by its constructor */
extern void worker(uint64_t arg);                  /* main: a guest thread entry */
extern void *nvnBootstrapLoader(const char *name); /* the host's, though the "sdk" could define it */
extern uint64_t _ZN2nn2oe18GetDesiredLanguageEv(void);

int sdk_constructed;
__attribute__((constructor)) static void sdk_ctor(void) { __asm__ volatile("" ::: "memory"); sdk_constructed = 1; }

static uint32_t svc_get_info(uint64_t *out, uint32_t id, uint32_t handle, uint64_t sub) {
    register uint64_t x0 __asm__("x0"), x1 __asm__("x1") = id, x2 __asm__("x2") = handle, x3 __asm__("x3") = sub;
    __asm__ volatile("svc #0x29" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3) : "memory");
    *out = x1;
    return (uint32_t)x0;
}
static uint32_t svc_set_heap_size(uint64_t *out, uint64_t size) {
    register uint64_t x0 __asm__("x0"), x1 __asm__("x1") = size;
    __asm__ volatile("svc #0x01" : "=r"(x0), "+r"(x1) : : "memory");
    *out = x1;
    return (uint32_t)x0;
}
static uint32_t svc_create_thread(uint32_t *out, uint64_t entry, uint64_t arg, uint64_t sp, int prio, int core) {
    register uint64_t x0 __asm__("x0"), x1 __asm__("x1") = entry, x2 __asm__("x2") = arg, x3 __asm__("x3") = sp,
        x4 __asm__("x4") = (uint64_t)prio, x5 __asm__("x5") = (uint64_t)(int64_t)core;
    __asm__ volatile("svc #0x08" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    *out = (uint32_t)x1;
    return (uint32_t)x0;
}
static uint32_t svc_start_thread(uint32_t handle) {
    register uint64_t x0 __asm__("x0") = handle;
    __asm__ volatile("svc #0x09" : "+r"(x0) : : "memory");
    return (uint32_t)x0;
}
static uint32_t svc_wait(int32_t *index, const uint32_t *handles, int32_t count, int64_t timeout) {
    register uint64_t x0 __asm__("x0"), x1 __asm__("x1") = (uint64_t)handles, x2 __asm__("x2") = (uint64_t)count,
        x3 __asm__("x3") = (uint64_t)timeout;
    __asm__ volatile("svc #0x18" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3) : "memory");
    *index = (int32_t)x1;
    return (uint32_t)x0;
}
static void svc_output(const char *s, uint64_t n) {
    register uint64_t x0 __asm__("x0") = (uint64_t)s, x1 __asm__("x1") = n;
    __asm__ volatile("svc #0x27" : "+r"(x0) : "r"(x1) : "memory");
}

uint64_t _ZN2nn4init5StartEmmPFvvES2_S2_(uint64_t main_handle, uint64_t arg, Fn ready, Fn inits, Fn finis) {
    (void)arg; (void)finis;
    uint32_t ok = 0;
    ready();
    inits();
    if (sdk_constructed && main_constructed) ok |= 1;
    uint64_t pid = 0;
    if (svc_get_info(&pid, 18, 0, 0) == 0 && pid == 0x0100f2c0115b6000ull) ok |= 2;
    uint64_t heap = 0;
    if (svc_set_heap_size(&heap, 0x400000) == 0 && heap) { *(volatile uint64_t *)heap = 42; ok |= 4; }
    if (heap) {
        volatile uint64_t *flag = (volatile uint64_t *)(heap + 64);
        *flag = 0;
        uint32_t th = 0;
        int32_t index = -1;
        if (svc_create_thread(&th, (uint64_t)worker, (uint64_t)flag, heap + 0x200000, 44, 0) == 0 &&
            svc_start_thread(th) == 0 && svc_wait(&index, &th, 1, 2000000000) == 0 && index == 0 && *flag == 0xC0FFEE)
            ok |= 8;
    }
    if (_ZN2nn2oe18GetDesiredLanguageEv()) ok |= 16;
    void *(*get_proc)(void *, const char *) = (void *(*)(void *, const char *))nvnBootstrapLoader("nvnDeviceGetProcAddress");
    if (get_proc) {
        unsigned char (*init)(void *, void *) = (unsigned char (*)(void *, void *))get_proc((void *)0, "nvnDeviceInitialize");
        static char device[0x3000], builder[0x40];
        if (init && init(device, builder) == 1) ok |= 32;
    }
    if (boot_main_value() == 1234 && main_handle) ok |= 64;
    char msg[] = "BOOTTEST ok=00\n";
    msg[12] = "0123456789abcdef"[(ok >> 4) & 15];
    msg[13] = "0123456789abcdef"[ok & 15];
    svc_output(msg, sizeof msg - 1);
    return ok;
}
