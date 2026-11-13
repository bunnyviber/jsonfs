/*
 * JSONfs - offline maintenance: integrity verification (fsck), point-in-time
 * extraction, versioning toggle, recompression, schema upgrades, history
 * pruning and version listing.
 * SPDX-License-Identifier: MIT
 */
#include "jfs_internal.h"
#include "jfs_http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* fsck                                                                       */
/* ======================================================================== */
typedef struct fsck_ctx {
    jfs_report_fn report;
    void *cookie;
    int verbose;
    jfs_fsck_result *res;
} fsck_ctx;

static void rep(fsck_ctx *c, int sev, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (sev == 0 && !c->verbose)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (sev == 1)
        c->res->warnings++;
    if (sev == 2)
        c->res->errors++;
    if (c->report)
        c->report(c->cookie, sev, buf);
}

typedef struct ino_info {
    jfs_type type;
    int live;
    uint64_t refs;
    int64_t stored_nlink;
    uint64_t subdirs;
    int reached;
    jv *rec;
} ino_info;

static void check_file_record(fsck_ctx *c, uint64_t ino, const char *what, jv *cur, jv *hist, jv *rec)
{
    jv *src = jfs_record_data_source(cur, hist, rec);
    const char *comp, *sha, *data;
    int64_t size;
    uint8_t want[32], *out = NULL;
    jv *d;
    int rc;
    if (!src) {
        rep(c, 2, "inode %llu (%s): content reference (data_ref) cannot be resolved", (unsigned long long)ino,
            what);
        return;
    }
    comp = jv_get_str(src, "compression");
    sha = jv_get_str(rec, "sha256");
    d = jv_get(src, "data");
    data = d && d->t == JV_STR ? d->s : NULL;
    if (jv_get_i64(rec, "size", &size) || size < 0) {
        rep(c, 2, "inode %llu (%s): invalid size", (unsigned long long)ino, what);
        return;
    }
    if (!comp || !jfs_compressor_find(comp)) {
        rep(c, 2, "inode %llu (%s): unknown compression \"%s\"", (unsigned long long)ino, what, comp ? comp : "");
        return;
    }
    if (!data) {
        rep(c, 2, "inode %llu (%s): missing data", (unsigned long long)ino, what);
        return;
    }
    if (!sha || jfs_unhex(sha, want, 32)) {
        rep(c, 1, "inode %llu (%s): missing or malformed sha256, content cannot be verified",
            (unsigned long long)ino, what);
        sha = NULL;
    }
    rc = jfs_decode_blob(comp, data, d->slen, (uint64_t)size, sha ? want : NULL, &out);
    if (rc == -JFS_ECHECKSUM)
        rep(c, 2, "inode %llu (%s): SHA-256 mismatch, content is corrupt", (unsigned long long)ino, what);
    else if (rc)
        rep(c, 2, "inode %llu (%s): content cannot be decoded with %s: %s", (unsigned long long)ino, what, comp,
            jfs_strerror(rc));
    else {
        c->res->bytes_checked += (uint64_t)size;
        rep(c, 0, "inode %llu (%s): %lld bytes, %s, checksum ok", (unsigned long long)ino, what, (long long)size,
            comp);
    }
    free(out);
}

