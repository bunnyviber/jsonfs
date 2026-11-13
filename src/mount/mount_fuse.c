/*
 * JSONfs mount driver - FUSE backend.
 *
 * POSIX (Linux, FreeBSD, macOS):
 *   libfuse is loaded with dlopen() at runtime, so the binary can be
 *   cross-compiled without a FUSE SDK and works with whichever FUSE flavour
 *   the host has: libfuse 3 (fuse3), libfuse 2, macFUSE, FUSE-T, FreeBSD's
 *   fusefs-libs/fusefs-libs3.  The handful of ABI types we need are declared
 *   below; they are the stable, documented layouts of FUSE API 2.9 and 3.x.
 *   JSONFS_FUSE_LIBRARY=/path/to/libfuse.so overrides the search.
 *
 * Windows (7 and later):
 *   WinFsp's FUSE compatibility layer, compiled against WinFsp's own
 *   headers (scripts/fetch-deps.sh) and bound at runtime with
 *   LoadLibrary, so jsonfs-mount.exe still starts on systems without
 *   WinFsp (and falls back to Dokan there).
 *
 *   WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
 *   https://github.com/winfsp/winfsp
 *
 * SPDX-License-Identifier: MIT
 */
#if !defined(_WIN32)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "mount_common.h"
#include "../lib/jfs_http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(JFS_HAVE_WINFSP)
/* ======================================================================== */
/* Windows build without WinFsp headers: Dokan only.                          */
/* ======================================================================== */
int jfs_fuse_available(char *why, size_t whylen)
{
    snprintf(why, whylen, "this build was made without WinFsp support");
    return 0;
}

int jfs_fuse_run(void)
{
    return 1;
}

#else

#ifdef _WIN32
/* ======================================================================== */
/* WinFsp FUSE: declarations come from WinFsp's headers; every API function  */
/* becomes a function pointer that we resolve with GetProcAddress.           */
/* ======================================================================== */
#include <windows.h>
#include <fcntl.h>
#define FSP_FUSE_API static
#define FSP_FUSE_API_NAME(n) (*pfn_##n)
#define FSP_FUSE_API_CALL(n) (pfn_##n)
#define FUSE_USE_VERSION 29
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <fuse/fuse.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct fuse_stat jstat_t;
typedef struct fuse_statvfs jstatvfs_t;
typedef fuse_off_t joff_t;
typedef fuse_mode_t jmode_t;
typedef fuse_dev_t jdev_t;
typedef fuse_uid_t juid_t;
typedef fuse_gid_t jgid_t;
typedef struct fuse_timespec jtimespec_t;
typedef struct fuse_file_info jfi2_t;
#define JFS_ATIM st_atim
#define JFS_MTIM st_mtim
#define JFS_CTIM st_ctim
#define JFS_HAVE_BIRTHTIM 1
#define JFS_BIRTHTIM st_birthtim
#ifndef UTIME_NOW
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
#endif
#define FUSE_ABI_2 1
#define FUSE_ABI_3 0

#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif

static HMODULE winfsp_dll;

static int winfsp_load(char *why, size_t whylen)
{
    WCHAR path[MAX_PATH + 64];
    DWORD size = sizeof path - 64 * sizeof(WCHAR), type;
    HKEY key;
    const WCHAR *dllname =
#if defined(_M_ARM64) || defined(__aarch64__)
        L"winfsp-a64.dll";
#elif defined(_WIN64)
        L"winfsp-x64.dll";
#else
        L"winfsp-x86.dll";
#endif
    if (winfsp_dll)
        return 1;
    path[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", 0, KEY_READ, &key) == ERROR_SUCCESS ||
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinFsp", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"InstallDir", NULL, &type, (LPBYTE)path, &size) != ERROR_SUCCESS ||
            type != REG_SZ)
            path[0] = 0;
        RegCloseKey(key);
    }
    if (path[0]) {
        size_t n = wcslen(path);
        if (n && path[n - 1] != L'\\')
            wcscat(path, L"\\");
        wcscat(path, L"bin\\");
        wcscat(path, dllname);
        winfsp_dll = LoadLibraryW(path);
    }
    if (!winfsp_dll)
        winfsp_dll = LoadLibraryW(dllname);
    if (!winfsp_dll) {
        snprintf(why, whylen, "WinFsp is not installed (%ls not found)", dllname);
        return 0;
    }
