/*
 * JSONfs - POSIX semantics on top of the in-memory image.
 *
 * Every public function takes the volume lock, and, for multi-attached
 * volumes, the transaction lock in the shared lock file (reloading the image
 * if a peer committed in the meantime).  Metadata changes are committed
 * immediately in multi-attach/sync mode and by the background committer
 * otherwise.  File data written through a handle is committed on
 * flush/close (close-to-open consistency) in multi-attach mode.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jfs_internal.h"

#include <stdlib.h>
#include <string.h>

#define JFS_NAME_MAX 255
#define JFS_LINK_MAX 65000
#define JFS_FILE_MAX ((uint64_t)1 << 40) /* 1 TiB, far beyond what fits in RAM anyway */

enum { OP_READ = 0, OP_META = 1, OP_DATA = 2 };

static int op_enter(jfs_fs *fs, int kind)
{
    jfs_mutex_lock(&fs->mu);
    if (kind != OP_READ && fs->readonly) {
        jfs_mutex_unlock(&fs->mu);
        return -JFS_EROFS;
    }
    if (fs->shared && fs->lock && !fs->replay_target) {
        int rc;
        jfs_lockfile_lock(fs->lock, JFS_LOCK_TXN, kind == OP_META, 1);
        rc = jfs_maybe_reload(fs);
        if (rc) {
            jfs_lockfile_unlock(fs->lock, JFS_LOCK_TXN);
            jfs_mutex_unlock(&fs->mu);
            return rc;
        }
    }
    return 0;
}

static int64_t op_leave(jfs_fs *fs, int kind, int64_t rc)
{
    if (kind == OP_META && rc >= 0 && fs->dirty && fs->sync_mode) {
        int c = jfs_commit_locked(fs);
        if (c) {
            rc = c;
            if (fs->shared)
                fs->need_reload = 1; /* roll back to the committed state */
        }
    }
    if (fs->shared && fs->lock)
        jfs_lockfile_unlock(fs->lock, JFS_LOCK_TXN);
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

#define ENTER(kind)                     \
    do {                                \
        int e_ = op_enter(fs, (kind));  \
        if (e_)                         \
            return e_;                  \
    } while (0)
#define LEAVE(kind, rc) return op_leave(fs, (kind), (rc))

/* ======================================================================== */
/* Versioning                                                                 */
/* ======================================================================== */
void jfs_version_touch(jfs_fs *fs, jfs_inode *in)
{
    jv *rec;
    if (in->version_open || in->orphan)
        return;
    in->version_open = 1;
    if (!fs->versioning || !in->persisted || in->deleted)
        return;
    if (in->type == JFS_T_FILE) {
        /* the content is shared with the current record until it changes */
        char *blob = in->blob;
        size_t bl = in->blob_len;
        if (in->data_dirty) /* cannot happen while version_open==0, be safe */
            jfs_inode_encode(fs, in, fs->write_comp);
        blob = in->blob;
        bl = in->blob_len;
        in->blob = NULL;
        in->blob_len = 0;
        rec = jfs_inode_to_record(fs, in);
        in->blob = blob;
        in->blob_len = bl;
        jv_obj_del(rec, "data");
        jv_obj_set(rec, "data_ref", jv_str("newer"));
        in->hist_needs_data = 1;
    } else {
        rec = jfs_inode_to_record(fs, in);
    }
    if (!in->history)
        in->history = jv_arr();
    jv_arr_push(in->history, rec);
}

void jfs_version_before_data_change(jfs_fs *fs, jfs_inode *in)
{
    jv *last;
    if (!in->hist_needs_data || !in->history || !in->history->n)
        return;
    if (in->data_dirty)
        jfs_inode_encode(fs, in, fs->write_comp);
    last = in->history->items[in->history->n - 1];
    jv_obj_del(last, "data_ref");
    jv_obj_set(last, "compression", jv_str(in->comp ? in->comp : "none"));
    jv_obj_set(last, "data", jv_strn(in->blob ? in->blob : "", in->blob ? in->blob_len : 0));
    in->hist_needs_data = 0;
}

/* Record a change to `in` (pushes the previous version when versioning). */
static void modified(jfs_fs *fs, jfs_inode *in)
{
    jfs_version_touch(fs, in);
    in->vtime = jfs_now();
    fs->dirty = 1;
}

static void data_modified(jfs_fs *fs, jfs_inode *in)
{
    modified(fs, in);
    jfs_version_before_data_change(fs, in);
}

/* ======================================================================== */
/* Path resolution                                                            */
/* ======================================================================== */
static int check_name(const char *name, size_t len)
{
    if (len == 0)
        return -JFS_EINVAL;
    if (len > JFS_NAME_MAX)
        return -JFS_ENAMETOOLONG;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -JFS_EINVAL;
    return 0;
}

int jfs_resolve(jfs_fs *fs, const char *path, jfs_inode **out)
{
    jfs_inode *cur = jfs_lookup_ino(fs, fs->root_ino);
    const char *p = path;
    char name[JFS_NAME_MAX + 1];
    if (!path || *path != '/')
        return -JFS_EINVAL;
    while (*p) {
        const char *s;
        size_t len;
        uint64_t ino;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - s);
        if (len > JFS_NAME_MAX)
            return -JFS_ENAMETOOLONG;
        if (cur->type != JFS_T_DIR)
            return -JFS_ENOTDIR;
        if (len == 1 && s[0] == '.')
            continue;
        memcpy(name, s, len);
        name[len] = 0;
        if (!jfs_strmap_get(cur->entries, name, &ino))
            return -JFS_ENOENT;
        cur = jfs_lookup_ino(fs, ino);
        if (!cur)
            return -JFS_ENOENT;
    }
    *out = cur;
    return 0;
}

