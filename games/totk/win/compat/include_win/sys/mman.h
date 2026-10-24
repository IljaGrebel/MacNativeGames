/* Stand-in for POSIX <sys/mman.h> on Windows (include_win/ is on the include path only there): the anonymous and
   read-only file mappings the upstream runtime makes, over VirtualAlloc / VirtualProtect / MapViewOfFile.
   Semantics kept: PROT_NONE keeps contents (VirtualProtect, as mprotect), MAP_FIXED over a reservation commits fresh
   zeroed memory (VirtualFree MEM_DECOMMIT + MEM_COMMIT), mprotect on reserved address space commits it. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4
#define MADV_FREE 8
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);
