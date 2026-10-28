/*==============================================================================
 * yyjson_ondemand.h - On Demand (lazy, forward-only) JSON reader for yyjson.
 *
 * The reader builds a SIMD structural index of the input (one pass, requires
 * a build with the stage-1 reader and AVX2) and then parses values only when a
 * getter is called. Values that are never read are skipped through the index.
 * The programming model follows simdjson::ondemand:
 *
 *  - Input: the buffer is neither copied nor modified, but YYJSON_OD_PADDING
 *    bytes after `len` must be readable (their content does not matter).
 *  - Cursor: a document has one forward-only cursor. A yyjson_od_val is a
 *    lightweight handle {doc, token}. Containers must be consumed in document
 *    order; a scalar that the cursor already passed may be re-read, a
 *    container may not (YYJSON_OD_OUT_OF_ORDER). yyjson_od_doc_rewind()
 *    restarts the iteration from the root.
 *  - Errors are sticky: after the first error every call fails and returns
 *    false. Check yyjson_od_doc_err() at the end (or after any false).
 *    "Not found" (yyjson_od_obj_find) and "not null" (yyjson_od_is_null) are
 *    not errors.
 *  - Validation: the whole input is checked for UTF-8, unclosed strings and
 *    unescaped control characters while indexing. Everything else (escapes,
 *    numbers, literals, structure) is checked when it is read or iterated.
 *    Content that is skipped is only checked for bracket balance, so a
 *    document with an invalid number inside a skipped value is accepted.
 *    yyjson_od_doc_finish() checks that nothing follows the root.
 *  - Strings without escapes are returned as views into the input (not NUL
 *    terminated). Escaped strings are decoded into the parser's scratch
 *    buffer. Both stay valid until the next yyjson_od_parse() or rewind.
 *  - Object keys are reported and compared raw (escaped form), as in
 *    simdjson. Keys passed by yyjson_od_obj_find() are not validated; keys
 *    returned by yyjson_od_obj_next() are (escapes must be well formed).
 *  - Numbers are parsed exactly like the yyjson DOM reader with default
 *    flags: integers become UINT/SINT, integers that do not fit 64 bits and
 *    all numbers with a fraction or exponent become REAL (correctly rounded).
 *============================================================================*/
#ifndef YYJSON_ONDEMAND_H
#define YYJSON_ONDEMAND_H

#include "yyjson.h"
#include <string.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of readable bytes required after the end of the input. */
#define YYJSON_OD_PADDING 64

/** Error codes (sticky in the document). */
typedef enum yyjson_od_code {
    YYJSON_OD_OK = 0,
    YYJSON_OD_MEMORY,         /**< allocation failed */
    YYJSON_OD_EMPTY,          /**< no JSON value in the input */
    YYJSON_OD_INVALID,        /**< invalid UTF-8, unclosed string or control
                                   character in a string */
    YYJSON_OD_TAPE,           /**< malformed structure */
    YYJSON_OD_INCORRECT_TYPE, /**< value has another type than requested */
    YYJSON_OD_NUMBER,         /**< invalid number, or out of range for the
                                   requested integer type */
    YYJSON_OD_STRING,         /**< invalid escape sequence */
    YYJSON_OD_LITERAL,        /**< invalid true/false/null */
    YYJSON_OD_OUT_OF_ORDER,   /**< value used after the cursor moved past */
    YYJSON_OD_TOO_LARGE       /**< input too large (32-bit offsets) */
} yyjson_od_code;

/** Value types, derived from the first byte of a value. */
typedef enum yyjson_od_type {
    YYJSON_OD_TYPE_NONE = 0,
    YYJSON_OD_TYPE_OBJ, YYJSON_OD_TYPE_ARR, YYJSON_OD_TYPE_STR,
    YYJSON_OD_TYPE_NUM, YYJSON_OD_TYPE_BOOL, YYJSON_OD_TYPE_NULL
} yyjson_od_type;

