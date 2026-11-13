/*
 * JSONfs - archive conversion (see jfs_archive.h).
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

#include "jfs_archive.h"
#include "jfs_codec.h"

#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/types.h>
#endif

#define AR_SKIP 1 /* entry skipped (not an error) */

void jfs_archive_opts_init(jfs_archive_opts *o)
{
    memset(o, 0, sizeof *o);
    o->same_owner = 1;
    o->level = -1;
}

static void warnf(const jfs_archive_opts *o, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (o && o->warn)
        o->warn(o->cookie, buf);
}

/* ======================================================================== */
/* Format names                                                               */
/* ======================================================================== */
typedef struct fmt_info {
    const char *ext;
    const char *name;
    const char *la_format; /* libarchive writer format (NULL: read only) */
    const char *la_filter; /* libarchive writer filter (NULL: none) */
    int native;            /* 1 tar, 2 tar.gz, 3 zip */
} fmt_info;

static const fmt_info formats[] = {
    {".tar.gz", "tar.gz", "paxr", "gzip", 2},
    {".tgz", "tar.gz", "paxr", "gzip", 2},
    {".tar", "tar", "paxr", NULL, 1},
    {".zip", "zip", "zip", NULL, 3},
    {".jar", "zip", "zip", NULL, 3},
    {".7z", "7z", "7zip", NULL, 0},
    {".tar.xz", "tar.xz", "paxr", "xz", 0},
    {".txz", "tar.xz", "paxr", "xz", 0},
    {".tar.bz2", "tar.bz2", "paxr", "bzip2", 0},
    {".tbz2", "tar.bz2", "paxr", "bzip2", 0},
    {".tbz", "tar.bz2", "paxr", "bzip2", 0},
    {".tar.zst", "tar.zst", "paxr", "zstd", 0},
    {".tzst", "tar.zst", "paxr", "zstd", 0},
    {".tar.lz4", "tar.lz4", "paxr", "lz4", 0},
    {".tar.lzma", "tar.lzma", "paxr", "lzma", 0},
    {".tar.lz", "tar.lz", "paxr", "lzip", 0},
    {".cpio", "cpio", "cpio", NULL, 0},
    {".iso", "iso", "iso9660", NULL, 0},
    {".xar", "xar", "xar", NULL, 0},
    {".ar", "ar", "ar", NULL, 0},
    {".shar", "shar", "shar", NULL, 0},
    {".warc", "warc", "warc", NULL, 0},
    {".rar", "rar", NULL, NULL, 0},
    {".cab", "cab", NULL, NULL, 0},
    {".lha", "lha", NULL, NULL, 0},
    {".lzh", "lha", NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0}};

static const fmt_info *fmt_by_name(const char *name)
{
    int i;
    for (i = 0; formats[i].ext; i++)
        if (!strcmp(formats[i].name, name) || !strcmp(formats[i].ext + 1, name))
            return &formats[i];
    return NULL;
}

const char *jfs_archive_format_from_name(const char *filename)
{
    size_t n = strlen(filename);
    int i;
    for (i = 0; formats[i].ext; i++) {
        size_t e = strlen(formats[i].ext), k;
        int match = n > e;
        for (k = 0; match && k < e; k++)
            if (tolower((unsigned char)filename[n - e + k]) != formats[i].ext[k])
                match = 0;
        if (match)
            return formats[i].name;
    }
    return NULL;
}

/* ======================================================================== */
/* libarchive (dlopen)                                                        */
/* ======================================================================== */
#ifdef _WIN32
typedef unsigned short la_mode_t;
typedef unsigned int la_dev_t;
typedef long long la_time_t;
#else
typedef mode_t la_mode_t;
typedef dev_t la_dev_t;
typedef time_t la_time_t;
#endif
typedef int64_t la_int64;
typedef struct la_archive la_archive;
typedef struct la_entry la_entry;

static struct {
    int tried, ok;
    char why[256];
    la_archive *(*read_new)(void);
    int (*read_support_filter_all)(la_archive *);
    int (*read_support_format_all)(la_archive *);
    int (*read_open_filename)(la_archive *, const char *, size_t);
    int (*read_next_header)(la_archive *, la_entry **);
    long (*read_data)(la_archive *, void *, size_t);
    int (*read_free)(la_archive *);
    const char *(*error_string)(la_archive *);
    const char *(*format_name)(la_archive *);
    const char *(*e_pathname)(la_entry *);
    const char *(*e_pathname_utf8)(la_entry *);
    la_mode_t (*e_filetype)(la_entry *);
    la_mode_t (*e_perm)(la_entry *);
    la_int64 (*e_uid)(la_entry *);
    la_int64 (*e_gid)(la_entry *);
    la_int64 (*e_size)(la_entry *);
    la_time_t (*e_mtime)(la_entry *);
    long (*e_mtime_nsec)(la_entry *);
    int (*e_mtime_is_set)(la_entry *);
    la_time_t (*e_atime)(la_entry *);
    long (*e_atime_nsec)(la_entry *);
    int (*e_atime_is_set)(la_entry *);
    const char *(*e_symlink)(la_entry *);
    const char *(*e_symlink_utf8)(la_entry *);
    const char *(*e_hardlink)(la_entry *);
    const char *(*e_hardlink_utf8)(la_entry *);
    la_dev_t (*e_rdev)(la_entry *);
    la_archive *(*write_new)(void);
    int (*write_set_format_by_name)(la_archive *, const char *);
    int (*write_add_filter_by_name)(la_archive *, const char *);
    int (*write_open_filename)(la_archive *, const char *);
    int (*write_header)(la_archive *, la_entry *);
    long (*write_data)(la_archive *, const void *, size_t);
    int (*write_close)(la_archive *);
    int (*write_free)(la_archive *);
    la_entry *(*entry_new)(void);
    void (*entry_free)(la_entry *);
    int (*e_update_pathname_utf8)(la_entry *, const char *);
    int (*e_update_symlink_utf8)(la_entry *, const char *);
    int (*e_update_hardlink_utf8)(la_entry *, const char *);
    void (*e_set_filetype)(la_entry *, unsigned int);
    void (*e_set_perm)(la_entry *, la_mode_t);
    void (*e_set_size)(la_entry *, la_int64);
    void (*e_set_uid)(la_entry *, la_int64);
    void (*e_set_gid)(la_entry *, la_int64);
    void (*e_set_mtime)(la_entry *, la_time_t, long);
    void (*e_set_atime)(la_entry *, la_time_t, long);
    void (*e_set_rdev)(la_entry *, la_dev_t);
    void (*e_set_ino64)(la_entry *, la_int64);
    void (*e_set_nlink)(la_entry *, unsigned int);
} la;

#define LA_OK 0
#define LA_EOF 1
#define LA_WARN (-20)

int jfs_libarchive_available(char *why, size_t whylen)
{
#ifdef _WIN32
    static const char *cands[] = {"archive.dll", "libarchive.dll", "libarchive-13.dll", NULL};
    HMODULE h = NULL;
#else
    static const char *cands[] = {
#if defined(__APPLE__)
        "/usr/lib/libarchive.2.dylib", "libarchive.2.dylib", "/opt/homebrew/opt/libarchive/lib/libarchive.13.dylib",
        "/usr/local/opt/libarchive/lib/libarchive.13.dylib",
#else
        "libarchive.so.13", "libarchive.so.7", "/usr/lib/libarchive.so.7", "/usr/local/lib/libarchive.so.13",
        "libarchive.so",
#endif
        NULL};
    void *h = NULL;
#endif
    const char *env = getenv("JSONFS_LIBARCHIVE");
    int i, missing = 0;
    void *sym;
    if (la.tried) {
        if (!la.ok && why)
            snprintf(why, whylen, "%s", la.why);
        return la.ok;
    }
    la.tried = 1;
#ifdef _WIN32
    if (env && *env)
        h = LoadLibraryA(env);
    for (i = 0; !h && cands[i]; i++)
        h = LoadLibraryA(cands[i]);
#define LA_SYM(n) (sym = (void *)GetProcAddress(h, n))
#else
    if (env && *env)
        h = dlopen(env, RTLD_NOW | RTLD_LOCAL);
    for (i = 0; !h && cands[i]; i++)
        h = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
#define LA_SYM(n) (sym = dlsym(h, n))
#endif
    if (!h) {
        snprintf(la.why, sizeof la.why,
                 "libarchive is not installed (needed for this format; "
#if defined(_WIN32)
                 "put archive.dll next to jsonfs.exe or set JSONFS_LIBARCHIVE"
#elif defined(__APPLE__)
                 "it ships with macOS - set JSONFS_LIBARCHIVE if it was removed"
#elif defined(__FreeBSD__)
                 "it is part of the base system - set JSONFS_LIBARCHIVE"
#else
                 "install libarchive13 / libarchive"
#endif
                 ")");
        if (why)
            snprintf(why, whylen, "%s", la.why);
        return 0;
    }
#define REQ(field, name)                                       \
    do {                                                       \
        if (!LA_SYM(name))                                     \
            missing = 1;                                       \
        memcpy(&la.field, &sym, sizeof sym);                   \
    } while (0)
#define OPT(field, name)                                       \
    do {                                                       \
        LA_SYM(name);                                          \
        memcpy(&la.field, &sym, sizeof sym);                   \
    } while (0)
    REQ(read_new, "archive_read_new");
    REQ(read_support_filter_all, "archive_read_support_filter_all");
    REQ(read_support_format_all, "archive_read_support_format_all");
    REQ(read_open_filename, "archive_read_open_filename");
    REQ(read_next_header, "archive_read_next_header");
    REQ(read_data, "archive_read_data");
    REQ(read_free, "archive_read_free");
    REQ(error_string, "archive_error_string");
    REQ(format_name, "archive_format_name");
    REQ(e_pathname, "archive_entry_pathname");
    OPT(e_pathname_utf8, "archive_entry_pathname_utf8");
    REQ(e_filetype, "archive_entry_filetype");
    REQ(e_perm, "archive_entry_perm");
    REQ(e_uid, "archive_entry_uid");
    REQ(e_gid, "archive_entry_gid");
    REQ(e_size, "archive_entry_size");
    REQ(e_mtime, "archive_entry_mtime");
    REQ(e_mtime_nsec, "archive_entry_mtime_nsec");
    REQ(e_mtime_is_set, "archive_entry_mtime_is_set");
    REQ(e_atime, "archive_entry_atime");
    REQ(e_atime_nsec, "archive_entry_atime_nsec");
    REQ(e_atime_is_set, "archive_entry_atime_is_set");
    REQ(e_symlink, "archive_entry_symlink");
    OPT(e_symlink_utf8, "archive_entry_symlink_utf8");
    REQ(e_hardlink, "archive_entry_hardlink");
    OPT(e_hardlink_utf8, "archive_entry_hardlink_utf8");
    REQ(e_rdev, "archive_entry_rdev");
    REQ(write_new, "archive_write_new");
    REQ(write_set_format_by_name, "archive_write_set_format_by_name");
    REQ(write_add_filter_by_name, "archive_write_add_filter_by_name");
    REQ(write_open_filename, "archive_write_open_filename");
    REQ(write_header, "archive_write_header");
    REQ(write_data, "archive_write_data");
    REQ(write_close, "archive_write_close");
    REQ(write_free, "archive_write_free");
    REQ(entry_new, "archive_entry_new");
    REQ(entry_free, "archive_entry_free");
    REQ(e_update_pathname_utf8, "archive_entry_update_pathname_utf8");
    REQ(e_update_symlink_utf8, "archive_entry_update_symlink_utf8");
    REQ(e_update_hardlink_utf8, "archive_entry_update_hardlink_utf8");
    REQ(e_set_filetype, "archive_entry_set_filetype");
    REQ(e_set_perm, "archive_entry_set_perm");
    REQ(e_set_size, "archive_entry_set_size");
    REQ(e_set_uid, "archive_entry_set_uid");
    REQ(e_set_gid, "archive_entry_set_gid");
    REQ(e_set_mtime, "archive_entry_set_mtime");
    REQ(e_set_atime, "archive_entry_set_atime");
    REQ(e_set_rdev, "archive_entry_set_rdev");
    OPT(e_set_ino64, "archive_entry_set_ino64");
    OPT(e_set_nlink, "archive_entry_set_nlink");
#undef REQ
#undef OPT
#undef LA_SYM
    if (missing) {
        snprintf(la.why, sizeof la.why, "the installed libarchive is too old (version 3.x is required)");
        if (why)
            snprintf(why, whylen, "%s", la.why);
        return 0;
    }
    /* libarchive converts names between UTF-8 and the locale's character
     * set; under the "C" locale non-ASCII names cannot be represented in
     * tar headers (and older versions crash on them), so use a UTF-8 LC_CTYPE */
    {
        const char *cur = setlocale(LC_CTYPE, NULL);
        if (!cur || (!strstr(cur, "UTF-8") && !strstr(cur, "utf8") && !strstr(cur, "UTF8") && !strstr(cur, "utf-8"))) {
            const char *tries[] = {"", "C.UTF-8", "C.utf8", "en_US.UTF-8", "UTF-8", NULL};
            int k;
            for (k = 0; tries[k]; k++) {
                const char *r = setlocale(LC_CTYPE, tries[k]);
                if (r && (strstr(r, "UTF-8") || strstr(r, "utf8") || strstr(r, "UTF8") || strstr(r, "utf-8")))
                    break;
            }
            if (!tries[k])
                setlocale(LC_CTYPE, cur ? cur : "C");
        }
    }
    la.ok = 1;
    return 1;
}