#define LOAD(n)                                                         \
    do {                                                                \
        FARPROC fp_ = GetProcAddress(winfsp_dll, #n);                   \
        memcpy(&pfn_##n, &fp_, sizeof fp_);                             \
        if (!fp_) {                                                     \
            snprintf(why, whylen, "WinFsp DLL lacks %s", #n);           \
            FreeLibrary(winfsp_dll);                                    \
            winfsp_dll = NULL;                                          \
            return 0;                                                   \
        }                                                               \
    } while (0)
    LOAD(fsp_fuse_main_real);
    LOAD(fsp_fuse_get_context);
    LOAD(fsp_fuse_signal_handler);
#undef LOAD
    {
        /* optional: change notifications (WinFsp 2017+) */
        FARPROC fp = GetProcAddress(winfsp_dll, "fsp_fuse_notify");
        memcpy(&pfn_fsp_fuse_notify, &fp, sizeof fp);
    }
    return 1;
}

static struct fuse *win_fuse;

static void winfsp_notify(const jfs_changes *ch)
{
    size_t i;
    if (!win_fuse || !pfn_fsp_fuse_notify)
        return;
    for (i = 0; i < ch->n; i++) {
        const jfs_change *c = &ch->v[i];
        int dir = (c->mode & JFS_S_IFMT) == JFS_S_IFDIR;
        switch (c->kind) {
        case JFS_CHANGE_CREATE:
        case JFS_CHANGE_LINK:
            fuse_notify(win_fuse, c->path, dir ? FSP_FUSE_NOTIFY_MKDIR : FSP_FUSE_NOTIFY_CREATE);
            break;
        case JFS_CHANGE_DELETE:
            fuse_notify(win_fuse, c->path, dir ? FSP_FUSE_NOTIFY_RMDIR : FSP_FUSE_NOTIFY_UNLINK);
            break;
        case JFS_CHANGE_RENAME:
            fuse_notify(win_fuse, c->path, dir ? FSP_FUSE_NOTIFY_RMDIR : FSP_FUSE_NOTIFY_UNLINK);
            fuse_notify(win_fuse, c->path2, dir ? FSP_FUSE_NOTIFY_MKDIR : FSP_FUSE_NOTIFY_CREATE);
            break;
        case JFS_CHANGE_MODIFY:
            fuse_notify(win_fuse, c->path, FSP_FUSE_NOTIFY_TRUNCATE | FSP_FUSE_NOTIFY_UTIME);
            break;
        case JFS_CHANGE_ATTRIB:
            fuse_notify(win_fuse, c->path, FSP_FUSE_NOTIFY_CHMOD | FSP_FUSE_NOTIFY_CHOWN | FSP_FUSE_NOTIFY_UTIME);
            break;
        }
    }
}

static void winfsp_capture_fuse(void)
{
    struct fuse_context *fc = fuse_get_context();
    if (fc)
        win_fuse = fc->fuse;
    if (pfn_fsp_fuse_notify)
        jfs_refresh_notify_hook = winfsp_notify;
}

static struct fuse_context *ctx_get(void)
{
    return fuse_get_context();
}

#else /* POSIX */
/* ======================================================================== */
/* POSIX: FUSE ABI declarations (FUSE API 2.9 and 3.x) + dlopen               */
/* ======================================================================== */
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct stat jstat_t;
typedef struct statvfs jstatvfs_t;
typedef off_t joff_t;
typedef mode_t jmode_t;
typedef dev_t jdev_t;
typedef uid_t juid_t;
typedef gid_t jgid_t;
typedef struct timespec jtimespec_t;
#if defined(__APPLE__)
#define JFS_ATIM st_atimespec
#define JFS_MTIM st_mtimespec
#define JFS_CTIM st_ctimespec
#define JFS_HAVE_BIRTHTIM 1
#define JFS_BIRTHTIM st_birthtimespec
#else
#define JFS_ATIM st_atim
#define JFS_MTIM st_mtim
#define JFS_CTIM st_ctim
#if defined(__FreeBSD__)
#define JFS_HAVE_BIRTHTIM 1
#define JFS_BIRTHTIM st_birthtim
#endif
#endif
#define FUSE_ABI_2 1
#define FUSE_ABI_3 1

struct fuse_conn_info; /* opaque */
struct fuse_chan;
struct fuse;

/* FUSE 2.9 */
typedef struct jfi2_t {
    int flags;
    unsigned long fh_old;
    int writepage;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    unsigned int flush : 1;
    unsigned int nonseekable : 1;
    unsigned int flock_release : 1;
    unsigned int padding : 27;
    uint64_t fh;
    uint64_t lock_owner;
} jfi2_t;

/* FUSE 3.x */
typedef struct jfi3_t {
    int flags;
    unsigned int writepage : 1;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    unsigned int flush : 1;
    unsigned int nonseekable : 1;
    unsigned int flock_release : 1;
    unsigned int cache_readdir : 1;
    unsigned int noflush : 1;
    unsigned int padding : 24;
    unsigned int padding2 : 32;
    uint64_t fh;
    uint64_t lock_owner;
    uint32_t poll_events;
} jfi3_t;

struct fuse_context {
    struct fuse *fuse;
    uid_t uid;
    gid_t gid;
    pid_t pid;
    void *private_data;
    mode_t umask;
};

/* The stable prefix of FUSE 3's struct fuse_config (3.0 .. 3.17). */
struct jfuse_config3 {
    int set_gid;
    unsigned int gid;
    int set_uid;
    unsigned int uid;
    int set_mode;
    unsigned int umask;
    double entry_timeout;
    double negative_timeout;
    double attr_timeout;
    int intr;
    int intr_signal;
    int remember;
    int hard_remove;
    int use_ino;
    int readdir_ino;
    int direct_io;
    int kernel_cache;
    int auto_cache;
};

typedef int (*fill2_t)(void *buf, const char *name, const struct stat *st, off_t off);
typedef int (*fill3_t)(void *buf, const char *name, const struct stat *st, off_t off, int flags);

struct fuse_operations2 {
    int (*getattr)(const char *, struct stat *);
    int (*readlink)(const char *, char *, size_t);
    void *getdir;
    int (*mknod)(const char *, mode_t, dev_t);
    int (*mkdir)(const char *, mode_t);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*symlink)(const char *, const char *);
    int (*rename)(const char *, const char *);
    int (*link)(const char *, const char *);
    int (*chmod)(const char *, mode_t);
    int (*chown)(const char *, uid_t, gid_t);
    int (*truncate)(const char *, off_t);
    void *utime;
    int (*open)(const char *, jfi2_t *);
    int (*read)(const char *, char *, size_t, off_t, jfi2_t *);
    int (*write)(const char *, const char *, size_t, off_t, jfi2_t *);
    int (*statfs)(const char *, struct statvfs *);
    int (*flush)(const char *, jfi2_t *);
    int (*release)(const char *, jfi2_t *);
    int (*fsync)(const char *, int, jfi2_t *);
#if defined(__APPLE__)
    int (*setxattr)(const char *, const char *, const char *, size_t, int, uint32_t);
    int (*getxattr)(const char *, const char *, char *, size_t, uint32_t);
#else
    int (*setxattr)(const char *, const char *, const char *, size_t, int);
    int (*getxattr)(const char *, const char *, char *, size_t);
#endif
    int (*listxattr)(const char *, char *, size_t);
    int (*removexattr)(const char *, const char *);
    int (*opendir)(const char *, jfi2_t *);
    int (*readdir)(const char *, void *, fill2_t, off_t, jfi2_t *);
    int (*releasedir)(const char *, jfi2_t *);
    int (*fsyncdir)(const char *, int, jfi2_t *);
    void *(*init)(struct fuse_conn_info *);
    void (*destroy)(void *);
    int (*access)(const char *, int);
    int (*create)(const char *, mode_t, jfi2_t *);
    int (*ftruncate)(const char *, off_t, jfi2_t *);
    int (*fgetattr)(const char *, struct stat *, jfi2_t *);
    void *lock;
    int (*utimens)(const char *, const struct timespec tv[2]);
    void *bmap;
    unsigned int flag_nullpath_ok : 1;
    unsigned int flag_nopath : 1;
    unsigned int flag_utime_omit_ok : 1;
    unsigned int flag_reserved : 29;
};

struct fuse_operations3 {
    int (*getattr)(const char *, struct stat *, jfi3_t *);
    int (*readlink)(const char *, char *, size_t);
    int (*mknod)(const char *, mode_t, dev_t);
    int (*mkdir)(const char *, mode_t);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*symlink)(const char *, const char *);
    int (*rename)(const char *, const char *, unsigned int);
    int (*link)(const char *, const char *);
    int (*chmod)(const char *, mode_t, jfi3_t *);
    int (*chown)(const char *, uid_t, gid_t, jfi3_t *);
    int (*truncate)(const char *, off_t, jfi3_t *);
    int (*open)(const char *, jfi3_t *);
    int (*read)(const char *, char *, size_t, off_t, jfi3_t *);
    int (*write)(const char *, const char *, size_t, off_t, jfi3_t *);
    int (*statfs)(const char *, struct statvfs *);
    int (*flush)(const char *, jfi3_t *);
    int (*release)(const char *, jfi3_t *);
    int (*fsync)(const char *, int, jfi3_t *);
    int (*setxattr)(const char *, const char *, const char *, size_t, int);
    int (*getxattr)(const char *, const char *, char *, size_t);
    int (*listxattr)(const char *, char *, size_t);
    int (*removexattr)(const char *, const char *);
    int (*opendir)(const char *, jfi3_t *);
    int (*readdir)(const char *, void *, fill3_t, off_t, jfi3_t *, int);
    int (*releasedir)(const char *, jfi3_t *);
    int (*fsyncdir)(const char *, int, jfi3_t *);
    void *(*init)(struct fuse_conn_info *, struct jfuse_config3 *);
    void (*destroy)(void *);
    int (*access)(const char *, int);
    int (*create)(const char *, mode_t, jfi3_t *);
    void *lock;
    int (*utimens)(const char *, const struct timespec tv[2], jfi3_t *);
};

