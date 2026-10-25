/* The Windows port's platform layer for milestone M0: no window, no graphics, no input device, silent audio.
 *
 * It provides what the upstream runtime expects from frontend.m, gamepad.m, gfx_bridge.c and the Metal NVN layer, so
 * the game boots to its first graphics calls:
 *   - graphics: every NVN function and the nv:: setup calls are host functions that log their call and return a
 *     neutral value (Initialize functions succeed, getters return 0). nvnBootstrapLoader and nvnDeviceGetProcAddress
 *     hand out the guest stubs of these functions, as the real NVN layer hands out its implementations. The Vulkan
 *     NVN layer (M1) replaces this file's graphics half.
 *   - display (nn::vi): the overrides frontend.m has, without a window; the vsync event is signalled at 60 Hz;
 *   - window and boot screen: progress goes to the log;
 *   - input: no controller (neutral state); rumble does nothing;
 *   - movies and Opus: not overridden (the game's own decoders run).
 */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_runtime.h"
#include "runtime.h"

/* ---- graphics trace (NVN) ---- */

static const char *const g_nvn_names[] = {
#include "nvn_names.inc"
    "nvnBootstrapLoader",
    "_ZN2nv18InitializeGraphicsEPvm",
    "_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",
    "_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",
};
#define NNVN (sizeof g_nvn_names / sizeof *g_nvn_names)
static char g_nvn_token[NNVN];          /* &g_nvn_token[i] is what runtime_gfx_override returns for name i */
static int g_nvn_index[NNVN];
static _Atomic unsigned long g_nvn_calls;
static _Atomic unsigned g_nvn_logged;

static unsigned trace_limit(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_WIN_NVN_TRACE"); v = e ? atoi(e) : 400; }
    return (unsigned)v;
}

static uint64_t nvn_stub_of(const char *name) {
    for (unsigned i = 0; i < NNVN; i++)
        if (!strcmp(g_nvn_names[i], name)) return host_runtime_stub((uint32_t)g_nvn_index[i]);
    return 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

static void nvn_trace_adapter(GuestCpu *c) {
    const char *name = host_runtime_calling();
    unsigned long n = atomic_fetch_add(&g_nvn_calls, 1) + 1;
    uint64_t result = 0;
    if (!strcmp(name, "nvnBootstrapLoader")) {
        result = nvn_stub_of((const char *)(uintptr_t)c->x[0]);
    } else if (!strcmp(name, "nvnDeviceGetProcAddress")) {
        result = nvn_stub_of((const char *)(uintptr_t)c->x[1]);
    } else if (ends_with(name, "Initialize")) {
        result = 1;   /* NVNboolean: success */
    }
    if (atomic_fetch_add(&g_nvn_logged, 1) < trace_limit()) {
        const char *arg = !strcmp(name, "nvnBootstrapLoader") ? (const char *)(uintptr_t)c->x[0]
                        : !strcmp(name, "nvnDeviceGetProcAddress") ? (const char *)(uintptr_t)c->x[1] : NULL;
        fprintf(stderr, "[nvn] #%lu %s(%#llx, %#llx, %#llx)%s%s%s -> %#llx\n", n, name, (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], arg ? " \"" : "", arg ? arg : "", arg ? "\"" : "",
                (unsigned long long)result);
    }
    c->x[0] = result;
}

void *runtime_gfx_override(const char *name) {
    for (unsigned i = 0; i < NNVN; i++)
        if (!strcmp(g_nvn_names[i], name)) return &g_nvn_token[i];
    return NULL;
}

/* ---- display (nn::vi): what frontend.m overrides, so the SDK never opens vi:m ---- */

enum { VI_INIT, VI_OPEN, VI_LAYER, VI_WINDOW, VI_VSYNC, VI_SCALING, NVI };
static const char *const g_vi_names[NVI] = {
    "_ZN2nn2vi10InitializeEv",
    "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",
    "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",
    "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",
    "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",
    "_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",
};
void hle_signal_system_event(void *event);   /* threads.c */
static char g_vi_token[NVI];
/* handles the game holds: display, layer and native window (the window goes to nvnWindowBuilderSetNativeWindow) */
static uint64_t g_display, g_layer, g_window;
static pthread_mutex_t g_vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_vsync_events[16];
static unsigned g_vsync_count;
#define VI_RESULT_INVALID 0xf601u   /* what frontend.m returns for a bad argument */

static void *vsync_thread(void *arg) {
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 1000000000L / 60;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&g_vsync_lock);
        for (unsigned i = 0; i < g_vsync_count; i++) hle_signal_system_event(g_vsync_events[i]);
        pthread_mutex_unlock(&g_vsync_lock);
    }
    return NULL;
}