/* Splits path into parent directory inode and final component. */
static int resolve_parent(jfs_fs *fs, const char *path, jfs_inode **dir, char name[JFS_NAME_MAX + 1])
{
    const char *end, *slash;
    char *parent;
    size_t len;
    int rc;
    if (!path || *path != '/')
        return -JFS_EINVAL;
    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/')
        end--;
    slash = end;
    while (slash > path && slash[-1] != '/')
        slash--;
    len = (size_t)(end - slash);
    if (len == 0)
        return -JFS_EBUSY; /* the root itself */
    rc = check_name(slash, len);
    if (rc)
        return rc;
    memcpy(name, slash, len);
    name[len] = 0;
    parent = jfs_strndup(path, (size_t)(slash - path));
    if (!*parent) {
        free(parent);
        parent = jfs_strdup("/");
    }
    rc = jfs_resolve(fs, parent, dir);
    free(parent);
    if (rc)
        return rc;
    if ((*dir)->type != JFS_T_DIR)
        return -JFS_ENOTDIR;
    return 0;
}

static jfs_inode *dir_lookup(jfs_fs *fs, jfs_inode *dir, const char *name)
{
    uint64_t ino;
    if (!jfs_strmap_get(dir->entries, name, &ino))
        return NULL;
    return jfs_lookup_ino(fs, ino);
}

/* ======================================================================== */
/* Helpers                                                                    */
/* ======================================================================== */
static void fill_stat(jfs_fs *fs, jfs_inode *in, jfs_stat *st)
{
    (void)fs;
    memset(st, 0, sizeof *st);
    st->ino = in->ino;
    st->mode = jfs_type_mode_bits(in->type) | (in->mode & 07777);
    st->nlink = in->nlink ? in->nlink : (in->orphan ? 0 : 1);
    st->uid = in->uid;
    st->gid = in->gid;
    st->rdev = in->rdev;
    st->atime = in->atime;
    st->mtime = in->mtime;
    st->ctime = in->ctime;
    st->btime = in->btime;
    switch (in->type) {
    case JFS_T_FILE:
        st->size = in->size;
        if (in->data_dirty || !in->blob)
            st->blocks = (in->size + 511) / 512;
        else
            st->blocks = (in->blob_len / 4 * 3 + 511) / 512; /* compressed footprint */
        break;
    case JFS_T_SYMLINK:
        st->size = in->target ? strlen(in->target) : 0;
        break;
    case JFS_T_DIR:
        st->size = 4096;
        st->blocks = 8;
        break;
    default: break;
    }
}

static void touch_mc(jfs_inode *in)
{
    in->mtime = in->ctime = jfs_now();
}

static void dir_link(jfs_fs *fs, jfs_inode *dir, const char *name, jfs_inode *child)
{
    modified(fs, dir);
    jfs_strmap_put(dir->entries, name, child->ino);
    touch_mc(dir);
    if (child->type == JFS_T_DIR)
        dir->nlink++;
    else
        child->nlink++;
    child->ctime = jfs_now();
    fs->dirty = 1;
}

static void dir_unlink(jfs_fs *fs, jfs_inode *dir, const char *name, jfs_inode *child)
{
    modified(fs, dir);
    jfs_strmap_del(dir->entries, name);
    touch_mc(dir);
    if (child->type == JFS_T_DIR) {
        if (dir->nlink > 2)
            dir->nlink--;
    } else if (child->nlink) {
        child->nlink--;
    }
    child->ctime = jfs_now();
    fs->dirty = 1;
}

/* Called when the last name of an inode disappears. */
static void drop_inode(jfs_fs *fs, jfs_inode *in)
{
    jfs_u64map_del(&fs->inodes, in->ino);
    if (fs->versioning && in->persisted) {
        jfs_inode *marker = jfs_calloc(1, sizeof *marker);
        jfs_version_touch(fs, in);
        jfs_version_before_data_change(fs, in);
        marker->ino = in->ino;
        marker->type = in->type;
        marker->deleted = 1;
        marker->vtime = jfs_now();
        marker->ctime = marker->vtime;
        marker->history = in->history;
        marker->version_open = 1;
        in->history = NULL;
        jfs_u64map_put(&fs->inodes, marker->ino, marker);
    }
    fs->dirty = 1;
    if (in->open_count > 0) {
        in->persisted = 0;
        jfs_orphan_add(fs, in);
    } else {
        jfs_inode_free(in);
    }
}

static int ensure_data(jfs_fs *fs, jfs_inode *in)
{
    return in->data_loaded ? 0 : jfs_inode_load_data(fs, in);
}

static int resize_data(jfs_inode *in, uint64_t size)
{
    if (size > JFS_FILE_MAX || size > (uint64_t)((size_t)-1) / 2)
        return -JFS_EFBIG;
    if (size + 1 > in->data_cap) {
        size_t nc = in->data_cap ? in->data_cap : 4096;
        while (nc < size + 1)
            nc = nc < ((size_t)1 << 30) ? nc * 2 : nc + ((size_t)1 << 30);
        in->data = jfs_realloc(in->data, nc);
        in->data_cap = nc;
    }
    if (size > in->size)
        memset(in->data + in->size, 0, (size_t)(size - in->size));
    in->size = size;
    return 0;
}