typedef int (*fuse_main_real_fn)(int, char **, const void *, size_t, void *);
typedef struct fuse_context *(*fuse_get_context_fn)(void);

static void *fuse_lib;
static int fuse_major; /* 2 or 3 */
static int fuse_minor; /* 3.x minor version when known */
static fuse_main_real_fn p_fuse_main_real;
static fuse_get_context_fn p_fuse_get_context;
static char fuse_lib_name[512];

static int try_load(const char *name, int major)
{
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        return 0;
    p_fuse_main_real = (fuse_main_real_fn)dlsym(h, "fuse_main_real");
    p_fuse_get_context = (fuse_get_context_fn)dlsym(h, "fuse_get_context");
    if (!p_fuse_main_real || !p_fuse_get_context) {
        dlclose(h);
        return 0;
    }
    if (major == 0) {
        /* user supplied library: libfuse 3 exports fuse_session_loop_mt_31/fuse_pkgversion */
        major = (dlsym(h, "fuse_loop_cfg_create") || dlsym(h, "fuse_session_loop_mt_31") ||
                 dlsym(h, "fuse_pkgversion"))
                    ? 3
                    : 2;
#if defined(__APPLE__)
        major = 2; /* macFUSE / FUSE-T expose the FUSE 2 API */
#endif
    }
    fuse_lib = h;
    fuse_major = major;
    if (major == 3) {
        const char *(*pkgver)(void);
        void *sym = dlsym(h, "fuse_pkgversion");
        memcpy(&pkgver, &sym, sizeof sym);
        if (pkgver) {
            int a = 0, b = 0;
            sscanf(pkgver(), "%d.%d", &a, &b);
            fuse_minor = a == 3 ? b : 0;
        }
    }
    snprintf(fuse_lib_name, sizeof fuse_lib_name, "%s", name);
    return 1;
}

static struct fuse_context *ctx_get(void)
{
    return p_fuse_get_context();
}
#endif /* POSIX */

/* ======================================================================== */
/* Shared helpers                                                             */
/* ======================================================================== */
static int E(int jfs_rc)
{
    return jfs_rc ? -jfs_err_to_errno(jfs_rc) : 0;
}

static jfs_handle *H(uint64_t fh)
{
    return (jfs_handle *)(uintptr_t)fh;
}

static void to_ts(jfs_time t, jtimespec_t *ts)
{
    ts->tv_sec = t.sec;
    ts->tv_nsec = t.nsec;
}

static void to_stat(const jfs_stat *s, jstat_t *st)
{
    memset(st, 0, sizeof *st);
    st->st_ino = s->ino;
    st->st_mode = s->mode;
    st->st_nlink = s->nlink;
    st->st_uid = s->uid;
    st->st_gid = s->gid;
    st->st_rdev = (jdev_t)s->rdev;
    st->st_size = (joff_t)s->size;
    st->st_blocks = s->blocks;
    st->st_blksize = 4096;
    to_ts(s->atime, &st->JFS_ATIM);
    to_ts(s->mtime, &st->JFS_MTIM);
    to_ts(s->ctime, &st->JFS_CTIM);
#ifdef JFS_HAVE_BIRTHTIM
    to_ts(s->btime, &st->JFS_BIRTHTIM);
#endif
}

