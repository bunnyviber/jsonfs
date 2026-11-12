/*
 * JSONfs - library internals shared between jfs_image.c, jfs_vfs.c and
 * jfs_maint.c.  Not part of the public API.
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_INTERNAL_H
#define JFS_INTERNAL_H

#include "jfs_codec.h"
#include "jfs_fs.h"
#include "jfs_json.h"

typedef enum {
    JFS_T_NONE = 0,
    JFS_T_FILE,
    JFS_T_DIR,
    JFS_T_SYMLINK,
    JFS_T_FIFO,
    JFS_T_SOCK,
    JFS_T_CHR,
    JFS_T_BLK
} jfs_type;

typedef struct jfs_xattr {
    char *name;
    uint8_t *val;
    size_t len;
} jfs_xattr;

typedef struct jfs_inode jfs_inode;
struct jfs_inode {
    uint64_t ino;
    jfs_type type;
    uint32_t mode; /* permission bits only (07777) */
    uint32_t uid, gid;
    uint64_t rdev;
    uint32_t nlink; /* derived from directory entries */
    jfs_time atime, mtime, ctime, btime, vtime;
    int deleted; /* the current record is a delete marker */

    /* regular file content */
    uint64_t size;
    char *comp;   /* algorithm of `blob` */
    char *blob;   /* base64(compress(content)); valid when !data_dirty */
    size_t blob_len;
    uint8_t sha[32];
    int sha_valid;
    uint8_t *data; /* decoded content cache */
    size_t data_cap;
    int data_loaded;
    int data_dirty;

    char *target;         /* symlink */
    jfs_strmap *entries;  /* directory */
    jfs_xattr *xattrs;
    size_t nxattrs;

    jv *history; /* JSON array of older records (oldest first), or NULL */

    /* bookkeeping */
    int persisted;    /* current record exists in the committed image */
    int version_open; /* a history record was already pushed since the last commit */
    int hist_needs_data; /* newest history record has "data_ref":"newer" (shares our content) */
    int blob_ref;        /* view only: record had no data of its own (resolved by the loader) */
    int open_count;
    int orphan;           /* unlinked while open; lives in fs->orphans */
    jfs_inode *moved_to;  /* reload bookkeeping */
};

struct jfs_handle {
    jfs_inode *ino;
    int flags;
    jfs_handle *prev, *next;
};

struct jfs_fs {
    char *image_path;
    jfs_lockfile *lock;
    int lockmode;
    int readonly;
    int shared;
    int sync_mode; /* commit after every mutation */
    int commit_interval_ms;
    int repair;

    int schema;
    char uuid[37];
    char *label;
    jfs_time created, modified;
    uint64_t generation;
    char *default_comp;   /* image default */
    char *write_comp;     /* used for data written by this session (spec, may include :level) */
    int versioning;
    uint64_t next_ino;
    uint64_t root_ino;
    jv *extra_header; /* unknown top-level keys preserved verbatim */

    jfs_u64map inodes;
    jfs_inode **orphans;
    size_t norphans, orphans_cap;
    jfs_handle *handles;

    int dirty;
    int need_reload;
    jfs_filesig sig; /* signature of the image file at last load/commit */

    int is_view;
    jfs_time view_time;

    jfs_mutex mu;
    jfs_thread *committer;
    volatile int committer_stop;
    jfs_time last_commit;
    int last_commit_error;
};

/* jfs_image.c */
const char *jfs_type_name(jfs_type t);
jfs_type jfs_type_from_name(const char *s);
uint32_t jfs_type_mode_bits(jfs_type t);
jfs_inode *jfs_inode_new(jfs_fs *fs, jfs_type t);
void jfs_inode_free(jfs_inode *in);
jfs_inode *jfs_inode_from_record(jv *rec, uint64_t ino, int schema, int take, char *err, size_t errlen);
jv *jfs_inode_to_record(jfs_fs *fs, jfs_inode *in);
int jfs_inode_load_data(jfs_fs *fs, jfs_inode *in);
int jfs_inode_encode(jfs_fs *fs, jfs_inode *in, const char *algo_spec);
jfs_inode *jfs_lookup_ino(jfs_fs *fs, uint64_t ino); /* NULL for missing or delete markers */
int jfs_commit_locked(jfs_fs *fs);
int jfs_serialize(jfs_fs *fs, jfs_buf *out);
int jfs_fs_create_empty(jfs_fs **out);
void jfs_fs_free(jfs_fs *fs);
int jfs_fs_load_json(jfs_fs *fs, jv *root, const jfs_time *at, char *err, size_t errlen);
int jfs_maybe_reload(jfs_fs *fs);
void jfs_recount_links(jfs_fs *fs);
void jfs_orphan_add(jfs_fs *fs, jfs_inode *in);
void jfs_orphan_release(jfs_fs *fs, jfs_inode *in);

/* jfs_vfs.c */
void jfs_version_touch(jfs_fs *fs, jfs_inode *in);
void jfs_version_before_data_change(jfs_fs *fs, jfs_inode *in);
/* Given an inode's current record and history array, find the record that
 * actually carries the content of `rec` (follows "data_ref": "newer"). */
jv *jfs_record_data_source(jv *cur, jv *hist, jv *rec);
int jfs_resolve(jfs_fs *fs, const char *path, jfs_inode **out);

#endif
