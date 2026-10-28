/*==============================================================================
 * yyjson_ondemand.h - EXPERIMENTAL On Demand (lazy, forward-only) reader.
 *
 * Prototype. Reuses the stage-1 SIMD structural index of the two-stage reader
 * (requires a build with stage1 and AVX2 enabled). Values are parsed only when
 * a getter is called; unread values are skipped through the structural index.
 *
 * Model (similar to simdjson::ondemand):
 *  - The input is not copied or modified, but YYJSON_OD_PADDING bytes after
 *    `len` must be readable (their content does not matter).
 *  - A document has a single forward-only cursor. A yyjson_od_val is a handle
 *    to a value position; containers must be iterated in document order.
 *    Scalars already passed may be re-read; containers may not.
 *  - Errors are sticky in the document: after the first error all calls
 *    return false. Check yyjson_od_doc_err() after an iteration ends.
 *  - Strings without escapes are returned as views into the input (not NUL
 *    terminated); escaped strings are decoded into the parser's scratch
 *    buffer. Both stay valid until the next yyjson_od_parse().
 *  - Object keys are compared raw (escaped form), like simdjson.
 *============================================================================*/
#ifndef YYJSON_ONDEMAND_H
#define YYJSON_ONDEMAND_H

#include "yyjson.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YYJSON_OD_PADDING 64

typedef enum yyjson_od_code {
    YYJSON_OD_OK = 0,
    YYJSON_OD_MEMORY,        /* allocation failed */
    YYJSON_OD_EMPTY,         /* no JSON value in input */
    YYJSON_OD_INVALID,       /* unclosed string or invalid UTF-8 */
    YYJSON_OD_TAPE,          /* malformed structure */
    YYJSON_OD_INCORRECT_TYPE,
    YYJSON_OD_NUMBER,        /* invalid number */
    YYJSON_OD_STRING,        /* invalid escape or control character */
    YYJSON_OD_LITERAL,       /* invalid true/false/null */
    YYJSON_OD_OUT_OF_ORDER,  /* value used after the cursor moved past it */
    YYJSON_OD_TOO_LARGE      /* input too large for 32-bit offsets */
} yyjson_od_code;

typedef enum yyjson_od_type {
    YYJSON_OD_TYPE_NONE = 0,
    YYJSON_OD_TYPE_OBJ, YYJSON_OD_TYPE_ARR, YYJSON_OD_TYPE_STR,
    YYJSON_OD_TYPE_NUM, YYJSON_OD_TYPE_BOOL, YYJSON_OD_TYPE_NULL
} yyjson_od_type;

typedef struct yyjson_od_doc {
    const unsigned char *buf;   /* input */
    const uint32_t *idx;        /* structural index (offsets into buf) */
    const uint64_t *spc;        /* per-64-byte masks: backslash | control */
    unsigned char *scr;         /* scratch cursor for unescaped strings */
    unsigned char *scr_end;     /* end of the scratch buffer */
    size_t len;
    uint32_t ntok;              /* number of tokens */
    uint32_t ip;                /* cursor: next token to consume */
    uint32_t depth;             /* current container depth */
    uint32_t err_tok;           /* token at which the first error happened */
    int err;                    /* yyjson_od_code */
} yyjson_od_doc;

typedef struct yyjson_od_parser yyjson_od_parser;

typedef struct yyjson_od_val {
    yyjson_od_doc *doc;
    uint32_t tok;               /* first token of the value */
} yyjson_od_val;

typedef struct yyjson_od_iter {
    yyjson_od_doc *doc;
    uint32_t depth;             /* depth of the members */
    uint32_t vtok;              /* first token of the last returned member */
    uint32_t state;             /* 0: before first, 1: in member, 2: done */
} yyjson_od_iter;

typedef struct yyjson_od_str {
    const char *ptr;
    size_t len;
} yyjson_od_str;

/*------------------------------------------------------------------------------
 * Parser / document
 *----------------------------------------------------------------------------*/

