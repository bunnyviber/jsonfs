/*
 * JSONfs - a POSIX filesystem stored in a single JSON document.
 *
 * jfs_util.h - portability layer and small utilities shared by every
 * component (library, maintenance toolkit and mount drivers).
 *
 * Everything in here is plain C99 and builds on Linux, macOS, FreeBSD and
 * on Windows starting with Windows XP (MinGW / MSVC).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_UTIL_H
#define JFS_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef _WIN32
#define JFS_WINDOWS 1
#endif

#define JFS_VERSION_STRING "1.0.0"

/* ------------------------------------------------------------------------ */
/* Error codes.                                                               */
/*                                                                            */
/* The library reports errors as negative JFS_E* values.  They use the Linux  */
/* errno numbering, but they are *not* native errno values: frontends map     */
/* them with jfs_err_to_errno() because e.g. ENOTEMPTY is 39 on Linux and 66  */
/* on BSD/macOS.                                                              */
/* ------------------------------------------------------------------------ */
enum {
    JFS_OK = 0,
    JFS_EPERM = 1,
    JFS_ENOENT = 2,
    JFS_EIO = 5,
    JFS_EBADF = 9,
    JFS_EAGAIN = 11,
    JFS_ENOMEM = 12,
    JFS_EACCES = 13,
    JFS_EBUSY = 16,
    JFS_EEXIST = 17,
    JFS_EXDEV = 18,
    JFS_ENOTDIR = 20,
    JFS_EISDIR = 21,
    JFS_EINVAL = 22,
    JFS_EFBIG = 27,
    JFS_ENOSPC = 28,
    JFS_EROFS = 30,
    JFS_EMLINK = 31,
    JFS_ERANGE = 34,
    JFS_ENAMETOOLONG = 36,
    JFS_ENOSYS = 38,
    JFS_ENOTEMPTY = 39,
    JFS_ELOOP = 40,
    JFS_ENOATTR = 61, /* ENODATA on Linux, ENOATTR on BSD/macOS */
    JFS_ENOTSUP = 95,
    /* JSONfs specific (never handed to a kernel) */
    JFS_ECORRUPT = 1000, /* image is malformed */
    JFS_EVERSION = 1001, /* unsupported schema version */
    JFS_ELOCKED = 1002,  /* image is in use by somebody else */
    JFS_ECHECKSUM = 1003 /* checksum mismatch */
};

const char *jfs_strerror(int err); /* accepts positive or negative codes */
int jfs_err_to_errno(int err);     /* JFS_E* (pos/neg) -> positive native errno */
int jfs_err_from_errno(int native_errno); /* native errno -> negative JFS_E* */

/* ------------------------------------------------------------------------ */
/* Memory helpers (abort on OOM: a half-built image is worse than a crash).  */
/* ------------------------------------------------------------------------ */
void *jfs_malloc(size_t n);
void *jfs_calloc(size_t n, size_t m);
void *jfs_realloc(void *p, size_t n);
char *jfs_strdup(const char *s);
char *jfs_strndup(const char *s, size_t n);
char *jfs_asprintf(const char *fmt, ...);

/* ------------------------------------------------------------------------ */
/* Growable byte buffer.                                                      */
/* ------------------------------------------------------------------------ */
typedef struct jfs_buf {
    char *data;
    size_t len;
    size_t cap;
} jfs_buf;

void jfs_buf_init(jfs_buf *b);
void jfs_buf_free(jfs_buf *b);
void jfs_buf_reserve(jfs_buf *b, size_t extra);
void jfs_buf_append(jfs_buf *b, const void *p, size_t n);
void jfs_buf_putc(jfs_buf *b, char c);
void jfs_buf_puts(jfs_buf *b, const char *s);
void jfs_buf_printf(jfs_buf *b, const char *fmt, ...);
char *jfs_buf_steal(jfs_buf *b, size_t *len); /* NUL terminated */

/* ------------------------------------------------------------------------ */
/* Time.                                                                      */
/* ------------------------------------------------------------------------ */
typedef struct jfs_time {
    int64_t sec;  /* seconds since the Unix epoch (UTC) */
    int32_t nsec; /* 0 .. 999999999 */
} jfs_time;

jfs_time jfs_now(void);
int jfs_time_cmp(jfs_time a, jfs_time b);
/* RFC 3339 / ISO 8601 UTC with nanoseconds: 2026-11-11T12:34:56.123456789Z */
#define JFS_TIME_STRLEN 40
void jfs_time_format(jfs_time t, char out[JFS_TIME_STRLEN]);
/* Strict parser for the stored format (also accepts fewer fraction digits
 * and numeric offsets). Returns 0 on success. */
int jfs_time_parse(const char *s, jfs_time *out);
/* Lenient parser for user input: everything jfs_time_parse() accepts, plus
 * "YYYY-MM-DD", "YYYY-MM-DD HH:MM[:SS]", "@<epoch>", "<epoch>", "now",
 * and relative offsets such as "-2h", "-30m", "-7d", "-1w", "-45s". */
int jfs_time_parse_user(const char *s, jfs_time *out);
int64_t jfs_days_from_civil(int64_t y, unsigned m, unsigned d);
void jfs_civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d);

/* ------------------------------------------------------------------------ */
/* Randomness / identifiers.                                                  */
/* ------------------------------------------------------------------------ */
int jfs_random_bytes(void *out, size_t n);
void jfs_uuid4(char out[37]);
int jfs_uuid_valid(const char *s);