static int do_truncate(jfs_fs *fs, jfs_inode *in, uint64_t size)
{
    int rc;
    if (in->type == JFS_T_DIR)
        return -JFS_EISDIR;
    if (in->type != JFS_T_FILE)
        return -JFS_EINVAL;
    rc = ensure_data(fs, in);
    if (rc)
        return rc;
    if (size == in->size && size != 0) {
        touch_mc(in);
        return 0;
    }
    data_modified(fs, in);
    rc = resize_data(in, size);
    if (rc)
        return rc;
    in->data_dirty = 1;
    touch_mc(in);
    return 0;
}

static jfs_handle *handle_new(jfs_fs *fs, jfs_inode *in, int flags)
{
    jfs_handle *h = jfs_calloc(1, sizeof *h);
    h->ino = in;
    h->flags = flags;
    h->next = fs->handles;
    if (fs->handles)
        fs->handles->prev = h;
    fs->handles = h;
    in->open_count++;
    return h;
}

static int make_node(jfs_fs *fs, const char *path, jfs_type type, uint32_t mode, const jfs_ctx *ctx,
                     jfs_inode **out)
{
    jfs_inode *dir, *in;
    char name[JFS_NAME_MAX + 1];
    int rc = resolve_parent(fs, path, &dir, name);
    if (rc)
        return rc == -JFS_EBUSY ? -JFS_EEXIST : rc;
    if (dir_lookup(fs, dir, name))
        return -JFS_EEXIST;
    if (dir->nlink >= JFS_LINK_MAX && type == JFS_T_DIR)
        return -JFS_EMLINK;
    in = jfs_inode_new(fs, type);
    in->mode = mode & 07777 & ~(ctx ? ctx->umask : 0);
    in->uid = ctx ? ctx->uid : 0;
    in->gid = ctx ? ctx->gid : 0;
    if (dir->mode & JFS_S_ISGID) {
        in->gid = dir->gid;
        if (type == JFS_T_DIR)
            in->mode |= JFS_S_ISGID;
    }
    in->nlink = type == JFS_T_DIR ? 2 : 0;
    dir_link(fs, dir, name, in);
    *out = in;
    return 0;
}

/* ======================================================================== */
/* Attributes                                                                 */
/* ======================================================================== */
int jfs_getattr(jfs_fs *fs, const char *path, jfs_stat *st)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_READ);
    rc = jfs_resolve(fs, path, &in);
    if (!rc)
        fill_stat(fs, in, st);
    LEAVE(OP_READ, rc);
}

int jfs_fgetattr(jfs_fs *fs, jfs_handle *h, jfs_stat *st)
{
    ENTER(OP_READ);
    fill_stat(fs, h->ino, st);
    LEAVE(OP_READ, 0);
}

int jfs_access(jfs_fs *fs, const char *path, int mask, const jfs_ctx *ctx)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_READ);
    rc = jfs_resolve(fs, path, &in);
    if (!rc && (mask & JFS_W_OK) && fs->readonly)
        rc = -JFS_EROFS;
    if (!rc && mask && ctx) {
        uint32_t bits;
        if (ctx->uid == 0) {
            if ((mask & JFS_X_OK) && in->type != JFS_T_DIR && !(in->mode & 0111))
                rc = -JFS_EACCES;
        } else {
            if (ctx->uid == in->uid)
                bits = (in->mode >> 6) & 7;
            else if (ctx->gid == in->gid)
                bits = (in->mode >> 3) & 7;
            else
                bits = in->mode & 7;
            if ((bits & (uint32_t)mask) != (uint32_t)mask)
                rc = -JFS_EACCES;
        }
    }
    LEAVE(OP_READ, rc);
}

int jfs_chmod(jfs_fs *fs, const char *path, uint32_t mode)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, path, &in);
    if (!rc) {
        modified(fs, in);
        in->mode = mode & 07777;
        in->ctime = jfs_now();
    }
    LEAVE(OP_META, rc);
}

int jfs_chown(jfs_fs *fs, const char *path, uint32_t uid, uint32_t gid)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, path, &in);
    if (!rc && (uid != (uint32_t)-1 || gid != (uint32_t)-1)) {
        modified(fs, in);
        if (uid != (uint32_t)-1)
            in->uid = uid;
        if (gid != (uint32_t)-1)
            in->gid = gid;
        /* POSIX: clear set-id bits on non-directories when ownership changes */
        if (in->type != JFS_T_DIR)
            in->mode &= ~(uint32_t)(JFS_S_ISUID | JFS_S_ISGID);
        in->ctime = jfs_now();
    }
    LEAVE(OP_META, rc);
}

static jfs_time resolve_time(jfs_time t, jfs_time now)
{
    return t.nsec == JFS_UTIME_NOW ? now : t;
}

int jfs_utimens(jfs_fs *fs, const char *path, const jfs_time times[2])
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, path, &in);
    if (!rc) {
        jfs_time now = jfs_now();
        int set_a = !times || times[0].nsec != JFS_UTIME_OMIT;
        int set_m = !times || times[1].nsec != JFS_UTIME_OMIT;
        if (set_m)
            modified(fs, in); /* atime-only updates are not versioned */
        if (set_a)
            in->atime = times ? resolve_time(times[0], now) : now;
        if (set_m)
            in->mtime = times ? resolve_time(times[1], now) : now;
        in->ctime = now;
        fs->dirty = 1;
    }
    LEAVE(OP_META, rc);
}

int jfs_set_btime(jfs_fs *fs, const char *path, jfs_time btime)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, path, &in);
    if (!rc) {
        modified(fs, in);
        in->btime = btime;
    }
    LEAVE(OP_META, rc);
}