/** A document: the indexed input and its cursor. Fields are internal. */
typedef struct yyjson_od_doc {
    const unsigned char *buf;   /* input */
    const uint32_t *idx;        /* token offsets: structurals, opening quotes
                                   and scalar starts; sentinels at the end */
    const uint64_t *spc;        /* per-64-byte masks of backslashes */
    unsigned char *scr;         /* scratch cursor for unescaped strings */
    unsigned char *scr_beg;     /* start of the scratch buffer */
    unsigned char *scr_end;     /* end of the scratch buffer */
    size_t len;
    uint32_t ntok;              /* number of tokens */
    uint32_t ip;                /* cursor: next token to consume */
    uint32_t depth;             /* current container depth */
    uint32_t err_tok;           /* token at which the first error happened */
    int err;                    /* yyjson_od_code */
} yyjson_od_doc;

/** Reusable parser holding the index and scratch buffers. */
typedef struct yyjson_od_parser yyjson_od_parser;

/** Handle to a value (its first token). Copyable; no ownership. */
typedef struct yyjson_od_val {
    yyjson_od_doc *doc;
    uint32_t tok;
} yyjson_od_val;

/** Object or array iterator. Fields are internal. */
typedef struct yyjson_od_iter {
    yyjson_od_doc *doc;
    uint32_t depth;             /* depth of the members */
    uint32_t vtok;              /* value token of the last returned member */
    uint32_t otok;              /* first token after '{' / '[' */
    uint32_t state;             /* 0: before first, 1: in member, 2: done */
} yyjson_od_iter;

/** A string view (not NUL-terminated). */
typedef struct yyjson_od_str {
    const char *ptr;
    size_t len;
} yyjson_od_str;

/*------------------------------------------------------------------------------
 * Parser and document
 *----------------------------------------------------------------------------*/

/** Create a parser. Buffers grow as needed and are reused across calls.
    @param alc allocator, or NULL for malloc/realloc/free.
    @return the parser, or NULL on allocation failure. */
yyjson_api yyjson_od_parser *yyjson_od_parser_new(const yyjson_alc *alc);

/** Free a parser and every document it produced. NULL is ignored. */
yyjson_api void yyjson_od_parser_free(yyjson_od_parser *p);

/** Index `len` bytes of `buf` (YYJSON_OD_PADDING more bytes must be
    readable). The returned document is owned by the parser and is valid
    until the next call with the same parser.
    @param code receives the error code on failure (may be NULL).
    @return the document, or NULL on error. */
yyjson_api yyjson_od_doc *yyjson_od_parse(yyjson_od_parser *p,
                                          const char *buf, size_t len,
                                          int *code);

/** Error code of the document (YYJSON_OD_OK if none). */
yyjson_api_inline int yyjson_od_doc_err(const yyjson_od_doc *d) {
    return d->err;
}

/** Approximate byte offset of the first error (start of the token). */
yyjson_api_inline size_t yyjson_od_doc_err_pos(const yyjson_od_doc *d) {
    return d->err_tok < d->ntok ? (size_t)d->idx[d->err_tok] : d->len;
}

/** Human-readable message for an error code (static string). */
yyjson_api const char *yyjson_od_err_msg(int code);

/** Handle of the root value. */
yyjson_api_inline yyjson_od_val yyjson_od_doc_root(yyjson_od_doc *d) {
    yyjson_od_val v;
    v.doc = d;
    v.tok = 0;
    return v;
}

/** Restart iteration from the root: resets the cursor, clears the error
    and invalidates strings decoded into the scratch buffer. Handles and
    iterators obtained before must not be used afterwards. */
yyjson_api_inline void yyjson_od_doc_rewind(yyjson_od_doc *d) {
    d->ip = 0;
    d->depth = 0;
    d->err = 0;
    d->err_tok = 0;
    d->scr = d->scr_beg;
}

