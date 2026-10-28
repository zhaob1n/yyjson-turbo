/*==============================================================================
 * yyjson_ondemand.c - EXPERIMENTAL On Demand reader prototype.
 * See yyjson_ondemand.h.
 *============================================================================*/
#include "yyjson_ondemand.h"

#include <stdlib.h>

struct yyjson_od_parser {
    yyjson_alc alc;
    uint32_t *idx;
    uint64_t *spc;
    unsigned char *scr;
    unsigned char *tmp;     /* padded copy for scalar roots */
    size_t cap;             /* input capacity of idx/spc/scr */
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
    if (p->spc) p->alc.free(p->alc.ctx, p->spc);
    if (p->scr) p->alc.free(p->alc.ctx, p->scr);
    p->idx = NULL;
    p->spc = NULL;
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
    p->spc = (uint64_t *)p->alc.malloc(p->alc.ctx,
                                       (cap / 64 + 2) * sizeof(uint64_t));
    p->scr = (unsigned char *)p->alc.malloc(p->alc.ctx, cap + 64);
    if (!p->idx || !p->spc || !p->scr) {
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
    if (!yyjson_od__index((const unsigned char *)buf, len, p->idx, p->spc,
                          &ntok)) {
        OD_RET(YYJSON_OD_INVALID);
    }
    if (ntok == 0) OD_RET(YYJSON_OD_EMPTY);
    d = &p->doc;
    d->buf = (const unsigned char *)buf;
    d->idx = p->idx;
    d->spc = p->spc;
    d->scr = p->scr;
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

bool yyjson_od__fail(yyjson_od_doc *d, int code) {
    if (!d->err) {
        d->err = code;
        d->err_tok = d->ip;
    }
    return false;
}

bool yyjson_od__skip_to(yyjson_od_doc *d, uint32_t target) {
    const unsigned char *buf = d->buf;
    const uint32_t *idx = d->idx;
    uint32_t ip = d->ip, n = d->ntok, depth = d->depth;
    while (depth > target) {
        if (yyjson_od_unlikely(ip >= n)) {
            d->ip = ip;
            d->depth = depth;
            return yyjson_od__fail(d, YYJSON_OD_TAPE);
        }
        depth = (uint32_t)((int32_t)depth + yyjson_od__dtab[buf[idx[ip]]]);
        ip++;
    }
    d->ip = ip;
    d->depth = depth;
    return true;
}

bool yyjson_od__key_check(yyjson_od_doc *d, uint32_t tok) {
    yyjson_od_str tmp;
    unsigned char *save = d->scr;
    if (!yyjson_od__str_slow(d, tok, &tmp)) return false;
    d->scr = save; /* decoded form is discarded */
    return true;
}

bool yyjson_od__str_slow(yyjson_od_doc *d, uint32_t tok, yyjson_od_str *out) {
    uint32_t s = d->idx[tok] + 1, e = d->idx[tok + 1];
    unsigned char *dst = d->scr, *end;
    if ((size_t)(d->scr_end - dst) < (size_t)(e - s) + 8)
        return yyjson_od__fail(d, YYJSON_OD_MEMORY); /* only on re-reads */
    end = yyjson_od__str_esc(d->buf, d->buf + s, d->buf + e, d->spc, dst);
    if (!end) return yyjson_od__fail(d, YYJSON_OD_STRING);
    out->ptr = (const char *)dst;
    out->len = (size_t)(end - dst);
    d->scr = end + 1;
    return true;
}

bool yyjson_od__num_tok(yyjson_od_doc *d, uint32_t tok, yyjson_val *out) {
    const unsigned char *p = d->buf + d->idx[tok], *e;
    unsigned char c = *p;
    if (c != '-' && (unsigned char)(c - '0') > 9)
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
    e = yyjson_od__num(p, d->buf + d->len, out);
    if (!e) return yyjson_od__fail(d, YYJSON_OD_NUMBER);
    if (e != d->buf + d->idx[tok + 1]) {
        c = *e;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return yyjson_od__fail(d, YYJSON_OD_NUMBER);
    }
    return true;
}

bool yyjson_od_doc_finish(yyjson_od_doc *d) {
    if (d->err) return false;
    if (!yyjson_od__finish(d, 0, 0)) return false;
    if (d->depth != 0 || d->ip != d->ntok)
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    return true;
}
