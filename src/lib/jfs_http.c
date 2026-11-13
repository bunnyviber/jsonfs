/*
 * JSONfs - fetching images over HTTP(S).  See jfs_http.h for the policy.
 * SPDX-License-Identifier: MIT
 */
#include "jfs_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JFS_HTTP_MAX_REDIRECTS 5
#define JFS_HTTP_USER_AGENT "jsonfs/" JFS_VERSION_STRING

static int g_insecure;
static char *g_cafile;

void jfs_http_configure(int insecure, const char *cafile)
{
    g_insecure = insecure ? 1 : 0;
    free(g_cafile);
    g_cafile = cafile && *cafile ? jfs_strdup(cafile) : NULL;
}

int jfs_http_insecure(void)
{
    return g_insecure;
}

static int has_prefix_ci(const char *s, const char *p)
{
    while (*p) {
        if (tolower((unsigned char)*s) != *p)
            return 0;
        s++;
        p++;
    }
    return 1;
}

int jfs_is_url(const char *path)
{
    return path && (has_prefix_ci(path, "http://") || has_prefix_ci(path, "https://"));
}

static int is_https(const char *url)
{
    return has_prefix_ci(url, "https://");
}

void jfs_url_redact(const char *url, char *out, size_t outlen)
{
    const char *scheme_end = strstr(url, "://"), *host, *slash, *at;
    if (!scheme_end) {
        snprintf(out, outlen, "%s", url);
        return;
    }
    host = scheme_end + 3;
    slash = strchr(host, '/');
    at = strchr(host, '@');
    if (at && (!slash || at < slash))
        snprintf(out, outlen, "%.*s***@%s", (int)(host - url), url, at + 1);
    else
        snprintf(out, outlen, "%s", url);
}

int jfs_load_bytes(const char *path, char **data, size_t *len, char *err, size_t errlen)
{
    int rc;
    if (jfs_is_url(path))
        return jfs_http_get(path, data, len, err, errlen);
    rc = jfs_read_file(path, data, len);
    if (rc && err)
        snprintf(err, errlen, "cannot read %s: %s", path, jfs_strerror(rc));
    return rc;
}

static int status_error(long code)
{
    if (code == 404 || code == 410)
        return -JFS_ENOENT;
    if (code == 401 || code == 403)
        return -JFS_EACCES;
    return -JFS_EREMOTE;
}

#ifndef _WIN32
/* ======================================================================== */
/* POSIX: libcurl via dlopen                                                  */
/* ======================================================================== */
#include <dlfcn.h>

/* libcurl ABI (stable since 7.x): option numbers from curl/curl.h */
#define C_OPT_WRITEDATA 10001
#define C_OPT_URL 10002
#define C_OPT_ERRORBUFFER 10010
#define C_OPT_USERAGENT 10018
#define C_OPT_CAINFO 10065
#define C_OPT_ACCEPT_ENCODING 10102
#define C_OPT_PROTOCOLS_STR 10318       /* 7.85+ */
#define C_OPT_REDIR_PROTOCOLS_STR 10319 /* 7.85+ */
#define C_OPT_WRITEFUNCTION 20011
#define C_OPT_LOW_SPEED_LIMIT 19
#define C_OPT_LOW_SPEED_TIME 20
#define C_OPT_FOLLOWLOCATION 52
#define C_OPT_SSL_VERIFYPEER 64
#define C_OPT_MAXREDIRS 68
#define C_OPT_CONNECTTIMEOUT 78
#define C_OPT_SSL_VERIFYHOST 81
#define C_OPT_NOSIGNAL 99
#define C_OPT_PROTOCOLS 181       /* deprecated, still supported */
#define C_OPT_REDIR_PROTOCOLS 182 /* deprecated, still supported */
#define C_PROTO_HTTP 1L
#define C_PROTO_HTTPS 2L
#define C_INFO_RESPONSE_CODE 0x200002
#define C_INFO_REDIRECT_URL 0x10001F
#define C_GLOBAL_DEFAULT 3L
#define C_ERROR_SIZE 256
#define C_E_OK 0
#define C_E_UNSUPPORTED_PROTOCOL 1
#define C_E_COULDNT_RESOLVE_HOST 6
#define C_E_COULDNT_CONNECT 7
#define C_E_WRITE_ERROR 23
#define C_E_OPERATION_TIMEDOUT 28
#define C_E_TOO_MANY_REDIRECTS 47
#define C_E_UNKNOWN_OPTION 48
#define C_E_PEER_FAILED_VERIFICATION 60
#define C_E_SSL_CACERT_BADFILE 77

