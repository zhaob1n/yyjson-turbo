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
 *    container may not. yyjson_od_doc_rewind() restarts the iteration from
 *    the root. Unread members and elements are skipped automatically.
 *  - Misuse checks: like simdjson (SIMDJSON_DEVELOPMENT_CHECKS), detection
 *    of out-of-order use (reading a value after the cursor moved past it,
 *    using an iterator after its parent advanced) is compiled in only when
 *    YYJSON_OD_CHECKS is defined to 1; misuse then fails with
 *    YYJSON_OD_OUT_OF_ORDER. By default (0) correct programs behave exactly
 *    the same, and misuse is memory-safe but its results are unspecified.
 *    The macro only affects inline code: it may differ between the library
 *    and the program.
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
#if defined(__AVX2__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif
#include <string.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of readable bytes required after the end of the input. */
#define YYJSON_OD_PADDING 64

/** Misuse (out-of-order) detection, see above. Default: off. */
#ifndef YYJSON_OD_CHECKS
#define YYJSON_OD_CHECKS 0
#endif

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

/** Depth limit of the misuse checks (deeper iterators are not checked). */
#define YYJSON_OD_CK_DEPTH 64
#define YYJSON_OD_PH_SIZE 64

/** A document: the indexed input. Fields are internal. */
typedef struct yyjson_od_doc {
    const unsigned char *buf;   /* input */
    const uint32_t *idx;        /* token offsets: structurals, opening quotes
                                   and scalar starts; 4 sentinels at ntok
                                   point at a byte no step accepts */
    unsigned char *scr;         /* scratch cursor for unescaped strings */
    unsigned char *scr_beg;     /* start of the scratch buffer */
    unsigned char *scr_end;     /* end of the scratch buffer */
    size_t len;
    uint32_t ntok;              /* number of tokens */
    uint32_t end_of;            /* last container that was completed ... */
    uint32_t end_ip;            /* ... and the token after it */
    /* resume hints for abandoned iterators: the last value each yielded,
       in a small table indexed by container token (a collision only loses
       a hint) */
    struct { uint32_t of, ip; } ph[YYJSON_OD_PH_SIZE];
    uint32_t err_tok;           /* token at which the first error happened */
    int err;                    /* yyjson_od_code */
    /* YYJSON_OD_CHECKS only (always present: the layout is fixed) */
    uint32_t ckd;                       /* depth of the innermost live iterator */
    uint32_t ck[YYJSON_OD_CK_DEPTH];    /* id of the live iterator per depth */
    uint32_t cv[YYJSON_OD_CK_DEPTH];    /* its current value (~0: consumed) */
} yyjson_od_doc;

/** Reusable parser holding the index and scratch buffers. */
typedef struct yyjson_od_parser yyjson_od_parser;

/** Handle to a value (its first token). Copyable; no ownership. */
typedef struct yyjson_od_val {
    yyjson_od_doc *doc;
    uint32_t tok;
    uint32_t depth;             /* depth of its parent iterator (checks) */
} yyjson_od_val;

/** Object or array iterator: it owns its position (16 bytes, kept in
    registers by the inlined loops). Fields are internal. */
typedef struct yyjson_od_iter {
    yyjson_od_doc *doc;
    uint32_t ip;                /* current member value; otok - 1 before the
                                   first one; ~0 when done */
    uint32_t otok;              /* first token after '{' / '[' */
#if YYJSON_OD_CHECKS
    uint32_t depth;             /* depth of the members (root value: 0) */
#endif
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

/** Like yyjson_od_parse, but fills a caller-owned document (usually a
    local variable). This is the fastest way to iterate: a document whose
    address does not escape lets the compiler keep the cursor in registers
    across the inlined iteration functions. The document stays valid until
    the next parse with the same parser; copies of it are independent
    cursors over the same index.
    @return YYJSON_OD_OK, or the error code (also stored in the document,
    whose iteration then fails). */
yyjson_api int yyjson_od_iterate(yyjson_od_parser *p, const char *buf,
                                 size_t len, yyjson_od_doc *doc);

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
    v.depth = 0;
    return v;
}

/** Restart iteration from the root: clears the error and invalidates
    strings decoded into the scratch buffer. Handles and iterators obtained
    before must not be used afterwards. */
