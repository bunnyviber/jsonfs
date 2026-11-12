/*
 * JSONfs - image model: parsing, serialization, commits, locking,
 * multi-attach reloads and the time-travel view.
 * SPDX-License-Identifier: MIT
 */
#include "jfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* Types                                                                      */
/* ======================================================================== */
static const char *type_names[] = {"none", "file", "directory", "symlink", "fifo", "socket", "chardev", "blockdev"};

const char *jfs_type_name(jfs_type t)
{
    return (t >= JFS_T_NONE && t <= JFS_T_BLK) ? type_names[t] : "none";
}

jfs_type jfs_type_from_name(const char *s)
{
    int i;
    if (!s)
        return JFS_T_NONE;
    for (i = 1; i <= JFS_T_BLK; i++)
        if (!strcmp(s, type_names[i]))
            return (jfs_type)i;
    return JFS_T_NONE;
}

uint32_t jfs_type_mode_bits(jfs_type t)
{
    switch (t) {
    case JFS_T_FILE: return JFS_S_IFREG;
    case JFS_T_DIR: return JFS_S_IFDIR;
    case JFS_T_SYMLINK: return JFS_S_IFLNK;
    case JFS_T_FIFO: return JFS_S_IFIFO;
    case JFS_T_SOCK: return JFS_S_IFSOCK;
    case JFS_T_CHR: return JFS_S_IFCHR;
    case JFS_T_BLK: return JFS_S_IFBLK;
    default: return 0;
    }
}

/* ======================================================================== */
/* Inodes                                                                     */
/* ======================================================================== */
jfs_inode *jfs_inode_new(jfs_fs *fs, jfs_type t)
{
    jfs_inode *in = jfs_calloc(1, sizeof *in);
    jfs_time now = jfs_now();
    in->type = t;
    in->atime = in->mtime = in->ctime = in->btime = in->vtime = now;
    if (fs) {
        in->ino = fs->next_ino++;
        jfs_u64map_put(&fs->inodes, in->ino, in);
        fs->dirty = 1;
    }
    in->version_open = 1; /* brand new: nothing to preserve */
    if (t == JFS_T_DIR) {
        in->entries = jfs_calloc(1, sizeof(jfs_strmap));
        jfs_strmap_init(in->entries);
    }
    if (t == JFS_T_FILE) {
        in->data_loaded = 1;
        in->data_dirty = 1;
    }
    return in;
}

static void free_xattrs(jfs_inode *in)
{
    size_t i;
    for (i = 0; i < in->nxattrs; i++) {
        free(in->xattrs[i].name);
        free(in->xattrs[i].val);
    }
    free(in->xattrs);
    in->xattrs = NULL;
    in->nxattrs = 0;
}

void jfs_inode_free(jfs_inode *in)
{
    if (!in)
        return;
    free(in->comp);
    free(in->blob);
    free(in->data);
    free(in->target);
    if (in->entries) {
        jfs_strmap_free(in->entries);
        free(in->entries);
    }
    free_xattrs(in);
    jv_free(in->history);
    free(in);
}

static int parse_mode(const jv *v, uint32_t *mode)
{
    if (!v)
        return -1;
    if (v->t == JV_STR) {
        char *e;
        unsigned long m = strtoul(v->s, &e, 8);
        if (*e || e == v->s)
            return -1;
        *mode = (uint32_t)m & 07777;
        return 0;
    }
    if (v->t == JV_NUM && v->is_int) {
        *mode = (uint32_t)v->i & 07777;
        return 0;
    }
    return -1;
}

static int get_time_field(const jv *rec, const char *key, jfs_time *out)
{
    const char *s = jv_get_str(rec, key);
    jv *v;
    if (s)
        return jfs_time_parse(s, out);
    v = jv_get(rec, key);
    if (v && v->t == JV_NUM) { /* tolerate numeric epoch seconds */
        out->sec = (int64_t)v->d;
        out->nsec = (int32_t)((v->d - (double)out->sec) * 1e9);
        return 0;
    }
    return -1;
}

