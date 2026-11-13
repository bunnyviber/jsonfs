/*
 * JSONfs - converting archives to and from JSONfs images.
 *
 * Built in (no dependencies, every platform incl. Windows XP):
 *   tar   POSIX ustar / pax, GNU (long names, base-256 numbers), v7 - read+write
 *   tgz   gzip compressed tar (.tar.gz, .tgz)                     - read+write
 *   zip   stored / DEFLATE, zip64, Unix modes, symlinks, UT times  - read+write
 *
 * Through libarchive (loaded at runtime when installed: macOS and FreeBSD
 * ship it, most Linux distributions have libarchive13):
 *   7z, tar.xz, tar.bz2, tar.zst, tar.lz4, tar.lzma, cpio, iso9660, xar, ar,
 *   rar / rar5 / cab / lha (read only) and every other format libarchive knows.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_ARCHIVE_H
#define JFS_ARCHIVE_H

#include "jfs_fs.h"

enum { JFS_AR_BACKEND_AUTO = 0, JFS_AR_BACKEND_NATIVE = 1, JFS_AR_BACKEND_LIBARCHIVE = 2 };

typedef struct jfs_archive_opts {
    const char *format;   /* NULL: from the file name (writing) / contents (reading) */
    int backend;          /* JFS_AR_BACKEND_* */
    int same_owner;       /* import: keep owners stored in the archive */
    uint32_t uid, gid;    /* import: owner otherwise (and for formats without owners) */
    int level;            /* export: compression level (-1 = default) */
    int pax;              /* export tar: always write pax headers (ns times, atime) */
    int zip64;            /* export zip: always write zip64 records (testing/compat) */
    void (*warn)(void *cookie, const char *msg);
    void *cookie;
} jfs_archive_opts;

typedef struct jfs_archive_stats {
    uint64_t entries, files, dirs, symlinks, hardlinks, specials, bytes, skipped;
    char format[64];  /* format actually used */
    char backend[16]; /* "native" or "libarchive" */
} jfs_archive_stats;

void jfs_archive_opts_init(jfs_archive_opts *o);

/* Extract `archive` into `dest_dir` of an open (writable) image. */
int jfs_archive_import(jfs_fs *fs, const char *archive, const char *dest_dir, const jfs_archive_opts *o,
                       jfs_archive_stats *st, char *err, size_t errlen);

/* Write the tree at `src_dir` of an image into a new archive file. */
int jfs_archive_export(jfs_fs *fs, const char *src_dir, const char *archive, const jfs_archive_opts *o,
                       jfs_archive_stats *st, char *err, size_t errlen);

/* Format name implied by a file name ("zip", "tar.gz", "7z", ...) or NULL. */
const char *jfs_archive_format_from_name(const char *filename);

/* Is libarchive usable?  Fills `why` otherwise. */
int jfs_libarchive_available(char *why, size_t whylen);

/* Prints the supported formats (one per line) with fn. */
void jfs_archive_list_formats(void (*fn)(void *cookie, const char *line), void *cookie);

#endif
