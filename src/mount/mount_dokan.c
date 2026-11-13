/*
 * JSONfs mount driver - Dokan backend (Windows).
 *
 * Targets the Dokan 0.6 user-mode filesystem API, the last Dokan line that
 * supports Windows XP SP2/SP3 and Server 2003 (and also runs on Vista and 7).
 * WinFsp, used on Windows 7+, does not support XP/Vista, so this backend is
 * what makes JSONfs mountable on every Windows release since XP.
 *
 * dokan.dll is loaded at runtime; the API declarations below follow the
 * published Dokan 0.6.0 header (dokan.h, LGPL).  No Dokan SDK is needed to
 * build.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mount_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
int jfs_dokan_available(char *why, size_t whylen)
{
    snprintf(why, whylen, "Dokan is a Windows-only backend");
    return 0;
}

int jfs_dokan_run(void)
{
    return 1;
}
#else

#include <windows.h>

/* ------------------------------------------------------------------------ */
/* Dokan 0.6.0 ABI                                                            */
/* ------------------------------------------------------------------------ */
#define DOKAN_CALLBACK __stdcall
#define DOKAN_VERSION_06 600
#define DOKAN_OPTION_KEEP_ALIVE 8
#define DOKAN_OPTION_REMOVABLE 32

typedef struct _DOKAN_OPTIONS {
    USHORT Version;
    USHORT ThreadCount;
    ULONG Options;
    ULONG64 GlobalContext;
    LPCWSTR MountPoint;
} DOKAN_OPTIONS, *PDOKAN_OPTIONS;

typedef struct _DOKAN_FILE_INFO {
    ULONG64 Context;
    ULONG64 DokanContext;
    PDOKAN_OPTIONS DokanOptions;
    ULONG ProcessId;
    UCHAR IsDirectory;
    UCHAR DeleteOnClose;
    UCHAR PagingIo;
    UCHAR SynchronousIo;
    UCHAR Nocache;
    UCHAR WriteToEndOfFile;
} DOKAN_FILE_INFO, *PDOKAN_FILE_INFO;

typedef int(WINAPI *PFillFindData)(PWIN32_FIND_DATAW, PDOKAN_FILE_INFO);

typedef struct _DOKAN_OPERATIONS {
    int(DOKAN_CALLBACK *CreateFile)(LPCWSTR, DWORD, DWORD, DWORD, DWORD, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *OpenDirectory)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *CreateDirectory)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *Cleanup)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *CloseFile)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *ReadFile)(LPCWSTR, LPVOID, DWORD, LPDWORD, LONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *WriteFile)(LPCWSTR, LPCVOID, DWORD, LPDWORD, LONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *FlushFileBuffers)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *GetFileInformation)(LPCWSTR, LPBY_HANDLE_FILE_INFORMATION, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *FindFiles)(LPCWSTR, PFillFindData, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *FindFilesWithPattern)(LPCWSTR, LPCWSTR, PFillFindData, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *SetFileAttributes)(LPCWSTR, DWORD, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *SetFileTime)(LPCWSTR, CONST FILETIME *, CONST FILETIME *, CONST FILETIME *, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *DeleteFile)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *DeleteDirectory)(LPCWSTR, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *MoveFile)(LPCWSTR, LPCWSTR, BOOL, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *SetEndOfFile)(LPCWSTR, LONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *SetAllocationSize)(LPCWSTR, LONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *LockFile)(LPCWSTR, LONGLONG, LONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *UnlockFile)(LPCWSTR, LONGLONG, LONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *GetDiskFreeSpace)(PULONGLONG, PULONGLONG, PULONGLONG, PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *GetVolumeInformation)(LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, DWORD,
                                              PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *Unmount)(PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *GetFileSecurity)(LPCWSTR, PSECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG,
                                         PDOKAN_FILE_INFO);
    int(DOKAN_CALLBACK *SetFileSecurity)(LPCWSTR, PSECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG,
                                         PDOKAN_FILE_INFO);
} DOKAN_OPERATIONS, *PDOKAN_OPERATIONS;

