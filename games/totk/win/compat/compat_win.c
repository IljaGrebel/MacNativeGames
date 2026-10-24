/* Windows implementations of the POSIX calls the upstream runtime makes (declared in win_compat.h and include_win/).
 * Paths are UTF-8 and converted to UTF-16 for the system. Built only on Windows. */
#if defined(_WIN32)
#include "win_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/mman.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

/* Files open in binary mode unless asked otherwise: the runtime reads game data with open()/read(). */
__attribute__((constructor)) static void binary_files(void) { _fmode = _O_BINARY; }   /* stdlib.h: msvcrt and UCRT */

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int set_errno_from_last(void) {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: errno = EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_COMMITMENT_LIMIT: errno = ENOMEM; break;
    default: errno = EIO;
    }
    return -1;
}

/* ---- files ---- */

int compat_mkdir(const char *path, int mode) {
    (void)mode;
    wchar_t *w = wide(path);
    int r = w && CreateDirectoryW(w, NULL) ? 0 : set_errno_from_last();
    free(w);
    return r;
}

long long pread(int fd, void *buf, size_t n, long long offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    o.Offset = (DWORD)offset;
    o.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!ReadFile(h, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)n, &got, &o))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : set_errno_from_last();
    return got;
}

long long pwrite(int fd, const void *buf, size_t n, long long offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    o.Offset = (DWORD)offset;
    o.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!WriteFile(h, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)n, &put, &o)) return set_errno_from_last();
    return put;
}

int fsync(int fd) { return _commit(fd); }

int flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (op & LOCK_UN) return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &o) ? 0 : set_errno_from_last();
    DWORD flags = (op & LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (op & LOCK_NB ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &o)) return 0;
    if (GetLastError() == ERROR_LOCK_VIOLATION) { errno = EWOULDBLOCK; return -1; }
    return set_errno_from_last();
}

static void fill_stat(const BY_HANDLE_FILE_INFORMATION *fi, struct compat_stat *st) {
    memset(st, 0, sizeof *st);
    st->st_dev = fi->dwVolumeSerialNumber;
    st->st_ino = ((uint64_t)fi->nFileIndexHigh << 32) | fi->nFileIndexLow;
    st->st_size = (long long)(((uint64_t)fi->nFileSizeHigh << 32) | fi->nFileSizeLow);
    st->st_nlink = fi->nNumberOfLinks;
    st->st_mode = (fi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? (_S_IFDIR | 0755) : (_S_IFREG | 0644);
    if (fi->dwFileAttributes & FILE_ATTRIBUTE_READONLY) st->st_mode &= ~0222u;
    const FILETIME *t[4] = {&fi->ftLastAccessTime, &fi->ftLastWriteTime, &fi->ftLastWriteTime, &fi->ftCreationTime};
    struct timespec *o[4] = {&st->st_atimespec, &st->st_mtimespec, &st->st_ctimespec, &st->st_birthtimespec};
    for (int i = 0; i < 4; i++) {
        uint64_t v = ((uint64_t)t[i]->dwHighDateTime << 32) | t[i]->dwLowDateTime;   /* 100 ns since 1601 */
        v -= 116444736000000000ull;
        o[i]->tv_sec = (time_t)(v / 10000000u);
        o[i]->tv_nsec = (long)(v % 10000000u) * 100;
    }
    st->st_mtime = st->st_mtimespec.tv_sec;
}

int compat_fstat(int fd, struct compat_stat *st) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION fi;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!GetFileInformationByHandle(h, &fi)) return set_errno_from_last();
    fill_stat(&fi, st);
    return 0;
}

int compat_stat(const char *path, struct compat_stat *st) {
    wchar_t *w = wide(path);
    HANDLE h = w ? CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return set_errno_from_last();
    BY_HANDLE_FILE_INFORMATION fi;
    int ok = GetFileInformationByHandle(h, &fi);
    CloseHandle(h);
    if (!ok) return set_errno_from_last();
    fill_stat(&fi, st);
    return 0;
}

long long compat_readlink(const char *path, char *buf, size_t n) {
    (void)path; (void)buf; (void)n;
    errno = EINVAL;   /* no symbolic-link time zone file: the caller falls back to UTC naming */
    return -1;
}

char *realpath(const char *path, char *resolved) {
    char *r = _fullpath(resolved, path, resolved ? 260 : 0);
    if (r) for (char *c = r; *c; c++) if (*c == '\\') *c = '/';
    return r;
}

/* ---- directories ---- */

