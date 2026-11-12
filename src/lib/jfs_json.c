/*
 * JSONfs - minimal JSON DOM.
 * SPDX-License-Identifier: MIT
 */
#include "jfs_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JV_MAX_DEPTH 512

typedef struct parser {
    const char *p, *end;
    char *err;
    size_t errlen;
    int depth;
} parser;

static void perr(parser *ps, const char *msg)
{
    if (ps->err && !ps->err[0])
        snprintf(ps->err, ps->errlen, "%s", msg);
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static jv *jv_new(jv_type t)
{
    jv *v = jfs_calloc(1, sizeof *v);
    v->t = t;
    return v;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int read_hex4(parser *ps, unsigned *out)
{
    unsigned v = 0;
    int i;
    if (ps->end - ps->p < 4)
        return -1;
    for (i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0)
            return -1;
        v = v * 16 + (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void put_utf8(jfs_buf *b, unsigned cp)
{
    char t[4];
    if (cp < 0x80) {
        jfs_buf_putc(b, (char)cp);
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        jfs_buf_append(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        jfs_buf_append(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        jfs_buf_append(b, t, 4);
    }
}

/* Parses a string starting after the opening quote. */
static char *parse_string_raw(parser *ps, size_t *outlen)
{
    const char *start = ps->p;
    jfs_buf b;
    /* fast path: no escapes */
    while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\' && (unsigned char)*ps->p >= 0x20)
        ps->p++;
    if (ps->p < ps->end && *ps->p == '"') {
        size_t n = (size_t)(ps->p - start);
        ps->p++;
        *outlen = n;
        return jfs_strndup(start, n);
    }
    jfs_buf_init(&b);
    jfs_buf_append(&b, start, (size_t)(ps->p - start));
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            return jfs_buf_steal(&b, outlen);
        } else if (c == '\\') {
            if (ps->p >= ps->end)
                break;
            c = *ps->p++;
            switch (c) {
            case '"': jfs_buf_putc(&b, '"'); break;
            case '\\': jfs_buf_putc(&b, '\\'); break;
            case '/': jfs_buf_putc(&b, '/'); break;
            case 'b': jfs_buf_putc(&b, '\b'); break;
            case 'f': jfs_buf_putc(&b, '\f'); break;
            case 'n': jfs_buf_putc(&b, '\n'); break;
            case 'r': jfs_buf_putc(&b, '\r'); break;
            case 't': jfs_buf_putc(&b, '\t'); break;
            case 'u': {
                unsigned cp, lo;
                if (read_hex4(ps, &cp)) {
                    perr(ps, "bad \\u escape");
                    goto fail;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        if (read_hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            perr(ps, "bad surrogate pair");
                            goto fail;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                put_utf8(&b, cp);
                break;
            }
            default: perr(ps, "bad escape"); goto fail;
            }
        } else if ((unsigned char)c < 0x20) {
            perr(ps, "control character in string");
            goto fail;
        } else {
            const char *run = ps->p - 1;
            while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\' && (unsigned char)*ps->p >= 0x20)
                ps->p++;
            jfs_buf_append(&b, run, (size_t)(ps->p - run));
        }
    }
    perr(ps, "unterminated string");
fail:
    jfs_buf_free(&b);
    return NULL;
}

static jv *parse_value(parser *ps);

static jv *parse_number(parser *ps)
{
    const char *s = ps->p;
    char tmp[64];
    size_t n;
    jv *v;
    int is_int = 1;
    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    if (ps->p >= ps->end || !(*ps->p >= '0' && *ps->p <= '9')) {
        perr(ps, "bad number");
        return NULL;
    }
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
        ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
        is_int = 0;
        ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        is_int = 0;
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    n = (size_t)(ps->p - s);
    if (n >= sizeof tmp) {
        perr(ps, "number too long");
        return NULL;
    }
    memcpy(tmp, s, n);
    tmp[n] = 0;
    v = jv_new(JV_NUM);
    v->d = strtod(tmp, NULL);
    if (is_int && n < 19) {
        v->i = strtoll(tmp, NULL, 10);
        v->is_int = 1;
    } else if (is_int) {
        /* up to 2^63-1 exactly */
        char *e;
        long long ll = strtoll(tmp, &e, 10);
        v->i = ll;
        v->is_int = 1;
    } else if (v->d == floor(v->d) && fabs(v->d) < 9e15) {
        v->i = (int64_t)v->d;
        v->is_int = 1;
    }
    return v;
}

static jv *parse_value(parser *ps)
{
    jv *v;
    skip_ws(ps);
    if (ps->p >= ps->end) {
        perr(ps, "unexpected end of input");
        return NULL;
    }
    if (++ps->depth > JV_MAX_DEPTH) {
        perr(ps, "nesting too deep");
        return NULL;
    }
    switch (*ps->p) {
    case '{':
        ps->p++;
        v = jv_obj();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
        }
        for (;;) {
            char *key;
            size_t klen;
            jv *val;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                perr(ps, "expected object key");
                goto fail;
            }
            ps->p++;
            key = parse_string_raw(ps, &klen);
            if (!key)
                goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                perr(ps, "expected ':'");
                goto fail;
            }
            ps->p++;
            val = parse_value(ps);
            if (!val) {
                free(key);
                goto fail;
            }
            /* append without duplicate check (fast); last one wins on lookup? keep first semantics simple */
            if (v->n == v->cap) {
                v->cap = v->cap ? v->cap * 2 : 8;
                v->items = jfs_realloc(v->items, v->cap * sizeof(jv *));
                v->keys = jfs_realloc(v->keys, v->cap * sizeof(char *));
            }
            v->keys[v->n] = key;
            v->items[v->n++] = val;
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == '}') {
                ps->p++;
                break;
            }
            perr(ps, "expected ',' or '}'");
            goto fail;
        }
        break;
    case '[':
        ps->p++;
        v = jv_arr();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            break;
        }
        for (;;) {
            jv *item = parse_value(ps);
            if (!item)
                goto fail;
            jv_arr_push(v, item);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == ']') {
                ps->p++;
                break;
            }
            perr(ps, "expected ',' or ']'");
            goto fail;
        }
        break;
    case '"': {
        size_t n;
        char *s;
        ps->p++;
        s = parse_string_raw(ps, &n);
        if (!s) {
            ps->depth--;
            return NULL;
        }
        v = jv_str_owned(s, n);
        break;
    }
    case 't':
        if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
            ps->p += 4;
            v = jv_bool(1);
            break;
        }
        perr(ps, "bad literal");
        ps->depth--;
        return NULL;
    case 'f':
        if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
            ps->p += 5;
            v = jv_bool(0);
            break;
        }
        perr(ps, "bad literal");
        ps->depth--;
        return NULL;
    case 'n':
        if (ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
            ps->p += 4;
            v = jv_null();
            break;
        }
        perr(ps, "bad literal");
        ps->depth--;
        return NULL;
    default:
        v = parse_number(ps);
        if (!v) {
            ps->depth--;
            return NULL;
        }
    }
    ps->depth--;
    return v;