typedef void CURL;
static struct {
    int loaded;
    char why[200];
    long (*global_init)(long);
    CURL *(*easy_init)(void);
    int (*easy_setopt)(CURL *, int, ...);
    int (*easy_perform)(CURL *);
    int (*easy_getinfo)(CURL *, int, ...);
    const char *(*easy_strerror)(int);
    void (*easy_cleanup)(CURL *);
} cu;
static jfs_mutex cu_mu;
static int cu_mu_init;

static int curl_load(char *why, size_t whylen)
{
    static const char *cands[] = {
#if defined(__APPLE__)
        "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "/opt/homebrew/opt/curl/lib/libcurl.4.dylib",
        "/usr/local/opt/curl/lib/libcurl.4.dylib",
#else
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "/usr/local/lib/libcurl.so.4", "libcurl.so",
#endif
        NULL};
    const char *env = getenv("JSONFS_CURL_LIBRARY");
    void *h = NULL;
    int i;
    void *sym;
    if (cu.loaded)
        return 1;
    if (env && *env)
        h = dlopen(env, RTLD_NOW | RTLD_LOCAL);
    for (i = 0; !h && cands[i]; i++)
        h = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(why, whylen,
                 "libcurl is required for http:// and https:// images but was not found "
#if defined(__FreeBSD__)
                 "(pkg install curl)"
#elif defined(__APPLE__)
                 "(it ships with macOS; set JSONFS_CURL_LIBRARY if it was removed)"
#else
                 "(install libcurl4 / libcurl or set JSONFS_CURL_LIBRARY)"
#endif
        );
        return 0;
    }
#define SYM(field, name)                                       \
    do {                                                       \
        sym = dlsym(h, name);                                  \
        if (!sym) {                                            \
            snprintf(why, whylen, "libcurl lacks %s", name);   \
            dlclose(h);                                        \
            return 0;                                          \
        }                                                      \
        memcpy(&cu.field, &sym, sizeof sym);                   \
    } while (0)
    SYM(global_init, "curl_global_init");
    SYM(easy_init, "curl_easy_init");
    SYM(easy_setopt, "curl_easy_setopt");
    SYM(easy_perform, "curl_easy_perform");
    SYM(easy_getinfo, "curl_easy_getinfo");
    SYM(easy_strerror, "curl_easy_strerror");
    SYM(easy_cleanup, "curl_easy_cleanup");
#undef SYM
    if (cu.global_init(C_GLOBAL_DEFAULT) != 0) {
        snprintf(why, whylen, "curl_global_init failed");
        return 0;
    }
    cu.loaded = 1;
    return 1;
}

typedef struct sink {
    jfs_buf buf;
    int too_big;
} sink;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    sink *s = ud;
    size_t n = size * nmemb;
    if (s->buf.len + n < s->buf.len || s->buf.len + n > (((size_t)-1) >> 2)) {
        s->too_big = 1;
        return 0; /* aborts the transfer */
    }
    jfs_buf_append(&s->buf, ptr, n);
    return n;
}