struct CompatDir { HANDLE h; WIN32_FIND_DATAW fd; int first, done; struct dirent e; };

DIR *opendir(const char *path) {
    size_t n = strlen(path);
    char *pattern = malloc(n + 3);
    if (!pattern) { errno = ENOMEM; return NULL; }
    snprintf(pattern, n + 3, "%s%s*", path, n && (path[n - 1] == '/' || path[n - 1] == '\\') ? "" : "/");
    wchar_t *w = wide(pattern);
    free(pattern);
    DIR *d = calloc(1, sizeof *d);
    if (!w || !d) { free(w); free(d); errno = ENOMEM; return NULL; }
    d->h = FindFirstFileW(w, &d->fd);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(d);
        if (e == ERROR_FILE_NOT_FOUND) { errno = 0; DIR *empty = calloc(1, sizeof *empty); if (empty) { empty->done = 1; empty->h = INVALID_HANDLE_VALUE; } return empty; }
        SetLastError(e);
        set_errno_from_last();
        return NULL;
    }
    d->first = 1;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (d->done) return NULL;
    if (!d->first && !FindNextFileW(d->h, &d->fd)) { d->done = 1; return NULL; }
    d->first = 0;
    WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->e.d_name, sizeof d->e.d_name, NULL, NULL);
    d->e.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    d->e.d_ino = 0;
    return &d->e;
}

int closedir(DIR *d) {
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

/* ---- environment ---- */

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }

/* ---- time ---- */

#undef localtime_r
#undef gmtime_r
#undef localtime
#undef gmtime
#undef timegm
#undef mktime
#undef strftime
#undef tm

static void to_compat(const struct tm *in, struct compat_tm *out, long gmtoff, const char *zone) {
    out->tm_sec = in->tm_sec; out->tm_min = in->tm_min; out->tm_hour = in->tm_hour; out->tm_mday = in->tm_mday;
    out->tm_mon = in->tm_mon; out->tm_year = in->tm_year; out->tm_wday = in->tm_wday; out->tm_yday = in->tm_yday;
    out->tm_isdst = in->tm_isdst; out->tm_gmtoff = gmtoff; out->tm_zone = zone;
}
static void from_compat(const struct compat_tm *in, struct tm *out) {
    memset(out, 0, sizeof *out);
    out->tm_sec = in->tm_sec; out->tm_min = in->tm_min; out->tm_hour = in->tm_hour; out->tm_mday = in->tm_mday;
    out->tm_mon = in->tm_mon; out->tm_year = in->tm_year; out->tm_wday = in->tm_wday; out->tm_yday = in->tm_yday;
    out->tm_isdst = in->tm_isdst;
}

struct compat_tm *compat_gmtime_r(const time_t *t, struct compat_tm *out) {
    struct tm g;
    if (gmtime_s(&g, t)) return NULL;
    to_compat(&g, out, 0, "UTC");
    return out;
}

struct compat_tm *compat_localtime_r(const time_t *t, struct compat_tm *out) {
    struct tm l;
    if (localtime_s(&l, t)) return NULL;
    /* the offset and zone name from the system (works with msvcrt and the UCRT alike) */
    TIME_ZONE_INFORMATION z;
    DWORD kind = GetTimeZoneInformation(&z);
    int dst = l.tm_isdst > 0 && kind == TIME_ZONE_ID_DAYLIGHT;
    long bias_minutes = z.Bias + (dst ? z.DaylightBias : z.StandardBias);   /* UTC = local + bias */
    static _Thread_local char name[64];
    WideCharToMultiByte(CP_UTF8, 0, dst ? z.DaylightName : z.StandardName, -1, name, sizeof name, NULL, NULL);
    to_compat(&l, out, -60L * bias_minutes, name[0] ? name : "UTC");
    return out;
}

static _Thread_local struct compat_tm g_tm;
struct compat_tm *compat_localtime(const time_t *t) { return compat_localtime_r(t, &g_tm); }
struct compat_tm *compat_gmtime(const time_t *t) { return compat_gmtime_r(t, &g_tm); }

time_t compat_timegm(struct compat_tm *t) {
    struct tm m;
    from_compat(t, &m);
    return _mkgmtime(&m);
}

time_t compat_mktime(struct compat_tm *t) {
    struct tm m;
    from_compat(t, &m);
    m.tm_isdst = t->tm_isdst;
    time_t r = mktime(&m);
    to_compat(&m, t, t->tm_gmtoff, t->tm_zone);
    return r;
}