/* ======================================================================== */
/* Import: shared entry handling                                              */
/* ======================================================================== */
typedef struct ar_xattr {
    char *name;
    uint8_t *val;
    size_t len;
} ar_xattr;

typedef struct ar_entry {
    char *path;
    uint32_t type; /* JFS_S_IF* */
    uint32_t perm;
    int has_owner;
    uint32_t uid, gid;
    int has_mtime, has_atime;
    jfs_time mtime, atime;
    uint64_t size;
    char *link;     /* symlink target */
    char *hardlink; /* hard link target (archive path) */
    uint64_t rdev;
    ar_xattr *xa;
    size_t nxa;
} ar_entry;

typedef struct dir_meta {
    char *path;
    ar_entry e; /* only metadata fields used */
} dir_meta;

typedef struct imp {
    jfs_fs *fs;
    const char *dest;
    const jfs_archive_opts *o;
    jfs_archive_stats *st;
    dir_meta *dm;
    size_t ndm, dmcap;
    char *err;
    size_t errlen;
} imp;

static void entry_clear(ar_entry *e)
{
    size_t i;
    free(e->path);
    free(e->link);
    free(e->hardlink);
    for (i = 0; i < e->nxa; i++) {
        free(e->xa[i].name);
        free(e->xa[i].val);
    }
    free(e->xa);
    memset(e, 0, sizeof *e);
}

static void entry_add_xattr(ar_entry *e, const char *name, const uint8_t *val, size_t len)
{
    e->xa = jfs_realloc(e->xa, (e->nxa + 1) * sizeof(ar_xattr));
    e->xa[e->nxa].name = jfs_strdup(name);
    e->xa[e->nxa].val = jfs_malloc(len ? len : 1);
    memcpy(e->xa[e->nxa].val, val, len);
    e->xa[e->nxa].len = len;
    e->nxa++;
}

/* archive path -> absolute image path below dest; NULL = refuse */
static char *norm_path(imp *im, const char *raw, int *is_root)
{
    jfs_buf b;
    const char *p = raw, *s;
    *is_root = 0;
    jfs_buf_init(&b);
    jfs_buf_puts(&b, im->dest);
    while (*p) {
        size_t len;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            jfs_buf_free(&b);
            return NULL;
        }
        if (b.len == 0 || b.data[b.len - 1] != '/')
            jfs_buf_putc(&b, '/');
        jfs_buf_append(&b, s, len);
    }
    if (b.len == strlen(im->dest)) {
        *is_root = 1;
        if (b.len == 0)
            jfs_buf_putc(&b, '/');
    }
    return jfs_buf_steal(&b, NULL);
}

static uint32_t img_type(jfs_fs *fs, const char *path)
{
    jfs_stat st;
    if (jfs_getattr(fs, path, &st))
        return 0;
    return st.mode & JFS_S_IFMT;
}

static int mkdir_parents(imp *im, const char *path)
{
    char *p = jfs_strdup(path), *s;
    int rc = 0;
    jfs_ctx c;
    memset(&c, 0, sizeof c);
    c.uid = im->o->uid;
    c.gid = im->o->gid;
    for (s = p + 1; *s; s++) {
        if (*s != '/')
            continue;
        *s = 0;
        if (img_type(im->fs, p) != JFS_S_IFDIR) {
            if (img_type(im->fs, p))
                jfs_unlink(im->fs, p);
            rc = jfs_mkdir(im->fs, p, 0755, &c);
            if (rc && rc != -JFS_EEXIST)
                break;
            rc = 0;
        }
        *s = '/';
    }
    free(p);
    return rc;
}

static void apply_meta(imp *im, const char *path, const ar_entry *e)
{
    uint32_t uid = (im->o->same_owner && e->has_owner) ? e->uid : im->o->uid;
    uint32_t gid = (im->o->same_owner && e->has_owner) ? e->gid : im->o->gid;
    size_t i;
    if (e->type != JFS_S_IFLNK)
        jfs_chmod(im->fs, path, e->perm & 07777);
    jfs_chown(im->fs, path, uid, gid);
    if (e->type != JFS_S_IFLNK && (e->perm & (JFS_S_ISUID | JFS_S_ISGID)))
        jfs_chmod(im->fs, path, e->perm & 07777); /* chown cleared set-id bits */
    for (i = 0; i < e->nxa; i++) {
        int rc = jfs_setxattr(im->fs, path, e->xa[i].name, e->xa[i].val, e->xa[i].len, 0);
        if (rc)
            warnf(im->o, "%s: extended attribute %s not stored: %s", path, e->xa[i].name, jfs_strerror(rc));
    }
    if (e->has_mtime) {
        jfs_time t[2];
        t[0] = e->has_atime ? e->atime : e->mtime;
        t[1] = e->mtime;
        jfs_utimens(im->fs, path, t);
    }
}

/* Remove whatever is at path unless it is a directory and keep_dir is set. */
static int clear_path(imp *im, const char *path, int keep_dir)
{
    uint32_t t = img_type(im->fs, path);
    if (!t)
        return 0;
    if (t == JFS_S_IFDIR) {
        if (keep_dir)
            return 0;
        return jfs_rmdir(im->fs, path);
    }
    return jfs_unlink(im->fs, path);
}

/* Starts an entry; for regular files returns an open handle in *h. */
static int imp_start(imp *im, ar_entry *e, char **out_path, jfs_handle **h)
{
    int is_root, rc = 0;
    char *path;
    jfs_ctx c;
    *h = NULL;
    *out_path = NULL;
    memset(&c, 0, sizeof c);
    c.uid = im->o->uid;
    c.gid = im->o->gid;
    im->st->entries++;
    path = norm_path(im, e->path, &is_root);
    if (!path) {
        warnf(im->o, "skipping \"%s\": path leaves the destination", e->path);
        im->st->skipped++;
        return AR_SKIP;
    }
    if (is_root && e->type != JFS_S_IFDIR) {
        free(path);
        im->st->skipped++;
        return AR_SKIP;
    }
    if (!is_root && (rc = mkdir_parents(im, path)) != 0)
        goto fail;
    switch (e->type) {
    case JFS_S_IFDIR:
        if (!is_root) {
            if (img_type(im->fs, path) != JFS_S_IFDIR) {
                if ((rc = clear_path(im, path, 0)) != 0)
                    goto fail;
                if ((rc = jfs_mkdir(im->fs, path, 0755, &c)) != 0)
                    goto fail;
            }
            im->st->dirs++;
        }
        if (im->ndm == im->dmcap) {
            im->dmcap = im->dmcap ? im->dmcap * 2 : 64;
            im->dm = jfs_realloc(im->dm, im->dmcap * sizeof(dir_meta));
        }
        im->dm[im->ndm].path = jfs_strdup(path);
        im->dm[im->ndm].e = *e; /* shallow: steal the metadata, see below */
        im->dm[im->ndm].e.path = NULL;
        im->dm[im->ndm].e.link = NULL;
        im->dm[im->ndm].e.hardlink = NULL;
        e->xa = NULL; /* xattrs now owned by the queued entry */
        e->nxa = 0;
        im->ndm++;
        break;
    case JFS_S_IFREG:
        if ((rc = clear_path(im, path, 0)) != 0)
            goto fail;
        if (e->hardlink) {
            int r2;
            char *target = norm_path(im, e->hardlink, &r2);
            if (!target || img_type(im->fs, target) == 0 || img_type(im->fs, target) == JFS_S_IFDIR) {
                warnf(im->o, "skipping hard link %s -> %s: target not found", e->path, e->hardlink);
                free(target);
                im->st->skipped++;
                free(path);
                return AR_SKIP;
            }
            rc = jfs_link(im->fs, target, path);
            free(target);
            if (rc)
                goto fail;
            im->st->hardlinks++;
            free(path);
            return AR_SKIP; /* nothing else to do */
        }
        if ((rc = jfs_create(im->fs, path, 0600, JFS_O_WRONLY | JFS_O_TRUNC, &c, h)) != 0)
            goto fail;
        im->st->files++;
        break;
    case JFS_S_IFLNK:
        if ((rc = clear_path(im, path, 0)) != 0)
            goto fail;
        if ((rc = jfs_symlink(im->fs, e->link ? e->link : "", path, &c)) != 0)
            goto fail;
        im->st->symlinks++;
        break;
    default:
        if ((rc = clear_path(im, path, 0)) != 0)
            goto fail;
        if ((rc = jfs_mknod(im->fs, path, e->type | 0600, e->rdev, &c)) != 0)
            goto fail;
        im->st->specials++;
    }
    *out_path = path;
    return 0;
fail:
    snprintf(im->err, im->errlen, "%s: %s", path, jfs_strerror(rc));
    free(path);
    return rc;
}