int jfs_readlink(jfs_fs *fs, const char *path, char *buf, size_t size)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_READ);
    rc = jfs_resolve(fs, path, &in);
    if (!rc && in->type != JFS_T_SYMLINK)
        rc = -JFS_EINVAL;
    if (!rc && size) {
        size_t n = strlen(in->target);
        if (n >= size)
            n = size - 1;
        memcpy(buf, in->target, n);
        buf[n] = 0;
    }
    LEAVE(OP_READ, rc);
}

/* ======================================================================== */
/* Namespace operations                                                       */
/* ======================================================================== */
int jfs_mknod(jfs_fs *fs, const char *path, uint32_t mode, uint64_t rdev, const jfs_ctx *ctx)
{
    jfs_type t;
    jfs_inode *in;
    int rc;
    switch (mode & JFS_S_IFMT) {
    case 0:
    case JFS_S_IFREG: t = JFS_T_FILE; break;
    case JFS_S_IFIFO: t = JFS_T_FIFO; break;
    case JFS_S_IFSOCK: t = JFS_T_SOCK; break;
    case JFS_S_IFCHR: t = JFS_T_CHR; break;
    case JFS_S_IFBLK: t = JFS_T_BLK; break;
    case JFS_S_IFDIR: return jfs_mkdir(fs, path, mode, ctx);
    default: return -JFS_EINVAL;
    }
    ENTER(OP_META);
    rc = make_node(fs, path, t, mode, ctx, &in);
    if (!rc && (t == JFS_T_CHR || t == JFS_T_BLK))
        in->rdev = rdev;
    LEAVE(OP_META, rc);
}

int jfs_mkdir(jfs_fs *fs, const char *path, uint32_t mode, const jfs_ctx *ctx)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = make_node(fs, path, JFS_T_DIR, mode, ctx, &in);
    LEAVE(OP_META, rc);
}

int jfs_symlink(jfs_fs *fs, const char *target, const char *linkpath, const jfs_ctx *ctx)
{
    jfs_inode *in;
    int rc;
    jfs_ctx c;
    if (!target || strlen(target) >= 4096)
        return -JFS_ENAMETOOLONG;
    if (ctx) {
        c = *ctx;
        c.umask = 0;
    } else {
        memset(&c, 0, sizeof c);
    }
    ENTER(OP_META);
    rc = make_node(fs, linkpath, JFS_T_SYMLINK, 0777, &c, &in);
    if (!rc) {
        in->target = jfs_strdup(target);
        in->size = strlen(target);
    }
    LEAVE(OP_META, rc);
}

int jfs_unlink(jfs_fs *fs, const char *path)
{
    jfs_inode *dir, *in;
    char name[JFS_NAME_MAX + 1];
    int rc;
    ENTER(OP_META);
    rc = resolve_parent(fs, path, &dir, name);
    if (rc == -JFS_EBUSY)
        rc = -JFS_EISDIR;
    if (!rc) {
        in = dir_lookup(fs, dir, name);
        if (!in)
            rc = -JFS_ENOENT;
        else if (in->type == JFS_T_DIR)
            rc = -JFS_EISDIR;
        else {
            dir_unlink(fs, dir, name, in);
            if (in->nlink == 0)
                drop_inode(fs, in);
        }
    }
    LEAVE(OP_META, rc);
}

int jfs_rmdir(jfs_fs *fs, const char *path)
{
    jfs_inode *dir, *in;
    char name[JFS_NAME_MAX + 1];
    int rc;
    ENTER(OP_META);
    rc = resolve_parent(fs, path, &dir, name);
    if (!rc) {
        in = dir_lookup(fs, dir, name);
        if (!in)
            rc = -JFS_ENOENT;
        else if (in->type != JFS_T_DIR)
            rc = -JFS_ENOTDIR;
        else if (in->entries->count)
            rc = -JFS_ENOTEMPTY;
        else {
            dir_unlink(fs, dir, name, in);
            drop_inode(fs, in);
        }
    } else if (rc == -JFS_EINVAL && path) {
        /* rmdir of "." or ".." */
        rc = -JFS_EINVAL;
    }
    LEAVE(OP_META, rc);
}

static int is_subpath(const char *parent, const char *child)
{
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/')
        n--;
    return !strncmp(parent, child, n) && child[n] == '/';
}