/** Create a parser (reusable; buffers grow as needed). NULL alc: default. */
yyjson_api yyjson_od_parser *yyjson_od_parser_new(const yyjson_alc *alc);
yyjson_api void yyjson_od_parser_free(yyjson_od_parser *p);

/** Index `buf` (len bytes + YYJSON_OD_PADDING readable bytes). Returns a
    document owned by the parser, valid until the next call; NULL on error
    (the code is stored in *code when code is not NULL). */
yyjson_api yyjson_od_doc *yyjson_od_parse(yyjson_od_parser *p,
                                          const char *buf, size_t len,
                                          int *code);

/** Error code of the document (0: no error). */
yyjson_api_inline int yyjson_od_doc_err(const yyjson_od_doc *d) {
    return d->err;
}

/** Byte offset of the first error (approximate: start of the token). */
yyjson_api_inline size_t yyjson_od_doc_err_pos(const yyjson_od_doc *d) {
    return d->err_tok < d->ntok ? (size_t)d->idx[d->err_tok] : d->len;
}

/** Root value handle. */
yyjson_api_inline yyjson_od_val yyjson_od_doc_root(yyjson_od_doc *d) {
    yyjson_od_val v;
    v.doc = d;
    v.tok = 0;
    return v;
}

/** True if the whole document has been consumed (no trailing content).
    Skips whatever of the root was not read. */
yyjson_api bool yyjson_od_doc_finish(yyjson_od_doc *d);

/*------------------------------------------------------------------------------
 * Internal helpers
 *----------------------------------------------------------------------------*/

/* +1 for '{' '[', -1 for '}' ']', 0 otherwise */
extern const signed char yyjson_od__dtab[256];
/* nonzero for bytes that may start a value */
extern const unsigned char yyjson_od__vtab[256];

yyjson_api bool yyjson_od__index(const unsigned char *buf, size_t len,
                                 uint32_t *idx, uint64_t *spc, size_t *ntok);
yyjson_api unsigned char *yyjson_od__str_esc(const unsigned char *hdr,
                                             const unsigned char *beg,
                                             const unsigned char *end,
                                             const uint64_t *spc,
                                             unsigned char *out);
yyjson_api const unsigned char *yyjson_od__num(const unsigned char *cur,
                                               const unsigned char *eof,
                                               yyjson_val *val);
yyjson_api bool yyjson_od__fail(yyjson_od_doc *d, int code);
yyjson_api bool yyjson_od__skip_to(yyjson_od_doc *d, uint32_t target);
yyjson_api bool yyjson_od__key_check(yyjson_od_doc *d, uint32_t tok);
yyjson_api bool yyjson_od__str_slow(yyjson_od_doc *d, uint32_t tok,
                                    yyjson_od_str *out);
yyjson_api bool yyjson_od__num_tok(yyjson_od_doc *d, uint32_t tok,
                                   yyjson_val *out);

#define YYJSON_OD_CH(d, t) ((d)->buf[(d)->idx[t]])

#if defined(__GNUC__) || defined(__clang__)
#   define yyjson_od_likely(x) __builtin_expect(!!(x), 1)
#   define yyjson_od_unlikely(x) __builtin_expect(!!(x), 0)
#else
#   define yyjson_od_likely(x) (x)
#   define yyjson_od_unlikely(x) (x)
#endif

/* Validate that handle `v` may be read. Returns 1 if it is the current token
   (the read must advance the cursor), 2 if it was already passed (re-read of
   a scalar, no cursor change), 0 on error. */
yyjson_api_inline int yyjson_od__pos(const yyjson_od_val *v) {
    yyjson_od_doc *d = v->doc;
    if (yyjson_od_unlikely(d->err)) return 0;
    if (yyjson_od_likely(v->tok == d->ip)) return 1;
    if (v->tok < d->ip) return 2;
    yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
    return 0;
}

/* Bring the cursor past the member starting at `vtok` of an iterator whose
   members are at `depth`, skipping whatever of it was not read. */