size_t compat_strftime(char *s, size_t n, const char *fmt, const struct compat_tm *t) {
    struct tm m;
    from_compat(t, &m);
    return strftime(s, n, fmt, &m);
}

/* ---- memory mappings ---- */

static DWORD page_protect(int prot) {
    if (prot & PROT_WRITE) return (prot & PROT_EXEC) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (prot & PROT_READ) return (prot & PROT_EXEC) ? PAGE_EXECUTE_READ : PAGE_READONLY;
    return PAGE_NOACCESS;
}

static int is_reserved_or_committed(void *addr) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(addr, &mi, sizeof mi) && mi.State != MEM_FREE;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
    if (!(flags & MAP_ANON)) {
        /* read-only file views (the RomFS image) */
        if (prot & PROT_WRITE) { errno = ENOTSUP; return MAP_FAILED; }
        HANDLE f = (HANDLE)_get_osfhandle(fd);
        HANDLE m = f == INVALID_HANDLE_VALUE ? NULL : CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!m) { set_errno_from_last(); return MAP_FAILED; }
        void *v = MapViewOfFile(m, FILE_MAP_READ, (DWORD)((unsigned long long)off >> 32), (DWORD)off, len);
        CloseHandle(m);   /* the view keeps the mapping alive */
        if (!v) { set_errno_from_last(); return MAP_FAILED; }
        return v;
    }
    if (flags & MAP_FIXED) {
        if (!addr) { errno = EINVAL; return MAP_FAILED; }
        if (is_reserved_or_committed(addr)) {
            /* a fresh mapping over existing address space: drop the old contents, keep the reservation */
            VirtualFree(addr, len, MEM_DECOMMIT);
            if (prot == PROT_NONE) return addr;
            return VirtualAlloc(addr, len, MEM_COMMIT, page_protect(prot)) ? addr : (set_errno_from_last(), MAP_FAILED);
        }
        void *p = VirtualAlloc(addr, len, prot == PROT_NONE ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT), page_protect(prot));
        return p == addr ? p : (p ? (VirtualFree(p, 0, MEM_RELEASE), errno = ENOMEM, MAP_FAILED) : (set_errno_from_last(), MAP_FAILED));
    }
    void *p = VirtualAlloc(addr, len, prot == PROT_NONE ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT), page_protect(prot));
    if (!p && addr) p = VirtualAlloc(NULL, len, prot == PROT_NONE ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT), page_protect(prot));
    return p ? p : (set_errno_from_last(), MAP_FAILED);
}

int munmap(void *addr, size_t len) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(addr, &mi, sizeof mi)) { errno = EINVAL; return -1; }
    if (mi.Type == MEM_MAPPED) return UnmapViewOfFile(mi.AllocationBase) ? 0 : set_errno_from_last();
    /* the whole allocation goes when the range covers it; part of one is decommitted (Windows frees only whole ones) */
    if (mi.AllocationBase == addr) {
        uint64_t end = (uint64_t)(uintptr_t)addr, total = 0;
        while (VirtualQuery((void *)(uintptr_t)end, &mi, sizeof mi) && mi.AllocationBase == addr) {
            total += mi.RegionSize;
            end += mi.RegionSize;
        }
        if (total <= len) return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : set_errno_from_last();
    }
    return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : set_errno_from_last();
}

int mprotect(void *addr, size_t len, int prot) {
    /* POSIX keeps the pages' contents under any protection; address space only reserved (from a PROT_NONE mmap) is
       committed when it becomes accessible */
    uint8_t *p = addr, *end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State == MEM_FREE) { errno = ENOMEM; return -1; }
        uint8_t *region_end = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        size_t n = (size_t)((region_end < end ? region_end : end) - p);
        DWORD old;
        if (mi.State == MEM_RESERVE) {
            if (prot != PROT_NONE && !VirtualAlloc(p, n, MEM_COMMIT, page_protect(prot))) return set_errno_from_last();
        } else if (!VirtualProtect(p, n, page_protect(prot), &old)) {
            return set_errno_from_last();
        }
        p += n;
    }
    return 0;
}

int madvise(void *addr, size_t len, int advice) {
    if (advice == MADV_DONTNEED || advice == MADV_FREE) {
        /* contents may go: offer the pages back, they read as zero when touched again */
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
            DWORD prot = mi.Protect;
            VirtualFree(addr, len, MEM_DECOMMIT);
            VirtualAlloc(addr, len, MEM_COMMIT, prot);
        }
    }
    return 0;
}
#endif
