/*
 * JSONfs - public filesystem API.
 *
 * A JSONfs volume is a single JSON document (see docs/FORMAT.md).  This API
 * loads it, exposes POSIX semantics on top of it (used by every mount
 * driver and by the toolkit's import/export commands) and writes it back
 * atomically.
 *
 * All functions are thread-safe (a volume-wide lock serializes them).
 * Errors are returned as negative JFS_E* codes (see jfs_util.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_FS_H
#define JFS_FS_H

#include "jfs_util.h"

#define JFS_FORMAT_NAME "jsonfs"
#define JFS_SCHEMA_MIN 1
#define JFS_SCHEMA_LATEST 2

/* File type bits (identical on Linux, BSD and macOS). */
#define JFS_S_IFMT 0170000
#define JFS_S_IFSOCK 0140000
#define JFS_S_IFLNK 0120000
#define JFS_S_IFREG 0100000
#define JFS_S_IFBLK 0060000
#define JFS_S_IFDIR 0040000
#define JFS_S_IFCHR 0020000
#define JFS_S_IFIFO 0010000
#define JFS_S_ISUID 0004000
#define JFS_S_ISGID 0002000
#define JFS_S_ISVTX 0001000

/* open() flags understood by jfs_open_file()/jfs_create() */
#define JFS_O_RDONLY 0x0
#define JFS_O_WRONLY 0x1
#define JFS_O_RDWR 0x2
#define JFS_O_ACCMODE 0x3
#define JFS_O_TRUNC 0x10
#define JFS_O_EXCL 0x20
#define JFS_O_APPEND 0x40

/* rename flags */
#define JFS_RENAME_NOREPLACE 1
#define JFS_RENAME_EXCHANGE 2

/* xattr flags */
#define JFS_XATTR_CREATE 1
#define JFS_XATTR_REPLACE 2

/* utimens special nanosecond values */
#define JFS_UTIME_NOW (-1)
#define JFS_UTIME_OMIT (-2)

/* access() mask */
#define JFS_R_OK 4
#define JFS_W_OK 2
#define JFS_X_OK 1

typedef struct jfs_fs jfs_fs;
typedef struct jfs_handle jfs_handle;

typedef struct jfs_stat {
    uint64_t ino;
    uint32_t mode; /* type | permissions */
    uint32_t nlink;
    uint32_t uid, gid;
    uint64_t rdev;
    uint64_t size;
    uint64_t blocks; /* 512-byte units (compressed footprint estimate) */
    jfs_time atime, mtime, ctime, btime;
} jfs_stat;

typedef struct jfs_statvfs {
    uint64_t bsize;
    uint64_t blocks, bfree, bavail;
    uint64_t files, ffree;
    uint64_t namemax;
    uint64_t fsid;
    int readonly;
} jfs_statvfs;

/* Credentials of the calling process (used for new files and access()). */
typedef struct jfs_ctx {
    uint32_t uid, gid;
    uint32_t umask; /* applied to create modes by the library */
} jfs_ctx;

/* ---------------------------------------------------------------------- */
/* Creating, opening and closing images                                    */
/* ---------------------------------------------------------------------- */
typedef struct jfs_mkfs_opts {
    const char *label;       /* may be NULL */
    const char *compression; /* default compression; NULL = "deflate" */
    int versioning;          /* enable time/date based versioning */
    int schema;              /* 0 = latest */
    const char *uuid;        /* NULL = random */
    uint32_t root_uid, root_gid;
    uint32_t root_mode; /* 0 = 0755 */
} jfs_mkfs_opts;

int jfs_mkfs(const char *image, const jfs_mkfs_opts *o, int overwrite);

enum {
    JFS_LOCKMODE_EXCLUSIVE = 0, /* single attach (default for mounts) */
    JFS_LOCKMODE_SHARED = 1,    /* multi-attach: coordinate with peers */
    JFS_LOCKMODE_OFFLINE = 2,   /* toolkit: exclusive, fail fast if mounted */
    JFS_LOCKMODE_NONE = 3       /* no locking at all (read-only views) */
};

typedef struct jfs_open_opts {
    int readonly;
    int lockmode;            /* JFS_LOCKMODE_* */
    const char *compression; /* algorithm for newly written data (NULL = image default) */
    int has_at;              /* time travel: show the state at `at` (forces readonly) */
    jfs_time at;
    int commit_interval_ms;  /* exclusive mode: <=0 commits after every change */
    int repair;              /* tolerate/fix inconsistencies while loading */
} jfs_open_opts;

void jfs_open_opts_init(jfs_open_opts *o);
int jfs_open(const char *image, const jfs_open_opts *o, jfs_fs **out, char *err, size_t errlen);
/* Commit pending changes (no-op when clean). */
int jfs_sync(jfs_fs *fs);
/* Commit, release locks and free. */
int jfs_close(jfs_fs *fs);
/* Starts the periodic background committer (exclusive mode). Call after a
 * possible fork()/daemonize. */
int jfs_start_committer(jfs_fs *fs);
void jfs_stop_committer(jfs_fs *fs);

typedef struct jfs_info {
    char uuid[37];
    char label[256];
    int schema;
    jfs_time created, modified;
    uint64_t generation;
    char default_compression[32];
    char write_compression[32];
    int versioning;
    int readonly;
    int shared;
    int is_view;
    jfs_time view_time;
    uint64_t inodes, files, dirs, symlinks, others, deleted_markers, history_records;
    uint64_t logical_bytes;
} jfs_info;
void jfs_get_info(jfs_fs *fs, jfs_info *info);