int jfs_http_get(const char *url, char **data, size_t *len, char *err, size_t errlen)
{
    CURL *c;
    sink s;
    char ebuf[C_ERROR_SIZE], shown[1024];
    long code = 0;
    int rc, https = is_https(url), ret = 0;
    char dummy[8];
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    jfs_url_redact(url, shown, sizeof shown);
    if (!cu_mu_init) { /* first call happens before any threads exist */
        jfs_mutex_init(&cu_mu);
        cu_mu_init = 1;
    }
    jfs_mutex_lock(&cu_mu);
    if (!curl_load(err, errlen)) {
        jfs_mutex_unlock(&cu_mu);
        return -JFS_ENOSYS;
    }
    jfs_mutex_unlock(&cu_mu);
    c = cu.easy_init();
    if (!c) {
        snprintf(err, errlen, "curl_easy_init failed");
        return -JFS_ENOMEM;
    }
    memset(&s, 0, sizeof s);
    jfs_buf_init(&s.buf);
    ebuf[0] = 0;
    cu.easy_setopt(c, C_OPT_URL, url);
    cu.easy_setopt(c, C_OPT_ERRORBUFFER, ebuf);
    cu.easy_setopt(c, C_OPT_WRITEFUNCTION, write_cb);
    cu.easy_setopt(c, C_OPT_WRITEDATA, &s);
    cu.easy_setopt(c, C_OPT_USERAGENT, JFS_HTTP_USER_AGENT);
    cu.easy_setopt(c, C_OPT_NOSIGNAL, 1L);
    cu.easy_setopt(c, C_OPT_CONNECTTIMEOUT, 30L);
    cu.easy_setopt(c, C_OPT_LOW_SPEED_LIMIT, 1L);
    cu.easy_setopt(c, C_OPT_LOW_SPEED_TIME, 60L);
    cu.easy_setopt(c, C_OPT_ACCEPT_ENCODING, ""); /* gzip/deflate/br as supported */
    /* only http/https; redirects only for https and only to https */
    if (cu.easy_setopt(c, C_OPT_PROTOCOLS_STR, "http,https") == C_E_UNKNOWN_OPTION)
        cu.easy_setopt(c, C_OPT_PROTOCOLS, C_PROTO_HTTP | C_PROTO_HTTPS);
    if (https) {
        cu.easy_setopt(c, C_OPT_FOLLOWLOCATION, 1L);
        cu.easy_setopt(c, C_OPT_MAXREDIRS, (long)JFS_HTTP_MAX_REDIRECTS);
        if (cu.easy_setopt(c, C_OPT_REDIR_PROTOCOLS_STR, "https") == C_E_UNKNOWN_OPTION)
            cu.easy_setopt(c, C_OPT_REDIR_PROTOCOLS, C_PROTO_HTTPS);
    } else {
        cu.easy_setopt(c, C_OPT_FOLLOWLOCATION, 0L);
    }
    if (g_insecure) {
        cu.easy_setopt(c, C_OPT_SSL_VERIFYPEER, 0L);
        cu.easy_setopt(c, C_OPT_SSL_VERIFYHOST, 0L);
    } else {
        cu.easy_setopt(c, C_OPT_SSL_VERIFYPEER, 1L);
        cu.easy_setopt(c, C_OPT_SSL_VERIFYHOST, 2L);
        if (g_cafile)
            cu.easy_setopt(c, C_OPT_CAINFO, g_cafile);
    }
    rc = cu.easy_perform(c);
    cu.easy_getinfo(c, C_INFO_RESPONSE_CODE, &code);
    if (rc != C_E_OK) {
        const char *detail = ebuf[0] ? ebuf : cu.easy_strerror(rc);
        switch (rc) {
        case C_E_PEER_FAILED_VERIFICATION:
            snprintf(err, errlen, "%s: TLS certificate verification failed: %s (use --insecure to ignore)", shown,
                     detail);
            ret = -JFS_EACCES;
            break;
        case C_E_SSL_CACERT_BADFILE:
            snprintf(err, errlen, "%s: cannot use the CA bundle: %s", shown, detail);
            ret = -JFS_EINVAL;
            break;
        case C_E_UNSUPPORTED_PROTOCOL:
            snprintf(err, errlen, "%s: %s (https:// URLs only follow redirects to other https:// URLs)", shown,
                     detail);
            ret = -JFS_EREMOTE;
            break;
        case C_E_TOO_MANY_REDIRECTS:
            snprintf(err, errlen, "%s: more than %d redirects", shown, JFS_HTTP_MAX_REDIRECTS);
            ret = -JFS_EREMOTE;
            break;
        case C_E_WRITE_ERROR:
            snprintf(err, errlen, "%s: %s", shown, s.too_big ? "image too large" : detail);
            ret = s.too_big ? -JFS_EFBIG : -JFS_EREMOTE;
            break;
        default:
            snprintf(err, errlen, "%s: %s", shown, detail);
            ret = -JFS_EREMOTE;
        }
    } else if (code >= 300 && code < 400) {
        char *loc = NULL;
        cu.easy_getinfo(c, C_INFO_REDIRECT_URL, &loc);
        snprintf(err, errlen, "%s: HTTP %ld redirect to %s is not followed for %s URLs; use the final URL", shown,
                 code, loc ? loc : "(unknown)", https ? "https:// -> http://" : "http://");
        ret = -JFS_EREMOTE;
    } else if (code != 200) {
        snprintf(err, errlen, "%s: HTTP status %ld", shown, code);
        ret = status_error(code);
    }
    cu.easy_cleanup(c);
    if (ret) {
        jfs_buf_free(&s.buf);
        return ret;
    }
    *data = jfs_buf_steal(&s.buf, len);
    return 0;
}

#else
/* ======================================================================== */
/* Windows: WinHTTP                                                           */
/* ======================================================================== */
#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_OPTION_REDIRECT_POLICY
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#define WINHTTP_OPTION_REDIRECT_POLICY_NEVER 0
#define WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP 1
#endif
#ifndef WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS
#define WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS 89
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 0x00000200
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

static void win_msg(DWORD e, char *buf, size_t n)
{
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: snprintf(buf, n, "host name could not be resolved"); break;
    case ERROR_WINHTTP_CANNOT_CONNECT: snprintf(buf, n, "cannot connect"); break;
    case ERROR_WINHTTP_TIMEOUT: snprintf(buf, n, "timed out"); break;
    case ERROR_WINHTTP_SECURE_FAILURE: snprintf(buf, n, "TLS handshake or certificate verification failed"); break;
    case ERROR_WINHTTP_REDIRECT_FAILED: snprintf(buf, n, "redirect failed (https:// -> http:// is refused)"); break;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: snprintf(buf, n, "unsupported URL scheme"); break;
    case ERROR_WINHTTP_INVALID_URL: snprintf(buf, n, "invalid URL"); break;
    default: snprintf(buf, n, "WinHTTP error %lu", (unsigned long)e);
    }
}