#ifdef _WIN64
#define DOKANAPI
#else
#define DOKANAPI __stdcall
#endif
typedef int(DOKANAPI *DokanMain_fn)(PDOKAN_OPTIONS, PDOKAN_OPERATIONS);
typedef BOOL(DOKANAPI *DokanUnmount_fn)(WCHAR);
typedef BOOL(DOKANAPI *DokanRemoveMountPoint_fn)(LPCWSTR);
typedef ULONG(DOKANAPI *DokanVersion_fn)(void);

static HMODULE dokan_dll;
static DokanMain_fn pDokanMain;
static DokanUnmount_fn pDokanUnmount;
static DokanRemoveMountPoint_fn pDokanRemoveMountPoint;
static DokanVersion_fn pDokanVersion;
static wchar_t *mount_point_w;

int jfs_dokan_available(char *why, size_t whylen)
{
    if (dokan_dll)
        return 1;
    dokan_dll = LoadLibraryW(L"dokan.dll");
    if (!dokan_dll) {
        snprintf(why, whylen, "dokan.dll not found (install Dokan 0.6.0)");
        return 0;
    }
#define DLSYM(var, name)                                   \
    do {                                                   \
        FARPROC fp_ = GetProcAddress(dokan_dll, name);     \
        memcpy(&var, &fp_, sizeof fp_);                    \
    } while (0)
    DLSYM(pDokanMain, "DokanMain");
    DLSYM(pDokanUnmount, "DokanUnmount");
    DLSYM(pDokanRemoveMountPoint, "DokanRemoveMountPoint");
    DLSYM(pDokanVersion, "DokanVersion");
#undef DLSYM
    if (!pDokanMain || !pDokanUnmount) {
        snprintf(why, whylen, "dokan.dll does not provide the Dokan 0.6 API (Dokany 1.x/2.x use a different "
                              "API; use WinFsp on Windows 7 and later)");
        FreeLibrary(dokan_dll);
        dokan_dll = NULL;
        return 0;
    }
    if (pDokanVersion && pDokanVersion() >= 700) {
        snprintf(why, whylen, "dokan.dll version %lu is not API compatible with Dokan 0.6",
                 (unsigned long)pDokanVersion());
        FreeLibrary(dokan_dll);
        dokan_dll = NULL;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                    */
/* ------------------------------------------------------------------------ */
static int werr(int jfs_rc)
{
    switch (jfs_rc < 0 ? -jfs_rc : jfs_rc) {
    case 0: return 0;
    case JFS_ENOENT: return -ERROR_FILE_NOT_FOUND;
    case JFS_ENOTDIR: return -ERROR_PATH_NOT_FOUND;
    case JFS_EEXIST: return -ERROR_ALREADY_EXISTS;
    case JFS_EACCES:
    case JFS_EPERM: return -ERROR_ACCESS_DENIED;
    case JFS_EROFS: return -ERROR_WRITE_PROTECT;
    case JFS_ENOSPC: return -ERROR_DISK_FULL;
    case JFS_ENOTEMPTY: return -ERROR_DIR_NOT_EMPTY;
    case JFS_EISDIR: return -ERROR_ACCESS_DENIED;
    case JFS_ENAMETOOLONG: return -ERROR_FILENAME_EXCED_RANGE;
    case JFS_EINVAL: return -ERROR_INVALID_PARAMETER;
    case JFS_ENOMEM: return -ERROR_NOT_ENOUGH_MEMORY;
    case JFS_EBUSY: return -ERROR_BUSY;
    case JFS_ENOTSUP:
    case JFS_ENOSYS: return -ERROR_NOT_SUPPORTED;
    default: return -ERROR_GEN_FAILURE;
    }
}

/* "\dir\file" (UTF-16) -> "/dir/file" (UTF-8) */
static char *upath(LPCWSTR w)
{
    char *s = jfs_wide_to_utf8(w), *p;
    for (p = s; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (!*s) {
        free(s);
        s = jfs_strdup("/");
    }
    return s;
}

static void to_filetime(jfs_time t, FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(t.sec * 10000000LL + t.nsec / 100) + 116444736000000000ULL;
    ft->dwLowDateTime = u.LowPart;
    ft->dwHighDateTime = u.HighPart;
}

static jfs_time from_filetime(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    jfs_time t;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    u.QuadPart -= 116444736000000000ULL;
    t.sec = (int64_t)(u.QuadPart / 10000000ULL);
    t.nsec = (int32_t)(u.QuadPart % 10000000ULL) * 100;
    return t;
}

static DWORD attrs_of(const jfs_stat *st)
{
    DWORD a = 0;
    if ((st->mode & JFS_S_IFMT) == JFS_S_IFDIR)
        a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->mode & 0200))
        a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

static DWORD volume_serial(void)
{
    jfs_info in;
    DWORD s = 0;
    const char *p;
    jfs_get_info(jfs_mount_fs, &in);
    for (p = in.uuid; *p; p++)
        s = s * 31 + (unsigned char)*p;
    return s;
}

static jfs_ctx root_ctx(void)
{
    jfs_ctx c;
    memset(&c, 0, sizeof c);
    c.umask = 022;
    return c;
}

/* ------------------------------------------------------------------------ */
/* operations                                                                 */
/* ------------------------------------------------------------------------ */
static int DOKAN_CALLBACK d_CreateFile(LPCWSTR name, DWORD access, DWORD share, DWORD disp, DWORD flags,
                                       PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_stat st;
    jfs_handle *h = NULL;
    int rc = jfs_getattr(jfs_mount_fs, p, &st), exists = rc == 0, ret = 0;
    int want_write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL)) != 0;
    int fl;
    jfs_ctx c = root_ctx();
    (void)share;
    (void)flags;
    if (rc && rc != -JFS_ENOENT) {
        free(p);
        return werr(rc);
    }
    if (exists && (st.mode & JFS_S_IFMT) == JFS_S_IFDIR) {
        free(p);
        fi->IsDirectory = TRUE;
        if (disp == CREATE_NEW)
            return -ERROR_FILE_EXISTS;
        return 0;
    }
    fl = want_write ? JFS_O_RDWR : JFS_O_RDONLY;
    switch (disp) {
    case CREATE_NEW:
        if (exists)
            ret = -ERROR_FILE_EXISTS;
        else
            ret = werr(jfs_create(jfs_mount_fs, p, 0644, JFS_O_RDWR | JFS_O_EXCL, &c, &h));
        break;
    case CREATE_ALWAYS:
        ret = werr(jfs_create(jfs_mount_fs, p, 0644, JFS_O_RDWR | JFS_O_TRUNC, &c, &h));
        if (!ret && exists)
            ret = ERROR_ALREADY_EXISTS;
        break;
    case OPEN_ALWAYS:
        if (exists)
            ret = werr(jfs_open_file(jfs_mount_fs, p, fl, &h));
        else
            ret = werr(jfs_create(jfs_mount_fs, p, 0644, JFS_O_RDWR, &c, &h));
        if (ret >= 0 && exists)
            ret = ERROR_ALREADY_EXISTS;
        break;
    case OPEN_EXISTING:
        ret = exists ? werr(jfs_open_file(jfs_mount_fs, p, fl, &h)) : -ERROR_FILE_NOT_FOUND;
        break;
    case TRUNCATE_EXISTING:
        ret = exists ? werr(jfs_open_file(jfs_mount_fs, p, JFS_O_RDWR | JFS_O_TRUNC, &h)) : -ERROR_FILE_NOT_FOUND;
        break;
    default: ret = -ERROR_INVALID_PARAMETER;
    }
    /* read-only volume: allow opening for reading even if write was asked
     * together with read (Explorer does that), fail real writes later */
    if (ret == -ERROR_WRITE_PROTECT && exists && (disp == OPEN_EXISTING || disp == OPEN_ALWAYS))
        ret = werr(jfs_open_file(jfs_mount_fs, p, JFS_O_RDONLY, &h));
    if (h)
        fi->Context = (ULONG64)(uintptr_t)h;
    free(p);
    return ret;
}