yyjson_api_inline bool yyjson_od__finish(yyjson_od_doc *d, uint32_t depth,
                                         uint32_t vtok) {
    if (yyjson_od_likely(d->depth == depth)) {
        if (d->ip == vtok) { /* untouched member */
            unsigned char c = YYJSON_OD_CH(d, vtok);
            if (c == '"') { d->ip += 2; return true; }
            if (c == '{' || c == '[') {
                d->ip++;
                d->depth++;
                return yyjson_od__skip_to(d, depth);
            }
            d->ip++;
        }
        return true;
    }
    if (d->depth > depth) return yyjson_od__skip_to(d, depth);
    return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
}

/* Shared begin for objects and arrays. */
yyjson_api_inline bool yyjson_od__begin(yyjson_od_val *v, yyjson_od_iter *it,
                                        unsigned char open,
                                        unsigned char close) {
    yyjson_od_doc *d = v->doc;
    uint32_t ip;
    it->doc = d;
    it->state = 2;
    if (yyjson_od_unlikely(d->err)) return false;
    if (yyjson_od_unlikely(v->tok != d->ip))
        return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
    ip = v->tok;
    if (yyjson_od_unlikely(YYJSON_OD_CH(d, ip) != open))
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
    ip++;
    if (yyjson_od_unlikely(ip >= d->ntok))
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    if (YYJSON_OD_CH(d, ip) == close) { /* empty */
        d->ip = ip + 1;
        return true;
    }
    d->ip = ip;
    d->depth++;
    it->depth = d->depth;
    it->state = 0;
    return true;
}

/* Move past the separator after the current member. Returns 1 when a next
   member follows, 0 at the end or on error. */
yyjson_api_inline int yyjson_od__sep(yyjson_od_iter *it, unsigned char close) {
    yyjson_od_doc *d = it->doc;
    unsigned char c;
    if (it->state == 0) { it->state = 1; return 1; }
    if (it->state == 2 || yyjson_od_unlikely(d->err)) return 0;
    if (yyjson_od_unlikely(!yyjson_od__finish(d, it->depth, it->vtok)))
        return 0;
    if (yyjson_od_unlikely(d->ip >= d->ntok)) {
        yyjson_od__fail(d, YYJSON_OD_TAPE);
        return 0;
    }
    c = YYJSON_OD_CH(d, d->ip);
    d->ip++;
    if (yyjson_od_likely(c == ',')) return 1;
    it->state = 2;
    if (yyjson_od_likely(c == close)) { d->depth--; return 0; }
    yyjson_od__fail(d, YYJSON_OD_TAPE);
    return 0;
}

/*------------------------------------------------------------------------------
 * Objects and arrays
 *----------------------------------------------------------------------------*/

/** Start iterating an object value. False if not an object (or error). */
yyjson_api_inline bool yyjson_od_get_obj(yyjson_od_val *v, yyjson_od_iter *it) {
    return yyjson_od__begin(v, it, '{', '}');
}

/** Start iterating an array value. False if not an array (or error). */
yyjson_api_inline bool yyjson_od_get_arr(yyjson_od_val *v, yyjson_od_iter *it) {
    return yyjson_od__begin(v, it, '[', ']');
}

yyjson_api_inline bool yyjson_od__has_spc(const uint64_t *spc, size_t a,
                                          size_t b) {
    size_t wa, wb;
    uint64_t ma, mb;
    if (a >= b) return false;
    wa = a >> 6; wb = (b - 1) >> 6;
    ma = ~(uint64_t)0 << (a & 63);
    mb = ~(uint64_t)0 >> (63 - ((b - 1) & 63));
    if (yyjson_od_likely(wa == wb)) return (spc[wa] & ma & mb) != 0;
    if (spc[wa] & ma) return true;
    for (wa++; wa < wb; wa++) if (spc[wa]) return true;
    return (spc[wb] & mb) != 0;
}

/** Next object member: raw key (escaped form, not NUL-terminated) and value.
    Unread parts of the previous value are skipped. False at the end. */