int jfs_rename(jfs_fs *fs, const char *from, const char *to, unsigned flags)
{
    jfs_inode *sdir, *ddir, *src, *dst;
    char sname[JFS_NAME_MAX + 1], dname[JFS_NAME_MAX + 1];
    int rc;
    if ((flags & JFS_RENAME_NOREPLACE) && (flags & JFS_RENAME_EXCHANGE))
        return -JFS_EINVAL;
    if (flags & ~(unsigned)(JFS_RENAME_NOREPLACE | JFS_RENAME_EXCHANGE))
        return -JFS_EINVAL;
    ENTER(OP_META);
    rc = resolve_parent(fs, from, &sdir, sname);
    if (!rc)
        rc = resolve_parent(fs, to, &ddir, dname);
    if (rc)
        LEAVE(OP_META, rc);
    src = dir_lookup(fs, sdir, sname);
    if (!src)
        LEAVE(OP_META, -JFS_ENOENT);
    dst = dir_lookup(fs, ddir, dname);
    if (sdir == ddir && !strcmp(sname, dname))
        LEAVE(OP_META, 0);
    if (src->type == JFS_T_DIR && is_subpath(from, to))
        LEAVE(OP_META, -JFS_EINVAL);
    if (dst && (flags & JFS_RENAME_NOREPLACE))
        LEAVE(OP_META, -JFS_EEXIST);
    if (flags & JFS_RENAME_EXCHANGE) {
        if (!dst)
            LEAVE(OP_META, -JFS_ENOENT);
        if (dst->type == JFS_T_DIR && is_subpath(to, from))
            LEAVE(OP_META, -JFS_EINVAL);
        modified(fs, sdir);
        modified(fs, ddir);
        jfs_strmap_put(sdir->entries, sname, dst->ino);
        jfs_strmap_put(ddir->entries, dname, src->ino);
        if (sdir != ddir) {
            if (src->type == JFS_T_DIR && dst->type != JFS_T_DIR) {
                sdir->nlink--;
                ddir->nlink++;
            } else if (dst->type == JFS_T_DIR && src->type != JFS_T_DIR) {
                ddir->nlink--;
                sdir->nlink++;
            }
        }
        touch_mc(sdir);
        touch_mc(ddir);
        src->ctime = dst->ctime = jfs_now();
        fs->dirty = 1;
        LEAVE(OP_META, 0);
    }
    if (dst) {
        if (dst == src)
            LEAVE(OP_META, 0); /* hard links to the same inode: no-op */
        if (src->type == JFS_T_DIR) {
            if (dst->type != JFS_T_DIR)
                LEAVE(OP_META, -JFS_ENOTDIR);
            if (dst->entries->count)
                LEAVE(OP_META, -JFS_ENOTEMPTY);
        } else if (dst->type == JFS_T_DIR) {
            LEAVE(OP_META, -JFS_EISDIR);
        }
        dir_unlink(fs, ddir, dname, dst);
        if (dst->type == JFS_T_DIR || dst->nlink == 0)
            drop_inode(fs, dst);
    }
    /* move the entry */
    modified(fs, sdir);
    jfs_strmap_del(sdir->entries, sname);
    touch_mc(sdir);
    modified(fs, ddir);
    jfs_strmap_put(ddir->entries, dname, src->ino);
    touch_mc(ddir);
    if (src->type == JFS_T_DIR && sdir != ddir) {
        sdir->nlink--;
        ddir->nlink++;
    }
    src->ctime = jfs_now();
    fs->dirty = 1;
    LEAVE(OP_META, 0);
}

int jfs_link(jfs_fs *fs, const char *from, const char *to)
{
    jfs_inode *src, *dir;
    char name[JFS_NAME_MAX + 1];
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, from, &src);
    if (!rc && src->type == JFS_T_DIR)
        rc = -JFS_EPERM;
    if (!rc && src->nlink >= JFS_LINK_MAX)
        rc = -JFS_EMLINK;
    if (!rc)
        rc = resolve_parent(fs, to, &dir, name);
    if (rc == -JFS_EBUSY)
        rc = -JFS_EEXIST;
    if (!rc && dir_lookup(fs, dir, name))
        rc = -JFS_EEXIST;
    if (!rc)
        dir_link(fs, dir, name, src);
    LEAVE(OP_META, rc);
}

/* ======================================================================== */
/* File I/O                                                                   */
/* ======================================================================== */
int jfs_truncate(jfs_fs *fs, const char *path, uint64_t size)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, path, &in);
    if (!rc)
        rc = do_truncate(fs, in, size);
    LEAVE(OP_META, rc);
}

int jfs_ftruncate(jfs_fs *fs, jfs_handle *h, uint64_t size)
{
    int rc;
    if ((h->flags & JFS_O_ACCMODE) == JFS_O_RDONLY)
        return -JFS_EBADF;
    ENTER(OP_DATA);
    rc = do_truncate(fs, h->ino, size);
    LEAVE(OP_DATA, rc);
}

int jfs_open_file(jfs_fs *fs, const char *path, int flags, jfs_handle **out)
{
    jfs_inode *in;
    int rc, wr = (flags & JFS_O_ACCMODE) != JFS_O_RDONLY || (flags & JFS_O_TRUNC);
    int kind = wr ? OP_META : OP_READ;
    ENTER(kind);
    rc = jfs_resolve(fs, path, &in);
    if (!rc && in->type == JFS_T_DIR && wr)
        rc = -JFS_EISDIR;
    if (!rc && (flags & JFS_O_TRUNC) && in->type == JFS_T_FILE && wr)
        rc = do_truncate(fs, in, 0);
    if (!rc)
        *out = handle_new(fs, in, flags);
    LEAVE(kind, rc);
}

int jfs_create(jfs_fs *fs, const char *path, uint32_t mode, int flags, const jfs_ctx *ctx, jfs_handle **out)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_META);
    rc = jfs_resolve(fs, path, &in);
    if (!rc) {
        if (flags & JFS_O_EXCL)
            rc = -JFS_EEXIST;
        else if (in->type == JFS_T_DIR)
            rc = -JFS_EISDIR;
        else if (flags & JFS_O_TRUNC)
            rc = do_truncate(fs, in, 0);
    } else if (rc == -JFS_ENOENT) {
        rc = make_node(fs, path, JFS_T_FILE, mode, ctx, &in);
    }
    if (!rc)
        *out = handle_new(fs, in, flags);
    LEAVE(OP_META, rc);
}