static int imp_finish(imp *im, ar_entry *e, char *path, jfs_handle *h)
{
    if (h)
        jfs_release(im->fs, h);
    if (path && e->type != JFS_S_IFDIR)
        apply_meta(im, path, e);
    free(path);
    return 0;
}

static int imp_write(imp *im, jfs_handle *h, const void *data, size_t n, uint64_t off)
{
    int64_t w = jfs_write(im->fs, h, data, n, off);
    if (w < 0) {
        snprintf(im->err, im->errlen, "write: %s", jfs_strerror((int)w));
        return (int)w;
    }
    im->st->bytes += n;
    return 0;
}

/* whole entry with in-memory data */
static int imp_entry(imp *im, ar_entry *e, const uint8_t *data, size_t n)
{
    char *path;
    jfs_handle *h;
    int rc = imp_start(im, e, &path, &h);
    if (rc == AR_SKIP)
        return 0;
    if (rc)
        return rc;
    if (h && n)
        rc = imp_write(im, h, data, n, 0);
    imp_finish(im, e, path, h);
    return rc;
}

static void imp_done(imp *im)
{
    size_t i;
    /* directories last and deepest first: adding children changes mtimes */
    for (i = im->ndm; i-- > 0;) {
        apply_meta(im, im->dm[i].path, &im->dm[i].e);
        free(im->dm[i].path);
        entry_clear(&im->dm[i].e);
    }
    free(im->dm);
    im->dm = NULL;
    im->ndm = 0;
    im->dmcap = 0;
}

/* ======================================================================== */
/* tar reader                                                                 */
/* ======================================================================== */
static uint64_t tar_num(const uint8_t *f, size_t len)
{
    uint64_t v = 0;
    size_t i = 0;
    if (f[0] & 0x80) { /* GNU base-256 */
        v = f[0] & 0x3f;
        for (i = 1; i < len; i++)
            v = (v << 8) | f[i];
        return v;
    }
    while (i < len && (f[i] == ' ' || f[i] == 0))
        i++;
    for (; i < len && f[i] >= '0' && f[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(f[i] - '0');
    return v;
}

static int tar_checksum_ok(const uint8_t *h)
{
    uint64_t want = tar_num(h + 148, 8), us = 0;
    int64_t ss = 0;
    int i;
    for (i = 0; i < 512; i++) {
        uint8_t c = (i >= 148 && i < 156) ? ' ' : h[i];
        us += c;
        ss += (signed char)c;
    }
    return want == us || (int64_t)want == ss;
}

static int is_zero_block(const uint8_t *h)
{
    int i;
    for (i = 0; i < 512; i++)
        if (h[i])
            return 0;
    return 1;
}

static int looks_like_tar(const uint8_t *b, size_t n)
{
    return n >= 512 && !is_zero_block(b) && tar_checksum_ok(b);
}

typedef struct pax_kv {
    char *key;
    uint8_t *val;
    size_t len;
} pax_kv;

typedef struct pax_set {
    pax_kv *kv;
    size_t n;
} pax_set;

static void pax_free(pax_set *p)
{
    size_t i;
    for (i = 0; i < p->n; i++) {
        free(p->kv[i].key);
        free(p->kv[i].val);
    }
    free(p->kv);
    memset(p, 0, sizeof *p);
}

static void pax_put(pax_set *p, const char *key, size_t klen, const uint8_t *val, size_t vlen)
{
    size_t i;
    for (i = 0; i < p->n; i++) {
        if (strlen(p->kv[i].key) == klen && !memcmp(p->kv[i].key, key, klen)) {
            free(p->kv[i].val);
            goto set;
        }
    }
    p->kv = jfs_realloc(p->kv, (p->n + 1) * sizeof(pax_kv));
    i = p->n++;
    p->kv[i].key = jfs_strndup(key, klen);
set:
    p->kv[i].val = jfs_malloc(vlen + 1);
    memcpy(p->kv[i].val, val, vlen);
    p->kv[i].val[vlen] = 0;
    p->kv[i].len = vlen;
}

static int pax_parse(pax_set *p, const uint8_t *d, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t len = 0, i = off, eq;
        while (i < n && d[i] >= '0' && d[i] <= '9')
            len = len * 10 + (size_t)(d[i++] - '0');
        if (i >= n || d[i] != ' ' || len == 0 || off + len > n || d[off + len - 1] != '\n') {
            if (d[off] == 0)
                break; /* padding */
            return -1;
        }
        i++;
        eq = i;
        while (eq < off + len && d[eq] != '=')
            eq++;
        if (eq >= off + len)
            return -1;
        pax_put(p, (const char *)d + i, eq - i, d + eq + 1, off + len - 1 - (eq + 1));
        off += len;
    }
    return 0;
}

static const pax_kv *pax_get(const pax_set *a, const pax_set *b, const char *key)
{
    size_t i;
    for (i = 0; a && i < a->n; i++)
        if (!strcmp(a->kv[i].key, key))
            return &a->kv[i];
    for (i = 0; b && i < b->n; i++)
        if (!strcmp(b->kv[i].key, key))
            return &b->kv[i];
    return NULL;
}

static int pax_time(const pax_kv *kv, jfs_time *t)
{
    char *end;
    const char *s = (const char *)kv->val;
    long long sec = strtoll(s, &end, 10);
    long ns = 0;
    int digits = 0;
    if (end == s)
        return -1;
    if (*end == '.') {
        end++;
        while (isdigit((unsigned char)*end) && digits < 9) {
            ns = ns * 10 + (*end - '0');
            end++;
            digits++;
        }
        while (digits++ < 9)
            ns *= 10;
        if (sec < 0 || s[0] == '-')
            ns = -ns;
    }
    t->sec = sec;
    t->nsec = (int32_t)ns;
    if (t->nsec < 0) {
        t->nsec += 1000000000;
        t->sec--;
    }
    return 0;
}

static uint64_t make_rdev(uint64_t major, uint64_t minor)
{
    /* glibc / Linux encoding (what the toolkit stores from st_rdev on Linux) */
    return ((major & 0xfffULL) << 8) | ((major & ~0xfffULL) << 32) | (minor & 0xffULL) |
           ((minor & ~0xffULL) << 12);
}

static void split_rdev(uint64_t rdev, uint64_t *major, uint64_t *minor)
{
    *major = ((rdev >> 8) & 0xfff) | ((rdev >> 32) & ~0xfffULL);
    *minor = (rdev & 0xff) | ((rdev >> 12) & ~0xffULL);
}

static char *field_str(const uint8_t *f, size_t len)
{
    size_t n = 0;
    while (n < len && f[n])
        n++;
    return jfs_strndup((const char *)f, n);
}

static int tar_read(imp *im, const uint8_t *b, size_t n)
{
    size_t off = 0;
    pax_set global, local;
    char *longname = NULL, *longlink = NULL;
    int rc = 0, zeros = 0;
    memset(&global, 0, sizeof global);
    memset(&local, 0, sizeof local);
    while (off + 512 <= n) {
        const uint8_t *h = b + off;
        uint64_t size;
        size_t data;
        char type;
        ar_entry e;
        const pax_kv *kv;
        if (is_zero_block(h)) {
            if (++zeros == 2)
                break;
            off += 512;
            continue;
        }
        zeros = 0;
        if (!tar_checksum_ok(h)) {
            snprintf(im->err, im->errlen, "tar: bad header checksum at offset %lu", (unsigned long)off);
            rc = -JFS_ECORRUPT;
            break;
        }
        type = (char)h[156];
        size = tar_num(h + 124, 12);
        if ((kv = pax_get(&local, NULL, "size")) != NULL)
            size = strtoull((const char *)kv->val, NULL, 10);
        data = off + 512;
        if (type == '1' || type == '2' || type == '3' || type == '4' || type == '5' || type == '6')
            size = 0; /* no data for these, whatever the header says */
        if (data + size > n || data + size < data) {
            snprintf(im->err, im->errlen, "tar: archive is truncated");
            rc = -JFS_ECORRUPT;
            break;
        }
        off = data + (size_t)((size + 511) & ~(uint64_t)511);
        switch (type) {
        case 'x':
            if (pax_parse(&local, b + data, (size_t)size))
                warnf(im->o, "tar: malformed pax header ignored");
            continue;
        case 'g':
            if (pax_parse(&global, b + data, (size_t)size))
                warnf(im->o, "tar: malformed global pax header ignored");
            continue;
        case 'L':
            free(longname);
            longname = field_str(b + data, (size_t)size);
            continue;
        case 'K':
            free(longlink);
            longlink = field_str(b + data, (size_t)size);
            continue;
        case 'V':
        case 'M':
            continue;
        default: break;
        }
        memset(&e, 0, sizeof e);
        if ((kv = pax_get(&local, &global, "path")) != NULL)
            e.path = jfs_strdup((const char *)kv->val);
        else if (longname)
            e.path = jfs_strdup(longname);
        else if (!memcmp(h + 257, "ustar\0", 6) && h[345]) {
            char *pre = field_str(h + 345, 155), *nm = field_str(h, 100);
            e.path = jfs_asprintf("%s/%s", pre, nm);
            free(pre);
            free(nm);
        } else {
            e.path = field_str(h, 100);
        }
        if ((kv = pax_get(&local, &global, "linkpath")) != NULL)
            e.link = jfs_strdup((const char *)kv->val);
        else if (longlink)
            e.link = jfs_strdup(longlink);
        else
            e.link = field_str(h + 157, 100);
        e.perm = (uint32_t)tar_num(h + 100, 8) & 07777;
        e.has_owner = 1;
        e.uid = (uint32_t)((kv = pax_get(&local, &global, "uid")) ? strtoull((const char *)kv->val, NULL, 10)
                                                                    : tar_num(h + 108, 8));
        e.gid = (uint32_t)((kv = pax_get(&local, &global, "gid")) ? strtoull((const char *)kv->val, NULL, 10)
                                                                    : tar_num(h + 116, 8));
        e.has_mtime = 1;
        if (!((kv = pax_get(&local, &global, "mtime")) && pax_time(kv, &e.mtime) == 0)) {
            e.mtime.sec = (int64_t)tar_num(h + 136, 12);
            e.mtime.nsec = 0;
        }
        if ((kv = pax_get(&local, &global, "atime")) && pax_time(kv, &e.atime) == 0)
            e.has_atime = 1;
        e.size = size;
        {
            size_t i;
            for (i = 0; i < local.n; i++)
                if (!strncmp(local.kv[i].key, "SCHILY.xattr.", 13))
                    entry_add_xattr(&e, local.kv[i].key + 13, local.kv[i].val, local.kv[i].len);
        }
        switch (type) {
        case '0':
        case '\0':
        case '7':
            e.type = JFS_S_IFREG;
            if (type == '\0' && e.path[0] && e.path[strlen(e.path) - 1] == '/')
                e.type = JFS_S_IFDIR; /* v7 directory */
            break;
        case '1':
            e.type = JFS_S_IFREG;
            e.hardlink = e.link;
            e.link = NULL;
            break;
        case '2': e.type = JFS_S_IFLNK; break;
        case '3': e.type = JFS_S_IFCHR; break;
        case '4': e.type = JFS_S_IFBLK; break;
        case '5':
        case 'D': e.type = JFS_S_IFDIR; break;
        case '6': e.type = JFS_S_IFIFO; break;
        default:
            warnf(im->o, "tar: skipping %s (unsupported entry type '%c')", e.path, type);
            im->st->skipped++;
            entry_clear(&e);
            goto next;
        }
        if (e.type == JFS_S_IFCHR || e.type == JFS_S_IFBLK)
            e.rdev = make_rdev(tar_num(h + 329, 8), tar_num(h + 337, 8));
        if (e.type != JFS_S_IFREG)
            size = 0;
        rc = imp_entry(im, &e, b + data, (size_t)size);
        entry_clear(&e);
        if (rc)
            break;
    next:
        pax_free(&local);
        free(longname);
        free(longlink);
        longname = longlink = NULL;
    }
    pax_free(&local);
    pax_free(&global);
    free(longname);
    free(longlink);
    return rc;
}

/* ======================================================================== */
/* zip reader                                                                 */
/* ======================================================================== */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int looks_like_zip(const uint8_t *b, size_t n)
{
    return n >= 22 && b[0] == 'P' && b[1] == 'K' && ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6));
}

