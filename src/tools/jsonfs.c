/*
 * jsonfs - offline maintenance toolkit for JSONfs images.
 *
 * Also usable as mkfs.jsonfs / fsck.jsonfs when invoked under those names.
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

#include "../lib/jfs_codec.h"
#include "../lib/jfs_fs.h"
#include "../lib/jfs_http.h"
#include "../lib/jfs_archive.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static const char *prog = "jsonfs";
static int g_overwrite; /* add -f: replace existing non-directories */
static int img_type(jfs_fs *fs, const char *path);

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int fail(int rc, const char *what)
{
    fprintf(stderr, "%s: %s: %s\n", prog, what, jfs_strerror(rc));
    return rc == -JFS_ELOCKED ? 3 : 1;
}

/* ------------------------------------------------------------------------ */
/* Tiny option parser (portable; no getopt_long on MSVC)                      */
/* ------------------------------------------------------------------------ */
typedef struct args {
    int argc;
    char **argv;
    int i;
    char **pos;
    int npos;
} args;

static void args_init(args *a, int argc, char **argv)
{
    a->argc = argc;
    a->argv = argv;
    a->i = 0;
    a->pos = calloc((size_t)argc + 1, sizeof(char *));
    a->npos = 0;
}

/* Returns the next option name ("-x" / "--long") or NULL when done;
 * positional arguments are collected in a->pos. */
static const char *args_next(args *a)
{
    while (a->i < a->argc) {
        char *s = a->argv[a->i++];
        if (!strcmp(s, "--")) {
            while (a->i < a->argc)
                a->pos[a->npos++] = a->argv[a->i++];
            return NULL;
        }
        if (s[0] == '-' && s[1])
            return s;
        a->pos[a->npos++] = s;
    }
    return NULL;
}

static const char *args_value(args *a, const char *opt)
{
    if (a->i >= a->argc)
        die("option %s needs a value", opt);
    return a->argv[a->i++];
}

static int is(const char *o, const char *s, const char *l)
{
    return (s && !strcmp(o, s)) || (l && !strcmp(o, l));
}

static jfs_time parse_time_arg(const char *s)
{
    jfs_time t;
    if (jfs_time_parse_user(s, &t))
        die("cannot parse time \"%s\" (use e.g. 2026-11-11T10:00:00Z, 2026-11-11, @1700000000, -2h)", s);
    return t;
}

static void fmt_time(jfs_time t, char *buf)
{
    jfs_time_format(t, buf);
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                   */
/* ------------------------------------------------------------------------ */
static int cmd_mkfs(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_mkfs_opts mo;
    int force = 0, rc;
    memset(&mo, 0, sizeof mo);
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-L", "--label"))
            mo.label = args_value(&a, o);
        else if (is(o, "-c", "--compression"))
            mo.compression = args_value(&a, o);
        else if (is(o, "-V", "--versioning"))
            mo.versioning = 1;
        else if (is(o, NULL, "--schema"))
            mo.schema = atoi(args_value(&a, o));
        else if (is(o, "-U", "--uuid"))
            mo.uuid = args_value(&a, o);
        else if (is(o, "-f", "--force"))
            force = 1;
        else if (is(o, NULL, "--root-mode"))
            mo.root_mode = (uint32_t)strtoul(args_value(&a, o), NULL, 8);
        else if (is(o, NULL, "--root-owner")) {
            const char *v = args_value(&a, o);
            mo.root_uid = (uint32_t)strtoul(v, NULL, 10);
            mo.root_gid = strchr(v, ':') ? (uint32_t)strtoul(strchr(v, ':') + 1, NULL, 10) : mo.root_uid;
        } else
            die("mkfs: unknown option %s", o);
    }
    if (a.npos != 1)
        die("usage: jsonfs mkfs [-L label] [-c compression] [-V] [--schema N] [-U uuid] [-f] IMAGE");
    if (jfs_is_url(a.pos[0]))
        die("cannot create an image at a URL; create it locally and upload it");
#ifndef _WIN32
    if (!mo.root_uid && !mo.root_gid && getuid() != 0) {
        mo.root_uid = (uint32_t)getuid();
        mo.root_gid = (uint32_t)getgid();
    }
#endif
    if (mo.compression && jfs_compression_parse(mo.compression, NULL, NULL))
        die("unknown compression \"%s\" (see 'jsonfs compressors')", mo.compression);
    rc = jfs_mkfs(a.pos[0], &mo, force);
    if (rc == -JFS_EEXIST)
        die("%s already exists (use -f to overwrite)", a.pos[0]);
    if (rc)
        return fail(rc, a.pos[0]);
    printf("created JSONfs image %s (schema %d, compression %s, versioning %s)\n", a.pos[0],
           mo.schema ? mo.schema : JFS_SCHEMA_LATEST, mo.compression ? mo.compression : JFS_DEFAULT_COMPRESSION,
           mo.versioning ? "on" : "off");
    return 0;
}

static int open_ro(const char *image, const char *at, jfs_fs **fs)
{
    jfs_open_opts o;
    char err[300];
    int rc;
    jfs_open_opts_init(&o);
    o.readonly = 1;
    o.lockmode = JFS_LOCKMODE_NONE;
    if (at) {
        o.has_at = 1;
        o.at = parse_time_arg(at);
    }
    rc = jfs_open(image, &o, fs, err, sizeof err);
    if (rc)
        die("%s", err);
    return 0;
}

static int open_rw(const char *image, jfs_fs **fs)
{
    jfs_open_opts o;
    char err[300];
    int rc;
    jfs_open_opts_init(&o);
    o.lockmode = JFS_LOCKMODE_OFFLINE;
    o.commit_interval_ms = 1000000000; /* commit once at the end */
    rc = jfs_open(image, &o, fs, err, sizeof err);
    if (rc)
        die("%s", err);
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_fs *fs;
    jfs_info in;
    char t1[JFS_TIME_STRLEN], t2[JFS_TIME_STRLEN];
    int json = 0;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, NULL, "--at"))
            at = args_value(&a, o);
        else if (is(o, "-j", "--json"))
            json = 1;
        else
            die("info: unknown option %s", o);
    }
    if (a.npos != 1)
        die("usage: jsonfs info [--at TIME] [--json] IMAGE");
    open_ro(a.pos[0], at, &fs);
    jfs_get_info(fs, &in);
    fmt_time(in.created, t1);
    fmt_time(in.modified, t2);
    if (json) {
        printf("{\"uuid\":\"%s\",\"label\":\"%s\",\"schema_version\":%d,\"created\":\"%s\",\"modified\":\"%s\","
               "\"generation\":%llu,\"default_compression\":\"%s\",\"versioning\":%s,\"inodes\":%llu,"
               "\"files\":%llu,\"directories\":%llu,\"symlinks\":%llu,\"other\":%llu,\"delete_markers\":%llu,"
               "\"history_records\":%llu,\"logical_bytes\":%llu}\n",
               in.uuid, in.label, in.schema, t1, t2, (unsigned long long)in.generation, in.default_compression,
               in.versioning ? "true" : "false", (unsigned long long)in.inodes, (unsigned long long)in.files,
               (unsigned long long)in.dirs, (unsigned long long)in.symlinks, (unsigned long long)in.others,
               (unsigned long long)in.deleted_markers, (unsigned long long)in.history_records,
               (unsigned long long)in.logical_bytes);
    } else {
        printf("image:               %s\n", a.pos[0]);
        printf("uuid:                %s\n", in.uuid);
        printf("label:               %s\n", in.label);
        printf("schema version:      %d%s\n", in.schema, in.schema < JFS_SCHEMA_LATEST ? " (upgrade available)" : "");
        printf("created:             %s\n", t1);
        printf("last modified:       %s\n", t2);
        printf("generation:          %llu\n", (unsigned long long)in.generation);
        printf("default compression: %s\n", in.default_compression);
        printf("versioning:          %s\n", in.versioning ? "on" : "off");
        if (in.is_view) {
            fmt_time(in.view_time, t1);
            printf("viewing state at:    %s\n", t1);
        }
        printf("inodes:              %llu (%llu files, %llu directories, %llu symlinks, %llu other)\n",
               (unsigned long long)in.inodes, (unsigned long long)in.files, (unsigned long long)in.dirs,
               (unsigned long long)in.symlinks, (unsigned long long)in.others);
        printf("file content:        %llu bytes\n", (unsigned long long)in.logical_bytes);
        printf("history records:     %llu\n", (unsigned long long)in.history_records);
        printf("delete markers:      %llu\n", (unsigned long long)in.deleted_markers);
    }
    jfs_close(fs);
    return 0;
}

static void fsck_report(void *cookie, int sev, const char *msg)
{
    static const char *tag[] = {"info", "WARNING", "ERROR"};
    (void)cookie;
    printf("%s: %s\n", tag[sev], msg);
}

