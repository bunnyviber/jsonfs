/*
 * JSONfs - base64, SHA-256, and the compressor registry (none/rle/lz4;
 * DEFLATE lives in jfs_deflate.c).
 * SPDX-License-Identifier: MIT
 */
#include "jfs_codec.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* base64                                                                     */
/* ======================================================================== */
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t jfs_b64_enc_len(size_t n)
{
    return (n + 2) / 3 * 4;
}

size_t jfs_b64_encode(const void *in, size_t n, char *out)
{
    const uint8_t *p = in;
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = b64chars[(v >> 6) & 63];
        out[o++] = b64chars[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = b64chars[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

int jfs_b64_decode(const char *in, size_t n, uint8_t **out, size_t *outlen)
{
    static signed char tbl[256];
    static int init;
    uint8_t *buf;
    size_t o = 0, i;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    if (!init) {
        int k;
        memset(tbl, -1, sizeof tbl);
        for (k = 0; k < 64; k++)
            tbl[(unsigned char)b64chars[k]] = (signed char)k;
        tbl['-'] = 62; /* tolerate URL-safe alphabet */
        tbl['_'] = 63;
        init = 1;
    }
    buf = jfs_malloc(n / 4 * 3 + 3);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad || tbl[c] < 0) {
            free(buf);
            return -JFS_ECORRUPT;
        }
        acc = (acc << 6) | (uint32_t)tbl[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (pad > 2 || bits >= 6) {
        free(buf);
        return -JFS_ECORRUPT;
    }
    *out = buf;
    *outlen = o;
    return 0;
}

/* ======================================================================== */
/* SHA-256                                                                    */
/* ======================================================================== */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(jfs_sha256 *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

void jfs_sha256_init(jfs_sha256 *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->blen = 0;
}

void jfs_sha256_update(jfs_sha256 *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->total += n;
    if (c->blen) {
        size_t take = 64 - c->blen;
        if (take > n)
            take = n;
        memcpy(c->buf + c->blen, p, take);
        c->blen += take;
        p += take;
        n -= take;
        if (c->blen == 64) {
            sha256_block(c, c->buf);
            c->blen = 0;
        }
    }
    while (n >= 64) {
        sha256_block(c, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(c->buf, p, n);
        c->blen = n;
    }
}

void jfs_sha256_final(jfs_sha256 *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    int i;
    jfs_sha256_update(c, &pad, 1);
    while (c->blen != 56)
        jfs_sha256_update(c, &zero, 1);
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    jfs_sha256_update(c, len, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void jfs_sha256_buf(const void *data, size_t n, uint8_t out[32])
{
    jfs_sha256 c;
    jfs_sha256_init(&c);
    jfs_sha256_update(&c, data, n);
    jfs_sha256_final(&c, out);
}

void jfs_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexdig(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int jfs_unhex(const char *in, uint8_t *out, size_t n)
{
    size_t i;
    if (!in || strlen(in) != 2 * n)
        return -1;
    for (i = 0; i < n; i++) {
        int a = hexdig(in[2 * i]), b = hexdig(in[2 * i + 1]);
        if (a < 0 || b < 0)
            return -1;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 0;
}

/* ======================================================================== */
/* none                                                                       */
/* ======================================================================== */
static int none_compress(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *outlen)
{
    (void)level;
    *out = jfs_malloc(n);
    if (n)
        memcpy(*out, in, n);
    *outlen = n;
    return 0;
}

static int none_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t expected)
{
    if (n != expected)
        return -JFS_ECORRUPT;
    if (n)
        memcpy(out, in, n);
    return 0;
}

/* ======================================================================== */
/* rle (PackBits)                                                             */
/* ======================================================================== */
static int rle_compress(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *outlen)
{
    jfs_buf b;
    size_t i = 0;
    (void)level;
    jfs_buf_init(&b);
    jfs_buf_reserve(&b, n / 64 + 16);
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && in[i + run] == in[i])
            run++;
        if (run >= 3) {
            jfs_buf_putc(&b, (char)(uint8_t)(257 - run));
            jfs_buf_putc(&b, (char)in[i]);
            i += run;
        } else {
            size_t lit = 0;
            /* gather literals until a run of >=3 starts */
            while (i + lit < n && lit < 128) {
                if (i + lit + 2 < n && in[i + lit] == in[i + lit + 1] && in[i + lit] == in[i + lit + 2])
                    break;
                lit++;
            }
            jfs_buf_putc(&b, (char)(uint8_t)(lit - 1));
            jfs_buf_append(&b, in + i, lit);
            i += lit;
        }
    }
    *outlen = b.len;
    *out = (uint8_t *)jfs_buf_steal(&b, NULL);
    return 0;
}

static int rle_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t expected)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t h = in[i++];
        if (h < 128) {
            size_t lit = (size_t)h + 1;
            if (i + lit > n || o + lit > expected)
                return -JFS_ECORRUPT;
            memcpy(out + o, in + i, lit);
            i += lit;
            o += lit;
        } else if (h > 128) {
            size_t run = 257 - (size_t)h;
            if (i >= n || o + run > expected)
                return -JFS_ECORRUPT;
            memset(out + o, in[i++], run);
            o += run;
        }
    }
    return o == expected ? 0 : -JFS_ECORRUPT;
}

/* ======================================================================== */
/* LZ4 block format                                                           */
/* ======================================================================== */
#define LZ4_HASH_LOG 14
#define LZ4_MINMATCH 4
#define LZ4_LASTLITERALS 5
#define LZ4_MFLIMIT 12
#define LZ4_MAX_DIST 65535

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t lz4_hash(uint32_t v)
{
    return (v * 2654435761U) >> (32 - LZ4_HASH_LOG);
}

static void lz4_putlen(jfs_buf *b, size_t len)
{
    while (len >= 255) {
        jfs_buf_putc(b, (char)255);
        len -= 255;
    }
    jfs_buf_putc(b, (char)(uint8_t)len);
}

static void lz4_emit(jfs_buf *b, const uint8_t *lit, size_t litlen, size_t off, size_t mlen, int last)
{
    uint8_t token = (uint8_t)((litlen >= 15 ? 15 : litlen) << 4);
    if (!last)
        token |= (uint8_t)((mlen - LZ4_MINMATCH) >= 15 ? 15 : (mlen - LZ4_MINMATCH));
    jfs_buf_putc(b, (char)token);
    if (litlen >= 15)
        lz4_putlen(b, litlen - 15);
    jfs_buf_append(b, lit, litlen);
    if (last)
        return;
    jfs_buf_putc(b, (char)(uint8_t)(off & 0xff));
    jfs_buf_putc(b, (char)(uint8_t)(off >> 8));
    if (mlen - LZ4_MINMATCH >= 15)
        lz4_putlen(b, mlen - LZ4_MINMATCH - 15);
}

static int lz4_compress(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *outlen)
{
    jfs_buf b;
    uint32_t *table;
    size_t anchor = 0, i = 0;
    (void)level;
    jfs_buf_init(&b);
    jfs_buf_reserve(&b, n + n / 255 + 16);
    if (n < LZ4_MFLIMIT + 1) {
        lz4_emit(&b, in, n, 0, 0, 1);
        goto done;
    }
    table = jfs_calloc((size_t)1 << LZ4_HASH_LOG, sizeof(uint32_t));
    /* table stores position+1 (0 = empty) */
    while (i + LZ4_MFLIMIT < n) {
        uint32_t seq = rd32(in + i);
        uint32_t h = lz4_hash(seq);
        size_t cand = table[h];
        table[h] = (uint32_t)(i + 1);
        if (cand && i - (cand - 1) <= LZ4_MAX_DIST && rd32(in + cand - 1) == seq) {
            size_t ref = cand - 1;
            size_t mlen = LZ4_MINMATCH;
            size_t limit = n - LZ4_LASTLITERALS;
            while (i + mlen < limit && in[ref + mlen] == in[i + mlen])
                mlen++;
            /* extend backwards into pending literals */
            while (i > anchor && ref > 0 && in[i - 1] == in[ref - 1]) {
                i--;
                ref--;
                mlen++;
            }
            lz4_emit(&b, in + anchor, i - anchor, i - ref, mlen, 0);
            i += mlen;
            anchor = i;
            if (i >= 2 && i - 2 + 4 <= n)
                table[lz4_hash(rd32(in + i - 2))] = (uint32_t)(i - 2 + 1);
        } else {
            i++;
        }
    }
    free(table);
    lz4_emit(&b, in + anchor, n - anchor, 0, 0, 1);
done:
    *outlen = b.len;
    *out = (uint8_t *)jfs_buf_steal(&b, NULL);
    return 0;
}

static int lz4_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t expected)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t token = in[i++];
        size_t lit = token >> 4, mlen, off;
        if (lit == 15) {
            uint8_t s;
            do {
                if (i >= n)
                    return -JFS_ECORRUPT;
                s = in[i++];
                lit += s;
            } while (s == 255);
        }
        if (i + lit > n || o + lit > expected)
            return -JFS_ECORRUPT;
        memcpy(out + o, in + i, lit);
        i += lit;
        o += lit;
        if (i == n)
            break; /* last sequence */
        if (i + 2 > n)
            return -JFS_ECORRUPT;
        off = (size_t)in[i] | ((size_t)in[i + 1] << 8);
        i += 2;
        if (off == 0 || off > o)
            return -JFS_ECORRUPT;
        mlen = (token & 15);
        if (mlen == 15) {
            uint8_t s;
            do {
                if (i >= n)
                    return -JFS_ECORRUPT;
                s = in[i++];
                mlen += s;
            } while (s == 255);
        }
        mlen += LZ4_MINMATCH;
        if (o + mlen > expected)
            return -JFS_ECORRUPT;
        {
            size_t k;
            const uint8_t *src = out + o - off;
            for (k = 0; k < mlen; k++)
                out[o + k] = src[k];
        }
        o += mlen;
    }
    return o == expected ? 0 : -JFS_ECORRUPT;
}

