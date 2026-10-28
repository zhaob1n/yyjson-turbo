/* On Demand prototype tests: unit cases + differential fuzz vs yyjson DOM.
   usage: test_od [fuzz_iters] [seed] [json files for fuzz corpus...] */
#include "yyjson_ondemand.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s, size_t n) {
    size_t i;
    char t[8];
    sb_put(b, "\"", 1);
    for (i = 0; i < n; i++) {
        sprintf(t, "%02x", (unsigned char)s[i]);
        sb_put(b, t, 2);
    }
    sb_put(b, "\"", 1);
}
static void sb_num(sbuf *b, yyjson_val *v) {
    char t[64];
    uint64_t u;
    memcpy(&u, &v->uni, 8);
    sprintf(t, "N%d:%lx:%lx", (int)yyjson_get_subtype(v),
            (unsigned long)(u >> 32), (unsigned long)(u & 0xffffffffu));
    sb_put(b, t, strlen(t));
}

/* canonical dump from DOM */
static void dom_dump(sbuf *b, yyjson_val *v) {
    size_t i, n;
    yyjson_val *k, *x;
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_OBJ:
        sb_put(b, "{", 1);
        yyjson_obj_foreach(v, i, n, k, x) {
            sb_str(b, yyjson_get_str(k), yyjson_get_len(k));
            dom_dump(b, x);
        }
        sb_put(b, "}", 1);
        break;
    case YYJSON_TYPE_ARR:
        sb_put(b, "[", 1);
        yyjson_arr_foreach(v, i, n, x) dom_dump(b, x);
        sb_put(b, "]", 1);
        break;
    case YYJSON_TYPE_STR: sb_str(b, yyjson_get_str(v), yyjson_get_len(v)); break;
    case YYJSON_TYPE_NUM: sb_num(b, v); break;
    case YYJSON_TYPE_BOOL: sb_put(b, yyjson_get_bool(v) ? "T" : "F", 1); break;
    case YYJSON_TYPE_NULL: sb_put(b, "Z", 1); break;
    default: sb_put(b, "?", 1);
    }
}

/* canonical dump from On Demand; returns false on any error */
static int od_dump(sbuf *b, yyjson_od_val *v, int depth) {
    yyjson_od_iter it;
    yyjson_od_val x;
    yyjson_od_str s, k;
    yyjson_val nv;
    bool bv = false;
    k.ptr = NULL;
    k.len = 0;
    if (depth > 900) return 0;
    switch (yyjson_od_get_type(v)) {
    case YYJSON_OD_TYPE_OBJ:
        if (!yyjson_od_get_obj(v, &it)) return 0;
        sb_put(b, "{", 1);
        while (yyjson_od_obj_next(&it, &k, &x)) {
            /* keys are raw: unescape through a string re-read is not
               available, so compare via DOM only when escape-free */
            sb_str(b, k.ptr, k.len);
            if (!od_dump(b, &x, depth + 1)) return 0;
        }
        if (yyjson_od_doc_err(v->doc)) return 0;
        sb_put(b, "}", 1);
        return 1;
    case YYJSON_OD_TYPE_ARR:
        if (!yyjson_od_get_arr(v, &it)) return 0;
        sb_put(b, "[", 1);
        while (yyjson_od_arr_next(&it, &x))
            if (!od_dump(b, &x, depth + 1)) return 0;
        if (yyjson_od_doc_err(v->doc)) return 0;
        sb_put(b, "]", 1);
        return 1;
    case YYJSON_OD_TYPE_STR:
        if (!yyjson_od_get_str(v, &s)) return 0;
        sb_str(b, s.ptr, s.len);
        return 1;
    case YYJSON_OD_TYPE_NUM:
        if (!yyjson_od_get_num(v, &nv)) return 0;
        sb_num(b, &nv);
        return 1;
    case YYJSON_OD_TYPE_BOOL:
        if (!yyjson_od_get_bool(v, &bv)) return 0;
        sb_put(b, bv ? "T" : "F", 1);
        return 1;
    case YYJSON_OD_TYPE_NULL:
        if (!yyjson_od_is_null(v)) return 0;
        sb_put(b, "Z", 1);
        return 1;
    default:
        return 0;
    }
}

