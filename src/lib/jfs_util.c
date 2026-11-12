/*
 * JSONfs - portability layer and utilities.
 * SPDX-License-Identifier: MIT
 */
#if !defined(_WIN32)
#if !defined(_GNU_SOURCE) && defined(__linux__)
#define _GNU_SOURCE
#endif
#if !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "jfs_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef JFS_WINDOWS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

int jfs_debug = 0;

/* ======================================================================== */
/* Errors                                                                     */
/* ======================================================================== */
const char *jfs_strerror(int err)
{
    if (err < 0)
        err = -err;
    switch (err) {
    case JFS_OK: return "success";
    case JFS_EPERM: return "operation not permitted";
    case JFS_ENOENT: return "no such file or directory";
    case JFS_EIO: return "input/output error";
    case JFS_EBADF: return "bad file handle";
    case JFS_EAGAIN: return "resource temporarily unavailable";
    case JFS_ENOMEM: return "out of memory";
    case JFS_EACCES: return "permission denied";
    case JFS_EBUSY: return "resource busy";
    case JFS_EEXIST: return "file exists";
    case JFS_EXDEV: return "cross-device link";
    case JFS_ENOTDIR: return "not a directory";
    case JFS_EISDIR: return "is a directory";
    case JFS_EINVAL: return "invalid argument";
    case JFS_EFBIG: return "file too large";
    case JFS_ENOSPC: return "no space left on device";
    case JFS_EROFS: return "read-only file system";
    case JFS_EMLINK: return "too many links";
    case JFS_ERANGE: return "result out of range";
    case JFS_ENAMETOOLONG: return "file name too long";
    case JFS_ENOSYS: return "function not implemented";
    case JFS_ENOTEMPTY: return "directory not empty";
    case JFS_ELOOP: return "too many levels of symbolic links";
    case JFS_ENOATTR: return "no such attribute";
    case JFS_ENOTSUP: return "operation not supported";
    case JFS_ECORRUPT: return "image is corrupt or not a JSONfs image";
    case JFS_EVERSION: return "unsupported JSONfs schema version";
    case JFS_ELOCKED: return "image is in use (mounted or locked by another process)";
    case JFS_ECHECKSUM: return "checksum mismatch";
    default: return "unknown error";
    }
}

int jfs_err_to_errno(int err)
{
    if (err < 0)
        err = -err;
    switch (err) {
    case JFS_OK: return 0;
    case JFS_EPERM: return EPERM;
    case JFS_ENOENT: return ENOENT;
    case JFS_EIO: return EIO;
    case JFS_EBADF: return EBADF;
    case JFS_EAGAIN: return EAGAIN;
    case JFS_ENOMEM: return ENOMEM;
    case JFS_EACCES: return EACCES;
    case JFS_EBUSY: return EBUSY;
    case JFS_EEXIST: return EEXIST;
    case JFS_EXDEV: return EXDEV;
    case JFS_ENOTDIR: return ENOTDIR;
    case JFS_EISDIR: return EISDIR;
    case JFS_EINVAL: return EINVAL;
    case JFS_EFBIG: return EFBIG;
    case JFS_ENOSPC: return ENOSPC;
    case JFS_EROFS: return EROFS;
    case JFS_EMLINK: return EMLINK;
    case JFS_ERANGE: return ERANGE;
    case JFS_ENAMETOOLONG: return ENAMETOOLONG;
    case JFS_ENOSYS: return ENOSYS;
    case JFS_ENOTEMPTY: return ENOTEMPTY;
#ifdef ELOOP
    case JFS_ELOOP: return ELOOP;
#endif
    case JFS_ENOATTR:
#if defined(ENOATTR)
        return ENOATTR;
#elif defined(ENODATA)
        return ENODATA;
#else
        return ENOENT;
#endif
    case JFS_ENOTSUP:
#if defined(ENOTSUP)
        return ENOTSUP;
#else
        return ENOSYS;
#endif
    default: return EIO;
    }
}

int jfs_err_from_errno(int e)
{
    switch (e) {
    case 0: return 0;
    case EPERM: return -JFS_EPERM;
    case ENOENT: return -JFS_ENOENT;
    case EACCES: return -JFS_EACCES;
    case EEXIST: return -JFS_EEXIST;
    case ENOTDIR: return -JFS_ENOTDIR;
    case EISDIR: return -JFS_EISDIR;
    case EINVAL: return -JFS_EINVAL;
    case ENOSPC: return -JFS_ENOSPC;
    case EROFS: return -JFS_EROFS;
    case ENOMEM: return -JFS_ENOMEM;
    case EBUSY: return -JFS_EBUSY;
    case ENAMETOOLONG: return -JFS_ENAMETOOLONG;
    default: return -JFS_EIO;
    }
}