static int cmd_fsck(int argc, char **argv)
{
    args a;
    const char *o;
    int repair = 0, verbose = 0, rc;
    jfs_fsck_result r;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-r", "--repair") || is(o, "-y", NULL) || is(o, "-p", NULL))
            repair = 1;
        else if (is(o, "-v", "--verbose"))
            verbose = 1;
        else if (is(o, "-n", "--no-repair") || is(o, "-f", NULL) || is(o, "-a", NULL))
            ;
        else
            die("fsck: unknown option %s", o);
    }
    if (a.npos != 1)
        die("usage: jsonfs fsck [-r|--repair] [-v] IMAGE");
    rc = jfs_fsck(a.pos[0], repair, verbose, fsck_report, NULL, &r);
    if (rc && rc != -JFS_ECORRUPT)
        return fail(rc, a.pos[0]);
    printf("%s: %llu inodes, %llu records, %llu files, %llu bytes verified: %llu error(s), %llu warning(s)%s\n",
           a.pos[0], (unsigned long long)r.inodes, (unsigned long long)r.records_checked,
           (unsigned long long)r.files_checked, (unsigned long long)r.bytes_checked, (unsigned long long)r.errors,
           (unsigned long long)r.warnings, r.repaired ? ", repaired" : "");
    /* fsck(8) style exit codes: 0 clean, 1 corrected, 4 uncorrected errors */
    if (r.errors && !r.repaired)
        return 4;
    if (r.repaired)
        return 1;
    return 0;
}

static int cmd_snapshot(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL, *label = NULL;
    int keep = 0, force = 0, rc;
    jfs_time t;
    char tb[JFS_TIME_STRLEN];
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else if (is(o, "-V", "--keep-versioning"))
            keep = 1;
        else if (is(o, "-L", "--label"))
            label = args_value(&a, o);
        else if (is(o, "-f", "--force"))
            force = 1;
        else
            die("snapshot: unknown option %s", o);
    }
    if (a.npos != 2 || !at)
        die("usage: jsonfs snapshot --at TIME [--keep-versioning] [-L label] [-f] IMAGE OUTPUT");
    t = parse_time_arg(at);
    rc = jfs_snapshot(a.pos[0], t, a.pos[1], keep, label, force);
    if (rc == -JFS_EEXIST)
        die("%s already exists (use -f to overwrite)", a.pos[1]);
    if (rc)
        return fail(rc, a.pos[0]);
    fmt_time(t, tb);
    printf("extracted state of %s at %s into %s\n", a.pos[0], tb, a.pos[1]);
    return 0;
}

static int cmd_versioning(int argc, char **argv)
{
    args a;
    const char *o;
    int purge = 0, rc;
    uint64_t purged = 0;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, NULL, "--purge"))
            purge = 1;
        else
            die("versioning: unknown option %s", o);
    }
    if (a.npos != 2)
        die("usage: jsonfs versioning on|off|status [--purge] IMAGE");
    if (!strcmp(a.pos[0], "status")) {
        jfs_fs *fs;
        jfs_info in;
        open_ro(a.pos[1], NULL, &fs);
        jfs_get_info(fs, &in);
        printf("versioning is %s (%llu history records, %llu delete markers)\n", in.versioning ? "on" : "off",
               (unsigned long long)in.history_records, (unsigned long long)in.deleted_markers);
        jfs_close(fs);
        return 0;
    }
    if (strcmp(a.pos[0], "on") && strcmp(a.pos[0], "off"))
        die("versioning: expected on, off or status");
    rc = jfs_set_versioning(a.pos[1], !strcmp(a.pos[0], "on"), purge, &purged);
    if (rc)
        return fail(rc, a.pos[1]);
    printf("versioning turned %s for %s", a.pos[0], a.pos[1]);
    if (purge)
        printf(" (%llu history records/markers purged)", (unsigned long long)purged);
    printf("\n");
    return 0;
}

static int cmd_recompress(int argc, char **argv)
{
    args a;
    const char *o, *algo = NULL;
    int set_default = 0, hist = 0, rc;
    uint64_t n, b0, b1;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-c", "--compression"))
            algo = args_value(&a, o);
        else if (is(o, "-d", "--set-default"))
            set_default = 1;
        else if (is(o, "-H", "--history"))
            hist = 1;
        else
            die("recompress: unknown option %s", o);
    }
    if (a.npos != 1 || !algo)
        die("usage: jsonfs recompress -c ALGORITHM[:LEVEL] [--set-default] [--history] IMAGE");
    if (jfs_compression_parse(algo, NULL, NULL))
        die("unknown compression \"%s\" (see 'jsonfs compressors')", algo);
    rc = jfs_recompress(a.pos[0], algo, set_default, hist, &n, &b0, &b1);
    if (rc)
        return fail(rc, a.pos[0]);
    printf("recompressed %llu blob(s) with %s: %llu -> %llu base64 bytes%s\n", (unsigned long long)n, algo,
           (unsigned long long)b0, (unsigned long long)b1, set_default ? " (now the default)" : "");
    return 0;
}

static int cmd_upgrade(int argc, char **argv)
{
    args a;
    const char *o;
    int to = 0, from = 0, rc;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, NULL, "--to"))
            to = atoi(args_value(&a, o));
        else
            die("upgrade: unknown option %s", o);
    }
    if (a.npos != 1)
        die("usage: jsonfs upgrade [--to N] IMAGE");
    rc = jfs_upgrade(a.pos[0], to, &from);
    if (rc == -JFS_EVERSION)
        die("cannot convert schema %d to %d (downgrades are not supported)", from, to ? to : JFS_SCHEMA_LATEST);
    if (rc)
        return fail(rc, a.pos[0]);
    if (from == (to ? to : JFS_SCHEMA_LATEST))
        printf("%s is already at schema %d\n", a.pos[0], from);
    else
        printf("upgraded %s from schema %d to %d\n", a.pos[0], from, to ? to : JFS_SCHEMA_LATEST);
    return 0;
}

static int cmd_prune(int argc, char **argv)
{
    args a;
    const char *o, *before = NULL;
    uint64_t n;
    int rc;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-b", "--before"))
            before = args_value(&a, o);
        else
            die("prune: unknown option %s", o);
    }
    if (a.npos != 1 || !before)
        die("usage: jsonfs prune --before TIME IMAGE");
    rc = jfs_prune_history(a.pos[0], parse_time_arg(before), &n);
    if (rc)
        return fail(rc, a.pos[0]);
    printf("removed %llu history record(s)/delete marker(s)\n", (unsigned long long)n);
    return 0;
}

static int log_cb(void *cookie, const jfs_version_info *vi)
{
    char tb[JFS_TIME_STRLEN];
    (void)cookie;
    fmt_time(vi->vtime, tb);
    if (vi->deleted)
        printf("%s  %-9s  deleted\n", tb, vi->is_current ? "current" : "");
    else
        printf("%s  %-9s  %-9s %12llu  %-8s %.16s\n", tb, vi->is_current ? "current" : "", vi->type,
               (unsigned long long)vi->size, vi->compression, vi->sha256);
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_time t;
    int rc;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else
            die("log: unknown option %s", o);
    }
    if (a.npos != 2)
        die("usage: jsonfs log [--at TIME] IMAGE PATH");
    if (at)
        t = parse_time_arg(at);
    printf("%-30s  %-9s  %-9s %12s  %-8s %s\n", "version time (UTC)", "", "type", "size", "algo", "sha256");
    rc = jfs_list_versions(a.pos[0], a.pos[1], at ? &t : NULL, log_cb, NULL);
    if (rc)
        return fail(rc, a.pos[1]);
    return 0;
}

/* ---- ls ---- */
typedef struct ls_ent {
    char *name;
    jfs_stat st;
} ls_ent;
typedef struct ls_list {
    ls_ent *e;
    size_t n, cap;
} ls_list;

static int ls_fill(void *cookie, const char *name, const jfs_stat *st)
{
    ls_list *l = cookie;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->e = jfs_realloc(l->e, l->cap * sizeof(ls_ent));
    }
    l->e[l->n].name = jfs_strdup(name);
    l->e[l->n].st = *st;
    l->n++;
    return 0;
}

static void mode_string(uint32_t m, char out[11])
{
    const char *rwx = "rwxrwxrwx";
    int i;
    switch (m & JFS_S_IFMT) {
    case JFS_S_IFDIR: out[0] = 'd'; break;
    case JFS_S_IFLNK: out[0] = 'l'; break;
    case JFS_S_IFIFO: out[0] = 'p'; break;
    case JFS_S_IFSOCK: out[0] = 's'; break;
    case JFS_S_IFCHR: out[0] = 'c'; break;
    case JFS_S_IFBLK: out[0] = 'b'; break;
    default: out[0] = '-';
    }
    for (i = 0; i < 9; i++)
        out[i + 1] = (m & (1u << (8 - i))) ? rwx[i] : '-';
    if (m & JFS_S_ISUID)
        out[3] = (m & 0100) ? 's' : 'S';
    if (m & JFS_S_ISGID)
        out[6] = (m & 0010) ? 's' : 'S';
    if (m & JFS_S_ISVTX)
        out[9] = (m & 0001) ? 't' : 'T';
    out[10] = 0;
}