int64_t jfs_read(jfs_fs *fs, jfs_handle *h, void *buf, size_t size, uint64_t off)
{
    jfs_inode *in;
    int64_t rc;
    ENTER(OP_READ);
    in = h->ino;
    if (in->type == JFS_T_DIR) {
        rc = -JFS_EISDIR;
    } else if (in->type != JFS_T_FILE) {
        rc = -JFS_EINVAL;
    } else if ((rc = ensure_data(fs, in)) == 0) {
        if (off >= in->size) {
            rc = 0;
        } else {
            uint64_t n = in->size - off;
            if (n > size)
                n = size;
            memcpy(buf, in->data + off, (size_t)n);
            rc = (int64_t)n;
        }
    }
    LEAVE(OP_READ, rc);
}

int64_t jfs_write(jfs_fs *fs, jfs_handle *h, const void *buf, size_t size, uint64_t off)
{
    jfs_inode *in;
    int64_t rc;
    if ((h->flags & JFS_O_ACCMODE) == JFS_O_RDONLY)
        return -JFS_EBADF;
    ENTER(OP_DATA);
    in = h->ino;
    if (in->type != JFS_T_FILE) {
        rc = -JFS_EINVAL;
    } else if ((rc = ensure_data(fs, in)) == 0) {
        if (h->flags & JFS_O_APPEND)
            off = in->size;
        if (off + size < off || off + size > JFS_FILE_MAX) {
            rc = -JFS_EFBIG;
        } else {
            data_modified(fs, in);
            if (off + size > in->size)
                rc = resize_data(in, off + size);
            if (!rc) {
                if (size)
                    memcpy(in->data + off, buf, size);
                in->data_dirty = 1;
                touch_mc(in);
                /* POSIX: writing clears set-user-ID */
                in->mode &= ~(uint32_t)JFS_S_ISUID;
                rc = (int64_t)size;
            }
        }
    }
    LEAVE(OP_DATA, rc);
}

int jfs_flush(jfs_fs *fs, jfs_handle *h)
{
    int kind = fs->readonly ? OP_READ : OP_META;
    (void)h;
    ENTER(kind);
    LEAVE(kind, 0);
}

int jfs_fsync(jfs_fs *fs, jfs_handle *h)
{
    int rc = 0;
    int kind = fs->readonly ? OP_READ : OP_META;
    (void)h;
    ENTER(kind);
    if (fs->dirty && !fs->readonly)
        rc = jfs_commit_locked(fs);
    LEAVE(kind, rc);
}

int jfs_release(jfs_fs *fs, jfs_handle *h)
{
    jfs_inode *in;
    int kind = fs->readonly ? OP_READ : OP_META;
    jfs_mutex_lock(&fs->mu);
    if (fs->shared && fs->lock && kind == OP_META) {
        jfs_lockfile_lock(fs->lock, JFS_LOCK_TXN, 1, 1);
        jfs_maybe_reload(fs);
    }
    in = h->ino;
    if (h->prev)
        h->prev->next = h->next;
    else
        fs->handles = h->next;
    if (h->next)
        h->next->prev = h->prev;
    free(h);
    if (in->open_count > 0)
        in->open_count--;
    if (in->orphan) {
        jfs_orphan_release(fs, in);
    } else if (in->open_count == 0 && in->type == JFS_T_FILE && in->data_loaded && !in->data_dirty && in->blob) {
        /* drop the decoded cache of clean files */
        free(in->data);
        in->data = NULL;
        in->data_loaded = 0;
        in->data_cap = 0;
    }
    if (fs->shared && fs->lock && kind == OP_META) {
        int64_t rc = 0;
        if (fs->dirty) {
            rc = jfs_commit_locked(fs);
            if (rc)
                fs->need_reload = 1;
        }
        jfs_lockfile_unlock(fs->lock, JFS_LOCK_TXN);
        jfs_mutex_unlock(&fs->mu);
        return (int)rc;
    }
    if (fs->dirty && fs->sync_mode && !fs->readonly) {
        int rc = jfs_commit_locked(fs);
        jfs_mutex_unlock(&fs->mu);
        return rc;
    }
    jfs_mutex_unlock(&fs->mu);
    return 0;
}

/* ======================================================================== */
/* Directories / statfs                                                       */
/* ======================================================================== */
int jfs_readdir(jfs_fs *fs, const char *path, jfs_fill_fn fill, void *cookie)
{
    jfs_inode *in;
    int rc;
    ENTER(OP_READ);
    rc = jfs_resolve(fs, path, &in);
    if (!rc && in->type != JFS_T_DIR)
        rc = -JFS_ENOTDIR;
    if (!rc) {
        jfs_stat st;
        const char **keys;
        size_t i;
        fill_stat(fs, in, &st);
        if (fill(cookie, ".", &st) || fill(cookie, "..", &st))
            LEAVE(OP_READ, 0);
        keys = jfs_strmap_sorted_keys(in->entries);
        for (i = 0; keys[i]; i++) {
            uint64_t ino;
            jfs_inode *c;
            jfs_strmap_get(in->entries, keys[i], &ino);
            c = jfs_lookup_ino(fs, ino);
            if (!c)
                continue;
            fill_stat(fs, c, &st);
            if (fill(cookie, keys[i], &st))
                break;
        }
        free((void *)keys);
    }
    LEAVE(OP_READ, rc);
}

