/*
 * JSONfs unit tests: codecs, JSON, time handling, POSIX semantics,
 * versioning, maintenance operations and multi-attach reloads.
 *
 * Usage: jfs-unit-tests [scratch-dir]
 * SPDX-License-Identifier: MIT
 */
#include "jfs_codec.h"
#include "jfs_fs.h"
#include "jfs_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static char scratch[1024] = ".";

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)
#define CHECK_RC(expr, want)                                                                         \
    do {                                                                                             \
        long long got_ = (long long)(expr);                                                          \
        checks++;                                                                                    \
        if (got_ != (long long)(want)) {                                                             \
            failures++;                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s = %lld (%s), want %lld\n", __FILE__, __LINE__, #expr,     \
                    got_, got_ < 0 ? jfs_strerror((int)got_) : "", (long long)(want));               \
        }                                                                                            \
    } while (0)

static char *tmp_path(const char *name)
{
    static char buf[8][1200];
    static int i;
    i = (i + 1) % 8;
    snprintf(buf[i], sizeof buf[i], "%s/%s", scratch, name);
    return buf[i];
}

static void rm_image(const char *p)
{
    char lock[1300];
    jfs_remove_file(p);
    snprintf(lock, sizeof lock, "%s.lock", p);
    jfs_remove_file(lock);
}

/* ------------------------------------------------------------------------ */
static void test_sha256(void)
{
    uint8_t h[32];
    char hex[65];
    jfs_sha256_buf("abc", 3, h);
    jfs_hex(h, 32, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    jfs_sha256_buf("", 0, h);
    jfs_hex(h, 32, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        jfs_sha256_buf(m, strlen(m), h);
        jfs_hex(h, 32, hex);
        CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }
}

static void test_base64(void)
{
    const char *v[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *e[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    int i;
    for (i = 0; i < 7; i++) {
        char out[32];
        uint8_t *d;
        size_t n;
        jfs_b64_encode(v[i], strlen(v[i]), out);
        CHECK(!strcmp(out, e[i]));
        CHECK(jfs_b64_decode(e[i], strlen(e[i]), &d, &n) == 0 && n == strlen(v[i]) && !memcmp(d, v[i], n));
        free(d);
    }
    {
        uint8_t *d;
        size_t n;
        CHECK(jfs_b64_decode("Zm9v!", 5, &d, &n) != 0);
    }
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_compressors(void)
{
    size_t sizes[] = {0, 1, 2, 3, 12, 13, 100, 4096, 65535, 65536, 70000, 300000};
    size_t ncomp, c, s, kind;
    const jfs_compressor *cs = jfs_compressor_list(&ncomp);
    for (kind = 0; kind < 3; kind++) {
        for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            size_t n = sizes[s], i;
            uint8_t *in = malloc(n + 1);
            for (i = 0; i < n; i++)
                in[i] = kind == 0 ? (uint8_t)rnd() : kind == 1 ? (uint8_t)(i % 7) : (uint8_t)("hello world "[i % 12] ^ (rnd() % 50 == 0));
            for (c = 0; c < ncomp; c++) {
                int lv;
                for (lv = -1; lv <= 9; lv += (cs[c].name[0] == 'd' ? 5 : 20)) {
                    uint8_t *out, *back = malloc(n + 1);
                    size_t olen;
                    int ok = cs[c].compress(in, n, lv, &out, &olen) == 0 &&
                             cs[c].decompress(out, olen, back, n) == 0 && !memcmp(in, back, n);
                    if (!ok)
                        fprintf(stderr, "  %s level %d kind %lu size %lu\n", cs[c].name, lv, (unsigned long)kind,
                                (unsigned long)n);
                    CHECK(ok);
                    /* truncated / wrong-size input must be rejected, never crash */
                    if (olen > 1 && n > 0) {
                        int rc = cs[c].decompress(out, olen - 1, back, n);
                        (void)rc;
                        CHECK(cs[c].decompress(out, olen, back, n - 1) != 0 || !strcmp(cs[c].name, "none") == 0);
                    }
                    free(out);
                    free(back);
                }
            }
            free(in);
        }
    }
    CHECK(jfs_compression_parse("deflate:9", NULL, NULL) == 0);
    CHECK(jfs_compression_parse("deflate:10", NULL, NULL) != 0);
    CHECK(jfs_compression_parse("zstd", NULL, NULL) != 0);
}

static void test_json(void)
{
    const char *src = "{\"a\": [1, -2, 3.5, true, false, null, \"x\\n\\u00e9\\ud83d\\ude00\"], \"b\": {}, \"big\": "
                      "9007199254740993}";
    char err[100];
    jv *v = jv_parse(src, strlen(src), err, sizeof err);
    jfs_buf b;
    int64_t i;
    CHECK(v != NULL);
    CHECK(jv_get(v, "a")->n == 7);
    CHECK(!strcmp(jv_get(v, "a")->items[6]->s, "x\n\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(jv_get_i64(v, "big", &i) == 0 && i == 9007199254740993LL);
    jfs_buf_init(&b);
    jv_write(&b, v, -1);
    CHECK(!strcmp(b.data, "{\"a\":[1,-2,3.5,true,false,null,\"x\\n\xc3\xa9\xf0\x9f\x98\x80\"],\"b\":{},\"big\":"
                          "9007199254740993}"));
    jfs_buf_free(&b);
    jv_free(v);
    CHECK(jv_parse("{\"a\":1,}", 8, err, sizeof err) == NULL);
    CHECK(jv_parse("[1 2]", 5, err, sizeof err) == NULL);
    CHECK(jv_parse("\"\\x\"", 4, err, sizeof err) == NULL);
    CHECK(jv_parse("{} x", 4, err, sizeof err) == NULL);
}

static void test_time(void)
{
    jfs_time t, u;
    char buf[JFS_TIME_STRLEN];
    CHECK(jfs_time_parse("2026-11-11T12:34:56.123456789Z", &t) == 0);
    jfs_time_format(t, buf);
    CHECK(!strcmp(buf, "2026-11-11T12:34:56.123456789Z"));
    CHECK(jfs_time_parse("1970-01-01T00:00:00Z", &t) == 0 && t.sec == 0 && t.nsec == 0);
    CHECK(jfs_time_parse("2000-02-29T00:00:00+01:00", &t) == 0 && t.sec == 951778800);
    CHECK(jfs_time_parse("1969-12-31T23:59:59.5Z", &t) == 0 && t.sec == -1 && t.nsec == 500000000);
    jfs_time_format(t, buf);
    CHECK(!strcmp(buf, "1969-12-31T23:59:59.500000000Z"));
    CHECK(jfs_time_parse_user("2026-11-11", &u) == 0);
    CHECK(jfs_time_parse("2026-11-11T00:00:00Z", &t) == 0 && t.sec == u.sec);
    CHECK(jfs_time_parse_user("@1700000000", &u) == 0 && u.sec == 1700000000);
    CHECK(jfs_time_parse_user("-1h", &u) == 0 && jfs_now().sec - u.sec >= 3599 && jfs_now().sec - u.sec <= 3601);
    CHECK(jfs_time_parse_user("yesterday", &u) != 0);
    CHECK(jfs_time_parse("2026-13-01T00:00:00Z", &t) != 0);
}

/* ------------------------------------------------------------------------ */
static jfs_fs *open_img(const char *p, int lockmode, int commit_ms)
{
    jfs_open_opts o;
    jfs_fs *fs = NULL;
    char err[300];
    int rc;
    jfs_open_opts_init(&o);
    o.lockmode = lockmode;
    o.commit_interval_ms = commit_ms;
    rc = jfs_open(p, &o, &fs, err, sizeof err);
    if (rc)
        fprintf(stderr, "open %s: %s\n", p, err);
    return fs;
}

static int write_file(jfs_fs *fs, const char *path, const char *data)
{
    jfs_handle *h;
    jfs_ctx c = {0, 0, 022};
    int rc = jfs_create(fs, path, 0644, JFS_O_WRONLY | JFS_O_TRUNC, &c, &h);
    if (rc)
        return rc;
    if (jfs_write(fs, h, data, strlen(data), 0) != (int64_t)strlen(data))
        rc = -JFS_EIO;
    jfs_release(fs, h);
    return rc;
}

static char *read_file(jfs_fs *fs, const char *path)
{
    static char buf[65536];
    jfs_handle *h;
    int64_t n;
    if (jfs_open_file(fs, path, JFS_O_RDONLY, &h))
        return NULL;
    n = jfs_read(fs, h, buf, sizeof buf - 1, 0);
    jfs_release(fs, h);
    if (n < 0)
        return NULL;
    buf[n] = 0;
    return buf;
}

static void test_posix(void)
{
    const char *img = tmp_path("posix.json");
    jfs_mkfs_opts mo;
    jfs_fs *fs;
    jfs_ctx c = {1000, 1000, 022};
    jfs_stat st;
    jfs_handle *h, *h2;
    char buf[64];
    rm_image(img);
    memset(&mo, 0, sizeof mo);
    CHECK_RC(jfs_mkfs(img, &mo, 0), 0);
    CHECK_RC(jfs_mkfs(img, &mo, 0), -JFS_EEXIST);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0);
    CHECK(fs != NULL);
    if (!fs)
        return;
    /* second exclusive attach must fail (same process: use OFFLINE semantics via file signature is not possible,
     * fcntl locks are per process, so this is covered by the shell tests) */
    CHECK_RC(jfs_mkdir(fs, "/d", 0777, &c), 0);
    CHECK_RC(jfs_getattr(fs, "/d", &st), 0);
    CHECK((st.mode & 07777) == 0755 && (st.mode & JFS_S_IFMT) == JFS_S_IFDIR && st.uid == 1000 && st.nlink == 2);
    CHECK_RC(jfs_getattr(fs, "/", &st), 0);
    CHECK(st.nlink == 3);
    CHECK_RC(jfs_mkdir(fs, "/d", 0777, &c), -JFS_EEXIST);
    CHECK_RC(jfs_mkdir(fs, "/nope/x", 0777, &c), -JFS_ENOENT);
    CHECK_RC(write_file(fs, "/d/f", "hello"), 0);
    CHECK_RC(jfs_mkdir(fs, "/d/f/x", 0777, &c), -JFS_ENOTDIR);
    CHECK(!strcmp(read_file(fs, "/d/f"), "hello"));
    /* sparse write extends with zeros */
    CHECK_RC(jfs_open_file(fs, "/d/f", JFS_O_RDWR, &h), 0);
    CHECK_RC(jfs_write(fs, h, "X", 1, 10), 1);
    CHECK_RC(jfs_fgetattr(fs, h, &st), 0);
    CHECK(st.size == 11);
    CHECK_RC(jfs_read(fs, h, buf, sizeof buf, 0), 11);
    CHECK(!memcmp(buf, "hello\0\0\0\0\0X", 11));
    CHECK_RC(jfs_read(fs, h, buf, sizeof buf, 100), 0);
    CHECK_RC(jfs_ftruncate(fs, h, 2), 0);
    CHECK_RC(jfs_read(fs, h, buf, sizeof buf, 0), 2);
    jfs_release(fs, h);
    /* append */
    CHECK_RC(jfs_open_file(fs, "/d/f", JFS_O_WRONLY | JFS_O_APPEND, &h), 0);
    CHECK_RC(jfs_write(fs, h, "yy", 2, 0), 2);
    jfs_release(fs, h);
    CHECK(!strcmp(read_file(fs, "/d/f"), "heyy"));
    /* read-only handle cannot write */
    CHECK_RC(jfs_open_file(fs, "/d/f", JFS_O_RDONLY, &h), 0);
    CHECK_RC(jfs_write(fs, h, "z", 1, 0), -JFS_EBADF);
    jfs_release(fs, h);
    /* O_EXCL */
    CHECK_RC(jfs_create(fs, "/d/f", 0644, JFS_O_WRONLY | JFS_O_EXCL, &c, &h), -JFS_EEXIST);
    /* hard links */
    CHECK_RC(jfs_link(fs, "/d/f", "/g"), 0);
    CHECK_RC(jfs_getattr(fs, "/g", &st), 0);
    CHECK(st.nlink == 2);
    CHECK_RC(jfs_link(fs, "/d", "/dl"), -JFS_EPERM);
    /* symlinks */
    CHECK_RC(jfs_symlink(fs, "d/f", "/s", &c), 0);
    CHECK_RC(jfs_readlink(fs, "/s", buf, sizeof buf), 0);
    CHECK(!strcmp(buf, "d/f"));
    CHECK_RC(jfs_getattr(fs, "/s", &st), 0);
    CHECK((st.mode & JFS_S_IFMT) == JFS_S_IFLNK && (st.mode & 0777) == 0777);
    CHECK_RC(jfs_readlink(fs, "/g", buf, sizeof buf), -JFS_EINVAL);
    /* special files */
    CHECK_RC(jfs_mknod(fs, "/fifo", JFS_S_IFIFO | 0600, 0, &c), 0);
    CHECK_RC(jfs_mknod(fs, "/chr", JFS_S_IFCHR | 0600, 0x0103, &c), 0);
    CHECK_RC(jfs_getattr(fs, "/chr", &st), 0);
    CHECK(st.rdev == 0x0103 && (st.mode & JFS_S_IFMT) == JFS_S_IFCHR);
    /* rmdir / unlink rules */
    CHECK_RC(jfs_rmdir(fs, "/d"), -JFS_ENOTEMPTY);
    CHECK_RC(jfs_unlink(fs, "/d"), -JFS_EISDIR);
    CHECK_RC(jfs_rmdir(fs, "/g"), -JFS_ENOTDIR);
    CHECK_RC(jfs_rmdir(fs, "/"), -JFS_EBUSY);
    /* unlink while open keeps data reachable through the handle */
    CHECK_RC(jfs_open_file(fs, "/g", JFS_O_RDWR, &h), 0);
    CHECK_RC(jfs_unlink(fs, "/g"), 0);
    CHECK_RC(jfs_unlink(fs, "/d/f"), 0);
    CHECK_RC(jfs_getattr(fs, "/d/f", &st), -JFS_ENOENT);
    CHECK_RC(jfs_read(fs, h, buf, sizeof buf, 0), 4);
    CHECK_RC(jfs_write(fs, h, "Z", 1, 0), 1);
    CHECK_RC(jfs_fgetattr(fs, h, &st), 0);
    CHECK(st.nlink == 0);
    jfs_release(fs, h);
    /* rename semantics */
    CHECK_RC(write_file(fs, "/a", "A"), 0);
    CHECK_RC(write_file(fs, "/b", "B"), 0);
    CHECK_RC(jfs_rename(fs, "/a", "/b", JFS_RENAME_NOREPLACE), -JFS_EEXIST);
    CHECK_RC(jfs_rename(fs, "/a", "/b", JFS_RENAME_EXCHANGE), 0);
    CHECK(!strcmp(read_file(fs, "/a"), "B"));
    CHECK_RC(jfs_rename(fs, "/a", "/b", 0), 0);
    CHECK(!strcmp(read_file(fs, "/b"), "B"));
    CHECK_RC(jfs_getattr(fs, "/a", &st), -JFS_ENOENT);
    CHECK_RC(jfs_mkdir(fs, "/d/sub", 0755, &c), 0);
    CHECK_RC(jfs_rename(fs, "/d", "/d/sub/x", 0), -JFS_EINVAL);
    CHECK_RC(jfs_rename(fs, "/b", "/d", 0), -JFS_EISDIR);
    CHECK_RC(jfs_rename(fs, "/d", "/b", 0), -JFS_ENOTDIR);
    CHECK_RC(jfs_mkdir(fs, "/e", 0755, &c), 0);
    CHECK_RC(jfs_rename(fs, "/e", "/d", 0), -JFS_ENOTEMPTY);
    CHECK_RC(jfs_rename(fs, "/d/sub", "/e/sub", 0), 0); /* move dir across parents */
    CHECK_RC(jfs_getattr(fs, "/d", &st), 0);
    CHECK(st.nlink == 2);
    CHECK_RC(jfs_getattr(fs, "/e", &st), 0);
    CHECK(st.nlink == 3);
    CHECK_RC(jfs_rename(fs, "/e/sub", "/d", 0), 0); /* replace empty dir */
    CHECK_RC(jfs_getattr(fs, "/e", &st), 0);
    CHECK(st.nlink == 2);
    /* chmod / chown / utimens */
    CHECK_RC(jfs_chmod(fs, "/b", 04755), 0);
    CHECK_RC(jfs_chown(fs, "/b", 5, (uint32_t)-1), 0);
    CHECK_RC(jfs_getattr(fs, "/b", &st), 0);
    CHECK(st.uid == 5 && st.gid == 0 && (st.mode & 07777) == 0755); /* suid cleared by chown */
    {
        jfs_time t[2];
        t[0].sec = 1000;
        t[0].nsec = 5;
        t[1].sec = 0;
        t[1].nsec = JFS_UTIME_OMIT;
        CHECK_RC(jfs_getattr(fs, "/b", &st), 0);
        {
            jfs_time oldm = st.mtime;
            CHECK_RC(jfs_utimens(fs, "/b", t), 0);
            CHECK_RC(jfs_getattr(fs, "/b", &st), 0);
            CHECK(st.atime.sec == 1000 && st.atime.nsec == 5 && jfs_time_cmp(st.mtime, oldm) == 0);
        }
    }
    /* xattrs */
    CHECK_RC(jfs_setxattr(fs, "/b", "user.k", "v1", 2, JFS_XATTR_REPLACE), -JFS_ENOATTR);
    CHECK_RC(jfs_setxattr(fs, "/b", "user.k", "v1", 2, JFS_XATTR_CREATE), 0);
    CHECK_RC(jfs_setxattr(fs, "/b", "user.k", "v2", 2, JFS_XATTR_CREATE), -JFS_EEXIST);
    CHECK_RC(jfs_setxattr(fs, "/b", "user.k2", "", 0, 0), 0);
    CHECK_RC(jfs_getxattr(fs, "/b", "user.k", NULL, 0), 2);
    CHECK_RC(jfs_getxattr(fs, "/b", "user.k", buf, 1), -JFS_ERANGE);
    CHECK_RC(jfs_listxattr(fs, "/b", NULL, 0), 15);
    CHECK_RC(jfs_removexattr(fs, "/b", "user.k"), 0);
    CHECK_RC(jfs_getxattr(fs, "/b", "user.k", buf, sizeof buf), -JFS_ENOATTR);
    /* names */
    {
        char longname[300];
        memset(longname, 'n', sizeof longname);
        longname[0] = '/';
        longname[sizeof longname - 1] = 0;
        CHECK_RC(jfs_mkdir(fs, longname, 0755, &c), -JFS_ENAMETOOLONG);
    }
    CHECK_RC(write_file(fs, "/unicode-\xc3\xa9\xe2\x82\xac.txt", "u"), 0);
    CHECK_RC(write_file(fs, "/quote\"back\\slash", "q"), 0);
    /* persistence */
    CHECK_RC(jfs_close(fs), 0);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 5000);
    CHECK(fs != NULL);
    if (!fs)
        return;
    CHECK(read_file(fs, "/unicode-\xc3\xa9\xe2\x82\xac.txt") && !strcmp(read_file(fs, "/quote\"back\\slash"), "q"));
    CHECK_RC(jfs_getattr(fs, "/b", &st), 0);
    CHECK(st.uid == 5 && st.atime.sec == 1000);
    CHECK_RC(jfs_getxattr(fs, "/b", "user.k2", buf, sizeof buf), 0);
    CHECK_RC(jfs_getattr(fs, "/g", &st), -JFS_ENOENT);
    CHECK_RC(jfs_getattr(fs, "/s", &st), 0);
    /* read-only open */
    jfs_close(fs);
    {
        jfs_open_opts o;
        char err[200];
        jfs_open_opts_init(&o);
        o.readonly = 1;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), 0);
        CHECK_RC(jfs_mkdir(fs, "/ro", 0755, &c), -JFS_EROFS);
        CHECK_RC(jfs_open_file(fs, "/b", JFS_O_RDWR, &h2), -JFS_EROFS);
        CHECK_RC(jfs_open_file(fs, "/b", JFS_O_RDONLY, &h2), 0);
        jfs_release(fs, h2);
        jfs_close(fs);
    }
    {
        jfs_fsck_result r;
        CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
        CHECK(r.errors == 0 && r.warnings == 0);
    }
    rm_image(img);
}

static void sleep_tick(void)
{
    jfs_sleep_ms(20);
}

static void test_versioning(void)
{
    const char *img = tmp_path("ver.json"), *snap = tmp_path("ver-snap.json");
    jfs_mkfs_opts mo;
    jfs_fs *fs;
    jfs_time t0, t1, t2, t3;
    jfs_stat st;
    jfs_info in;
    rm_image(img);
    rm_image(snap);
    memset(&mo, 0, sizeof mo);
    mo.versioning = 1;
    mo.compression = "lz4";
    CHECK_RC(jfs_mkfs(img, &mo, 1), 0);
    sleep_tick();
    t0 = jfs_now();
    sleep_tick();
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0); /* commit after every change */
    if (!fs)
        return;
    CHECK_RC(write_file(fs, "/f", "version one"), 0);
    sleep_tick();
    t1 = jfs_now();
    sleep_tick();
    CHECK_RC(jfs_chmod(fs, "/f", 0600), 0); /* metadata only: content shared (data_ref) */
    CHECK_RC(write_file(fs, "/f", "version two"), 0);
    CHECK_RC(write_file(fs, "/other", "o"), 0);
    sleep_tick();
    t2 = jfs_now();
    sleep_tick();
    CHECK_RC(jfs_unlink(fs, "/f"), 0); /* becomes a delete marker */
    CHECK_RC(jfs_rename(fs, "/other", "/renamed", 0), 0);
    sleep_tick();
    t3 = jfs_now();
    jfs_get_info(fs, &in);
    CHECK(in.deleted_markers == 1 && in.history_records > 3);
    CHECK_RC(jfs_close(fs), 0);
    {
        jfs_open_opts o;
        char err[200];
        jfs_open_opts_init(&o);
        o.has_at = 1;
        o.at = t1;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), 0);
        CHECK(read_file(fs, "/f") && !strcmp(read_file(fs, "/f"), "version one"));
        CHECK_RC(jfs_getattr(fs, "/other", &st), -JFS_ENOENT);
        CHECK_RC(jfs_getattr(fs, "/f", &st), 0);
        CHECK((st.mode & 0777) == 0644);
        CHECK_RC(write_file(fs, "/x", "x"), -JFS_EROFS);
        jfs_close(fs);
        o.at = t2;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), 0);
        CHECK(read_file(fs, "/f") && !strcmp(read_file(fs, "/f"), "version two"));
        CHECK_RC(jfs_getattr(fs, "/f", &st), 0);
        CHECK((st.mode & 0777) == 0600);
        CHECK(read_file(fs, "/other") && !strcmp(read_file(fs, "/other"), "o"));
        jfs_close(fs);
        o.at = t3;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), 0);
        CHECK_RC(jfs_getattr(fs, "/f", &st), -JFS_ENOENT);
        CHECK(read_file(fs, "/renamed") != NULL);
        jfs_close(fs);
        o.at = t0;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), 0);
        CHECK_RC(jfs_getattr(fs, "/f", &st), -JFS_ENOENT);
        jfs_close(fs);
        t0.sec -= 3600;
        o.at = t0;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), -JFS_ENOENT); /* before mkfs */
    }
    {
        jfs_fsck_result r;
        CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
        CHECK(r.errors == 0 && r.warnings == 0);
    }
    /* snapshot extraction */
    CHECK_RC(jfs_snapshot(img, t2, snap, 0, "snap", 0), 0);
    fs = open_img(snap, JFS_LOCKMODE_EXCLUSIVE, 5000);
    CHECK(fs != NULL);
    if (fs) {
        jfs_get_info(fs, &in);
        CHECK(!in.versioning && in.history_records == 0 && !strcmp(in.label, "snap"));
        CHECK(read_file(fs, "/f") && !strcmp(read_file(fs, "/f"), "version two"));
        jfs_close(fs);
    }
    /* recompress everything, history included */
    {
        uint64_t n, b0, b1;
        jfs_fsck_result r;
        CHECK_RC(jfs_recompress(img, "deflate:9", 1, 1, &n, &b0, &b1), 0);
        CHECK(n >= 2);
        CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
        CHECK(r.errors == 0);
        fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 5000);
        if (fs) {
            jfs_get_info(fs, &in);
            CHECK(!strcmp(in.default_compression, "deflate"));
            jfs_close(fs);
        }
    }
    /* prune history before t2: the t1 view must disappear, t2 must survive */
    {
        uint64_t removed;
        jfs_open_opts o;
        char err[200];
        CHECK_RC(jfs_prune_history(img, t2, &removed), 0);
        CHECK(removed > 0);
        jfs_open_opts_init(&o);
        o.has_at = 1;
        o.at = t2;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), 0);
        CHECK(read_file(fs, "/f") && !strcmp(read_file(fs, "/f"), "version two"));
        jfs_close(fs);
    }
    /* versioning off + purge */
    {
        uint64_t purged;
        CHECK_RC(jfs_set_versioning(img, 0, 1, &purged), 0);
        fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 5000);
        if (fs) {
            jfs_get_info(fs, &in);
            CHECK(!in.versioning && in.history_records == 0 && in.deleted_markers == 0);
            CHECK_RC(write_file(fs, "/n", "1"), 0);
            CHECK_RC(jfs_unlink(fs, "/n"), 0);
            jfs_close(fs);
        }
    }
    rm_image(img);
    rm_image(snap);
}