static jfs_ctx caller(void)
{
    jfs_ctx c;
    struct fuse_context *fc = ctx_get();
    memset(&c, 0, sizeof c);
    if (fc) {
        c.uid = (uint32_t)fc->uid;
        c.gid = (uint32_t)fc->gid;
    }
    c.umask = 0; /* the kernel already applied the umask */
    return c;
}

static int open_flags(int f)
{
    int r = 0;
    switch (f & O_ACCMODE) {
    case O_WRONLY: r = JFS_O_WRONLY; break;
    case O_RDWR: r = JFS_O_RDWR; break;
    default: r = JFS_O_RDONLY;
    }
    if (f & O_TRUNC)
        r |= JFS_O_TRUNC;
    if (f & O_EXCL)
        r |= JFS_O_EXCL;
    if (f & O_APPEND)
        r |= JFS_O_APPEND;
    return r;
}

static jfs_time from_ts(const jtimespec_t *ts)
{
    jfs_time t;
    t.sec = ts->tv_sec;
    t.nsec = (int32_t)ts->tv_nsec;
    if (ts->tv_nsec == UTIME_NOW)
        t.nsec = JFS_UTIME_NOW;
    else if (ts->tv_nsec == UTIME_OMIT)
        t.nsec = JFS_UTIME_OMIT;
    return t;
}

/* ---- common implementations (path may be NULL when a handle is given) --- */
static int c_getattr(const char *path, jstat_t *st, uint64_t fh, int has_fh)
{
    jfs_stat s;
    int rc = (has_fh && fh) ? jfs_fgetattr(jfs_mount_fs, H(fh), &s) : jfs_getattr(jfs_mount_fs, path, &s);
    if (!rc)
        to_stat(&s, st);
    return E(rc);
}

/* one of the system calls our refresher issues to replay remote changes? */
static int is_replay(void)
{
    struct fuse_context *fc = ctx_get();
    return fc && jfs_refresh_is_replay((uint64_t)fc->pid);
}

static uint64_t caller_pid(void)
{
    struct fuse_context *fc = ctx_get();
    return fc ? (uint64_t)fc->pid : 0;
}

static int c_readlink(const char *path, char *buf, size_t size)
{
    return E(jfs_readlink(jfs_mount_fs, path, buf, size));
}

static int c_mknod(const char *path, jmode_t mode, jdev_t rdev)
{
    jfs_ctx c = caller();
    if (is_replay())
        return E(jfs_replay_create(jfs_mount_fs, path));
    return E(jfs_mknod(jfs_mount_fs, path, (uint32_t)mode, (uint64_t)rdev, &c));
}

static int c_mkdir(const char *path, jmode_t mode)
{
    jfs_ctx c = caller();
    if (is_replay())
        return E(jfs_replay_create(jfs_mount_fs, path));
    return E(jfs_mkdir(jfs_mount_fs, path, (uint32_t)mode, &c));
}

static int c_unlink(const char *path)
{
    if (is_replay())
        return E(jfs_replay_remove(jfs_mount_fs, path));
    return E(jfs_unlink(jfs_mount_fs, path));
}

static int c_rmdir(const char *path)
{
    if (is_replay())
        return E(jfs_replay_remove(jfs_mount_fs, path));
    return E(jfs_rmdir(jfs_mount_fs, path));
}

static int c_symlink(const char *target, const char *path)
{
    jfs_ctx c = caller();
    if (is_replay())
        return E(jfs_replay_create(jfs_mount_fs, path));
    return E(jfs_symlink(jfs_mount_fs, target, path, &c));
}

static int c_rename(const char *from, const char *to, unsigned flags)
{
    unsigned jf = 0;
    if (flags & 1) /* RENAME_NOREPLACE */
        jf |= JFS_RENAME_NOREPLACE;
    if (flags & 2) /* RENAME_EXCHANGE */
        jf |= JFS_RENAME_EXCHANGE;
    if (flags & ~3u)
        return -EINVAL;
    if (is_replay())
        return E(jfs_replay_rename(jfs_mount_fs, from, to));
    return E(jfs_rename(jfs_mount_fs, from, to, jf));
}

static int c_link(const char *from, const char *to)
{
    if (is_replay())
        return E(jfs_replay_link(jfs_mount_fs, from, to));
    return E(jfs_link(jfs_mount_fs, from, to));
}

static int c_chmod(const char *path, jmode_t mode)
{
    if (is_replay())
        return E(jfs_replay_update(jfs_mount_fs, path));
    return E(jfs_chmod(jfs_mount_fs, path, (uint32_t)mode));
}

static int c_chown(const char *path, juid_t uid, jgid_t gid)
{
    if (is_replay())
        return E(jfs_replay_update(jfs_mount_fs, path));
    return E(jfs_chown(jfs_mount_fs, path, (uint32_t)uid, (uint32_t)gid));
}

static int c_truncate(const char *path, joff_t size, uint64_t fh, int has_fh)
{
    if (size < 0)
        return -EINVAL;
    if (is_replay())
        return E(has_fh && fh ? jfs_replay_update_handle(jfs_mount_fs, H(fh)) : jfs_replay_update(jfs_mount_fs, path));
    if (has_fh && fh)
        return E(jfs_ftruncate(jfs_mount_fs, H(fh), (uint64_t)size));
    return E(jfs_truncate(jfs_mount_fs, path, (uint64_t)size));
}

static int c_open(const char *path, int flags, uint64_t *fh)
{
    jfs_handle *h;
    int rc;
    if (is_replay()) {
        /* replayed open-for-write of a read-only web image (content update) */
        rc = jfs_open_file(jfs_mount_fs, path, JFS_O_RDONLY, &h);
        if (!rc)
            *fh = (uint64_t)(uintptr_t)h;
        return E(rc);
    }
    jfs_refresh_on_access(caller_pid()); /* strict refresh mode */
    rc = jfs_open_file(jfs_mount_fs, path, open_flags(flags), &h);
    if (!rc)
        *fh = (uint64_t)(uintptr_t)h;
    return E(rc);
}

