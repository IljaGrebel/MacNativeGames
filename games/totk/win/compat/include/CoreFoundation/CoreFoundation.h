/* Stand-in for the part of CoreFoundation the upstream runtime uses: the user's language and region, which pick the
   game's language. Backed by compat_locale (the Windows user locale, or LANG elsewhere). */
#pragma once
#include <stdio.h>
#include <string.h>
typedef const char *CFStringRef;
typedef struct CompatLocale { char language[16], region[16]; } *CFLocaleRef;
typedef int CFStringEncoding;
#define kCFStringEncodingUTF8 0x08000100
#define kCFLocaleLanguageCode ((CFStringRef)"language")
#define kCFLocaleCountryCode ((CFStringRef)"region")
void compat_locale(char *language, size_t language_size, char *region, size_t region_size);
static inline CFLocaleRef CFLocaleCopyCurrent(void) {
    static struct CompatLocale l;
    compat_locale(l.language, sizeof l.language, l.region, sizeof l.region);
    return &l;
}
static inline CFStringRef CFLocaleGetValue(CFLocaleRef l, CFStringRef key) {
    const char *v = !strcmp(key, "language") ? l->language : l->region;
    return v[0] ? v : NULL;
}
static inline int CFStringGetCString(CFStringRef s, char *out, long size, CFStringEncoding e) {
    (void)e;
    if (!s || size <= 0) return 0;
    snprintf(out, (size_t)size, "%s", s);
    return 1;
}
static inline void CFRelease(const void *p) { (void)p; }
