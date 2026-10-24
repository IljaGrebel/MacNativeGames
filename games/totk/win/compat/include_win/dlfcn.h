/* Stand-in for <dlfcn.h> on Windows: symbol names of host addresses are only used in diagnostics. */
#pragma once
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
static inline int dladdr(const void *addr, Dl_info *info) { (void)addr; (void)info; return 0; }
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NOW 2
static inline void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return 0; }