static int c_create(const char *path, jmode_t mode, int flags, uint64_t *fh)
{
    jfs_handle *h;
    jfs_ctx c = caller();
    int rc;
    if (is_replay()) {
        rc = jfs_replay_create(jfs_mount_fs, path);
        if (!rc)
            rc = jfs_open_file(jfs_mount_fs, path, JFS_O_RDONLY, &h);
        if (!rc)
            *fh = (uint64_t)(uintptr_t)h;
        return E(rc);
    }
    rc = jfs_create(jfs_mount_fs, path, (uint32_t)mode, open_flags(flags) & ~JFS_O_TRUNC, &c, &h);
    if (!rc)
        *fh = (uint64_t)(uintptr_t)h;
    return E(rc);
}

static int c_read(char *buf, size_t size, joff_t off, uint64_t fh)
{
    int64_t n;
    if (off < 0)
        return -EINVAL;
    n = jfs_read(jfs_mount_fs, H(fh), buf, size, (uint64_t)off);
    return n < 0 ? E((int)n) : (int)n;
}

static int c_write(const char *buf, size_t size, joff_t off, uint64_t fh)
{
    int64_t n;
    if (off < 0)
        return -EINVAL;
    n = jfs_write(jfs_mount_fs, H(fh), buf, size, (uint64_t)off);
    return n < 0 ? E((int)n) : (int)n;
}

static int c_statfs(jstatvfs_t *sv)
{
    jfs_statvfs s;
    int rc = jfs_statfs(jfs_mount_fs, &s);
    if (rc)
        return E(rc);
    memset(sv, 0, sizeof *sv);
    sv->f_bsize = s.bsize;
    sv->f_frsize = s.bsize;
    sv->f_blocks = s.blocks;
    sv->f_bfree = s.bfree;
    sv->f_bavail = s.bavail;
    sv->f_files = s.files;
    sv->f_ffree = s.ffree;
    sv->f_favail = s.ffree;
    sv->f_fsid = s.fsid;
    sv->f_namemax = s.namemax;
#ifdef ST_RDONLY
    if (s.readonly)
        sv->f_flag |= ST_RDONLY;
#endif
    return 0;
}

static int c_flush(uint64_t fh)
{
    return E(jfs_flush(jfs_mount_fs, H(fh)));
}

static int c_release(uint64_t fh)
{
    return E(jfs_release(jfs_mount_fs, H(fh)));
}

static int c_fsync(uint64_t fh)
{
    return E(jfs_fsync(jfs_mount_fs, H(fh)));
}

/* setxattr flags as delivered by the kernel FUSE module */
#undef XATTR_CREATE
#undef XATTR_REPLACE
#if defined(__APPLE__)
#define XATTR_CREATE 0x0002
#define XATTR_REPLACE 0x0004
#else
#define XATTR_CREATE 0x1
#define XATTR_REPLACE 0x2
#endif

static int c_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    int jf = 0;
    if (flags & XATTR_CREATE)
        jf |= JFS_XATTR_CREATE;
    if (flags & XATTR_REPLACE)
        jf |= JFS_XATTR_REPLACE;
    return E(jfs_setxattr(jfs_mount_fs, path, name, value, size, jf));
}

static int c_getxattr(const char *path, const char *name, char *value, size_t size)
{
    int64_t n = jfs_getxattr(jfs_mount_fs, path, name, value, size);
    return n < 0 ? E((int)n) : (int)n;
}

static int c_listxattr(const char *path, char *list, size_t size)
{
    int64_t n = jfs_listxattr(jfs_mount_fs, path, list, size);
    return n < 0 ? E((int)n) : (int)n;
}

static int c_removexattr(const char *path, const char *name)
{
    return E(jfs_removexattr(jfs_mount_fs, path, name));
}

static int c_access(const char *path, int mask)
{
    jfs_ctx c = caller();
    int m = 0;
    if (mask & 4)
        m |= JFS_R_OK;
    if (mask & 2)
        m |= JFS_W_OK;
    if (mask & 1)
        m |= JFS_X_OK;
    return E(jfs_access(jfs_mount_fs, path, m, &c));
}

static int c_utimens(const char *path, const jtimespec_t tv[2])
{
    jfs_time t[2];
    if (is_replay())
        return E(jfs_replay_update(jfs_mount_fs, path));
    if (!tv)
        return E(jfs_utimens(jfs_mount_fs, path, NULL));
    t[0] = from_ts(&tv[0]);
    t[1] = from_ts(&tv[1]);
    return E(jfs_utimens(jfs_mount_fs, path, t));
}

static void *c_init(void)
{
    jfs_start_committer(jfs_mount_fs);
#ifdef _WIN32
    winfsp_capture_fuse();
#endif
    jfs_refresh_start();
    jfs_mount_signal_ready();
    return NULL;
}

static void c_destroy(void)
{
    int rc;
    if (!jfs_mount_fs)
        return;
    jfs_refresh_stop();
    rc = jfs_close(jfs_mount_fs);
    if (rc)
        jfs_log("final commit failed: %s", jfs_strerror(rc));
    jfs_mount_fs = NULL;
}

/* readdir */
typedef struct rd_ctx {
    void *buf;
    void *filler;
    int abi3;
} rd_ctx;

static int rd_fill(void *cookie, const char *name, const jfs_stat *s)
{
    rd_ctx *r = cookie;
    jstat_t st;
    to_stat(s, &st);
#if FUSE_ABI_3 && !defined(_WIN32)
    if (r->abi3)
        return ((fill3_t)r->filler)(r->buf, name, &st, 0, 0);
#endif
    return ((int (*)(void *, const char *, const jstat_t *, joff_t))r->filler)(r->buf, name, &st, 0);
}