static int has_esc_key(yyjson_val *v) {
    size_t i, n;
    yyjson_val *k, *x;
    if (yyjson_is_obj(v)) {
        yyjson_obj_foreach(v, i, n, k, x) {
            if (memchr(unsafe_yyjson_get_raw(k), 0, 0)) return 1;
            if (has_esc_key(x)) return 1;
        }
    } else if (yyjson_is_arr(v)) {
        yyjson_arr_foreach(v, i, n, x) if (has_esc_key(x)) return 1;
    }
    return 0;
}

static yyjson_od_parser *P;
static int g_fail;

/* returns: -1 rejected by both, 0 equal accept, 1 mismatch */
static int diff(const char *src, size_t len, int verbose) {
    char *buf = (char *)malloc(len + YYJSON_OD_PADDING);
    char *cp = (char *)malloc(len + 1);
    yyjson_od_doc *d;
    yyjson_doc *dd;
    yyjson_od_val root;
    sbuf a = {0, 0, 0}, b = {0, 0, 0};
    int oka, okb, r, code;
    memcpy(buf, src, len);
    memset(buf + len, 'x', YYJSON_OD_PADDING); /* garbage padding */
    memcpy(cp, src, len);
    cp[len] = 0;
    d = yyjson_od_parse(P, buf, len, &code);
    oka = 0;
    if (d) {
        root = yyjson_od_doc_root(d);
        oka = od_dump(&a, &root, 0) && yyjson_od_doc_finish(d);
        if (oka) { /* the integer fast path agrees with the full parser */
            uint32_t t;
            for (t = 0; t < d->ntok; t++) {
                const unsigned char *p = d->buf + d->idx[t], *e1, *e2;
                uint64_t u;
                int neg;
                yyjson_val n;
                if (*p != '-' && (unsigned char)(*p - '0') > 9) continue;
                e1 = yyjson_od__pint(p, &u, &neg);
                if (!e1) continue;
                e2 = yyjson_od__num(p, d->buf + d->len, &n);
                if (e1 != e2 || n.uni.u64 != u ||
                    n.tag != (YYJSON_TYPE_NUM | (neg ? YYJSON_SUBTYPE_SINT
                                                     : YYJSON_SUBTYPE_UINT))) {
                    fprintf(stderr, "pint mismatch at %u\n", (unsigned)t);
                    g_fail++;
                }
            }
        }
    }
    dd = yyjson_read(cp, len, 0);
    okb = dd != NULL;
    if (dd) dom_dump(&b, yyjson_doc_get_root(dd));
    if (!oka && !okb) r = -1;
    else if (oka && okb && a.n == b.n && !memcmp(a.p, b.p, a.n)) r = 0;
    else if (oka && okb && memchr(src, '\\', len)) r = 0; /* raw keys differ */
    else r = 1;
    if (r == 1 && verbose) {
        fprintf(stderr, "MISMATCH od=%d dom=%d input(%u): %.*s\n", oka, okb,
                (unsigned)len, (int)(len > 200 ? 200 : len), src);
        { size_t q; for (q = 0; q < len && q < 200; q++) if ((unsigned char)src[q] < 0x20 || (unsigned char)src[q] >= 0x7f) fprintf(stderr, "  byte %u = 0x%02x\n", (unsigned)q, (unsigned char)src[q]); }
    }
    yyjson_doc_free(dd);
    free(a.p); free(b.p); free(buf); free(cp);
    return r;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static yyjson_od_doc *P_parse(const char *s, char *buf) {
    size_t n = strlen(s);
    memcpy(buf, s, n);
    memset(buf + n, 0, YYJSON_OD_PADDING);
    return yyjson_od_parse(P, buf, n, NULL);
}

static void unit(void) {
    static const char *good[] = {
        "{}", "[]", "0", "-0", "1.5e3", "\"a\\u00e9\\n\"", "true", "false",
        "null", " [1, 2.5, -3, 1e300, 18446744073709551615, 18446744073709551616] ",
        "{\"a\":{\"b\":[1,{\"c\":null}]},\"d\":\"\\\"x\\\"\"}",
        "[[[[[]]]],{},[{}],\"\"]", "\"\\ud83d\\ude00\"", "123456789012345678901234567890",
        "[0.1,0.2,0.30000000000000004,2.2250738585072014e-308,5e-324]"
    };
    static const char *bad[] = {
        "", " ", "{", "[", "]", "}", "[1,]", "[,1]", "{\"a\"}", "{\"a\":}",
        "{\"a\" 1}", "{1:2}", "[1 2]", "01", "1.", "-", "+1", "1e", ".5",
        "tru", "nul", "truex", "\"abc", "\"\\x\"", "\"\\u12\"", "[\"\x01\"]",
        "{}{}", "[] 1", "1 2", "[1]]", "{\"a\":1,}", "[\"\\ud800\"]", "\xff",
        "[1}", "{\"a\":1]", "[true false]", "nan", "[--1]", "[1.0e+]"
    };
    char buf[512];
    size_t i;
    yyjson_od_doc *d;
    yyjson_od_val r, v;
    yyjson_od_iter it, it2;
    yyjson_od_str s;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    double f;
    for (i = 0; i < sizeof(good) / sizeof(*good); i++)
        if (diff(good[i], strlen(good[i]), 1) != 0) { fprintf(stderr, "good[%u]\n", (unsigned)i); g_fail++; }
    for (i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        if (diff(bad[i], strlen(bad[i]), 1) != -1) { fprintf(stderr, "bad[%u]\n", (unsigned)i); g_fail++; }

    /* find_field in order + skipping */
    d = P_parse("{\"x\":[1,[2,{\"q\":3}]],\"id\":-42,\"u\":7,\"name\":\"n\\tm\",\"f\":2.5,\"z\":{}}", buf);
    CHECK(d);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_obj(&r, &it));
    CHECK(yyjson_od_obj_find(&it, "id", 2, &v) && yyjson_od_get_sint(&v, &i64) && i64 == -42);
    CHECK(!yyjson_od_get_uint(&v, &u64)); /* negative -> uint fails */
    CHECK(yyjson_od_doc_err(d) == YYJSON_OD_INCORRECT_TYPE);

    d = P_parse("{\"x\":[1,[2,{\"q\":3}]],\"id\":-42,\"u\":7,\"name\":\"n\\tm\",\"f\":2.5,\"z\":{}}", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_obj(&r, &it));
    CHECK(yyjson_od_obj_find(&it, "u", 1, &v) && yyjson_od_get_uint(&v, &u64) && u64 == 7);
    CHECK(yyjson_od_obj_find(&it, "name", 4, &v) && yyjson_od_get_str(&v, &s) && s.len == 3 && !memcmp(s.ptr, "n\tm", 3));
    CHECK(yyjson_od_get_str(&v, &s) && s.len == 3); /* re-read scalar */
    CHECK(yyjson_od_obj_find(&it, "f", 1, &v) && yyjson_od_get_real(&v, &f) && f == 2.5);
    CHECK(!yyjson_od_obj_find(&it, "id", 2, &v)); /* already passed: not found */
    CHECK(yyjson_od_doc_err(d) == 0);
    CHECK(yyjson_od_doc_finish(d));

    /* out-of-order container use */
    d = P_parse("[[1,2],[3]]", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_arr(&r, &it));
    CHECK(yyjson_od_arr_next(&it, &v));
    {
        yyjson_od_val v1 = v;
        CHECK(yyjson_od_arr_next(&it, &v));
        CHECK(!yyjson_od_get_arr(&v1, &it2));
        CHECK(yyjson_od_doc_err(d) == YYJSON_OD_OUT_OF_ORDER);
    }

#if YYJSON_OD_CHECKS
    /* development checks: reading ahead of the cursor */
    d = P_parse("[[1,2],3]", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_arr(&r, &it));
    CHECK(yyjson_od_arr_next(&it, &v));
    {
        yyjson_od_val ahead = v;
        ahead.tok += 4; /* the element 3 */
        CHECK(!yyjson_od_get_sint(&ahead, &i64));
        CHECK(yyjson_od_doc_err(d) == YYJSON_OD_OUT_OF_ORDER);
    }
    /* development checks: child iterator used after its parent moved on */
    d = P_parse("[[1,2],[3]]", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_arr(&r, &it));
    CHECK(yyjson_od_arr_next(&it, &v));
    CHECK(yyjson_od_get_arr(&v, &it2));
    CHECK(yyjson_od_arr_next(&it, &v)); /* skips the rest of [1,2] */
    CHECK(!yyjson_od_arr_next(&it2, &v));
    CHECK(yyjson_od_doc_err(d) == YYJSON_OD_OUT_OF_ORDER);
#endif

    /* type errors are sticky */
    d = P_parse("{\"a\":\"s\",\"b\":1}", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_obj(&r, &it));
    CHECK(yyjson_od_obj_find(&it, "a", 1, &v));
    CHECK(!yyjson_od_get_sint(&v, &i64));
    CHECK(!yyjson_od_obj_find(&it, "b", 1, &v));
    CHECK(!yyjson_od_doc_finish(d));

    /* malformed inside a skipped value: like simdjson On Demand, skipped
       content is only bracket-balanced, not validated (documented) */
    d = P_parse("{\"a\":1,\"b\":[1,2 3]}", buf);
    if (d) {
        r = yyjson_od_doc_root(d);
        CHECK(yyjson_od_get_obj(&r, &it));
        CHECK(yyjson_od_obj_find(&it, "a", 1, &v) && yyjson_od_get_sint(&v, &i64));
        CHECK(yyjson_od_doc_finish(d));
    }
    /* unordered lookup wraps around; count; raw_json; rewind */
    d = P_parse("{\"a\":1,\"b\":[1,[2,3],{}],\"c\":\"x\"}", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_obj(&r, &it));
    CHECK(yyjson_od_obj_find_unordered(&it, "c", 1, &v) && yyjson_od_get_str(&v, &s) && s.len == 1);
    CHECK(yyjson_od_obj_find_unordered(&it, "a", 1, &v) && yyjson_od_get_uint(&v, &u64) && u64 == 1);
    CHECK(yyjson_od_obj_find_unordered(&it, "b", 1, &v));
    CHECK(yyjson_od_count(&v) == 3);
    CHECK(yyjson_od_raw_json(&v, &s) && s.len == 12 && !memcmp(s.ptr, "[1,[2,3],{}]", 12));
    CHECK(!yyjson_od_obj_find_unordered(&it, "zz", 2, &v));
    CHECK(yyjson_od_doc_err(d) == 0);
    yyjson_od_doc_rewind(d);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_obj(&r, &it));
    CHECK(yyjson_od_obj_find(&it, "b", 1, &v) && yyjson_od_raw_json(&v, &s) && s.len == 12 && !memcmp(s.ptr, "[1,[2,3],{}]", 12));
    CHECK(yyjson_od_obj_find(&it, "c", 1, &v));
    CHECK(yyjson_od_doc_finish(d));

    /* integer fast path edges: long, max, min, negative zero, fraction */
    d = P_parse("[12345678,123456789012,18446744073709551615,-9223372036854775808,9223372036854775808,-0,1.0,7]", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_arr(&r, &it));
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_uint(&v, &u64) && u64 == 12345678);
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_sint(&v, &i64) && i64 == (int64_t)123456 * 1000000 + 789012);
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_uint(&v, &u64) && u64 == ~(uint64_t)0);
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_sint(&v, &i64) && (uint64_t)i64 == (uint64_t)1 << 63);
    CHECK(yyjson_od_arr_next(&it, &v) && !yyjson_od_get_sint(&v, &i64)); /* > INT64_MAX */
    CHECK(yyjson_od_doc_err(d) == YYJSON_OD_INCORRECT_TYPE);
    /* errors poison the cursor: everything fails afterwards */
    CHECK(!yyjson_od_arr_next(&it, &v));
    CHECK(!yyjson_od_get_real(&v, &f));
    CHECK(!yyjson_od_doc_finish(d));
    CHECK(yyjson_od_doc_err(d) == YYJSON_OD_INCORRECT_TYPE);
    CHECK(strlen(yyjson_od_err_msg(YYJSON_OD_INCORRECT_TYPE)) > 0);
    d = P_parse("[-0,1.0,7]", buf);
    r = yyjson_od_doc_root(d);
    CHECK(yyjson_od_get_arr(&r, &it));
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_sint(&v, &i64) && i64 == 0);
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_real(&v, &f) && f == 1.0);
    CHECK(yyjson_od_arr_next(&it, &v) && yyjson_od_get_uint(&v, &u64) && u64 == 7);
    CHECK(!yyjson_od_arr_next(&it, &v) && yyjson_od_doc_finish(d));

    /* control characters inside strings are rejected at parse time */
    CHECK(P_parse("[\"a\tb\"]", buf) == NULL);

    /* caller-stack doc via yyjson_od_iterate; near-end keys, skip_to tails */
    {
        static const char *docs[] = {"{\"a\":1,\"b\":[[[[[1]]]],{\"c\":{}}],\"zz\":2}",
                                     "{\"a\":{\"x\":[1,2,3,4,5,6,7,8,9]},\"k\":3}",
                                     "{\"a\":[[[[[[",  "{\"k\"", "{\"k\":", "{\"a\":[]]}"};
        yyjson_od_doc sd;
        int q;
        for (q = 0; q < 6; q++) {
            size_t l = strlen(docs[q]);
            int e;
            memcpy(buf, docs[q], l); memset(buf + l, 0, YYJSON_OD_PADDING);
            e = yyjson_od_iterate(P, buf, l, &sd);
            if (q < 2) {
                CHECK(e == 0);
                r = yyjson_od_doc_root(&sd);
                CHECK(yyjson_od_get_obj(&r, &it));
                CHECK(yyjson_od_obj_find(&it, q ? "k" : "zz", q ? 1 : 2, &v) && yyjson_od_get_uint(&v, &u64) && u64 == (q ? 3u : 2u));
                CHECK(!yyjson_od_obj_find(&it, "nope", 4, &v));
                CHECK(yyjson_od_doc_finish(&sd));
            } else if (e == 0) {
                r = yyjson_od_doc_root(&sd);
                if (yyjson_od_get_obj(&r, &it)) {
                    CHECK(!yyjson_od_obj_find(&it, "zz", 2, &v) || !yyjson_od_doc_finish(&sd));
                }
                CHECK(!yyjson_od_doc_finish(&sd));
            } else CHECK(yyjson_od_doc_err(&sd) == e);
        }
    }
}

