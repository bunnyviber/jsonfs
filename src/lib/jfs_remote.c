/*
 * JSONfs - remote (http/https) images: initial load with an optional on-disk
 * cache, cheap change detection, pending updates, and the change list used
 * to replay remote updates through the kernel so that file system watchers
 * (inotify, kqueue, FSEvents, ReadDirectoryChangesW) see them.
 *
 * Network economy:
 *   - every check is a conditional GET (If-None-Match / If-Modified-Since)
 *     when the server provided validators: an unchanged image costs one
 *     small 304 response;
 *   - without validators a 4 KiB range request of the image header is used
 *     (uuid + generation + modified identify a version exactly);
 *   - the full image is downloaded only when it really changed (gzip
 *     transfer encoding is accepted);
 *   - with a cache file, even the initial mount is a conditional request.
 * Failures never discard data: the last good version stays mounted.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jfs_http.h"
#include "jfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_BYTES 4096

struct jfs_pending {
    jfs_fs *nfs;
    char *etag, *last_modified;
};

int jfs_is_remote(jfs_fs *fs)
{
    return fs && fs->is_remote;
}

void jfs_remote_get_stats(jfs_fs *fs, jfs_remote_stats *st)
{
    jfs_mutex_lock(&fs->mu);
    *st = fs->rstats;
    jfs_mutex_unlock(&fs->mu);
}

static void set_str(char **dst, const char *v)
{
    free(*dst);
    *dst = v && *v ? jfs_strdup(v) : NULL;
}

/* ------------------------------------------------------------------------ */
/* header probe: find top-level "key": value in the first bytes               */
/* ------------------------------------------------------------------------ */
static const char *find_key(const char *t, size_t n, const char *key)
{
    char pat[64];
    size_t pl;
    const char *p = t, *end = t + n, *inodes;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    pl = strlen(pat);
    /* only look at the header, i.e. before "inodes" */
    inodes = NULL;
    {
        const char *q = t;
        while (q + 8 <= end) {
            if (!memcmp(q, "\"inodes\"", 8)) {
                inodes = q;
                break;
            }
            q++;
        }
    }
    if (inodes)
        end = inodes;
    while (p + pl <= end) {
        if (!memcmp(p, pat, pl)) {
            p += pl;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                p++;
            if (p < end && *p == ':') {
                p++;
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                    p++;
                return p < end ? p : NULL;
            }
        }
        p++;
    }
    return NULL;
}

typedef struct ident {
    char uuid[40];
    char modified[64];
    uint64_t generation;
    int has_gen;
} ident;

static int probe_ident(const char *t, size_t n, ident *id)
{
    const char *p;
    size_t k;
    memset(id, 0, sizeof *id);
    p = find_key(t, n, "uuid");
    if (!p || *p != '"')
        return -1;
    for (k = 0; k < sizeof id->uuid - 1 && p + 1 + k < t + n && p[1 + k] != '"'; k++)
        id->uuid[k] = p[1 + k];
    p = find_key(t, n, "modified");
    if (!p || *p != '"')
        return -1;
    for (k = 0; k < sizeof id->modified - 1 && p + 1 + k < t + n && p[1 + k] != '"'; k++)
        id->modified[k] = p[1 + k];
    p = find_key(t, n, "generation");
    if (p) {
        id->generation = strtoull(p, NULL, 10);
        id->has_gen = 1;
    }
    return 0;
}

static void fs_ident(jfs_fs *fs, ident *id)
{
    memset(id, 0, sizeof *id);
    snprintf(id->uuid, sizeof id->uuid, "%s", fs->uuid);
    jfs_time_format(fs->modified, id->modified);
    id->generation = fs->generation;
    id->has_gen = fs->schema >= 2;
}

static int same_ident(const ident *a, const ident *b)
{
    jfs_time ta, tb;
    if (strcmp(a->uuid, b->uuid))
        return 0;
    if (a->has_gen && b->has_gen && a->generation != b->generation)
        return 0;
    if (jfs_time_parse(a->modified, &ta) || jfs_time_parse(b->modified, &tb))
        return !strcmp(a->modified, b->modified);
    return jfs_time_cmp(ta, tb) == 0;
}

