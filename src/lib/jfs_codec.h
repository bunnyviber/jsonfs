/*
 * JSONfs - base64, SHA-256 and compression codecs.
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_CODEC_H
#define JFS_CODEC_H

#include "jfs_util.h"

/* ---------------- base64 (RFC 4648, standard alphabet, padded) ---------- */
size_t jfs_b64_enc_len(size_t n);
/* out must hold jfs_b64_enc_len(n)+1 bytes; returns length written. */
size_t jfs_b64_encode(const void *in, size_t n, char *out);
/* Returns 0 on success; *out is malloc'ed. Whitespace is ignored. */
int jfs_b64_decode(const char *in, size_t n, uint8_t **out, size_t *outlen);

/* ---------------- SHA-256 (FIPS 180-4) ---------------------------------- */
typedef struct jfs_sha256 {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t blen;
} jfs_sha256;
void jfs_sha256_init(jfs_sha256 *c);
void jfs_sha256_update(jfs_sha256 *c, const void *data, size_t n);
void jfs_sha256_final(jfs_sha256 *c, uint8_t out[32]);
void jfs_sha256_buf(const void *data, size_t n, uint8_t out[32]);
void jfs_hex(const uint8_t *in, size_t n, char *out); /* out: 2n+1 */
int jfs_unhex(const char *in, uint8_t *out, size_t n);

/* ---------------- Compression ------------------------------------------- */
/*
 * Algorithms are identified by name in the image ("compression" field of
 * every file record).  Decompression always uses the algorithm named by the
 * record; the filesystem default / mount option only affects how *new* data
 * is written.
 *
 *   none     stored as-is
 *   deflate  raw DEFLATE stream (RFC 1951), zlib-compatible (wbits=-15)
 *   lz4      LZ4 block format (lz4 frame-less "block" API)
 *   rle      PackBits style run-length encoding (cheap, for sparse files)
 */
#define JFS_DEFAULT_COMPRESSION "deflate"

typedef struct jfs_compressor {
    const char *name;
    const char *description;
    /* Returns 0 and a malloc'ed buffer, or a negative JFS error. */
    int (*compress)(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *outlen);
    /* Decompresses exactly `expected` bytes into out (pre-allocated). */
    int (*decompress)(const uint8_t *in, size_t n, uint8_t *out, size_t expected);
} jfs_compressor;

const jfs_compressor *jfs_compressor_find(const char *name);
const jfs_compressor *jfs_compressor_list(size_t *count);
/* Validate an algorithm specification of the form "name" or "name:level". */
int jfs_compression_parse(const char *spec, const jfs_compressor **c, int *level);

/* Convenience: compress + base64. */
int jfs_encode_blob(const char *algo_spec, const uint8_t *data, size_t n, char **b64, size_t *b64len,
                    char **algo_name_out);
/* base64 decode + decompress + (optional) sha256 verification. */
int jfs_decode_blob(const char *algo, const char *b64, size_t b64len, uint64_t expected_size,
                    const uint8_t *expected_sha /* may be NULL */, uint8_t **out);

#endif