int jfs_fsck(const char *image, int repair, int verbose, jfs_report_fn report, void *cookie, jfs_fsck_result *res)
{
    fsck_ctx c;
    char *text = NULL, perr[200];
    size_t len, i;
    jv *root, *inodes;
    int64_t i64, schema = 0, root_ino = 0, next_ino = 0;
    uint64_t max_ino = 0;
    jfs_u64map infos;
    jfs_lockfile *lf = NULL;
    int rc;
    const char *s;

    memset(res, 0, sizeof *res);
    c.report = report;
    c.cookie = cookie;
    c.verbose = verbose;
    c.res = res;

    if (jfs_is_url(image) && repair)
        return -JFS_EROFS;
    if (!jfs_is_url(image) && jfs_lockfile_open(image, &lf) == 0 && jfs_lockfile_lock_timeout(lf, JFS_LOCK_ATTACH, repair, jfs_lock_timeout_ms()) != 0) {
        jfs_lockfile_close(lf, 0);
        if (repair)
            return -JFS_ELOCKED;
        rep(&c, 1, "image is currently mounted; results reflect the last committed state");
        lf = NULL;
    }
    {
        char lerr[1200];
        rc = jfs_load_bytes(image, &text, &len, lerr, sizeof lerr);
        if (rc) {
            rep(&c, 2, "%s", lerr);
            goto out_lock;
        }
    }
    root = jv_parse(text, len, perr, sizeof perr);
    free(text);
    if (!root) {
        rep(&c, 2, "not valid JSON: %s", perr);
        rc = -JFS_ECORRUPT;
        goto out_lock;
    }
    /* ---- header ---- */
    s = jv_get_str(root, "format");
    if (!s || strcmp(s, "jsonfs"))
        rep(&c, 2, "header: \"format\" is not \"jsonfs\"");
    if (jv_get_i64(root, "schema_version", &schema))
        rep(&c, 2, "header: missing schema_version");
    else if (schema < JFS_SCHEMA_MIN || schema > JFS_SCHEMA_LATEST)
        rep(&c, 2, "header: unsupported schema_version %lld", (long long)schema);
    else if (schema < JFS_SCHEMA_LATEST)
        rep(&c, 1, "header: schema %lld is outdated (latest %d); run 'jsonfs upgrade'", (long long)schema,
            JFS_SCHEMA_LATEST);
    if (!jfs_uuid_valid(jv_get_str(root, "uuid")))
        rep(&c, 2, "header: missing or invalid uuid");
    {
        jfs_time t1, t2;
        if (!jv_get_str(root, "created") || jfs_time_parse(jv_get_str(root, "created"), &t1))
            rep(&c, 2, "header: invalid \"created\" timestamp");
        else if (!jv_get_str(root, "modified") || jfs_time_parse(jv_get_str(root, "modified"), &t2))
            rep(&c, 2, "header: invalid \"modified\" timestamp");
        else if (jfs_time_cmp(t2, t1) < 0)
            rep(&c, 1, "header: \"modified\" is earlier than \"created\"");
    }
    s = jv_get_str(root, "default_compression");
    if (!s || !jfs_compressor_find(s))
        rep(&c, 2, "header: unknown default_compression \"%s\"", s ? s : "");
    if (!jv_get(root, "versioning") || jv_get(root, "versioning")->t != JV_BOOL)
        rep(&c, 1, "header: \"versioning\" flag missing or not a boolean");
    if (schema >= 2 && jv_get_i64(root, "generation", &i64))
        rep(&c, 1, "header: missing generation counter");
    if (jv_get_i64(root, "root_inode", &root_ino) || root_ino <= 0)
        rep(&c, 2, "header: missing root_inode");
    if (jv_get_i64(root, "next_inode", &next_ino))
        rep(&c, 1, "header: missing next_inode");

    inodes = jv_get(root, "inodes");
    if (!inodes || inodes->t != JV_OBJ) {
        rep(&c, 2, "\"inodes\" object missing");
        jv_free(root);
        rc = 0;
        goto out_lock;
    }
    jfs_u64map_init(&infos);
    /* ---- per inode ---- */
    for (i = 0; i < inodes->n; i++) {
        char *e, err[256];
        unsigned long long ino = strtoull(inodes->keys[i], &e, 10);
        jv *cur = inodes->items[i], *hist;
        ino_info *ii;
        jfs_inode *in;
        if (*e || !ino) {
            rep(&c, 2, "invalid inode key \"%s\"", inodes->keys[i]);
            continue;
        }
        if (ino > max_ino)
            max_ino = ino;
        res->inodes++;
        ii = jfs_calloc(1, sizeof *ii);
        ii->rec = cur;
        ii->stored_nlink = -1;
        jfs_u64map_put(&infos, ino, ii);
        hist = jv_get(cur, "history");
        if (hist && hist->t != JV_ARR) {
            rep(&c, 2, "inode %llu: \"history\" is not an array", ino);
            hist = NULL;
        }
        /* history records */
        if (hist) {
            size_t k;
            jfs_time prev = {0, 0};
            for (k = 0; k < hist->n; k++) {
                jv *r = hist->items[k];
                char what[64];
                jfs_time t;
                res->records_checked++;
                snprintf(what, sizeof what, "history #%lu", (unsigned long)k);
                if (!jv_get_str(r, "version_time") || jfs_time_parse(jv_get_str(r, "version_time"), &t)) {
                    rep(&c, 2, "inode %llu (%s): missing version_time", ino, what);
                    continue;
                }
                if (jfs_time_cmp(t, prev) < 0)
                    rep(&c, 1, "inode %llu (%s): history is not in chronological order", ino, what);
                prev = t;
                if (jv_get_bool(r, "deleted", 0))
                    continue;
                err[0] = 0;
                in = jfs_inode_from_record(r, ino, (int)schema, 0, err, sizeof err);
                if (!in) {
                    rep(&c, 2, "%s (%s)", err, what);
                    continue;
                }
                if (in->type == JFS_T_FILE)
                    check_file_record(&c, ino, what, cur, hist, r);
                jfs_inode_free(in);
            }
        }
        res->records_checked++;
        if (jv_get_bool(cur, "deleted", 0)) {
            ii->type = jfs_type_from_name(jv_get_str(cur, "type"));
            rep(&c, 0, "inode %llu: delete marker", ino);
            continue;
        }
        err[0] = 0;
        in = jfs_inode_from_record(cur, ino, (int)schema, 0, err, sizeof err);
        if (!in) {
            rep(&c, 2, "%s", err);
            continue;
        }
        if (in->blob_ref) {
            rep(&c, 2, "inode %llu: current record has no data of its own", ino);
        }
        ii->type = in->type;
        ii->live = 1;
        if (!jv_get_i64(cur, "nlink", &i64))
            ii->stored_nlink = i64;
        if (in->type == JFS_T_FILE) {
            res->files_checked++;
            if (!in->blob_ref)
                check_file_record(&c, ino, "current", cur, hist, cur);
        }
        jfs_inode_free(in);
    }
    /* ---- structure ---- */
    for (i = 0; i < infos.cap; i++) {
        ino_info *d = infos.vals[i];
        jv *ents;
        size_t k;
        if (!d || !d->live || d->type != JFS_T_DIR)
            continue;
        ents = jv_get(d->rec, "entries");
        for (k = 0; ents && ents->t == JV_OBJ && k < ents->n; k++) {
            uint64_t t = ents->items[k]->t == JV_NUM ? (uint64_t)ents->items[k]->i : 0;
            ino_info *ti = jfs_u64map_get(&infos, t);
            const char *nm = ents->keys[k];
            if (!*nm || strchr(nm, '/') || !strcmp(nm, ".") || !strcmp(nm, ".."))
                rep(&c, 2, "directory %llu: invalid entry name \"%s\"", (unsigned long long)infos.keys[i], nm);
            if (!ti || !ti->live) {
                rep(&c, 2, "directory %llu: entry \"%s\" points to %s inode %llu", (unsigned long long)infos.keys[i],
                    nm, ti ? "deleted" : "missing", (unsigned long long)t);
                continue;
            }
            ti->refs++;
            if (ti->type == JFS_T_DIR)
                d->subdirs++;
        }
    }
    {
        ino_info *r = jfs_u64map_get(&infos, (uint64_t)root_ino);
        if (!r || !r->live || r->type != JFS_T_DIR)
            rep(&c, 2, "root inode %lld is missing or not a directory", (long long)root_ino);
        else {
            /* reachability walk */
            uint64_t *stack = jfs_malloc((infos.count + 1) * sizeof(uint64_t));
            size_t sp = 0;
            stack[sp++] = (uint64_t)root_ino;
            r->reached = 1;
            while (sp) {
                ino_info *d = jfs_u64map_get(&infos, stack[--sp]);
                jv *ents = jv_get(d->rec, "entries");
                size_t k;
                for (k = 0; ents && ents->t == JV_OBJ && k < ents->n; k++) {
                    uint64_t t = ents->items[k]->t == JV_NUM ? (uint64_t)ents->items[k]->i : 0;
                    ino_info *ti = jfs_u64map_get(&infos, t);
                    if (!ti || !ti->live || ti->reached)
                        continue;
                    ti->reached = 1;
                    if (ti->type == JFS_T_DIR)
                        stack[sp++] = t;
                }
            }
            free(stack);
        }
    }
    for (i = 0; i < infos.cap; i++) {
        ino_info *ii = infos.vals[i];
        uint64_t ino = infos.keys[i];
        if (!ii || !ii->live)
            continue;
        if (ino != (uint64_t)root_ino && ii->type == JFS_T_DIR && ii->refs > 1)
            rep(&c, 2, "directory %llu has %llu names (hard-linked directory)", (unsigned long long)ino,
                (unsigned long long)ii->refs);
        if (!ii->reached)
            rep(&c, 2, "inode %llu (%s) is not reachable from the root (orphan)", (unsigned long long)ino,
                jfs_type_name(ii->type));
        if (ii->stored_nlink >= 0) {
            uint64_t want = ii->type == JFS_T_DIR ? 2 + ii->subdirs : ii->refs;
            if ((uint64_t)ii->stored_nlink != want)
                rep(&c, 1, "inode %llu: stored nlink %lld, actual %llu", (unsigned long long)ino,
                    (long long)ii->stored_nlink, (unsigned long long)want);
        }
    }
    if (next_ino && (uint64_t)next_ino <= max_ino)
        rep(&c, 2, "header: next_inode %lld is not above the highest inode %llu", (long long)next_ino,
            (unsigned long long)max_ino);
    for (i = 0; i < infos.cap; i++)
        free(infos.vals[i]);
    jfs_u64map_free(&infos);
    jv_free(root);
    rc = 0;

    if (repair && (res->errors || res->warnings)) {
        /* Reload tolerantly (drops broken records and dangling entries,
         * recomputes link counts), then rescue orphans into /lost+found. */
        jfs_open_opts o;
        jfs_fs *fs;
        char err[256];
        if (lf) {
            jfs_lockfile_close(lf, 0);
            lf = NULL;
        }
        jfs_open_opts_init(&o);
        o.lockmode = JFS_LOCKMODE_OFFLINE;
        o.repair = 1;
        rc = jfs_open(image, &o, &fs, err, sizeof err);
        if (rc) {
            rep(&c, 2, "repair failed: %s", err);
            return rc;
        }
        jfs_mutex_lock(&fs->mu);
        {
            /* reachability again on the loaded tree */
            jfs_u64map seen;
            uint64_t *stack = jfs_malloc((fs->inodes.count + 1) * sizeof(uint64_t));
            size_t sp = 0;
            jfs_inode *lost = NULL;
            jfs_u64map_init(&seen);
            stack[sp++] = fs->root_ino;
            jfs_u64map_put(&seen, fs->root_ino, (void *)1);
            while (sp) {
                jfs_inode *d = jfs_lookup_ino(fs, stack[--sp]);
                size_t k;
                if (!d || d->type != JFS_T_DIR)
                    continue;
                for (k = 0; k < d->entries->cap; k++) {
                    uint64_t t;
                    jfs_inode *ch;
                    if (!d->entries->keys[k])
                        continue;
                    t = d->entries->vals[k];
                    ch = jfs_lookup_ino(fs, t);
                    if (!ch || jfs_u64map_get(&seen, t))
                        continue;
                    jfs_u64map_put(&seen, t, (void *)1);
                    if (ch->type == JFS_T_DIR)
                        stack[sp++] = t;
                }
            }
            for (i = 0; i < fs->inodes.cap; i++) {
                jfs_inode *in = fs->inodes.vals[i];
                char nm[64];
                if (!in || in->deleted || jfs_u64map_get(&seen, in->ino))
                    continue;
                if (!lost) {
                    jfs_inode *r = jfs_lookup_ino(fs, fs->root_ino);
                    uint64_t lino;
                    if (jfs_strmap_get(r->entries, "lost+found", &lino))
                        lost = jfs_lookup_ino(fs, lino);
                    if (!lost || lost->type != JFS_T_DIR) {
                        lost = jfs_inode_new(fs, JFS_T_DIR);
                        lost->mode = 0700;
                        jfs_strmap_put(r->entries, "lost+found", lost->ino);
                    }
                }
                snprintf(nm, sizeof nm, "#%llu", (unsigned long long)in->ino);
                jfs_strmap_put(lost->entries, nm, in->ino);
                res->repaired++;
                rep(&c, 1, "repair: moved orphan inode %llu to /lost+found/%s", (unsigned long long)in->ino, nm);
            }
            free(stack);
            jfs_u64map_free(&seen);
        }
        jfs_recount_links(fs);
        fs->dirty = 1;
        res->repaired += 1;
        jfs_mutex_unlock(&fs->mu);
        rc = jfs_close(fs);
        if (rc)
            rep(&c, 2, "repair: cannot write image: %s", jfs_strerror(rc));
        else
            rep(&c, 1, "repair: image rewritten (link counts, dangling entries and header fixed)");
    }
out_lock:
    if (lf)
        jfs_lockfile_close(lf, 0);
    return rc;
}