yyjson_api_inline void yyjson_od_doc_rewind(yyjson_od_doc *d) {
    d->end_of = ~(uint32_t)0;
    d->end_ip = 0;
    memset(d->ph, 0xff, sizeof(d->ph));
    d->err = 0;
    d->err_tok = 0;
    d->scr = d->scr_beg;
    d->ckd = 0;
    d->ck[0] = 0;
    d->cv[0] = 0;
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
                                 uint32_t *idx, size_t *ntok);
yyjson_api unsigned char *yyjson_od__str_esc(const unsigned char *beg,
                                             const unsigned char *end,
                                             unsigned char *out);
yyjson_api unsigned char *yyjson_od__str_esc2(const unsigned char *beg,
                                              const unsigned char *bs,
                                              const unsigned char *end,
                                              unsigned char *out);
yyjson_api const unsigned char *yyjson_od__num(const unsigned char *cur,
                                               const unsigned char *eof,
                                               yyjson_val *val);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((const))
#endif
yyjson_api uint64_t yyjson_od__f64(uint64_t sig, int32_t exp, int sign);
/* The out-of-line helpers below never receive the doc: they take the
   buffer/index by value and return results by value, and are declared
   pure/const. A call on a cold path then cannot clobber the caller's doc,
   so buf/idx/ntok stay in registers across iterator loops. */
#if defined(__GNUC__) || defined(__clang__)
#   define YYJSON_OD_PURE __attribute__((pure))
#else
#   define YYJSON_OD_PURE
#endif
/* skip tokens from `ip` at `depth` until depth <= target: returns
   ip | depth << 32 (depth != target on failure) */
YYJSON_OD_PURE yyjson_api uint64_t yyjson_od__skip(const unsigned char *buf,
                                                   const uint32_t *idx,
                                                   uint32_t ntok, uint32_t ip,
                                                   uint32_t depth,
                                                   uint32_t target);
/* full number parse of [p, next token): err 0 on success */
typedef struct yyjson_od__nr { uint64_t tag, u; int err; } yyjson_od__nr;
YYJSON_OD_PURE yyjson_api yyjson_od__nr yyjson_od__numv(
    const unsigned char *p, const unsigned char *nx, const unsigned char *eof);
/* `bs` <= first backslash offset (the scanner's stop point) */
yyjson_api bool yyjson_od__key_check(yyjson_od_doc *d, uint32_t s,
                                     uint32_t bs, uint32_t e);
yyjson_api bool yyjson_od__str_slow(yyjson_od_doc *d, uint32_t s, uint32_t bs,
                                    uint32_t e, yyjson_od_str *out);

/* Record the first error (at token `tok`). Inline: not a call that would
   take the doc's address. Iterators stop on their own errors; the error
   code stays in the document. */
yyjson_api_inline bool yyjson_od__fail(yyjson_od_doc *d, int code,
                                       uint32_t tok) {
    if (!d->err) {
        d->err = code;
        d->err_tok = tok;
    }
    return false;
}

yyjson_api_inline bool yyjson_od__num_slow(yyjson_od_doc *d, uint32_t tok,
                                           yyjson_val *out) {
    yyjson_od__nr r = yyjson_od__numv(d->buf + d->idx[tok],
                                      d->buf + d->idx[tok + 1],
                                      d->buf + d->len);
    if (r.err) return yyjson_od__fail(d, r.err, tok);
    out->tag = r.tag;
    out->uni.u64 = r.u;
    return true;
}

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


/* Offset of the closing quote of the string at token `tok` (0 if broken).
   Only whitespace may lie between a closing quote and the next token. */
yyjson_api_inline uint32_t yyjson_od__str_end(const yyjson_od_doc *d,
                                              uint32_t tok) {
    uint32_t s = d->idx[tok], e = d->idx[tok + 1] - 1;
    while (yyjson_od_unlikely(YYJSON_OD_WS(d->buf[e])) && e > s) e--;
    return (e > s && d->buf[e] == '"') ? e : 0;
}

/* Find the end of the string whose content starts at offset `s` of token
   `tok`: returns the offset of the closing quote, or 0 if the string has
   escapes (or the tape is broken), in which case the caller takes the slow
   path. One forward pass, 8 bytes per step (simdjson finds escapes lazily
   the same way, at string read time); the closing quote exists (stage 1
   checked it) and the padding keeps the loads in bounds. */