/* ------------------------------------------------------------------------ */
/* Files (paths are UTF-8 on every platform).                                 */
/* ------------------------------------------------------------------------ */
int jfs_read_file(const char *path, char **data, size_t *len);
/* Atomically replace `path` with `data` (temp file + fsync + rename). */
int jfs_write_file_atomic(const char *path, const void *data, size_t len);
int jfs_file_exists(const char *path);
int jfs_remove_file(const char *path);
/* Bytes free / total on the volume holding `path`. */
int jfs_disk_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);
/* Cheap change detector for an on-disk file (size, mtime, file id). */
typedef struct jfs_filesig {
    uint64_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    uint64_t id;
    int valid;
} jfs_filesig;
int jfs_file_signature(const char *path, jfs_filesig *sig);
int jfs_filesig_equal(const jfs_filesig *a, const jfs_filesig *b);

/* ------------------------------------------------------------------------ */
/* Lock file used for single/multi attach coordination.                       */
/*                                                                            */
/* <image>.lock carries two byte-range locks (far away from the data so that  */
/* Windows' mandatory range locks never block reading the payload):          */
/*   ATTACH  - held for the lifetime of a mount (exclusive for single-attach, */
/*             shared for multi-attach) and exclusively by the offline tools.*/
/*   TXN     - held around every operation of a multi-attach mount.          */
/* The payload holds the image generation counter so that a multi-attached   */
/* mount can cheaply detect commits made by its peers.                        */
/* ------------------------------------------------------------------------ */
typedef struct jfs_lockfile jfs_lockfile;
enum { JFS_LOCK_ATTACH = 0, JFS_LOCK_TXN = 1 };
int jfs_lockfile_open(const char *image_path, jfs_lockfile **out);
/* exclusive: 1/0; wait: block until available. Returns JFS_ELOCKED if busy. */
int jfs_lockfile_lock(jfs_lockfile *lf, int which, int exclusive, int wait);
void jfs_lockfile_unlock(jfs_lockfile *lf, int which);
/* Like jfs_lockfile_lock(.., wait=0) but retries for up to timeout_ms
 * (a mount that is being unmounted holds its lock until its final commit). */
int jfs_lockfile_lock_timeout(jfs_lockfile *lf, int which, int exclusive, unsigned timeout_ms);
/* Default for offline tools: $JSONFS_LOCK_TIMEOUT seconds, else 10 s. */
unsigned jfs_lock_timeout_ms(void);
int jfs_lockfile_read_gen(jfs_lockfile *lf, uint64_t *gen, char uuid[37]);
int jfs_lockfile_write_gen(jfs_lockfile *lf, uint64_t gen, const char *uuid);
void jfs_lockfile_close(jfs_lockfile *lf, int remove_file);

/* ------------------------------------------------------------------------ */
/* Threads.                                                                   */
/* ------------------------------------------------------------------------ */
typedef struct jfs_mutex {
    void *impl;
} jfs_mutex;
void jfs_mutex_init(jfs_mutex *m);
void jfs_mutex_destroy(jfs_mutex *m);
void jfs_mutex_lock(jfs_mutex *m);
void jfs_mutex_unlock(jfs_mutex *m);
typedef struct jfs_thread jfs_thread;
int jfs_thread_start(jfs_thread **t, void (*fn)(void *), void *arg);
void jfs_thread_join(jfs_thread *t);
void jfs_sleep_ms(unsigned ms);

/* ------------------------------------------------------------------------ */
/* Hash maps.                                                                 */
/* ------------------------------------------------------------------------ */
/* uint64 -> pointer (inode table). */
typedef struct jfs_u64map {
    uint64_t *keys;
    void **vals;
    size_t cap; /* power of two */
    size_t count;
} jfs_u64map;
void jfs_u64map_init(jfs_u64map *m);
void jfs_u64map_free(jfs_u64map *m);
void *jfs_u64map_get(const jfs_u64map *m, uint64_t key);
void jfs_u64map_put(jfs_u64map *m, uint64_t key, void *val);
void *jfs_u64map_del(jfs_u64map *m, uint64_t key);
/* Iterate: for (i = 0; i < m->cap; i++) if (m->vals[i]) ... */

/* string -> uint64 (directory entries). Keys are owned by the map. */
typedef struct jfs_strmap {
    char **keys;
    uint64_t *vals;
    size_t cap;
    size_t count;
} jfs_strmap;
void jfs_strmap_init(jfs_strmap *m);
void jfs_strmap_free(jfs_strmap *m);
int jfs_strmap_get(const jfs_strmap *m, const char *key, uint64_t *val);
void jfs_strmap_put(jfs_strmap *m, const char *key, uint64_t val);
int jfs_strmap_del(jfs_strmap *m, const char *key);
void jfs_strmap_copy(jfs_strmap *dst, const jfs_strmap *src);
/* Returns a newly allocated array of the keys sorted bytewise (not copies). */
const char **jfs_strmap_sorted_keys(const jfs_strmap *m);

/* ------------------------------------------------------------------------ */
/* Misc.                                                                      */
/* ------------------------------------------------------------------------ */
void jfs_log(const char *fmt, ...);
void jfs_set_log_sink(void (*sink)(const char *msg));
extern int jfs_debug;
#define JFS_DEBUG(...)            \
    do {                          \
        if (jfs_debug)            \
            jfs_log(__VA_ARGS__); \
    } while (0)

#ifdef JFS_WINDOWS
/* UTF-8 <-> UTF-16 helpers (callers free the result). */
wchar_t *jfs_utf8_to_wide(const char *s);
char *jfs_wide_to_utf8(const wchar_t *w);
#endif

#endif /* JFS_UTIL_H */