/* ======================================================================== */
/* Memory                                                                     */
/* ======================================================================== */
static void oom(void)
{
    fprintf(stderr, "jsonfs: out of memory\n");
    abort();
}

void *jfs_malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *jfs_calloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p)
        oom();
    return p;
}

void *jfs_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        oom();
    return q;
}

char *jfs_strdup(const char *s)
{
    return s ? jfs_strndup(s, strlen(s)) : NULL;
}

char *jfs_strndup(const char *s, size_t n)
{
    char *p = jfs_malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *jfs_asprintf(const char *fmt, ...)
{
    jfs_buf b;
    va_list ap;
    int n;
    jfs_buf_init(&b);
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    jfs_buf_reserve(&b, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(b.data, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b.len = (size_t)n;
    return jfs_buf_steal(&b, NULL);
}

/* ======================================================================== */
/* Buffers                                                                    */
/* ======================================================================== */
void jfs_buf_init(jfs_buf *b)
{
    b->data = NULL;
    b->len = b->cap = 0;
}

void jfs_buf_free(jfs_buf *b)
{
    free(b->data);
    jfs_buf_init(b);
}

void jfs_buf_reserve(jfs_buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < need)
            nc = nc + nc / 2 + 64;
        b->data = jfs_realloc(b->data, nc);
        b->cap = nc;
    }
}

void jfs_buf_append(jfs_buf *b, const void *p, size_t n)
{
    jfs_buf_reserve(b, n);
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

void jfs_buf_putc(jfs_buf *b, char c)
{
    jfs_buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

void jfs_buf_puts(jfs_buf *b, const char *s)
{
    jfs_buf_append(b, s, strlen(s));
}

void jfs_buf_printf(jfs_buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    jfs_buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *jfs_buf_steal(jfs_buf *b, size_t *len)
{
    char *p;
    jfs_buf_reserve(b, 0);
    p = b->data;
    if (len)
        *len = b->len;
    jfs_buf_init(b);
    return p;
}

/* ======================================================================== */
/* Time                                                                       */
/* ======================================================================== */
jfs_time jfs_now(void)
{
    jfs_time t;
#ifdef JFS_WINDOWS
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100ns ticks since 1601-01-01 */
    u.QuadPart -= 116444736000000000ULL;
    t.sec = (int64_t)(u.QuadPart / 10000000ULL);
    t.nsec = (int32_t)((u.QuadPart % 10000000ULL) * 100);
#elif defined(CLOCK_REALTIME)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    t.sec = ts.tv_sec;
    t.nsec = (int32_t)ts.tv_nsec;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    t.sec = tv.tv_sec;
    t.nsec = (int32_t)tv.tv_usec * 1000;
#endif
    return t;
}

int jfs_time_cmp(jfs_time a, jfs_time b)
{
    if (a.sec != b.sec)
        return a.sec < b.sec ? -1 : 1;
    if (a.nsec != b.nsec)
        return a.nsec < b.nsec ? -1 : 1;
    return 0;
}

/* Howard Hinnant's civil calendar algorithms (proleptic Gregorian). */
int64_t jfs_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void jfs_civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    int64_t era;
    unsigned doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int64_t)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y += (*m <= 2);
}

void jfs_time_format(jfs_time t, char out[JFS_TIME_STRLEN])
{
    int64_t days = t.sec / 86400, rem = t.sec % 86400, y;
    unsigned m, d;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    jfs_civil_from_days(days, &y, &m, &d);
    snprintf(out, JFS_TIME_STRLEN, "%04lld-%02u-%02uT%02u:%02u:%02u.%09ldZ", (long long)y, m, d,
             (unsigned)(rem / 3600), (unsigned)(rem / 60 % 60), (unsigned)(rem % 60), (long)t.nsec);
}

static int parse_digits(const char **p, int n, int *out)
{
    int v = 0, i;
    for (i = 0; i < n; i++) {
        if (!isdigit((unsigned char)(*p)[i]))
            return -1;
        v = v * 10 + ((*p)[i] - '0');
    }
    *p += n;
    *out = v;
    return 0;
}

/* Parses [YYYY-MM-DD] then optional [T| ]HH:MM[:SS[.frac]] then optional
 * Z / +HH:MM / -HH:MM. require_time: the stored format always has time. */
static int parse_iso(const char *s, jfs_time *out, int require_time)
{
    const char *p = s;
    int Y, M, D, h = 0, mi = 0, se = 0;
    int64_t nsec = 0, off = 0;
    int neg = 0;
    if (*p == '-' || *p == '+')
        return -1;
    if (parse_digits(&p, 4, &Y) || *p++ != '-' || parse_digits(&p, 2, &M) || *p++ != '-' ||
        parse_digits(&p, 2, &D))
        return -1;
    if (M < 1 || M > 12 || D < 1 || D > 31)
        return -1;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        if (parse_digits(&p, 2, &h) || *p++ != ':' || parse_digits(&p, 2, &mi))
            return -1;
        if (*p == ':') {
            p++;
            if (parse_digits(&p, 2, &se))
                return -1;
            if (*p == '.' || *p == ',') {
                int digits = 0;
                p++;
                if (!isdigit((unsigned char)*p))
                    return -1;
                while (isdigit((unsigned char)*p)) {
                    if (digits < 9) {
                        nsec = nsec * 10 + (*p - '0');
                        digits++;
                    }
                    p++;
                }
                while (digits++ < 9)
                    nsec *= 10;
            }
        }
        if (h > 23 || mi > 59 || se > 60)
            return -1;
    } else if (require_time) {
        return -1;
    }
    if (*p == 'Z' || *p == 'z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        int oh, om;
        neg = *p == '-';
        p++;
        if (parse_digits(&p, 2, &oh))
            return -1;
        if (*p == ':')
            p++;
        if (parse_digits(&p, 2, &om))
            return -1;
        off = (int64_t)oh * 3600 + om * 60;
        if (neg)
            off = -off;
    }
    if (*p)
        return -1;
    out->sec = jfs_days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + se - off;
    out->nsec = (int32_t)nsec;
    return 0;
}

int jfs_time_parse(const char *s, jfs_time *out)
{
    if (!s)
        return -1;
    return parse_iso(s, out, 1);
}

int jfs_time_parse_user(const char *s, jfs_time *out)
{
    char *end;
    if (!s || !*s)
        return -1;
    while (isspace((unsigned char)*s))
        s++;
    if (!strcmp(s, "now")) {
        *out = jfs_now();
        return 0;
    }
    if (*s == '-' && isdigit((unsigned char)s[1])) {
        /* relative */
        double v = strtod(s + 1, &end);
        double mult = 1;
        switch (*end) {
        case 's': mult = 1; break;
        case 'm': mult = 60; break;
        case 'h': mult = 3600; break;
        case 'd': mult = 86400; break;
        case 'w': mult = 7 * 86400; break;
        case 0: mult = 1; break;
        default: return -1;
        }
        if (*end && end[1])
            return -1;
        {
            jfs_time now = jfs_now();
            double delta = v * mult;
            int64_t ds = (int64_t)delta;
            int64_t dns = (int64_t)((delta - (double)ds) * 1e9);
            now.sec -= ds;
            now.nsec -= (int32_t)dns;
            if (now.nsec < 0) {
                now.nsec += 1000000000;
                now.sec--;
            }
            *out = now;
        }
        return 0;
    }
    if (*s == '@' || (isdigit((unsigned char)*s) && strlen(s) != 10 && !strchr(s, '-'))) {
        const char *q = *s == '@' ? s + 1 : s;
        double v = strtod(q, &end);
        if (*end || end == q)
            return -1;
        out->sec = (int64_t)v;
        out->nsec = (int32_t)((v - (double)out->sec) * 1e9);
        return 0;
    }
    return parse_iso(s, out, 0);
}

/* ======================================================================== */
/* Random / UUID                                                              */
/* ======================================================================== */
int jfs_random_bytes(void *out, size_t n)
{
#ifdef JFS_WINDOWS
    HCRYPTPROV prov;
    int ok = 0;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        ok = CryptGenRandom(prov, (DWORD)n, (BYTE *)out) ? 1 : 0;
        CryptReleaseContext(prov, 0);
    }
    if (ok)
        return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, (char *)out + got, n - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        close(fd);
        if (got == n)
            return 0;
    }