/* ======================================================================== */
/* helpers                                                                    */
/* ======================================================================== */
static int open_offline(const char *image, jfs_fs **fs, const char *compression)
{
    jfs_open_opts o;
    char err[256];
    int rc;
    jfs_open_opts_init(&o);
    o.lockmode = JFS_LOCKMODE_OFFLINE;
    o.compression = compression;
    rc = jfs_open(image, &o, fs, err, sizeof err);
    if (rc)
        jfs_log("%s", err);
    return rc;
}

/* ======================================================================== */
/* snapshot (point-in-time extraction)                                        */
/* ======================================================================== */
int jfs_snapshot(const char *image, jfs_time at, const char *out_image, int keep_versioning, const char *label,
                 int overwrite)
{
    jfs_open_opts o;
    jfs_fs *fs;
    char err[256];
    jfs_buf out;
    size_t i;
    int rc;
    if (jfs_is_url(out_image))
        return -JFS_EROFS;
    if (!overwrite && jfs_file_exists(out_image))
        return -JFS_EEXIST;
    jfs_open_opts_init(&o);
    o.has_at = 1;
    o.at = at;
    rc = jfs_open(image, &o, &fs, err, sizeof err);
    if (rc) {
        jfs_log("%s", err);
        return rc;
    }
    jfs_uuid4(fs->uuid);
    fs->created = fs->modified = jfs_now();
    fs->generation = 1;
    fs->versioning = keep_versioning;
    if (label) {
        free(fs->label);
        fs->label = jfs_strdup(label);
    }
    jv_free(fs->extra_header);
    fs->extra_header = jv_obj();
    {
        char tb[JFS_TIME_STRLEN];
        jfs_time_format(at, tb);
        jv_obj_set(fs->extra_header, "snapshot_of", jv_str(image));
        jv_obj_set(fs->extra_header, "snapshot_time", jv_str(tb));
    }
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (in) {
            in->persisted = 0;
            jv_free(in->history);
            in->history = NULL;
        }
    }
    jfs_buf_init(&out);
    jfs_serialize(fs, &out);
    rc = jfs_write_file_atomic(out_image, out.data, out.len);
    jfs_buf_free(&out);
    jfs_fs_free(fs);
    return rc;
}