int jfs_http_get(const char *url, char **data, size_t *len, char *err, size_t errlen)
{
    wchar_t *wurl = jfs_utf8_to_wide(url);
    wchar_t host[256], user[256], pass[256];
    wchar_t *path;
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sz = sizeof status, e;
    jfs_buf buf;
    int https = is_https(url), ret = 0;
    char shown[1024], msg[200], dummy[8];
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    jfs_url_redact(url, shown, sizeof shown);
    jfs_buf_init(&buf);
    if (g_cafile) {
        snprintf(err, errlen, "--cacert is not supported on Windows; import the CA into the Windows certificate store");
        free(wurl);
        return -JFS_ENOTSUP;
    }
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUserName = user;
    uc.dwUserNameLength = 256;
    uc.lpszPassword = pass;
    uc.dwPasswordLength = 256;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(err, errlen, "%s: invalid URL", shown);
        free(wurl);
        return -JFS_EINVAL;
    }
    /* path + query (both point into wurl) */
    path = jfs_calloc(uc.dwUrlPathLength + uc.dwExtraInfoLength + 2, sizeof(wchar_t));
    if (uc.dwUrlPathLength)
        memcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(wchar_t));
    else
        path[0] = L'/';
    if (uc.dwExtraInfoLength)
        memcpy(path + wcslen(path), uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(wchar_t));

    ses = WinHttpOpen(L"jsonfs/" JFS_VERSION_WIDE, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        goto werr;
    {
        /* enable TLS 1.1/1.2(/1.3) where the OS supports it (Windows 7 needs this) */
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                       WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos)) {
            protos &= ~(DWORD)WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
        }
    }
    WinHttpSetTimeouts(ses, 30000, 30000, 60000, 60000);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con)
        goto werr;
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             https ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        goto werr;
    {
        DWORD pol = https ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD maxr = 5;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
        WinHttpSetOption(req, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxr, sizeof maxr);
    }
    if (g_insecure) {
        DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof f);
    }
    if (uc.dwUserNameLength)
        WinHttpSetCredentials(req, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, user,
                              uc.dwPasswordLength ? pass : L"", NULL);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto werr;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status >= 300 && status < 400) {
        wchar_t loc[2048];
        DWORD ls = sizeof loc;
        char *l8 = NULL;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls,
                                WINHTTP_NO_HEADER_INDEX))
            l8 = jfs_wide_to_utf8(loc);
        snprintf(err, errlen, "%s: HTTP %lu redirect to %s is not followed for %s URLs; use the final URL", shown,
                 (unsigned long)status, l8 ? l8 : "(unknown)", https ? "https:// -> http://" : "http://");
        free(l8);
        ret = -JFS_EREMOTE;
        goto out;
    }
    if (status != 200) {
        snprintf(err, errlen, "%s: HTTP status %lu", shown, (unsigned long)status);
        ret = status_error((long)status);
        goto out;
    }
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail))
            goto werr;
        if (!avail)
            break;
        if (buf.len + avail > (((size_t)-1) >> 2)) {
            snprintf(err, errlen, "%s: image too large", shown);
            ret = -JFS_EFBIG;
            goto out;
        }
        jfs_buf_reserve(&buf, avail);
        if (!WinHttpReadData(req, buf.data + buf.len, avail, &got))
            goto werr;
        buf.len += got;
        buf.data[buf.len] = 0;
    }
    goto out;
werr:
    e = GetLastError();
    win_msg(e, msg, sizeof msg);
    /* 12175 SECURE_FAILURE, 12157 SECURE_CHANNEL_ERROR, 12037/12038/12045
     * certificate date / name / unknown CA */
    if (e == 12175 || e == 12157 || e == 12037 || e == 12038 || e == 12045) {
        snprintf(err, errlen, "%s: TLS certificate verification failed (%s)%s", shown, msg,
                 g_insecure ? "" : "; use --insecure to ignore certificate errors");
        ret = -JFS_EACCES;
    } else {
        snprintf(err, errlen, "%s: %s", shown, msg);
        ret = -JFS_EREMOTE;
    }
out:
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    if (ses)
        WinHttpCloseHandle(ses);
    free(path);
    free(wurl);
    if (ret) {
        jfs_buf_free(&buf);
        return ret;
    }
    *data = jfs_buf_steal(&buf, len);
    return 0;
}
#endif