/* ======================================================================== */
/* FUSE 2 wrappers (POSIX libfuse 2 / macFUSE / FUSE-T / WinFsp)              */
/* ======================================================================== */
static int f2_getattr(const char *p, jstat_t *st) { return c_getattr(p, st, 0, 0); }
static int f2_fgetattr(const char *p, jstat_t *st, jfi2_t *fi) { return c_getattr(p, st, fi ? fi->fh : 0, fi != NULL); }
static int f2_rename(const char *a, const char *b) { return c_rename(a, b, 0); }
static int f2_truncate(const char *p, joff_t s) { return c_truncate(p, s, 0, 0); }
static int f2_ftruncate(const char *p, joff_t s, jfi2_t *fi) { return c_truncate(p, s, fi ? fi->fh : 0, fi != NULL); }
static int f2_open(const char *p, jfi2_t *fi) { return c_open(p, fi->flags, &fi->fh); }
static int f2_create(const char *p, jmode_t m, jfi2_t *fi) { return c_create(p, m, fi->flags, &fi->fh); }
static int f2_read(const char *p, char *b, size_t s, joff_t o, jfi2_t *fi) { (void)p; return c_read(b, s, o, fi->fh); }
static int f2_write(const char *p, const char *b, size_t s, joff_t o, jfi2_t *fi) { (void)p; return c_write(b, s, o, fi->fh); }
static int f2_statfs(const char *p, jstatvfs_t *sv) { (void)p; return c_statfs(sv); }
static int f2_flush(const char *p, jfi2_t *fi) { (void)p; return c_flush(fi->fh); }
static int f2_release(const char *p, jfi2_t *fi) { (void)p; return c_release(fi->fh); }
static int f2_fsync(const char *p, int d, jfi2_t *fi) { (void)p; (void)d; return c_fsync(fi->fh); }
static int f2_opendir(const char *p, jfi2_t *fi) { (void)p; (void)fi; jfs_refresh_on_access(caller_pid()); return 0; }
static int f2_releasedir(const char *p, jfi2_t *fi) { (void)p; (void)fi; return 0; }
static int f2_fsyncdir(const char *p, int d, jfi2_t *fi) { (void)p; (void)d; (void)fi; return E(jfs_sync(jfs_mount_fs)); }
static void *f2_init(struct fuse_conn_info *c) { (void)c; return c_init(); }
static void f2_destroy(void *d) { (void)d; c_destroy(); }
static int f2_utimens(const char *p, const jtimespec_t tv[2]) { return c_utimens(p, tv); }
#if defined(__APPLE__)
static int f2_setxattr(const char *p, const char *n, const char *v, size_t s, int f, uint32_t pos)
{
    if (pos)
        return -ENOTSUP;
    return c_setxattr(p, n, v, s, f);
}
static int f2_getxattr(const char *p, const char *n, char *v, size_t s, uint32_t pos)
{
    if (pos)
        return -ENOTSUP;
    return c_getxattr(p, n, v, s);
}
#else
static int f2_setxattr(const char *p, const char *n, const char *v, size_t s, int f) { return c_setxattr(p, n, v, s, f); }
static int f2_getxattr(const char *p, const char *n, char *v, size_t s) { return c_getxattr(p, n, v, s); }
#endif
static int f2_readdir(const char *p, void *buf, void *filler, joff_t off, jfi2_t *fi)
{
    rd_ctx r;
    (void)off;
    (void)fi;
    r.buf = buf;
    r.filler = filler;
    r.abi3 = 0;
    return E(jfs_readdir(jfs_mount_fs, p, rd_fill, &r));
}

/* ======================================================================== */
/* FUSE 3 wrappers (POSIX only)                                               */
/* ======================================================================== */
#if !defined(_WIN32)
static int f3_getattr(const char *p, struct stat *st, jfi3_t *fi) { return c_getattr(p, st, fi ? fi->fh : 0, fi != NULL); }
static int f3_rename(const char *a, const char *b, unsigned f) { return c_rename(a, b, f); }
static int f3_chmod(const char *p, mode_t m, jfi3_t *fi) { (void)fi; return c_chmod(p, m); }
static int f3_chown(const char *p, uid_t u, gid_t g, jfi3_t *fi) { (void)fi; return c_chown(p, u, g); }
static int f3_truncate(const char *p, off_t s, jfi3_t *fi) { return c_truncate(p, s, fi ? fi->fh : 0, fi != NULL); }
static int f3_open(const char *p, jfi3_t *fi) { return c_open(p, fi->flags, &fi->fh); }
static int f3_create(const char *p, mode_t m, jfi3_t *fi) { return c_create(p, m, fi->flags, &fi->fh); }
static int f3_read(const char *p, char *b, size_t s, off_t o, jfi3_t *fi) { (void)p; return c_read(b, s, o, fi->fh); }
static int f3_write(const char *p, const char *b, size_t s, off_t o, jfi3_t *fi) { (void)p; return c_write(b, s, o, fi->fh); }
static int f3_statfs(const char *p, struct statvfs *sv) { (void)p; return c_statfs(sv); }
static int f3_flush(const char *p, jfi3_t *fi) { (void)p; return c_flush(fi->fh); }
static int f3_release(const char *p, jfi3_t *fi) { (void)p; return c_release(fi->fh); }
static int f3_fsync(const char *p, int d, jfi3_t *fi) { (void)p; (void)d; return c_fsync(fi->fh); }
static int f3_opendir(const char *p, jfi3_t *fi) { (void)p; (void)fi; jfs_refresh_on_access(caller_pid()); return 0; }
static int f3_releasedir(const char *p, jfi3_t *fi) { (void)p; (void)fi; return 0; }
static int f3_fsyncdir(const char *p, int d, jfi3_t *fi) { (void)p; (void)d; (void)fi; return E(jfs_sync(jfs_mount_fs)); }
static void *f3_init(struct fuse_conn_info *c, struct jfuse_config3 *cfg)
{
    (void)c;
    if (cfg) {
        cfg->use_ino = 1;
        cfg->hard_remove = 1;
        if (jfs_mount_cfg.shared) {
            cfg->entry_timeout = 0;
            cfg->attr_timeout = 0;
            cfg->negative_timeout = 0;
        }
    }
    return c_init();
}
static int f3_utimens(const char *p, const struct timespec tv[2], jfi3_t *fi) { (void)fi; return c_utimens(p, tv); }
static int f3_readdir(const char *p, void *buf, fill3_t filler, off_t off, jfi3_t *fi, int flags)
{
    rd_ctx r;
    (void)off;
    (void)fi;
    (void)flags;
    r.buf = buf;
    r.filler = (void *)filler;
    r.abi3 = 1;
    return E(jfs_readdir(jfs_mount_fs, p, rd_fill, &r));
}
#endif

