/*
 * jsonfs-mount - mounts a JSONfs image.
 *
 *   Linux / FreeBSD / macOS : through FUSE (libfuse 2/3, macFUSE, FUSE-T),
 *                             loaded at runtime.
 *   Windows 7 .. 11         : through WinFsp's FUSE layer (loaded at runtime).
 *   Windows XP / Vista      : through the Dokan 0.6 user-mode filesystem
 *                             driver (loaded at runtime); also usable on later
 *                             versions with -o backend=dokan.
 *
 * Also works as a mount(8) helper (mount.jsonfs IMAGE DIR -o OPTIONS).
 * SPDX-License-Identifier: MIT
 */
#if !defined(_WIN32)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "mount_common.h"
#include "../lib/jfs_http.h"
#include "../lib/jfs_codec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#endif

jfs_fs *jfs_mount_fs;
jfs_mount_opts jfs_mount_cfg;
static const char *prog = "jsonfs-mount";

static int rw_explicit, insecure;
static char *cafile;

static void usage(FILE *f)
{
    fprintf(f,
            "usage: %s [options] IMAGE MOUNTPOINT\n"
            "\n"
            "Mounts the JSONfs image IMAGE on MOUNTPOINT (a directory; on Windows a\n"
            "drive letter such as X: or an empty NTFS directory).\n"
            "IMAGE may be a local file or an http:// or https:// URL; web images are\n"
            "downloaded once and mounted read-only.\n"
            "\n"
            "options:\n"
            "  -o OPT[,OPT...]      mount options (see below)\n"
            "  -f                   stay in the foreground\n"
            "  -d                   debug output (implies -f)\n"
            "  -s                   single threaded\n"
            "  -h, --help           this help\n"
            "  -V, --version        print the version\n"
            "  -k, --insecure       https: do not verify the server certificate\n"
            "  --cacert FILE        https: also trust the CA certificates in FILE (PEM)\n"
            "\n"
            "JSONfs mount options:\n"
            "  ro / rw              read-only / read-write (default rw)\n"
            "  compression=ALGO     compression for data written during this mount\n"
            "                       (deflate, deflate:0-9, lz4, rle, none; default: image default)\n"
            "  at=TIME              mount the state of a versioned image as it was at TIME\n"
            "                       (read-only; RFC 3339, YYYY-MM-DD, @epoch, -2h ...)\n"
            "  shared               multi-attach: several mounts (also on different hosts\n"
            "                       through a network share) may use the image concurrently\n"
            "  commit=SECONDS       commit interval for single-attach mounts (default 5,\n"
            "                       0 = commit after every change)\n"
            "  insecure, cafile=F   same as --insecure / --cacert F\n"
            "  refresh=MODE         web images: periodic (default; eventually consistent),\n"
            "                       strict (check before every open / directory listing)\n"
            "                       or off\n"
            "  refresh_interval=S   seconds between background checks (default 60)\n"
            "  refresh_min=S        strict mode: reuse a check younger than S seconds\n"
            "  cache=PATH|auto      keep the latest copy on disk; mount from it when the\n"
            "                       server is unreachable, revalidate it cheaply otherwise\n"
            "  events=on|off        report remote changes as file system events (inotify...)\n"
            "  backend=winfsp|dokan Windows only: force a kernel interface\n"
            "  volname=NAME         Windows only: volume label shown in Explorer\n"
            "\n"
            "Every other option is passed to FUSE (e.g. allow_other, uid=, gid=).\n",
            prog);
}

static void append_passthrough(const char *opt)
{
    if (!jfs_mount_cfg.passthrough) {
        jfs_mount_cfg.passthrough = jfs_strdup(opt);
    } else {
        char *n = jfs_asprintf("%s,%s", jfs_mount_cfg.passthrough, opt);
        free(jfs_mount_cfg.passthrough);
        jfs_mount_cfg.passthrough = n;
    }
}

