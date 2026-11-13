/*
 * JSONfs mount driver - keeping web-mounted images up to date.
 *
 * Modes (-o refresh=...):
 *   periodic  (default) eventually consistent: a background thread checks
 *             every refresh_interval seconds (default 60)
 *   strict    additionally, opening a file or listing a directory first
 *             checks the server and waits for a newer version to be applied
 *             (refresh_min=SECONDS coalesces checks)
 *   off       never check again after mounting
 *
 * Checks are cheap (see jfs_remote.c): a 304 for an unchanged image, a 4 KiB
 * header probe for servers without ETags.  Failed checks keep the version we
 * have and are logged once per distinct error.
 *
 * File system events: on POSIX the changes of a new version are replayed as
 * real system calls on the mount point (relative to a directory handle of
 * the mounted root, so nothing can leak to the underlying directory after an
 * unmount); the FUSE handlers recognise them (jfs_refresh_is_replay) and
 * apply them from the pending version.  The kernel therefore emits genuine
 * inotify / kqueue / FSEvents notifications.  With WinFsp the changes are
 * reported with fuse_notify() after the new version is installed.
 *
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

#include "mount_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

static struct {
    int running;
    jfs_thread *th;
    volatile int stop;
    jfs_mutex mu; /* counters below */
    uint64_t requested, completed;
    jfs_time last_cycle;
    volatile uint64_t replay_tid;
    char last_err[512];
    jfs_mutex cycle_mu;
} R;

void (*jfs_refresh_notify_hook)(const jfs_changes *ch);

static unsigned ms_since(jfs_time t)
{
    jfs_time now = jfs_now();
    int64_t ms = (now.sec - t.sec) * 1000 + (now.nsec - t.nsec) / 1000000;
    return ms < 0 ? 0 : (unsigned)(ms > 0x7fffffff ? 0x7fffffff : ms);
}

int jfs_refresh_is_replay(uint64_t pid)
{
#ifdef _WIN32
    (void)pid;
    return 0;
#else
    if (!R.running || !jfs_mount_fs || !jfs_replay_active(jfs_mount_fs))
        return 0;
    return pid == (uint64_t)getpid() || (R.replay_tid && pid == R.replay_tid);
#endif
}

#ifndef _WIN32
/* -------------------------------------------------------------------------- */
/* replay through the kernel                                                  */
/* -------------------------------------------------------------------------- */
static int open_root_fd(void)
{
    struct stat mst, pst;
    char *parent, *slash;
    int fd;
    fd = open(jfs_mount_cfg.mountpoint, O_RDONLY
#ifdef O_DIRECTORY
                                            | O_DIRECTORY
#endif
    );
    if (fd < 0)
        return -1;
    /* make sure it really is the mounted file system, not the bare directory */
    parent = jfs_strdup(jfs_mount_cfg.mountpoint);
    slash = strrchr(parent, '/');
    if (slash && slash != parent)
        *slash = 0;
    else if (slash)
        slash[1] = 0;
    if (fstat(fd, &mst) != 0 || stat(parent, &pst) != 0 || mst.st_dev == pst.st_dev) {
        free(parent);
        close(fd);
        return -1;
    }
    free(parent);
    return fd; /* closed after each replay: a held handle would make unmounting fail (EBUSY) */
}

static const char *rel(const char *p)
{
    while (*p == '/')
        p++;
    return *p ? p : ".";
}

static int replay_one(int dfd, const jfs_change *c)
{
    const char *p = rel(c->path);
    int rc = 0, fd;
    switch (c->kind) {
    case JFS_CHANGE_CREATE:
        switch (c->mode & JFS_S_IFMT) {
        case JFS_S_IFDIR: rc = mkdirat(dfd, p, 0700); break;
        case JFS_S_IFLNK: rc = symlinkat(c->target ? c->target : "", dfd, p); break;
        case JFS_S_IFREG:
            fd = openat(dfd, p, O_CREAT | O_EXCL | O_WRONLY, 0600);
            rc = fd < 0 ? -1 : close(fd);
            break;
        default:
#if defined(__APPLE__)
            /* mknodat needs macOS 13; special files simply appear at install time */
            errno = ENOTSUP;
            rc = -1;
#else
            rc = mknodat(dfd, p, (mode_t)((c->mode & JFS_S_IFMT) | 0600), (dev_t)c->rdev);
#endif
        }
        break;
    case JFS_CHANGE_LINK: rc = linkat(dfd, rel(c->path2), dfd, p, 0); break;
    case JFS_CHANGE_DELETE: rc = unlinkat(dfd, p, (c->mode & JFS_S_IFMT) == JFS_S_IFDIR ? AT_REMOVEDIR : 0); break;
    case JFS_CHANGE_RENAME: rc = renameat(dfd, p, dfd, rel(c->path2)); break;
    case JFS_CHANGE_MODIFY:
        /* ftruncate -> IN_MODIFY; the handler installs the new content */
        fd = openat(dfd, p, O_WRONLY
#ifdef O_NOFOLLOW
                                | O_NOFOLLOW
#endif
        );
        if (fd < 0) {
            rc = -1;
            break;
        }
        rc = ftruncate(fd, (off_t)c->size);
        close(fd);
        break;
    case JFS_CHANGE_ATTRIB: rc = utimensat(dfd, p, NULL, AT_SYMLINK_NOFOLLOW); break;
    }
    return rc;
}