static void ls_dir(jfs_fs *fs, const char *path, int lng, int all, int recursive, int header)
{
    ls_list l;
    size_t i;
    int rc;
    memset(&l, 0, sizeof l);
    rc = jfs_readdir(fs, path, ls_fill, &l);
    if (rc == -JFS_ENOTDIR) {
        jfs_stat st;
        jfs_getattr(fs, path, &st);
        ls_fill(&l, path, &st);
    } else if (rc) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, jfs_strerror(rc));
        return;
    }
    if (header)
        printf("%s:\n", path);
    for (i = 0; i < l.n; i++) {
        ls_ent *e = &l.e[i];
        if (!all && e->name[0] == '.')
            continue;
        if (lng) {
            char ms[11], tb[JFS_TIME_STRLEN];
            mode_string(e->st.mode, ms);
            fmt_time(e->st.mtime, tb);
            tb[19] = 0;
            printf("%s %3u %5u %5u %10llu %s %s", ms, e->st.nlink, e->st.uid, e->st.gid,
                   (unsigned long long)e->st.size, tb, e->name);
            if ((e->st.mode & JFS_S_IFMT) == JFS_S_IFLNK && strcmp(e->name, ".") && strcmp(e->name, "..")) {
                char tgt[4096];
                char *p = jfs_asprintf("%s/%s", strcmp(path, "/") ? path : "", e->name);
                if (!jfs_readlink(fs, p, tgt, sizeof tgt))
                    printf(" -> %s", tgt);
                free(p);
            }
            printf("\n");
        } else {
            printf("%s\n", e->name);
        }
    }
    if (recursive) {
        for (i = 0; i < l.n; i++) {
            ls_ent *e = &l.e[i];
            if ((e->st.mode & JFS_S_IFMT) == JFS_S_IFDIR && strcmp(e->name, ".") && strcmp(e->name, "..") &&
                (all || e->name[0] != '.')) {
                char *p = jfs_asprintf("%s/%s", strcmp(path, "/") ? path : "", e->name);
                printf("\n");
                ls_dir(fs, p, lng, all, recursive, 1);
                free(p);
            }
        }
    }
    for (i = 0; i < l.n; i++)
        free(l.e[i].name);
    free(l.e);
}

static int cmd_ls(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    int lng = 0, all = 0, rec = 0;
    jfs_fs *fs;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else if (is(o, "-l", NULL))
            lng = 1;
        else if (is(o, "-a", NULL))
            all = 1;
        else if (is(o, "-R", NULL))
            rec = 1;
        else if (!strcmp(o, "-la") || !strcmp(o, "-al"))
            lng = all = 1;
        else if (!strcmp(o, "-lR") || !strcmp(o, "-Rl"))
            lng = rec = 1;
        else
            die("ls: unknown option %s", o);
    }
    if (a.npos < 1 || a.npos > 2)
        die("usage: jsonfs ls [-l] [-a] [-R] [--at TIME] IMAGE [PATH]");
    open_ro(a.pos[0], at, &fs);
    ls_dir(fs, a.npos > 1 ? a.pos[1] : "/", lng, all, rec, rec);
    jfs_close(fs);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_fs *fs;
    jfs_handle *h;
    char buf[65536];
    uint64_t off = 0;
    int rc;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else
            die("cat: unknown option %s", o);
    }
    if (a.npos < 2)
        die("usage: jsonfs cat [--at TIME] IMAGE PATH...");
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    open_ro(a.pos[0], at, &fs);
    {
        int i;
        for (i = 1; i < a.npos; i++) {
            rc = jfs_open_file(fs, a.pos[i], JFS_O_RDONLY, &h);
            if (rc) {
                jfs_close(fs);
                return fail(rc, a.pos[i]);
            }
            off = 0;
            for (;;) {
                int64_t n = jfs_read(fs, h, buf, sizeof buf, off);
                if (n < 0) {
                    jfs_release(fs, h);
                    jfs_close(fs);
                    return fail((int)n, a.pos[i]);
                }
                if (n == 0)
                    break;
                fwrite(buf, 1, (size_t)n, stdout);
                off += (uint64_t)n;
            }
            jfs_release(fs, h);
        }
    }
    jfs_close(fs);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* import / export (host <-> image)                                           */
/* ------------------------------------------------------------------------ */
static jfs_ctx tool_ctx(void)
{
    jfs_ctx c;
    memset(&c, 0, sizeof c);
#ifndef _WIN32
    c.uid = (uint32_t)getuid();
    c.gid = (uint32_t)getgid();
#endif
    return c;
}

static char *join(const char *a, const char *b)
{
    size_t n = strlen(a);
    if (n && (a[n - 1] == '/' || a[n - 1] == '\\'))
        return jfs_asprintf("%s%s", a, b);
    return jfs_asprintf("%s/%s", a, b);
}

static int import_file_data(jfs_fs *fs, const char *host, const char *dst, uint32_t mode, uint64_t *bytes)
{
    FILE *f = NULL;
    jfs_handle *h;
    jfs_ctx c = tool_ctx();
    char buf[65536];
    uint64_t off = 0;
    int rc;
#ifdef _WIN32
    wchar_t *w = jfs_utf8_to_wide(host);
    f = _wfopen(w, L"rb");
    free(w);
#else
    f = fopen(host, "rb");
#endif
    if (!f) {
        fprintf(stderr, "%s: cannot read %s: %s\n", prog, host, strerror(errno));
        return -JFS_EIO;
    }
    rc = jfs_create(fs, dst, mode, JFS_O_WRONLY | JFS_O_TRUNC, &c, &h);
    if (rc) {
        fclose(f);
        return rc;
    }
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, f);
        int64_t w;
        if (n == 0)
            break;
        w = jfs_write(fs, h, buf, n, off);
        if (w < 0) {
            rc = (int)w;
            break;
        }
        off += n;
    }
    fclose(f);
    jfs_release(fs, h);
    *bytes += off;
    return rc;
}

#ifndef _WIN32
static jfs_time ts_of(time_t s, long ns)
{
    jfs_time t;
    t.sec = (int64_t)s;
    t.nsec = (int32_t)ns;
    return t;
}

static void import_meta(jfs_fs *fs, const char *dst, const struct stat *st)
{
    jfs_time tm[2];
#if defined(__APPLE__)
    tm[0] = ts_of(st->st_atimespec.tv_sec, st->st_atimespec.tv_nsec);
    tm[1] = ts_of(st->st_mtimespec.tv_sec, st->st_mtimespec.tv_nsec);
#else
    tm[0] = ts_of(st->st_atim.tv_sec, st->st_atim.tv_nsec);
    tm[1] = ts_of(st->st_mtim.tv_sec, st->st_mtim.tv_nsec);
#endif
    if (!S_ISLNK(st->st_mode)) {
        jfs_chmod(fs, dst, st->st_mode & 07777);
        if (getuid() == 0)
            jfs_chown(fs, dst, st->st_uid, st->st_gid);
    }
    jfs_utimens(fs, dst, tm);
}