/** Skip whatever of the root was not read and check that nothing follows
    it. Returns false on any (sticky or new) error. */
yyjson_api bool yyjson_od_doc_finish(yyjson_od_doc *d);

/*------------------------------------------------------------------------------
 * Internal helpers (not part of the API)
 *----------------------------------------------------------------------------*/

extern const signed char yyjson_od__dtab[256];   /* {[: +1, }]: -1 */
extern const unsigned char yyjson_od__vtab[256]; /* may start a value */
extern const unsigned char yyjson_od__ttab[256]; /* may end a number: ws , ] } */

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
yyjson_api bool yyjson_od__f64(uint64_t sig, int32_t exp, bool sign,
                               yyjson_val *val);
yyjson_api bool yyjson_od__fail(yyjson_od_doc *d, int code);
yyjson_api bool yyjson_od__skip_to(yyjson_od_doc *d, uint32_t target);
yyjson_api bool yyjson_od__key_check(yyjson_od_doc *d, uint32_t s,
                                     uint32_t e);
yyjson_api bool yyjson_od__str_slow(yyjson_od_doc *d, uint32_t s, uint32_t e,
                                    yyjson_od_str *out);
yyjson_api bool yyjson_od__num_slow(yyjson_od_doc *d, uint32_t tok,
                                    yyjson_val *out);
yyjson_api bool yyjson_od__find_slow(yyjson_od_iter *it, const char *key,
                                     size_t klen, yyjson_od_val *val);

#define YYJSON_OD_CH(d, t) ((d)->buf[(d)->idx[t]])

#if defined(__GNUC__) || defined(__clang__)
#   define yyjson_od_likely(x) __builtin_expect(!!(x), 1)
#   define yyjson_od_unlikely(x) __builtin_expect(!!(x), 0)
#else
#   define yyjson_od_likely(x) (x)
#   define yyjson_od_unlikely(x) (x)
#endif

#define YYJSON_OD_WS(c) ((c) == ' ' || (c) == '\n' || (c) == '\r' || \
                         (c) == '\t')

/* Validate that handle `v` may be read. 1: it is the current token (the
   read advances the cursor); 2: already passed (scalar re-read); 0: error. */
yyjson_api_inline int yyjson_od__pos(const yyjson_od_val *v) {
    yyjson_od_doc *d = v->doc;
    if (yyjson_od_likely(v->tok == d->ip)) return 1; /* ip is poisoned on error */
    if (d->err) return 0;
    if (v->tok < d->ip) return 2;
    yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
    return 0;
}

/* Offset of the closing quote of the string at token `tok` (0 if broken).
   Only whitespace may lie between a closing quote and the next token. */
yyjson_api_inline uint32_t yyjson_od__str_end(const yyjson_od_doc *d,
                                              uint32_t tok) {
    uint32_t s = d->idx[tok], e = d->idx[tok + 1] - 1;
    while (yyjson_od_unlikely(YYJSON_OD_WS(d->buf[e])) && e > s) e--;
    return (e > s && d->buf[e] == '"') ? e : 0;
}

/* True if a backslash lies in [a, b). */
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

/* Skip the value starting at the cursor (which must be at `depth`). */
yyjson_api_inline bool yyjson_od__skip_val(yyjson_od_doc *d, uint32_t depth) {
    unsigned char c = YYJSON_OD_CH(d, d->ip);
    d->ip++;
    if (c == '{' || c == '[') {
        d->depth++;
        return yyjson_od__skip_to(d, depth);
    }
    return true;
}

/* Bring the cursor past the member whose value starts at `vtok` (members at
   `depth`), skipping whatever of it was not read. */
yyjson_api_inline bool yyjson_od__finish(yyjson_od_doc *d, uint32_t depth,
                                         uint32_t vtok) {
    if (yyjson_od_likely(d->depth == depth)) {
        if (d->ip == vtok) return yyjson_od__skip_val(d, depth);
        return true;
    }
    if (d->depth > depth) return yyjson_od__skip_to(d, depth);
    return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
}