static void test_upgrade(void)
{
    const char *img = tmp_path("v1.json");
    jfs_mkfs_opts mo;
    jfs_fs *fs;
    int from = 0;
    jfs_info in;
    rm_image(img);
    memset(&mo, 0, sizeof mo);
    mo.schema = 1;
    mo.versioning = 1;
    CHECK_RC(jfs_mkfs(img, &mo, 1), 0);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0);
    if (!fs)
        return;
    CHECK_RC(write_file(fs, "/a", "1"), 0);
    CHECK_RC(write_file(fs, "/a", "2"), 0);
    CHECK_RC(jfs_setxattr(fs, "/a", "user.x", "y", 1, 0), -JFS_ENOTSUP);
    jfs_close(fs);
    {
        jfs_open_opts o;
        char err[200];
        jfs_open_opts_init(&o);
        o.lockmode = JFS_LOCKMODE_SHARED;
        CHECK_RC(jfs_open(img, &o, &fs, err, sizeof err), -JFS_EVERSION);
    }
    CHECK_RC(jfs_upgrade(img, 0, &from), 0);
    CHECK(from == 1);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0);
    if (!fs)
        return;
    jfs_get_info(fs, &in);
    CHECK(in.schema == 2);
    CHECK(!strcmp(read_file(fs, "/a"), "2"));
    CHECK_RC(jfs_setxattr(fs, "/a", "user.x", "y", 1, 0), 0);
    jfs_close(fs);
    CHECK_RC(jfs_upgrade(img, 0, &from), 0);
    CHECK(from == 2);
    CHECK_RC(jfs_upgrade(img, 1, &from), -JFS_EVERSION);
    {
        jfs_fsck_result r;
        CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
        CHECK(r.errors == 0 && r.warnings == 0);
    }
    rm_image(img);
}