/* ======================================================================== */
/* Registry                                                                   */
/* ======================================================================== */
int jfs_deflate_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t expected);

static const jfs_compressor compressors[] = {
    {"deflate", "raw DEFLATE (RFC 1951), good ratio, default", jfs_deflate_compress, jfs_deflate_decompress},
    {"lz4", "LZ4 block format, very fast", lz4_compress, lz4_decompress},
    {"rle", "PackBits run-length encoding, for sparse/repetitive data", rle_compress, rle_decompress},
    {"none", "no compression", none_compress, none_decompress},
};

const jfs_compressor *jfs_compressor_list(size_t *count)
{
    *count = sizeof compressors / sizeof compressors[0];
    return compressors;
}

const jfs_compressor *jfs_compressor_find(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof compressors / sizeof compressors[0]; i++)
        if (!strcmp(compressors[i].name, name))
            return &compressors[i];
    return NULL;
}

int jfs_compression_parse(const char *spec, const jfs_compressor **c, int *level)
{
    char name[32];
    const char *colon;
    size_t n;
    int lv = -1;
    if (!spec || !*spec)
        return -JFS_EINVAL;
    colon = strchr(spec, ':');
    n = colon ? (size_t)(colon - spec) : strlen(spec);
    if (n >= sizeof name)
        return -JFS_EINVAL;
    memcpy(name, spec, n);
    name[n] = 0;
    if (colon) {
        char *e;
        long v = strtol(colon + 1, &e, 10);
        if (*e || v < 0 || v > 9)
            return -JFS_EINVAL;
        lv = (int)v;
    }
    if (!jfs_compressor_find(name))
        return -JFS_EINVAL;
    if (c)
        *c = jfs_compressor_find(name);
    if (level)
        *level = lv;
    return 0;
}