/* code page 437 (the zip default without the UTF-8 flag) -> UTF-8 */
static const uint16_t cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

static int valid_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t k, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n)
            return 0;
        for (k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
        i += len;
    }
    return 1;
}

static char *zip_name(const uint8_t *s, size_t n, int utf8)
{
    jfs_buf b;
    size_t i;
    jfs_buf_init(&b);
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '\\' && !utf8)
            c = '/'; /* some Windows tools */
        if (c < 0x80 || utf8) {
            jfs_buf_putc(&b, (char)c);
        } else {
            unsigned cp = cp437[c - 0x80];
            if (cp < 0x800) {
                jfs_buf_putc(&b, (char)(0xC0 | (cp >> 6)));
                jfs_buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
            } else {
                jfs_buf_putc(&b, (char)(0xE0 | (cp >> 12)));
                jfs_buf_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                jfs_buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
            }
        }
    }
    return jfs_buf_steal(&b, NULL);
}

static jfs_time dos_time(uint16_t t, uint16_t d)
{
    struct tm tm;
    jfs_time r;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = ((d >> 9) & 0x7f) + 80;
    tm.tm_mon = ((d >> 5) & 0x0f) - 1;
    tm.tm_mday = d & 0x1f;
    tm.tm_hour = (t >> 11) & 0x1f;
    tm.tm_min = (t >> 5) & 0x3f;
    tm.tm_sec = (t & 0x1f) * 2;
    tm.tm_isdst = -1;
    r.sec = (int64_t)mktime(&tm); /* DOS times are local time */
    r.nsec = 0;
    return r;
}

static int zip_read(imp *im, const uint8_t *b, size_t n, int *need_libarchive)
{
    size_t eocd = 0, i, cd, pos;
    uint64_t entries, cdsize, cdoff;
    int found = 0, rc = 0;
    *need_libarchive = 0;
    for (i = n >= 22 ? n - 22 : 0;; i--) {
        if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 5 && b[i + 3] == 6) {
            eocd = i;
            found = 1;
            break;
        }
        if (i == 0 || n - i > 65535 + 22)
            break;
    }
    if (!found) {
        snprintf(im->err, im->errlen, "zip: end of central directory not found");
        return -JFS_ECORRUPT;
    }
    entries = rd16(b + eocd + 10);
    cdsize = rd32(b + eocd + 12);
    cdoff = rd32(b + eocd + 16);
    if (eocd >= 20 && rd32(b + eocd - 20) == 0x07064b50) { /* zip64 locator */
        uint64_t z = rd64(b + eocd - 20 + 8);
        if (z + 56 <= n && rd32(b + z) == 0x06064b50) {
            entries = rd64(b + z + 32);
            cdsize = rd64(b + z + 40);
            cdoff = rd64(b + z + 48);
        }
    }
    if (cdoff + cdsize > n) {
        snprintf(im->err, im->errlen, "zip: central directory out of range");
        return -JFS_ECORRUPT;
    }
    cd = (size_t)cdoff;
    for (pos = cd; entries-- > 0;) {
        uint16_t made, flags, method, mt, md, nl, xl, cl;
        uint32_t crc, ext;
        uint64_t csize, usize, loff;
        const uint8_t *x;
        size_t xi, data;
        ar_entry e;
        int unix_mode = 0;
        if (pos + 46 > n || rd32(b + pos) != 0x02014b50) {
            snprintf(im->err, im->errlen, "zip: corrupt central directory");
            rc = -JFS_ECORRUPT;
            break;
        }
        made = rd16(b + pos + 4);
        flags = rd16(b + pos + 8);
        method = rd16(b + pos + 10);
        mt = rd16(b + pos + 12);
        md = rd16(b + pos + 14);
        crc = rd32(b + pos + 16);
        csize = rd32(b + pos + 20);
        usize = rd32(b + pos + 24);
        nl = rd16(b + pos + 28);
        xl = rd16(b + pos + 30);
        cl = rd16(b + pos + 32);
        ext = rd32(b + pos + 38);
        loff = rd32(b + pos + 42);
        if (pos + 46 + nl + xl + cl > n) {
            rc = -JFS_ECORRUPT;
            snprintf(im->err, im->errlen, "zip: corrupt central directory");
            break;
        }
        memset(&e, 0, sizeof e);
        {
            /* UTF-8 flag; else Info-ZIP's Unicode Path extra field; else raw
             * UTF-8 from Unix hosts; else the historical code page 437 */
            const uint8_t *raw = b + pos + 46, *xx = raw + nl;
            size_t k;
            int host = made >> 8;
            for (k = 0; k + 4 <= xl; k += 4 + (size_t)rd16(xx + k + 2)) {
                uint16_t id = rd16(xx + k), sz = rd16(xx + k + 2);
                if (k + 4 + sz > xl)
                    break;
                if (id == 0x7075 && sz >= 5 && xx[k + 4] == 1 && rd32(xx + k + 5) == jfs_crc32(0, raw, nl)) {
                    e.path = jfs_strndup((const char *)xx + k + 9, sz - 5);
                    break;
                }
            }
            if (!e.path)
                e.path = zip_name(raw, nl,
                                  (flags & 0x800) || ((host == 3 || host == 19 || valid_utf8(raw, nl)) && valid_utf8(raw, nl)));
        }
        e.has_mtime = 1;
        e.mtime = dos_time(mt, md);
        x = b + pos + 46 + nl;
        for (xi = 0; xi + 4 <= xl;) {
            uint16_t id = rd16(x + xi), sz = rd16(x + xi + 2);
            const uint8_t *d = x + xi + 4;
            if (xi + 4 + sz > xl)
                break;
            if (id == 0x0001) { /* zip64 */
                size_t k = 0;
                if (usize == 0xFFFFFFFF && k + 8 <= sz) {
                    usize = rd64(d + k);
                    k += 8;
                }
                if (csize == 0xFFFFFFFF && k + 8 <= sz) {
                    csize = rd64(d + k);
                    k += 8;
                }
                if (loff == 0xFFFFFFFF && k + 8 <= sz)
                    loff = rd64(d + k);
            } else if (id == 0x5455 && sz >= 5 && (d[0] & 1)) { /* extended timestamp */
                e.mtime.sec = (int32_t)rd32(d + 1);
                e.mtime.nsec = 0;
            } else if (id == 0x7875 && sz >= 3) { /* Info-ZIP Unix uid/gid */
                size_t us = d[1];
                if (2 + us + 1 <= sz) {
                    size_t gs = d[2 + us];
                    uint64_t u = 0, g = 0;
                    size_t k;
                    for (k = 0; k < us && k < 8; k++)
                        u |= (uint64_t)d[2 + k] << (8 * k);
                    for (k = 0; k < gs && k < 8 && 3 + us + k < sz; k++)
                        g |= (uint64_t)d[3 + us + k] << (8 * k);
                    e.has_owner = 1;
                    e.uid = (uint32_t)u;
                    e.gid = (uint32_t)g;
                }
            }
            xi += 4 + (size_t)sz;
        }
        pos += 46 + (size_t)nl + xl + cl;
        if ((made >> 8) == 3 && (ext >> 16)) {
            uint32_t m = ext >> 16;
            unix_mode = 1;
            e.type = m & JFS_S_IFMT;
            e.perm = m & 07777;
            if (!e.type)
                e.type = JFS_S_IFREG;
        }
        if (!unix_mode) {
            int dir = (e.path[0] && e.path[strlen(e.path) - 1] == '/') || (ext & 0x10);
            e.type = dir ? JFS_S_IFDIR : JFS_S_IFREG;
            e.perm = dir ? 0755 : ((ext & 1) ? 0444 : 0644);
        }
        if (e.path[0] && e.path[strlen(e.path) - 1] == '/' && e.type == JFS_S_IFREG)
            e.type = JFS_S_IFDIR;
        if (e.type == JFS_S_IFDIR) {
            rc = imp_entry(im, &e, NULL, 0);
            entry_clear(&e);
            if (rc)
                break;
            continue;
        }
        if (e.type != JFS_S_IFREG && e.type != JFS_S_IFLNK) {
            warnf(im->o, "zip: skipping %s (special file)", e.path);
            im->st->skipped++;
            entry_clear(&e);
            continue;
        }
        if (flags & 1) {
            warnf(im->o, "zip: skipping %s (encrypted)", e.path);
            im->st->skipped++;
            entry_clear(&e);
            continue;
        }
        if (loff + 30 > n || rd32(b + loff) != 0x04034b50) {
            snprintf(im->err, im->errlen, "zip: bad local header for %s", e.path);
            entry_clear(&e);
            rc = -JFS_ECORRUPT;
            break;
        }
        data = (size_t)loff + 30 + rd16(b + loff + 26) + rd16(b + loff + 28);
        if (data + csize > n || data + csize < data) {
            snprintf(im->err, im->errlen, "zip: %s is truncated", e.path);
            entry_clear(&e);
            rc = -JFS_ECORRUPT;
            break;
        }
        {
            uint8_t *content = NULL;
            size_t clen = 0;
            if (method == 0) {
                content = jfs_malloc((size_t)csize + 1);
                memcpy(content, b + data, (size_t)csize);
                clen = (size_t)csize;
            } else if (method == 8) {
                if (jfs_inflate_alloc(b + data, (size_t)csize, &content, &clen, NULL)) {
                    snprintf(im->err, im->errlen, "zip: %s: corrupt DEFLATE data", e.path);
                    entry_clear(&e);
                    rc = -JFS_ECORRUPT;
                    break;
                }
            } else {
                entry_clear(&e);
                *need_libarchive = 1;
                snprintf(im->err, im->errlen, "zip: compression method %u is not built in", method);
                rc = -JFS_ENOTSUP;
                break;
            }
            if (clen != usize || jfs_crc32(0, content, clen) != crc) {
                snprintf(im->err, im->errlen, "zip: %s: CRC or size mismatch", e.path);
                free(content);
                entry_clear(&e);
                rc = -JFS_ECHECKSUM;
                break;
            }
            if (e.type == JFS_S_IFLNK) {
                e.link = jfs_strndup((const char *)content, clen);
                rc = imp_entry(im, &e, NULL, 0);
            } else {
                rc = imp_entry(im, &e, content, clen);
            }
            free(content);
        }
        entry_clear(&e);
        if (rc)
            break;
    }
    return rc;
}