static int import_tree(jfs_fs *fs, const char *host, const char *dst, uint64_t *nfiles, uint64_t *bytes)
{
    struct stat st;
    int rc = 0;
    jfs_ctx c = tool_ctx();
    if (lstat(host, &st) != 0) {
        fprintf(stderr, "%s: %s: %s\n", prog, host, strerror(errno));
        return -JFS_ENOENT;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        if (strcmp(dst, "/")) {
            rc = jfs_mkdir(fs, dst, 0700, &c);
            if (rc && rc != -JFS_EEXIST)
                return rc;
        }
        d = opendir(host);
        if (!d)
            return -JFS_EACCES;
        while ((de = readdir(d))) {
            char *hp, *dp;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            hp = join(host, de->d_name);
            dp = join(dst, de->d_name);
            rc = import_tree(fs, hp, dp, nfiles, bytes);
            free(hp);
            free(dp);
            if (rc)
                break;
        }
        closedir(d);
        if (rc)
            return rc;
    } else if (S_ISREG(st.st_mode)) {
        /* keep hard links: the second name of an inode becomes a link */
        static jfs_u64map seen_inodes;
        uint64_t key = ((uint64_t)st.st_dev << 40) ^ (uint64_t)st.st_ino;
        const char *first = st.st_nlink > 1 ? jfs_u64map_get(&seen_inodes, key) : NULL;
        if (first && img_type(fs, first) == JFS_S_IFREG) {
            if (g_overwrite && img_type(fs, dst))
                jfs_unlink(fs, dst);
            rc = jfs_link(fs, first, dst);
            if (!rc) {
                (*nfiles)++;
                return 0; /* metadata is shared with the first name */
            }
        }
        rc = import_file_data(fs, host, dst, 0600, bytes);
        if (!rc && st.st_nlink > 1)
            jfs_u64map_put(&seen_inodes, key, jfs_strdup(dst));
    } else if (S_ISLNK(st.st_mode)) {
        char tgt[4096];
        ssize_t n = readlink(host, tgt, sizeof tgt - 1);
        if (n < 0)
            return -JFS_EIO;
        tgt[n] = 0;
        rc = jfs_symlink(fs, tgt, dst, &c);
        if (rc == -JFS_EEXIST && g_overwrite && jfs_unlink(fs, dst) == 0)
            rc = jfs_symlink(fs, tgt, dst, &c);
    } else {
        uint32_t m = (uint32_t)(st.st_mode & 0170000) | 0600;
        rc = jfs_mknod(fs, dst, m, (uint64_t)st.st_rdev, &c);
        if (rc == -JFS_EEXIST && g_overwrite && jfs_unlink(fs, dst) == 0)
            rc = jfs_mknod(fs, dst, m, (uint64_t)st.st_rdev, &c);
    }
    if (rc)
        return rc;
    import_meta(fs, dst, &st);
    (*nfiles)++;
    return 0;
}
#else
static int import_tree(jfs_fs *fs, const char *host, const char *dst, uint64_t *nfiles, uint64_t *bytes)
{
    wchar_t *w = jfs_utf8_to_wide(host);
    DWORD attr = GetFileAttributesW(w);
    jfs_ctx c = tool_ctx();
    int rc = 0;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        free(w);
        fprintf(stderr, "%s: %s: not found\n", prog, host);
        return -JFS_ENOENT;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        HANDLE hf;
        char *pat = join(host, "*");
        wchar_t *wp = jfs_utf8_to_wide(pat);
        free(pat);
        if (strcmp(dst, "/")) {
            rc = jfs_mkdir(fs, dst, 0755, &c);
            if (rc && rc != -JFS_EEXIST) {
                free(w);
                free(wp);
                return rc;
            }
            rc = 0;
        }
        hf = FindFirstFileW(wp, &fd);
        free(wp);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                char *name, *hp, *dp;
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                name = jfs_wide_to_utf8(fd.cFileName);
                hp = join(host, name);
                dp = join(dst, name);
                rc = import_tree(fs, hp, dp, nfiles, bytes);
                free(name);
                free(hp);
                free(dp);
            } while (!rc && FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    } else {
        rc = import_file_data(fs, host, dst, (attr & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644, bytes);
    }
    free(w);
    if (!rc)
        (*nfiles)++;
    return rc;
}
#endif

static int cmd_import(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_fs *fs;
    uint64_t n = 0, bytes = 0;
    int rc, rc2;
    const char *dst;
    args_init(&a, argc, argv);
    while ((o = args_next(&a)))
        die("import: unknown option %s", o);
    if (a.npos < 2 || a.npos > 3)
        die("usage: jsonfs import IMAGE HOST_PATH [IMAGE_PATH]");
    dst = a.npos == 3 ? a.pos[2] : "/";
    open_rw(a.pos[0], &fs);
    {
        /* importing a single file into an existing directory */
        jfs_stat st;
        const char *base = a.pos[1] + strlen(a.pos[1]);
        while (base > a.pos[1] && base[-1] != '/' && base[-1] != '\\')
            base--;
        if (jfs_getattr(fs, dst, &st) == 0 && (st.mode & JFS_S_IFMT) == JFS_S_IFDIR && *base) {
            char *d2;
#ifdef _WIN32
            DWORD at = GetFileAttributesA(a.pos[1]);
            int host_is_dir = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
#else
            struct stat hs;
            int host_is_dir = stat(a.pos[1], &hs) == 0 && S_ISDIR(hs.st_mode);
#endif
            if (!host_is_dir) {
                d2 = join(dst, base);
                rc = import_tree(fs, a.pos[1], d2, &n, &bytes);
                free(d2);
                goto done;
            }
        }
    }
    rc = import_tree(fs, a.pos[1], dst, &n, &bytes);
done:
    rc2 = jfs_close(fs);
    if (rc)
        return fail(rc, "import");
    if (rc2)
        return fail(rc2, "commit");
    printf("imported %llu object(s), %llu bytes\n", (unsigned long long)n, (unsigned long long)bytes);
    return 0;
}

static int export_tree(jfs_fs *fs, const char *src, const char *host, uint64_t *n, uint64_t *bytes);

typedef struct exp_ctx {
    jfs_fs *fs;
    const char *src, *host;
    uint64_t *n, *bytes;
    int rc;
} exp_ctx;

static int exp_fill(void *cookie, const char *name, const jfs_stat *st)
{
    exp_ctx *e = cookie;
    char *sp, *hp;
    (void)st;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    sp = join(e->src, name);
    hp = join(e->host, name);
    e->rc = export_tree(e->fs, sp, hp, e->n, e->bytes);
    free(sp);
    free(hp);
    return e->rc;
}

static int export_tree(jfs_fs *fs, const char *src, const char *host, uint64_t *n, uint64_t *bytes)
{
    jfs_stat st;
    int rc = jfs_getattr(fs, src, &st);
    if (rc)
        return rc;
    switch (st.mode & JFS_S_IFMT) {
    case JFS_S_IFDIR: {
        exp_ctx e;
#ifdef _WIN32
        wchar_t *w = jfs_utf8_to_wide(host);
        CreateDirectoryW(w, NULL);
        free(w);
#else
        if (mkdir(host, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "%s: mkdir %s: %s\n", prog, host, strerror(errno));
            return -JFS_EIO;
        }
#endif
        e.fs = fs;
        e.src = src;
        e.host = host;
        e.n = n;
        e.bytes = bytes;
        e.rc = 0;
        rc = jfs_readdir(fs, src, exp_fill, &e);
        if (rc || e.rc)
            return rc ? rc : e.rc;
        break;
    }
    case JFS_S_IFREG: {
        jfs_handle *h;
        FILE *f;
        char buf[65536];
        uint64_t off = 0;
#ifdef _WIN32
        wchar_t *w = jfs_utf8_to_wide(host);
        f = _wfopen(w, L"wb");
        free(w);
#else
        f = fopen(host, "wb");
#endif
        if (!f) {
            fprintf(stderr, "%s: cannot create %s: %s\n", prog, host, strerror(errno));
            return -JFS_EIO;
        }
        rc = jfs_open_file(fs, src, JFS_O_RDONLY, &h);
        if (rc) {
            fclose(f);
            return rc;
        }
        for (;;) {
            int64_t r = jfs_read(fs, h, buf, sizeof buf, off);
            if (r <= 0) {
                rc = (int)r;
                break;
            }
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
                rc = -JFS_EIO;
                break;
            }
            off += (uint64_t)r;
        }
        jfs_release(fs, h);
        if (fclose(f) != 0 && !rc)
            rc = -JFS_EIO;
        *bytes += off;
        if (rc)
            return rc;
        break;
    }
    case JFS_S_IFLNK: {
        char tgt[4096];
        jfs_readlink(fs, src, tgt, sizeof tgt);
#ifdef _WIN32
        fprintf(stderr, "%s: skipping symlink %s -> %s (not supported on this host)\n", prog, src, tgt);
        return 0;
#else
        unlink(host);
        if (symlink(tgt, host) != 0) {
            fprintf(stderr, "%s: symlink %s: %s\n", prog, host, strerror(errno));
            return -JFS_EIO;
        }
        (*n)++;
        return 0;
#endif
    }
    default:
#ifndef _WIN32
        if (mknod(host, (mode_t)st.mode, (dev_t)st.rdev) != 0) {
            fprintf(stderr, "%s: skipping special file %s: %s\n", prog, src, strerror(errno));
            return 0;
        }
#else
        fprintf(stderr, "%s: skipping special file %s\n", prog, src);
        return 0;
#endif
    }
#ifndef _WIN32
    {
        struct timeval tv[2];
        chmod(host, st.mode & 07777);
        if (getuid() == 0 && lchown(host, st.uid, st.gid) != 0) { /* best effort */
        }
        tv[0].tv_sec = (time_t)st.atime.sec;
        tv[0].tv_usec = st.atime.nsec / 1000;
        tv[1].tv_sec = (time_t)st.mtime.sec;
        tv[1].tv_usec = st.mtime.nsec / 1000;
        utimes(host, tv);
    }
#endif
    (*n)++;
    return 0;
}

static int cmd_export(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_fs *fs;
    uint64_t n = 0, bytes = 0;
    int rc;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else
            die("export: unknown option %s", o);
    }
    if (a.npos < 2 || a.npos > 3)
        die("usage: jsonfs export [--at TIME] IMAGE HOST_DIR [IMAGE_PATH]");
    open_ro(a.pos[0], at, &fs);
    rc = export_tree(fs, a.npos == 3 ? a.pos[2] : "/", a.pos[1], &n, &bytes);
    jfs_close(fs);
    if (rc)
        return fail(rc, "export");
    printf("exported %llu object(s), %llu bytes\n", (unsigned long long)n, (unsigned long long)bytes);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Folder <-> image conversion and offline file manipulation                  */