jfs_inode *jfs_inode_from_record(jv *rec, uint64_t ino, int schema, int take, char *err, size_t errlen)
{
    jfs_inode *in;
    int64_t i64;
    jv *v;
    if (!rec || rec->t != JV_OBJ) {
        snprintf(err, errlen, "inode %llu: record is not an object", (unsigned long long)ino);
        return NULL;
    }
    in = jfs_calloc(1, sizeof *in);
    in->ino = ino;
    in->type = jfs_type_from_name(jv_get_str(rec, "type"));
    in->deleted = jv_get_bool(rec, "deleted", 0);
    if (get_time_field(rec, "ctime", &in->ctime))
        in->ctime.sec = 0;
    if (get_time_field(rec, "version_time", &in->vtime))
        in->vtime = in->ctime;
    in->persisted = 1;
    if (in->deleted)
        return in;
    if (in->type == JFS_T_NONE) {
        snprintf(err, errlen, "inode %llu: missing or unknown \"type\"", (unsigned long long)ino);
        goto bad;
    }
    if (parse_mode(jv_get(rec, "mode"), &in->mode)) {
        snprintf(err, errlen, "inode %llu: bad \"mode\"", (unsigned long long)ino);
        goto bad;
    }
    if (!jv_get_i64(rec, "uid", &i64))
        in->uid = (uint32_t)i64;
    if (!jv_get_i64(rec, "gid", &i64))
        in->gid = (uint32_t)i64;
    get_time_field(rec, "atime", &in->atime);
    get_time_field(rec, "mtime", &in->mtime);
    if (get_time_field(rec, "btime", &in->btime) || schema < 2)
        in->btime = in->ctime;
    if (!jv_get_i64(rec, "rdev", &i64))
        in->rdev = (uint64_t)i64;
    switch (in->type) {
    case JFS_T_FILE: {
        const char *comp = jv_get_str(rec, "compression");
        const char *sha = jv_get_str(rec, "sha256");
        if (jv_get_i64(rec, "size", &i64) || i64 < 0) {
            snprintf(err, errlen, "inode %llu: bad \"size\"", (unsigned long long)ino);
            goto bad;
        }
        in->size = (uint64_t)i64;
        in->comp = jfs_strdup(comp ? comp : "none");
        if (sha && !jfs_unhex(sha, in->sha, 32))
            in->sha_valid = 1;
        v = jv_get(rec, "data");
        if (v && v->t == JV_STR) {
            if (take) {
                in->blob = jv_take_str(rec, "data", &in->blob_len);
            } else {
                in->blob = jfs_strndup(v->s, v->slen);
                in->blob_len = v->slen;
            }
        } else if (jv_get_str(rec, "data_ref")) {
            in->blob_ref = 1; /* content lives in a newer record */
        } else if (in->size) {
            snprintf(err, errlen, "inode %llu: missing \"data\"", (unsigned long long)ino);
            goto bad;
        } else {
            in->blob = jfs_strdup("");
        }
        break;
    }
    case JFS_T_SYMLINK: {
        const char *t = jv_get_str(rec, "target");
        if (!t) {
            snprintf(err, errlen, "inode %llu: symlink without \"target\"", (unsigned long long)ino);
            goto bad;
        }
        in->target = jfs_strdup(t);
        in->size = strlen(t);
        break;
    }
    case JFS_T_DIR: {
        size_t i;
        v = jv_get(rec, "entries");
        in->entries = jfs_calloc(1, sizeof(jfs_strmap));
        jfs_strmap_init(in->entries);
        if (v && v->t != JV_OBJ) {
            snprintf(err, errlen, "inode %llu: \"entries\" is not an object", (unsigned long long)ino);
            goto bad;
        }
        for (i = 0; v && i < v->n; i++) {
            jv *e = v->items[i];
            if (e->t != JV_NUM || !e->is_int || e->i <= 0) {
                snprintf(err, errlen, "inode %llu: entry \"%s\" is not an inode number", (unsigned long long)ino,
                         v->keys[i]);
                goto bad;
            }
            jfs_strmap_put(in->entries, v->keys[i], (uint64_t)e->i);
        }
        break;
    }
    default: break;
    }
    v = jv_get(rec, "xattrs");
    if (v && v->t == JV_OBJ && schema >= 2) {
        size_t i;
        in->xattrs = jfs_calloc(v->n ? v->n : 1, sizeof(jfs_xattr));
        for (i = 0; i < v->n; i++) {
            uint8_t *val;
            size_t len;
            if (v->items[i]->t != JV_STR || jfs_b64_decode(v->items[i]->s, v->items[i]->slen, &val, &len)) {
                snprintf(err, errlen, "inode %llu: bad xattr \"%s\"", (unsigned long long)ino, v->keys[i]);
                goto bad;
            }
            in->xattrs[in->nxattrs].name = jfs_strdup(v->keys[i]);
            in->xattrs[in->nxattrs].val = val;
            in->xattrs[in->nxattrs].len = len;
            in->nxattrs++;
        }
    }
    return in;
bad:
    jfs_inode_free(in);
    return NULL;
}

static jv *time_jv(jfs_time t)
{
    char buf[JFS_TIME_STRLEN];
    jfs_time_format(t, buf);
    return jv_str(buf);
}

jv *jfs_inode_to_record(jfs_fs *fs, jfs_inode *in)
{
    jv *r = jv_obj();
    char mode[16];
    jv_obj_set(r, "type", jv_str(jfs_type_name(in->type)));
    if (in->deleted) {
        jv_obj_set(r, "deleted", jv_bool(1));
        jv_obj_set(r, "version_time", time_jv(in->vtime));
        return r;
    }
    if (in->type == JFS_T_FILE && in->data_dirty)
        jfs_inode_encode(fs, in, fs->write_comp);
    snprintf(mode, sizeof mode, "%04o", (unsigned)(in->mode & 07777));
    jv_obj_set(r, "mode", jv_str(mode));
    jv_obj_set(r, "uid", jv_int(in->uid));
    jv_obj_set(r, "gid", jv_int(in->gid));
    jv_obj_set(r, "nlink", jv_int(in->nlink));
    if (in->type == JFS_T_DIR)
        jv_obj_set(r, "size", jv_int((int64_t)(in->entries ? in->entries->count : 0)));
    else
        jv_obj_set(r, "size", jv_int((int64_t)in->size));
    if (in->type == JFS_T_CHR || in->type == JFS_T_BLK)
        jv_obj_set(r, "rdev", jv_int((int64_t)in->rdev));
    jv_obj_set(r, "atime", time_jv(in->atime));
    jv_obj_set(r, "mtime", time_jv(in->mtime));
    jv_obj_set(r, "ctime", time_jv(in->ctime));
    if (fs->schema >= 2)
        jv_obj_set(r, "btime", time_jv(in->btime));
    jv_obj_set(r, "version_time", time_jv(in->vtime));
    if (in->type == JFS_T_FILE) {
        char hex[65];
        jfs_hex(in->sha, 32, hex);
        jv_obj_set(r, "compression", jv_str(in->comp ? in->comp : "none"));
        jv_obj_set(r, "sha256", jv_str(hex));
        jv_obj_set(r, "data", jv_strn(in->blob ? in->blob : "", in->blob ? in->blob_len : 0));
    } else if (in->type == JFS_T_SYMLINK) {
        jv_obj_set(r, "target", jv_str(in->target ? in->target : ""));
    } else if (in->type == JFS_T_DIR) {
        jv *e = jv_obj();
        const char **keys = jfs_strmap_sorted_keys(in->entries);
        size_t i;
        for (i = 0; keys[i]; i++) {
            uint64_t ino = 0;
            jfs_strmap_get(in->entries, keys[i], &ino);
            jv_obj_set(e, keys[i], jv_int((int64_t)ino));
        }
        free((void *)keys);
        jv_obj_set(r, "entries", e);
    }
    if (fs->schema >= 2 && in->nxattrs) {
        jv *x = jv_obj();
        size_t i;
        for (i = 0; i < in->nxattrs; i++) {
            char *b = jfs_malloc(jfs_b64_enc_len(in->xattrs[i].len) + 1);
            size_t bl = jfs_b64_encode(in->xattrs[i].val, in->xattrs[i].len, b);
            jv_obj_set(x, in->xattrs[i].name, jv_str_owned(b, bl));
        }
        jv_obj_set(r, "xattrs", x);
    }
    return r;
}

