/*==============================================================================
 * yyjson_ondemand.c - EXPERIMENTAL On Demand reader prototype.
 * See yyjson_ondemand.h.
 *============================================================================*/
#include "yyjson_ondemand.h"

#include <stdlib.h>

struct yyjson_od_parser {
    yyjson_alc alc;
    uint32_t *idx;
    unsigned char *scr;
    unsigned char *tmp;     /* padded copy for scalar roots */
    size_t cap;             /* input capacity of idx/scr */
    size_t tmp_cap;
    yyjson_od_doc doc;
};

const signed char yyjson_od__dtab[256] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0, -1,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0, -1,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

const unsigned char yyjson_od__ttab[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const unsigned char yyjson_od__vtab[256] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  1,  0,
     0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static void *od_def_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void *od_def_realloc(void *ctx, void *ptr, size_t old_size,
                            size_t size) {
    (void)ctx; (void)old_size;
    return realloc(ptr, size);
}
static void od_def_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

yyjson_od_parser *yyjson_od_parser_new(const yyjson_alc *alc) {
    yyjson_od_parser *p;
    yyjson_alc a;
    if (alc) {
        a = *alc;
    } else {
        a.malloc = od_def_malloc;
        a.realloc = od_def_realloc;
        a.free = od_def_free;
        a.ctx = NULL;
    }
    p = (yyjson_od_parser *)a.malloc(a.ctx, sizeof(yyjson_od_parser));
    if (!p) return NULL;
    memset(p, 0, sizeof(yyjson_od_parser));
    p->alc = a;
    return p;
}

static void od_release(yyjson_od_parser *p) {
    if (p->idx) p->alc.free(p->alc.ctx, p->idx);
    if (p->scr) p->alc.free(p->alc.ctx, p->scr);
    p->idx = NULL;
    p->scr = NULL;
    p->cap = 0;
}

void yyjson_od_parser_free(yyjson_od_parser *p) {
    if (!p) return;
    od_release(p);
    if (p->tmp) p->alc.free(p->alc.ctx, p->tmp);
    p->alc.free(p->alc.ctx, p);
}

static bool od_reserve(yyjson_od_parser *p, size_t len) {
    size_t cap;
    if (len <= p->cap) return true;
    od_release(p);
    cap = len + len / 4; /* some headroom for growing inputs */
    if (cap < 4096) cap = 4096;
    p->idx = (uint32_t *)p->alc.malloc(p->alc.ctx,
                                       (cap + 128) * sizeof(uint32_t));
    p->scr = (unsigned char *)p->alc.malloc(p->alc.ctx, cap + 64);
    if (!p->idx || !p->scr) {
        od_release(p);
        return false;
    }
    p->cap = cap;
    return true;
}

#define OD_RET(c) do { if (code) *code = (c); return NULL; } while (0)

yyjson_od_doc *yyjson_od_parse(yyjson_od_parser *p, const char *buf,
                               size_t len, int *code) {
    yyjson_od_doc *d;
    size_t ntok = 0;
    unsigned char c0, cl;
    if (code) *code = YYJSON_OD_OK;
    if (!p || !buf) OD_RET(YYJSON_OD_EMPTY);
    if (len == 0) OD_RET(YYJSON_OD_EMPTY);
    if (len >= (size_t)0xFFFFFF00u) OD_RET(YYJSON_OD_TOO_LARGE);
    if (!od_reserve(p, len)) OD_RET(YYJSON_OD_MEMORY);
    if (!yyjson_od__index((const unsigned char *)buf, len, p->idx,
                          &ntok)) {
        OD_RET(YYJSON_OD_INVALID);
    }
    if (ntok == 0) OD_RET(YYJSON_OD_EMPTY);
    d = &p->doc;
    d->buf = (const unsigned char *)buf;
    d->idx = p->idx;
    d->scr = p->scr;
    d->scr_beg = p->scr;
    d->scr_end = p->scr + p->cap + 64;
    d->len = len;
    d->ntok = (uint32_t)ntok;
    d->ip = 0;
    d->depth = 0;
    d->err = 0;
    d->err_tok = 0;
    c0 = d->buf[p->idx[0]];
    if (c0 == '{' || c0 == '[') {
        /* every scalar is then followed by a structural byte inside the
           input, which keeps number parsing in bounds */
        cl = d->buf[p->idx[ntok - 1]];
        if (cl != '}' && cl != ']') OD_RET(YYJSON_OD_TAPE);
    } else {
        /* scalar root: parse a zero-padded private copy */
        if (p->tmp_cap < len + YYJSON_OD_PADDING) {
            if (p->tmp) p->alc.free(p->alc.ctx, p->tmp);
            p->tmp_cap = 0;
            p->tmp = (unsigned char *)p->alc.malloc(p->alc.ctx,
                                                    len + YYJSON_OD_PADDING);
            if (!p->tmp) OD_RET(YYJSON_OD_MEMORY);
            p->tmp_cap = len + YYJSON_OD_PADDING;
        }
        memcpy(p->tmp, buf, len);
        memset(p->tmp + len, 0, YYJSON_OD_PADDING);
        d->buf = p->tmp;
    }
    return d;
}

int yyjson_od_iterate(yyjson_od_parser *p, const char *buf, size_t len,
                      yyjson_od_doc *doc) {
    int code;
    yyjson_od_doc *d = yyjson_od_parse(p, buf, len, &code);
    if (!d) {
        memset(doc, 0, sizeof(*doc));
        doc->err = code;
        return code;
    }
    *doc = *d;
    return YYJSON_OD_OK;
}

bool yyjson_od__fail(yyjson_od_doc *d, int code) {
    if (!d->err) {
        d->err = code;
        d->err_tok = d->ip;
    }
    /* Poison the cursor: no handle matches it, and depth 0 makes every
       iterator step fail. Sentinels keep reads at ntok in bounds. */
    d->ip = d->ntok;
    d->depth = 0;
    return false;
}

bool yyjson_od__skip_to(yyjson_od_doc *d, uint32_t target) {
    const unsigned char *buf = d->buf;
    const uint32_t *idx = d->idx;
    uint32_t ip = d->ip, n = d->ntok, depth = d->depth;
    /* bounds are checked once per 4 tokens: the 4 sentinels at ntok keep
       the over-read in bounds, and ip > ntok is caught below */
    while (ip + 4 <= n) {
#define OD_SKIP_STEP \
        depth = (uint32_t)((int32_t)depth + yyjson_od__dtab[buf[idx[ip]]]); \
        ip++; \
        if (depth <= target) goto done;
        OD_SKIP_STEP OD_SKIP_STEP OD_SKIP_STEP OD_SKIP_STEP
#undef OD_SKIP_STEP
    }
    while (depth > target) {
        if (yyjson_od_unlikely(ip >= n)) {
            d->ip = ip;
            d->depth = depth;
            return yyjson_od__fail(d, YYJSON_OD_TAPE);
        }
        depth = (uint32_t)((int32_t)depth + yyjson_od__dtab[buf[idx[ip]]]);
        ip++;
    }
done:
    d->ip = ip;
    d->depth = depth;
    if (yyjson_od_unlikely(depth != target)) /* unbalanced brackets */
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    return true;
}

bool yyjson_od__key_check(yyjson_od_doc *d, uint32_t s, uint32_t e) {
    yyjson_od_str tmp;
    unsigned char *save = d->scr;
    if (!yyjson_od__str_slow(d, s, e, &tmp)) return false;
    d->scr = save; /* decoded form is discarded */
    return true;
}

bool yyjson_od__str_slow(yyjson_od_doc *d, uint32_t s, uint32_t e,
                         yyjson_od_str *out) {
    unsigned char *dst = d->scr, *end;
    if ((size_t)(d->scr_end - dst) < (size_t)(e - s) + 40)
        return yyjson_od__fail(d, YYJSON_OD_MEMORY); /* only on re-reads */
    end = yyjson_od__str_esc(d->buf + s, d->buf + e, dst);
    if (!end) return yyjson_od__fail(d, YYJSON_OD_STRING);
    out->ptr = (const char *)dst;
    out->len = (size_t)(end - dst);
    d->scr = end + 1;
    return true;
}

bool yyjson_od__num_slow(yyjson_od_doc *d, uint32_t tok, yyjson_val *out) {
    const unsigned char *p = d->buf + d->idx[tok], *e;
    unsigned char c = *p;
    if (c != '-' && (unsigned char)(c - '0') > 9)
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
    e = yyjson_od__num(p, d->buf + d->len, out);
    if (!e) return yyjson_od__fail(d, YYJSON_OD_NUMBER);
    if (e != d->buf + d->idx[tok + 1] && !YYJSON_OD_WS(*e))
        return yyjson_od__fail(d, YYJSON_OD_NUMBER);
    return true;
}

size_t yyjson_od_count(yyjson_od_val *v) {
    yyjson_od_doc *d = v->doc;
    uint32_t ip = v->tok, n = d->ntok;
    int32_t depth = 1;
    size_t cnt = 1;
    unsigned char c, close;
    if (d->err) return 0;
    if (ip != d->ip) {
        yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
        return 0;
    }
    c = YYJSON_OD_CH(d, ip);
    if (c != '{' && c != '[') {
        yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
        return 0;
    }
    close = (unsigned char)(c + 2);
    ip++;
    if (ip >= n) { yyjson_od__fail(d, YYJSON_OD_TAPE); return 0; }
    if (YYJSON_OD_CH(d, ip) == close) return 0;
    for (; ip < n; ip++) {
        c = YYJSON_OD_CH(d, ip);
        depth += yyjson_od__dtab[c];
        if (depth == 1 && c == ',') cnt++;
        else if (depth == 0) return cnt;
    }
    yyjson_od__fail(d, YYJSON_OD_TAPE);
    return 0;
}

bool yyjson_od_raw_json(yyjson_od_val *v, yyjson_od_str *out) {
    yyjson_od_doc *d = v->doc;
    int pos = yyjson_od__pos(v);
    uint32_t s, e;
    unsigned char c;
    if (!pos) return false;
    s = d->idx[v->tok];
    c = d->buf[s];
    if (c == '{' || c == '[') {
        if (pos != 1) return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
        if (!yyjson_od__skip_val(d, d->depth)) return false;
        e = d->idx[d->ip - 1] + 1;
    } else if (c == '"') {
        e = yyjson_od__str_end(d, v->tok);
        if (!e) return yyjson_od__fail(d, YYJSON_OD_TAPE);
        e++;
        if (pos == 1) d->ip++;
    } else {
        e = d->idx[v->tok + 1];
        while (e > s && YYJSON_OD_WS(d->buf[e - 1])) e--;
        if (pos == 1) d->ip++;
    }
    out->ptr = (const char *)d->buf + s;
    out->len = (size_t)(e - s);
    return true;
}

const char *yyjson_od_err_msg(int code) {
    switch (code) {
        case YYJSON_OD_OK: return "no error";
        case YYJSON_OD_MEMORY: return "memory allocation failed";
        case YYJSON_OD_EMPTY: return "no JSON value in input";
        case YYJSON_OD_INVALID:
            return "invalid UTF-8, unclosed string or control character";
        case YYJSON_OD_TAPE: return "malformed structure";
        case YYJSON_OD_INCORRECT_TYPE: return "incorrect value type";
        case YYJSON_OD_NUMBER: return "invalid number";
        case YYJSON_OD_STRING: return "invalid escape sequence";
        case YYJSON_OD_LITERAL: return "invalid literal";
        case YYJSON_OD_OUT_OF_ORDER: return "value used out of order";
        case YYJSON_OD_TOO_LARGE: return "input too large";
        default: return "unknown error";
    }
}

bool yyjson_od_doc_finish(yyjson_od_doc *d) {
    if (d->err) return false;
    if (d->depth > 0) {
        if (!yyjson_od__skip_to(d, 0)) return false;
    } else if (d->ip == 0 && d->ntok > 0) {
        if (!yyjson_od__skip_val(d, 0)) return false;
    }
    if (d->depth != 0 || d->ip != d->ntok)
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    return true;
}