/* ======================================================================== */
/* libarchive reader                                                          */
/* ======================================================================== */
static int la_read(imp *im, const char *archive)
{
    la_archive *a = la.read_new();
    la_entry *le;
    int rc = 0, r;
    char buf[65536];
    la.read_support_filter_all(a);
    la.read_support_format_all(a);
    if (la.read_open_filename(a, archive, 65536) != LA_OK) {
        snprintf(im->err, im->errlen, "%s", la.error_string(a) ? la.error_string(a) : "cannot open archive");
        la.read_free(a);
        return -JFS_EINVAL;
    }
    while ((r = la.read_next_header(a, &le)) == LA_OK || r == LA_WARN) {
        ar_entry e;
        const char *p = la.e_pathname_utf8 ? la.e_pathname_utf8(le) : NULL;
        const char *hl = la.e_hardlink_utf8 ? la.e_hardlink_utf8(le) : la.e_hardlink(le);
        const char *sl = la.e_symlink_utf8 ? la.e_symlink_utf8(le) : la.e_symlink(le);
        char *path;
        jfs_handle *h;
        uint64_t off = 0;
        if (!p)
            p = la.e_pathname(le);
        if (!p) {
            im->st->skipped++;
            continue;
        }
        memset(&e, 0, sizeof e);
        e.path = jfs_strdup(p);
        e.type = (uint32_t)la.e_filetype(le) & JFS_S_IFMT;
        e.perm = (uint32_t)la.e_perm(le) & 07777;
        e.has_owner = 1;
        e.uid = (uint32_t)la.e_uid(le);
        e.gid = (uint32_t)la.e_gid(le);
        if (la.e_mtime_is_set(le)) {
            e.has_mtime = 1;
            e.mtime.sec = (int64_t)la.e_mtime(le);
            e.mtime.nsec = (int32_t)la.e_mtime_nsec(le);
        }
        if (la.e_atime_is_set(le)) {
            e.has_atime = 1;
            e.atime.sec = (int64_t)la.e_atime(le);
            e.atime.nsec = (int32_t)la.e_atime_nsec(le);
        }
        if (hl && *hl) {
            e.type = JFS_S_IFREG;
            e.hardlink = jfs_strdup(hl);
        }
        if (e.type == JFS_S_IFLNK)
            e.link = jfs_strdup(sl ? sl : "");
        if (e.type == JFS_S_IFCHR || e.type == JFS_S_IFBLK)
            e.rdev = (uint64_t)la.e_rdev(le);
        if (!e.type)
            e.type = JFS_S_IFREG;
        rc = imp_start(im, &e, &path, &h);
        if (rc == AR_SKIP && e.hardlink && la.e_size(le) > 0) {
            /* cpio keeps the data with the last name of a hard-linked file:
             * the link was made, now fill in the shared content */
            int r2;
            char *p2 = norm_path(im, e.path, &r2);
            jfs_ctx c;
            memset(&c, 0, sizeof c);
            rc = p2 ? jfs_open_file(im->fs, p2, JFS_O_WRONLY | JFS_O_TRUNC, &h) : -JFS_ENOENT;
            if (!rc) {
                long got;
                while ((got = la.read_data(a, buf, sizeof buf)) > 0 && !rc) {
                    rc = imp_write(im, h, buf, (size_t)got, off);
                    off += (uint64_t)got;
                }
                jfs_release(im->fs, h);
                e.type = JFS_S_IFREG;
                apply_meta(im, p2, &e);
            }
            free(p2);
            entry_clear(&e);
            if (rc)
                break;
            continue;
        }
        if (rc == AR_SKIP) {
            rc = 0;
            entry_clear(&e);
            continue;
        }
        if (rc) {
            entry_clear(&e);
            break;
        }
        if (h) {
            long got;
            while ((got = la.read_data(a, buf, sizeof buf)) > 0) {
                rc = imp_write(im, h, buf, (size_t)got, off);
                if (rc)
                    break;
                off += (uint64_t)got;
            }
            if (got < 0 && !rc) {
                snprintf(im->err, im->errlen, "%s: %s", e.path, la.error_string(a) ? la.error_string(a) : "read error");
                rc = -JFS_EIO;
            }
        }
        imp_finish(im, &e, path, h);
        entry_clear(&e);
        if (rc)
            break;
    }
    if (!rc && r != LA_EOF && r != LA_OK) {
        snprintf(im->err, im->errlen, "%s", la.error_string(a) ? la.error_string(a) : "archive error");
        rc = -JFS_ECORRUPT;
    }
    if (!rc)
        snprintf(im->st->format, sizeof im->st->format, "%s", la.format_name(a) ? la.format_name(a) : "?");
    la.read_free(a);
    return rc;
}