/* ======================================================================== */
/* versioning toggle                                                          */
/* ======================================================================== */
int jfs_set_versioning(const char *image, int enable, int purge_history, uint64_t *purged)
{
    jfs_fs *fs;
    size_t i;
    uint64_t n = 0;
    int rc = open_offline(image, &fs, NULL);
    if (rc)
        return rc;
    jfs_mutex_lock(&fs->mu);
    fs->versioning = enable ? 1 : 0;
    if (purge_history) {
        for (i = 0; i < fs->inodes.cap; i++) {
            jfs_inode *in = fs->inodes.vals[i];
            if (!in)
                continue;
            if (in->history) {
                n += in->history->n;
                jv_free(in->history);
                in->history = NULL;
            }
            in->hist_needs_data = 0;
        }
        /* delete markers: drop entirely */
        for (i = 0; i < fs->inodes.cap;) {
            jfs_inode *in = fs->inodes.vals[i];
            if (in && in->deleted) {
                jfs_u64map_del(&fs->inodes, in->ino);
                jfs_inode_free(in);
                n++;
                continue;
            }
            i++;
        }
    }
    fs->dirty = 1;
    jfs_mutex_unlock(&fs->mu);
    if (purged)
        *purged = n;
    return jfs_close(fs);
}

/* ======================================================================== */
/* recompress                                                                 */
/* ======================================================================== */
int jfs_recompress(const char *image, const char *algo, int set_default, int include_history, uint64_t *count,
                   uint64_t *bytes_before, uint64_t *bytes_after)
{
    jfs_fs *fs;
    const jfs_compressor *comp;
    size_t i;
    uint64_t cnt = 0, before = 0, after = 0;
    int rc;
    if (jfs_compression_parse(algo, &comp, NULL))
        return -JFS_EINVAL;
    rc = open_offline(image, &fs, algo);
    if (rc)
        return rc;
    jfs_mutex_lock(&fs->mu);
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        if (!in)
            continue;
        if (!in->deleted && in->type == JFS_T_FILE) {
            before += in->blob_len;
            rc = jfs_inode_encode(fs, in, algo);
            if (rc) {
                jfs_log("inode %llu: %s", (unsigned long long)in->ino, jfs_strerror(rc));
                goto fail;
            }
            after += in->blob_len;
            free(in->data);
            in->data = NULL;
            in->data_loaded = 0;
            cnt++;
        }
        if (include_history && in->history) {
            size_t k;
            for (k = 0; k < in->history->n; k++) {
                jv *r = in->history->items[k], *d = jv_get(r, "data");
                const char *c = jv_get_str(r, "compression");
                int64_t size;
                uint8_t *raw;
                char *b64, *name;
                size_t blen;
                if (!d || d->t != JV_STR || !c || jv_get_i64(r, "size", &size))
                    continue;
                before += d->slen;
                rc = jfs_decode_blob(c, d->s, d->slen, (uint64_t)size, NULL, &raw);
                if (rc) {
                    jfs_log("inode %llu history #%lu: %s", (unsigned long long)in->ino, (unsigned long)k,
                            jfs_strerror(rc));
                    goto fail;
                }
                rc = jfs_encode_blob(algo, raw, (size_t)size, &b64, &blen, &name);
                free(raw);
                if (rc)
                    goto fail;
                jv_obj_set(r, "compression", jv_str(name));
                jv_obj_set(r, "data", jv_str_owned(b64, blen));
                free(name);
                after += blen;
                cnt++;
            }
        }
    }
    if (set_default) {
        free(fs->default_comp);
        fs->default_comp = jfs_strdup(comp->name);
    }
    fs->dirty = 1;
    jfs_mutex_unlock(&fs->mu);
    if (count)
        *count = cnt;
    if (bytes_before)
        *bytes_before = before;
    if (bytes_after)
        *bytes_after = after;
    return jfs_close(fs);