yyjson_api_inline bool yyjson_od__begin(yyjson_od_val *v, yyjson_od_iter *it,
                                        unsigned char open,
                                        unsigned char close) {
    yyjson_od_doc *d = v->doc;
    uint32_t ip = v->tok;
    it->doc = d;
    it->state = 2;
    it->otok = ip + 1;
    it->depth = d->depth + 1;
    it->vtok = 0;
    if (yyjson_od_unlikely(ip != d->ip)) {
        if (d->err) return false;
        return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER);
    }
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
    it->state = 0;
    return true;
}

/* Move past the separator after the current member. 1: a member follows;
   0: end of the container or error. */
yyjson_api_inline int yyjson_od__sep(yyjson_od_iter *it, unsigned char close) {
    yyjson_od_doc *d = it->doc;
    unsigned char c;
    if (it->state != 1) {
        if (it->state == 0) { /* the caller validates the token */
            it->state = 1;
            return 1;
        }
        return 0;
    }
    /* on error depth is 0, so finish fails */
    if (yyjson_od_unlikely(!yyjson_od__finish(d, it->depth, it->vtok)))
        return 0;
    c = YYJSON_OD_CH(d, d->ip); /* sentinels keep this in bounds */
    d->ip++;
    if (yyjson_od_likely(c == ',')) return 1;
    it->state = 2;
    if (yyjson_od_likely(c == close && d->ip <= d->ntok)) {
        d->depth--;
        return 0;
    }
    yyjson_od__fail(d, YYJSON_OD_TAPE);
    return 0;
}

/*------------------------------------------------------------------------------
 * Objects and arrays
 *----------------------------------------------------------------------------*/

/** Start iterating an object. False if the value is not an object or on
    error. An empty object yields an iterator that is already at its end. */
yyjson_api_inline bool yyjson_od_get_obj(yyjson_od_val *v, yyjson_od_iter *it) {
    return yyjson_od__begin(v, it, '{', '}');
}

/** Start iterating an array. False if the value is not an array or on
    error. */
yyjson_api_inline bool yyjson_od_get_arr(yyjson_od_val *v, yyjson_od_iter *it) {
    return yyjson_od__begin(v, it, '[', ']');
}

/** Next object member: its raw key (escaped form, not NUL-terminated) and
    value handle. Unread parts of the previous member are skipped.
    @return false at the end of the object or on error. */
yyjson_api_inline bool yyjson_od_obj_next(yyjson_od_iter *it,
                                          yyjson_od_str *key,
                                          yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    uint32_t ip, s, e;
    if (!yyjson_od__sep(it, '}')) return false;
    ip = d->ip;
    if (yyjson_od_unlikely(ip + 2 >= d->ntok ||
                           YYJSON_OD_CH(d, ip) != '"' ||
                           YYJSON_OD_CH(d, ip + 1) != ':' ||
                           !yyjson_od__vtab[YYJSON_OD_CH(d, ip + 2)])) {
        it->state = 2;
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    }
    s = d->idx[ip] + 1;
    e = yyjson_od__str_end(d, ip);
    if (yyjson_od_unlikely(!e)) {
        it->state = 2;
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    }
    key->ptr = (const char *)d->buf + s;
    key->len = (size_t)(e - s);
    if (yyjson_od_unlikely(yyjson_od__has_spc(d->spc, s, e)) &&
        !yyjson_od__key_check(d, s, e)) {
        it->state = 2;
        return false;
    }
    ip += 2;
    d->ip = ip;
    it->vtok = ip;
    val->doc = d;
    val->tok = ip;
    return true;
}

/** Find the next member whose raw key equals `key` (ordered lookup: the
    search continues from the current member to the end of the object; the
    members passed are skipped). `key` must not contain '"' or '\\' unless
    it is written in the escaped form used by the document.
    @return true if found (`val` is set); false if not found or on error. */