int jfs_statfs(jfs_fs *fs, jfs_statvfs *sv)
{
    uint64_t free_b = 0, total_b = 0, used = 0;
    size_t i;
    ENTER(OP_READ);
    memset(sv, 0, sizeof *sv);
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (in && !in->deleted && in->type == JFS_T_FILE)
            used += in->data_dirty ? in->size : in->blob_len;
        if (in)
            used += 256; /* metadata overhead estimate */
    }
    jfs_disk_space(fs->image_path, &free_b, &total_b);
    sv->bsize = 4096;
    sv->blocks = (used + free_b) / sv->bsize + 1;
    sv->bfree = sv->bavail = fs->readonly ? 0 : free_b / sv->bsize;
    sv->files = fs->inodes.count + (sv->bfree ? 1000000 : 0);
    sv->ffree = sv->bfree ? 1000000 : 0;
    sv->namemax = JFS_NAME_MAX;
    {
        uint64_t f = 0;
        const char *u = fs->uuid;
        while (*u)
            f = f * 131 + (unsigned char)*u++;
        sv->fsid = f;
    }
    sv->readonly = fs->readonly;
    LEAVE(OP_READ, 0);
}

/* ======================================================================== */
/* Extended attributes                                                        */
/* ======================================================================== */
static jfs_xattr *find_xattr(jfs_inode *in, const char *name)
{
    size_t i;
    for (i = 0; i < in->nxattrs; i++)
        if (!strcmp(in->xattrs[i].name, name))
            return &in->xattrs[i];
    return NULL;
}

int jfs_setxattr(jfs_fs *fs, const char *path, const char *name, const void *value, size_t size, int flags)
{
    jfs_inode *in;
    jfs_xattr *x;
    int rc;
    if (!name || !*name || strlen(name) > 255)
        return -JFS_ERANGE;
    if (size > 65536)
        return -JFS_ENOSPC;
    ENTER(OP_META);
    if (fs->schema < 2)
        LEAVE(OP_META, -JFS_ENOTSUP);
    rc = jfs_resolve(fs, path, &in);
    if (rc)
        LEAVE(OP_META, rc);
    x = find_xattr(in, name);
    if (x && (flags & JFS_XATTR_CREATE))
        LEAVE(OP_META, -JFS_EEXIST);
    if (!x && (flags & JFS_XATTR_REPLACE))
        LEAVE(OP_META, -JFS_ENOATTR);
    modified(fs, in);
    if (!x) {
        in->xattrs = jfs_realloc(in->xattrs, (in->nxattrs + 1) * sizeof(jfs_xattr));
        x = &in->xattrs[in->nxattrs++];
        x->name = jfs_strdup(name);
        x->val = NULL;
    }
    free(x->val);
    x->val = jfs_malloc(size);
    if (size)
        memcpy(x->val, value, size);
    x->len = size;
    in->ctime = jfs_now();
    LEAVE(OP_META, 0);
}

int64_t jfs_getxattr(jfs_fs *fs, const char *path, const char *name, void *value, size_t size)
{
    jfs_inode *in;
    jfs_xattr *x;
    int64_t rc;
    ENTER(OP_READ);
    if (fs->schema < 2)
        LEAVE(OP_READ, -JFS_ENOTSUP);
    rc = jfs_resolve(fs, path, &in);
    if (rc)
        LEAVE(OP_READ, rc);
    x = find_xattr(in, name);
    if (!x)
        rc = -JFS_ENOATTR;
    else if (size == 0)
        rc = (int64_t)x->len;
    else if (size < x->len)
        rc = -JFS_ERANGE;
    else {
        if (x->len)
            memcpy(value, x->val, x->len);
        rc = (int64_t)x->len;
    }
    LEAVE(OP_READ, rc);
}

int64_t jfs_listxattr(jfs_fs *fs, const char *path, char *list, size_t size)
{
    jfs_inode *in;
    int64_t rc;
    size_t i, need = 0;
    ENTER(OP_READ);
    if (fs->schema < 2)
        LEAVE(OP_READ, 0);
    rc = jfs_resolve(fs, path, &in);
    if (rc)
        LEAVE(OP_READ, rc);
    for (i = 0; i < in->nxattrs; i++)
        need += strlen(in->xattrs[i].name) + 1;
    if (size == 0)
        LEAVE(OP_READ, (int64_t)need);
    if (size < need)
        LEAVE(OP_READ, -JFS_ERANGE);
    for (i = 0; i < in->nxattrs; i++) {
        size_t l = strlen(in->xattrs[i].name) + 1;
        memcpy(list, in->xattrs[i].name, l);
        list += l;
    }
    LEAVE(OP_READ, (int64_t)need);
}

int jfs_removexattr(jfs_fs *fs, const char *path, const char *name)
{
    jfs_inode *in;
    jfs_xattr *x;
    int rc;
    ENTER(OP_META);
    if (fs->schema < 2)
        LEAVE(OP_META, -JFS_ENOTSUP);
    rc = jfs_resolve(fs, path, &in);
    if (rc)
        LEAVE(OP_META, rc);
    x = find_xattr(in, name);
    if (!x)
        LEAVE(OP_META, -JFS_ENOATTR);
    modified(fs, in);
    x = find_xattr(in, name); /* modified() does not move xattrs, but be safe */
    free(x->name);
    free(x->val);
    *x = in->xattrs[--in->nxattrs];
    in->ctime = jfs_now();
    LEAVE(OP_META, 0);
}

/* ======================================================================== */
/* Replay of remote changes (see jfs_replay_begin in jfs_fs.h)                */
/* These run for system calls the mount driver issues on its own mount point; */
/* they copy state from the pending version and never version, dirty or       */
/* commit anything: jfs_pending_install() makes the state exact afterwards.  */
/* ======================================================================== */
static void replay_unbind(jfs_fs *fs, jfs_inode *dir, const char *name, jfs_inode *child)
{
    jfs_strmap_del(dir->entries, name);
    if (child->type == JFS_T_DIR) {
        if (dir->nlink > 2)
            dir->nlink--;
    } else if (child->nlink) {
        child->nlink--;
    }
    if (child->type == JFS_T_DIR || child->nlink == 0) {
        jfs_u64map_del(&fs->inodes, child->ino);
        if (child->open_count > 0) {
            child->persisted = 0;
            jfs_orphan_add(fs, child);
        } else {
            jfs_inode_free(child);
        }
    }
}

