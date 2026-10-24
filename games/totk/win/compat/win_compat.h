/* Force-included (-include) into the upstream runtime sources when they are built for the Windows port, on Windows
 * and on Linux (where the tests run). It maps the few Apple-only calls they make onto portable ones, so the upstream
 * files build unchanged; the stub headers in include/ stand in for the Apple headers they include.
 */
#pragma once
#if defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64   /* MinGW's off_t is 32-bit otherwise; also for the bridge generator's clang */
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <pthread.h>

#if defined(_WIN32)
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <direct.h>
#include <sys/types.h>
/* The first run on the real game stopped at 2 GB into the RomFS hash: off_t wrapped (build with _FILE_OFFSET_BITS=64) */
_Static_assert(sizeof(off_t) == 8, "64-bit off_t");
typedef jmp_buf sigjmp_buf;
/* POSIX calls MinGW's CRT lacks or spells differently (compat_win.c) */
int compat_mkdir(const char *path, int mode);
#define mkdir(path, mode) compat_mkdir(path, mode)
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *realpath(const char *path, char *resolved);
long long pread(int fd, void *buf, size_t n, long long offset);
long long pwrite(int fd, const void *buf, size_t n, long long offset);
int fsync(int fd);

#define sigsetjmp(b, s) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)
#endif

uint64_t compat_now_ns(void);
int compat_readable(uint64_t address, uint64_t size);

/* Darwin pthread extensions */
uint64_t compat_thread_id(void);
#define pthread_threadid_np(t, id) ((void)(t), *(id) = compat_thread_id(), 0)
void *compat_stack_top(void);
size_t compat_stack_size(void);
#define pthread_get_stackaddr_np(t) ((void)(t), compat_stack_top())
#define pthread_get_stacksize_np(t) ((void)(t), compat_stack_size())
static inline unsigned compat_mach_port(void) { return 0; }
#define pthread_mach_thread_np(t) ((void)(t), compat_mach_port())

/* arc4random */
void compat_random_bytes(void *p, size_t n);
#define arc4random_buf compat_random_bytes

/* fcntl(fd, F_FULLFSYNC): flush to the disk (saves) */
#include <fcntl.h>
#ifndef F_FULLFSYNC
#define F_FULLFSYNC 0x7F51
int compat_fcntl(int fd, int cmd, ...);
#define fcntl compat_fcntl
#endif

/* Darwin's struct stat time fields */
#include <sys/stat.h>
#if defined(_WIN32)
/* stat with Darwin's fields, from GetFileInformationByHandle: st_dev the volume serial, st_ino the file index (so file
   identity checks work), times with 100 ns resolution. `struct stat` and stat() / fstat() become these. */
struct timespec;
#include <time.h>
struct compat_stat {
    uint64_t st_dev, st_ino;
    unsigned st_mode, st_nlink;
    long long st_size;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    time_t st_mtime;
};
int compat_stat(const char *path, struct compat_stat *st);
int compat_fstat(int fd, struct compat_stat *st);
#define stat compat_stat
#define fstat compat_fstat
#define lstat compat_stat
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x0080   /* _O_NOINHERIT */
#endif
/* struct tm with tm_gmtoff and tm_zone (POSIX), and the time calls that take it */
struct compat_tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; const char *tm_zone; };
#define tm compat_tm
struct compat_tm *compat_localtime_r(const time_t *t, struct compat_tm *out);
struct compat_tm *compat_gmtime_r(const time_t *t, struct compat_tm *out);
struct compat_tm *compat_localtime(const time_t *t);
struct compat_tm *compat_gmtime(const time_t *t);
time_t compat_timegm(struct compat_tm *t);
time_t compat_mktime(struct compat_tm *t);
size_t compat_strftime(char *s, size_t n, const char *fmt, const struct compat_tm *t);
#define localtime_r compat_localtime_r
#define gmtime_r compat_gmtime_r
#define localtime compat_localtime
#define gmtime compat_gmtime
#define timegm compat_timegm
#define mktime compat_mktime
#define strftime compat_strftime
long long compat_readlink(const char *path, char *buf, size_t n);
#define readlink compat_readlink
/* flock */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int flock(int fd, int op);
#else
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#define st_birthtimespec st_ctim   /* no birth time in Linux's stat: the change time stands in (file identity checks) */
#endif