/* ======================================================================== */
/* Entry points                                                               */
/* ======================================================================== */
int jfs_fuse_available(char *why, size_t whylen)
{
#ifdef _WIN32
    return winfsp_load(why, whylen);
#else
    const char *env = getenv("JSONFS_FUSE_LIBRARY");
    static const struct {
        const char *name;
        int major;
    } cands[] = {
#if defined(__APPLE__)
        {"/usr/local/lib/libfuse-t.dylib", 2},
        {"libfuse-t.dylib", 2},
        {"/usr/local/lib/libfuse.2.dylib", 2},
        {"/opt/homebrew/lib/libfuse.2.dylib", 2},
        {"libfuse.2.dylib", 2},
        {"/usr/local/lib/libosxfuse.2.dylib", 2},
#else
        {"libfuse3.so.3", 3},
        {"libfuse.so.2", 2},
        {"/usr/local/lib/libfuse3.so.3", 3},
        {"/usr/local/lib/libfuse.so.2", 2},
        {"libfuse.so", 2},
#endif
        {NULL, 0}};
    int i;
    if (fuse_lib)
        return 1;
    if (env && *env) {
        if (try_load(env, 0))
            return 1;
        snprintf(why, whylen, "cannot load %s: %s", env, dlerror());
        return 0;
    }
    for (i = 0; cands[i].name; i++)
        if (try_load(cands[i].name, cands[i].major))
            return 1;
    snprintf(why, whylen, "no FUSE library found (tried libfuse3, libfuse 2%s)",
#if defined(__APPLE__)
             ", macFUSE and FUSE-T"
#else
             ""
#endif
    );
    return 0;
#endif
}

static void push(char ***argv, int *argc, const char *s)
{
    *argv = jfs_realloc(*argv, (size_t)(*argc + 2) * sizeof(char *));
    (*argv)[(*argc)++] = jfs_strdup(s);
    (*argv)[*argc] = NULL;
}