static void parse_o(const char *list)
{
    char *copy = jfs_strdup(list), *p = copy, *tok;
    while ((tok = p) != NULL) {
        char *comma = strchr(p, ',');
        if (comma) {
            *comma = 0;
            p = comma + 1;
        } else {
            p = NULL;
        }
        if (!*tok)
            continue;
        if (!strcmp(tok, "ro"))
            jfs_mount_cfg.readonly = 1;
        else if (!strcmp(tok, "rw")) {
            jfs_mount_cfg.readonly = 0;
            rw_explicit = 1;
        } else if (!strcmp(tok, "insecure"))
            insecure = 1;
        else if (!strncmp(tok, "cafile=", 7) || !strncmp(tok, "cacert=", 7)) {
            free(cafile);
            cafile = jfs_strdup(tok + 7);
        }
        else if (!strcmp(tok, "shared") || !strcmp(tok, "multiattach") || !strcmp(tok, "multi-attach"))
            jfs_mount_cfg.shared = 1;
        else if (!strcmp(tok, "exclusive"))
            jfs_mount_cfg.shared = 0;
        else if (!strncmp(tok, "compression=", 12) || !strncmp(tok, "compress=", 9)) {
            const char *v = strchr(tok, '=') + 1;
            if (jfs_compression_parse(v, NULL, NULL)) {
                fprintf(stderr, "%s: unknown compression algorithm \"%s\"\n", prog, v);
                exit(2);
            }
            free(jfs_mount_cfg.compression);
            jfs_mount_cfg.compression = jfs_strdup(v);
        } else if (!strncmp(tok, "at=", 3) || !strncmp(tok, "time=", 5) || !strncmp(tok, "asof=", 5)) {
            const char *v = strchr(tok, '=') + 1;
            if (jfs_time_parse_user(v, &jfs_mount_cfg.at)) {
                fprintf(stderr, "%s: cannot parse time \"%s\"\n", prog, v);
                exit(2);
            }
            jfs_mount_cfg.has_at = 1;
            jfs_mount_cfg.readonly = 1;
        } else if (!strncmp(tok, "refresh=", 8)) {
            const char *v = tok + 8;
            if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "never"))
                jfs_mount_cfg.refresh_mode = JFS_REFRESH_OFF;
            else if (!strcmp(v, "periodic") || !strcmp(v, "eventual"))
                jfs_mount_cfg.refresh_mode = JFS_REFRESH_PERIODIC;
            else if (!strcmp(v, "strict") || !strcmp(v, "always"))
                jfs_mount_cfg.refresh_mode = JFS_REFRESH_STRICT;
            else {
                fprintf(stderr, "%s: refresh must be periodic, strict or off\n", prog);
                exit(2);
            }
        } else if (!strncmp(tok, "refresh_interval=", 17)) {
            jfs_mount_cfg.refresh_interval_ms = (int)(atof(tok + 17) * 1000);
            if (jfs_mount_cfg.refresh_interval_ms < 100)
                jfs_mount_cfg.refresh_interval_ms = 100;
        } else if (!strncmp(tok, "refresh_min=", 12)) {
            jfs_mount_cfg.refresh_min_ms = (int)(atof(tok + 12) * 1000);
        } else if (!strcmp(tok, "events") || !strcmp(tok, "events=on")) {
            jfs_mount_cfg.events = 1;
        } else if (!strcmp(tok, "noevents") || !strcmp(tok, "events=off")) {
            jfs_mount_cfg.events = 0;
        } else if (!strncmp(tok, "cache=", 6)) {
            free(jfs_mount_cfg.cache);
            jfs_mount_cfg.cache = jfs_strdup(tok + 6);
        } else if (!strncmp(tok, "commit=", 7)) {
            jfs_mount_cfg.commit_interval_ms = (int)(atof(tok + 7) * 1000);
        } else if (!strncmp(tok, "backend=", 8)) {
            free(jfs_mount_cfg.backend);
            jfs_mount_cfg.backend = jfs_strdup(tok + 8);
        } else if (!strncmp(tok, "volname=", 8)) {
            free(jfs_mount_cfg.volname);
            jfs_mount_cfg.volname = jfs_strdup(tok + 8);
        } else if (!strcmp(tok, "defaults") || !strcmp(tok, "auto") || !strcmp(tok, "noauto") ||
                   !strcmp(tok, "user") || !strcmp(tok, "users") || !strcmp(tok, "nofail") ||
                   !strcmp(tok, "_netdev") || !strncmp(tok, "x-", 2) || !strncmp(tok, "comment=", 8)) {
            /* fstab-only options */
        } else {
            append_passthrough(tok);
        }
    }
    free(copy);
}