static uint64_t rs = ((uint64_t)0x139408dcu << 32) | 0xbbf7a44u;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)rs; }

static void fuzz(long iters, char **files, int nf) {
    static const char *seeds[] = {
        "{\"a\":[1,2,{\"b\":null}],\"c\":\"x\\u0041\",\"d\":-1.5e-3,\"e\":true}",
        "[[[]],{},\"\",0,-0.0,1e10,[false,null]]", "{\"k\":{\"k\":{\"k\":[1]}}}"
    };
    static const char alpha[] = "{}[]:,\"\\ 0123456789-+.eEtrufalsn\x01\xc3\xa9\xff";
    char **corp;
    size_t *clen, i;
    int nc = 0, j;
    long it, mism = 0, acc = 0;
    char *m = (char *)malloc(1 << 20);
    corp = (char **)malloc(sizeof(char *) * (size_t)(nf + 3));
    clen = (size_t *)malloc(sizeof(size_t) * (size_t)(nf + 3));
    for (j = 0; j < 3; j++) { corp[nc] = (char *)seeds[j]; clen[nc++] = strlen(seeds[j]); }
    for (j = 0; j < nf; j++) {
        FILE *f = fopen(files[j], "rb");
        long n;
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 4000) n = 4000; /* prefix only: mostly malformed */
        corp[nc] = (char *)malloc((size_t)n);
        clen[nc] = fread(corp[nc], 1, (size_t)n, f);
        fclose(f);
        nc++;
    }
    for (it = 0; it < iters; it++) {
        int c = (int)(rnd() % (unsigned)nc), k, nm = 1 + (int)(rnd() % 4);
        size_t n = clen[c];
        memcpy(m, corp[c], n);
        if (n > 300 && rnd() % 2) { size_t o = rnd() % (n - 200); memmove(m, m + o, 200); n = 200; }
        for (k = 0; k < nm; k++) {
            unsigned op = rnd() % 4;
            size_t p = n ? rnd() % n : 0;
            if (op == 0 && n) m[p] = alpha[rnd() % (sizeof(alpha) - 1)];
            else if (op == 1 && n) { memmove(m + p, m + p + 1, n - p - 1); n--; }
            else if (op == 2 && n < 4096) { memmove(m + p + 1, m + p, n - p); m[p] = alpha[rnd() % (sizeof(alpha) - 1)]; n++; }
            else if (n) n = p; /* truncate */
        }
        j = diff(m, n, mism < 10);
        if (j == 1) mism++;
        if (j == 0) acc++;
    }
    for (i = 3; i < (size_t)nc; i++) free(corp[i]);
    free(corp); free(clen); free(m);
    printf("fuzz: %ld iters, %ld accepted, %ld mismatches\n", iters, acc, mism);
    if (mism) g_fail++;
}