#endif
    {
        /* Last resort: time-seeded xorshift (identifiers only, not security). */
        static uint64_t s;
        size_t i;
        jfs_time t = jfs_now();
        s ^= (uint64_t)t.sec * 1000000007ULL ^ (uint64_t)t.nsec ^ (uint64_t)(uintptr_t)out;
        for (i = 0; i < n; i++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            ((unsigned char *)out)[i] = (unsigned char)(s >> 24);
        }
    }
    return 0;
}

void jfs_uuid4(char out[37])
{
    unsigned char b[16];
    static const char hex[] = "0123456789abcdef";
    int i, j = 0;
    jfs_random_bytes(b, sizeof b);
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[j++] = '-';
        out[j++] = hex[b[i] >> 4];
        out[j++] = hex[b[i] & 15];
    }
    out[j] = 0;
}

int jfs_uuid_valid(const char *s)
{
    int i;
    if (!s || strlen(s) != 36)
        return 0;
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

/* ======================================================================== */
/* Files                                                                      */
/* ======================================================================== */
#ifdef JFS_WINDOWS
wchar_t *jfs_utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0)
        n = 1;
    w = jfs_malloc((size_t)n * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

char *jfs_wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0)
        n = 1;
    s = jfs_malloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = 0;
    return s;
}