yyjson_api_inline uint32_t yyjson_od__str_scan(const yyjson_od_doc *d,
                                               uint32_t tok, uint32_t s,
                                               uint32_t *bs) {
    const unsigned char *buf = d->buf;
    uint32_t e, nx;
#if defined(__AVX2__) && (defined(__GNUC__) || defined(__clang__))
    const __m256i vq = _mm256_set1_epi8('"'), vb = _mm256_set1_epi8('\\');
    uint32_t m;
    e = s;
    for (;;) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(const void *)(buf + e));
        m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_cmpeq_epi8(x, vq), _mm256_cmpeq_epi8(x, vb)));
        if (m) break;
        e += 32;
    }
    e += (uint32_t)__builtin_ctz(m);
#elif (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const uint64_t k1 = ((uint64_t)0x01010101 << 32) | 0x01010101;
    const uint64_t k80 = ((uint64_t)0x80808080 << 32) | 0x80808080;
    const uint64_t kq = ((uint64_t)0x22222222 << 32) | 0x22222222;
    const uint64_t kbs = ((uint64_t)0x5C5C5C5C << 32) | 0x5C5C5C5C;
    uint64_t x, xq, xb, t;
    e = s;
    for (;;) {
        memcpy(&x, buf + e, 8);
        xq = x ^ kq;
        xb = x ^ kbs;
        /* the lowest set bit of each term is exact */
        t = ((xq - k1) & ~xq) | ((xb - k1) & ~xb);
        t &= k80;
        if (t) break;
        e += 8;
    }
    e += (uint32_t)(__builtin_ctzll(t) >> 3);
#else
    e = s;
    while (buf[e] != '"' && buf[e] != '\\') e++;
#endif
    *bs = e; /* no backslash before e */
    if (yyjson_od_unlikely(buf[e] != '"')) return 0;
    *bs = s;
    nx = d->idx[tok + 1];
    if (yyjson_od_likely(e + 1 == nx)) return e;
    return yyjson_od__str_end(d, tok) == e ? e : 0;
}

#define YYJSON_OD_DONE (~(uint32_t)0)

/* Progress hint for the parent: the value an iterator just yielded. Only
   stored here (never loaded on the iterator's own path), so the iterator
   position stays in registers. */
#define YYJSON_OD__HINT(d, it, ip) \
    do { \
        uint32_t ph_ = (it)->otok - 1; \
        (d)->ph[ph_ % YYJSON_OD_PH_SIZE].of = ph_; \
        (d)->ph[ph_ % YYJSON_OD_PH_SIZE].ip = (ip); \
    } while (0)

/* Token after the container that starts at `tok`: the handoff left by the
   child iterator that completed it (one compare, simdjson-style), else a
   skip through the index: from the last value the child yielded if it was
   abandoned after partial reading (past that value if it was a completed
   container), else from the start. 0 on error. */
yyjson_api_inline uint32_t yyjson_od__after(yyjson_od_doc *d, uint32_t tok) {
    uint64_t r;
    uint32_t ip = tok + 1;
    if (d->end_of == tok) return d->end_ip;
    /* an abandoned child: resume from its last position */
    if (d->ph[tok % YYJSON_OD_PH_SIZE].of == tok) {
        ip = d->ph[tok % YYJSON_OD_PH_SIZE].ip;
        if (d->end_of == ip) ip = d->end_ip; /* its value was completed */
    }
    r = yyjson_od__skip(d->buf, d->idx, d->ntok, ip, 1, 0);
    if (yyjson_od_unlikely((uint32_t)(r >> 32) != 0)) {
        yyjson_od__fail(d, YYJSON_OD_TAPE, tok);
        return 0;
    }
    return (uint32_t)r;
}

#if YYJSON_OD_CHECKS
/* An iterator may be used if it is the innermost live iterator or one of
   its ancestors (which abandons the descendants). */
yyjson_api_inline bool yyjson_od__ck_use(yyjson_od_iter *it) {
    yyjson_od_doc *d = it->doc;
    uint32_t j = it->depth;
    if (j >= YYJSON_OD_CK_DEPTH) return true;
    if (j > d->ckd || d->ck[j] != it->otok) {
        it->ip = YYJSON_OD_DONE;
        return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER, it->otok - 1);
    }
    d->ckd = j;
    return true;
}
#   define YYJSON_OD__CK_USE(it) \
        if (!yyjson_od__ck_use(it)) return false
#   define YYJSON_OD__CK_CUR(it, val, tok) do { \
        (val)->depth = (it)->depth; \
        if ((it)->depth < YYJSON_OD_CK_DEPTH) \
            (it)->doc->cv[(it)->depth] = (tok); \
    } while (0)