yyjson_api_inline bool yyjson_od_obj_find(yyjson_od_iter *it, const char *key,
                                          size_t klen, yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    const unsigned char *buf = d->buf;
    const uint32_t *idx = d->idx;
    uint32_t ip, s;
    while (yyjson_od__sep(it, '}')) {
        ip = d->ip;
        s = idx[ip] + 1;
        if (yyjson_od_unlikely(buf[s - 1] != '"' ||
                               buf[idx[ip + 1]] != ':' || ip + 2 >= d->ntok)) {
            it->state = 2;
            return yyjson_od__fail(d, YYJSON_OD_TAPE);
        }
        ip += 2;
        d->ip = ip;
        it->vtok = ip;
        if (s + klen < d->len && buf[s + klen] == '"' &&
            memcmp(buf + s, key, klen) == 0 &&
            yyjson_od__str_end(d, ip - 2) == s + klen) {
            if (yyjson_od_unlikely(!yyjson_od__vtab[YYJSON_OD_CH(d, ip)])) {
                it->state = 2;
                return yyjson_od__fail(d, YYJSON_OD_TAPE);
            }
            val->doc = d;
            val->tok = ip;
            return true;
        }
    }
    return false;
}

/** Find a member by raw key in any order (like simdjson's
    find_field_unordered): search forward from the current member, then
    wrap around to the start of the object and search up to where the
    search began. If not found the object is consumed to its end.
    @return true if found; false if not found or on error. */
yyjson_api_inline bool yyjson_od_obj_find_unordered(yyjson_od_iter *it,
                                                    const char *key,
                                                    size_t klen,
                                                    yyjson_od_val *val) {
    if (it->state == 0) return yyjson_od_obj_find(it, key, klen, val);
    return yyjson_od__find_slow(it, key, klen, val);
}

/** Next array element. Unread parts of the previous element are skipped.
    @return false at the end of the array or on error. */
yyjson_api_inline bool yyjson_od_arr_next(yyjson_od_iter *it,
                                          yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    uint32_t ip;
    if (!yyjson_od__sep(it, ']')) return false;
    ip = d->ip;
    if (yyjson_od_unlikely(!yyjson_od__vtab[YYJSON_OD_CH(d, ip)] ||
                           ip >= d->ntok)) {
        it->state = 2;
        return yyjson_od__fail(d, YYJSON_OD_TAPE);
    }
    it->vtok = ip;
    val->doc = d;
    val->tok = ip;
    return true;
}

/** Count the elements of an array (or members of an object) value without
    moving the cursor. The value must be at the cursor. Structure is checked
    only for bracket balance. Returns 0 on error (see yyjson_od_doc_err). */
yyjson_api size_t yyjson_od_count(yyjson_od_val *v);

/** Raw JSON text of a value, from its first to its last byte (whitespace
    around it excluded). A container is consumed (skipped) by this call and
    must be at the cursor; a scalar is consumed when it is at the cursor.
    The content is not validated. */
yyjson_api bool yyjson_od_raw_json(yyjson_od_val *v, yyjson_od_str *out);

/*------------------------------------------------------------------------------
 * Scalars
 *----------------------------------------------------------------------------*/

/** Type of a value from its first byte (not validated, never fails). */
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

/** Get a string (unescaped; a view into the input when it has no escape).
    @return false if the value is not a string or on error. */