/* ---------------------------------------------------------------------- */
/* POSIX operations (paths are absolute, '/'-separated, UTF-8)             */
/* ---------------------------------------------------------------------- */
int jfs_getattr(jfs_fs *fs, const char *path, jfs_stat *st);
int jfs_fgetattr(jfs_fs *fs, jfs_handle *h, jfs_stat *st);
int jfs_access(jfs_fs *fs, const char *path, int mask, const jfs_ctx *ctx);
int jfs_readlink(jfs_fs *fs, const char *path, char *buf, size_t size);
int jfs_mknod(jfs_fs *fs, const char *path, uint32_t mode, uint64_t rdev, const jfs_ctx *ctx);
int jfs_mkdir(jfs_fs *fs, const char *path, uint32_t mode, const jfs_ctx *ctx);
int jfs_unlink(jfs_fs *fs, const char *path);
int jfs_rmdir(jfs_fs *fs, const char *path);
int jfs_symlink(jfs_fs *fs, const char *target, const char *linkpath, const jfs_ctx *ctx);
int jfs_rename(jfs_fs *fs, const char *from, const char *to, unsigned flags);
int jfs_link(jfs_fs *fs, const char *from, const char *to);
int jfs_chmod(jfs_fs *fs, const char *path, uint32_t mode);
int jfs_chown(jfs_fs *fs, const char *path, uint32_t uid, uint32_t gid); /* (uint32_t)-1 = keep */
int jfs_truncate(jfs_fs *fs, const char *path, uint64_t size);
int jfs_ftruncate(jfs_fs *fs, jfs_handle *h, uint64_t size);
/* times: [0]=atime [1]=mtime; nsec may be JFS_UTIME_NOW / JFS_UTIME_OMIT; NULL = now */
int jfs_utimens(jfs_fs *fs, const char *path, const jfs_time times[2]);
int jfs_set_btime(jfs_fs *fs, const char *path, jfs_time btime);

int jfs_open_file(jfs_fs *fs, const char *path, int flags, jfs_handle **h);
int jfs_create(jfs_fs *fs, const char *path, uint32_t mode, int flags, const jfs_ctx *ctx, jfs_handle **h);
/* Returns bytes read/written (>=0) or a negative error. */
int64_t jfs_read(jfs_fs *fs, jfs_handle *h, void *buf, size_t size, uint64_t off);
int64_t jfs_write(jfs_fs *fs, jfs_handle *h, const void *buf, size_t size, uint64_t off);
int jfs_flush(jfs_fs *fs, jfs_handle *h);
int jfs_fsync(jfs_fs *fs, jfs_handle *h);
int jfs_release(jfs_fs *fs, jfs_handle *h);

/* Calls fill() for ".", ".." and every entry (sorted). Stop if fill != 0. */
typedef int (*jfs_fill_fn)(void *cookie, const char *name, const jfs_stat *st);
int jfs_readdir(jfs_fs *fs, const char *path, jfs_fill_fn fill, void *cookie);
int jfs_statfs(jfs_fs *fs, jfs_statvfs *sv);

int jfs_setxattr(jfs_fs *fs, const char *path, const char *name, const void *value, size_t size, int flags);
/* Returns the value size; with size==0 only reports the size. */
int64_t jfs_getxattr(jfs_fs *fs, const char *path, const char *name, void *value, size_t size);
/* NUL-separated names; with size==0 only reports the size. */
int64_t jfs_listxattr(jfs_fs *fs, const char *path, char *list, size_t size);
int jfs_removexattr(jfs_fs *fs, const char *path, const char *name);

/* ---------------------------------------------------------------------- */
/* Offline maintenance (jfs_maint.c)                                       */
/* ---------------------------------------------------------------------- */
typedef void (*jfs_report_fn)(void *cookie, int severity /*0 info,1 warn,2 error*/, const char *msg);

typedef struct jfs_fsck_result {
    uint64_t inodes, files_checked, records_checked, bytes_checked;
    uint64_t errors, warnings, repaired;
} jfs_fsck_result;
int jfs_fsck(const char *image, int repair, int verbose, jfs_report_fn report, void *cookie, jfs_fsck_result *res);

int jfs_snapshot(const char *image, jfs_time at, const char *out_image, int keep_versioning, const char *label,
                 int overwrite);
int jfs_set_versioning(const char *image, int enable, int purge_history, uint64_t *purged);
int jfs_recompress(const char *image, const char *algo, int set_default, int include_history, uint64_t *count,
                   uint64_t *bytes_before, uint64_t *bytes_after);
int jfs_upgrade(const char *image, int target_schema, int *from_schema);
/* Removes history records (and delete markers) older than `before`. */
int jfs_prune_history(const char *image, jfs_time before, uint64_t *removed);

/* List the versions of the inode reachable at `path` (current view). */
typedef struct jfs_version_info {
    jfs_time vtime;
    int deleted;
    int is_current;
    char type[16];
    uint64_t size;
    char sha256[65];
    char compression[32];
} jfs_version_info;
typedef int (*jfs_version_fn)(void *cookie, const jfs_version_info *vi);
int jfs_list_versions(const char *image, const char *path, const jfs_time *at, jfs_version_fn fn, void *cookie);

#endif