int jfs_encode_blob(const char *algo_spec, const uint8_t *data, size_t n, char **b64, size_t *b64len,
                    char **algo_name_out)
{
    const jfs_compressor *c;
    int level, rc;
    uint8_t *comp;
    size_t clen;
    rc = jfs_compression_parse(algo_spec ? algo_spec : JFS_DEFAULT_COMPRESSION, &c, &level);
    if (rc)
        return rc;
    rc = c->compress(data, n, level, &comp, &clen);
    if (rc)
        return rc;
    *b64 = jfs_malloc(jfs_b64_enc_len(clen) + 1);
    *b64len = jfs_b64_encode(comp, clen, *b64);
    free(comp);
    if (algo_name_out)
        *algo_name_out = jfs_strdup(c->name);
    return 0;
}

int jfs_decode_blob(const char *algo, const char *b64, size_t b64len, uint64_t expected_size,
                    const uint8_t *expected_sha, uint8_t **out)
{
    const jfs_compressor *c = jfs_compressor_find(algo);
    uint8_t *raw, *buf;
    size_t rawlen;
    int rc;
    if (!c)
        return -JFS_ENOTSUP;
    if (expected_size > (uint64_t)((size_t)-1) / 2)
        return -JFS_EFBIG;
    rc = jfs_b64_decode(b64 ? b64 : "", b64 ? b64len : 0, &raw, &rawlen);
    if (rc)
        return rc;
    buf = jfs_malloc((size_t)expected_size + 1);
    rc = c->decompress(raw, rawlen, buf, (size_t)expected_size);
    free(raw);
    if (rc) {
        free(buf);
        return rc;
    }
    if (expected_sha) {
        uint8_t got[32];
        jfs_sha256_buf(buf, (size_t)expected_size, got);
        if (memcmp(got, expected_sha, 32)) {
            free(buf);
            return -JFS_ECHECKSUM;
        }
    }
    *out = buf;
    return 0;
}

