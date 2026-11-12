/*
 * JSONfs - minimal JSON DOM (RFC 8259) used for the filesystem image.
 * SPDX-License-Identifier: MIT
 */
#ifndef JFS_JSON_H
#define JFS_JSON_H

#include "jfs_util.h"

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jv_type;

typedef struct jv jv;
struct jv {
    jv_type t;
    int b;          /* JV_BOOL */
    double d;       /* JV_NUM */
    int64_t i;      /* JV_NUM (exact when is_int) */
    int is_int;     /* JV_NUM */
    char *s;        /* JV_STR (NUL terminated, may contain embedded NULs) */
    size_t slen;    /* JV_STR */
    jv **items;     /* JV_ARR / JV_OBJ values */
    char **keys;    /* JV_OBJ keys */
    size_t n, cap;  /* JV_ARR / JV_OBJ */
};

/* Parsing.  On error returns NULL and fills err (if given) with a message. */
jv *jv_parse(const char *text, size_t len, char *err, size_t errlen);
void jv_free(jv *v);
jv *jv_clone(const jv *v);

/* Construction. */
jv *jv_null(void);
jv *jv_bool(int b);
jv *jv_int(int64_t i);
jv *jv_num(double d);
jv *jv_str(const char *s);
jv *jv_strn(const char *s, size_t n);
jv *jv_str_owned(char *s, size_t n); /* takes ownership */
jv *jv_arr(void);
jv *jv_obj(void);
void jv_arr_push(jv *a, jv *item);
/* Sets key (replacing an existing value). Takes ownership of val. */
void jv_obj_set(jv *o, const char *key, jv *val);
void jv_obj_del(jv *o, const char *key);

/* Access (NULL-safe). */
jv *jv_get(const jv *o, const char *key);
const char *jv_get_str(const jv *o, const char *key);
int jv_get_i64(const jv *o, const char *key, int64_t *out);
int jv_get_bool(const jv *o, const char *key, int dflt);
/* Detach the string payload of o[key] (avoids copying multi-MB blobs). */
char *jv_take_str(jv *o, const char *key, size_t *len);

/* Serialization.  indent < 0: compact. */
void jv_write(jfs_buf *out, const jv *v, int indent);
/* Same, starting at nesting `level` (for embedding into a larger document). */
void jv_write_level(jfs_buf *out, const jv *v, int indent, int level);
void jv_write_string(jfs_buf *out, const char *s, size_t len);

#endif