static void test_fsck_detects_corruption(void)
{
    const char *img = tmp_path("corrupt.json");
    jfs_mkfs_opts mo;
    jfs_fs *fs;
    char *text, *p;
    size_t len;
    jfs_fsck_result r;
    rm_image(img);
    memset(&mo, 0, sizeof mo);
    mo.compression = "none";
    CHECK_RC(jfs_mkfs(img, &mo, 1), 0);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0);
    if (!fs)
        return;
    CHECK_RC(write_file(fs, "/f", "hello world"), 0);
    jfs_close(fs);
    /* flip the content without fixing the checksum: "hello world" = aGVsbG8gd29ybGQ= */
    CHECK(jfs_read_file(img, &text, &len) == 0);
    p = strstr(text, "aGVsbG8gd29ybGQ=");
    CHECK(p != NULL);
    if (p) {
        memcpy(p, "aGVtbG8gd29ybGQ=", 16); /* "hemlo world" */
        CHECK(jfs_write_file_atomic(img, text, len) == 0);
    }
    free(text);
    CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
    CHECK(r.errors == 1);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0);
    if (fs) {
        CHECK(read_file(fs, "/f") == NULL); /* EIO, never wrong data */
        jfs_close(fs);
    }
    /* dangling entry + orphan, then repair */
    CHECK(jfs_read_file(img, &text, &len) == 0);
    p = strstr(text, "\"f\": 2");
    CHECK(p != NULL);
    if (p) {
        memcpy(p, "\"f\": 9", 6);
        CHECK(jfs_write_file_atomic(img, text, len) == 0);
    }
    free(text);
    CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
    CHECK(r.errors >= 2);
    CHECK_RC(jfs_fsck(img, 1, 0, NULL, NULL, &r), 0);
    CHECK(r.repaired > 0);
    CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
    CHECK(r.errors == 1); /* the checksum error cannot be repaired */
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 0);
    if (fs) {
        jfs_stat st;
        CHECK_RC(jfs_getattr(fs, "/lost+found/#2", &st), 0);
        jfs_close(fs);
    }
    rm_image(img);
}