static char *absolute_path(const char *p)
{
#ifdef _WIN32
    wchar_t *w = jfs_utf8_to_wide(p), buf[4096];
    DWORD n = GetFullPathNameW(w, 4096, buf, NULL);
    free(w);
    if (n == 0 || n >= 4096)
        return jfs_strdup(p);
    return jfs_wide_to_utf8(buf);
#else
    char buf[PATH_MAX];
    if (realpath(p, buf))
        return jfs_strdup(buf);
    return jfs_strdup(p);
#endif
}

/* $XDG_CACHE_HOME/jsonfs (or ~/.cache/jsonfs, %LOCALAPPDATA%\\jsonfs) + hash of the URL */
static char *default_cache_path(const char *url)
{
    uint8_t h[32];
    char hex[65], *dir;
    const char *base;
    jfs_sha256_buf(url, strlen(url), h);
    jfs_hex(h, 32, hex);
    hex[24] = 0;
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    dir = jfs_asprintf("%s\\jsonfs", base ? base : ".");
    {
        wchar_t *w = jfs_utf8_to_wide(dir);
        CreateDirectoryW(w, NULL);
        free(w);
    }
    {
        char *r = jfs_asprintf("%s\\%s.json", dir, hex);
        free(dir);
        return r;
    }
#else
    base = getenv("XDG_CACHE_HOME");
    if (base && *base) {
        dir = jfs_asprintf("%s/jsonfs", base);
    } else {
        const char *home = getenv("HOME");
        char *c = jfs_asprintf("%s/.cache", home ? home : "/tmp");
        mkdir(c, 0700);
        free(c);
        dir = jfs_asprintf("%s/.cache/jsonfs", home ? home : "/tmp");
    }
    mkdir(dir, 0700);
    {
        char *r = jfs_asprintf("%s/%s.json", dir, hex);
        free(dir);
        return r;
    }
#endif
}

#ifndef _WIN32
static int daemon_mode;
static void syslog_sink(const char *msg)
{
    if (daemon_mode)
        syslog(LOG_NOTICE, "%s", msg);
    else
        fprintf(stderr, "jsonfs: %s\n", msg);
}
#endif

int jfs_mount_open_volume(void)
{
    jfs_open_opts o;
    char err[512];
    int rc;
    jfs_open_opts_init(&o);
    o.readonly = jfs_mount_cfg.readonly;
    o.lockmode = jfs_mount_cfg.shared ? JFS_LOCKMODE_SHARED : JFS_LOCKMODE_EXCLUSIVE;
    o.compression = jfs_mount_cfg.compression;
    o.commit_interval_ms = jfs_mount_cfg.commit_interval_ms;
    o.has_at = jfs_mount_cfg.has_at;
    o.at = jfs_mount_cfg.at;
    o.cache_path = jfs_mount_cfg.cache;
    rc = jfs_open(jfs_mount_cfg.image, &o, &jfs_mount_fs, err, sizeof err);
    if (rc) {
        fprintf(stderr, "%s: %s\n", prog, err);
        return rc;
    }
    if (jfs_mount_cfg.has_at) {
        jfs_info in;
        char tb[JFS_TIME_STRLEN];
        jfs_get_info(jfs_mount_fs, &in);
        jfs_time_format(jfs_mount_cfg.at, tb);
        if (!in.versioning)
            fprintf(stderr, "%s: warning: versioning is off for this image; the view at %s may be incomplete\n",
                    prog, tb);
    }
    return 0;
}