static int win_err(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return -JFS_ENOENT;
    case ERROR_ACCESS_DENIED: return -JFS_EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return -JFS_ELOCKED;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return -JFS_ENOSPC;
    case ERROR_WRITE_PROTECT: return -JFS_EROFS;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return -JFS_ENOMEM;
    default: return -JFS_EIO;
    }
}
#endif

int jfs_read_file(const char *path, char **data, size_t *len)
{
#ifdef JFS_WINDOWS
    wchar_t *w = jfs_utf8_to_wide(path);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    char *buf;
    size_t got = 0;
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return win_err(GetLastError());
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return -JFS_EIO;
    }
    buf = jfs_malloc((size_t)sz.QuadPart + 1);
    while (got < (size_t)sz.QuadPart) {
        DWORD chunk = (DWORD)(((size_t)sz.QuadPart - got) > 0x10000000 ? 0x10000000 : ((size_t)sz.QuadPart - got));
        DWORD r = 0;
        if (!ReadFile(h, buf + got, chunk, &r, NULL) || r == 0)
            break;
        got += r;
    }
    CloseHandle(h);
    buf[got] = 0;
    *data = buf;
    *len = got;
    return 0;
#else
    int fd = open(path, O_RDONLY);
    struct stat st;
    char *buf;
    size_t got = 0, cap;
    if (fd < 0)
        return jfs_err_from_errno(errno);
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -JFS_EIO;
    }
    cap = (size_t)st.st_size + 1;
    buf = jfs_malloc(cap);
    for (;;) {
        ssize_t r;
        if (got + 1 >= cap) {
            cap = cap * 2;
            buf = jfs_realloc(buf, cap);
        }
        r = read(fd, buf + got, cap - got - 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return -JFS_EIO;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    *data = buf;
    *len = got;
    return 0;
#endif
}

int jfs_write_file_atomic(const char *path, const void *data, size_t len)
{
    char *tmp;
    char rnd[8];
    unsigned char rb[4];
    jfs_random_bytes(rb, sizeof rb);
    snprintf(rnd, sizeof rnd, "%02x%02x%02x", rb[0], rb[1], rb[2]);
    tmp = jfs_asprintf("%s.tmp-%s", path, rnd);
#ifdef JFS_WINDOWS
    {
        wchar_t *wt = jfs_utf8_to_wide(tmp), *wp = jfs_utf8_to_wide(path);
        HANDLE h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        size_t done = 0;
        int rc = 0;
        if (h == INVALID_HANDLE_VALUE) {
            rc = win_err(GetLastError());
            goto wout;
        }
        while (done < len) {
            DWORD chunk = (DWORD)((len - done) > 0x10000000 ? 0x10000000 : (len - done));
            DWORD w = 0;
            if (!WriteFile(h, (const char *)data + done, chunk, &w, NULL) || w == 0) {
                rc = win_err(GetLastError());
                break;
            }
            done += w;
        }
        if (!rc && !FlushFileBuffers(h))
            rc = -JFS_EIO;
        CloseHandle(h);
        if (!rc && !MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            rc = win_err(GetLastError());
        if (rc)
            DeleteFileW(wt);
    wout:
        free(wt);
        free(wp);
        free(tmp);
        return rc;
    }
#else
    {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
        size_t done = 0;
        int rc = 0;
        struct stat st;
        if (fd < 0) {
            rc = jfs_err_from_errno(errno);
            free(tmp);
            return rc;
        }
        /* keep the permissions/ownership of the image we replace */
        if (stat(path, &st) == 0) {
            if (fchmod(fd, st.st_mode & 07777) != 0) { /* best effort */
            }
            if (fchown(fd, st.st_uid, st.st_gid) != 0) { /* best effort */
            }
        }
        while (done < len) {
            ssize_t w = write(fd, (const char *)data + done, len - done);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = errno == ENOSPC ? -JFS_ENOSPC : -JFS_EIO;
                break;
            }
            done += (size_t)w;
        }
        if (!rc && fsync(fd) != 0)
            rc = -JFS_EIO;
        if (close(fd) != 0 && !rc)
            rc = -JFS_EIO;
        if (!rc && rename(tmp, path) != 0)
            rc = jfs_err_from_errno(errno);
        if (rc) {
            unlink(tmp);
        } else {
            /* make the rename durable */
            char *dir = jfs_strdup(path), *slash = strrchr(dir, '/');
            int dfd;
            if (slash) {
                if (slash == dir)
                    slash[1] = 0;
                else
                    *slash = 0;
            } else {
                strcpy(dir, ".");
            }
            dfd = open(dir, O_RDONLY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
            free(dir);
        }
        free(tmp);
        return rc;
    }
#endif
}

int jfs_file_exists(const char *path)
{
#ifdef JFS_WINDOWS
    wchar_t *w = jfs_utf8_to_wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int jfs_remove_file(const char *path)
{
#ifdef JFS_WINDOWS
    wchar_t *w = jfs_utf8_to_wide(path);
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : win_err(GetLastError());
#else
    return unlink(path) == 0 ? 0 : jfs_err_from_errno(errno);
#endif
}

int jfs_disk_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes)
{
    char *dir = jfs_strdup(path), *slash;
    int rc = 0;
#ifdef JFS_WINDOWS
    wchar_t *w;
    ULARGE_INTEGER avail, total, tfree;
    slash = strrchr(dir, '\\');
    if (!slash || (strrchr(dir, '/') && strrchr(dir, '/') > slash))
        slash = strrchr(dir, '/');
    if (slash)
        slash[1] = 0;
    else
        strcpy(dir, ".\\");
    w = jfs_utf8_to_wide(dir);
    if (GetDiskFreeSpaceExW(w, &avail, &total, &tfree)) {
        *free_bytes = avail.QuadPart;
        *total_bytes = total.QuadPart;
    } else {
        rc = -JFS_EIO;
    }
    free(w);
#else
    struct statvfs sv;
    slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir)
            slash[1] = 0;
        else
            *slash = 0;
    } else {
        strcpy(dir, ".");
    }
    if (statvfs(dir, &sv) == 0) {
        *free_bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
        *total_bytes = (uint64_t)sv.f_blocks * (uint64_t)sv.f_frsize;
    } else {
        rc = -JFS_EIO;
    }
#endif
    free(dir);
    return rc;
}