#else
#   define YYJSON_OD__CK_USE(it) (void)0
#   define YYJSON_OD__CK_CUR(it, val, tok) (void)0
#endif

/* Step an iterator from its current member value to the start of the next
   member (after ','). Returns the new position, or 0 at the end (the
   iterator is then done and the end is handed to the parent) or on
   error. */
yyjson_api_inline uint32_t yyjson_od__step(yyjson_od_iter *it, uint32_t ip,
                                           unsigned char close) {
    yyjson_od_doc *d = it->doc;
    const unsigned char *buf = d->buf;
    const uint32_t *idx = d->idx;
    unsigned char c = buf[idx[ip]];
    if (c == '{' || c == '[') {
        ip = yyjson_od__after(d, ip);
        if (yyjson_od_unlikely(!ip)) {
            it->ip = YYJSON_OD_DONE;
            return 0;
        }
    } else {
        ip++;
    }
    c = buf[idx[ip]]; /* the sentinels stop every step at ntok */
    if (yyjson_od_likely(c == ',')) return ip + 1;
    it->ip = YYJSON_OD_DONE;
    if (yyjson_od_likely(c == close)) {
        d->end_of = it->otok - 1;
        d->end_ip = ip + 1;
        return 0;
    }
    yyjson_od__fail(d, YYJSON_OD_TAPE, ip);
    return 0;
}

yyjson_api_inline bool yyjson_od__begin(yyjson_od_val *v, yyjson_od_iter *it,
                                        unsigned char open,
                                        unsigned char close) {
    yyjson_od_doc *d = v->doc;
    uint32_t tok = v->tok;
    it->doc = d;
    it->ip = YYJSON_OD_DONE;
    it->otok = tok + 1;
#if YYJSON_OD_CHECKS
    it->depth = v->depth + 1;
#endif
    if (yyjson_od_unlikely(YYJSON_OD_CH(d, tok) != open))
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE, tok);
#if YYJSON_OD_CHECKS
    {   /* the value must be the current one of a live iterator, and a
           container is iterated once */
        uint32_t k = v->depth;
        if (k < YYJSON_OD_CK_DEPTH) {
            if (k > d->ckd || d->cv[k] != tok)
                return yyjson_od__fail(d, YYJSON_OD_OUT_OF_ORDER, tok);
            d->cv[k] = YYJSON_OD_DONE;
            d->ckd = k + 1;
            if (k + 1 < YYJSON_OD_CK_DEPTH) {
                d->ck[k + 1] = tok + 1;
                d->cv[k + 1] = YYJSON_OD_DONE;
            }
        }
    }
#endif
    if (YYJSON_OD_CH(d, tok + 1) == close) { /* empty */
        d->end_of = tok;
        d->end_ip = tok + 2;
        return true;
    }
    it->ip = tok; /* before the first member */
    return true;
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

/* Position of the next member of an object or array iterator (0: none). */
yyjson_api_inline uint32_t yyjson_od__next(yyjson_od_iter *it,
                                           unsigned char close) {
    uint32_t ip = it->ip;
    if (yyjson_od_unlikely(ip + 1 <= it->otok)) { /* first, or done */
        if (ip == YYJSON_OD_DONE) return 0;
        YYJSON_OD__CK_USE(it);
        return ip + 1;
    }
    YYJSON_OD__CK_USE(it);
    return yyjson_od__step(it, ip, close);
}

/** Next object member: its raw key (escaped form, not NUL-terminated) and
    value handle. Unread parts of the previous member are skipped.
    @return false at the end of the object or on error. */
yyjson_api_inline bool yyjson_od_obj_next(yyjson_od_iter *it,
                                          yyjson_od_str *key,
                                          yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    const unsigned char *buf = d->buf;
    const uint32_t *idx = d->idx;
    uint32_t ip = yyjson_od__next(it, '}'), s, e, bs;
    if (!ip) return false;
    if (yyjson_od_unlikely(buf[idx[ip]] != '"' || buf[idx[ip + 1]] != ':' ||
                           !yyjson_od__vtab[buf[idx[ip + 2]]])) {
        it->ip = YYJSON_OD_DONE;
        return yyjson_od__fail(d, YYJSON_OD_TAPE, ip);
    }
    s = idx[ip] + 1;
    e = yyjson_od__str_scan(d, ip, s, &bs);
    if (yyjson_od_unlikely(!e)) {
        e = yyjson_od__str_end(d, ip);
        if (!e || !yyjson_od__key_check(d, s, bs, e)) {
            it->ip = YYJSON_OD_DONE;
            return yyjson_od__fail(d, e ? YYJSON_OD_STRING : YYJSON_OD_TAPE,
                                   ip);
        }
    }
    key->ptr = (const char *)buf + s;
    key->len = (size_t)(e - s);
    ip += 2;
    it->ip = ip;
    YYJSON_OD__HINT(d, it, ip);
    val->doc = d;
    val->tok = ip;
    YYJSON_OD__CK_CUR(it, val, ip);
    return true;
}