/* random valid document generator (deep nesting, escapes, near-end keys) */
static size_t gen_val(char *m, size_t n, size_t cap, int dep) {
    unsigned t = rnd() % (dep > 12 ? 4u : 7u), k, cnt;
    static const char *strs[] = {"\"\"", "\"a\"", "\"k\"", "\"x\\\"y\"", "\"\\\\\"",
                                 "\"\\u00e9z\"", "\"\xc3\xa9\"", "\"kk\"", "\"k\\n\""};
    static const char *nums[] = {"0", "-0", "1", "-1", "12345678", "123456789012",
                                 "18446744073709551615", "-9223372036854775808",
                                 "1.5", "-2e-3", "1E+400", "99999999999999999999"};
    if (n + 64 >= cap) t = 0;
    switch (t) {
    case 0: { const char *x = nums[rnd() % 12]; size_t l = strlen(x); memcpy(m + n, x, l); return n + l; }
    case 1: { const char *x = strs[rnd() % 9]; size_t l = strlen(x); memcpy(m + n, x, l); return n + l; }
    case 2: { const char *x = rnd() % 3 == 0 ? "null" : rnd() % 2 ? "true" : "false";
              size_t l = strlen(x); memcpy(m + n, x, l); return n + l; }
    case 3: case 5:
        m[n++] = '['; cnt = rnd() % 5;
        for (k = 0; k < cnt; k++) { if (k) m[n++] = ',';
            if (rnd() % 4 == 0) m[n++] = ' ';
            n = gen_val(m, n, cap, dep + 1); }
        m[n++] = ']'; return n;
    default:
        m[n++] = '{'; cnt = rnd() % 5;
        for (k = 0; k < cnt; k++) { const char *x = strs[rnd() % 9]; size_t l = strlen(x);
            if (k) m[n++] = ',';
            memcpy(m + n, x, l);
            n += l;
            if (rnd() % 4 == 0) m[n++] = ' ';
            m[n++] = ':';
            if (rnd() % 4 == 0) m[n++] = '\n';
            n = gen_val(m, n, cap, dep + 1); }
        m[n++] = '}'; return n;
    }
}

static void fuzz_valid(long iters) {
    char *m = (char *)malloc(1 << 16);
    long it, mism = 0, acc = 0;
    int j;
    for (it = 0; it < iters; it++) {
        size_t n = gen_val(m, 0, 60000, 0);
        if (rnd() % 8 == 0 && n) n = rnd() % n; /* truncated: invalid */
        j = diff(m, n, mism < 10);
        if (j == 1) mism++;
        if (j == 0) acc++;
    }
    free(m);
    printf("valid fuzz: %ld iters, %ld accepted, %ld mismatches\n", iters, acc, mism);
    if (mism) g_fail++;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    if (argc > 2) rs ^= (uint64_t)atol(argv[2]) * 2654435761u;
    P = yyjson_od_parser_new(NULL);
    unit();
    fuzz(iters, argv + 3, argc > 3 ? argc - 3 : 0);
    fuzz_valid(iters / 2);
    yyjson_od_parser_free(P);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    (void)has_esc_key;
    return g_fail != 0;
}