int jfs_inode_load_data(jfs_fs *fs, jfs_inode *in)
{
    int rc;
    (void)fs;
    if (in->type != JFS_T_FILE)
        return -JFS_EINVAL;
    if (in->data_loaded)
        return 0;
    rc = jfs_decode_blob(in->comp, in->blob, in->blob_len, in->size, in->sha_valid ? in->sha : NULL, &in->data);
    if (rc) {
        jfs_log("inode %llu: cannot decode content (%s, compression=%s)", (unsigned long long)in->ino,
                jfs_strerror(rc), in->comp ? in->comp : "?");
        return rc == -JFS_ENOTSUP ? -JFS_EIO : (rc == -JFS_ENOMEM ? rc : -JFS_EIO);
    }
    in->data_cap = (size_t)in->size + 1;
    in->data_loaded = 1;
    return 0;
}

int jfs_inode_encode(jfs_fs *fs, jfs_inode *in, const char *algo_spec)
{
    char *b64, *name;
    size_t blen;
    int rc;
    if (in->type != JFS_T_FILE)
        return 0;
    if (!in->data_loaded) {
        rc = jfs_inode_load_data(fs, in);
        if (rc)
            return rc;
    }
    rc = jfs_encode_blob(algo_spec, in->data, (size_t)in->size, &b64, &blen, &name);
    if (rc)
        return rc;
    free(in->blob);
    free(in->comp);
    in->blob = b64;
    in->blob_len = blen;
    in->comp = name;
    jfs_sha256_buf(in->data, (size_t)in->size, in->sha);
    in->sha_valid = 1;
    in->data_dirty = 0;
    return 0;
}

jfs_inode *jfs_lookup_ino(jfs_fs *fs, uint64_t ino)
{
    jfs_inode *in = jfs_u64map_get(&fs->inodes, ino);
    return (in && !in->deleted) ? in : NULL;
}

void jfs_orphan_add(jfs_fs *fs, jfs_inode *in)
{
    if (fs->norphans == fs->orphans_cap) {
        fs->orphans_cap = fs->orphans_cap ? fs->orphans_cap * 2 : 8;
        fs->orphans = jfs_realloc(fs->orphans, fs->orphans_cap * sizeof(jfs_inode *));
    }
    in->orphan = 1;
    in->nlink = 0;
    fs->orphans[fs->norphans++] = in;
}

void jfs_orphan_release(jfs_fs *fs, jfs_inode *in)
{
    size_t i;
    if (!in->orphan || in->open_count > 0)
        return;
    for (i = 0; i < fs->norphans; i++) {
        if (fs->orphans[i] == in) {
            fs->orphans[i] = fs->orphans[--fs->norphans];
            break;
        }
    }
    jfs_inode_free(in);
}

/* ======================================================================== */
/* fs object                                                                  */
/* ======================================================================== */
int jfs_fs_create_empty(jfs_fs **out)
{
    jfs_fs *fs = jfs_calloc(1, sizeof *fs);
    jfs_u64map_init(&fs->inodes);
    jfs_mutex_init(&fs->mu);
    fs->schema = JFS_SCHEMA_LATEST;
    fs->next_ino = 2;
    fs->root_ino = 1;
    fs->default_comp = jfs_strdup(JFS_DEFAULT_COMPRESSION);
    fs->write_comp = jfs_strdup(JFS_DEFAULT_COMPRESSION);
    fs->label = jfs_strdup("");
    fs->commit_interval_ms = 5000;
    *out = fs;
    return 0;
}

static void free_inode_table(jfs_u64map *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++)
        if (m->vals[i])
            jfs_inode_free(m->vals[i]);
    jfs_u64map_free(m);
}

void jfs_fs_free(jfs_fs *fs)
{
    size_t i;
    jfs_handle *h, *n;
    if (!fs)
        return;
    free_inode_table(&fs->inodes);
    for (i = 0; i < fs->norphans; i++)
        jfs_inode_free(fs->orphans[i]);
    free(fs->orphans);
    for (h = fs->handles; h; h = n) {
        n = h->next;
        free(h);
    }
    jv_free(fs->extra_header);
    free(fs->image_path);
    free(fs->label);
    free(fs->default_comp);
    free(fs->write_comp);
    jfs_mutex_destroy(&fs->mu);
    free(fs);
}

void jfs_recount_links(jfs_fs *fs)
{
    size_t i;
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (in)
            in->nlink = (in->type == JFS_T_DIR) ? 2 : 0;
    }
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *d = fs->inodes.vals[i];
        size_t k;
        if (!d || d->deleted || d->type != JFS_T_DIR)
            continue;
        for (k = 0; k < d->entries->cap; k++) {
            jfs_inode *c;
            if (!d->entries->keys[k])
                continue;
            c = jfs_lookup_ino(fs, d->entries->vals[k]);
            if (!c)
                continue;
            if (c->type == JFS_T_DIR)
                d->nlink++; /* child's ".." */
            else
                c->nlink++;
        }
    }
}