int jfs_file_signature(const char *path, jfs_filesig *sig)
{
    memset(sig, 0, sizeof *sig);
#ifdef JFS_WINDOWS
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        wchar_t *w = jfs_utf8_to_wide(path);
        BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
        free(w);
        if (!ok)
            return -JFS_ENOENT;
        sig->size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        sig->mtime_sec = (int64_t)(((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
                                   fad.ftLastWriteTime.dwLowDateTime);
        sig->id = ((uint64_t)fad.ftCreationTime.dwHighDateTime << 32) | fad.ftCreationTime.dwLowDateTime;
    }
#else
    {
        struct stat st;
        if (stat(path, &st) != 0)
            return jfs_err_from_errno(errno);
        sig->size = (uint64_t)st.st_size;
        sig->mtime_sec = (int64_t)st.st_mtime;
#if defined(__APPLE__)
        sig->mtime_nsec = st.st_mtimespec.tv_nsec;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        sig->mtime_nsec = st.st_mtim.tv_nsec;
#endif
        sig->id = (uint64_t)st.st_ino ^ ((uint64_t)st.st_dev << 32);
    }
#endif
    sig->valid = 1;
    return 0;
}

int jfs_filesig_equal(const jfs_filesig *a, const jfs_filesig *b)
{
    return a->valid && b->valid && a->size == b->size && a->mtime_sec == b->mtime_sec &&
           a->mtime_nsec == b->mtime_nsec && a->id == b->id;
}

/* ======================================================================== */
/* Lock file                                                                  */
/* ======================================================================== */
#define JFS_LOCK_BASE ((uint64_t)1 << 30)
#define JFS_LOCK_PAYLOAD 96

struct jfs_lockfile {
    char *path;
#ifdef JFS_WINDOWS
    HANDLE h;
#else
    int fd;
#endif
    int held[2];
};

int jfs_lockfile_open(const char *image_path, jfs_lockfile **out)
{
    jfs_lockfile *lf = jfs_calloc(1, sizeof *lf);
    lf->path = jfs_asprintf("%s.lock", image_path);
#ifdef JFS_WINDOWS
    {
        wchar_t *w = jfs_utf8_to_wide(lf->path);
        lf->h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(w);
        if (lf->h == INVALID_HANDLE_VALUE) {
            int rc = win_err(GetLastError());
            free(lf->path);
            free(lf);
            return rc;
        }
    }
#else
    lf->fd = open(lf->path, O_RDWR | O_CREAT, 0666);
    if (lf->fd < 0) {
        int rc = jfs_err_from_errno(errno);
        /* read-only media: fall back to no-op locking */
        if (errno == EROFS || errno == EACCES) {
            lf->fd = -1;
            *out = lf;
            return 0;
        }
        free(lf->path);
        free(lf);
        return rc;
    }
#endif
    *out = lf;
    return 0;
}

int jfs_lockfile_lock(jfs_lockfile *lf, int which, int exclusive, int wait)
{
#ifdef JFS_WINDOWS
    OVERLAPPED ov;
    DWORD flags = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    uint64_t off = JFS_LOCK_BASE + (uint64_t)which;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (!LockFileEx(lf->h, flags, 0, 1, 0, &ov))
        return -JFS_ELOCKED;
#else
    struct flock fl;
    if (lf->fd < 0)
        return 0;
    memset(&fl, 0, sizeof fl);
    fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)(JFS_LOCK_BASE + (uint64_t)which);
    fl.l_len = 1;
    while (fcntl(lf->fd, wait ? F_SETLKW : F_SETLK, &fl) != 0) {
        if (errno == EINTR)
            continue;
        if (errno == ENOLCK || errno == ENOSYS || errno == EOPNOTSUPP)
            return 0; /* filesystem without locking support: best effort */
        return -JFS_ELOCKED;
    }
#endif
    lf->held[which] = 1;
    return 0;
}