static void test_multi_attach(void)
{
    const char *img = tmp_path("shared.json");
    jfs_mkfs_opts mo;
    jfs_fs *a, *b;
    jfs_handle *h, *hb;
    jfs_stat st;
    char buf[64];
    rm_image(img);
    memset(&mo, 0, sizeof mo);
    CHECK_RC(jfs_mkfs(img, &mo, 1), 0);
    a = open_img(img, JFS_LOCKMODE_SHARED, 5000);
    b = open_img(img, JFS_LOCKMODE_SHARED, 5000);
    CHECK(a && b);
    if (!a || !b)
        return;
    CHECK_RC(jfs_mkdir(a, "/fromA", 0755, NULL), 0);
    CHECK_RC(jfs_getattr(b, "/fromA", &st), 0);
    CHECK_RC(write_file(b, "/fromA/file", "bbb"), 0);
    CHECK(read_file(a, "/fromA/file") && !strcmp(read_file(a, "/fromA/file"), "bbb"));
    /* close-to-open: data written through an open handle appears after release */
    CHECK_RC(jfs_open_file(a, "/fromA/file", JFS_O_RDWR, &h), 0);
    CHECK_RC(jfs_write(a, h, "AAAA", 4, 0), 4);
    CHECK_RC(jfs_mkdir(b, "/meanwhile", 0755, NULL), 0); /* forces a reload in A with dirty data */
    CHECK_RC(jfs_getattr(a, "/meanwhile", &st), 0);
    CHECK_RC(jfs_read(a, h, buf, sizeof buf, 0), 4);
    CHECK(!memcmp(buf, "AAAA", 4));
    CHECK_RC(jfs_release(a, h), 0);
    CHECK(!strcmp(read_file(b, "/fromA/file"), "AAAA"));
    /* peer deletes a file we have open: our handle keeps working */
    CHECK_RC(jfs_open_file(b, "/fromA/file", JFS_O_RDONLY, &hb), 0);
    CHECK_RC(jfs_unlink(a, "/fromA/file"), 0);
    CHECK_RC(jfs_getattr(b, "/fromA/file", &st), -JFS_ENOENT);
    CHECK_RC(jfs_read(b, hb, buf, sizeof buf, 0), 4);
    CHECK_RC(jfs_release(b, hb), 0);
    CHECK_RC(jfs_close(a), 0);
    CHECK_RC(jfs_close(b), 0);
    {
        jfs_fsck_result r;
        CHECK_RC(jfs_fsck(img, 0, 0, NULL, NULL, &r), 0);
        CHECK(r.errors == 0 && r.warnings == 0);
    }
    rm_image(img);
}