/* ======================================================================== */
/* Import entry point                                                         */
/* ======================================================================== */
int jfs_archive_import(jfs_fs *fs, const char *archive, const char *dest_dir, const jfs_archive_opts *o,
                       jfs_archive_stats *st, char *err, size_t errlen)
{
    imp im;
    jfs_archive_opts dflt;
    jfs_archive_stats dst;
    char *data = NULL, *dest;
    size_t len = 0;
    const fmt_info *fi = NULL;
    int rc, native = 0, need_la = 0;
    char dummy[8];
    if (!o) {
        jfs_archive_opts_init(&dflt);
        o = &dflt;
    }
    if (!st)
        st = &dst;
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    memset(st, 0, sizeof *st);
    err[0] = 0;
    if (o->format) {
        fi = fmt_by_name(o->format);
        if (!fi) {
            snprintf(err, errlen, "unknown archive format \"%s\"", o->format);
            return -JFS_EINVAL;
        }
    }
    dest = jfs_strdup(dest_dir && *dest_dir ? dest_dir : "/");
    {
        size_t n = strlen(dest);
        while (n > 1 && dest[n - 1] == '/')
            dest[--n] = 0;
        if (!strcmp(dest, "/"))
            dest[0] = 0; /* paths are built as dest + "/" + name */
    }
    memset(&im, 0, sizeof im);
    im.fs = fs;
    im.dest = dest;
    im.o = o;
    im.st = st;
    im.err = err;
    im.errlen = errlen;
    if (*dest && img_type(fs, dest) != JFS_S_IFDIR) {
        char *probe = jfs_asprintf("%s/x", dest); /* creates dest and its parents */
        mkdir_parents(&im, probe);
        free(probe);
        if (img_type(fs, dest) != JFS_S_IFDIR) {
            snprintf(err, errlen, "%s is not a directory in the image", dest);
            free(dest);
            return -JFS_ENOTDIR;
        }
    }
    if (o->backend != JFS_AR_BACKEND_LIBARCHIVE) {
        rc = jfs_read_file(archive, &data, &len);
        if (rc) {
            snprintf(err, errlen, "cannot read %s: %s", archive, jfs_strerror(rc));
            free(dest);
            return rc;
        }
        if (looks_like_zip((uint8_t *)data, len) && (!fi || fi->native == 3))
            native = 3;
        else if (len >= 2 && (uint8_t)data[0] == 0x1f && (uint8_t)data[1] == 0x8b && (!fi || fi->native == 2))
            native = 2;
        else if (looks_like_tar((uint8_t *)data, len) && (!fi || fi->native == 1))
            native = 1;
        else if (len == 0 || (len >= 1024 && is_zero_block((uint8_t *)data) && (!fi || fi->native == 1)))
            native = 1; /* empty tar */
    }
    if (native == 2) {
        uint8_t *plain;
        size_t plen;
        rc = jfs_gzip_decode((uint8_t *)data, len, &plain, &plen);
        if (rc) {
            snprintf(err, errlen, "%s: corrupt gzip data", archive);
            goto out;
        }
        if (!looks_like_tar(plain, plen) && !(plen >= 512 && is_zero_block(plain))) {
            free(plain);
            native = 0; /* a gzip file that is not a tarball: let libarchive try */
        } else {
            snprintf(st->format, sizeof st->format, "tar.gz");
            snprintf(st->backend, sizeof st->backend, "native");
            rc = tar_read(&im, plain, plen);
            free(plain);
            goto out;
        }
    }
    if (native == 1) {
        snprintf(st->format, sizeof st->format, "tar");
        snprintf(st->backend, sizeof st->backend, "native");
        rc = tar_read(&im, (uint8_t *)data, len);
        goto out;
    }
    if (native == 3) {
        snprintf(st->format, sizeof st->format, "zip");
        snprintf(st->backend, sizeof st->backend, "native");
        rc = zip_read(&im, (uint8_t *)data, len, &need_la);
        if (!(rc && need_la))
            goto out;
        /* zip with bzip2/LZMA/zstd members: start over with libarchive */
        if (o->backend == JFS_AR_BACKEND_NATIVE || !jfs_libarchive_available(NULL, 0)) {
            size_t l = strlen(err);
            snprintf(err + l, errlen - l, " (libarchive is needed for this archive)");
            goto out;
        }
        imp_done(&im);
        err[0] = 0;
        memset(st, 0, sizeof *st);
    }
    if (o->backend == JFS_AR_BACKEND_NATIVE) {
        snprintf(err, errlen, "%s: not a tar, tar.gz or zip archive (the built-in formats)", archive);
        rc = -JFS_ENOTSUP;
        goto out;
    }
    if (!jfs_libarchive_available(err, errlen)) {
        size_t l = strlen(err);
        snprintf(err + l, errlen - l, "; built-in formats are tar, tar.gz and zip");
        rc = -JFS_ENOTSUP;
        goto out;
    }
    snprintf(st->backend, sizeof st->backend, "libarchive");
    rc = la_read(&im, archive);
out:
    imp_done(&im);
    free(data);
    free(dest);
    return rc;
}

/* ======================================================================== */
/* Export: walking the image                                                  */
/* ======================================================================== */
typedef struct exp_ctx exp_ctx;
typedef int (*emit_fn)(exp_ctx *x, const char *rel, const char *img_path, const jfs_stat *st, const char *link,
                       const char *hardlink_rel);

struct exp_ctx {
    jfs_fs *fs;
    const jfs_archive_opts *o;
    jfs_archive_stats *st;
    emit_fn emit;
    int hardlinks; /* format supports hard links */
    jfs_u64map seen; /* ino -> first rel path (char*) for nlink > 1 */
    char *err;
    size_t errlen;
    /* backends */
    jfs_buf out;
    jfs_buf cdir;
    uint64_t nentries;
    int zip64_needed;
    la_archive *la;
};

typedef struct name_list {
    char **v;
    jfs_stat *st;
    size_t n, cap;
} name_list;

static int collect(void *cookie, const char *name, const jfs_stat *st)
{
    name_list *l = cookie;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = jfs_realloc(l->v, l->cap * sizeof(char *));
        l->st = jfs_realloc(l->st, l->cap * sizeof(jfs_stat));
    }
    l->v[l->n] = jfs_strdup(name);
    l->st[l->n++] = *st;
    return 0;
}

static int read_all(jfs_fs *fs, const char *path, uint64_t size, uint8_t **out)
{
    jfs_handle *h;
    uint8_t *buf;
    uint64_t off = 0;
    int rc = jfs_open_file(fs, path, JFS_O_RDONLY, &h);
    if (rc)
        return rc;
    buf = jfs_malloc((size_t)size + 1);
    while (off < size) {
        int64_t n = jfs_read(fs, h, buf + off, (size_t)(size - off), off);
        if (n <= 0) {
            rc = n < 0 ? (int)n : -JFS_EIO;
            break;
        }
        off += (uint64_t)n;
    }
    jfs_release(fs, h);
    if (rc) {
        free(buf);
        return rc;
    }
    *out = buf;
    return 0;
}

static int walk(exp_ctx *x, const char *img_dir, const char *rel_dir)
{
    name_list l;
    size_t i;
    int rc;
    memset(&l, 0, sizeof l);
    rc = jfs_readdir(x->fs, img_dir, collect, &l);
    for (i = 0; !rc && i < l.n; i++) {
        char *ip = strcmp(img_dir, "/") ? jfs_asprintf("%s/%s", img_dir, l.v[i]) : jfs_asprintf("/%s", l.v[i]);
        char *rp = *rel_dir ? jfs_asprintf("%s/%s", rel_dir, l.v[i]) : jfs_strdup(l.v[i]);
        jfs_stat *s = &l.st[i];
        uint32_t t = s->mode & JFS_S_IFMT;
        char link[4096];
        const char *hard = NULL;
        link[0] = 0;
        if (t == JFS_S_IFLNK)
            jfs_readlink(x->fs, ip, link, sizeof link);
        if (t != JFS_S_IFDIR && s->nlink > 1) {
            hard = jfs_u64map_get(&x->seen, s->ino);
            if (!hard)
                jfs_u64map_put(&x->seen, s->ino, jfs_strdup(rp));
            else if (!x->hardlinks)
                hard = NULL;
        }
        x->st->entries++;
        rc = x->emit(x, rp, ip, s, t == JFS_S_IFLNK ? link : NULL, hard);
        if (!rc && t == JFS_S_IFDIR)
            rc = walk(x, ip, rp);
        free(ip);
        free(rp);
    }
    for (i = 0; i < l.n; i++)
        free(l.v[i]);
    free(l.v);
    free(l.st);
    return rc;
}

static void count_type(exp_ctx *x, uint32_t t, const char *hard, uint64_t size)
{
    if (hard)
        x->st->hardlinks++;
    else if (t == JFS_S_IFDIR)
        x->st->dirs++;
    else if (t == JFS_S_IFREG) {
        x->st->files++;
        x->st->bytes += size;
    } else if (t == JFS_S_IFLNK)
        x->st->symlinks++;
    else
        x->st->specials++;
}

/* ---------------------------------------------------------------- tar ---- */
static void pax_rec(jfs_buf *b, const char *key, const void *val, size_t vlen)
{
    size_t base = strlen(key) + vlen + 3, len = base, digits;
    char num[32];
    for (;;) {
        digits = (size_t)snprintf(num, sizeof num, "%lu", (unsigned long)len);
        if (base + digits == len)
            break;
        len = base + digits;
    }
    jfs_buf_printf(b, "%s %s=", num, key);
    jfs_buf_append(b, val, vlen);
    jfs_buf_putc(b, '\n');
}

static void oct(uint8_t *f, size_t width, uint64_t v)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%0*llo", (int)(width - 1), (unsigned long long)v);
    memcpy(f, tmp, width - 1);
    f[width - 1] = 0;
}