/* Choose the record valid at `at` among history + current. */
static jv *record_at(jv *cur, jv *hist, jfs_time at)
{
    jv *best = NULL;
    jfs_time bt = {0, 0};
    size_t i, n = hist && hist->t == JV_ARR ? hist->n : 0;
    for (i = 0; i <= n; i++) {
        jv *r = i < n ? hist->items[i] : cur;
        jfs_time t;
        if (!r || r->t != JV_OBJ)
            continue;
        if (get_time_field(r, "version_time", &t) && get_time_field(r, "ctime", &t))
            continue;
        if (jfs_time_cmp(t, at) <= 0 && (!best || jfs_time_cmp(t, bt) >= 0)) {
            best = r;
            bt = t;
        }
    }
    return best;
}

jv *jfs_record_data_source(jv *cur, jv *hist, jv *rec)
{
    const char *sha;
    size_t i, n = hist && hist->t == JV_ARR ? hist->n : 0, start = n + 1;
    if (!rec || jv_get(rec, "data") || !jv_get_str(rec, "data_ref"))
        return rec;
    sha = jv_get_str(rec, "sha256");
    if (!sha)
        return NULL;
    for (i = 0; i < n; i++)
        if (hist->items[i] == rec)
            start = i + 1;
    if (rec == cur)
        return NULL;
    if (start > n)
        start = 0;
    for (i = start; i <= n; i++) {
        jv *r = i < n ? hist->items[i] : cur;
        const char *s2 = jv_get_str(r, "sha256");
        jv *d = jv_get(r, "data");
        if (s2 && !strcmp(s2, sha) && d && d->t == JV_STR)
            return r;
    }
    return NULL;
}

int jfs_fs_load_json(jfs_fs *fs, jv *root, const jfs_time *at, char *err, size_t errlen)
{
    const char *s;
    int64_t i64;
    jv *inodes;
    size_t i;
    char lerr[256];

    if (!root || root->t != JV_OBJ) {
        snprintf(err, errlen, "image is not a JSON object");
        return -JFS_ECORRUPT;
    }
    s = jv_get_str(root, "format");
    if (!s || strcmp(s, JFS_FORMAT_NAME)) {
        snprintf(err, errlen, "not a JSONfs image (missing \"format\": \"jsonfs\")");
        return -JFS_ECORRUPT;
    }
    if (jv_get_i64(root, "schema_version", &i64)) {
        snprintf(err, errlen, "missing \"schema_version\"");
        return -JFS_ECORRUPT;
    }
    if (i64 < JFS_SCHEMA_MIN || i64 > JFS_SCHEMA_LATEST) {
        snprintf(err, errlen, "schema version %lld is not supported by this build (supports %d..%d)", (long long)i64,
                 JFS_SCHEMA_MIN, JFS_SCHEMA_LATEST);
        return -JFS_EVERSION;
    }
    fs->schema = (int)i64;
    s = jv_get_str(root, "uuid");
    if (!jfs_uuid_valid(s)) {
        snprintf(err, errlen, "missing or invalid \"uuid\"");
        return -JFS_ECORRUPT;
    }
    strcpy(fs->uuid, s);
    free(fs->label);
    fs->label = jfs_strdup(jv_get_str(root, "label") ? jv_get_str(root, "label") : "");
    if (get_time_field(root, "created", &fs->created) || get_time_field(root, "modified", &fs->modified)) {
        snprintf(err, errlen, "missing or invalid \"created\"/\"modified\" timestamps");
        return -JFS_ECORRUPT;
    }
    fs->generation = jv_get_i64(root, "generation", &i64) ? 0 : (uint64_t)i64;
    s = jv_get_str(root, "default_compression");
    if (!s || !jfs_compressor_find(s)) {
        snprintf(err, errlen, "unknown default compression \"%s\"", s ? s : "(missing)");
        return -JFS_ECORRUPT;
    }
    free(fs->default_comp);
    fs->default_comp = jfs_strdup(s);
    fs->versioning = jv_get_bool(root, "versioning", 0);
    if (jv_get_i64(root, "root_inode", &i64) || i64 <= 0) {
        snprintf(err, errlen, "missing \"root_inode\"");
        return -JFS_ECORRUPT;
    }
    fs->root_ino = (uint64_t)i64;
    fs->next_ino = jv_get_i64(root, "next_inode", &i64) ? 2 : (uint64_t)i64;

    /* keep unknown top-level fields */
    jv_free(fs->extra_header);
    fs->extra_header = jv_obj();
    for (i = 0; i < root->n; i++) {
        static const char *known[] = {"format",   "schema_version", "uuid",       "label",      "created",
                                      "modified", "generation",     "features",   "default_compression",
                                      "versioning", "next_inode",   "root_inode", "inodes",     NULL};
        int k, found = 0;
        for (k = 0; known[k]; k++)
            if (!strcmp(root->keys[i], known[k]))
                found = 1;
        if (!found)
            jv_obj_set(fs->extra_header, root->keys[i], jv_clone(root->items[i]));
    }

    inodes = jv_get(root, "inodes");
    if (!inodes || inodes->t != JV_OBJ) {
        snprintf(err, errlen, "missing \"inodes\" object");
        return -JFS_ECORRUPT;
    }
    for (i = 0; i < inodes->n; i++) {
        char *e;
        unsigned long long ino = strtoull(inodes->keys[i], &e, 10);
        jv *cur = inodes->items[i], *hist, *rec;
        jfs_inode *in;
        if (*e || !ino) {
            snprintf(err, errlen, "bad inode key \"%s\"", inodes->keys[i]);
            if (fs->repair)
                continue;
            return -JFS_ECORRUPT;
        }
        hist = jv_get(cur, "history");
        if (at) {
            rec = record_at(cur, hist, *at);
            if (!rec || jv_get_bool(rec, "deleted", 0))
                continue;
        } else {
            rec = cur;
        }
        lerr[0] = 0;
        in = jfs_inode_from_record(rec, ino, fs->schema, 1, lerr, sizeof lerr);
        if (!in) {
            if (fs->repair) {
                jfs_log("repair: dropping inode %llu (%s)", ino, lerr);
                continue;
            }
            snprintf(err, errlen, "%s", lerr);
            return -JFS_ECORRUPT;
        }
        if (in->blob_ref) {
            jv *src = jfs_record_data_source(cur, hist, rec);
            jv *d = src ? jv_get(src, "data") : NULL;
            const char *c = src ? jv_get_str(src, "compression") : NULL;
            if (!at || !d || d->t != JV_STR || !c) {
                snprintf(err, errlen, "inode %llu: content reference cannot be resolved", ino);
                jfs_inode_free(in);
                if (fs->repair)
                    continue;
                return -JFS_ECORRUPT;
            }
            in->blob = jfs_strndup(d->s, d->slen);
            in->blob_len = d->slen;
            free(in->comp);
            in->comp = jfs_strdup(c);
            in->blob_ref = 0;
        }
        if (at) {
            in->persisted = 0;
        } else if (hist && hist->t == JV_ARR && hist->n) {
            /* detach the history array from the DOM */
            size_t k;
            for (k = 0; k < cur->n; k++) {
                if (!strcmp(cur->keys[k], "history")) {
                    in->history = cur->items[k];
                    cur->items[k] = jv_null();
                }
            }
            if (in->type == JFS_T_FILE && !jv_get(in->history->items[in->history->n - 1], "data"))
                in->hist_needs_data = 1;
        }
        if (ino >= fs->next_ino)
            fs->next_ino = ino + 1;
        if (jfs_u64map_get(&fs->inodes, ino)) {
            jfs_inode_free(in);
            snprintf(err, errlen, "duplicate inode %llu", ino);
            if (fs->repair)
                continue;
            return -JFS_ECORRUPT;
        }
        jfs_u64map_put(&fs->inodes, ino, in);
    }
    {
        jfs_inode *r = jfs_lookup_ino(fs, fs->root_ino);
        if (!r || r->type != JFS_T_DIR) {
            if (at) {
                snprintf(err, errlen, "the filesystem did not exist yet at the requested time");
                return -JFS_ENOENT;
            }
            snprintf(err, errlen, "root inode %llu missing or not a directory", (unsigned long long)fs->root_ino);
            return -JFS_ECORRUPT;
        }
    }
    /* drop dangling directory entries (they can legitimately appear in time
     * views when a directory record predates the creation of a child) */
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *d = fs->inodes.vals[i];
        size_t k;
        if (!d || d->deleted || d->type != JFS_T_DIR)
            continue;
        for (k = 0; k < d->entries->cap;) {
            const char *name = d->entries->keys[k];
            if (name && !jfs_lookup_ino(fs, d->entries->vals[k])) {
                if (!at)
                    jfs_log("warning: %s: dangling entry \"%s\" -> %llu in directory %llu dropped",
                            fs->image_path ? fs->image_path : "image", name,
                            (unsigned long long)d->entries->vals[k], (unsigned long long)d->ino);
                jfs_strmap_del(d->entries, name);
                if (!at && fs->repair)
                    fs->dirty = 1;
                continue; /* re-examine slot k (backward shift) */
            }
            k++;
        }
    }
    jfs_recount_links(fs);
    if (at) {
        fs->is_view = 1;
        fs->view_time = *at;
    }
    return 0;
}