int jfs_lockfile_lock_timeout(jfs_lockfile *lf, int which, int exclusive, unsigned timeout_ms)
{
    unsigned waited = 0;
    for (;;) {
        int rc = jfs_lockfile_lock(lf, which, exclusive, 0);
        if (rc != -JFS_ELOCKED || waited >= timeout_ms)
            return rc;
        jfs_sleep_ms(100);
        waited += 100;
    }
}

unsigned jfs_lock_timeout_ms(void)
{
    const char *e = getenv("JSONFS_LOCK_TIMEOUT");
    if (e && *e)
        return (unsigned)(atof(e) * 1000);
    return 10000;
}

void jfs_lockfile_unlock(jfs_lockfile *lf, int which)
{
    if (!lf || !lf->held[which])
        return;
#ifdef JFS_WINDOWS
    {
        OVERLAPPED ov;
        uint64_t off = JFS_LOCK_BASE + (uint64_t)which;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        UnlockFileEx(lf->h, 0, 1, 0, &ov);
    }
#else
    {
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = (off_t)(JFS_LOCK_BASE + (uint64_t)which);
        fl.l_len = 1;
        fcntl(lf->fd, F_SETLK, &fl);
    }
#endif
    lf->held[which] = 0;
}

int jfs_lockfile_read_gen(jfs_lockfile *lf, uint64_t *gen, char uuid[37])
{
    char buf[JFS_LOCK_PAYLOAD + 1];
    unsigned long long g = 0;
    char u[64] = "";
    size_t n = 0;
#ifdef JFS_WINDOWS
    OVERLAPPED ov;
    DWORD r = 0;
    memset(&ov, 0, sizeof ov);
    if (ReadFile(lf->h, buf, JFS_LOCK_PAYLOAD, &r, &ov))
        n = r;
#else
    ssize_t r;
    if (lf->fd < 0)
        return -JFS_EIO;
    r = pread(lf->fd, buf, JFS_LOCK_PAYLOAD, 0);
    if (r > 0)
        n = (size_t)r;
#endif
    buf[n] = 0;
    if (sscanf(buf, "JSONFS-LOCK %llu %63s", &g, u) < 1)
        return -JFS_ENOENT;
    *gen = g;
    if (uuid) {
        strncpy(uuid, u, 36);
        uuid[36] = 0;
    }
    return 0;
}

int jfs_lockfile_write_gen(jfs_lockfile *lf, uint64_t gen, const char *uuid)
{
    char buf[JFS_LOCK_PAYLOAD];
    memset(buf, ' ', sizeof buf);
    snprintf(buf, sizeof buf, "JSONFS-LOCK %llu %s", (unsigned long long)gen, uuid ? uuid : "-");
    buf[strlen(buf)] = ' ';
    buf[sizeof buf - 1] = '\n';
#ifdef JFS_WINDOWS
    {
        OVERLAPPED ov;
        DWORD w = 0;
        memset(&ov, 0, sizeof ov);
        if (!WriteFile(lf->h, buf, sizeof buf, &w, &ov) || w != sizeof buf)
            return -JFS_EIO;
    }
#else
    if (lf->fd < 0)
        return 0;
    if (pwrite(lf->fd, buf, sizeof buf, 0) != (ssize_t)sizeof buf)
        return -JFS_EIO;
#endif
    return 0;
}

void jfs_lockfile_close(jfs_lockfile *lf, int remove_file)
{
    if (!lf)
        return;
    jfs_lockfile_unlock(lf, JFS_LOCK_TXN);
    jfs_lockfile_unlock(lf, JFS_LOCK_ATTACH);
#ifdef JFS_WINDOWS
    CloseHandle(lf->h);
#else
    if (lf->fd >= 0)
        close(lf->fd);
#endif
    if (remove_file)
        jfs_remove_file(lf->path);
    free(lf->path);
    free(lf);
}