/* ------------------------------------------------------------------------ */

/* -1 missing, 0 empty directory, 1 non-empty directory, 2 not a directory */
static int host_dir_state(const char *path)
{
#ifdef _WIN32
    wchar_t *w = jfs_utf8_to_wide(path);
    DWORD attr = GetFileAttributesW(w);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char *pat;
    int state = 0;
    free(w);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return -1;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
        return 2;
    pat = join(path, "*");
    w = jfs_utf8_to_wide(pat);
    free(pat);
    h = FindFirstFileW(w, &fd);
    free(w);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                state = 1;
        } while (!state && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return state;
#else
    struct stat st;
    DIR *d;
    struct dirent *de;
    int state = 0;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return 2;
    d = opendir(path);
    if (!d)
        return 1;
    while (!state && (de = readdir(d)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            state = 1;
    closedir(d);
    return state;
#endif
}

static const char *base_name(const char *p)
{
    const char *e = p + strlen(p), *b;
    while (e > p + 1 && (e[-1] == '/' || e[-1] == '\\'))
        e--;
    b = e;
    while (b > p && b[-1] != '/' && b[-1] != '\\')
        b--;
    {
        static char buf[4096];
        size_t n = (size_t)(e - b);
        if (n >= sizeof buf)
            n = sizeof buf - 1;
        memcpy(buf, b, n);
        buf[n] = 0;
        return buf;
    }
}

/* Image paths: make absolute, strip trailing slashes. */
static char *img_path(const char *p)
{
    char *r = p[0] == '/' ? jfs_strdup(p) : jfs_asprintf("/%s", p);
    size_t n = strlen(r);
    while (n > 1 && r[n - 1] == '/')
        r[--n] = 0;
    return r;
}

static char *img_join(const char *dir, const char *name)
{
    return strcmp(dir, "/") ? jfs_asprintf("%s/%s", dir, name) : jfs_asprintf("/%s", name);
}

static int img_type(jfs_fs *fs, const char *path)
{
    jfs_stat st;
    if (jfs_getattr(fs, path, &st))
        return 0;
    return (int)(st.mode & JFS_S_IFMT);
}

static int mkdir_p(jfs_fs *fs, const char *path, uint32_t mode)
{
    char *p = img_path(path), *s;
    jfs_ctx c = tool_ctx();
    int rc = 0;
    for (s = p + 1;; s++) {
        if (*s == '/' || !*s) {
            char save = *s;
            *s = 0;
            rc = jfs_mkdir(fs, p, mode, &c);
            if (rc == -JFS_EEXIST)
                rc = img_type(fs, p) == JFS_S_IFDIR ? 0 : -JFS_ENOTDIR;
            *s = save;
            if (rc || !save)
                break;
        }
    }
    free(p);
    return rc;
}

static int rm_tree(jfs_fs *fs, const char *path);

typedef struct rm_ctx {
    jfs_fs *fs;
    const char *dir;
    char **names;
    size_t n, cap;
} rm_ctx;

static int rm_collect(void *cookie, const char *name, const jfs_stat *st)
{
    rm_ctx *r = cookie;
    (void)st;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->names = jfs_realloc(r->names, r->cap * sizeof(char *));
    }
    r->names[r->n++] = jfs_strdup(name);
    return 0;
}

static int rm_tree(jfs_fs *fs, const char *path)
{
    rm_ctx r;
    size_t i;
    int rc = 0;
    if (img_type(fs, path) != JFS_S_IFDIR)
        return jfs_unlink(fs, path);
    memset(&r, 0, sizeof r);
    r.fs = fs;
    r.dir = path;
    rc = jfs_readdir(fs, path, rm_collect, &r);
    for (i = 0; i < r.n; i++) {
        char *child = img_join(path, r.names[i]);
        if (!rc)
            rc = rm_tree(fs, child);
        free(child);
        free(r.names[i]);
    }
    free(r.names);
    return rc ? rc : jfs_rmdir(fs, path);
}

/* finish an offline modification: commit on success, discard on failure */
static int finish_rw(jfs_fs *fs, int rc, const char *what)
{
    if (rc) {
        jfs_abort(fs);
        fprintf(stderr, "%s: %s failed: %s (image left unchanged)\n", prog, what, jfs_strerror(rc));
        return rc == -JFS_ELOCKED ? 3 : 1;
    }
    rc = jfs_close(fs);
    return rc ? fail(rc, "commit") : 0;
}

static int cmd_pack(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_mkfs_opts mo;
    jfs_fs *fs;
    int force = 0, rc, state;
    uint64_t n = 0, bytes = 0;
    memset(&mo, 0, sizeof mo);
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-L", "--label"))
            mo.label = args_value(&a, o);
        else if (is(o, "-c", "--compression"))
            mo.compression = args_value(&a, o);
        else if (is(o, "-V", "--versioning"))
            mo.versioning = 1;
        else if (is(o, NULL, "--schema"))
            mo.schema = atoi(args_value(&a, o));
        else if (is(o, "-f", "--force"))
            force = 1;
        else
            die("pack: unknown option %s", o);
    }
    if (a.npos != 2)
        die("usage: jsonfs pack [-L label] [-c compression] [-V] [-f] FOLDER IMAGE");
    state = host_dir_state(a.pos[0]);
    if (state < 0 || state == 2)
        die("%s is not a folder", a.pos[0]);
    if (jfs_is_url(a.pos[1]))
        die("cannot create an image at a URL");
    if (mo.compression && jfs_compression_parse(mo.compression, NULL, NULL))
        die("unknown compression \"%s\" (see 'jsonfs compressors')", mo.compression);
#ifndef _WIN32
    if (getuid() != 0) {
        mo.root_uid = (uint32_t)getuid();
        mo.root_gid = (uint32_t)getgid();
    }
#endif
    rc = jfs_mkfs(a.pos[1], &mo, force);
    if (rc == -JFS_EEXIST)
        die("%s already exists (use -f to overwrite)", a.pos[1]);
    if (rc)
        return fail(rc, a.pos[1]);
    open_rw(a.pos[1], &fs);
    rc = import_tree(fs, a.pos[0], "/", &n, &bytes);
    if (rc) {
        char lock[4200];
        jfs_abort(fs);
        jfs_remove_file(a.pos[1]); /* do not leave a half-filled image behind */
        snprintf(lock, sizeof lock, "%s.lock", a.pos[1]);
        jfs_remove_file(lock);
        return fail(rc, "pack");
    }
    rc = jfs_close(fs);
    if (rc)
        return fail(rc, "commit");
    printf("packed %s into %s: %llu object(s), %llu bytes\n", a.pos[0], a.pos[1], (unsigned long long)n,
           (unsigned long long)bytes);
    return 0;
}

static int cmd_unpack(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_fs *fs;
    int force = 0, rc, state;
    uint64_t n = 0, bytes = 0;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else if (is(o, "-f", "--force"))
            force = 1;
        else
            die("unpack: unknown option %s", o);
    }
    if (a.npos != 2)
        die("usage: jsonfs unpack [--at TIME] [-f] IMAGE FOLDER");
    state = host_dir_state(a.pos[1]);
    if (state == 2)
        die("%s exists and is not a folder", a.pos[1]);
    if (state == 1 && !force)
        die("%s is not empty (use -f to extract into it anyway)", a.pos[1]);
    open_ro(a.pos[0], at, &fs);
    rc = export_tree(fs, "/", a.pos[1], &n, &bytes);
    jfs_close(fs);
    if (rc)
        return fail(rc, "unpack");
    printf("unpacked %s into %s: %llu object(s), %llu bytes\n", a.pos[0], a.pos[1], (unsigned long long)n,
           (unsigned long long)bytes);
    return 0;
}