yyjson_api_inline bool yyjson_od_get_str(yyjson_od_val *v, yyjson_od_str *out) {
    yyjson_od_doc *d = v->doc;
    int pos = yyjson_od__pos(v);
    uint32_t s, e;
    if (yyjson_od_unlikely(!pos)) return false;
    if (yyjson_od_unlikely(YYJSON_OD_CH(d, v->tok) != '"'))
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE);
    s = d->idx[v->tok] + 1;
    e = yyjson_od__str_end(d, v->tok);
    if (yyjson_od_unlikely(!e)) return yyjson_od__fail(d, YYJSON_OD_TAPE);
    if (yyjson_od_likely(!yyjson_od__has_spc(d->spc, s, e))) {
        out->ptr = (const char *)d->buf + s;
        out->len = (size_t)(e - s);
    } else if (!yyjson_od__str_slow(d, s, e, out)) {
        return false;
    }
    if (pos == 1) d->ip++;
    return true;
}

static const uint32_t yyjson_od__pow10u[9] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
};

/* Fast number parser. Returns the end of the number, or NULL when the slow
   path must decide (invalid input, >19 digits, -0, big exponents, inexact
   Eisel-Lemire). Results are bit-identical to the yyjson DOM reader. */
yyjson_api_inline const unsigned char *yyjson_od__pnum(const unsigned char *p,
                                                       yyjson_val *v) {
    static const double pow10[23] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    const unsigned char *s, *f;
    uint64_t sig = 0, x;
    uint32_t dg;
    int32_t e = 0, ex;
    int neg, eneg;
    size_t nd;
    neg = (*p == '-');
    p += neg;
    s = p;
    if (*p == '0') {
        p++;
        if ((unsigned)(*p - '0') <= 9) return NULL;
    } else {
        while ((dg = (uint32_t)(*p - '0')) <= 9) {
            sig = sig * 10 + dg;
            p++;
        }
        if (p == s) return NULL;
    }
    nd = (size_t)(p - s);
    if (yyjson_od_likely(*p != '.' && (*p | 0x20) != 'e')) {
        if (yyjson_od_unlikely(nd > 19)) return NULL;
        if (!neg) {
            v->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT;
            v->uni.u64 = sig;
        } else {
            if (sig - 1 >= ((uint64_t)1 << 63)) return NULL; /* -0, < min */
            v->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT;
            v->uni.u64 = (uint64_t)0 - sig;
        }
        return p;
    }
    if (*p == '.') {
        p++;
        f = p;
#if (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        {
            const uint64_t f0 = ((uint64_t)0xF0F0F0F0 << 32) | 0xF0F0F0F0;
            const uint64_t a6 = ((uint64_t)0x06060606 << 32) | 0x06060606;
            const uint64_t c3 = ((uint64_t)0x33333333 << 32) | 0x33333333;
            const uint64_t c0 = ((uint64_t)0x30303030 << 32) | 0x30303030;
            const uint64_t mff = ((uint64_t)0x00FF00FF << 32) | 0x00FF00FF;
            const uint64_t mffff = ((uint64_t)0x0000FFFF << 32) | 0xFFFF;
            const uint64_t k3 = ((uint64_t)10000 << 32) | 1;
            uint64_t t;
            uint32_t n;
            for (;;) { /* 8 digits at a time (SWAR) */
                memcpy(&x, p, 8);
                t = ((x & f0) | (((x + a6) & f0) >> 4)) ^ c3;
                if (t) break;
                x -= c0;
                x = (x * 2561) >> 8;
                x = ((x & mff) * 6553601) >> 16;
                x = ((x & mffff) * k3) >> 32;
                sig = sig * 100000000 + x;
                p += 8;
            }
            /* 0..7 remaining digits in one step */
            n = (uint32_t)__builtin_ctzll(t) >> 3;
            if (n) {
                x = (x - c0) << (64 - 8 * n);
                x = (x * 2561) >> 8;
                x = ((x & mff) * 6553601) >> 16;
                x = ((x & mffff) * k3) >> 32;
                sig = sig * (uint64_t)yyjson_od__pow10u[n] + x;
                p += n;
            }
        }
#else
        while ((dg = (uint32_t)(*p - '0')) <= 9) {
            sig = sig * 10 + dg;
            p++;
        }
#endif
        if (yyjson_od_unlikely(p == f)) return NULL;
        nd += (size_t)(p - f);
        e = -(int32_t)(p - f);
    }
    if ((*p | 0x20) == 'e') {
        p++;
        eneg = (*p == '-');
        if (*p == '-' || *p == '+') p++;
        f = p;
        ex = 0;
        while ((dg = (uint32_t)(*p - '0')) <= 9 && p - f < 4) {
            ex = ex * 10 + (int32_t)dg;
            p++;
        }
        if (yyjson_od_unlikely(p == f || dg <= 9)) return NULL;
        e += eneg ? -ex : ex;
    }
    if (yyjson_od_unlikely(nd > 19)) return NULL;
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
    if (yyjson_od_likely(sig < ((uint64_t)1 << 53) && e >= -22 && e <= 22)) {
        double dbl = (double)sig;
        if (e < 0) dbl /= pow10[-e];
        else dbl *= pow10[e];
        v->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
        v->uni.f64 = neg ? -dbl : dbl;
        return p;
    }