static void vi_adapter(GuestCpu *c) {
    const char *name = host_runtime_calling();
    unsigned i = 0;
    while (i < NVI && strcmp(g_vi_names[i], name)) i++;
    uint64_t *out = (uint64_t *)(uintptr_t)c->x[0];
    uint32_t r = 0;
    switch (i) {
    case VI_INIT: fprintf(stderr, "[totk-win] VI display initialized (no window in M0)\n"); break;
    case VI_OPEN: if (out) *out = (uintptr_t)&g_display; else r = VI_RESULT_INVALID; break;
    case VI_LAYER: if (out && c->x[1] == (uintptr_t)&g_display) *out = (uintptr_t)&g_layer; else r = VI_RESULT_INVALID; break;
    case VI_WINDOW: if (out && c->x[1] == (uintptr_t)&g_layer) *out = (uintptr_t)&g_window; else r = VI_RESULT_INVALID; break;
    case VI_VSYNC: {
        void *ev = (void *)(uintptr_t)c->x[0];
        if (!ev || c->x[1] != (uintptr_t)&g_display) { r = VI_RESULT_INVALID; break; }
        hle_bind_system_event(ev, 1);   /* on the guest thread, as the SDK code it runs needs */
        pthread_mutex_lock(&g_vsync_lock);
        if (g_vsync_count >= 16) runtime_fail("VI display event limit");
        g_vsync_events[g_vsync_count++] = ev;
        int first = g_vsync_count == 1;
        pthread_mutex_unlock(&g_vsync_lock);
        pthread_t t;
        if (first && (pthread_create(&t, NULL, vsync_thread, NULL) || pthread_detach(t))) runtime_fail("vsync thread");
        break;
    }
    case VI_SCALING: break;
    default: runtime_fail("VI: unknown function");
    }
    fprintf(stderr, "[totk-win] vi %s(%#llx, %#llx) -> %#x\n", name, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], r);
    c->x[0] = r;
}

void host_runtime_register_platform(void) {
    for (unsigned i = 0; i < NNVN; i++)
        g_nvn_index[i] = (int)host_runtime_register(g_nvn_names[i], &g_nvn_token[i], nvn_trace_adapter);
    for (unsigned i = 0; i < NVI; i++) host_runtime_register(g_vi_names[i], &g_vi_token[i], vi_adapter);
}

unsigned long host_platform_nvn_calls(void) { return atomic_load(&g_nvn_calls); }

/* graphics diagnostics the input file and shortcuts reach (no-ops without a renderer) */
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
void dm_gfx_shot_at(long frames, const char *path) { (void)frames; (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_anomdump(void) {}
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
void taa_toggle(void) {}
int mfxup_output_enabled(void) { return 0; }

/* ---- window and boot screen ---- */

void runtime_boot_status(const char *stage, const char *detail) { fprintf(stderr, "[boot] %s: %s\n", stage, detail ? detail : ""); }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    static uint64_t last;
    if (total && (done == total || done - last >= total / 20)) {
        last = done;
        fprintf(stderr, "[boot] %s %llu/%llu\n", stage, (unsigned long long)done, (unsigned long long)total);
    }
}
void *runtime_front_override(const char *name) {
    for (unsigned i = 0; i < NVI; i++)
        if (!strcmp(g_vi_names[i], name)) return &g_vi_token[i];
    return NULL;
}
int runtime_front_focus(void) { return 1; }   /* nn::oe focus state: in focus */
void runtime_front_request_close(void) {}
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
void runtime_shortcut_help_toggle(void) {}
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    (void)req;
    if (out_size) out[0] = 0;
    return 0;   /* cancelled: no keyboard window yet */
}

/* ---- input ---- */

void runtime_gamepad_read(RuntimeGamepadState *out) { memset(out, 0, sizeof *out); }
void runtime_gamepad_clear(void) {}
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)side; (void)amp_low; (void)freq_low; (void)amp_high; (void)freq_high;
}

/* ---- codecs: the game's own ---- */

void *runtime_movie_override(const char *name) { (void)name; return NULL; }
void *runtime_codec_override(const char *name) { (void)name; return NULL; }