/* ------------------------------------------------------------------------ */
/* cache                                                                      */
/* ------------------------------------------------------------------------ */
static char *meta_path(const char *cache)
{
    return jfs_asprintf("%s.meta", cache);
}

static void cache_store(const char *cache, const char *url, const char *data, size_t len, const char *etag,
                        const char *lm)
{
    jv *m;
    jfs_buf b;
    char shown[1024], *mp;
    char tb[JFS_TIME_STRLEN];
    int rc;
    if (!cache)
        return;
    rc = jfs_write_file_atomic(cache, data, len);
    if (rc) {
        jfs_log("cannot write cache %s: %s", cache, jfs_strerror(rc));
        return;
    }
    jfs_url_redact(url, shown, sizeof shown);
    m = jv_obj();
    jv_obj_set(m, "url", jv_str(shown));
    jv_obj_set(m, "etag", jv_str(etag ? etag : ""));
    jv_obj_set(m, "last_modified", jv_str(lm ? lm : ""));
    jfs_time_format(jfs_now(), tb);
    jv_obj_set(m, "saved", jv_str(tb));
    jfs_buf_init(&b);
    jv_write(&b, m, 2);
    jfs_buf_putc(&b, '\n');
    mp = meta_path(cache);
    jfs_write_file_atomic(mp, b.data, b.len);
    free(mp);
    jfs_buf_free(&b);
    jv_free(m);
}