static int DOKAN_CALLBACK d_OpenDirectory(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_stat st;
    int rc = jfs_getattr(jfs_mount_fs, p, &st);
    free(p);
    if (rc)
        return -ERROR_PATH_NOT_FOUND;
    if ((st.mode & JFS_S_IFMT) != JFS_S_IFDIR)
        return -ERROR_DIRECTORY;
    fi->IsDirectory = TRUE;
    return 0;
}

static int DOKAN_CALLBACK d_CreateDirectory(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_ctx c = root_ctx();
    int rc = jfs_mkdir(jfs_mount_fs, p, 0777, &c);
    free(p);
    fi->IsDirectory = TRUE;
    return werr(rc);
}

static int DOKAN_CALLBACK d_Cleanup(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    if (fi->DeleteOnClose) {
        char *p = upath(name);
        int rc = fi->IsDirectory ? jfs_rmdir(jfs_mount_fs, p) : jfs_unlink(jfs_mount_fs, p);
        free(p);
        return werr(rc);
    }
    return 0;
}

static int DOKAN_CALLBACK d_CloseFile(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    (void)name;
    if (fi->Context) {
        jfs_release(jfs_mount_fs, (jfs_handle *)(uintptr_t)fi->Context);
        fi->Context = 0;
    }
    return 0;
}

