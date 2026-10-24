#pragma once
#include <stdint.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
uint64_t compat_now_ns(void);
static inline uint64_t mach_absolute_time(void) { return compat_now_ns(); }
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = 1; t->denom = 1; return 0; }
static inline uint64_t mach_continuous_time(void) { return compat_now_ns(); }   /* steady, counts while asleep */