/* ======================================================================== */
/* Serialization                                                              */
/* ======================================================================== */
static int cmp_u64p(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int jfs_serialize(jfs_fs *fs, jfs_buf *out)
{
    char tbuf[JFS_TIME_STRLEN];
    uint64_t *inos;
    size_t n = 0, i;
    jfs_buf_puts(out, "{\n  \"format\": \"jsonfs\",\n");
    jfs_buf_printf(out, "  \"schema_version\": %d,\n", fs->schema);
    jfs_buf_printf(out, "  \"uuid\": \"%s\",\n  \"label\": ", fs->uuid);
    jv_write_string(out, fs->label, strlen(fs->label));
    jfs_time_format(fs->created, tbuf);
    jfs_buf_printf(out, ",\n  \"created\": \"%s\",\n", tbuf);
    jfs_time_format(fs->modified, tbuf);
    jfs_buf_printf(out, "  \"modified\": \"%s\",\n", tbuf);
    if (fs->schema >= 2) {
        jfs_buf_printf(out, "  \"generation\": %llu,\n", (unsigned long long)fs->generation);
        jfs_buf_puts(out, "  \"features\": [\"xattr\", \"multi-attach\", \"btime\"],\n");
    }
    jfs_buf_printf(out, "  \"default_compression\": \"%s\",\n", fs->default_comp);
    jfs_buf_printf(out, "  \"versioning\": %s,\n", fs->versioning ? "true" : "false");
    jfs_buf_printf(out, "  \"next_inode\": %llu,\n", (unsigned long long)fs->next_ino);
    jfs_buf_printf(out, "  \"root_inode\": %llu,\n", (unsigned long long)fs->root_ino);
    if (fs->extra_header) {
        for (i = 0; i < fs->extra_header->n; i++) {
            jfs_buf_puts(out, "  ");
            jv_write_string(out, fs->extra_header->keys[i], strlen(fs->extra_header->keys[i]));
            jfs_buf_puts(out, ": ");
            jv_write_level(out, fs->extra_header->items[i], 2, 1);
            jfs_buf_puts(out, ",\n");
        }
    }
    jfs_buf_puts(out, "  \"inodes\": {");
    inos = jfs_malloc((fs->inodes.count + 1) * sizeof(uint64_t));
    for (i = 0; i < fs->inodes.cap; i++)
        if (fs->inodes.vals[i])
            inos[n++] = fs->inodes.keys[i];
    qsort(inos, n, sizeof(uint64_t), cmp_u64p);
    for (i = 0; i < n; i++) {
        jfs_inode *in = jfs_u64map_get(&fs->inodes, inos[i]);
        jv *rec = jfs_inode_to_record(fs, in);
        if (in->history && in->history->t == JV_ARR && in->history->n) {
            jv_obj_set(rec, "history", in->history);
        }
        jfs_buf_printf(out, "%s\n    \"%llu\": ", i ? "," : "", (unsigned long long)inos[i]);
        jv_write_level(out, rec, 2, 2);
        /* detach history again (owned by the inode) */
        {
            size_t k;
            for (k = 0; k < rec->n; k++)
                if (!strcmp(rec->keys[k], "history"))
                    rec->items[k] = jv_null();
        }
        jv_free(rec);
    }
    free(inos);
    jfs_buf_puts(out, n ? "\n  }\n}\n" : "}\n}\n");
    return 0;
}

int jfs_commit_locked(jfs_fs *fs)
{
    jfs_buf out;
    size_t i;
    int rc;
    jfs_time prev_mod = fs->modified;
    if (fs->readonly || fs->is_view)
        return 0;
    /* encode modified file data first so failures don't leave half state */
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (in && !in->deleted && in->type == JFS_T_FILE && in->data_dirty) {
            rc = jfs_inode_encode(fs, in, fs->write_comp);
            if (rc)
                return rc;
        }
    }
    fs->generation++;
    fs->modified = jfs_now();
    jfs_buf_init(&out);
    jfs_serialize(fs, &out);
    rc = jfs_write_file_atomic(fs->image_path, out.data, out.len);
    jfs_buf_free(&out);
    if (rc) {
        fs->generation--;
        fs->modified = prev_mod;
        fs->last_commit_error = rc;
        jfs_log("commit of %s failed: %s", fs->image_path, jfs_strerror(rc));
        return rc;
    }
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (!in)
            continue;
        in->persisted = 1;
        in->version_open = 0;
        if (in->type == JFS_T_FILE && in->data_loaded && !in->data_dirty && in->open_count == 0) {
            free(in->data);
            in->data = NULL;
            in->data_loaded = 0;
            in->data_cap = 0;
        }
    }
    fs->dirty = 0;
    fs->need_reload = 0;
    fs->last_commit = jfs_now();
    fs->last_commit_error = 0;
    if (fs->lock)
        jfs_lockfile_write_gen(fs->lock, fs->generation, fs->uuid);
    jfs_file_signature(fs->image_path, &fs->sig);
    JFS_DEBUG("committed %s generation %llu", fs->image_path, (unsigned long long)fs->generation);
    return 0;
}