/** Find the next member whose raw key equals `key` (ordered lookup: the
    search continues from the current member to the end of the object; the
    members passed are skipped). `key` must not contain '"' or '\\' unless
    it is written in the escaped form used by the document. Keys compared
    this way are not validated (see the top of the file).
    @return true if found (`val` is set); false if not found or on error. */
yyjson_api_inline bool yyjson_od_obj_find(yyjson_od_iter *it, const char *key,
                                          size_t klen, yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    const unsigned char *buf = d->buf;
    const uint32_t *idx = d->idx;
    uint32_t ip = yyjson_od__next(it, '}'), s, q;
    if (!ip) return false;
    for (;;) {
        s = idx[ip] + 1;
        q = idx[ip + 1];
        if (yyjson_od_unlikely(buf[s - 1] != '"' || buf[q] != ':')) break;
        ip += 2;
        if (yyjson_od_unlikely(!yyjson_od__vtab[buf[idx[ip]]])) break;
        /* a quote right after the bytes of a well-formed escaped key is
           the closing quote, so no str_end scan is needed */
        if (s + klen < q && buf[s + klen] == '"' &&
            memcmp(buf + s, key, klen) == 0) {
            it->ip = ip;
            YYJSON_OD__HINT(d, it, ip);
            val->doc = d;
            val->tok = ip;
            YYJSON_OD__CK_CUR(it, val, ip);
            return true;
        }
        ip = yyjson_od__step(it, ip, '}');
        if (!ip) return false;
    }
    it->ip = YYJSON_OD_DONE;
    return yyjson_od__fail(d, YYJSON_OD_TAPE, ip);
}

/** Find a member by raw key in any order (like simdjson's
    find_field_unordered): search forward from the current member, then
    wrap around to the start of the object and search up to the current
    member (included). If not found the object is consumed to its end.
    @return true if found; false if not found or on error. */
yyjson_api_inline bool yyjson_od_obj_find_unordered(yyjson_od_iter *it,
                                                    const char *key,
                                                    size_t klen,
                                                    yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    yyjson_od_iter w;
    yyjson_od_str k;
    yyjson_od_val v;
    uint32_t start = it->ip;
    if (start + 1 == it->otok) return yyjson_od_obj_find(it, key, klen, val);
    if (start != YYJSON_OD_DONE) {
        if (yyjson_od_obj_find(it, key, klen, val)) return true;
        /* stop unless the search reached the end of the object */
        if (d->end_of != it->otok - 1) return false;
    }
    if (YYJSON_OD_CH(d, it->otok) == '}') return false; /* empty */
    w = *it;
    w.ip = w.otok - 1;
    while (yyjson_od_obj_next(&w, &k, &v)) {
        if (v.tok > start) break;
        if (k.len == klen && memcmp(k.ptr, key, klen) == 0) {
            *it = w;
            *val = v;
            return true;
        }
    }
    return false;
}

/** Next array element. Unread parts of the previous element are skipped.
    @return false at the end of the array or on error. */
yyjson_api_inline bool yyjson_od_arr_next(yyjson_od_iter *it,
                                          yyjson_od_val *val) {
    yyjson_od_doc *d = it->doc;
    uint32_t ip = yyjson_od__next(it, ']');
    if (!ip) return false;
    if (yyjson_od_unlikely(!yyjson_od__vtab[YYJSON_OD_CH(d, ip)])) {
        it->ip = YYJSON_OD_DONE;
        return yyjson_od__fail(d, YYJSON_OD_TAPE, ip);
    }
    it->ip = ip;
    val->doc = d;
    val->tok = ip;
    YYJSON_OD__CK_CUR(it, val, ip);
    return true;
}