#else
    (void)pow10;
#endif
    if (!yyjson_od__f64(sig, e, neg != 0, v)) return NULL;
    return p;
}

/* Fast integer parser for get_uint/get_sint (simdjson-style: integers
   get their own path). Returns the end, or NULL to fall back to the full
   number parser (fraction/exponent, -0, leading zero, >19 digits, overflow
   of the sign range). */
yyjson_api_inline const unsigned char *yyjson_od__pint(const unsigned char *p,
                                                       uint64_t *out,
                                                       int *negp) {
    const unsigned char *s;
    uint64_t sig = 0;
    uint32_t dg;
    int neg = (*p == '-');
    p += neg;
    s = p;
    if (*p == '0') return NULL; /* 0 and leading zeros: slow path */
#if (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    { /* like simdjson: a predictable "8 digits?" branch, then 4, then
         scalar (a ctz-based variable step was slower: it lengthens the
         dependency chain to the cursor) */
        uint64_t x;
        uint32_t y;
        memcpy(&x, p, 8);
        if ((((x & (((uint64_t)0xF0F0F0F0 << 32) | 0xF0F0F0F0)) |
              (((x + (((uint64_t)0x06060606 << 32) | 0x06060606)) &
                (((uint64_t)0xF0F0F0F0 << 32) | 0xF0F0F0F0)) >> 4)) ==
             (((uint64_t)0x33333333 << 32) | 0x33333333))) {
            x -= ((uint64_t)0x30303030 << 32) | 0x30303030;
            x = (x * 2561) >> 8;
            x = ((x & (((uint64_t)0x00FF00FF << 32) | 0x00FF00FF)) *
                 6553601) >> 16;
            x = ((x & (((uint64_t)0x0000FFFF << 32) | 0xFFFF)) *
                 (((uint64_t)10000 << 32) | 1)) >> 32;
            sig = x;
            p += 8;
        }
        memcpy(&y, p, 4);
        if ((((y + 0x46464646u) | (y - 0x30303030u)) & 0x80808080u) == 0) {
            y -= 0x30303030u;
            y = (y * 10) + (y >> 8);
            y = ((y & 0xFF) * 100) + ((y >> 16) & 0xFF);
            sig = sig * 10000 + y;
            p += 4;
        }
    }
#endif
    while ((dg = (uint32_t)(*p - '0')) <= 9) {
        sig = sig * 10 + dg;
        p++;
    }
    /* one table lookup checks both "no fraction/exponent" and the
       delimiter (the next token can only be , ] } after a value) */
    if (yyjson_od_unlikely(p == s || p - s > 19 || !yyjson_od__ttab[*p]))
        return NULL;
    if (neg && sig - 1 >= ((uint64_t)1 << 63)) return NULL;
    *out = neg ? (uint64_t)0 - sig : sig;
    *negp = neg;
    return p;
}