yyjson_api_inline bool yyjson_od_obj_next(yyjson_od_iter *it,
                                          yyjson_od_str *key,
                                          yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    uint32_t ip;
    if (!yyjson_od__sep(it, '}')) return false;
    ip = d->ip;
    if (yyjson_od_unlikely(ip + 3 >= d->ntok ||
                           YYJSON_OD_CH(d, ip) != '"' ||
                           YYJSON_OD_CH(d, ip + 2) != ':' ||
                           !yyjson_od__vtab[YYJSON_OD_CH(d, ip + 3)])) {
        it->state = 2;
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    }
    key->ptr = (const char *)d->buf + d->idx[ip] + 1;
    key->len = (size_t)(d->idx[ip + 1] - d->idx[ip] - 1);
    /* keys stay raw, but escapes / control chars must still be valid */
    if (yyjson_od_unlikely(yyjson_od__has_spc(d->spc, d->idx[ip] + 1,
                                              d->idx[ip + 1])) &&
        !yyjson_od__key_check(d, ip)) {
        it->state = 2;
        return false;
    }
    ip += 3;
    d->ip = ip;
    it->vtok = ip;
    val->doc = d;
    val->tok = ip;
    return true;
}

/** Find the next member named `key` (raw compare), continuing from the
    current position (no wrap-around). False if not found. */
yyjson_api_inline bool yyjson_od_obj_find(yyjson_od_iter *it, const char *key,
                                          size_t klen, yyjson_od_val *val) {
    yyjson_od_str k;
    k.ptr = NULL;
    k.len = 0;
    while (yyjson_od_obj_next(it, &k, val)) {
        if (k.len == klen && memcmp(k.ptr, key, klen) == 0) return true;
    }
    return false;
}

/** Next array element. False at the end. */
yyjson_api_inline bool yyjson_od_arr_next(yyjson_od_iter *it,
                                          yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    uint32_t ip;
    if (!yyjson_od__sep(it, ']')) return false;
    ip = d->ip;
    if (yyjson_od_unlikely(ip >= d->ntok ||
                           !yyjson_od__vtab[YYJSON_OD_CH(d, ip)])) {
        it->state = 2;
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    }
    it->vtok = ip;
    val->doc = d;
    val->tok = ip;
    return true;
}

/*------------------------------------------------------------------------------
 * Scalars
 *----------------------------------------------------------------------------*/

/** Type of a value from its first byte (not validated). */
yyjson_api_inline yyjson_od_type yyjson_od_get_type(const yyjson_od_val *v) {
    switch (YYJSON_OD_CH(v->doc, v->tok)) {
        case '{': return YYJSON_OD_TYPE_OBJ;
        case '[': return YYJSON_OD_TYPE_ARR;
        case '"': return YYJSON_OD_TYPE_STR;
        case 't': case 'f': return YYJSON_OD_TYPE_BOOL;
        case 'n': return YYJSON_OD_TYPE_NULL;
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return YYJSON_OD_TYPE_NUM;
        default: return YYJSON_OD_TYPE_NONE;
    }
}

/** Get a string (unescaped). False if not a string or invalid. */
yyjson_api_inline bool yyjson_od_get_str(yyjson_od_val *v, yyjson_od_str *out) {
    yyjson_od_doc *d = v->doc;
    int pos = yyjson_od__pos(v);
    uint32_t s, e;
    if (yyjson_od_unlikely(!pos)) return false;
    if (yyjson_od_unlikely(YYJSON_OD_CH(d, v->tok) != '"'))
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
    s = d->idx[v->tok] + 1;
    e = d->idx[v->tok + 1];
    if (yyjson_od_likely(!yyjson_od__has_spc(d->spc, s, e))) {
        out->ptr = (const char *)d->buf + s;
        out->len = (size_t)(e - s);
    } else if (!yyjson_od__str_slow(d, v->tok, out)) {
        return false;
    }
    if (pos == 1) d->ip += 2;
    return true;
}