/* ======================================================================== */
/* Threads                                                                    */
/* ======================================================================== */
void jfs_mutex_init(jfs_mutex *m)
{
#ifdef JFS_WINDOWS
    CRITICAL_SECTION *cs = jfs_malloc(sizeof *cs);
    InitializeCriticalSection(cs);
    m->impl = cs;
#else
    pthread_mutex_t *pm = jfs_malloc(sizeof *pm);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(pm, &a);
    pthread_mutexattr_destroy(&a);
    m->impl = pm;
#endif
}

void jfs_mutex_destroy(jfs_mutex *m)
{
    if (!m->impl)
        return;
#ifdef JFS_WINDOWS
    DeleteCriticalSection((CRITICAL_SECTION *)m->impl);
#else
    pthread_mutex_destroy((pthread_mutex_t *)m->impl);
#endif
    free(m->impl);
    m->impl = NULL;
}

void jfs_mutex_lock(jfs_mutex *m)
{
#ifdef JFS_WINDOWS
    EnterCriticalSection((CRITICAL_SECTION *)m->impl);
#else
    pthread_mutex_lock((pthread_mutex_t *)m->impl);
#endif
}

void jfs_mutex_unlock(jfs_mutex *m)
{
#ifdef JFS_WINDOWS
    LeaveCriticalSection((CRITICAL_SECTION *)m->impl);
#else
    pthread_mutex_unlock((pthread_mutex_t *)m->impl);
#endif
}

struct jfs_thread {
    void (*fn)(void *);
    void *arg;
#ifdef JFS_WINDOWS
    HANDLE h;
#else
    pthread_t t;
#endif
};

#ifdef JFS_WINDOWS
static DWORD WINAPI thread_tramp(LPVOID p)
{
    jfs_thread *t = p;
    t->fn(t->arg);
    return 0;
}
#else
static void *thread_tramp(void *p)
{
    jfs_thread *t = p;
    t->fn(t->arg);
    return NULL;
}
#endif

int jfs_thread_start(jfs_thread **out, void (*fn)(void *), void *arg)
{
    jfs_thread *t = jfs_calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
#ifdef JFS_WINDOWS
    t->h = CreateThread(NULL, 0, thread_tramp, t, 0, NULL);
    if (!t->h) {
        free(t);
        return -JFS_EAGAIN;
    }
#else
    if (pthread_create(&t->t, NULL, thread_tramp, t) != 0) {
        free(t);
        return -JFS_EAGAIN;
    }
#endif
    *out = t;
    return 0;
}

void jfs_thread_join(jfs_thread *t)
{
    if (!t)
        return;
#ifdef JFS_WINDOWS
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
#else
    pthread_join(t->t, NULL);
#endif
    free(t);
}

void jfs_sleep_ms(unsigned ms)
{
#ifdef JFS_WINDOWS
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

/* ======================================================================== */
/* Hash maps (open addressing, linear probing, backward-shift deletion)       */
/* ======================================================================== */
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return mix64(h);
}

void jfs_u64map_init(jfs_u64map *m)
{
    memset(m, 0, sizeof *m);
}

