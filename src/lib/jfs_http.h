/*
 * JSONfs - fetching images over HTTP(S).
 *
 * POSIX: libcurl, loaded at runtime with dlopen() (libcurl.so.4 /
 *        libcurl.4.dylib), so no SDK is needed to build or cross-compile.
 * Windows: WinHTTP (part of Windows since XP SP1).
 *
 * Policy:
 *   - TLS certificates (chain + host name) are verified unless insecure mode
 *     is enabled with jfs_http_configure().
 *   - http:// URLs never follow redirects; a 3xx answer is an error that
 *     names the redirect target.
 *   - https:// URLs follow up to 5 redirects, but only to other https:// URLs
 *     (no downgrade to plain HTTP).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_HTTP_H
#define JFS_HTTP_H

#include "jfs_util.h"

/* Non-zero if `path` starts with http:// or https:// (case-insensitive). */
int jfs_is_url(const char *path);

/* Global settings for every following request.
 * insecure: skip certificate verification (https only).
 * cafile:   extra PEM bundle of trusted CAs (POSIX only; NULL = system store). */
void jfs_http_configure(int insecure, const char *cafile);
int jfs_http_insecure(void);

/* Downloads `url` into a NUL-terminated malloc'ed buffer. On failure returns a
 * negative JFS error and a human readable message in err. */
int jfs_http_get(const char *url, char **data, size_t *len, char *err, size_t errlen);

/* Extended request: conditional GET, byte ranges, size limit, validators. */
typedef struct jfs_http_req {
    const char *if_none_match;     /* ETag from a previous response, or NULL */
    const char *if_modified_since; /* Last-Modified from a previous response, or NULL */
    uint64_t range_end;            /* >0: ask for bytes 0..range_end (Range header) */
    size_t max_bytes;              /* >0: stop reading after this many bytes (not an error) */
} jfs_http_req;

typedef struct jfs_http_resp {
    long status;          /* final HTTP status */
    int not_modified;     /* 304: *data is empty, the previous copy is current */
    int truncated;        /* max_bytes reached */
    char etag[256];       /* validators of the response ("" if absent) */
    char last_modified[128];
} jfs_http_resp;

int jfs_http_fetch(const char *url, const jfs_http_req *req, jfs_http_resp *resp, char **data, size_t *len,
                   char *err, size_t errlen);

/* Copies `url` into out with any user:password@ part replaced by "***@". */
void jfs_url_redact(const char *url, char *out, size_t outlen);

/* Reads a local file or downloads a URL. */
int jfs_load_bytes(const char *path_or_url, char **data, size_t *len, char *err, size_t errlen);

#endif