/* number into a yyjson_val (SINT/UINT/REAL), advancing the cursor */
yyjson_api_inline bool yyjson_od__getnum(yyjson_od_val *v, yyjson_val *n) {
    int pos = yyjson_od__pos(v);
    if (yyjson_od_unlikely(!pos)) return false;
    if (yyjson_od_unlikely(!yyjson_od__num_tok(v->doc, v->tok, n)))
        return false;
    if (pos == 1) v->doc->ip++;
    return true;
}

/** Get a number as yyjson_val (subtype SINT, UINT or REAL; big integers
    are read as REAL, as in yyjson's default reader). */
yyjson_api_inline bool yyjson_od_get_num(yyjson_od_val *v, yyjson_val *out) {
    return yyjson_od__getnum(v, out);
}

/** Get an integer that fits int64_t. */
yyjson_api_inline bool yyjson_od_get_sint(yyjson_od_val *v, int64_t *out) {
    yyjson_val n;
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(yyjson_is_sint(&n) ||
                         (yyjson_is_uint(&n) &&
                          n.uni.u64 <= (uint64_t)INT64_MAX))) {
        *out = n.uni.i64;
        return true;
    }
    return yyjson_od__fail(v->doc, YYJSON_OD_INCORRECT_TYPE);
}

/** Get a non-negative integer that fits uint64_t. */
yyjson_api_inline bool yyjson_od_get_uint(yyjson_od_val *v, uint64_t *out) {
    yyjson_val n;
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(yyjson_is_uint(&n))) {
        *out = n.uni.u64;
        return true;
    }
    return yyjson_od__fail(v->doc, YYJSON_OD_INCORRECT_TYPE);
}

/** Get any number as double. */
yyjson_api_inline bool yyjson_od_get_real(yyjson_od_val *v, double *out) {
    yyjson_val n;
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(yyjson_is_real(&n))) *out = n.uni.f64;
    else if (yyjson_is_sint(&n)) *out = (double)n.uni.i64;
    else *out = (double)n.uni.u64;
    return true;
}

/* literal of `n` bytes at token `tok`, followed by a delimiter */
yyjson_api_inline bool yyjson_od__lit(yyjson_od_doc *d, uint32_t tok,
                                      const char *lit, size_t n) {
    const unsigned char *p = d->buf + d->idx[tok];
    unsigned char c;
    if (memcmp(p, lit, n) != 0) return false;
    if (p + n == d->buf + d->idx[tok + 1]) return true;
    c = p[n];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/** Get a bool. */
yyjson_api_inline bool yyjson_od_get_bool(yyjson_od_val *v, bool *out) {
    yyjson_od_doc *d = v->doc;
    int pos = yyjson_od__pos(v);
    unsigned char c;
    if (yyjson_od_unlikely(!pos)) return false;
    c = YYJSON_OD_CH(d, v->tok);
    if (c == 't') {
        if (yyjson_od_unlikely(!yyjson_od__lit(d, v->tok, "true", 4)))
            return yyjson_od__fail(d, YYJSON_OD_LITERAL);
        *out = true;
    } else if (c == 'f') {
        if (yyjson_od_unlikely(!yyjson_od__lit(d, v->tok, "false", 5)))
            return yyjson_od__fail(d, YYJSON_OD_LITERAL);
        *out = false;
    } else {
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
    }
    if (pos == 1) d->ip++;
    return true;
}

/** If the value is null, consume it and return true. Otherwise return false
    without consuming it (and without error for non-null values). */
yyjson_api_inline bool yyjson_od_is_null(yyjson_od_val *v) {
    yyjson_od_doc *d = v->doc;
    int pos = yyjson_od__pos(v);
    if (yyjson_od_unlikely(!pos)) return false;
    if (YYJSON_OD_CH(d, v->tok) != 'n') return false;
    if (yyjson_od_unlikely(!yyjson_od__lit(d, v->tok, "null", 4)))
        return yyjson_od__fail(d, YYJSON_OD_LITERAL);
    if (pos == 1) d->ip++;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* YYJSON_ONDEMAND_H */