/** Count the elements of an array (or members of an object) value. Does
    not move any iterator. Structure is checked only for bracket balance.
    Returns 0 on error (see yyjson_od_doc_err). */
yyjson_api size_t yyjson_od_count(yyjson_od_val *v);

/** Raw JSON text of a value, from its first to its last byte (whitespace
    around it excluded). The content is not validated. */
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
    uint32_t s, e, bs;
    if (yyjson_od_unlikely(YYJSON_OD_CH(d, v->tok) != '"'))
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE, v->tok);
    s = d->idx[v->tok] + 1;
    e = yyjson_od__str_scan(d, v->tok, s, &bs);
    if (yyjson_od_likely(e)) {
        out->ptr = (const char *)d->buf + s;
        out->len = (size_t)(e - s);
    } else {
        e = yyjson_od__str_end(d, v->tok);
        if (yyjson_od_unlikely(!e)) return yyjson_od__fail(d, YYJSON_OD_TAPE, v->tok);
        if (!yyjson_od__str_slow(d, s, bs, e, out)) return false;
    }
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
    x = yyjson_od__f64(sig, e, neg);
    if (yyjson_od_unlikely(x == ~(uint64_t)0)) return NULL;
    v->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
    v->uni.u64 = x;
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

/* Parse the number at token `tok`. */
yyjson_api_inline bool yyjson_od__num_at(yyjson_od_doc *d, uint32_t tok,
                                         yyjson_val *n) {
    const unsigned char *p = d->buf + d->idx[tok], *e;
    e = yyjson_od__pnum(p, n);
    if (yyjson_od_likely(e && yyjson_od__ttab[*e]))
        return true;
    return yyjson_od__num_slow(d, tok, n);
}

/* Number into a yyjson_val. */
yyjson_api_inline bool yyjson_od__getnum(yyjson_od_val *v, yyjson_val *n) {
    if (yyjson_od_unlikely(!yyjson_od__num_at(v->doc, v->tok, n)))
        return false;
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
    {
        const unsigned char *p = d->buf + d->idx[v->tok], *e;
        uint64_t u;
        int neg;
        e = yyjson_od__pint(p, &u, &neg);
        if (yyjson_od_likely(e && (neg || u <= (uint64_t)INT64_MAX))) {
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
    return yyjson_od__fail(v->doc, YYJSON_OD_INCORRECT_TYPE, v->tok);
}

/** Get a non-negative integer that fits uint64_t. */
yyjson_api_inline bool yyjson_od_get_uint(yyjson_od_val *v, uint64_t *out) {
    yyjson_od_doc *d = v->doc;
    yyjson_val n;
    {
        const unsigned char *p = d->buf + d->idx[v->tok], *e;
        uint64_t u;
        int neg;
        e = yyjson_od__pint(p, &u, &neg);
        if (yyjson_od_likely(e && !neg)) {
            *out = u;
            return true;
        }
    }
    if (!yyjson_od__getnum(v, &n)) return false;
    if (yyjson_od_likely(n.tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT))) {
        *out = n.uni.u64;
        return true;
    }
    return yyjson_od__fail(v->doc, YYJSON_OD_INCORRECT_TYPE, v->tok);
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
    unsigned char c;
    c = YYJSON_OD_CH(d, v->tok);
    if (c == 't') {
        if (yyjson_od_unlikely(!yyjson_od__lit(d, v->tok, "true", 4)))
            return yyjson_od__fail(d, YYJSON_OD_LITERAL, v->tok);
        *out = true;
    } else if (c == 'f') {
        if (yyjson_od_unlikely(!yyjson_od__lit(d, v->tok, "false", 5)))
            return yyjson_od__fail(d, YYJSON_OD_LITERAL, v->tok);
        *out = false;
    } else {
        return yyjson_od__fail(d, YYJSON_OD_INCORRECT_TYPE, v->tok);
    }
    return true;
}

/** If the value is null, consume it and return true. Otherwise return false
    without consuming it (not an error for non-null values). */
yyjson_api_inline bool yyjson_od_is_null(yyjson_od_val *v) {
    yyjson_od_doc *d = v->doc;
    if (YYJSON_OD_CH(d, v->tok) != 'n') return false;
    if (yyjson_od_unlikely(!yyjson_od__lit(d, v->tok, "null", 4)))
        return yyjson_od__fail(d, YYJSON_OD_LITERAL, v->tok);
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* YYJSON_ONDEMAND_H */