static int cmd_add(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_fs *fs;
    int parents = 0, rc = 0, i, nsrc;
    uint64_t n = 0, bytes = 0;
    char *dest;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-f", "--force"))
            g_overwrite = 1;
        else if (is(o, "-p", "--parents"))
            parents = 1;
        else
            die("add: unknown option %s", o);
    }
    if (a.npos < 3)
        die("usage: jsonfs add [-f] [-p] IMAGE HOST_PATH... IMAGE_DEST");
    nsrc = a.npos - 2;
    dest = img_path(a.pos[a.npos - 1]);
    open_rw(a.pos[0], &fs);
    if (parents) {
        const char *raw = a.pos[a.npos - 1];
        if (nsrc > 1 || raw[strlen(raw) - 1] == '/')
            rc = mkdir_p(fs, dest, 0755);
        else {
            char *parent = jfs_strdup(dest), *sl = strrchr(parent, '/');
            if (sl && sl != parent) {
                *sl = 0;
                rc = mkdir_p(fs, parent, 0755);
            }
            free(parent);
        }
    }
    for (i = 1; !rc && i <= nsrc; i++) {
        const char *src = a.pos[i];
        int into_dir = img_type(fs, dest) == JFS_S_IFDIR;
        char *target;
        int ttype, sdir = host_dir_state(src) >= 0 && host_dir_state(src) != 2;
        if (host_dir_state(src) == -1) {
            fprintf(stderr, "%s: %s: no such file or directory\n", prog, src);
            rc = -JFS_ENOENT;
            break;
        }
        if (nsrc > 1 && !into_dir) {
            fprintf(stderr, "%s: %s is not a directory in the image\n", prog, dest);
            rc = -JFS_ENOTDIR;
            break;
        }
        target = into_dir ? img_join(dest, base_name(src)) : jfs_strdup(dest);
        ttype = img_type(fs, target);
        if (ttype) {
            if (!g_overwrite) {
                fprintf(stderr, "%s: %s already exists in the image (use -f to replace)\n", prog, target);
                rc = -JFS_EEXIST;
            } else if (ttype == JFS_S_IFDIR && !sdir) {
                fprintf(stderr, "%s: cannot replace directory %s with a file\n", prog, target);
                rc = -JFS_EISDIR;
            } else if (ttype != JFS_S_IFDIR && sdir) {
                rc = jfs_unlink(fs, target);
            } else if (ttype != JFS_S_IFDIR && ttype != JFS_S_IFREG) {
                rc = jfs_unlink(fs, target); /* symlink/special file replaced by a file */
            }
        }
        if (!rc)
            rc = import_tree(fs, src, target, &n, &bytes);
        free(target);
    }
    free(dest);
    rc = finish_rw(fs, rc, "add");
    if (!rc)
        printf("added %llu object(s), %llu bytes\n", (unsigned long long)n, (unsigned long long)bytes);
    return rc;
}

static int cmd_mv(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_fs *fs;
    int noclobber = 0, rc = 0, i, nsrc;
    char *dest;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-n", "--no-clobber"))
            noclobber = 1;
        else
            die("mv: unknown option %s", o);
    }
    if (a.npos < 3)
        die("usage: jsonfs mv [-n] IMAGE SOURCE... DEST");
    nsrc = a.npos - 2;
    dest = img_path(a.pos[a.npos - 1]);
    open_rw(a.pos[0], &fs);
    for (i = 1; !rc && i <= nsrc; i++) {
        char *src = img_path(a.pos[i]), *target;
        int into_dir = img_type(fs, dest) == JFS_S_IFDIR && strcmp(src, dest);
        if (nsrc > 1 && !into_dir) {
            fprintf(stderr, "%s: %s is not a directory in the image\n", prog, dest);
            rc = -JFS_ENOTDIR;
        } else {
            target = into_dir ? img_join(dest, base_name(src)) : jfs_strdup(dest);
            rc = jfs_rename(fs, src, target, noclobber ? JFS_RENAME_NOREPLACE : 0);
            if (rc)
                fprintf(stderr, "%s: cannot move %s to %s: %s\n", prog, src, target, jfs_strerror(rc));
            free(target);
        }
        free(src);
    }
    free(dest);
    return finish_rw(fs, rc, "mv");
}

static int cmd_rm(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_fs *fs;
    int recursive = 0, force = 0, rc = 0, i;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-r", "--recursive") || is(o, "-R", NULL))
            recursive = 1;
        else if (is(o, "-f", "--force"))
            force = 1;
        else if (!strcmp(o, "-rf") || !strcmp(o, "-fr"))
            recursive = force = 1;
        else
            die("rm: unknown option %s", o);
    }
    if (a.npos < 2)
        die("usage: jsonfs rm [-r] [-f] IMAGE PATH...");
    open_rw(a.pos[0], &fs);
    for (i = 1; !rc && i < a.npos; i++) {
        char *p = img_path(a.pos[i]);
        int t = img_type(fs, p);
        if (!strcmp(p, "/")) {
            fprintf(stderr, "%s: refusing to remove the root directory\n", prog);
            rc = -JFS_EBUSY;
        } else if (!t) {
            if (!force) {
                fprintf(stderr, "%s: %s: no such file or directory\n", prog, p);
                rc = -JFS_ENOENT;
            }
        } else if (t == JFS_S_IFDIR && !recursive) {
            fprintf(stderr, "%s: %s is a directory (use -r)\n", prog, p);
            rc = -JFS_EISDIR;
        } else {
            rc = rm_tree(fs, p);
        }
        free(p);
    }
    return finish_rw(fs, rc, "rm");
}

static int cmd_mkdir(int argc, char **argv)
{
    args a;
    const char *o;
    jfs_fs *fs;
    int parents = 0, rc = 0, i;
    uint32_t mode = 0755;
    jfs_ctx c = tool_ctx();
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-p", "--parents"))
            parents = 1;
        else if (is(o, "-m", "--mode"))
            mode = (uint32_t)strtoul(args_value(&a, o), NULL, 8);
        else
            die("mkdir: unknown option %s", o);
    }
    if (a.npos < 2)
        die("usage: jsonfs mkdir [-p] [-m MODE] IMAGE PATH...");
    open_rw(a.pos[0], &fs);
    for (i = 1; !rc && i < a.npos; i++) {
        char *p = img_path(a.pos[i]);
        if (parents) {
            rc = mkdir_p(fs, p, mode);
        } else {
            rc = jfs_mkdir(fs, p, mode, &c);
            if (rc)
                fprintf(stderr, "%s: mkdir %s: %s\n", prog, p, jfs_strerror(rc));
        }
        if (!rc)
            jfs_chmod(fs, p, mode); /* exact mode, independent of any umask */
        free(p);
    }
    return finish_rw(fs, rc, "mkdir");
}

static int cmd_get(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_fs *fs;
    int force = 0, rc, state;
    uint64_t n = 0, bytes = 0;
    char *src, *dest;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else if (is(o, "-f", "--force"))
            force = 1;
        else
            die("get: unknown option %s", o);
    }
    if (a.npos < 2 || a.npos > 3)
        die("usage: jsonfs get [--at TIME] [-f] IMAGE PATH [HOST_DEST]");
    src = img_path(a.pos[1]);
    open_ro(a.pos[0], at, &fs);
    if (!img_type(fs, src)) {
        jfs_close(fs);
        die("%s: no such file or directory in the image", src);
    }
    if (a.npos == 3) {
        state = host_dir_state(a.pos[2]);
        dest = (state == 0 || state == 1) && strcmp(src, "/") ? join(a.pos[2], base_name(src)) : jfs_strdup(a.pos[2]);
    } else {
        dest = jfs_strdup(strcmp(src, "/") ? base_name(src) : ".");
    }
    state = host_dir_state(dest);
    if (state != -1 && !force && !(state == 0 && img_type(fs, src) == JFS_S_IFDIR)) {
        jfs_close(fs);
        die("%s already exists (use -f to overwrite)", dest);
    }
    rc = export_tree(fs, src, dest, &n, &bytes);
    jfs_close(fs);
    if (rc)
        return fail(rc, "get");
    printf("extracted %s to %s: %llu object(s), %llu bytes\n", src, dest, (unsigned long long)n,
           (unsigned long long)bytes);
    free(src);
    free(dest);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Archives (zip, tar, tar.gz built in; 7z, tar.xz, ... through libarchive)   */
/* ------------------------------------------------------------------------ */
static void ar_warn(void *cookie, const char *msg)
{
    (void)cookie;
    fprintf(stderr, "%s: warning: %s\n", prog, msg);
}

static void ar_print_stats(const char *verb, const jfs_archive_stats *st)
{
    printf("%s %llu entries (%llu files, %llu directories, %llu symlinks, %llu hard links, %llu special), "
           "%llu bytes [%s, %s]",
           verb, (unsigned long long)st->entries, (unsigned long long)st->files, (unsigned long long)st->dirs,
           (unsigned long long)st->symlinks, (unsigned long long)st->hardlinks, (unsigned long long)st->specials,
           (unsigned long long)st->bytes, st->format, st->backend);
    if (st->skipped)
        printf(", %llu skipped", (unsigned long long)st->skipped);
    printf("\n");
}

static int parse_backend(const char *v)
{
    if (!strcmp(v, "native") || !strcmp(v, "builtin"))
        return JFS_AR_BACKEND_NATIVE;
    if (!strcmp(v, "libarchive"))
        return JFS_AR_BACKEND_LIBARCHIVE;
    if (!strcmp(v, "auto"))
        return JFS_AR_BACKEND_AUTO;
    die("--backend must be auto, native or libarchive");
    return 0;
}

