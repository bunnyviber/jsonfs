/*
 * JSONfs mount driver - shared declarations between the command line front
 * end and the kernel-interface backends (FUSE, WinFsp, Dokan).
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_MOUNT_COMMON_H
#define JFS_MOUNT_COMMON_H

#include "../lib/jfs_fs.h"

typedef struct jfs_mount_opts {
    char *image;           /* absolute path of the image */
    char *mountpoint;      /* directory (POSIX) or drive letter/dir (Windows) */
    int readonly;
    int shared;            /* multi-attach */
    int foreground;
    int debug;
    int single_thread;
    int commit_interval_ms;
    char *compression;     /* algorithm for new data */
    int has_at;
    jfs_time at;
    char *backend;         /* windows: "winfsp" | "dokan" | NULL (auto) */
    char *volname;
    char *passthrough;     /* comma separated options handed to the backend */
    int status_fd;         /* POSIX: pipe to the parent while daemonizing (-1 if none) */
} jfs_mount_opts;

/* The mounted volume (set by mount_main before the backend runs). */
extern jfs_fs *jfs_mount_fs;
extern jfs_mount_opts jfs_mount_cfg;

/* Opens jfs_mount_fs according to jfs_mount_cfg; prints errors. */
int jfs_mount_open_volume(void);
/* Called by backends once the filesystem is visible (daemon handshake). */
void jfs_mount_signal_ready(void);

/* Backends: return a process exit code. */
int jfs_fuse_available(char *why, size_t whylen);
int jfs_fuse_run(void);
int jfs_dokan_available(char *why, size_t whylen);
int jfs_dokan_run(void);

#endif