fail:
    fs->dirty = 0; /* do not write a half-converted image */
    jfs_mutex_unlock(&fs->mu);
    jfs_close(fs);
    return rc;
}

/* ======================================================================== */
/* schema upgrade                                                             */
/* ======================================================================== */
int jfs_upgrade(const char *image, int target_schema, int *from_schema)
{
    jfs_fs *fs;
    size_t i;
    int rc;
    if (target_schema <= 0)
        target_schema = JFS_SCHEMA_LATEST;
    if (target_schema > JFS_SCHEMA_LATEST)
        return -JFS_EVERSION;
    rc = open_offline(image, &fs, NULL);
    if (rc)
        return rc;
    jfs_mutex_lock(&fs->mu);
    if (from_schema)
        *from_schema = fs->schema;
    if (fs->schema >= target_schema) {
        int same = fs->schema == target_schema;
        jfs_mutex_unlock(&fs->mu);
        jfs_close(fs);
        return same ? 0 : -JFS_EVERSION; /* downgrades are not supported */
    }
    /* 1 -> 2: adds the generation counter (multi-attach), xattrs and btime.
     * btime of existing inodes/history records is initialised from ctime. */
    for (i = 0; i < fs->inodes.cap; i++) {
        jfs_inode *in = fs->inodes.vals[i];
        size_t k;
        if (!in || !in->history)
            continue;
        for (k = 0; k < in->history->n; k++) {
            jv *r = in->history->items[k];
            const char *ct = jv_get_str(r, "ctime");
            if (!jv_get(r, "btime") && ct && !jv_get_bool(r, "deleted", 0))
                jv_obj_set(r, "btime", jv_str(ct));
        }
    }
    fs->schema = target_schema;
    fs->dirty = 1;
    jfs_mutex_unlock(&fs->mu);
    return jfs_close(fs);
}