/* ======================================================================== */
/* mkfs                                                                       */
/* ======================================================================== */
int jfs_mkfs(const char *image, const jfs_mkfs_opts *o, int overwrite)
{
    jfs_fs *fs;
    jfs_inode *root;
    jfs_buf out;
    jfs_lockfile *lf = NULL;
    int rc;
    jfs_mkfs_opts dflt;
    if (!o) {
        memset(&dflt, 0, sizeof dflt);
        o = &dflt;
    }
    if (o->compression && jfs_compression_parse(o->compression, NULL, NULL))
        return -JFS_EINVAL;
    if (o->schema && (o->schema < JFS_SCHEMA_MIN || o->schema > JFS_SCHEMA_LATEST))
        return -JFS_EVERSION;
    if (o->uuid && !jfs_uuid_valid(o->uuid))
        return -JFS_EINVAL;
    if (jfs_file_exists(image)) {
        if (!overwrite)
            return -JFS_EEXIST;
        if (jfs_lockfile_open(image, &lf) == 0 &&
            jfs_lockfile_lock_timeout(lf, JFS_LOCK_ATTACH, 1, jfs_lock_timeout_ms()) != 0) {
            jfs_lockfile_close(lf, 0);
            return -JFS_ELOCKED;
        }
    }
    jfs_fs_create_empty(&fs);
    fs->image_path = jfs_strdup(image);
    fs->schema = o->schema ? o->schema : JFS_SCHEMA_LATEST;
    if (o->uuid)
        strcpy(fs->uuid, o->uuid);
    else
        jfs_uuid4(fs->uuid);
    free(fs->label);
    fs->label = jfs_strdup(o->label ? o->label : "");
    free(fs->default_comp);
    {
        /* store only the algorithm name (levels are a write-time detail) */
        const jfs_compressor *c;
        jfs_compression_parse(o->compression ? o->compression : JFS_DEFAULT_COMPRESSION, &c, NULL);
        fs->default_comp = jfs_strdup(c->name);
    }
    fs->versioning = o->versioning;
    fs->created = fs->modified = jfs_now();
    fs->next_ino = 1;
    root = jfs_inode_new(fs, JFS_T_DIR);
    root->mode = o->root_mode ? (o->root_mode & 07777) : 0755;
    root->uid = o->root_uid;
    root->gid = o->root_gid;
    fs->root_ino = root->ino;
    jfs_recount_links(fs);
    fs->generation = 1;
    jfs_buf_init(&out);
    jfs_serialize(fs, &out);
    rc = jfs_write_file_atomic(image, out.data, out.len);
    jfs_buf_free(&out);
    if (!rc) {
        if (!lf)
            jfs_lockfile_open(image, &lf);
        if (lf)
            jfs_lockfile_write_gen(lf, fs->generation, fs->uuid);
    }
    if (lf)
        jfs_lockfile_close(lf, 0);
    jfs_fs_free(fs);
    return rc;
}

/* ======================================================================== */
/* open / close                                                               */
/* ======================================================================== */
void jfs_open_opts_init(jfs_open_opts *o)
{
    memset(o, 0, sizeof *o);
    o->lockmode = JFS_LOCKMODE_EXCLUSIVE;
    o->commit_interval_ms = 5000;
}