void jfs_u64map_free(jfs_u64map *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

static void u64map_grow(jfs_u64map *m)
{
    size_t oc = m->cap, i;
    uint64_t *ok = m->keys;
    void **ov = m->vals;
    m->cap = oc ? oc * 2 : 64;
    m->keys = jfs_calloc(m->cap, sizeof(uint64_t));
    m->vals = jfs_calloc(m->cap, sizeof(void *));
    m->count = 0;
    for (i = 0; i < oc; i++)
        if (ov[i])
            jfs_u64map_put(m, ok[i], ov[i]);
    free(ok);
    free(ov);
}

void *jfs_u64map_get(const jfs_u64map *m, uint64_t key)
{
    size_t i, mask;
    if (!m->cap)
        return NULL;
    mask = m->cap - 1;
    for (i = (size_t)mix64(key) & mask; m->vals[i]; i = (i + 1) & mask)
        if (m->keys[i] == key)
            return m->vals[i];
    return NULL;
}

void jfs_u64map_put(jfs_u64map *m, uint64_t key, void *val)
{
    size_t i, mask;
    if (!val)
        return;
    if ((m->count + 1) * 4 >= m->cap * 3)
        u64map_grow(m);
    mask = m->cap - 1;
    for (i = (size_t)mix64(key) & mask; m->vals[i]; i = (i + 1) & mask) {
        if (m->keys[i] == key) {
            m->vals[i] = val;
            return;
        }
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
}

void *jfs_u64map_del(jfs_u64map *m, uint64_t key)
{
    size_t i, j, mask;
    void *v;
    if (!m->cap)
        return NULL;
    mask = m->cap - 1;
    for (i = (size_t)mix64(key) & mask; m->vals[i]; i = (i + 1) & mask)
        if (m->keys[i] == key)
            break;
    if (!m->vals[i])
        return NULL;
    v = m->vals[i];
    m->vals[i] = NULL;
    m->count--;
    /* backward shift */
    j = i;
    for (;;) {
        size_t k;
        j = (j + 1) & mask;
        if (!m->vals[j])
            break;
        k = (size_t)mix64(m->keys[j]) & mask;
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            m->vals[j] = NULL;
            i = j;
        }
    }
    return v;
}

void jfs_strmap_init(jfs_strmap *m)
{
    memset(m, 0, sizeof *m);
}

void jfs_strmap_free(jfs_strmap *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++)
        free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

static void strmap_insert_owned(jfs_strmap *m, char *key, uint64_t val);

static void strmap_grow(jfs_strmap *m)
{
    size_t oc = m->cap, i;
    char **ok = m->keys;
    uint64_t *ov = m->vals;
    m->cap = oc ? oc * 2 : 16;
    m->keys = jfs_calloc(m->cap, sizeof(char *));
    m->vals = jfs_calloc(m->cap, sizeof(uint64_t));
    m->count = 0;
    for (i = 0; i < oc; i++)
        if (ok[i])
            strmap_insert_owned(m, ok[i], ov[i]);
    free(ok);
    free(ov);
}

static void strmap_insert_owned(jfs_strmap *m, char *key, uint64_t val)
{
    size_t i, mask;
    if ((m->count + 1) * 4 >= m->cap * 3)
        strmap_grow(m);
    mask = m->cap - 1;
    for (i = (size_t)hash_str(key) & mask; m->keys[i]; i = (i + 1) & mask) {
        if (!strcmp(m->keys[i], key)) {
            free(key);
            m->vals[i] = val;
            return;
        }
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
}

int jfs_strmap_get(const jfs_strmap *m, const char *key, uint64_t *val)
{
    size_t i, mask;
    if (!m->cap)
        return 0;
    mask = m->cap - 1;
    for (i = (size_t)hash_str(key) & mask; m->keys[i]; i = (i + 1) & mask) {
        if (!strcmp(m->keys[i], key)) {
            if (val)
                *val = m->vals[i];
            return 1;
        }
    }
    return 0;
}

void jfs_strmap_put(jfs_strmap *m, const char *key, uint64_t val)
{
    size_t i, mask;
    if (m->cap) {
        mask = m->cap - 1;
        for (i = (size_t)hash_str(key) & mask; m->keys[i]; i = (i + 1) & mask) {
            if (!strcmp(m->keys[i], key)) {
                m->vals[i] = val;
                return;
            }
        }
    }
    strmap_insert_owned(m, jfs_strdup(key), val);
}

int jfs_strmap_del(jfs_strmap *m, const char *key)
{
    size_t i, j, mask;
    if (!m->cap)
        return 0;
    mask = m->cap - 1;
    for (i = (size_t)hash_str(key) & mask; m->keys[i]; i = (i + 1) & mask)
        if (!strcmp(m->keys[i], key))
            break;
    if (!m->keys[i])
        return 0;
    free(m->keys[i]);
    m->keys[i] = NULL;
    m->count--;
    j = i;
    for (;;) {
        size_t k;
        j = (j + 1) & mask;
        if (!m->keys[j])
            break;
        k = (size_t)hash_str(m->keys[j]) & mask;
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            m->keys[j] = NULL;
            i = j;
        }
    }
    return 1;
}

void jfs_strmap_copy(jfs_strmap *dst, const jfs_strmap *src)
{
    size_t i;
    jfs_strmap_init(dst);
    for (i = 0; i < src->cap; i++)
        if (src->keys[i])
            jfs_strmap_put(dst, src->keys[i], src->vals[i]);
}

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

const char **jfs_strmap_sorted_keys(const jfs_strmap *m)
{
    const char **out = jfs_malloc((m->count + 1) * sizeof(char *));
    size_t i, n = 0;
    for (i = 0; i < m->cap; i++)
        if (m->keys[i])
            out[n++] = m->keys[i];
    qsort(out, n, sizeof(char *), cmp_strp);
    out[n] = NULL;
    return out;
}

/* ======================================================================== */
/* Logging                                                                    */
/* ======================================================================== */
static void (*log_sink)(const char *);

void jfs_set_log_sink(void (*sink)(const char *msg))
{
    log_sink = sink;
}

void jfs_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_sink)
        log_sink(buf);
    else
        fprintf(stderr, "jsonfs: %s\n", buf);
}