/* ======================================================================== */
/* prune history                                                              */
/* ======================================================================== */
static int rec_time(jv *r, jfs_time *t)
{
    const char *s = jv_get_str(r, "version_time");
    return s ? jfs_time_parse(s, t) : -1;
}

int jfs_prune_history(const char *image, jfs_time before, uint64_t *removed)
{
    jfs_fs *fs;
    size_t i;
    uint64_t n = 0;
    int rc = open_offline(image, &fs, NULL);
    if (rc)
        return rc;
    jfs_mutex_lock(&fs->mu);
    for (i = 0; i < fs->inodes.cap;) {
        jfs_inode *in = fs->inodes.vals[i];
        size_t k, keep_from = 0;
        if (!in) {
            i++;
            continue;
        }
        if (in->history) {
            /* record k is valid until record k+1 (or the current record) starts */
            for (k = 0; k < in->history->n; k++) {
                jv *next = k + 1 < in->history->n ? in->history->items[k + 1] : NULL;
                jfs_time t;
                if (next) {
                    if (rec_time(next, &t))
                        break;
                } else {
                    t = in->vtime;
                }
                if (jfs_time_cmp(t, before) <= 0)
                    keep_from = k + 1;
                else
                    break;
            }
            for (k = 0; k < keep_from; k++)
                jv_free(in->history->items[k]);
            if (keep_from) {
                memmove(in->history->items, in->history->items + keep_from,
                        (in->history->n - keep_from) * sizeof(jv *));
                in->history->n -= keep_from;
                n += keep_from;
            }
            if (!in->history->n) {
                jv_free(in->history);
                in->history = NULL;
                in->hist_needs_data = 0;
            }
        }
        if (in->deleted && !in->history && jfs_time_cmp(in->vtime, before) <= 0) {
            jfs_u64map_del(&fs->inodes, in->ino);
            jfs_inode_free(in);
            n++;
            continue;
        }
        i++;
    }
    fs->dirty = 1;
    jfs_mutex_unlock(&fs->mu);
    if (removed)
        *removed = n;
    return jfs_close(fs);
}

