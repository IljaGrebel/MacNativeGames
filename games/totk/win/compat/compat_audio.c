/* The output unit behind the AudioToolbox stand-in. M0 has no audio device yet: a clock thread pulls 5 ms of frames at
   real time and discards them, so the mixer and the game's audio renderer run exactly as with a device. The device
   (WASAPI on Windows) replaces the clock later behind the same calls. */
#include "win_compat.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <AudioToolbox/AudioToolbox.h>

struct CompatAudioComponent { int unused; };
struct CompatAudioUnit {
    AURenderCallbackStruct cb;
    AudioStreamBasicDescription format;
    pthread_t thread;
    volatile int running, started;
};
static const struct CompatAudioComponent g_component;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    if (after || d->componentType != kAudioUnitType_Output) return NULL;
    return &g_component;
}

OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_component) return -1;
    *out = calloc(1, sizeof **out);
    return *out ? noErr : -1;
}

OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { AudioOutputUnitStop(u); free(u); return noErr; }

OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 element, const void *data, UInt32 size) {
    (void)scope; (void)element;
    if (id == kAudioUnitProperty_StreamFormat && size == sizeof u->format) { memcpy(&u->format, data, size); return noErr; }
    if (id == kAudioUnitProperty_SetRenderCallback && size == sizeof u->cb) { memcpy(&u->cb, data, size); return noErr; }
    return -1;
}

OSStatus AudioUnitInitialize(AudioUnit u) { return u->cb.inputProc && u->format.mSampleRate > 0 ? noErr : -1; }
OSStatus AudioUnitUninitialize(AudioUnit u) { (void)u; return noErr; }

static void *clock_thread(void *p) {
    AudioUnit u = p;
    const UInt32 frames = (UInt32)(u->format.mSampleRate / 200);   /* 5 ms */
    float *buf = calloc((size_t)frames * 2, sizeof *buf);
    uint64_t next = compat_now_ns(), period = 5000000;
    double sample = 0;
    while (u->running && buf) {
        AudioBufferList list = {1, {{2, frames * 8, buf}}};
        AudioTimeStamp ts = {sample, next};
        AudioUnitRenderActionFlags flags = 0;
        u->cb.inputProc(u->cb.inputProcRefCon, &flags, &ts, 0, frames, &list);
        sample += frames;
        next += period;
        uint64_t now = compat_now_ns();
        if (next > now) {
            struct timespec t = {(time_t)((next - now) / 1000000000u), (long)((next - now) % 1000000000u)};
            nanosleep(&t, NULL);
        } else if (now - next > 100 * period) {
            next = now;   /* the process was paused: do not try to catch up */
        }
    }
    free(buf);
    return NULL;
}

OSStatus AudioOutputUnitStart(AudioUnit u) {
    if (u->started) return noErr;
    u->running = 1;
    if (pthread_create(&u->thread, NULL, clock_thread, u)) { u->running = 0; return -1; }
    u->started = 1;
    return noErr;
}

OSStatus AudioOutputUnitStop(AudioUnit u) {
    if (!u->started) return noErr;
    u->running = 0;
    pthread_join(u->thread, NULL);
    u->started = 0;
    return noErr;
}
