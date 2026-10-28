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

/* zero-padded private copy of the input in p->tmp */
static bool od_tmp(yyjson_od_parser *p, const char *buf, size_t len) {
    if (p->tmp_cap < len + YYJSON_OD_PADDING) {
        if (p->tmp) p->alc.free(p->alc.ctx, p->tmp);
        p->tmp_cap = 0;
        p->tmp = (unsigned char *)p->alc.malloc(p->alc.ctx,
                                                len + YYJSON_OD_PADDING);
        if (!p->tmp) return false;
        p->tmp_cap = len + YYJSON_OD_PADDING;
    }
    memcpy(p->tmp, buf, len);
    memset(p->tmp + len, 0, YYJSON_OD_PADDING);
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
    yyjson_od_doc_rewind(d);
    c0 = d->buf[p->idx[0]];
    if (c0 == '{' || c0 == '[') {
        /* every scalar is then followed by a structural byte inside the
           input, which keeps number parsing in bounds */
        cl = d->buf[p->idx[ntok - 1]];
        if (cl != '}' && cl != ']') OD_RET(YYJSON_OD_TAPE);
        /* point the sentinels at a byte that no iterator step accepts (not
           a value start, separator or closing bracket), so steps need no
           bounds check: found within the first bytes of nearly any input
           (whitespace, a letter of a key, a '.'); otherwise use a copy
           with one space appended */
        {
            size_t i;
            for (i = 0; i < len; i++) {
                unsigned char c = d->buf[i];
                if (!yyjson_od__vtab[c] && c != ',' && c != ':' && c != '}' &&
                    c != ']')
                    break;
            }
            if (i == len) {
                if (!od_tmp(p, buf, len)) OD_RET(YYJSON_OD_MEMORY);
                p->tmp[len] = ' ';
                d->buf = p->tmp;
            }
            p->idx[ntok] = p->idx[ntok + 1] = p->idx[ntok + 2] =
                p->idx[ntok + 3] = (uint32_t)i;
        }
    } else {
        /* scalar root: parse a zero-padded private copy */
        if (!od_tmp(p, buf, len)) OD_RET(YYJSON_OD_MEMORY);
        d->buf = p->tmp;
    }
    return d;
}

int yyjson_od_iterate(yyjson_od_parser *p, const char *buf, size_t len,
                      yyjson_od_doc *doc) {
    int code;
    yyjson_od_doc *d = yyjson_od_parse(p, buf, len, &code);
    if (!d) {
        /* a document whose root is no value: every call fails */
        static const uint32_t zidx[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        static const unsigned char junk[YYJSON_OD_PADDING] = {' '};
        memset(doc, 0, sizeof(*doc));
        doc->buf = junk;
        doc->idx = zidx;
        yyjson_od_doc_rewind(doc);
        doc->err = code;
        return code;
    }
    *doc = *d;
    return YYJSON_OD_OK;
}

uint64_t yyjson_od__skip(const unsigned char *buf, const uint32_t *idx,
                         uint32_t n, uint32_t ip, uint32_t depth,
                         uint32_t target) {
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
        if (yyjson_od_unlikely(ip >= n))
            return ip | ((uint64_t)(target + 1) << 32);
        depth = (uint32_t)((int32_t)depth + yyjson_od__dtab[buf[idx[ip]]]);
        ip++;
    }
done:
    if (yyjson_od_unlikely(depth != target)) /* unbalanced brackets */
        return ip | ((uint64_t)(target + 1) << 32);
    return ip | ((uint64_t)depth << 32);
}

bool yyjson_od__key_check(yyjson_od_doc *d, uint32_t s, uint32_t bs,
                          uint32_t e) {
    yyjson_od_str tmp;
    unsigned char *save = d->scr;
    if (!yyjson_od__str_slow(d, s, bs, e, &tmp)) return false;
    d->scr = save; /* decoded form is discarded */
    return true;
}

uint64_t yyjson_od__resume(const unsigned char *buf, const uint32_t *idx,
                           uint32_t n, const yyjson_od__ph *ph,
                           uint32_t end_of, uint32_t end_ip, uint32_t tok) {
    uint32_t k = 0, c = tok, s;
    uint64_t r;
    /* hints point strictly forward, so the walk ends */
    while (k < YYJSON_OD_PH_SIZE && c != end_of &&
           ph[c % YYJSON_OD_PH_SIZE].of == c &&
           ph[c % YYJSON_OD_PH_SIZE].ip > c) {
        k++;
        c = ph[c % YYJSON_OD_PH_SIZE].ip;
    }
    if (c == end_of) {
        s = end_ip;
    } else {
        r = yyjson_od__skip(buf, idx, n, c + 1, 1, 0);
        if ((uint32_t)(r >> 32) != 0) return r;
        s = (uint32_t)r;
    }
    while (k--) {
        r = yyjson_od__skip(buf, idx, n, s, 1, 0);
        if ((uint32_t)(r >> 32) != 0) return r;
        s = (uint32_t)r;
    }
    return s;
}