static int read_and_parse(const char *path, jv **root, char *err, size_t errlen)
{
    char *text;
    size_t len;
    char perr[200];
    int rc = jfs_read_file(path, &text, &len);
    if (rc) {
        snprintf(err, errlen, "cannot read %s: %s", path, jfs_strerror(rc));
        return rc;
    }
    *root = jv_parse(text, len, perr, sizeof perr);
    free(text);
    if (!*root) {
        snprintf(err, errlen, "%s: invalid JSON: %s", path, perr);
        return -JFS_ECORRUPT;
    }
    return 0;
}

int jfs_open(const char *image, const jfs_open_opts *o, jfs_fs **out, char *err, size_t errlen)
{
    jfs_fs *fs;
    jv *root = NULL;
    int rc;
    jfs_open_opts dflt;
    char dummy[8];
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = 0;
    if (!o) {
        jfs_open_opts_init(&dflt);
        o = &dflt;
    }
    jfs_fs_create_empty(&fs);
    fs->image_path = jfs_strdup(image);
    fs->lockmode = o->has_at ? JFS_LOCKMODE_NONE : o->lockmode;
    fs->readonly = o->readonly || o->has_at;
    fs->shared = fs->lockmode == JFS_LOCKMODE_SHARED;
    fs->repair = o->repair;
    fs->commit_interval_ms = o->commit_interval_ms;
    fs->sync_mode = fs->shared || o->commit_interval_ms <= 0;

    if (fs->lockmode != JFS_LOCKMODE_NONE) {
        rc = jfs_lockfile_open(image, &fs->lock);
        if (rc) {
            if (!fs->readonly) {
                snprintf(err, errlen, "cannot open lock file %s.lock: %s", image, jfs_strerror(rc));
                goto fail;
            }
            fs->lock = NULL;
        }
        if (fs->lock) {
            int excl = (fs->lockmode == JFS_LOCKMODE_EXCLUSIVE && !fs->readonly) ||
                       fs->lockmode == JFS_LOCKMODE_OFFLINE;
            /* a mount that was just unmounted holds its lock until its final
             * commit is on disk, so wait a little before giving up */
            rc = jfs_lockfile_lock_timeout(fs->lock, JFS_LOCK_ATTACH, excl, jfs_lock_timeout_ms());
            if (rc) {
                if (fs->lockmode == JFS_LOCKMODE_SHARED)
                    snprintf(err, errlen,
                             "%s is attached exclusively elsewhere (single-attach mount or maintenance tool)", image);
                else if (fs->lockmode == JFS_LOCKMODE_OFFLINE)
                    snprintf(err, errlen, "%s is mounted; unmount it before running maintenance", image);
                else
                    snprintf(err, errlen,
                             "%s is already in use; mount every attachment with -o shared for multi-attach", image);
                goto fail;
            }
        }
    }
    if (fs->shared && fs->lock)
        jfs_lockfile_lock(fs->lock, JFS_LOCK_TXN, 0, 1);
    rc = read_and_parse(image, &root, err, errlen);
    if (!rc)
        jfs_file_signature(image, &fs->sig);
    if (fs->shared && fs->lock)
        jfs_lockfile_unlock(fs->lock, JFS_LOCK_TXN);
    if (rc)
        goto fail;
    rc = jfs_fs_load_json(fs, root, o->has_at ? &o->at : NULL, err, errlen);
    jv_free(root);
    root = NULL;
    if (rc)
        goto fail;
    if (fs->shared && fs->schema < 2) {
        snprintf(err, errlen, "multi-attach needs schema 2; run 'jsonfs upgrade %s' first", image);
        rc = -JFS_EVERSION;
        goto fail;
    }
    free(fs->write_comp);
    fs->write_comp = jfs_strdup(o->compression ? o->compression : fs->default_comp);
    if (jfs_compression_parse(fs->write_comp, NULL, NULL)) {
        snprintf(err, errlen, "unknown compression algorithm \"%s\"", fs->write_comp);
        rc = -JFS_EINVAL;
        goto fail;
    }
    if (fs->lock && !fs->readonly && !fs->shared)
        jfs_lockfile_write_gen(fs->lock, fs->generation, fs->uuid);
    if (fs->dirty && !fs->readonly) { /* repairs */
        rc = jfs_commit_locked(fs);
        if (rc) {
            snprintf(err, errlen, "cannot write repaired image: %s", jfs_strerror(rc));
            goto fail;
        }
    }
    *out = fs;
    return 0;
fail:
    jv_free(root);
    if (fs->lock)
        jfs_lockfile_close(fs->lock, 0);
    fs->lock = NULL;
    jfs_fs_free(fs);
    if (!err[0])
        snprintf(err, errlen, "%s", jfs_strerror(rc));
    return rc;
}