fail:
    ps->depth--;
    jv_free(v);
    return NULL;
}

jv *jv_parse(const char *text, size_t len, char *err, size_t errlen)
{
    parser ps;
    jv *v;
    ps.p = text;
    ps.end = text + len;
    ps.err = err;
    ps.errlen = errlen;
    ps.depth = 0;
    if (err && errlen)
        err[0] = 0;
    /* skip UTF-8 BOM */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    v = parse_value(&ps);
    if (!v) {
        if (err && errlen) {
            size_t off = (size_t)(ps.p - text);
            size_t l = strlen(err);
            snprintf(err + l, errlen - l, " at byte %lu", (unsigned long)off);
        }
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        perr(&ps, "trailing garbage after JSON value");
        jv_free(v);
        return NULL;
    }
    return v;
}

void jv_free(jv *v)
{
    size_t i;
    if (!v)
        return;
    if (v->t == JV_STR)
        free(v->s);
    if (v->t == JV_ARR || v->t == JV_OBJ) {
        for (i = 0; i < v->n; i++) {
            jv_free(v->items[i]);
            if (v->keys)
                free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
    free(v);
}

jv *jv_clone(const jv *v)
{
    jv *c;
    size_t i;
    if (!v)
        return NULL;
    switch (v->t) {
    case JV_NULL: return jv_null();
    case JV_BOOL: return jv_bool(v->b);
    case JV_NUM:
        c = jv_new(JV_NUM);
        c->d = v->d;
        c->i = v->i;
        c->is_int = v->is_int;
        return c;
    case JV_STR: return jv_strn(v->s, v->slen);
    case JV_ARR:
        c = jv_arr();
        for (i = 0; i < v->n; i++)
            jv_arr_push(c, jv_clone(v->items[i]));
        return c;
    case JV_OBJ:
        c = jv_obj();
        for (i = 0; i < v->n; i++)
            jv_obj_set(c, v->keys[i], jv_clone(v->items[i]));
        return c;
    }
    return NULL;
}

jv *jv_null(void) { return jv_new(JV_NULL); }

jv *jv_bool(int b)
{
    jv *v = jv_new(JV_BOOL);
    v->b = !!b;
    return v;
}

jv *jv_int(int64_t i)
{
    jv *v = jv_new(JV_NUM);
    v->i = i;
    v->d = (double)i;
    v->is_int = 1;
    return v;
}

jv *jv_num(double d)
{
    jv *v = jv_new(JV_NUM);
    v->d = d;
    return v;
}

jv *jv_str(const char *s) { return jv_strn(s, strlen(s)); }

jv *jv_strn(const char *s, size_t n) { return jv_str_owned(jfs_strndup(s, n), n); }

jv *jv_str_owned(char *s, size_t n)
{
    jv *v = jv_new(JV_STR);
    v->s = s;
    v->slen = n;
    return v;
}

jv *jv_arr(void) { return jv_new(JV_ARR); }
jv *jv_obj(void) { return jv_new(JV_OBJ); }

void jv_arr_push(jv *a, jv *item)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->items = jfs_realloc(a->items, a->cap * sizeof(jv *));
    }
    a->items[a->n++] = item;
}

