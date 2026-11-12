/*
 * JSONfs - self-contained raw DEFLATE (RFC 1951) encoder and decoder.
 *
 * The encoder does LZ77 with hash chains + lazy matching and picks per block
 * the cheapest of stored / fixed Huffman / dynamic Huffman encodings.  The
 * output is a standard raw deflate stream (zlib: wbits = -15).  The decoder
 * is a careful, bounds-checked canonical Huffman decoder in the spirit of
 * Mark Adler's "puff".
 *
 * SPDX-License-Identifier: MIT
 */
#include "jfs_codec.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Shared tables                                                              */
/* ------------------------------------------------------------------------ */
static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t clen_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* ======================================================================== */
/* Decoder                                                                    */
/* ======================================================================== */
typedef struct inflate_state {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outlen, outpos;
} inflate_state;

typedef struct huff {
    uint16_t count[16];
    uint16_t symbol[288];
} huff;

static int getbits(inflate_state *s, int need, uint32_t *val)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen)
            return -1;
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    *val = v & ((need == 32) ? 0xffffffffU : ((1U << need) - 1));
    return 0;
}

static int decode_sym(inflate_state *s, const huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        uint32_t b;
        int count;
        if (getbits(s, 1, &b))
            return -1;
        code |= (int)b;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

/* Returns 0 for a complete code, >0 for incomplete, <0 for over-subscribed */
static int build_huff(huff *h, const uint8_t *lengths, int n)
{
    int len, sym, left;
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (sym = 0; sym < n; sym++)
        h->count[lengths[sym]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (lengths[sym])
            h->symbol[offs[lengths[sym]]++] = (uint16_t)sym;
    return left;
}

static int inflate_codes(inflate_state *s, const huff *lencode, const huff *distcode)
{
    for (;;) {
        int sym = decode_sym(s, lencode);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen)
                return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            uint32_t e;
            size_t len, dist, k;
            sym -= 257;
            if (sym >= 29)
                return -1;
            if (getbits(s, len_extra[sym], &e))
                return -1;
            len = len_base[sym] + e;
            sym = decode_sym(s, distcode);
            if (sym < 0 || sym >= 30)
                return -1;
            if (getbits(s, dist_extra[sym], &e))
                return -1;
            dist = dist_base[sym] + e;
            if (dist > s->outpos || s->outpos + len > s->outlen)
                return -1;
            for (k = 0; k < len; k++) {
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

static int inflate_stored(inflate_state *s)
{
    size_t len;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen)
        return -1;
    len = (size_t)s->in[s->inpos] | ((size_t)s->in[s->inpos + 1] << 8);
    if ((s->in[s->inpos + 2] != (uint8_t)~s->in[s->inpos]) || (s->in[s->inpos + 3] != (uint8_t)~s->in[s->inpos + 1]))
        return -1;
    s->inpos += 4;
    if (s->inpos + len > s->inlen || s->outpos + len > s->outlen)
        return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len;
    s->outpos += len;
    return 0;
}

static int inflate_fixed(inflate_state *s)
{
    static huff lencode, distcode;
    static int built;
    if (!built) {
        uint8_t l[288];
        int i;
        for (i = 0; i < 144; i++)
            l[i] = 8;
        for (; i < 256; i++)
            l[i] = 9;
        for (; i < 280; i++)
            l[i] = 7;
        for (; i < 288; i++)
            l[i] = 8;
        build_huff(&lencode, l, 288);
        for (i = 0; i < 30; i++)
            l[i] = 5;
        build_huff(&distcode, l, 30);
        built = 1;
    }
    return inflate_codes(s, &lencode, &distcode);
}

static int inflate_dynamic(inflate_state *s)
{
    uint8_t lengths[320];
    uint32_t nlen, ndist, ncode, v;
    unsigned idx;
    huff lencode, distcode;
    int err;
    if (getbits(s, 5, &nlen) || getbits(s, 5, &ndist) || getbits(s, 4, &ncode))
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30)
        return -1;
    memset(lengths, 0, sizeof lengths);
    for (idx = 0; idx < ncode; idx++) {
        if (getbits(s, 3, &v))
            return -1;
        lengths[clen_order[idx]] = (uint8_t)v;
    }
    if (build_huff(&lencode, lengths, 19) != 0)
        return -1;
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode_sym(s, &lencode);
        uint32_t rep;
        uint8_t val = 0;
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (idx == 0)
                return -1;
            val = lengths[idx - 1];
            if (getbits(s, 2, &rep))
                return -1;
            rep += 3;
        } else if (sym == 17) {
            if (getbits(s, 3, &rep))
                return -1;
            rep += 3;
        } else {
            if (getbits(s, 7, &rep))
                return -1;
            rep += 11;
        }
        if (idx + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[idx++] = val;
    }
    if (lengths[256] == 0)
        return -1;
    err = build_huff(&lencode, lengths, (int)nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return -1;
    err = build_huff(&distcode, lengths + nlen, (int)ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return -1;
    return inflate_codes(s, &lencode, &distcode);
}

int jfs_deflate_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t expected)
{
    inflate_state s;
    uint32_t last, type;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = n;
    s.out = out;
    s.outlen = expected;
    do {
        int rc;
        if (getbits(&s, 1, &last) || getbits(&s, 2, &type))
            return -JFS_ECORRUPT;
        if (type == 0)
            rc = inflate_stored(&s);
        else if (type == 1)
            rc = inflate_fixed(&s);
        else if (type == 2)
            rc = inflate_dynamic(&s);
        else
            rc = -1;
        if (rc)
            return -JFS_ECORRUPT;
    } while (!last);
    return s.outpos == expected ? 0 : -JFS_ECORRUPT;
}

/* ======================================================================== */
/* Encoder                                                                    */
/* ======================================================================== */
typedef struct bitwriter {
    jfs_buf *b;
    uint64_t acc;
    int n;
} bitwriter;

static void putbits(bitwriter *w, uint32_t v, int nbits)
{
    w->acc |= (uint64_t)v << w->n;
    w->n += nbits;
    while (w->n >= 8) {
        jfs_buf_putc(w->b, (char)(uint8_t)w->acc);
        w->acc >>= 8;
        w->n -= 8;
    }
}

static void flush_bits(bitwriter *w)
{
    if (w->n > 0)
        jfs_buf_putc(w->b, (char)(uint8_t)w->acc);
    w->acc = 0;
    w->n = 0;
}

static uint32_t revbits(uint32_t code, int len)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* Build length-limited Huffman code lengths. */
typedef struct hnode {
    uint32_t freq;
    int left, right; /* -1 for leaves */
    int sym;
} hnode;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void huff_lengths(const uint32_t *freq_in, int n, int maxlen, uint8_t *lens)
{
    uint32_t freq[288];
    int i;
    memcpy(freq, freq_in, (size_t)n * sizeof(uint32_t));
    for (;;) {
        hnode nodes[2 * 288];
        uint64_t leaves[288];
        int nl = 0, nn, qi = 0, qn, qend, depth_ok = 1;
        int nq[288]; /* internal node queue */
        int q2s = 0, q2e = 0;
        memset(lens, 0, (size_t)n);
        for (i = 0; i < n; i++)
            if (freq[i])
                leaves[nl++] = ((uint64_t)freq[i] << 16) | (uint64_t)i;
        if (nl == 0)
            return;
        if (nl == 1) {
            lens[leaves[0] & 0xffff] = 1;
            return;
        }
        qsort(leaves, (size_t)nl, sizeof leaves[0], cmp_u64);
        for (i = 0; i < nl; i++) {
            nodes[i].freq = (uint32_t)(leaves[i] >> 16);
            nodes[i].sym = (int)(leaves[i] & 0xffff);
            nodes[i].left = nodes[i].right = -1;
        }
        nn = nl;
        qn = nl;
        qend = nl;
        (void)qn;
        /* two-queue Huffman construction */
        while ((qend - qi) + (q2e - q2s) > 1) {
            int pick[2], k;
            for (k = 0; k < 2; k++) {
                if (qi < qend && (q2s >= q2e || nodes[qi].freq <= nodes[nq[q2s]].freq))
                    pick[k] = qi++;
                else
                    pick[k] = nq[q2s++];
            }
            nodes[nn].freq = nodes[pick[0]].freq + nodes[pick[1]].freq;
            nodes[nn].left = pick[0];
            nodes[nn].right = pick[1];
            nodes[nn].sym = -1;
            nq[q2e++] = nn;
            nn++;
        }
        /* compute depths iteratively (root is last node) */
        {
            int stack[2 * 288], dstack[2 * 288], sp = 0;
            stack[sp] = nn - 1;
            dstack[sp++] = 0;
            while (sp) {
                int node, d;
                sp--;
                node = stack[sp];
                d = dstack[sp];
                if (nodes[node].left < 0) {
                    if (d > maxlen)
                        depth_ok = 0;
                    lens[nodes[node].sym] = (uint8_t)(d > 255 ? 255 : d);
                } else {
                    stack[sp] = nodes[node].left;
                    dstack[sp++] = d + 1;
                    stack[sp] = nodes[node].right;
                    dstack[sp++] = d + 1;
                }
            }
        }
        if (depth_ok)
            return;
        /* flatten the distribution and retry */
        for (i = 0; i < n; i++)
            if (freq[i])
                freq[i] = (freq[i] >> 1) | 1;
    }
}

static void canon_codes(const uint8_t *lens, int n, uint16_t *codes)
{
    uint16_t bl_count[16] = {0}, next[16];
    int i, code = 0;
    for (i = 0; i < n; i++)
        bl_count[lens[i]]++;
    bl_count[0] = 0;
    for (i = 1; i < 16; i++) {
        code = (code + bl_count[i - 1]) << 1;
        next[i] = (uint16_t)code;
    }
    for (i = 0; i < n; i++)
        if (lens[i])
            codes[i] = (uint16_t)revbits(next[lens[i]]++, lens[i]);
}

/* LZ77 symbol: lit (dist==0) or match */
typedef struct lzsym {
    uint16_t litlen; /* literal byte, or match length */
    uint16_t dist;   /* 0 for literals */
} lzsym;

static int len_code(int len)
{
    int i;
    for (i = 28; i >= 0; i--)
        if (len >= len_base[i])
            return i;
    return 0;
}

static int dist_code(int dist)
{
    int i;
    for (i = 29; i >= 0; i--)
        if (dist >= dist_base[i])
            return i;
    return 0;
}

#define WSIZE 32768
#define WMASK (WSIZE - 1)
#define HBITS 15
#define HSIZE (1 << HBITS)
#define MAX_MATCH 258
#define MIN_MATCH 3
#define BLOCK_SYMS 32768

static uint32_t hash3(const uint8_t *p)
{
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) & (HSIZE - 1);
}

static void write_block(bitwriter *w, const lzsym *syms, size_t nsyms, const uint8_t *raw, size_t rawlen,
                        int last)
{
    uint32_t lfreq[286] = {0}, dfreq[30] = {0};
    uint8_t llens[286], dlens[30], fl[288], fd[30];
    uint16_t lcodes[286], dcodes[30], fcodes[288], fdcodes[30];
    size_t i;
    uint64_t cost_dyn, cost_fix, cost_stored;
    int nlen, ndist;
    /* header encoding */
    uint8_t all[316];
    int nall, rle[316 * 2], nrle = 0;
    uint32_t cfreq[19] = {0};
    uint8_t clens[19];
    uint16_t ccodes[19];
    int ncl;

    for (i = 0; i < nsyms; i++) {
        if (syms[i].dist == 0) {
            lfreq[syms[i].litlen]++;
        } else {
            lfreq[257 + len_code(syms[i].litlen)]++;
            dfreq[dist_code(syms[i].dist)]++;
        }
    }
    lfreq[256] = 1;
    huff_lengths(lfreq, 286, 15, llens);
    huff_lengths(dfreq, 30, 15, dlens);
    /* at least one distance code must be present for some decoders */
    {
        int any = 0;
        for (i = 0; i < 30; i++)
            any |= dlens[i];
        if (!any)
            dlens[0] = 1;
    }
    for (nlen = 286; nlen > 257 && !llens[nlen - 1]; nlen--)
        ;
    for (ndist = 30; ndist > 1 && !dlens[ndist - 1]; ndist--)
        ;
    memcpy(all, llens, (size_t)nlen);
    memcpy(all + nlen, dlens, (size_t)ndist);
    nall = nlen + ndist;
    /* run-length encode code lengths */
    for (i = 0; i < (size_t)nall;) {
        uint8_t v = all[i];
        size_t run = 1;
        while (i + run < (size_t)nall && all[i + run] == v)
            run++;
        if (v == 0 && run >= 3) {
            size_t r = run;
            while (r >= 3) {
                size_t take = r > 138 ? 138 : r;
                if (take >= 11) {
                    rle[nrle++] = 18;
                    rle[nrle++] = (int)(take - 11);
                    cfreq[18]++;
                } else {
                    rle[nrle++] = 17;
                    rle[nrle++] = (int)(take - 3);
                    cfreq[17]++;
                }
                r -= take;
            }
            while (r--) {
                rle[nrle++] = 0;
                rle[nrle++] = 0;
                cfreq[0]++;
            }
        } else if (v != 0 && run >= 4) {
            size_t r = run - 1;
            rle[nrle++] = v;
            rle[nrle++] = 0;
            cfreq[v]++;
            while (r >= 3) {
                size_t take = r > 6 ? 6 : r;
                rle[nrle++] = 16;
                rle[nrle++] = (int)(take - 3);
                cfreq[16]++;
                r -= take;
            }
            while (r--) {
                rle[nrle++] = v;
                rle[nrle++] = 0;
                cfreq[v]++;
            }
        } else {
            size_t r = run;
            while (r--) {
                rle[nrle++] = v;
                rle[nrle++] = 0;
                cfreq[v]++;
            }
        }
        i += run;
    }
    huff_lengths(cfreq, 19, 7, clens);
    for (ncl = 19; ncl > 4 && !clens[clen_order[ncl - 1]]; ncl--)
        ;

    /* costs in bits */
    cost_dyn = 3 + 5 + 5 + 4 + 3 * (uint64_t)ncl;
    for (i = 0; i < (size_t)nrle; i += 2) {
        int s = rle[i];
        cost_dyn += clens[s] + (s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0);
    }
    cost_fix = 3;
    for (i = 0; i < 286; i++) {
        int fixlen = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        uint64_t extra = i >= 257 ? len_extra[i - 257] : 0;
        cost_dyn += (uint64_t)lfreq[i] * (llens[i] + extra);
        cost_fix += (uint64_t)lfreq[i] * (fixlen + extra);
    }
    for (i = 0; i < 30; i++) {
        cost_dyn += (uint64_t)dfreq[i] * (dlens[i] + dist_extra[i]);
        cost_fix += (uint64_t)dfreq[i] * (5 + dist_extra[i]);
    }
    cost_stored = ((uint64_t)rawlen + 5 * ((rawlen + 65534) / 65535 + 1)) * 8 + 8;

    if (cost_stored <= cost_dyn && cost_stored <= cost_fix) {
        size_t off = 0;
        do {
            size_t take = rawlen - off > 65535 ? 65535 : rawlen - off;
            int fin = last && off + take == rawlen;
            putbits(w, (uint32_t)fin, 1);
            putbits(w, 0, 2);
            flush_bits(w);
            jfs_buf_putc(w->b, (char)(uint8_t)(take & 0xff));
            jfs_buf_putc(w->b, (char)(uint8_t)(take >> 8));
            jfs_buf_putc(w->b, (char)(uint8_t)(~take & 0xff));
            jfs_buf_putc(w->b, (char)(uint8_t)((~take >> 8) & 0xff));
            jfs_buf_append(w->b, raw + off, take);
            off += take;
        } while (off < rawlen);
        return;
    }

    if (cost_fix <= cost_dyn) {
        for (i = 0; i < 288; i++)
            fl[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        for (i = 0; i < 30; i++)
            fd[i] = 5;
        canon_codes(fl, 288, fcodes);
        canon_codes(fd, 30, fdcodes);
        putbits(w, (uint32_t)last, 1);
        putbits(w, 1, 2);
        memcpy(llens, fl, 286);
        memcpy(lcodes, fcodes, sizeof lcodes);
        memcpy(dlens, fd, 30);
        memcpy(dcodes, fdcodes, sizeof dcodes);
    } else {
        canon_codes(llens, 286, lcodes);
        canon_codes(dlens, 30, dcodes);
        canon_codes(clens, 19, ccodes);
        putbits(w, (uint32_t)last, 1);
        putbits(w, 2, 2);
        putbits(w, (uint32_t)(nlen - 257), 5);
        putbits(w, (uint32_t)(ndist - 1), 5);
        putbits(w, (uint32_t)(ncl - 4), 4);
        for (i = 0; i < (size_t)ncl; i++)
            putbits(w, clens[clen_order[i]], 3);
        for (i = 0; i < (size_t)nrle; i += 2) {
            int s = rle[i];
            putbits(w, ccodes[s], clens[s]);
            if (s == 16)
                putbits(w, (uint32_t)rle[i + 1], 2);
            else if (s == 17)
                putbits(w, (uint32_t)rle[i + 1], 3);
            else if (s == 18)
                putbits(w, (uint32_t)rle[i + 1], 7);
        }
    }
    for (i = 0; i < nsyms; i++) {
        if (syms[i].dist == 0) {
            putbits(w, lcodes[syms[i].litlen], llens[syms[i].litlen]);
        } else {
            int lc = len_code(syms[i].litlen), dc = dist_code(syms[i].dist);
            putbits(w, lcodes[257 + lc], llens[257 + lc]);
            if (len_extra[lc])
                putbits(w, (uint32_t)(syms[i].litlen - len_base[lc]), len_extra[lc]);
            putbits(w, dcodes[dc], dlens[dc]);
            if (dist_extra[dc])
                putbits(w, (uint32_t)(syms[i].dist - dist_base[dc]), dist_extra[dc]);
        }
    }
    putbits(w, lcodes[256], llens[256]);
}

int jfs_deflate_compress(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *outlen)
{
    jfs_buf b;
    bitwriter w;
    int32_t *head, *prev;
    lzsym *syms;
    size_t nsyms = 0, pos = 0, block_start = 0;
    int max_chain, lazy;
    if (level < 0)
        level = 6;
    if (level == 0) {
        max_chain = 0;
    } else {
        static const int chains[10] = {0, 4, 8, 16, 32, 64, 128, 256, 1024, 4096};
        max_chain = chains[level];
    }
    lazy = level >= 4;
    jfs_buf_init(&b);
    jfs_buf_reserve(&b, n / 2 + 64);
    w.b = &b;
    w.acc = 0;
    w.n = 0;
    if (n == 0) {
        /* single empty fixed block */
        putbits(&w, 1, 1);
        putbits(&w, 1, 2);
        putbits(&w, 0, 7); /* end of block (code 0000000) */
        flush_bits(&w);
        *outlen = b.len;
        *out = (uint8_t *)jfs_buf_steal(&b, NULL);
        return 0;
    }
    head = jfs_malloc(HSIZE * sizeof(int32_t));
    prev = jfs_malloc(WSIZE * sizeof(int32_t));
    for (pos = 0; pos < HSIZE; pos++)
        head[pos] = -1;
    syms = jfs_malloc((BLOCK_SYMS + 2) * sizeof(lzsym));
    pos = 0;

#define INSERT(p)                                     \
    do {                                              \
        if ((p) + MIN_MATCH <= n) {                   \
            uint32_t h_ = hash3(in + (p));            \
            prev[(p) & WMASK] = head[h_];             \
            head[h_] = (int32_t)(p);                  \
        }                                             \
    } while (0)

    while (pos < n) {
        int best_len = 0, best_dist = 0;
        if (max_chain && pos + MIN_MATCH <= n) {
            /* find longest match at pos */
            int32_t cand = head[hash3(in + pos)];
            int chain = max_chain;
            size_t maxl = n - pos > MAX_MATCH ? MAX_MATCH : n - pos;
            while (cand >= 0 && chain-- > 0 && pos - (size_t)cand <= WSIZE - 262) {
                const uint8_t *a = in + cand, *c = in + pos;
                if (a[best_len] == c[best_len] && a[0] == c[0]) {
                    size_t l = 0;
                    while (l < maxl && a[l] == c[l])
                        l++;
                    if ((int)l > best_len) {
                        best_len = (int)l;
                        best_dist = (int)(pos - (size_t)cand);
                        if (l == maxl)
                            break;
                    }
                }
                {
                    int32_t nx = prev[cand & WMASK];
                    if (nx >= cand)
                        break;
                    cand = nx;
                }
            }
            if (best_len < MIN_MATCH)
                best_len = 0;
            /* lazy evaluation: is the match at pos+1 better? */
            if (lazy && best_len && best_len < 32 && pos + 1 + MIN_MATCH <= n) {
                int32_t c2 = head[hash3(in + pos + 1)];
                int chain2 = max_chain / 2;
                size_t maxl2 = n - pos - 1 > MAX_MATCH ? MAX_MATCH : n - pos - 1;
                int better = 0;
                while (c2 >= 0 && chain2-- > 0 && pos + 1 - (size_t)c2 <= WSIZE - 262) {
                    const uint8_t *a = in + c2, *c = in + pos + 1;
                    if ((size_t)best_len < maxl2 && a[best_len] == c[best_len]) {
                        size_t l = 0;
                        while (l < maxl2 && a[l] == c[l])
                            l++;
                        if ((int)l > best_len) {
                            better = 1;
                            break;
                        }
                    }
                    {
                        int32_t nx = prev[c2 & WMASK];
                        if (nx >= c2)
                            break;
                        c2 = nx;
                    }
                }
                if (better)
                    best_len = 0; /* emit literal now, match next round */
            }
        }
        if (best_len) {
            size_t k;
            syms[nsyms].litlen = (uint16_t)best_len;
            syms[nsyms].dist = (uint16_t)best_dist;
            nsyms++;
            for (k = 0; k < (size_t)best_len; k++)
                INSERT(pos + k);
            pos += (size_t)best_len;
        } else {
            syms[nsyms].litlen = in[pos];
            syms[nsyms].dist = 0;
            nsyms++;
            INSERT(pos);
            pos++;
        }
        if (nsyms >= BLOCK_SYMS) {
            write_block(&w, syms, nsyms, in + block_start, pos - block_start, pos == n);
            nsyms = 0;
            block_start = pos;
        }
    }
    if (nsyms || block_start < n)
        write_block(&w, syms, nsyms, in + block_start, pos - block_start, 1);
#undef INSERT
    flush_bits(&w);
    free(head);
    free(prev);
    free(syms);
    *outlen = b.len;
    *out = (uint8_t *)jfs_buf_steal(&b, NULL);
    return 0;
}