static void tar_finish_header(uint8_t *h)
{
    unsigned sum = 0;
    int i;
    memset(h + 148, ' ', 8);
    for (i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

static void tar_block(jfs_buf *b, const uint8_t *data, uint64_t len)
{
    static const uint8_t zero[512];
    jfs_buf_append(b, data, (size_t)len);
    if (len % 512)
        jfs_buf_append(b, zero, 512 - (size_t)(len % 512));
}

static void tar_raw_header(jfs_buf *b, const char *name, char type, uint32_t perm, uint32_t uid, uint32_t gid,
                           uint64_t size, int64_t mtime, const char *link, const char *prefix, uint64_t rdev)
{
    uint8_t h[512];
    memset(h, 0, sizeof h);
    memcpy(h, name, strlen(name) > 100 ? 100 : strlen(name));
    oct(h + 100, 8, perm & 07777);
    oct(h + 108, 8, uid <= 07777777 ? uid : 0);
    oct(h + 116, 8, gid <= 07777777 ? gid : 0);
    if (size <= 077777777777ULL) {
        oct(h + 124, 12, size);
    } else { /* GNU base-256 (pax "size" carries the value as well) */
        int i;
        h[124] = 0x80;
        for (i = 0; i < 8; i++)
            h[135 - i] = (uint8_t)(size >> (8 * i));
    }
    oct(h + 136, 12, mtime >= 0 && (uint64_t)mtime <= 077777777777ULL ? (uint64_t)mtime : 0);
    h[156] = (uint8_t)type;
    if (link)
        memcpy(h + 157, link, strlen(link) > 100 ? 100 : strlen(link));
    memcpy(h + 257, "ustar\0" "00", 8);
    if (type == '3' || type == '4') {
        uint64_t ma, mi;
        split_rdev(rdev, &ma, &mi);
        oct(h + 329, 8, ma);
        oct(h + 337, 8, mi);
    }
    if (prefix)
        memcpy(h + 345, prefix, strlen(prefix) > 155 ? 155 : strlen(prefix));
    tar_finish_header(h);
    jfs_buf_append(b, h, 512);
}

static int tar_emit(exp_ctx *x, const char *rel, const char *ip, const jfs_stat *s, const char *link,
                    const char *hard)
{
    uint32_t t = s->mode & JFS_S_IFMT;
    char *name = t == JFS_S_IFDIR ? jfs_asprintf("%s/", rel) : jfs_strdup(rel);
    const char *lnk = hard ? hard : link;
    char type;
    uint64_t size = (t == JFS_S_IFREG && !hard) ? s->size : 0;
    jfs_buf pax;
    char *prefix = NULL, *shortname = name;
    size_t nl = strlen(name);
    uint8_t *data = NULL;
    int rc = 0;
    char xbuf[4096];
    int64_t xl;
    switch (t) {
    case JFS_S_IFREG: type = hard ? '1' : '0'; break;
    case JFS_S_IFDIR: type = '5'; break;
    case JFS_S_IFLNK: type = '2'; break;
    case JFS_S_IFCHR: type = '3'; break;
    case JFS_S_IFBLK: type = '4'; break;
    case JFS_S_IFIFO: type = '6'; break;
    default:
        warnf(x->o, "%s: sockets cannot be stored in tar archives, skipped", rel);
        x->st->skipped++;
        free(name);
        return 0;
    }
    jfs_buf_init(&pax);
    if (nl > 100) {
        /* try the ustar prefix/name split first */
        size_t i;
        for (i = nl - 1; i > 0; i--) {
            if (name[i] == '/' && i <= 155 && nl - i - 1 <= 100 && nl - i - 1 > 0) {
                prefix = jfs_strndup(name, i);
                shortname = name + i + 1;
                break;
            }
        }
        if (!prefix)
            pax_rec(&pax, "path", name, nl);
    }
    if (lnk && strlen(lnk) > 100)
        pax_rec(&pax, "linkpath", lnk, strlen(lnk));
    if (size > 077777777777ULL) {
        char v[32];
        snprintf(v, sizeof v, "%llu", (unsigned long long)size);
        pax_rec(&pax, "size", v, strlen(v));
    }
    if (s->uid > 07777777 || s->gid > 07777777) {
        char v[32];
        snprintf(v, sizeof v, "%lu", (unsigned long)s->uid);
        pax_rec(&pax, "uid", v, strlen(v));
        snprintf(v, sizeof v, "%lu", (unsigned long)s->gid);
        pax_rec(&pax, "gid", v, strlen(v));
    }
    if (x->o->pax || s->mtime.sec < 0 || (uint64_t)s->mtime.sec > 077777777777ULL) {
        char v[64];
        snprintf(v, sizeof v, "%lld.%09ld", (long long)s->mtime.sec, (long)s->mtime.nsec);
        pax_rec(&pax, "mtime", v, strlen(v));
        if (x->o->pax) {
            snprintf(v, sizeof v, "%lld.%09ld", (long long)s->atime.sec, (long)s->atime.nsec);
            pax_rec(&pax, "atime", v, strlen(v));
        }
    }
    xl = jfs_listxattr(x->fs, ip, xbuf, sizeof xbuf);
    if (xl > 0) {
        const char *p = xbuf;
        while (p < xbuf + xl) {
            int64_t vl = jfs_getxattr(x->fs, ip, p, NULL, 0);
            if (vl >= 0) {
                uint8_t *v = jfs_malloc((size_t)vl + 1);
                char *key = jfs_asprintf("SCHILY.xattr.%s", p);
                jfs_getxattr(x->fs, ip, p, v, (size_t)vl);
                pax_rec(&pax, key, v, (size_t)vl);
                free(key);
                free(v);
            }
            p += strlen(p) + 1;
        }
    }
    if (pax.len) {
        char pname[101];
        const char *base = strrchr(rel, '/');
        snprintf(pname, sizeof pname, "PaxHeaders/%s", base ? base + 1 : rel);
        tar_raw_header(&x->out, pname, 'x', 0644, 0, 0, pax.len, s->mtime.sec, NULL, NULL, 0);
        tar_block(&x->out, (uint8_t *)pax.data, pax.len);
    }
    jfs_buf_free(&pax);
    tar_raw_header(&x->out, shortname, type, s->mode, s->uid, s->gid, size, s->mtime.sec, lnk, prefix, s->rdev);
    if (size) {
        rc = read_all(x->fs, ip, size, &data);
        if (!rc)
            tar_block(&x->out, data, size);
        free(data);
    }
    count_type(x, t, hard, size);
    free(prefix);
    free(name);
    if (rc)
        snprintf(x->err, x->errlen, "%s: %s", ip, jfs_strerror(rc));
    return rc;
}

/* ---------------------------------------------------------------- zip ---- */
static void w16(jfs_buf *b, unsigned v)
{
    uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    jfs_buf_append(b, t, 2);
}

static void w32(jfs_buf *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    jfs_buf_append(b, t, 4);
}

static void w64(jfs_buf *b, uint64_t v)
{
    w32(b, (uint32_t)v);
    w32(b, (uint32_t)(v >> 32));
}

static void dos_datetime(jfs_time t, uint16_t *dt, uint16_t *dd)
{
    time_t tt = (time_t)t.sec;
    struct tm *tm = localtime(&tt);
    if (!tm || tm->tm_year < 80) {
        *dt = 0;
        *dd = (1 << 5) | 1; /* 1980-01-01 */
        return;
    }
    *dt = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    *dd = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
}

static int zip_emit(exp_ctx *x, const char *rel, const char *ip, const jfs_stat *s, const char *link,
                    const char *hard)
{
    uint32_t t = s->mode & JFS_S_IFMT;
    char *name = t == JFS_S_IFDIR ? jfs_asprintf("%s/", rel) : jfs_strdup(rel);
    uint8_t *data = NULL, *comp = NULL;
    size_t ulen = 0, clen = 0, i, nl = strlen(name);
    uint16_t method = 0, dt, dd, flags = 0;
    uint32_t crc = 0, ext;
    uint64_t off = x->out.len;
    int rc = 0, z64, has_ut = s->mtime.sec >= 0 && s->mtime.sec <= 0x7fffffffLL;
    (void)hard;
    if (t != JFS_S_IFREG && t != JFS_S_IFDIR && t != JFS_S_IFLNK) {
        warnf(x->o, "%s: special files cannot be stored in zip archives, skipped", rel);
        x->st->skipped++;
        free(name);
        return 0;
    }
    for (i = 0; i < nl; i++)
        if ((uint8_t)name[i] >= 0x80)
            flags |= 0x800; /* UTF-8 names */
    if (t == JFS_S_IFREG) {
        if ((rc = read_all(x->fs, ip, s->size, &data)) != 0) {
            snprintf(x->err, x->errlen, "%s: %s", ip, jfs_strerror(rc));
            free(name);
            return rc;
        }
        ulen = (size_t)s->size;
    } else if (t == JFS_S_IFLNK) {
        data = (uint8_t *)jfs_strdup(link ? link : "");
        ulen = strlen((char *)data);
    }
    crc = jfs_crc32(0, data, ulen);
    if (t == JFS_S_IFREG && ulen > 0 && jfs_deflate_compress(data, ulen, x->o->level, &comp, &clen) == 0 &&
        clen < ulen) {
        method = 8;
    } else {
        free(comp);
        comp = NULL;
        clen = ulen;
    }
    dos_datetime(s->mtime, &dt, &dd);
    z64 = x->o->zip64 || ulen >= 0xFFFFFFFFULL || clen >= 0xFFFFFFFFULL || off >= 0xFFFFFFFFULL;
    if (z64)
        x->zip64_needed = 1;
    /* local header */
    w32(&x->out, 0x04034b50);
    w16(&x->out, z64 ? 45 : 20);
    w16(&x->out, flags);
    w16(&x->out, method);
    w16(&x->out, dt);
    w16(&x->out, dd);
    w32(&x->out, crc);
    w32(&x->out, z64 ? 0xFFFFFFFFU : (uint32_t)clen);
    w32(&x->out, z64 ? 0xFFFFFFFFU : (uint32_t)ulen);
    w16(&x->out, (unsigned)nl);
    w16(&x->out, (z64 ? 20 : 0) + (has_ut ? 9 : 0));
    jfs_buf_append(&x->out, name, nl);
    if (z64) {
        w16(&x->out, 0x0001);
        w16(&x->out, 16);
        w64(&x->out, ulen);
        w64(&x->out, clen);
    }
    if (has_ut) {
        w16(&x->out, 0x5455);
        w16(&x->out, 5);
        jfs_buf_putc(&x->out, 1);
        w32(&x->out, (uint32_t)s->mtime.sec);
    }
    jfs_buf_append(&x->out, method == 8 ? comp : data, clen);
    /* central directory record */
    ext = ((uint32_t)(s->mode & 0xFFFF) << 16) | (t == JFS_S_IFDIR ? 0x10 : 0);
    w32(&x->cdir, 0x02014b50);
    w16(&x->cdir, (3 << 8) | 63); /* made by: Unix, spec 6.3 */
    w16(&x->cdir, z64 ? 45 : 20);
    w16(&x->cdir, flags);
    w16(&x->cdir, method);
    w16(&x->cdir, dt);
    w16(&x->cdir, dd);
    w32(&x->cdir, crc);
    w32(&x->cdir, z64 ? 0xFFFFFFFFU : (uint32_t)clen);
    w32(&x->cdir, z64 ? 0xFFFFFFFFU : (uint32_t)ulen);
    w16(&x->cdir, (unsigned)nl);
    w16(&x->cdir, (z64 ? 28 : 0) + (has_ut ? 9 : 0));
    w16(&x->cdir, 0); /* comment */
    w16(&x->cdir, 0); /* disk */
    w16(&x->cdir, 0); /* internal attributes */
    w32(&x->cdir, ext);
    w32(&x->cdir, z64 ? 0xFFFFFFFFU : (uint32_t)off);
    jfs_buf_append(&x->cdir, name, nl);
    if (z64) {
        w16(&x->cdir, 0x0001);
        w16(&x->cdir, 24);
        w64(&x->cdir, ulen);
        w64(&x->cdir, clen);
        w64(&x->cdir, off);
    }
    if (has_ut) {
        w16(&x->cdir, 0x5455);
        w16(&x->cdir, 5);
        jfs_buf_putc(&x->cdir, 1);
        w32(&x->cdir, (uint32_t)s->mtime.sec);
    }
    x->nentries++;
    count_type(x, t, NULL, t == JFS_S_IFREG ? ulen : 0);
    free(data);
    free(comp);
    free(name);
    return 0;
}

static void zip_finish(exp_ctx *x)
{
    uint64_t cdoff = x->out.len, cdsize = x->cdir.len;
    int z64 = x->zip64_needed || x->nentries >= 0xFFFF || cdoff >= 0xFFFFFFFFULL || cdsize >= 0xFFFFFFFFULL;
    jfs_buf_append(&x->out, x->cdir.data, x->cdir.len);
    if (z64) {
        uint64_t z = x->out.len;
        w32(&x->out, 0x06064b50);
        w64(&x->out, 44);
        w16(&x->out, (3 << 8) | 63);
        w16(&x->out, 45);
        w32(&x->out, 0);
        w32(&x->out, 0);
        w64(&x->out, x->nentries);
        w64(&x->out, x->nentries);
        w64(&x->out, cdsize);
        w64(&x->out, cdoff);
        w32(&x->out, 0x07064b50);
        w32(&x->out, 0);
        w64(&x->out, z);
        w32(&x->out, 1);
    }
    w32(&x->out, 0x06054b50);
    w16(&x->out, 0);
    w16(&x->out, 0);
    w16(&x->out, z64 ? 0xFFFF : (unsigned)x->nentries);
    w16(&x->out, z64 ? 0xFFFF : (unsigned)x->nentries);
    w32(&x->out, z64 ? 0xFFFFFFFFU : (uint32_t)cdsize);
    w32(&x->out, z64 ? 0xFFFFFFFFU : (uint32_t)cdoff);
    w16(&x->out, 0);
}

/* ---------------------------------------------------------- libarchive ---- */
static int la_emit(exp_ctx *x, const char *rel, const char *ip, const jfs_stat *s, const char *link,
                   const char *hard)
{
    uint32_t t = s->mode & JFS_S_IFMT;
    la_entry *e = la.entry_new();
    int rc = 0;
    uint64_t size = (t == JFS_S_IFREG && !hard) ? s->size : 0;
    la.e_update_pathname_utf8(e, rel);
    la.e_set_filetype(e, t);
    la.e_set_perm(e, (la_mode_t)(s->mode & 07777));
    la.e_set_uid(e, s->uid);
    la.e_set_gid(e, s->gid);
    la.e_set_mtime(e, (la_time_t)s->mtime.sec, s->mtime.nsec);
    la.e_set_atime(e, (la_time_t)s->atime.sec, s->atime.nsec);
    la.e_set_size(e, (la_int64)size);
    if (t == JFS_S_IFLNK)
        la.e_update_symlink_utf8(e, link ? link : "");
    if (t == JFS_S_IFCHR || t == JFS_S_IFBLK)
        la.e_set_rdev(e, (la_dev_t)s->rdev);
    if (la.e_set_ino64) /* cpio identifies hard links by inode number */
        la.e_set_ino64(e, (la_int64)s->ino);
    if (la.e_set_nlink)
        la.e_set_nlink(e, s->nlink);
    if (hard)
        la.e_update_hardlink_utf8(e, hard);
    if (la.write_header(x->la, e) < LA_WARN) {
        const char *m = la.error_string(x->la);
        if (t != JFS_S_IFREG && t != JFS_S_IFDIR && t != JFS_S_IFLNK) {
            warnf(x->o, "%s: not supported by this format, skipped (%s)", rel, m ? m : "");
            x->st->skipped++;
            la.entry_free(e);
            return 0;
        }
        snprintf(x->err, x->errlen, "%s: %s", rel, m ? m : "write error");
        la.entry_free(e);
        return -JFS_EIO;
    }
    if (size) {
        jfs_handle *h;
        char buf[65536];
        uint64_t off = 0;
        rc = jfs_open_file(x->fs, ip, JFS_O_RDONLY, &h);
        if (rc) {
            snprintf(x->err, x->errlen, "%s: %s", rel, jfs_strerror(rc));
            la.entry_free(e);
            return rc;
        }
        while (off < size) {
            int64_t n = jfs_read(x->fs, h, buf, sizeof buf, off);
            if (n <= 0) {
                rc = n < 0 ? (int)n : -JFS_EIO;
                break;
            }
            if (la.write_data(x->la, buf, (size_t)n) < 0) {
                rc = -JFS_EIO;
                break;
            }
            off += (uint64_t)n;
        }
        jfs_release(x->fs, h);
        if (rc)
            snprintf(x->err, x->errlen, "%s: %s", rel,
                     la.error_string(x->la) ? la.error_string(x->la) : jfs_strerror(rc));
    }
    count_type(x, t, hard, size);
    la.entry_free(e);
    return rc;
}

/* ======================================================================== */
/* Export entry point                                                         */
/* ======================================================================== */
int jfs_archive_export(jfs_fs *fs, const char *src_dir, const char *archive, const jfs_archive_opts *o,
                       jfs_archive_stats *st, char *err, size_t errlen)
{
    exp_ctx x;
    jfs_archive_opts dflt;
    jfs_archive_stats dst;
    const fmt_info *fi;
    const char *fname = o && o->format ? o->format : jfs_archive_format_from_name(archive);
    int rc, use_la;
    jfs_stat rootst;
    size_t i;
    char dummy[8];
    if (!o) {
        jfs_archive_opts_init(&dflt);
        o = &dflt;
    }
    if (!st)
        st = &dst;
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    memset(st, 0, sizeof *st);
    if (!fname) {
        snprintf(err, errlen, "cannot tell the archive format from \"%s\"; use --format", archive);
        return -JFS_EINVAL;
    }
    fi = fmt_by_name(fname);
    if (!fi) {
        snprintf(err, errlen, "unknown archive format \"%s\"", fname);
        return -JFS_EINVAL;
    }
    if (!fi->la_format) {
        snprintf(err, errlen, "%s archives can be read but not written", fi->name);
        return -JFS_ENOTSUP;
    }
    if (!src_dir || !*src_dir)
        src_dir = "/";
    rc = jfs_getattr(fs, src_dir, &rootst);
    if (rc || (rootst.mode & JFS_S_IFMT) != JFS_S_IFDIR) {
        snprintf(err, errlen, "%s is not a directory in the image", src_dir);
        return rc ? rc : -JFS_ENOTDIR;
    }
    use_la = o->backend == JFS_AR_BACKEND_LIBARCHIVE || !fi->native;
    if (use_la && o->backend == JFS_AR_BACKEND_NATIVE) {
        snprintf(err, errlen, "%s is not a built-in format (tar, tar.gz, zip)", fi->name);
        return -JFS_ENOTSUP;
    }
    if (use_la && !jfs_libarchive_available(err, errlen)) {
        size_t l = strlen(err);
        snprintf(err + l, errlen - l, "; built-in formats are tar, tar.gz and zip");
        return -JFS_ENOTSUP;
    }
    memset(&x, 0, sizeof x);
    x.fs = fs;
    x.o = o;
    x.st = st;
    x.err = err;
    x.errlen = errlen;
    jfs_u64map_init(&x.seen);
    jfs_buf_init(&x.out);
    jfs_buf_init(&x.cdir);
    snprintf(st->format, sizeof st->format, "%s", fi->name);
    snprintf(st->backend, sizeof st->backend, "%s", use_la ? "libarchive" : "native");
    if (use_la) {
        x.la = la.write_new();
        x.emit = la_emit;
        /* tar formats store hard links by name; cpio by inode number (with the
         * data written for every name); the others duplicate the content */
        x.hardlinks = !!strstr(fi->la_format, "pax");
        if (la.write_set_format_by_name(x.la, fi->la_format) != LA_OK ||
            (fi->la_filter && la.write_add_filter_by_name(x.la, fi->la_filter) != LA_OK) ||
            la.write_open_filename(x.la, archive) != LA_OK) {
            const char *m = la.error_string(x.la);
            snprintf(err, errlen, "libarchive: %s", m ? m : "cannot create the archive");
            la.write_free(x.la);
            jfs_u64map_free(&x.seen);
            return -JFS_EIO;
        }
        rc = walk(&x, src_dir, "");
        if (la.write_close(x.la) != LA_OK && !rc) {
            const char *m = la.error_string(x.la);
            snprintf(err, errlen, "libarchive: %s", m ? m : "write error");
            rc = -JFS_EIO;
        }
        la.write_free(x.la);
        if (rc)
            jfs_remove_file(archive);
    } else {
        x.emit = fi->native == 3 ? zip_emit : tar_emit;
        x.hardlinks = fi->native != 3;
        rc = walk(&x, src_dir, "");
        if (!rc) {
            if (fi->native == 3) {
                zip_finish(&x);
            } else {
                static const uint8_t zero[1024];
                jfs_buf_append(&x.out, zero, 1024);
                while (x.out.len % 10240) /* default blocking factor 20 */
                    jfs_buf_append(&x.out, zero, 512);
            }
            if (fi->native == 2) {
                uint8_t *gz;
                size_t gl;
                rc = jfs_gzip_encode((uint8_t *)x.out.data, x.out.len, o->level, &gz, &gl);
                if (!rc) {
                    rc = jfs_write_file_atomic(archive, gz, gl);
                    free(gz);
                }
            } else {
                rc = jfs_write_file_atomic(archive, x.out.data, x.out.len);
            }
            if (rc)
                snprintf(err, errlen, "cannot write %s: %s", archive, jfs_strerror(rc));
        }
    }
    for (i = 0; i < x.seen.cap; i++)
        free(x.seen.vals[i]);
    jfs_u64map_free(&x.seen);
    jfs_buf_free(&x.out);
    jfs_buf_free(&x.cdir);
    return rc;
}

void jfs_archive_list_formats(void (*fn)(void *cookie, const char *line), void *cookie)
{
    char why[256], line[400];
    int have = jfs_libarchive_available(why, sizeof why), i;
    fn(cookie, "built in (all platforms):");
    fn(cookie, "  tar      .tar             read + write (ustar, pax, GNU)");
    fn(cookie, "  tar.gz   .tar.gz .tgz     read + write");
    fn(cookie, "  zip      .zip .jar        read + write (stored/DEFLATE, zip64)");
    fn(cookie, have ? "through libarchive (available):" : "through libarchive (NOT available):");
    for (i = 0; formats[i].ext; i++) {
        if (formats[i].native || (i > 0 && !strcmp(formats[i - 1].name, formats[i].name)))
            continue;
        snprintf(line, sizeof line, "  %-8s %-16s %s", formats[i].name, formats[i].ext,
                 formats[i].la_format ? "read + write" : "read only");
        fn(cookie, line);
    }
    if (!have) {
        snprintf(line, sizeof line, "  (%s)", why);
        fn(cookie, line);
    } else {
        fn(cookie, "  ...and any other format libarchive can read (auto-detected)");
    }
}