static int with_handle(LPCWSTR name, PDOKAN_FILE_INFO fi, int write, jfs_handle **h, int *temp)
{
    *temp = 0;
    if (fi->Context) {
        *h = (jfs_handle *)(uintptr_t)fi->Context;
        return 0;
    } else {
        char *p = upath(name);
        int rc = jfs_open_file(jfs_mount_fs, p, write ? JFS_O_RDWR : JFS_O_RDONLY, h);
        free(p);
        *temp = 1;
        return rc;
    }
}

static int DOKAN_CALLBACK d_ReadFile(LPCWSTR name, LPVOID buf, DWORD len, LPDWORD read, LONGLONG off,
                                     PDOKAN_FILE_INFO fi)
{
    jfs_handle *h;
    int temp, rc = with_handle(name, fi, 0, &h, &temp);
    int64_t n;
    if (rc)
        return werr(rc);
    n = jfs_read(jfs_mount_fs, h, buf, len, (uint64_t)(off < 0 ? 0 : off));
    if (temp)
        jfs_release(jfs_mount_fs, h);
    if (n < 0)
        return werr((int)n);
    *read = (DWORD)n;
    return 0;
}

static int DOKAN_CALLBACK d_WriteFile(LPCWSTR name, LPCVOID buf, DWORD len, LPDWORD written, LONGLONG off,
                                      PDOKAN_FILE_INFO fi)
{
    jfs_handle *h;
    int temp, rc = with_handle(name, fi, 1, &h, &temp);
    int64_t n;
    if (rc)
        return werr(rc);
    if (fi->WriteToEndOfFile) {
        jfs_stat st;
        jfs_fgetattr(jfs_mount_fs, h, &st);
        off = (LONGLONG)st.size;
    }
    n = jfs_write(jfs_mount_fs, h, buf, len, (uint64_t)(off < 0 ? 0 : off));
    if (temp)
        jfs_release(jfs_mount_fs, h);
    if (n < 0)
        return werr((int)n);
    *written = (DWORD)n;
    return 0;
}

static int DOKAN_CALLBACK d_FlushFileBuffers(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    (void)name;
    if (fi->Context)
        return werr(jfs_fsync(jfs_mount_fs, (jfs_handle *)(uintptr_t)fi->Context));
    return werr(jfs_sync(jfs_mount_fs));
}