/* ======================================================================== */
/* CRC-32 and gzip                                                            */
/* ======================================================================== */
uint32_t jfs_crc32(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    static int init;
    const uint8_t *p = data;
    size_t i;
    if (!init) {
        uint32_t k, c;
        int b;
        for (k = 0; k < 256; k++) {
            c = k;
            for (b = 0; b < 8; b++)
                c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            table[k] = c;
        }
        init = 1;
    }
    crc = ~crc;
    for (i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int jfs_gzip_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *outlen)
{
    jfs_buf all;
    size_t pos = 0;
    jfs_buf_init(&all);
    while (pos + 18 <= n && in[pos] == 0x1f && in[pos + 1] == 0x8b) {
        size_t p = pos + 10, used = 0, dlen = 0;
        uint8_t flg = in[pos + 3], *data = NULL;
        int rc;
        if (in[pos + 2] != 8)
            goto bad; /* only DEFLATE is defined */
        if (flg & 4) { /* FEXTRA */
            if (p + 2 > n)
                goto bad;
            p += 2 + ((size_t)in[p] | ((size_t)in[p + 1] << 8));
        }
        if (flg & 8) /* FNAME */
            while (p < n && in[p++]) {
            }
        if (flg & 16) /* FCOMMENT */
            while (p < n && in[p++]) {
            }
        if (flg & 2) /* FHCRC */
            p += 2;
        if (p >= n)
            goto bad;
        rc = jfs_inflate_alloc(in + p, n - p, &data, &dlen, &used);
        if (rc)
            goto bad;
        p += used;
        if (p + 8 > n || rd32le(in + p) != jfs_crc32(0, data, dlen) || rd32le(in + p + 4) != (uint32_t)dlen) {
            free(data);
            goto bad;
        }
        jfs_buf_append(&all, data, dlen);
        free(data);
        pos = p + 8;
        while (pos < n && in[pos] == 0) /* padding some tools append */
            pos++;
    }
    if (pos == 0)
        goto bad;
    *outlen = all.len;
    *out = (uint8_t *)jfs_buf_steal(&all, NULL);
    return 0;
bad:
    jfs_buf_free(&all);
    return -JFS_ECORRUPT;
}

int jfs_gzip_encode(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *outlen)
{
    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 /* Unix */};
    uint8_t *c, tr[8];
    size_t cl;
    uint32_t crc = jfs_crc32(0, in, n);
    jfs_buf b;
    int rc = jfs_deflate_compress(in, n, level, &c, &cl);
    if (rc)
        return rc;
    jfs_buf_init(&b);
    jfs_buf_append(&b, hdr, 10);
    jfs_buf_append(&b, c, cl);
    free(c);
    tr[0] = (uint8_t)crc;
    tr[1] = (uint8_t)(crc >> 8);
    tr[2] = (uint8_t)(crc >> 16);
    tr[3] = (uint8_t)(crc >> 24);
    tr[4] = (uint8_t)n;
    tr[5] = (uint8_t)(n >> 8);
    tr[6] = (uint8_t)(n >> 16);
    tr[7] = (uint8_t)(n >> 24);
    jfs_buf_append(&b, tr, 8);
    *outlen = b.len;
    *out = (uint8_t *)jfs_buf_steal(&b, NULL);
    return 0;
}