int jfs_sync(jfs_fs *fs)
{
    int rc = 0;
    jfs_mutex_lock(&fs->mu);
    if (fs->dirty && !fs->readonly) {
        if (fs->shared && fs->lock) {
            jfs_lockfile_lock(fs->lock, JFS_LOCK_TXN, 1, 1);
            rc = jfs_commit_locked(fs);
            jfs_lockfile_unlock(fs->lock, JFS_LOCK_TXN);
        } else {
            rc = jfs_commit_locked(fs);
        }
    }
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

static void committer_main(void *arg)
{
    jfs_fs *fs = arg;
    unsigned waited = 0;
    while (!fs->committer_stop) {
        jfs_sleep_ms(100);
        waited += 100;
        if (waited < (unsigned)fs->commit_interval_ms)
            continue;
        waited = 0;
        jfs_mutex_lock(&fs->mu);
        if (fs->dirty && !fs->readonly)
            jfs_commit_locked(fs);
        jfs_mutex_unlock(&fs->mu);
    }
}

int jfs_start_committer(jfs_fs *fs)
{
    if (fs->committer || fs->sync_mode || fs->readonly)
        return 0;
    fs->committer_stop = 0;
    return jfs_thread_start(&fs->committer, committer_main, fs);
}

void jfs_stop_committer(jfs_fs *fs)
{
    if (!fs->committer)
        return;
    fs->committer_stop = 1;
    jfs_thread_join(fs->committer);
    fs->committer = NULL;
}

int jfs_close(jfs_fs *fs)
{
    int rc;
    if (!fs)
        return 0;
    jfs_stop_committer(fs);
    rc = jfs_sync(fs);
    if (fs->lock)
        jfs_lockfile_close(fs->lock, 0);
    fs->lock = NULL;
    jfs_fs_free(fs);
    return rc;
}

/* ======================================================================== */
/* Multi-attach: reload when a peer committed                                 */
/* ======================================================================== */
int jfs_maybe_reload(jfs_fs *fs)
{
    uint64_t gen = 0;
    jfs_filesig sig;
    jfs_fs *tmp;
    jv *root;
    char err[256];
    int rc;
    size_t i;
    jfs_handle *h;
    int changed = fs->need_reload;

    if (!fs->shared)
        return 0;
    if (!changed && fs->lock && jfs_lockfile_read_gen(fs->lock, &gen, NULL) == 0 && gen != fs->generation)
        changed = 1;
    if (!changed && jfs_file_signature(fs->image_path, &sig) == 0 && !jfs_filesig_equal(&sig, &fs->sig))
        changed = 1;
    if (!changed)
        return 0;

    rc = read_and_parse(fs->image_path, &root, err, sizeof err);
    if (rc) {
        jfs_log("reload failed: %s", err);
        return -JFS_EIO;
    }
    jfs_fs_create_empty(&tmp);
    tmp->image_path = jfs_strdup(fs->image_path);
    rc = jfs_fs_load_json(tmp, root, NULL, err, sizeof err);
    jv_free(root);
    if (rc) {
        jfs_log("reload failed: %s", err);
        jfs_fs_free(tmp);
        return -JFS_EIO;
    }
    JFS_DEBUG("reloading %s: generation %llu -> %llu", fs->image_path, (unsigned long long)fs->generation,
              (unsigned long long)tmp->generation);

    /* swap state: fs gets the fresh image, tmp keeps the old inodes */
    {
        jfs_u64map m = fs->inodes;
        char *s;
        jv *x;
        fs->inodes = tmp->inodes;
        tmp->inodes = m;
        s = fs->label;
        fs->label = tmp->label;
        tmp->label = s;
        s = fs->default_comp;
        fs->default_comp = tmp->default_comp;
        tmp->default_comp = s;
        x = fs->extra_header;
        fs->extra_header = tmp->extra_header;
        tmp->extra_header = x;
        fs->schema = tmp->schema;
        memcpy(fs->uuid, tmp->uuid, sizeof fs->uuid);
        fs->created = tmp->created;
        fs->modified = tmp->modified;
        fs->generation = tmp->generation;
        fs->versioning = tmp->versioning;
        fs->next_ino = tmp->next_ino;
        fs->root_ino = tmp->root_ino;
    }
    fs->dirty = 0;
    fs->need_reload = 0;
    jfs_file_signature(fs->image_path, &fs->sig);

    /* re-attach open handles */
    for (h = fs->handles; h; h = h->next) {
        jfs_inode *old = h->ino, *nw;
        if (old->orphan)
            continue;
        if (old->moved_to) {
            h->ino = old->moved_to;
            h->ino->open_count++;
            continue;
        }
        nw = jfs_lookup_ino(fs, old->ino);
        if (nw && nw->type == old->type) {
            if (old->data_dirty) {
                jfs_version_touch(fs, nw);
                jfs_version_before_data_change(fs, nw);
                free(nw->data);
                nw->data = old->data;
                nw->data_cap = old->data_cap;
                nw->size = old->size;
                nw->data_loaded = 1;
                nw->data_dirty = 1;
                nw->mtime = old->mtime;
                nw->ctime = old->ctime;
                nw->vtime = old->vtime;
                old->data = NULL;
                old->data_loaded = 0;
                fs->dirty = 1;
            }
            old->moved_to = nw;
            nw->open_count++;
            h->ino = nw;
        } else {
            /* deleted by a peer: keep our copy alive until closed */
            jfs_u64map_del(&tmp->inodes, old->ino);
            jfs_orphan_add(fs, old);
        }
    }
    /* old inodes are freed with tmp (except orphans, removed above) */
    for (i = 0; i < tmp->inodes.cap; i++)
        if (tmp->inodes.vals[i])
            ((jfs_inode *)tmp->inodes.vals[i])->moved_to = NULL;
    jfs_fs_free(tmp);
    return 0;
}

/* ======================================================================== */
/* Info                                                                       */
/* ======================================================================== */
void jfs_get_info(jfs_fs *fs, jfs_info *info)
{
    size_t i;
    memset(info, 0, sizeof *info);
    jfs_mutex_lock(&fs->mu);
    strcpy(info->uuid, fs->uuid);
    snprintf(info->label, sizeof info->label, "%s", fs->label);
    info->schema = fs->schema;
    info->created = fs->created;
    info->modified = fs->modified;
    info->generation = fs->generation;
    snprintf(info->default_compression, sizeof info->default_compression, "%s", fs->default_comp);
    snprintf(info->write_compression, sizeof info->write_compression, "%s", fs->write_comp);
    info->versioning = fs->versioning;
    info->readonly = fs->readonly;
    info->shared = fs->shared;
    info->is_view = fs->is_view;
    info->view_time = fs->view_time;
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (!in)
            continue;
        if (in->history && in->history->t == JV_ARR)
            info->history_records += in->history->n;
        if (in->deleted) {
            info->deleted_markers++;
            continue;
        }
        info->inodes++;
        switch (in->type) {
        case JFS_T_FILE:
            info->files++;
            info->logical_bytes += in->size;
            break;
        case JFS_T_DIR: info->dirs++; break;
        case JFS_T_SYMLINK: info->symlinks++; break;
        default: info->others++;
        }
    }
    jfs_mutex_unlock(&fs->mu);
}