static int DOKAN_CALLBACK d_GetFileInformation(LPCWSTR name, LPBY_HANDLE_FILE_INFORMATION bi, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_stat st;
    int rc = fi->Context ? jfs_fgetattr(jfs_mount_fs, (jfs_handle *)(uintptr_t)fi->Context, &st)
                         : jfs_getattr(jfs_mount_fs, p, &st);
    free(p);
    if (rc)
        return werr(rc);
    memset(bi, 0, sizeof *bi);
    bi->dwFileAttributes = attrs_of(&st);
    to_filetime(st.btime, &bi->ftCreationTime);
    to_filetime(st.atime, &bi->ftLastAccessTime);
    to_filetime(st.mtime, &bi->ftLastWriteTime);
    bi->dwVolumeSerialNumber = volume_serial();
    bi->nFileSizeHigh = (DWORD)(st.size >> 32);
    bi->nFileSizeLow = (DWORD)st.size;
    bi->nNumberOfLinks = st.nlink;
    bi->nFileIndexHigh = (DWORD)(st.ino >> 32);
    bi->nFileIndexLow = (DWORD)st.ino;
    return 0;
}

typedef struct find_ctx {
    PFillFindData fill;
    PDOKAN_FILE_INFO fi;
} find_ctx;

static int find_cb(void *cookie, const char *name, const jfs_stat *st)
{
    find_ctx *f = cookie;
    WIN32_FIND_DATAW fd;
    wchar_t *w;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    memset(&fd, 0, sizeof fd);
    fd.dwFileAttributes = attrs_of(st);
    to_filetime(st->btime, &fd.ftCreationTime);
    to_filetime(st->atime, &fd.ftLastAccessTime);
    to_filetime(st->mtime, &fd.ftLastWriteTime);
    fd.nFileSizeHigh = (DWORD)(st->size >> 32);
    fd.nFileSizeLow = (DWORD)st->size;
    w = jfs_utf8_to_wide(name);
    wcsncpy(fd.cFileName, w, MAX_PATH - 1);
    free(w);
    f->fill(&fd, f->fi);
    return 0;
}

static int DOKAN_CALLBACK d_FindFiles(LPCWSTR name, PFillFindData fill, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    find_ctx f;
    int rc;
    f.fill = fill;
    f.fi = fi;
    rc = jfs_readdir(jfs_mount_fs, p, find_cb, &f);
    free(p);
    return werr(rc);
}

static int DOKAN_CALLBACK d_SetFileAttributes(LPCWSTR name, DWORD attrs, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_stat st;
    int rc = jfs_getattr(jfs_mount_fs, p, &st);
    (void)fi;
    if (!rc) {
        uint32_t m = st.mode & 07777;
        if (attrs & FILE_ATTRIBUTE_READONLY)
            m &= ~0222u;
        else if (!(m & 0200))
            m |= 0200;
        if (m != (st.mode & 07777))
            rc = jfs_chmod(jfs_mount_fs, p, m);
    }
    free(p);
    return werr(rc);
}

static int DOKAN_CALLBACK d_SetFileTime(LPCWSTR name, CONST FILETIME *ct, CONST FILETIME *at, CONST FILETIME *mt,
                                        PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_time t[2];
    int rc = 0;
    (void)fi;
    t[0].sec = t[1].sec = 0;
    t[0].nsec = t[1].nsec = JFS_UTIME_OMIT;
    if (at && (at->dwLowDateTime || at->dwHighDateTime))
        t[0] = from_filetime(at);
    if (mt && (mt->dwLowDateTime || mt->dwHighDateTime))
        t[1] = from_filetime(mt);
    if (t[0].nsec != JFS_UTIME_OMIT || t[1].nsec != JFS_UTIME_OMIT)
        rc = jfs_utimens(jfs_mount_fs, p, t);
    if (!rc && ct && (ct->dwLowDateTime || ct->dwHighDateTime))
        rc = jfs_set_btime(jfs_mount_fs, p, from_filetime(ct));
    free(p);
    return werr(rc);
}

static int DOKAN_CALLBACK d_DeleteFile(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_stat st;
    int rc = jfs_getattr(jfs_mount_fs, p, &st);
    jfs_info in;
    (void)fi;
    free(p);
    jfs_get_info(jfs_mount_fs, &in);
    if (in.readonly)
        return -ERROR_WRITE_PROTECT;
    return werr(rc); /* actual removal happens in Cleanup (DeleteOnClose) */
}