void jfs_mount_signal_ready(void)
{
#ifndef _WIN32
    if (jfs_mount_cfg.status_fd >= 0) {
        char ok = 'k';
        int fd;
        if (write(jfs_mount_cfg.status_fd, &ok, 1) != 1) { /* parent gone */
        }
        close(jfs_mount_cfg.status_fd);
        jfs_mount_cfg.status_fd = -1;
        daemon_mode = 1;
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            if (!jfs_mount_cfg.debug)
                dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
    }
#endif
}

int main(int argc, char **argv)
{
    int i, npos = 0, rc;
    char *pos[2] = {NULL, NULL};
    char why[256];
    const char *base = argv[0] + strlen(argv[0]);

    while (base > argv[0] && base[-1] != '/' && base[-1] != '\\')
        base--;
    if (!strncmp(base, "mount.jsonfs", 12) || !strncmp(base, "mount_jsonfs", 12))
        prog = base;

    memset(&jfs_mount_cfg, 0, sizeof jfs_mount_cfg);
    jfs_mount_cfg.commit_interval_ms = 5000;
    jfs_mount_cfg.refresh_mode = JFS_REFRESH_PERIODIC;
    jfs_mount_cfg.refresh_interval_ms = 60000;
    jfs_mount_cfg.events = 1;
    jfs_mount_cfg.status_fd = -1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("jsonfs-mount %s\n", JFS_VERSION_STRING);
            return 0;
        } else if (!strcmp(a, "-o")) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            parse_o(argv[i]);
        } else if (!strncmp(a, "-o", 2)) {
            parse_o(a + 2);
        } else if (!strcmp(a, "-f")) {
            jfs_mount_cfg.foreground = 1;
        } else if (!strcmp(a, "-d") || !strcmp(a, "--debug")) {
            jfs_mount_cfg.debug = 1;
            jfs_mount_cfg.foreground = 1;
            jfs_debug = 1;
        } else if (!strcmp(a, "-k") || !strcmp(a, "--insecure")) {
            insecure = 1;
        } else if (!strcmp(a, "--cacert") || !strncmp(a, "--cacert=", 9)) {
            const char *v = a[8] == '=' ? a + 9 : (i + 1 < argc ? argv[++i] : NULL);
            if (!v) {
                usage(stderr);
                return 2;
            }
            free(cafile);
            cafile = jfs_strdup(v);
        } else if (!strcmp(a, "-s")) {
            jfs_mount_cfg.single_thread = 1;
        } else if (!strcmp(a, "-n") || !strcmp(a, "-v")) {
            /* mount(8) helper flags: no mtab / verbose */
        } else if (!strcmp(a, "-t")) {
            i++; /* mount helper: -t type */
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "%s: unknown option %s\n", prog, a);
            usage(stderr);
            return 2;
        } else if (npos < 2) {
            pos[npos++] = argv[i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (npos != 2) {
        usage(stderr);
        return 2;
    }
    jfs_http_configure(insecure, cafile);
    if (jfs_is_url(pos[0])) {
        if (rw_explicit) {
            fprintf(stderr, "%s: images mounted over http/https are read-only; drop the rw option\n", prog);
            return 2;
        }
        if (jfs_mount_cfg.shared) {
            fprintf(stderr, "%s: multi-attach (shared) is not available for http/https images\n", prog);
            return 2;
        }
        jfs_mount_cfg.readonly = 1;
        jfs_mount_cfg.image = jfs_strdup(pos[0]);
        if (jfs_mount_cfg.cache && !strcmp(jfs_mount_cfg.cache, "auto")) {
            free(jfs_mount_cfg.cache);
            jfs_mount_cfg.cache = default_cache_path(pos[0]);
        } else if (jfs_mount_cfg.cache) {
            char *abs = absolute_path(jfs_mount_cfg.cache);
            free(jfs_mount_cfg.cache);
            jfs_mount_cfg.cache = abs;
        }
        if (insecure && !strncmp(pos[0], "https", 5))
            fprintf(stderr, "%s: warning: TLS certificate verification is disabled (--insecure)\n", prog);
    } else if (jfs_mount_cfg.image = absolute_path(pos[0]), !jfs_file_exists(jfs_mount_cfg.image)) {
        fprintf(stderr, "%s: %s: no such image (create one with 'jsonfs mkfs')\n", prog, pos[0]);
        return 1;
    }
#ifdef _WIN32
    jfs_mount_cfg.mountpoint = jfs_strdup(pos[1]);
    jfs_mount_cfg.foreground = 1;
    {
        const char *be = jfs_mount_cfg.backend;
        int use_dokan = 0;
        if (be && !strcmp(be, "dokan")) {
            use_dokan = 1;
        } else if (be && strcmp(be, "winfsp")) {
            fprintf(stderr, "%s: unknown backend \"%s\" (winfsp or dokan)\n", prog, be);
            return 2;
        } else if (!jfs_fuse_available(why, sizeof why)) {
            if (be) {
                fprintf(stderr, "%s: WinFsp is not usable: %s\n", prog, why);
                return 1;
            }
            if (!jfs_dokan_available(why, sizeof why)) {
                fprintf(stderr,
                        "%s: no filesystem driver found.\n"
                        "  Windows 7 and later: install WinFsp (https://winfsp.dev)\n"
                        "  Windows XP / Vista : install Dokan 0.6.0\n"
                        "  (%s)\n",
                        prog, why);
                return 1;
            }
            use_dokan = 1;
        }
        if (use_dokan && !jfs_dokan_available(why, sizeof why)) {
            fprintf(stderr, "%s: Dokan is not usable: %s\n", prog, why);
            return 1;
        }
        if (jfs_mount_open_volume())
            return 1;
        rc = use_dokan ? jfs_dokan_run() : jfs_fuse_run();
        return rc;
    }
#else
    jfs_mount_cfg.mountpoint = absolute_path(pos[1]);
    jfs_set_log_sink(syslog_sink);
    openlog("jsonfs", LOG_PID, LOG_DAEMON);
    if (!jfs_fuse_available(why, sizeof why)) {
        fprintf(stderr, "%s: FUSE is not available: %s\n", prog, why);
#if defined(__APPLE__)
        fprintf(stderr, "  install macFUSE (https://osxfuse.github.io) or FUSE-T (https://www.fuse-t.org)\n");
#elif defined(__FreeBSD__)
        fprintf(stderr, "  run: pkg install fusefs-libs && kldload fusefs\n");
#else
        fprintf(stderr, "  install libfuse (e.g. 'apt install fuse3' or 'dnf install fuse3')\n");
#endif
        return 1;
    }
    if (!jfs_mount_cfg.foreground) {
        int p[2];
        pid_t pid;
        if (pipe(p) != 0) {
            perror("pipe");
            return 1;
        }
        pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid > 0) {
            char c = 0;
            int st = 0;
            ssize_t r;
            close(p[1]);
            do {
                r = read(p[0], &c, 1);
            } while (r < 0 && errno == EINTR);
            if (r == 1 && c == 'k')
                return 0;
            waitpid(pid, &st, 0);
            return WIFEXITED(st) && WEXITSTATUS(st) ? WEXITSTATUS(st) : 1;
        }
        close(p[0]);
        setsid();
        signal(SIGHUP, SIG_IGN);
        jfs_mount_cfg.status_fd = p[1];
    }
    if (jfs_mount_open_volume())
        return 1;
    if (chdir("/") != 0) { /* not fatal */
    }
    rc = jfs_fuse_run();
    return rc;
#endif
}
