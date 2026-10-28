// This file is used to test YYJSON_READ_NO_NUL_TERM (zero-copy strings for
// the two-stage reader): the result must match the regular reader when
// strings are compared by length and bytes, and the input must not change.

#include "yyjson.h"
#include "yy_test_utils.h"

#if !YYJSON_DISABLE_READER && !YYJSON_DISABLE_WRITER

static const char pad_junk[] = "\"\\1e9tu{[ \xff";

/* compare all values of two documents by type, length and bytes */
static bool doc_equal_len(yyjson_doc *a, yyjson_doc *b) {
    usize i, n, l1, l2;
    yyjson_val *ra, *rb;
    char *w1, *w2;
    bool ok;
    if (yyjson_doc_get_read_size(a) != yyjson_doc_get_read_size(b)) return false;
    n = yyjson_doc_get_val_count(a);
    if (n != yyjson_doc_get_val_count(b)) return false;
    ra = yyjson_doc_get_root(a);
    rb = yyjson_doc_get_root(b);
    for (i = 0; i < n; i++) {
        yyjson_val *x = ra + i, *y = rb + i;
        if (x->tag != y->tag) return false;
        switch (yyjson_get_type(x)) {
            case YYJSON_TYPE_STR:
                if (memcmp(x->uni.str, y->uni.str, yyjson_get_len(x))) return false;
                if (!yyjson_equals_strn(y, x->uni.str, yyjson_get_len(x))) return false;
                break;
            case YYJSON_TYPE_NUM:
                if (x->uni.u64 != y->uni.u64) return false;
                break;
            case YYJSON_TYPE_ARR:
            case YYJSON_TYPE_OBJ:
                if (x->uni.ofs != y->uni.ofs) return false;
                break;
            default: break;
        }
    }
    /* writer and mutable copy use lengths */
    w1 = yyjson_write(a, 0, &l1);
    w2 = yyjson_write(b, 0, &l2);
    ok = w1 && w2 && l1 == l2 && !memcmp(w1, w2, l1);
    free(w1); free(w2);
    if (!ok) return false;
    {
        yyjson_mut_doc *m = yyjson_doc_mut_copy(b, NULL);
        w1 = yyjson_write(a, YYJSON_WRITE_PRETTY, &l1);
        w2 = yyjson_mut_write(m, YYJSON_WRITE_PRETTY, &l2);
        ok = m && w1 && w2 && l1 == l2 && !memcmp(w1, w2, l1);
        free(w1); free(w2);
        yyjson_mut_doc_free(m);
    }
    return ok;
}

/* read `dat` with and without the flag; the flagged input buffer has exactly
   YYJSON_PADDING_SIZE bytes of non-zero padding */
static void test_one(const u8 *dat, usize len, usize seed) {
    yyjson_read_err e1, e2;
    yyjson_doc *d1, *d2;
    usize i;
    u8 *b2 = (u8 *)malloc(len + YYJSON_PADDING_SIZE);
    u8 *chk = (u8 *)malloc(len + YYJSON_PADDING_SIZE);
    yy_assert(b2 && chk);
    memcpy(b2, dat, len);
    for (i = 0; i < YYJSON_PADDING_SIZE; i++) {
        b2[len + i] = (u8)pad_junk[(seed + i * 7) % (sizeof(pad_junk) - 1)];
    }
    memcpy(chk, b2, len + YYJSON_PADDING_SIZE);
    d1 = yyjson_read_opts((char *)(void *)dat, len, 0, NULL, &e1);
    d2 = yyjson_read_opts((char *)b2, len,
                          YYJSON_READ_STAGE1 | YYJSON_READ_NO_NUL_TERM,
                          NULL, &e2);
    yy_assert(!!d1 == !!d2);
    if (d1) {
        yy_assert(doc_equal_len(d1, d2));
    } else {
        yy_assert(e1.code == e2.code && e1.pos == e2.pos);
    }
    yy_assert(memcmp(chk, b2, len + YYJSON_PADDING_SIZE) == 0);
    yyjson_doc_free(d1);
    yyjson_doc_free(d2);
    free(b2);
    free(chk);
}