static int dir_nonempty_cb(void *cookie, const char *name, const jfs_stat *st)
{
    (void)st;
    if (strcmp(name, ".") && strcmp(name, "..")) {
        *(int *)cookie = 1;
        return 1;
    }
    return 0;
}

static int DOKAN_CALLBACK d_DeleteDirectory(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    int nonempty = 0, rc = jfs_readdir(jfs_mount_fs, p, dir_nonempty_cb, &nonempty);
    (void)fi;
    free(p);
    if (rc)
        return werr(rc);
    return nonempty ? -ERROR_DIR_NOT_EMPTY : 0;
}

static int DOKAN_CALLBACK d_MoveFile(LPCWSTR from, LPCWSTR to, BOOL replace, PDOKAN_FILE_INFO fi)
{
    char *a = upath(from), *b = upath(to);
    int rc = jfs_rename(jfs_mount_fs, a, b, replace ? 0 : JFS_RENAME_NOREPLACE);
    (void)fi;
    free(a);
    free(b);
    return werr(rc);
}

static int DOKAN_CALLBACK d_SetEndOfFile(LPCWSTR name, LONGLONG len, PDOKAN_FILE_INFO fi)
{
    int rc;
    if (len < 0)
        return -ERROR_INVALID_PARAMETER;
    if (fi->Context) {
        rc = jfs_ftruncate(jfs_mount_fs, (jfs_handle *)(uintptr_t)fi->Context, (uint64_t)len);
    } else {
        char *p = upath(name);
        rc = jfs_truncate(jfs_mount_fs, p, (uint64_t)len);
        free(p);
    }
    return werr(rc);
}

static int DOKAN_CALLBACK d_SetAllocationSize(LPCWSTR name, LONGLONG len, PDOKAN_FILE_INFO fi)
{
    char *p = upath(name);
    jfs_stat st;
    int rc = jfs_getattr(jfs_mount_fs, p, &st);
    free(p);
    if (rc)
        return werr(rc);
    if ((uint64_t)len < st.size) /* shrinking the allocation truncates */
        return d_SetEndOfFile(name, len, fi);
    return 0;
}

static int DOKAN_CALLBACK d_LockFile(LPCWSTR n, LONGLONG o, LONGLONG l, PDOKAN_FILE_INFO fi)
{
    (void)n;
    (void)o;
    (void)l;
    (void)fi;
    return 0;
}

static int DOKAN_CALLBACK d_GetDiskFreeSpace(PULONGLONG avail, PULONGLONG total, PULONGLONG tfree,
                                             PDOKAN_FILE_INFO fi)
{
    jfs_statvfs sv;
    int rc = jfs_statfs(jfs_mount_fs, &sv);
    (void)fi;
    if (rc)
        return werr(rc);
    *avail = sv.bavail * sv.bsize;
    *total = sv.blocks * sv.bsize;
    *tfree = sv.bfree * sv.bsize;
    return 0;
}

static int DOKAN_CALLBACK d_GetVolumeInformation(LPWSTR vname, DWORD vlen, LPDWORD serial, LPDWORD maxcomp,
                                                 LPDWORD flags, LPWSTR fsname, DWORD fslen, PDOKAN_FILE_INFO fi)
{
    jfs_info in;
    wchar_t *w;
    (void)fi;
    jfs_get_info(jfs_mount_fs, &in);
    w = jfs_utf8_to_wide(jfs_mount_cfg.volname ? jfs_mount_cfg.volname : (in.label[0] ? in.label : "JSONfs"));
    if (vlen) {
        wcsncpy(vname, w, vlen - 1);
        vname[vlen - 1] = 0;
    }
    free(w);
    *serial = volume_serial();
    *maxcomp = 255;
    *flags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
             (in.readonly ? FILE_READ_ONLY_VOLUME : 0);
    if (fslen) {
        wcsncpy(fsname, L"JSONFS", fslen - 1);
        fsname[fslen - 1] = 0;
    }
    return 0;
}