/* token containing byte offset `off` (for error positions) */
static uint32_t od_tok_at(const yyjson_od_doc *d, uint32_t off) {
    uint32_t lo = 0, hi = d->ntok;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d->idx[mid] <= off) lo = mid;
        else hi = mid;
    }
    return lo;
}

bool yyjson_od__str_slow(yyjson_od_doc *d, uint32_t s, uint32_t bs,
                         uint32_t e, yyjson_od_str *out) {
    unsigned char *dst = d->scr, *end;
    if ((size_t)(d->scr_end - dst) < (size_t)(e - s) + 40)
        return yyjson_od__fail(d, YYJSON_OD_MEMORY, od_tok_at(d, s));
    end = yyjson_od__str_esc2(d->buf + s, d->buf + (bs < e ? bs : e),
                              d->buf + e, dst);
    if (!end) return yyjson_od__fail(d, YYJSON_OD_STRING, od_tok_at(d, s));
    out->ptr = (const char *)dst;
    out->len = (size_t)(end - dst);
    d->scr = end + 1;
    return true;
}

yyjson_od__nr yyjson_od__numv(const unsigned char *p,
                              const unsigned char *nx,
                              const unsigned char *eof) {
    yyjson_od__nr r;
    yyjson_val v;
    const unsigned char *e;
    unsigned char c = *p;
    r.tag = 0;
    r.u = 0;
    r.err = 0;
    if (c != '-' && (unsigned char)(c - '0') > 9) {
        r.err = YYJSON_OD_INCORRECT_TYPE;
        return r;
    }
    e = yyjson_od__num(p, eof, &v);
    if (!e || (e != nx && !YYJSON_OD_WS(*e))) {
        r.err = YYJSON_OD_NUMBER;
        return r;
    }
    r.tag = v.tag;
    r.u = v.uni.u64;
    return r;
}

size_t yyjson_od_count(yyjson_od_val *v) {
    yyjson_od_doc *d = v->doc;
    uint32_t ip = v->tok, n = d->ntok;
    int32_t depth = 1;
    size_t cnt = 1;
    unsigned char c, close;
    c = YYJSON_OD_CH(d, ip);
    if (c != '{' && c != '[') {
        yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE, ip);
        return 0;
    }
    close = (unsigned char)(c + 2);
    ip++;
    if (YYJSON_OD_CH(d, ip) == close) return 0;
    for (; ip < n; ip++) {
        c = YYJSON_OD_CH(d, ip);
        depth += yyjson_od__dtab[c];
        if (depth == 1 && c == ',') cnt++;
        else if (depth == 0) return cnt;
    }
    yyjson_od__fail(d, YYJSON_OD_TAPE, v->tok);
    return 0;
}

bool yyjson_od_raw_json(yyjson_od_val *v, yyjson_od_str *out) {
    yyjson_od_doc *d = v->doc;
    uint32_t s, e, t;
    unsigned char c;
    s = d->idx[v->tok];
    c = d->buf[s];
    if (c == '{' || c == '[') {
        t = yyjson_od__after(d, v->tok);
        if (!t) return false;
        d->end_of = v->tok; /* the parent steps over it without a skip */
        d->end_ip = t;
        e = d->idx[t - 1] + 1;
    } else if (c == '"') {
        e = yyjson_od__str_end(d, v->tok);
        if (!e) return yyjson_od__fail(d, YYJSON_OD_TAPE, v->tok);
        e++;
    } else {
        e = d->idx[v->tok + 1];
        while (e > s && YYJSON_OD_WS(d->buf[e - 1])) e--;
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
    uint32_t e = 1;
    unsigned char c;
    if (d->err) return false;
    c = YYJSON_OD_CH(d, 0);
    if (c == '{' || c == '[') {
        e = yyjson_od__after(d, 0);
        if (!e) return false;
    }
    if (e != d->ntok) return yyjson_od__fail(d, YYJSON_OD_TAPE, e);
    return true;
}