/* Returns 1 if a cache for `url` exists; fills validators. */
static int cache_meta(const char *cache, const char *url, char **etag, char **lm)
{
    char *mp, *text, shown[1024];
    size_t len;
    jv *m;
    int ok = 0;
    *etag = *lm = NULL;
    if (!cache || !jfs_file_exists(cache))
        return 0;
    mp = meta_path(cache);
    if (jfs_read_file(mp, &text, &len) == 0) {
        m = jv_parse(text, len, NULL, 0);
        free(text);
        jfs_url_redact(url, shown, sizeof shown);
        if (m && jv_get_str(m, "url") && !strcmp(jv_get_str(m, "url"), shown)) {
            ok = 1;
            if (jv_get_str(m, "etag") && *jv_get_str(m, "etag"))
                *etag = jfs_strdup(jv_get_str(m, "etag"));
            if (jv_get_str(m, "last_modified") && *jv_get_str(m, "last_modified"))
                *lm = jfs_strdup(jv_get_str(m, "last_modified"));
        }
        jv_free(m);
    }
    free(mp);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* initial load                                                               */
/* ------------------------------------------------------------------------ */
int jfs_remote_initial_load(jfs_fs *fs, const char *url, jv **root, char *err, size_t errlen)
{
    jfs_http_req rq;
    jfs_http_resp rs;
    char *data = NULL, *c_etag = NULL, *c_lm = NULL, perr[200], shown[1024];
    size_t len = 0;
    int rc, have_cache;
    fs->is_remote = 1;
    have_cache = cache_meta(fs->cache_path, url, &c_etag, &c_lm);
    memset(&rq, 0, sizeof rq);
    if (have_cache) {
        rq.if_none_match = c_etag;
        rq.if_modified_since = c_etag ? NULL : c_lm;
    }
    fs->rstats.checks++;
    fs->rstats.last_check = jfs_now();
    rc = jfs_http_fetch(url, &rq, &rs, &data, &len, err, errlen);
    if (rc == 0 && rs.not_modified && have_cache) {
        fs->rstats.not_modified++;
        free(data);
        data = NULL;
        rc = jfs_read_file(fs->cache_path, &data, &len);
        if (rc) {
            snprintf(err, errlen, "cannot read cache %s: %s", fs->cache_path, jfs_strerror(rc));
            goto out;
        }
        set_str(&fs->etag, rs.etag[0] ? rs.etag : c_etag);
        set_str(&fs->last_modified, rs.last_modified[0] ? rs.last_modified : c_lm);
    } else if (rc == 0 && !rs.not_modified) {
        fs->rstats.full_downloads++;
        fs->rstats.bytes_downloaded += len;
        set_str(&fs->etag, rs.etag);
        set_str(&fs->last_modified, rs.last_modified);
        cache_store(fs->cache_path, url, data, len, fs->etag, fs->last_modified);
    } else if (have_cache || (fs->cache_path && jfs_file_exists(fs->cache_path))) {
        /* server unreachable / image removed: serve the last copy we got */
        snprintf(fs->rstats.last_error, sizeof fs->rstats.last_error, "%s", err);
        fs->rstats.failures++;
        jfs_log("warning: %s; using the cached copy %s", err, fs->cache_path);
        free(data);
        data = NULL;
        rc = jfs_read_file(fs->cache_path, &data, &len);
        if (rc) {
            snprintf(err, errlen, "cannot read cache %s: %s", fs->cache_path, jfs_strerror(rc));
            goto out;
        }
        fs->rstats.serving_cached_copy = 1;
        set_str(&fs->etag, c_etag);
        set_str(&fs->last_modified, c_lm);
    } else {
        if (rc == 0)
            rc = -JFS_EREMOTE;
        goto out;
    }
    *root = jv_parse(data, len, perr, sizeof perr);
    if (!*root) {
        jfs_url_redact(url, shown, sizeof shown);
        snprintf(err, errlen, "%s: invalid JSON: %s", shown, perr);
        rc = -JFS_ECORRUPT;
    }
out:
    free(data);
    free(c_etag);
    free(c_lm);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* refresh check                                                              */
/* ------------------------------------------------------------------------ */
void jfs_pending_free(jfs_pending *p)
{
    if (!p)
        return;
    jfs_fs_free(p->nfs);
    free(p->etag);
    free(p->last_modified);
    free(p);
}

static int record_failure(jfs_fs *fs, int rc, const char *err)
{
    jfs_mutex_lock(&fs->mu);
    fs->rstats.failures++;
    snprintf(fs->rstats.last_error, sizeof fs->rstats.last_error, "%s", err);
    jfs_mutex_unlock(&fs->mu);
    return rc;
}

int jfs_refresh_check(jfs_fs *fs, jfs_pending **out, char *err, size_t errlen)
{
    jfs_http_req rq;
    jfs_http_resp rs;
    char *url, *etag, *lm, *data = NULL, perr[200];
    size_t len = 0;
    ident cur, got;
    int rc, is_view;
    jfs_time view_time;
    jv *root;
    jfs_fs *nfs;
    char dummy[8];

    *out = NULL;
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = 0;
    if (!fs->is_remote)
        return 0;
    jfs_mutex_lock(&fs->remote_mu);
    jfs_mutex_lock(&fs->mu);
    url = jfs_strdup(fs->image_path);
    etag = fs->etag ? jfs_strdup(fs->etag) : NULL;
    lm = fs->last_modified ? jfs_strdup(fs->last_modified) : NULL;
    fs_ident(fs, &cur);
    is_view = fs->is_view;
    view_time = fs->view_time;
    fs->rstats.checks++;
    fs->rstats.last_check = jfs_now();
    jfs_mutex_unlock(&fs->mu);

    memset(&rq, 0, sizeof rq);
    if (etag || lm) {
        rq.if_none_match = etag;
        rq.if_modified_since = etag ? NULL : lm;
        rc = jfs_http_fetch(url, &rq, &rs, &data, &len, err, errlen);
        if (rc)
            goto fail;
        if (rs.not_modified) {
            jfs_mutex_lock(&fs->mu);
            fs->rstats.not_modified++;
            fs->rstats.serving_cached_copy = 0;
            fs->rstats.last_error[0] = 0;
            jfs_mutex_unlock(&fs->mu);
            rc = 0;
            goto done;
        }
    } else {
        /* no validators: look at the header only */
        rq.range_end = PROBE_BYTES - 1;
        rq.max_bytes = PROBE_BYTES;
        rc = jfs_http_fetch(url, &rq, &rs, &data, &len, err, errlen);
        if (rc)
            goto fail;
        jfs_mutex_lock(&fs->mu);
        fs->rstats.probes++;
        fs->rstats.bytes_downloaded += len;
        if (rs.etag[0])
            set_str(&fs->etag, rs.etag); /* a range response carries the entity's ETag */
        jfs_mutex_unlock(&fs->mu);
        if (probe_ident(data, len, &got) == 0 && same_ident(&cur, &got)) {
            jfs_mutex_lock(&fs->mu);
            fs->rstats.not_modified++;
            fs->rstats.serving_cached_copy = 0;
            fs->rstats.last_error[0] = 0;
            jfs_mutex_unlock(&fs->mu);
            rc = 0;
            goto done;
        }
        if (!(rs.status == 200 && !rs.truncated)) {
            /* changed (or header not recognised): fetch the whole image */
            free(data);
            data = NULL;
            memset(&rq, 0, sizeof rq);
            rc = jfs_http_fetch(url, &rq, &rs, &data, &len, err, errlen);
            if (rc)
                goto fail;
        }
    }
    /* full body in data */
    jfs_mutex_lock(&fs->mu);
    fs->rstats.full_downloads++;
    fs->rstats.bytes_downloaded += len;
    jfs_mutex_unlock(&fs->mu);
    root = jv_parse(data, len, perr, sizeof perr);
    if (!root) {
        char shown[1024];
        jfs_url_redact(url, shown, sizeof shown);
        snprintf(err, errlen, "%s: invalid JSON: %s (keeping the previous version)", shown, perr);
        rc = -JFS_ECORRUPT;
        goto fail;
    }
    jfs_fs_create_empty(&nfs);
    nfs->image_path = jfs_strdup(url);
    rc = jfs_fs_load_json(nfs, root, is_view ? &view_time : NULL, err, errlen);
    jv_free(root);
    if (rc) {
        jfs_fs_free(nfs);
        goto fail;
    }
    fs_ident(nfs, &got);
    cache_store(fs->cache_path, url, data, len, rs.etag, rs.last_modified);
    if (same_ident(&cur, &got)) {
        /* same version (server without useful validators): remember the new ones */
        jfs_mutex_lock(&fs->mu);
        set_str(&fs->etag, rs.etag);
        set_str(&fs->last_modified, rs.last_modified);
        fs->rstats.not_modified++;
        fs->rstats.serving_cached_copy = 0;
        fs->rstats.last_error[0] = 0;
        jfs_mutex_unlock(&fs->mu);
        jfs_fs_free(nfs);
        rc = 0;
        goto done;
    }
    *out = jfs_calloc(1, sizeof **out);
    (*out)->nfs = nfs;
    (*out)->etag = rs.etag[0] ? jfs_strdup(rs.etag) : NULL;
    (*out)->last_modified = rs.last_modified[0] ? jfs_strdup(rs.last_modified) : NULL;
    rc = 0;
    goto done;
fail:
    record_failure(fs, rc, err);
done:
    free(data);
    free(url);
    free(etag);
    free(lm);
    jfs_mutex_unlock(&fs->remote_mu);
    return rc;
}

void jfs_replay_begin(jfs_fs *fs, jfs_pending *p)
{
    jfs_mutex_lock(&fs->mu);
    fs->replay_target = p->nfs;
    jfs_mutex_unlock(&fs->mu);
}

int jfs_replay_active(jfs_fs *fs)
{
    int r;
    jfs_mutex_lock(&fs->mu);
    r = fs->replay_target != NULL;
    jfs_mutex_unlock(&fs->mu);
    return r;
}

void jfs_pending_install(jfs_fs *fs, jfs_pending *p)
{
    jfs_mutex_lock(&fs->mu);
    fs->replay_target = NULL;
    set_str(&fs->etag, p->etag);
    set_str(&fs->last_modified, p->last_modified);
    jfs_install_loaded(fs, p->nfs); /* frees p->nfs */
    p->nfs = NULL;
    fs->rstats.updates++;
    fs->rstats.last_update = jfs_now();
    fs->rstats.serving_cached_copy = 0;
    fs->rstats.last_error[0] = 0;
    jfs_mutex_unlock(&fs->mu);
    jfs_pending_free(p);
}

/* ======================================================================== */
/* Diff                                                                       */
/* ======================================================================== */
typedef struct wdir {
    uint64_t ino;    /* key in the working tree */
    uint64_t parent; /* 0 for the root */
    char *name;
    jfs_strmap ents;
} wdir;

typedef struct nbinding {
    uint64_t parent;
    char *name;
} nbinding;

typedef struct dctx {
    jfs_fs *o, *n;
    uint64_t oroot, nroot;
    int same_fs;
    jfs_u64map wd;      /* working tree dirs (keyed by old ino; root by oroot) */
    jfs_u64map nparent; /* new dirs: ino -> nbinding */
    jfs_u64map created; /* inodes created by the replay (fully copied) */
    jfs_changes *out;
} dctx;

void jfs_changes_free(jfs_changes *c)
{
    size_t i;
    for (i = 0; i < c->n; i++) {
        free(c->v[i].path);
        free(c->v[i].path2);
        free(c->v[i].target);
    }
    free(c->v);
    memset(c, 0, sizeof *c);
}

static void emit(dctx *d, int kind, char *path, char *path2, const jfs_inode *in)
{
    jfs_change *c;
    if (d->out->n == d->out->cap) {
        d->out->cap = d->out->cap ? d->out->cap * 2 : 32;
        d->out->v = jfs_realloc(d->out->v, d->out->cap * sizeof(jfs_change));
    }
    c = &d->out->v[d->out->n++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->path = path;
    c->path2 = path2;
    if (in) {
        c->mode = jfs_type_mode_bits(in->type) | (in->mode & 07777);
        c->size = in->size;
        c->rdev = in->rdev;
        if (in->target)
            c->target = jfs_strdup(in->target);
    }
}

static wdir *wd_get(dctx *d, uint64_t new_dir_ino)
{
    return jfs_u64map_get(&d->wd, new_dir_ino == d->nroot ? d->oroot : new_dir_ino);
}

static char *wpath(dctx *d, wdir *w)
{
    /* build from the root down */
    const char *parts[512];
    int n = 0, i;
    jfs_buf b;
    while (w && w->parent && n < 512) {
        parts[n++] = w->name;
        w = jfs_u64map_get(&d->wd, w->parent);
    }
    if (n == 0)
        return jfs_strdup("/");
    jfs_buf_init(&b);
    for (i = n - 1; i >= 0; i--) {
        jfs_buf_putc(&b, '/');
        jfs_buf_puts(&b, parts[i]);
    }
    return jfs_buf_steal(&b, NULL);
}

static char *wchild(dctx *d, wdir *w, const char *name)
{
    char *p = wpath(d, w), *r;
    r = strcmp(p, "/") ? jfs_asprintf("%s/%s", p, name) : jfs_asprintf("/%s", name);
    free(p);
    return r;
}

static wdir *wd_add(dctx *d, uint64_t ino, uint64_t parent, const char *name)
{
    wdir *w = jfs_calloc(1, sizeof *w);
    w->ino = ino;
    w->parent = parent;
    w->name = name ? jfs_strdup(name) : NULL;
    jfs_strmap_init(&w->ents);
    jfs_u64map_put(&d->wd, ino, w);
    return w;
}

static void wd_free(wdir *w)
{
    jfs_strmap_free(&w->ents);
    free(w->name);
    free(w);
}

/* the live new inode for an old inode number (same filesystem only) */
static jfs_inode *new_same(dctx *d, uint64_t ino, jfs_type type)
{
    jfs_inode *n;
    if (!d->same_fs)
        return NULL;
    n = jfs_lookup_ino(d->n, ino == d->oroot ? d->nroot : ino);
    return n && n->type == type ? n : NULL;
}

static int binding_in_new(dctx *d, wdir *w, const char *name, uint64_t ino)
{
    jfs_inode *nd;
    uint64_t v;
    if (!d->same_fs)
        return 0;
    nd = jfs_lookup_ino(d->n, w->ino == d->oroot ? d->nroot : w->ino);
    if (!nd || nd->type != JFS_T_DIR)
        return 0;
    return jfs_strmap_get(nd->entries, name, &v) && v == ino;
}

static int w_is_descendant(dctx *d, uint64_t dir, uint64_t ancestor)
{
    wdir *w = jfs_u64map_get(&d->wd, dir);
    while (w) {
        if (w->ino == ancestor)
            return 1;
        w = w->parent ? jfs_u64map_get(&d->wd, w->parent) : NULL;
    }
    return 0;
}

static void build_wtree(dctx *d, wdir *w)
{
    jfs_inode *od = jfs_lookup_ino(d->o, w->ino);
    const char **keys;
    size_t i;
    if (!od || od->type != JFS_T_DIR)
        return;
    keys = jfs_strmap_sorted_keys(od->entries);
    for (i = 0; keys[i]; i++) {
        uint64_t ino;
        jfs_inode *c;
        jfs_strmap_get(od->entries, keys[i], &ino);
        c = jfs_lookup_ino(d->o, ino);
        if (!c)
            continue;
        jfs_strmap_put(&w->ents, keys[i], ino);
        if (c->type == JFS_T_DIR && !jfs_u64map_get(&d->wd, ino))
            build_wtree(d, wd_add(d, ino, w->ino, keys[i]));
    }
    free((void *)keys);
}

static void build_nparents(dctx *d, uint64_t dir)
{
    jfs_inode *nd = jfs_lookup_ino(d->n, dir);
    size_t i;
    if (!nd || nd->type != JFS_T_DIR)
        return;
    for (i = 0; i < nd->entries->cap; i++) {
        jfs_inode *c;
        nbinding *b;
        if (!nd->entries->keys[i])
            continue;
        c = jfs_lookup_ino(d->n, nd->entries->vals[i]);
        if (!c || c->type != JFS_T_DIR || jfs_u64map_get(&d->nparent, c->ino))
            continue;
        b = jfs_calloc(1, sizeof *b);
        b->parent = dir;
        b->name = jfs_strdup(nd->entries->keys[i]);
        jfs_u64map_put(&d->nparent, c->ino, b);
        build_nparents(d, c->ino);
    }
}

/* first new binding (parent, name) of a non-directory inode not present in W */
static int new_free_binding(dctx *d, uint64_t ino, wdir **pw, const char **pname)
{
    size_t i;
    for (i = 0; i < d->n->inodes.cap; i++) {
        jfs_inode *nd = d->n->inodes.vals[i];
        size_t k;
        wdir *w;
        if (!nd || nd->deleted || nd->type != JFS_T_DIR)
            continue;
        if (nd->ino != d->nroot && !jfs_u64map_get(&d->nparent, nd->ino))
            continue; /* unreachable */
        w = wd_get(d, nd->ino);
        if (!w)
            continue;
        for (k = 0; k < nd->entries->cap; k++) {
            if (nd->entries->keys[k] && nd->entries->vals[k] == ino &&
                !jfs_strmap_get(&w->ents, nd->entries->keys[k], NULL)) {
                *pw = w;
                *pname = nd->entries->keys[k];
                return 1;
            }
        }
    }
    return 0;
}

static void phase_renames(dctx *d)
{
    int pass, moved = 1;
    for (pass = 0; pass < 4 && moved; pass++) {
        size_t i;
        moved = 0;
        for (i = 0; i < d->wd.cap; i++) {
            wdir *w = d->wd.vals[i];
            const char **keys;
            size_t k;
            if (!w)
                continue;
            keys = jfs_strmap_sorted_keys(&w->ents);
            for (k = 0; keys[k]; k++) {
                uint64_t ino;
                jfs_inode *oi, *ni;
                wdir *dst = NULL;
                const char *dname = NULL;
                char *from, *to, *name;
                if (!jfs_strmap_get(&w->ents, keys[k], &ino))
                    continue;
                if (binding_in_new(d, w, keys[k], ino))
                    continue;
                oi = jfs_lookup_ino(d->o, ino);
                if (!oi || !(ni = new_same(d, ino, oi->type)))
                    continue;
                if (oi->type == JFS_T_DIR) {
                    nbinding *b = jfs_u64map_get(&d->nparent, ino);
                    if (!b)
                        continue;
                    dst = wd_get(d, b->parent);
                    dname = b->name;
                    if (!dst || jfs_strmap_get(&dst->ents, dname, NULL) || w_is_descendant(d, dst->ino, ino))
                        continue;
                } else if (!new_free_binding(d, ino, &dst, &dname)) {
                    continue;
                }
                from = wchild(d, w, keys[k]);
                to = wchild(d, dst, dname);
                name = jfs_strdup(keys[k]);
                emit(d, JFS_CHANGE_RENAME, from, to, oi);
                jfs_strmap_del(&w->ents, name);
                jfs_strmap_put(&dst->ents, dname, ino);
                if (oi->type == JFS_T_DIR) {
                    wdir *mw = jfs_u64map_get(&d->wd, ino);
                    if (mw) {
                        mw->parent = dst->ino;
                        free(mw->name);
                        mw->name = jfs_strdup(dname);
                    }
                }
                free(name);
                moved = 1;
            }
            free((void *)keys);
        }
    }
}

static void remove_subtree(dctx *d, wdir *w)
{
    const char **keys = jfs_strmap_sorted_keys(&w->ents);
    size_t k;
    for (k = 0; keys[k]; k++) {
        uint64_t ino;
        wdir *cw;
        jfs_inode *oi;
        jfs_strmap_get(&w->ents, keys[k], &ino);
        oi = jfs_lookup_ino(d->o, ino);
        cw = jfs_u64map_get(&d->wd, ino);
        if (cw) {
            remove_subtree(d, cw);
            jfs_u64map_del(&d->wd, ino);
            wd_free(cw);
        }
        emit(d, JFS_CHANGE_DELETE, wchild(d, w, keys[k]), NULL, oi);
    }
    free((void *)keys);
    jfs_strmap_free(&w->ents);
    jfs_strmap_init(&w->ents);
}

static void phase_deletes(dctx *d, wdir *w)
{
    const char **keys = jfs_strmap_sorted_keys(&w->ents);
    char **names;
    size_t k, n = 0;
    for (k = 0; keys[k]; k++)
        n++;
    names = jfs_malloc((n + 1) * sizeof(char *));
    for (k = 0; k < n; k++)
        names[k] = jfs_strdup(keys[k]);
    free((void *)keys);
    for (k = 0; k < n; k++) {
        uint64_t ino;
        wdir *cw;
        jfs_inode *oi;
        if (!jfs_strmap_get(&w->ents, names[k], &ino))
            continue;
        oi = jfs_lookup_ino(d->o, ino);
        cw = jfs_u64map_get(&d->wd, ino);
        if (binding_in_new(d, w, names[k], ino) && new_same(d, ino, oi ? oi->type : JFS_T_NONE)) {
            if (cw)
                phase_deletes(d, cw);
            continue;
        }
        if (cw) {
            remove_subtree(d, cw);
            jfs_u64map_del(&d->wd, ino);
            wd_free(cw);
        }
        emit(d, JFS_CHANGE_DELETE, wchild(d, w, names[k]), NULL, oi);
        jfs_strmap_del(&w->ents, names[k]);
    }
    for (k = 0; k < n; k++)
        free(names[k]);
    free(names);
}

static char *w_find_binding(dctx *d, uint64_t ino)
{
    size_t i;
    for (i = 0; i < d->wd.cap; i++) {
        wdir *w = d->wd.vals[i];
        size_t k;
        if (!w)
            continue;
        for (k = 0; k < w->ents.cap; k++)
            if (w->ents.keys[k] && w->ents.vals[k] == ino)
                return wchild(d, w, w->ents.keys[k]);
    }
    return NULL;
}

static void phase_creates(dctx *d, uint64_t ndir)
{
    jfs_inode *nd = jfs_lookup_ino(d->n, ndir);
    wdir *w = wd_get(d, ndir);
    const char **keys;
    size_t k;
    if (!nd || nd->type != JFS_T_DIR || !w)
        return;
    keys = jfs_strmap_sorted_keys(nd->entries);
    for (k = 0; keys[k]; k++) {
        uint64_t ino, have;
        jfs_inode *ni;
        jfs_strmap_get(nd->entries, keys[k], &ino);
        ni = jfs_lookup_ino(d->n, ino);
        if (!ni)
            continue;
        if (jfs_strmap_get(&w->ents, keys[k], &have)) {
            if (ni->type == JFS_T_DIR && (have == ino || !d->same_fs))
                phase_creates(d, ino);
            continue;
        }
        if (ni->type != JFS_T_DIR && d->same_fs) {
            char *existing = w_find_binding(d, ino);
            if (existing) {
                emit(d, JFS_CHANGE_LINK, wchild(d, w, keys[k]), existing, ni);
                jfs_strmap_put(&w->ents, keys[k], ino);
                continue;
            }
        }
        emit(d, JFS_CHANGE_CREATE, wchild(d, w, keys[k]), NULL, ni);
        jfs_strmap_put(&w->ents, keys[k], ino);
        jfs_u64map_put(&d->created, ino, (void *)1);
        if (ni->type == JFS_T_DIR) {
            if (!jfs_u64map_get(&d->wd, ino))
                wd_add(d, ino, w->ino, keys[k]);
            phase_creates(d, ino);
        }
    }
    free((void *)keys);
}

static int xattrs_differ(const jfs_inode *a, const jfs_inode *b)
{
    size_t i, j;
    if (a->nxattrs != b->nxattrs)
        return 1;
    for (i = 0; i < a->nxattrs; i++) {
        int found = 0;
        for (j = 0; j < b->nxattrs; j++)
            if (!strcmp(a->xattrs[i].name, b->xattrs[j].name) && a->xattrs[i].len == b->xattrs[j].len &&
                !memcmp(a->xattrs[i].val, b->xattrs[j].val, a->xattrs[i].len))
                found = 1;
        if (!found)
            return 1;
    }
    return 0;
}

static void phase_modifies(dctx *d)
{
    size_t i;
    for (i = 0; i < d->n->inodes.cap; i++) {
        jfs_inode *ni = d->n->inodes.vals[i], *oi;
        int content = 0, meta = 0;
        char *path;
        if (!ni || ni->deleted || jfs_u64map_get(&d->created, ni->ino))
            continue;
        if (ni->ino == d->nroot)
            oi = jfs_lookup_ino(d->o, d->oroot);
        else if (d->same_fs)
            oi = jfs_lookup_ino(d->o, ni->ino);
        else
            continue;
        if (!oi || oi->type != ni->type)
            continue;
        if (ni->type == JFS_T_FILE) /* without both checksums assume it changed */
            content = oi->size != ni->size || !oi->sha_valid || !ni->sha_valid || memcmp(oi->sha, ni->sha, 32);
        meta = oi->mode != ni->mode || oi->uid != ni->uid || oi->gid != ni->gid || oi->rdev != ni->rdev ||
               xattrs_differ(oi, ni) ||
               (ni->type == JFS_T_SYMLINK && strcmp(oi->target ? oi->target : "", ni->target ? ni->target : "")) ||
               (ni->type != JFS_T_DIR && !content && jfs_time_cmp(oi->mtime, ni->mtime));
        if (!content && !meta)
            continue;
        if (ni->type == JFS_T_DIR) {
            wdir *w = ni->ino == d->nroot ? jfs_u64map_get(&d->wd, d->oroot) : jfs_u64map_get(&d->wd, ni->ino);
            path = w ? wpath(d, w) : NULL;
        } else {
            path = w_find_binding(d, ni->ino);
        }
        if (!path)
            continue;
        if (content)
            emit(d, JFS_CHANGE_MODIFY, jfs_strdup(path), NULL, ni);
        if (meta && !(content && oi->mode == ni->mode && oi->uid == ni->uid && oi->gid == ni->gid &&
                      !xattrs_differ(oi, ni)))
            emit(d, JFS_CHANGE_ATTRIB, jfs_strdup(path), NULL, ni);
        free(path);
    }
}

int jfs_pending_diff(jfs_fs *fs, jfs_pending *p, jfs_changes *out)
{
    dctx d;
    size_t i;
    memset(&d, 0, sizeof d);
    memset(out, 0, sizeof *out);
    jfs_mutex_lock(&fs->mu);
    d.o = fs;
    d.n = p->nfs;
    d.oroot = fs->root_ino;
    d.nroot = p->nfs->root_ino;
    d.same_fs = !strcmp(fs->uuid, p->nfs->uuid) && d.oroot == d.nroot;
    d.out = out;
    jfs_u64map_init(&d.wd);
    jfs_u64map_init(&d.nparent);
    jfs_u64map_init(&d.created);
    build_wtree(&d, wd_add(&d, d.oroot, 0, NULL));
    build_nparents(&d, d.nroot);
    if (d.same_fs)
        phase_renames(&d);
    phase_deletes(&d, jfs_u64map_get(&d.wd, d.oroot));
    phase_creates(&d, d.nroot);
    phase_modifies(&d);
    jfs_mutex_unlock(&fs->mu);
    for (i = 0; i < d.wd.cap; i++)
        if (d.wd.vals[i])
            wd_free(d.wd.vals[i]);
    for (i = 0; i < d.nparent.cap; i++)
        if (d.nparent.vals[i]) {
            free(((nbinding *)d.nparent.vals[i])->name);
            free(d.nparent.vals[i]);
        }
    jfs_u64map_free(&d.wd);
    jfs_u64map_free(&d.nparent);
    jfs_u64map_free(&d.created);
    return 0;
}