int jfs_fuse_run(void)
{
    char **argv = NULL, *opts;
    int argc = 0, rc;
    int web_events = 0, allow_other = 0;
    (void)web_events;
    (void)allow_other;
    jfs_buf o;
    jfs_info info;

    jfs_get_info(jfs_mount_fs, &info);
    jfs_buf_init(&o);
#ifndef _WIN32
    {
        const char *pt = jfs_mount_cfg.passthrough ? jfs_mount_cfg.passthrough : "";
        allow_other = strstr(pt, "allow_other") != NULL;
        web_events = jfs_is_remote(jfs_mount_fs) && jfs_mount_cfg.events && !jfs_mount_cfg.single_thread &&
                     jfs_mount_cfg.refresh_mode != JFS_REFRESH_OFF;
    }
#endif
#ifdef _WIN32
    jfs_buf_printf(&o, "uid=-1,gid=-1,FileSystemName=JSONFS,volname=%s",
                   jfs_mount_cfg.volname ? jfs_mount_cfg.volname : (info.label[0] ? info.label : "JSONfs"));
    if (jfs_mount_cfg.readonly)
        jfs_buf_puts(&o, ",ro");
    jfs_buf_puts(&o, ",umask=022");
#else
    {
        /* fsname must not contain commas */
        char shown[1024], *fsn, *q;
        jfs_url_redact(jfs_mount_cfg.image, shown, sizeof shown); /* no passwords in the mount table */
        fsn = jfs_asprintf("jsonfs:%s", shown);
        for (q = fsn; *q; q++)
            if (*q == ',')
                *q = '_';
        /* attr_timeout=0: the high-level FUSE API gives every hard link its own
         * kernel inode, so cached attributes could go stale across links */
        /* web images that propagate remote changes are mounted rw at the kernel
         * level (the volume itself refuses writes) so that replayed changes
         * can pass; the volume is private to the mounting user, and permission
         * checks are left to the kernel only when it is shared (allow_other) */
        jfs_buf_printf(&o, "fsname=%s%s,attr_timeout=0", fsn, web_events && !allow_other ? "" : ",default_permissions");
        free(fsn);
    }
#if defined(__linux__) || defined(__FreeBSD__)
    jfs_buf_puts(&o, ",subtype=jsonfs");
#endif
#if defined(__APPLE__)
    jfs_buf_printf(&o, ",volname=%s",
                   jfs_mount_cfg.volname ? jfs_mount_cfg.volname : (info.label[0] ? info.label : "JSONfs"));
#endif
    if (fuse_major == 2)
        jfs_buf_puts(&o, ",use_ino,hard_remove");
    if (jfs_mount_cfg.readonly && !web_events)
        jfs_buf_puts(&o, ",ro");
    if (jfs_mount_cfg.shared)
        jfs_buf_puts(&o, ",entry_timeout=0,negative_timeout=0");
#endif
    if (jfs_mount_cfg.passthrough) {
        jfs_buf_putc(&o, ',');
        jfs_buf_puts(&o, jfs_mount_cfg.passthrough);
    }
    opts = jfs_buf_steal(&o, NULL);

    push(&argv, &argc, "jsonfs");
    push(&argv, &argc, "-f");
    if (jfs_mount_cfg.debug)
        push(&argv, &argc, "-d");
    if (jfs_mount_cfg.single_thread)
        push(&argv, &argc, "-s");
    push(&argv, &argc, "-o");
    push(&argv, &argc, opts);
    push(&argv, &argc, jfs_mount_cfg.mountpoint);
    free(opts);
    JFS_DEBUG("fuse args: %s %s", argv[argc - 2], argv[argc - 1]);

#ifdef _WIN32
    {
        static struct fuse_operations ops;
        ops.getattr = f2_getattr;
        ops.fgetattr = f2_fgetattr;
        ops.readlink = c_readlink;
        ops.mknod = c_mknod;
        ops.mkdir = c_mkdir;
        ops.unlink = c_unlink;
        ops.rmdir = c_rmdir;
        ops.symlink = c_symlink;
        ops.rename = f2_rename;
        ops.link = c_link;
        ops.chmod = c_chmod;
        ops.chown = c_chown;
        ops.truncate = f2_truncate;
        ops.ftruncate = f2_ftruncate;
        ops.open = f2_open;
        ops.create = f2_create;
        ops.read = f2_read;
        ops.write = f2_write;
        ops.statfs = f2_statfs;
        ops.flush = f2_flush;
        ops.release = f2_release;
        ops.fsync = f2_fsync;
        ops.setxattr = f2_setxattr;
        ops.getxattr = f2_getxattr;
        ops.listxattr = c_listxattr;
        ops.removexattr = c_removexattr;
        ops.opendir = f2_opendir;
        ops.readdir = (int (*)(const char *, void *, fuse_fill_dir_t, fuse_off_t, struct fuse_file_info *))f2_readdir;
        ops.releasedir = f2_releasedir;
        ops.fsyncdir = f2_fsyncdir;
        ops.init = f2_init;
        ops.destroy = f2_destroy;
        ops.access = c_access;
        ops.utimens = f2_utimens;
        fprintf(stderr, "jsonfs: mounting %s on %s via WinFsp (Ctrl+C to unmount)\n", jfs_mount_cfg.image,
                jfs_mount_cfg.mountpoint);
        rc = fuse_main_real(argc, argv, &ops, sizeof ops, NULL);
    }
#else
    if (fuse_major == 3) {
        static struct fuse_operations3 ops;
        ops.getattr = f3_getattr;
        ops.readlink = c_readlink;
        ops.mknod = c_mknod;
        ops.mkdir = c_mkdir;
        ops.unlink = c_unlink;
        ops.rmdir = c_rmdir;
        ops.symlink = c_symlink;
        ops.rename = f3_rename;
        ops.link = c_link;
        ops.chmod = f3_chmod;
        ops.chown = f3_chown;
        ops.truncate = f3_truncate;
        ops.open = f3_open;
        ops.read = f3_read;
        ops.write = f3_write;
        ops.statfs = f3_statfs;
        ops.flush = f3_flush;
        ops.release = f3_release;
        ops.fsync = f3_fsync;
        ops.setxattr = c_setxattr;
        ops.getxattr = c_getxattr;
        ops.listxattr = c_listxattr;
        ops.removexattr = c_removexattr;
        ops.opendir = f3_opendir;
        ops.readdir = f3_readdir;
        ops.releasedir = f3_releasedir;
        ops.fsyncdir = f3_fsyncdir;
        ops.init = f3_init;
        ops.destroy = f2_destroy;
        ops.access = c_access;
        ops.create = f3_create;
        ops.utimens = f3_utimens;
        JFS_DEBUG("using %s (FUSE 3.%d API)", fuse_lib_name, fuse_minor);
        rc = p_fuse_main_real(argc, argv, &ops, sizeof ops, NULL);
    } else {
        static struct fuse_operations2 ops;
        ops.getattr = f2_getattr;
        ops.fgetattr = f2_fgetattr;
        ops.readlink = c_readlink;
        ops.mknod = c_mknod;
        ops.mkdir = c_mkdir;
        ops.unlink = c_unlink;
        ops.rmdir = c_rmdir;
        ops.symlink = c_symlink;
        ops.rename = f2_rename;
        ops.link = c_link;
        ops.chmod = c_chmod;
        ops.chown = c_chown;
        ops.truncate = f2_truncate;
        ops.ftruncate = f2_ftruncate;
        ops.open = f2_open;
        ops.create = f2_create;
        ops.read = f2_read;
        ops.write = f2_write;
        ops.statfs = f2_statfs;
        ops.flush = f2_flush;
        ops.release = f2_release;
        ops.fsync = f2_fsync;
        ops.setxattr = f2_setxattr;
        ops.getxattr = f2_getxattr;
        ops.listxattr = c_listxattr;
        ops.removexattr = c_removexattr;
        ops.opendir = f2_opendir;
        ops.readdir = (int (*)(const char *, void *, fill2_t, off_t, jfi2_t *))f2_readdir;
        ops.releasedir = f2_releasedir;
        ops.fsyncdir = f2_fsyncdir;
        ops.init = f2_init;
        ops.destroy = f2_destroy;
        ops.access = c_access;
        ops.utimens = f2_utimens;
        ops.flag_nullpath_ok = 1;
        ops.flag_utime_omit_ok = 1;
        JFS_DEBUG("using %s (FUSE 2 API)", fuse_lib_name);
        rc = p_fuse_main_real(argc, argv, &ops, sizeof ops, NULL);
    }
#endif
    /* fuse_main returns after unmount; destroy() normally closed the volume */
    if (jfs_mount_fs) {
        jfs_close(jfs_mount_fs);
        jfs_mount_fs = NULL;
    }
    {
        int i;
        for (i = 0; i < argc; i++)
            free(argv[i]);
        free(argv);
    }
    return rc ? 1 : 0;
}

#endif /* !(WIN32 && !WINFSP) */