int jfs_replay_create(jfs_fs *fs, const char *path)
{
    jfs_inode *dir, *t, *existing, *c;
    char name[JFS_NAME_MAX + 1];
    int rc;
    jfs_mutex_lock(&fs->mu);
    if (!fs->replay_target) {
        rc = -JFS_EROFS;
        goto out;
    }
    rc = jfs_resolve(fs->replay_target, path, &t);
    if (!rc)
        rc = resolve_parent(fs, path, &dir, name);
    if (rc)
        goto out;
    if (dir_lookup(fs, dir, name)) {
        rc = -JFS_EEXIST;
        goto out;
    }
    existing = jfs_u64map_get(&fs->inodes, t->ino);
    if (existing && !existing->deleted) {
        if (existing->type == JFS_T_DIR || existing->type != t->type) {
            rc = -JFS_EEXIST;
            goto out;
        }
        c = existing; /* another name of an existing file */
    } else {
        if (existing) {
            jfs_u64map_del(&fs->inodes, existing->ino);
            jfs_inode_free(existing);
        }
        c = jfs_inode_clone(t);
        c->nlink = t->type == JFS_T_DIR ? 2 : 0;
        jfs_u64map_put(&fs->inodes, c->ino, c);
    }
    jfs_strmap_put(dir->entries, name, c->ino);
    if (c->type == JFS_T_DIR)
        dir->nlink++;
    else
        c->nlink++;
out:
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

int jfs_replay_link(jfs_fs *fs, const char *existing, const char *path)
{
    jfs_inode *src, *dir;
    char name[JFS_NAME_MAX + 1];
    int rc;
    jfs_mutex_lock(&fs->mu);
    rc = fs->replay_target ? jfs_resolve(fs, existing, &src) : -JFS_EROFS;
    if (!rc && src->type == JFS_T_DIR)
        rc = -JFS_EPERM;
    if (!rc)
        rc = resolve_parent(fs, path, &dir, name);
    if (!rc && dir_lookup(fs, dir, name))
        rc = -JFS_EEXIST;
    if (!rc) {
        jfs_strmap_put(dir->entries, name, src->ino);
        src->nlink++;
    }
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

int jfs_replay_remove(jfs_fs *fs, const char *path)
{
    jfs_inode *dir, *c;
    char name[JFS_NAME_MAX + 1];
    int rc;
    jfs_mutex_lock(&fs->mu);
    rc = fs->replay_target ? resolve_parent(fs, path, &dir, name) : -JFS_EROFS;
    if (!rc && !(c = dir_lookup(fs, dir, name)))
        rc = -JFS_ENOENT;
    if (!rc && c->type == JFS_T_DIR && c->entries->count)
        rc = -JFS_ENOTEMPTY;
    if (!rc)
        replay_unbind(fs, dir, name, c);
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

int jfs_replay_rename(jfs_fs *fs, const char *from, const char *to)
{
    jfs_inode *sdir, *ddir, *src, *dst;
    char sname[JFS_NAME_MAX + 1], dname[JFS_NAME_MAX + 1];
    int rc;
    jfs_mutex_lock(&fs->mu);
    rc = fs->replay_target ? resolve_parent(fs, from, &sdir, sname) : -JFS_EROFS;
    if (!rc)
        rc = resolve_parent(fs, to, &ddir, dname);
    if (!rc && !(src = dir_lookup(fs, sdir, sname)))
        rc = -JFS_ENOENT;
    if (rc)
        goto out;
    dst = dir_lookup(fs, ddir, dname);
    if (dst == src)
        goto out;
    if (dst) {
        if (dst->type == JFS_T_DIR && dst->entries->count) {
            rc = -JFS_ENOTEMPTY;
            goto out;
        }
        replay_unbind(fs, ddir, dname, dst);
    }
    jfs_strmap_del(sdir->entries, sname);
    jfs_strmap_put(ddir->entries, dname, src->ino);
    if (src->type == JFS_T_DIR && sdir != ddir) {
        if (sdir->nlink > 2)
            sdir->nlink--;
        ddir->nlink++;
    }
out:
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

int jfs_replay_update(jfs_fs *fs, const char *path)
{
    jfs_inode *t, *c;
    int rc;
    jfs_mutex_lock(&fs->mu);
    rc = fs->replay_target ? jfs_resolve(fs->replay_target, path, &t) : -JFS_EROFS;
    if (!rc)
        rc = jfs_resolve(fs, path, &c);
    if (!rc && c->type != t->type)
        rc = -JFS_EINVAL;
    if (!rc)
        jfs_inode_copy_state(c, t);
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

int jfs_replay_update_handle(jfs_fs *fs, jfs_handle *h)
{
    jfs_inode *t;
    int rc = 0;
    jfs_mutex_lock(&fs->mu);
    if (!fs->replay_target)
        rc = -JFS_EROFS;
    else if (!(t = jfs_lookup_ino(fs->replay_target, h->ino->ino)) || t->type != h->ino->type)
        rc = -JFS_ENOENT;
    else
        jfs_inode_copy_state(h->ino, t);
    jfs_mutex_unlock(&fs->mu);
    return rc;
}