static int cmd_from_archive(int argc, char **argv)
{
    args a;
    const char *o, *into = NULL;
    jfs_mkfs_opts mo;
    jfs_archive_opts ao;
    jfs_archive_stats st;
    jfs_fs *fs;
    char err[1024];
    int force = 0, rc, created = 0;
    memset(&mo, 0, sizeof mo);
    jfs_archive_opts_init(&ao);
    ao.warn = ar_warn;
#ifndef _WIN32
    ao.uid = (uint32_t)getuid();
    ao.gid = (uint32_t)getgid();
    ao.same_owner = getuid() == 0; /* like tar: keep owners only as root */
#else
    ao.same_owner = 0;
#endif
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-F", "--format"))
            ao.format = args_value(&a, o);
        else if (is(o, NULL, "--backend"))
            ao.backend = parse_backend(args_value(&a, o));
        else if (is(o, NULL, "--same-owner"))
            ao.same_owner = 1;
        else if (is(o, NULL, "--no-same-owner"))
            ao.same_owner = 0;
        else if (is(o, "-i", "--into"))
            into = args_value(&a, o);
        else if (is(o, "-L", "--label"))
            mo.label = args_value(&a, o);
        else if (is(o, "-c", "--compression"))
            mo.compression = args_value(&a, o);
        else if (is(o, "-V", "--versioning"))
            mo.versioning = 1;
        else if (is(o, "-f", "--force"))
            force = 1;
        else
            die("from-archive: unknown option %s", o);
    }
    if (a.npos != 2)
        die("usage: jsonfs from-archive [-F FORMAT] [-L label] [-c algo] [-V] [-f] [--into PATH]\n"
            "                           [--same-owner|--no-same-owner] [--backend B] ARCHIVE IMAGE");
    if (jfs_is_url(a.pos[1]))
        die("cannot write an image at a URL");
    if (!into) {
        if (mo.compression && jfs_compression_parse(mo.compression, NULL, NULL))
            die("unknown compression \"%s\"", mo.compression);
        mo.root_uid = ao.uid;
        mo.root_gid = ao.gid;
        rc = jfs_mkfs(a.pos[1], &mo, force);
        if (rc == -JFS_EEXIST)
            die("%s already exists (use -f to overwrite, or --into PATH to add to it)", a.pos[1]);
        if (rc)
            return fail(rc, a.pos[1]);
        created = 1;
    }
    open_rw(a.pos[1], &fs);
    rc = jfs_archive_import(fs, a.pos[0], into ? into : "/", &ao, &st, err, sizeof err);
    if (rc) {
        jfs_abort(fs);
        if (created) {
            char lock[4200];
            jfs_remove_file(a.pos[1]);
            snprintf(lock, sizeof lock, "%s.lock", a.pos[1]);
            jfs_remove_file(lock);
        }
        fprintf(stderr, "%s: %s\n", prog, err[0] ? err : jfs_strerror(rc));
        if (!created)
            fprintf(stderr, "%s: image left unchanged\n", prog);
        return 1;
    }
    rc = jfs_close(fs);
    if (rc)
        return fail(rc, "commit");
    ar_print_stats("imported", &st);
    return 0;
}

static int cmd_to_archive(int argc, char **argv)
{
    args a;
    const char *o, *at = NULL;
    jfs_archive_opts ao;
    jfs_archive_stats st;
    jfs_fs *fs;
    char err[1024];
    int force = 0, rc;
    jfs_archive_opts_init(&ao);
    ao.warn = ar_warn;
    args_init(&a, argc, argv);
    while ((o = args_next(&a))) {
        if (is(o, "-F", "--format"))
            ao.format = args_value(&a, o);
        else if (is(o, NULL, "--backend"))
            ao.backend = parse_backend(args_value(&a, o));
        else if (is(o, "-t", "--at"))
            at = args_value(&a, o);
        else if (is(o, NULL, "--pax"))
            ao.pax = 1;
        else if (is(o, NULL, "--zip64"))
            ao.zip64 = 1;
        else if (is(o, "-l", "--level"))
            ao.level = atoi(args_value(&a, o));
        else if (is(o, "-f", "--force"))
            force = 1;
        else
            die("to-archive: unknown option %s", o);
    }
    if (a.npos < 2 || a.npos > 3)
        die("usage: jsonfs to-archive [-F FORMAT] [--at TIME] [--pax] [--zip64] [-l LEVEL] [-f]\n"
            "                         [--backend B] IMAGE ARCHIVE [IMAGE_DIR]");
    if (!force && jfs_file_exists(a.pos[1]))
        die("%s already exists (use -f to overwrite)", a.pos[1]);
    open_ro(a.pos[0], at, &fs);
    rc = jfs_archive_export(fs, a.npos == 3 ? a.pos[2] : "/", a.pos[1], &ao, &st, err, sizeof err);
    jfs_close(fs);
    if (rc) {
        fprintf(stderr, "%s: %s\n", prog, err[0] ? err : jfs_strerror(rc));
        return 1;
    }
    ar_print_stats("archived", &st);
    return 0;
}

/* Is `path` a JSONfs image (or a URL, which can only be an image here)? */
static int looks_like_image(const char *path)
{
    FILE *f;
    char buf[1024];
    size_t n;
    if (jfs_is_url(path))
        return 1;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "\"format\"") && strstr(buf, "\"jsonfs\"") && buf[strspn(buf, " \t\r\n")] == '{';
}

static int cmd_convert(int argc, char **argv)
{
    int i, npos = 0;
    const char *pos[2] = {NULL, NULL};
    /* the first two positional arguments decide the direction */
    for (i = 0; i < argc && npos < 2; i++) {
        if (argv[i][0] == '-') {
            if (!strcmp(argv[i], "-F") || !strcmp(argv[i], "--format") || !strcmp(argv[i], "--backend") ||
                !strcmp(argv[i], "-t") || !strcmp(argv[i], "--at") || !strcmp(argv[i], "-L") ||
                !strcmp(argv[i], "--label") || !strcmp(argv[i], "-c") || !strcmp(argv[i], "--compression") ||
                !strcmp(argv[i], "-l") || !strcmp(argv[i], "--level") || !strcmp(argv[i], "-i") ||
                !strcmp(argv[i], "--into"))
                i++;
            continue;
        }
        pos[npos++] = argv[i];
    }
    if (npos != 2)
        die("usage: jsonfs convert [options] SOURCE DESTINATION\n"
            "  archive -> image: jsonfs convert photos.zip photos.json\n"
            "  image -> archive: jsonfs convert photos.json photos.tar.gz");
    if (looks_like_image(pos[0]))
        return cmd_to_archive(argc, argv);
    return cmd_from_archive(argc, argv);
}

static void print_line(void *cookie, const char *line)
{
    (void)cookie;
    printf("%s\n", line);
}

#ifdef _WIN32
/* Installer helper: add/remove a directory to/from the machine PATH without
 * the 1024 character limit of NSIS strings. */
static int cmd_winpath(int argc, char **argv)
{
    static const wchar_t *keyname = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    HKEY k;
    DWORD type = REG_EXPAND_SZ, size = 0;
    wchar_t *cur, *dir, *out, *tok;
    size_t dl, outlen = 0, cap;
    int add, found = 0;
    LONG r;
    DWORD_PTR res;
    if (argc != 2 || (strcmp(argv[0], "add") && strcmp(argv[0], "remove")))
        die("usage: jsonfs win-path add|remove DIR");
    add = !strcmp(argv[0], "add");
    dir = jfs_utf8_to_wide(argv[1]);
    dl = wcslen(dir);
    while (dl > 3 && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/'))
        dir[--dl] = 0;
    r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyname, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
    if (r != ERROR_SUCCESS)
        die("cannot open the system environment key (error %ld); run as administrator", (long)r);
    r = RegQueryValueExW(k, L"Path", NULL, &type, NULL, &size);
    if (r != ERROR_SUCCESS)
        size = 0;
    cur = jfs_calloc(size / sizeof(wchar_t) + 2, sizeof(wchar_t));
    if (size && RegQueryValueExW(k, L"Path", NULL, &type, (LPBYTE)cur, &size) != ERROR_SUCCESS)
        die("cannot read PATH");
    cap = wcslen(cur) + dl + 4;
    out = jfs_calloc(cap, sizeof(wchar_t));
    tok = cur;
    while (tok) {
        wchar_t *semi = wcschr(tok, L';');
        size_t tl;
        int match;
        if (semi)
            *semi = 0;
        tl = wcslen(tok);
        while (tl > 3 && (tok[tl - 1] == L'\\' || tok[tl - 1] == L'/'))
            tl--;
        match = tl == dl && !_wcsnicmp(tok, dir, dl);
        if (match)
            found = 1;
        if (*tok && !(match && !add)) { /* drop empty entries and, when removing, ours */
            if (outlen)
                out[outlen++] = L';';
            wcscpy(out + outlen, tok);
            outlen += wcslen(tok);
        }
        tok = semi ? semi + 1 : NULL;
    }
    if (add && !found) {
        if (outlen)
            out[outlen++] = L';';
        wcscpy(out + outlen, dir);
        outlen += dl;
    }
    if ((add && !found) || (!add && found)) {
        r = RegSetValueExW(k, L"Path", 0, type == REG_SZ ? REG_SZ : REG_EXPAND_SZ, (const BYTE *)out,
                           (DWORD)((outlen + 1) * sizeof(wchar_t)));
        if (r != ERROR_SUCCESS)
            die("cannot write PATH (error %ld)", (long)r);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000,
                            &res);
        printf("%s %s %s the system PATH\n", add ? "added" : "removed", argv[1], add ? "to" : "from");
    } else {
        printf("PATH unchanged\n");
    }
    RegCloseKey(k);
    free(cur);
    free(out);
    free(dir);
    return 0;
}
#endif