void jv_obj_set(jv *o, const char *key, jv *val)
{
    size_t i;
    for (i = 0; i < o->n; i++) {
        if (!strcmp(o->keys[i], key)) {
            jv_free(o->items[i]);
            o->items[i] = val;
            return;
        }
    }
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 8;
        o->items = jfs_realloc(o->items, o->cap * sizeof(jv *));
        o->keys = jfs_realloc(o->keys, o->cap * sizeof(char *));
    }
    o->keys[o->n] = jfs_strdup(key);
    o->items[o->n++] = val;
}

void jv_obj_del(jv *o, const char *key)
{
    size_t i;
    if (!o || o->t != JV_OBJ)
        return;
    for (i = 0; i < o->n; i++) {
        if (!strcmp(o->keys[i], key)) {
            jv_free(o->items[i]);
            free(o->keys[i]);
            memmove(o->items + i, o->items + i + 1, (o->n - i - 1) * sizeof(jv *));
            memmove(o->keys + i, o->keys + i + 1, (o->n - i - 1) * sizeof(char *));
            o->n--;
            return;
        }
    }
}

jv *jv_get(const jv *o, const char *key)
{
    size_t i;
    if (!o || o->t != JV_OBJ)
        return NULL;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->keys[i], key))
            return o->items[i];
    return NULL;
}

const char *jv_get_str(const jv *o, const char *key)
{
    jv *v = jv_get(o, key);
    return v && v->t == JV_STR ? v->s : NULL;
}

int jv_get_i64(const jv *o, const char *key, int64_t *out)
{
    jv *v = jv_get(o, key);
    if (!v || v->t != JV_NUM || !v->is_int)
        return -1;
    *out = v->i;
    return 0;
}

int jv_get_bool(const jv *o, const char *key, int dflt)
{
    jv *v = jv_get(o, key);
    if (!v || v->t != JV_BOOL)
        return dflt;
    return v->b;
}

char *jv_take_str(jv *o, const char *key, size_t *len)
{
    jv *v = jv_get(o, key);
    char *s;
    if (!v || v->t != JV_STR)
        return NULL;
    s = v->s;
    if (len)
        *len = v->slen;
    v->s = jfs_strdup("");
    v->slen = 0;
    return s;
}

/* ------------------------------------------------------------------------ */
void jv_write_string(jfs_buf *out, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, run = 0;
    jfs_buf_reserve(out, len + 2);
    jfs_buf_putc(out, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[7];
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            ubuf[0] = '\\';
            ubuf[1] = 'u';
            ubuf[2] = '0';
            ubuf[3] = '0';
            ubuf[4] = hex[c >> 4];
            ubuf[5] = hex[c & 15];
            ubuf[6] = 0;
            esc = ubuf;
        }
        jfs_buf_append(out, s + run, i - run);
        jfs_buf_puts(out, esc);
        run = i + 1;
    }
    jfs_buf_append(out, s + run, len - run);
    jfs_buf_putc(out, '"');
}

static void newline(jfs_buf *out, int indent, int level)
{
    int i;
    if (indent < 0)
        return;
    jfs_buf_putc(out, '\n');
    for (i = 0; i < indent * level; i++)
        jfs_buf_putc(out, ' ');
}

void jv_write_level(jfs_buf *out, const jv *v, int indent, int level)
{
    size_t i;
    switch (v->t) {
    case JV_NULL: jfs_buf_puts(out, "null"); break;
    case JV_BOOL: jfs_buf_puts(out, v->b ? "true" : "false"); break;
    case JV_NUM:
        if (v->is_int)
            jfs_buf_printf(out, "%lld", (long long)v->i);
        else if (isfinite(v->d))
            jfs_buf_printf(out, "%.17g", v->d);
        else
            jfs_buf_puts(out, "null");
        break;
    case JV_STR: jv_write_string(out, v->s, v->slen); break;
    case JV_ARR:
        if (!v->n) {
            jfs_buf_puts(out, "[]");
            break;
        }
        jfs_buf_putc(out, '[');
        for (i = 0; i < v->n; i++) {
            if (i)
                jfs_buf_putc(out, ',');
            newline(out, indent, level + 1);
            jv_write_level(out, v->items[i], indent, level + 1);
        }
        newline(out, indent, level);
        jfs_buf_putc(out, ']');
        break;
    case JV_OBJ:
        if (!v->n) {
            jfs_buf_puts(out, "{}");
            break;
        }
        jfs_buf_putc(out, '{');
        for (i = 0; i < v->n; i++) {
            if (i)
                jfs_buf_putc(out, ',');
            newline(out, indent, level + 1);
            jv_write_string(out, v->keys[i], strlen(v->keys[i]));
            jfs_buf_puts(out, indent >= 0 ? ": " : ":");
            jv_write_level(out, v->items[i], indent, level + 1);
        }
        newline(out, indent, level);
        jfs_buf_putc(out, '}');
        break;
    }
}

void jv_write(jfs_buf *out, const jv *v, int indent)
{
    jv_write_level(out, v, indent, 0);
}