static int DOKAN_CALLBACK d_Unmount(PDOKAN_FILE_INFO fi)
{
    (void)fi;
    jfs_sync(jfs_mount_fs);
    return 0;
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    if (mount_point_w) {
        if (wcslen(mount_point_w) <= 3 && pDokanUnmount)
            pDokanUnmount(mount_point_w[0]);
        else if (pDokanRemoveMountPoint)
            pDokanRemoveMountPoint(mount_point_w);
    }
    return TRUE;
}

int jfs_dokan_run(void)
{
    static DOKAN_OPERATIONS ops;
    static DOKAN_OPTIONS opt;
    const char *mp = jfs_mount_cfg.mountpoint;
    char *m;
    int rc;
    memset(&ops, 0, sizeof ops);
    ops.CreateFile = d_CreateFile;
    ops.OpenDirectory = d_OpenDirectory;
    ops.CreateDirectory = d_CreateDirectory;
    ops.Cleanup = d_Cleanup;
    ops.CloseFile = d_CloseFile;
    ops.ReadFile = d_ReadFile;
    ops.WriteFile = d_WriteFile;
    ops.FlushFileBuffers = d_FlushFileBuffers;
    ops.GetFileInformation = d_GetFileInformation;
    ops.FindFiles = d_FindFiles;
    ops.FindFilesWithPattern = NULL; /* Dokan filters FindFiles results itself */
    ops.SetFileAttributes = d_SetFileAttributes;
    ops.SetFileTime = d_SetFileTime;
    ops.DeleteFile = d_DeleteFile;
    ops.DeleteDirectory = d_DeleteDirectory;
    ops.MoveFile = d_MoveFile;
    ops.SetEndOfFile = d_SetEndOfFile;
    ops.SetAllocationSize = d_SetAllocationSize;
    ops.LockFile = d_LockFile;
    ops.UnlockFile = d_LockFile;
    ops.GetDiskFreeSpace = d_GetDiskFreeSpace;
    ops.GetVolumeInformation = d_GetVolumeInformation;
    ops.Unmount = d_Unmount;

    /* "X" / "X:" -> "X:\" */
    if (strlen(mp) == 1 || (strlen(mp) == 2 && mp[1] == ':'))
        m = jfs_asprintf("%c:\\", mp[0]);
    else
        m = jfs_strdup(mp);
    mount_point_w = jfs_utf8_to_wide(m);
    free(m);

    memset(&opt, 0, sizeof opt);
    opt.Version = DOKAN_VERSION_06;
    opt.ThreadCount = jfs_mount_cfg.single_thread ? 1 : 0;
    opt.Options = DOKAN_OPTION_KEEP_ALIVE | DOKAN_OPTION_REMOVABLE;
    opt.MountPoint = mount_point_w;

    jfs_start_committer(jfs_mount_fs);
    jfs_refresh_start(); /* web images: refresh (Dokan 0.6 cannot report change events) */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    fprintf(stderr, "jsonfs: mounting %s on %s via Dokan (Ctrl+C to unmount)\n", jfs_mount_cfg.image,
            jfs_mount_cfg.mountpoint);
    rc = pDokanMain(&opt, &ops);
    jfs_refresh_stop();
    jfs_close(jfs_mount_fs);
    jfs_mount_fs = NULL;
    switch (rc) {
    case 0: return 0;
    case -2: fprintf(stderr, "jsonfs: bad drive letter\n"); break;
    case -3: fprintf(stderr, "jsonfs: cannot install the Dokan driver\n"); break;
    case -4: fprintf(stderr, "jsonfs: the Dokan driver reported an error\n"); break;
    case -5: fprintf(stderr, "jsonfs: cannot assign the drive letter / mount point\n"); break;
    case -6: fprintf(stderr, "jsonfs: invalid mount point\n"); break;
    default: fprintf(stderr, "jsonfs: Dokan error %d\n", rc);
    }
    return 1;
}
#endif