static int cmd_compressors(void)
{
    size_t n, i;
    const jfs_compressor *c = jfs_compressor_list(&n);
    for (i = 0; i < n; i++)
        printf("%-8s %s\n", c[i].name, c[i].description);
    printf("\nLevels may be given as NAME:LEVEL (0-9) for deflate, e.g. deflate:9.\n");
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
            "JSONfs maintenance toolkit %s\n"
            "\n"
            "usage: jsonfs COMMAND [options] ...\n"
            "\n"
            "Image management:\n"
            "  mkfs [-L label] [-c algo] [-V] [--schema N] [-U uuid] [-f] IMAGE\n"
            "                                   create a new filesystem image\n"
            "  info [--at TIME] [--json] IMAGE  show image metadata and statistics\n"
            "  fsck [-r] [-v] IMAGE             verify checksums and structure (-r repairs)\n"
            "  upgrade [--to N] IMAGE           upgrade to a newer schema version\n"
            "  recompress -c ALGO [--set-default] [--history] IMAGE\n"
            "                                   recompress every file with ALGO\n"
            "\n"
            "Versioning:\n"
            "  versioning on|off|status [--purge] IMAGE\n"
            "                                   toggle time based versioning\n"
            "  snapshot --at TIME [-V] [-L label] [-f] IMAGE OUTPUT\n"
            "                                   extract the state at TIME into a new image\n"
            "  log [--at TIME] IMAGE PATH       list the versions of a file\n"
            "  prune --before TIME IMAGE        drop history older than TIME\n"
            "\n"
            "Folders <-> images:\n"
            "  pack [-L label] [-c algo] [-V] [-f] FOLDER IMAGE   create IMAGE from FOLDER\n"
            "  unpack [--at TIME] [-f] IMAGE FOLDER              extract IMAGE into FOLDER\n"
            "\n"
            "Archives (zip, tar, tar.gz built in; 7z, tar.xz, tar.bz2, tar.zst, ... via libarchive):\n"
            "  convert SOURCE DEST                     archive -> image or image -> archive\n"
            "  from-archive [-F fmt] [mkfs opts] [--into PATH] ARCHIVE IMAGE\n"
            "  to-archive [-F fmt] [--at TIME] [--pax] IMAGE ARCHIVE [IMAGE_DIR]\n"
            "  archive-formats                         list supported formats\n"
            "\n"
            "Editing without mounting:\n"
            "  add [-f] [-p] IMAGE HOST_PATH... DEST   add files/folders (like cp -r)\n"
            "  mv [-n] IMAGE SOURCE... DEST            rename / move inside the image\n"
            "  get [--at TIME] [-f] IMAGE PATH [HOST_DEST]   extract a file or folder\n"
            "  rm [-r] [-f] IMAGE PATH...              remove files / folders\n"
            "  mkdir [-p] [-m MODE] IMAGE PATH...      create directories\n"
            "\n"
            "Content access (works without mounting):\n"
            "  ls [-l] [-a] [-R] [--at TIME] IMAGE [PATH]\n"
            "  cat [--at TIME] IMAGE PATH...\n"
            "  import IMAGE HOST_PATH [IMAGE_PATH]   copy files/trees into the image\n"
            "  export [--at TIME] IMAGE HOST_DIR [IMAGE_PATH]\n"
            "\n"
            "Other:\n"
            "  compressors                      list compression algorithms\n"
            "  version                          print the version\n"
            "\n"
            "Read-only commands (info, fsck, ls, cat, export, log, snapshot source)\n"
            "also accept http:// and https:// URLs.  https certificates are verified;\n"
            "-k/--insecure skips verification, --cacert FILE adds trusted CAs.\n"
            "\n"
            "TIME accepts RFC 3339 (2026-11-11T10:00:00Z, offsets allowed), dates\n"
            "(2026-11-11), Unix epochs (@1762855200) and relative times (-2h, -3d).\n"
            "Mount images with jsonfs-mount (see 'jsonfs-mount --help').\n",
            JFS_VERSION_STRING);
}

int main(int argc, char **argv)
{
    const char *base = argv[0] + strlen(argv[0]);
    const char *cmd;
    while (base > argv[0] && base[-1] != '/' && base[-1] != '\\')
        base--;
    if (!strncmp(base, "mkfs.jsonfs", 11)) {
        prog = "mkfs.jsonfs";
        return cmd_mkfs(argc - 1, argv + 1);
    }
    if (!strncmp(base, "fsck.jsonfs", 11)) {
        prog = "fsck.jsonfs";
        return cmd_fsck(argc - 1, argv + 1);
    }
    if (getenv("JSONFS_DEBUG"))
        jfs_debug = 1;
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    cmd = argv[1];
    argc -= 2;
    argv += 2;
    {
        /* global options for http(s) images, accepted anywhere after the command */
        int i, j = 0, insecure = 0;
        const char *cafile = NULL;
        for (i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--insecure"))
                insecure = 1;
            else if (!strcmp(argv[i], "--cacert") && i + 1 < argc)
                cafile = argv[++i];
            else if (!strncmp(argv[i], "--cacert=", 9))
                cafile = argv[i] + 9;
            else
                argv[j++] = argv[i];
        }
        argc = j;
        argv[argc] = NULL;
        jfs_http_configure(insecure, cafile);
    }
    if (!strcmp(cmd, "mkfs"))
        return cmd_mkfs(argc, argv);
    if (!strcmp(cmd, "info"))
        return cmd_info(argc, argv);
    if (!strcmp(cmd, "fsck") || !strcmp(cmd, "verify"))
        return cmd_fsck(argc, argv);
    if (!strcmp(cmd, "snapshot") || !strcmp(cmd, "extract"))
        return cmd_snapshot(argc, argv);
    if (!strcmp(cmd, "versioning"))
        return cmd_versioning(argc, argv);
    if (!strcmp(cmd, "recompress"))
        return cmd_recompress(argc, argv);
    if (!strcmp(cmd, "upgrade"))
        return cmd_upgrade(argc, argv);
    if (!strcmp(cmd, "prune"))
        return cmd_prune(argc, argv);
    if (!strcmp(cmd, "log") || !strcmp(cmd, "history"))
        return cmd_log(argc, argv);
    if (!strcmp(cmd, "ls"))
        return cmd_ls(argc, argv);
    if (!strcmp(cmd, "cat"))
        return cmd_cat(argc, argv);
    if (!strcmp(cmd, "from-archive") || !strcmp(cmd, "import-archive"))
        return cmd_from_archive(argc, argv);
    if (!strcmp(cmd, "to-archive") || !strcmp(cmd, "export-archive"))
        return cmd_to_archive(argc, argv);
    if (!strcmp(cmd, "convert"))
        return cmd_convert(argc, argv);
    if (!strcmp(cmd, "archive-formats")) {
        jfs_archive_list_formats(print_line, NULL);
        return 0;
    }
    if (!strcmp(cmd, "pack") || !strcmp(cmd, "create") || !strcmp(cmd, "from-dir"))
        return cmd_pack(argc, argv);
    if (!strcmp(cmd, "unpack") || !strcmp(cmd, "to-dir"))
        return cmd_unpack(argc, argv);
    if (!strcmp(cmd, "add") || !strcmp(cmd, "put"))
        return cmd_add(argc, argv);
    if (!strcmp(cmd, "mv") || !strcmp(cmd, "rename"))
        return cmd_mv(argc, argv);
    if (!strcmp(cmd, "rm") || !strcmp(cmd, "remove"))
        return cmd_rm(argc, argv);
    if (!strcmp(cmd, "mkdir"))
        return cmd_mkdir(argc, argv);
    if (!strcmp(cmd, "get") || !strcmp(cmd, "extract-file"))
        return cmd_get(argc, argv);
    if (!strcmp(cmd, "import"))
        return cmd_import(argc, argv);
    if (!strcmp(cmd, "export"))
        return cmd_export(argc, argv);
#ifdef _WIN32
    if (!strcmp(cmd, "win-path"))
        return cmd_winpath(argc, argv);
#endif
    if (!strcmp(cmd, "compressors"))
        return cmd_compressors();
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        printf("jsonfs %s (schema %d..%d)\n", JFS_VERSION_STRING, JFS_SCHEMA_MIN, JFS_SCHEMA_LATEST);
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return 0;
    }
    fprintf(stderr, "%s: unknown command \"%s\"\n\n", prog, cmd);
    usage(stderr);
    return 2;
}