static void test_dir(const char *sub) {
    char dir[YY_MAX_PATH];
    int count, i;
    char **names;
    yy_path_combine(dir, YYJSON_TEST_DATA_PATH, "data", "json", sub, NULL);
    names = yy_dir_read(dir, &count);
    yy_assertf(names != NULL && count != 0, "read dir fail:%s\n", dir);
    for (i = 0; i < count; i++) {
        char path[YY_MAX_PATH];
        u8 *dat;
        usize len, cut;
        if (names[i][0] == '.') continue;
        yy_path_combine(path, dir, names[i], NULL);
        if (!yy_file_read(path, &dat, &len)) continue; /* sub directory */
        if (len) {
            test_one(dat, len, (usize)i);
            /* truncated inputs exercise the tail handling */
            for (cut = 1; cut < len && cut <= 16; cut++) {
                test_one(dat, len - cut, (usize)i + cut);
            }
        }
        free(dat);
    }
    yy_dir_free(names);
}

static void test_cases(void) {
    static const char *cases[] = {
        "[]", "{}", "[1]", "[1,2", "[tr", "[\"a\\u12", "{\"a\":1e", "[[1]",
        "[123456789012345678901234567890123456789012345678901234567890]",
        "[\"\\ud83d\\ude00\",\"x\\n\",\"\\u00e9\\u4e2d\\u6587\\u65e5\\u672c\"]",
        "{\"key\":\"value\",\"esc\":\"a\\tb\",\"n\":-1.5e3,\"t\":true,"
        "\"f\":false,\"z\":null,\"arr\":[\"\",\"\\\\\"]}",
        NULL
    };
    usize i, k;
    for (i = 0; cases[i]; i++) {
        for (k = 0; k < sizeof(pad_junk); k++) {
            test_one((const u8 *)cases[i], strlen(cases[i]), k);
        }
    }

#ifndef YYJSON_DISABLE_STAGE1
    { /* zero-copy: unescaped strings point into the input, which is const */
        const char *src = "{\"key\":\"value\",\"esc\":\"a\\nb\"}";
        usize len = strlen(src);
        char *buf = (char *)malloc(len + YYJSON_PADDING_SIZE);
        yyjson_doc *doc;
        yyjson_val *v;
        memcpy(buf, src, len);
        memset(buf + len, 'x', YYJSON_PADDING_SIZE);
        doc = yyjson_read(buf, len, YYJSON_READ_STAGE1 | YYJSON_READ_NO_NUL_TERM);
        yy_assert(doc);
        v = yyjson_obj_get(yyjson_doc_get_root(doc), "key");
        yy_assert(yyjson_equals_str(v, "value"));
        yy_assert(yyjson_get_str(v) == buf + 8);
        v = yyjson_obj_get(yyjson_doc_get_root(doc), "esc");
        yy_assert(yyjson_get_len(v) == 3 && !memcmp(yyjson_get_str(v), "a\nb", 3));
        yy_assert(yyjson_get_str(v) < buf || yyjson_get_str(v) >= buf + len);
        yy_assert(!memcmp(buf, src, len));
        yyjson_doc_free(doc);
        /* ignored with INSITU: strings are NUL-terminated as usual */
        memcpy(buf, src, len);
        memset(buf + len, 0, YYJSON_PADDING_SIZE);
        doc = yyjson_read_opts(buf, len, YYJSON_READ_INSITU |
                               YYJSON_READ_STAGE1 | YYJSON_READ_NO_NUL_TERM,
                               NULL, NULL);
        yy_assert(doc);
        v = yyjson_obj_get(yyjson_doc_get_root(doc), "key");
        yy_assert(strcmp(yyjson_get_str(v), "value") == 0);
        yyjson_doc_free(doc);
        free(buf);
    }
#endif
}

yy_test_case(test_read_no_nul_term) {
    test_cases();
    test_dir("test_yyjson");
    test_dir("test_checker");
    test_dir("test_parsing");
    test_dir("test_transform");
    test_dir("test_encoding");
    test_dir("test_roundtrip");
}

#else
yy_test_case(test_read_no_nul_term) {}
#endif