/* Parse the number at token `tok` (cursor checks done by the caller). */
yyjson_api_inline bool yyjson_od__num_at(yyjson_od_doc *d, uint32_t tok,
                                         yyjson_val *n) {
    const unsigned char *p = d->buf + d->idx[tok], *e;
    e = yyjson_od__pnum(p, n);
    if (yyjson_od_likely(e && yyjson_od__ttab[*e]))
        return true;
    return yyjson_od__num_slow(d, tok, n);
}

/* Number into a yyjson_val, advancing the cursor. */
yyjson_api_inline bool yyjson_od__getnum(yyjson_od_val *v, yyjson_val *n) {
    int pos = yyjson_od__pos(v);
    if (yyjson_od_unlikely(!pos)) return false;
    if (yyjson_od_unlikely(!yyjson_od__num_at(v->doc, v->tok, n)))
        return false;
    if (pos == 1) v->doc->ip++;
    return true;
}

/** Get a number as a yyjson_val (subtype UINT, SINT or REAL), with the same
    result as the yyjson DOM reader. */
yyjson_api_inline bool yyjson_od_get_num(yyjson_od_val *v, yyjson_val *out) {
    return yyjson_od__getnum(v, out);
}

/** Get an integer that fits int64_t. A number with a fraction, exponent or
    out of range is an error (INCORRECT_TYPE). */
yyjson_api_inline bool yyjson_od_get_sint(yyjson_od_val *v, int64_t *out) {
    yyjson_od_doc *d = v->doc;
    yyjson_val n;
    if (yyjson_od_likely(v->tok == d->ip)) {
        const unsigned char *p = d->buf + d->idx[v->tok], *e;
        uint64_t u;
        int neg;
        e = yyjson_od__pint(p, &u, &neg);
        if (yyjson_od_likely(e && (neg || u <= (uint64_t)INT64_MAX))) {
            d->ip++;
            *out = (int64_t)u;
            return true;
        }
    }
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(n.tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT) ||
                         (n.tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT) &&
                          n.uni.u64 <= (uint64_t)INT64_MAX))) {
        *out = n.uni.i64;
        return true;
    }
    return yyjson_od__fail(v->doc, YYJSON_OD_INCORRECT_TYPE);
}

/** Get a non-negative integer that fits uint64_t. */
yyjson_api_inline bool yyjson_od_get_uint(yyjson_od_val *v, uint64_t *out) {
    yyjson_od_doc *d = v->doc;
    yyjson_val n;
    if (yyjson_od_likely(v->tok == d->ip)) {
        const unsigned char *p = d->buf + d->idx[v->tok], *e;
        uint64_t u;
        int neg;
        e = yyjson_od__pint(p, &u, &neg);
        if (yyjson_od_likely(e && !neg)) {
            d->ip++;
            *out = u;
            return true;
        }
    }
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(n.tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT))) {
        *out = n.uni.u64;
        return true;
    }
    return yyjson_od__fail(v->doc, YYJSON_OD_INCORRECT_TYPE);
}

/** Get any number as a double (integers are converted). */
yyjson_api_inline bool yyjson_od_get_real(yyjson_od_val *v, double *out) {
    yyjson_val n;
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(n.tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL)))
        *out = n.uni.f64;
    else if (n.tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT))
        *out = (double)n.uni.i64;
    else *out = (double)n.uni.u64;
    return true;
}

/* literal of `n` bytes at token `tok`, followed by a delimiter */
yyjson_api_inline bool yyjson_od__lit(yyjson_od_doc *d, uint32_t tok,
                                      const char *lit, size_t n) {
    const unsigned char *p = d->buf + d->idx[tok];
    if (memcmp(p, lit, n) != 0) return false;
    if (p + n == d->buf + d->idx[tok + 1]) return true;
    return YYJSON_OD_WS(p[n]);
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
    without consuming it (not an error for non-null values). */
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