static void replay(const jfs_changes *ch)
{
    size_t i, failed = 0;
    int dfd = open_root_fd();
    if (dfd < 0) {
        JFS_DEBUG("refresh: mount point not accessible, applying %lu change(s) without events",
                  (unsigned long)ch->n);
        return;
    }
    for (i = 0; i < ch->n; i++) {
        if (R.stop)
            break;
        if (replay_one(dfd, &ch->v[i]) != 0) {
            failed++;
            JFS_DEBUG("refresh: replay of %s failed: %s", ch->v[i].path, strerror(errno));
            if (errno == ENOTCONN || errno == ENODEV || errno == EBADF)
                break; /* unmounted */
        }
    }
    close(dfd);
    if (failed)
        JFS_DEBUG("refresh: %lu of %lu change(s) could not be replayed (state is exact anyway)",
                  (unsigned long)failed, (unsigned long)ch->n);
}
#endif

static int events_enabled(void)
{
    return jfs_mount_cfg.events && !jfs_mount_cfg.single_thread;
}

static void cycle(void)
{
    jfs_pending *p = NULL;
    jfs_changes ch;
    char err[512];
    int rc;
    jfs_mutex_lock(&R.cycle_mu);
    rc = jfs_refresh_check(jfs_mount_fs, &p, err, sizeof err);
    if (rc) {
        if (strcmp(err, R.last_err))
            jfs_log("refresh failed: %s; keeping the current version", err);
        snprintf(R.last_err, sizeof R.last_err, "%s", err);
        jfs_mutex_unlock(&R.cycle_mu);
        return;
    }
    if (R.last_err[0]) {
        jfs_log("refresh: %s is reachable again", "the image server");
        R.last_err[0] = 0;
    }
    if (!p) {
        jfs_mutex_unlock(&R.cycle_mu);
        return;
    }
    memset(&ch, 0, sizeof ch);
    jfs_pending_diff(jfs_mount_fs, p, &ch);
#ifndef _WIN32
    if (events_enabled() && ch.n) {
        jfs_replay_begin(jfs_mount_fs, p);
        replay(&ch);
    }
#endif
    jfs_pending_install(jfs_mount_fs, p);
    if (events_enabled() && jfs_refresh_notify_hook && ch.n)
        jfs_refresh_notify_hook(&ch);
    {
        jfs_info in;
        jfs_get_info(jfs_mount_fs, &in);
        jfs_log("refresh: installed generation %llu (%lu change(s))", (unsigned long long)in.generation,
                (unsigned long)ch.n);
    }
    jfs_changes_free(&ch);
    jfs_mutex_unlock(&R.cycle_mu);
}

static void refresher_main(void *arg)
{
    (void)arg;
#if defined(__linux__)
    R.replay_tid = (uint64_t)syscall(SYS_gettid);
#elif !defined(_WIN32)
    R.replay_tid = (uint64_t)getpid();
#endif
    while (!R.stop) {
        int due;
        uint64_t target;
        jfs_sleep_ms(25);
        jfs_mutex_lock(&R.mu);
        target = R.requested;
        due = R.requested > R.completed ||
              (jfs_mount_cfg.refresh_mode != JFS_REFRESH_OFF &&
               ms_since(R.last_cycle) >= (unsigned)jfs_mount_cfg.refresh_interval_ms);
        jfs_mutex_unlock(&R.mu);
        if (!due)
            continue;
        cycle();
        jfs_mutex_lock(&R.mu);
        R.completed = target;
        R.last_cycle = jfs_now();
        jfs_mutex_unlock(&R.mu);
    }
}

void jfs_refresh_on_access(uint64_t caller_pid)
{
    uint64_t my;
    unsigned waited = 0;
    if (!R.running || jfs_mount_cfg.refresh_mode != JFS_REFRESH_STRICT)
        return;
    if (jfs_refresh_is_replay(caller_pid))
        return;
    jfs_mutex_lock(&R.mu);
    if (jfs_mount_cfg.refresh_min_ms > 0 && ms_since(R.last_cycle) < (unsigned)jfs_mount_cfg.refresh_min_ms) {
        jfs_mutex_unlock(&R.mu);
        return;
    }
    my = ++R.requested;
    jfs_mutex_unlock(&R.mu);
    if (jfs_mount_cfg.single_thread) {
        /* nobody could serve replayed calls: check right here, without events */
        cycle();
        jfs_mutex_lock(&R.mu);
        if (R.completed < my)
            R.completed = my;
        R.last_cycle = jfs_now();
        jfs_mutex_unlock(&R.mu);
        return;
    }
    for (;;) {
        uint64_t done;
        jfs_mutex_lock(&R.mu);
        done = R.completed;
        jfs_mutex_unlock(&R.mu);
        if (done >= my || waited >= 30000 || R.stop)
            break;
        jfs_sleep_ms(5);
        waited += 5;
    }
}

void jfs_refresh_start(void)
{
    if (R.running || !jfs_mount_fs || !jfs_is_remote(jfs_mount_fs) || jfs_mount_cfg.refresh_mode == JFS_REFRESH_OFF)
        return;
    jfs_mutex_init(&R.mu);
    jfs_mutex_init(&R.cycle_mu);
    R.stop = 0;
    R.last_cycle = jfs_now();
    if (jfs_thread_start(&R.th, refresher_main, NULL) == 0)
        R.running = 1;
}

void jfs_refresh_stop(void)
{
    if (!R.running)
        return;
    R.stop = 1;
    jfs_thread_join(R.th);
    R.th = NULL;
    R.running = 0;
}