static void test_large_file(void)
{
    const char *img = tmp_path("large.json");
    jfs_mkfs_opts mo;
    jfs_fs *fs;
    jfs_handle *h;
    size_t n = 3 * 1024 * 1024, i;
    uint8_t *data = malloc(n), *back = malloc(n);
    rm_image(img);
    for (i = 0; i < n; i++)
        data[i] = (uint8_t)((i * 7) ^ (i >> 9) ^ (rnd() % 4 == 0 ? rnd() : 0));
    memset(&mo, 0, sizeof mo);
    CHECK_RC(jfs_mkfs(img, &mo, 1), 0);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 5000);
    if (!fs)
        return;
    CHECK_RC(jfs_create(fs, "/big", 0644, JFS_O_WRONLY, NULL, &h), 0);
    for (i = 0; i < n; i += 128 * 1024)
        CHECK_RC(jfs_write(fs, h, data + i, n - i < 131072 ? n - i : 131072, i), n - i < 131072 ? n - i : 131072);
    jfs_release(fs, h);
    CHECK_RC(jfs_close(fs), 0);
    fs = open_img(img, JFS_LOCKMODE_EXCLUSIVE, 5000);
    if (!fs)
        return;
    CHECK_RC(jfs_open_file(fs, "/big", JFS_O_RDONLY, &h), 0);
    CHECK_RC(jfs_read(fs, h, back, n, 0), n);
    CHECK(!memcmp(data, back, n));
    jfs_release(fs, h);
    jfs_close(fs);
    free(data);
    free(back);
    rm_image(img);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(scratch, sizeof scratch, "%s", argv[1]);
    test_sha256();
    test_base64();
    test_compressors();
    test_json();
    test_time();
    test_posix();
    test_versioning();
    test_upgrade();
    test_fsck_detects_corruption();
    test_multi_attach();
    test_large_file();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