/* ======================================================================== */
/* version listing                                                            */
/* ======================================================================== */
static void fill_vi(jv *cur, jv *hist, jv *r, int is_current, jfs_version_info *vi)
{
    const char *s;
    int64_t i64;
    jv *src;
    memset(vi, 0, sizeof *vi);
    s = jv_get_str(r, "version_time");
    if (!s || jfs_time_parse(s, &vi->vtime)) {
        s = jv_get_str(r, "ctime");
        if (s)
            jfs_time_parse(s, &vi->vtime);
    }
    vi->deleted = jv_get_bool(r, "deleted", 0);
    vi->is_current = is_current;
    snprintf(vi->type, sizeof vi->type, "%s", jv_get_str(r, "type") ? jv_get_str(r, "type") : "?");
    if (!jv_get_i64(r, "size", &i64))
        vi->size = (uint64_t)i64;
    snprintf(vi->sha256, sizeof vi->sha256, "%s", jv_get_str(r, "sha256") ? jv_get_str(r, "sha256") : "");
    src = jfs_record_data_source(cur, hist, r);
    s = src ? jv_get_str(src, "compression") : NULL;
    snprintf(vi->compression, sizeof vi->compression, "%s", s ? s : "");
}

int jfs_list_versions(const char *image, const char *path, const jfs_time *at, jfs_version_fn fn, void *cookie)
{
    jfs_open_opts o;
    jfs_fs *fs;
    jfs_inode *in;
    char err[256], *text;
    size_t len, k;
    uint64_t ino;
    jv *root, *cur, *hist;
    char key[32];
    int rc;
    jfs_open_opts_init(&o);
    o.lockmode = JFS_LOCKMODE_NONE;
    o.readonly = 1;
    if (at) {
        o.has_at = 1;
        o.at = *at;
    }
    rc = jfs_open(image, &o, &fs, err, sizeof err);
    if (rc) {
        jfs_log("%s", err);
        return rc;
    }
    rc = jfs_resolve(fs, path, &in);
    ino = rc ? 0 : in->ino;
    jfs_fs_free(fs);
    if (rc)
        return rc;
    rc = jfs_load_bytes(image, &text, &len, err, sizeof err);
    if (rc) {
        jfs_log("%s", err);
        return rc;
    }
    root = jv_parse(text, len, NULL, 0);
    free(text);
    if (!root)
        return -JFS_ECORRUPT;
    snprintf(key, sizeof key, "%llu", (unsigned long long)ino);
    cur = jv_get(jv_get(root, "inodes"), key);
    if (!cur) {
        jv_free(root);
        return -JFS_ENOENT;
    }
    hist = jv_get(cur, "history");
    for (k = 0; hist && hist->t == JV_ARR && k < hist->n; k++) {
        jfs_version_info vi;
        fill_vi(cur, hist, hist->items[k], 0, &vi);
        if (fn(cookie, &vi))
            break;
    }
    {
        jfs_version_info vi;
        fill_vi(cur, hist, cur, 1, &vi);
        fn(cookie, &vi);
    }
    jv_free(root);
    return 0;
}
