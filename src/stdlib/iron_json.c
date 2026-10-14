/* iron_json.c -- strict RFC 8259 JSON scanner and streaming writer.
 *
 * Scanner: one pass over the input that validates the grammar, UTF-8 and
 * escapes, decodes strings and numbers, and records the document as a
 * tape (see iron_json.h). The Iron side of the module builds its Json tree
 * from the tape, so this file never depends on a compiler-generated layout.
 * Hot paths: strings are scanned eight bytes at a time and copied once;
 * integers are accumulated directly and floats take an exact fast path
 * (Clinger) before falling back to strtod.
 *
 * Writer: a growable byte buffer with a container stack, addressed by an
 * opaque Int handle that a JsonWriter on the Iron side owns. Values are
 * appended in place (commas, indentation and escaping are inserted here),
 * so encoding builds no intermediate tree or strings.
 */

#include "iron_json.h"
#include "runtime/iron_errors.h"
#include "iron_json_ryu.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Small helpers ──────────────────────────────────────────────────────── */

static Iron_String json_cstr(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

static void *json_grow(void *p, size_t *cap, size_t need, size_t elem, const char *where) {
    if (need <= *cap) return p;
    size_t nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    void *np = iron_mem_realloc(p, nc * elem);
    if (!np) iron_oom_abort(where);
    *cap = nc;
    return np;
}

Iron_String Iron_jsonnative_error_text(int64_t code) {
    switch (code) {
        case 0: return json_cstr("");
        case IRON_ERR_JSON_SYNTAX: return json_cstr("malformed JSON");
        case IRON_ERR_JSON_DEPTH: return json_cstr("JSON nesting exceeds the depth limit");
        case IRON_ERR_JSON_TOO_LARGE: return json_cstr("JSON input exceeds the size limit");
        case IRON_ERR_JSON_BAD_UTF8: return json_cstr("invalid UTF-8 in JSON input");
        case IRON_ERR_JSON_BAD_STRING: return json_cstr("invalid JSON string");
        case IRON_ERR_JSON_BAD_NUMBER: return json_cstr("invalid JSON number");
        case IRON_ERR_JSON_TYPE: return json_cstr("JSON value has another type");
        case IRON_ERR_JSON_MISSING: return json_cstr("JSON value not present");
        default: return json_cstr("JSON error");
    }
}

/* ── UTF-8 ──────────────────────────────────────────────────────────────── */

/* Length of the well-formed UTF-8 sequence starting at p[i] (first byte
 * >= 0x80), or 0 when it is ill-formed (Unicode Table 3-7: no overlongs,
 * no surrogates, nothing above U+10FFFF). */
static size_t json_utf8_len(const unsigned char *p, size_t i, size_t len) {
    unsigned char c = p[i];
    if (c >= 0xC2 && c <= 0xDF) {
        if (i + 1 < len && (p[i + 1] & 0xC0) == 0x80) return 2;
        return 0;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (i + 2 >= len) return 0;
        unsigned char c1 = p[i + 1], c2 = p[i + 2];
        if ((c2 & 0xC0) != 0x80) return 0;
        if (c == 0xE0) { if (c1 < 0xA0 || c1 > 0xBF) return 0; }
        else if (c == 0xED) { if (c1 < 0x80 || c1 > 0x9F) return 0; }
        else if ((c1 & 0xC0) != 0x80) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (i + 3 >= len) return 0;
        unsigned char c1 = p[i + 1], c2 = p[i + 2], c3 = p[i + 3];
        if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return 0;
        if (c == 0xF0) { if (c1 < 0x90 || c1 > 0xBF) return 0; }
        else if (c == 0xF4) { if (c1 < 0x80 || c1 > 0x8F) return 0; }
        else if ((c1 & 0xC0) != 0x80) return 0;
        return 4;
    }
    return 0;
}

/* Bytes of w that are '"', '\\', < 0x20 or >= 0x80 (nonzero when any). */
static inline uint64_t json_special8(uint64_t w) {
    const uint64_t ones = 0x0101010101010101ULL, high = 0x8080808080808080ULL;
    uint64_t q = w ^ (ones * '"');
    uint64_t b = w ^ (ones * '\\');
    uint64_t r = ((q - ones) & ~q) | ((b - ones) & ~b) | ((w - ones * 0x20) & ~w) | w;
    return r & high;
}

/* ── Scanner ────────────────────────────────────────────────────────────── */

typedef struct {
    const unsigned char *p;
    size_t len, pos;
    int64_t max_depth;

    int64_t *ops;   size_t nops, capops;
    double *fl;     size_t nfl, capfl;
    Iron_String *strs; size_t nstrs, capstrs;
    unsigned char *stack; size_t depth, capstack;
    char *scratch;  size_t capscratch;

    int64_t err;
    size_t err_pos;
    char err_msg[200];
} JsonScan;

static void js_fail(JsonScan *j, int64_t code, size_t pos, const char *msg) {
    if (j->err) return;
    j->err = code;
    j->err_pos = pos;
    snprintf(j->err_msg, sizeof(j->err_msg), "%s", msg);
}

static inline void js_op(JsonScan *j, int64_t op) {
    if (j->nops == j->capops)
        j->ops = (int64_t *)json_grow(j->ops, &j->capops, j->nops + 1, sizeof(int64_t), "json tape");
    j->ops[j->nops++] = op;
}

static inline void js_str(JsonScan *j, int64_t op, const char *s, size_t n) {
    js_op(j, op);
    if (j->nstrs == j->capstrs)
        j->strs = (Iron_String *)json_grow(j->strs, &j->capstrs, j->nstrs + 1, sizeof(Iron_String), "json strings");
    j->strs[j->nstrs++] = iron_string_from_cstr(s, n);
}

static inline void js_ws(JsonScan *j) {
    const unsigned char *p = j->p;
    size_t i = j->pos, n = j->len;
    /* Compact JSON has no whitespace: return at once. */
    if (i < n && p[i] > ' ') return;
    while (i < n) {
        unsigned char c = p[i];
        if (c == ' ') {
            i++;
            /* Indentation: skip runs of spaces eight at a time. */
            while (i + 8 <= n) {
                uint64_t w;
                memcpy(&w, p + i, 8);
                if (w != 0x2020202020202020ULL) break;
                i += 8;
            }
        } else if (c == '\n' || c == '\r' || c == '\t') {
            i++;
        } else {
            break;
        }
    }
    j->pos = i;
}

static int js_hex4(const unsigned char *p, size_t i, size_t len, unsigned *out) {
    if (i + 4 > len) return 0;
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        unsigned char c = p[i + k];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        v = v * 16 + d;
    }
    *out = v;
    return 1;
}

static size_t js_put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A string starting at the opening quote j->pos. Emits `op` with the
 * decoded text. Returns 0 on error. */
static int js_string(JsonScan *j, int64_t op) {
    const unsigned char *p = j->p;
    size_t len = j->len;
    size_t start = j->pos + 1, i = start;
    for (;;) {
        while (i + 8 <= len) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (json_special8(w)) break;
            i += 8;
        }
        if (i >= len) { js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, "unterminated string"); return 0; }
        unsigned char c = p[i];
        if (c == '"') {
            js_str(j, op, (const char *)p + start, i - start);
            j->pos = i + 1;
            return 1;
        }
        if (c == '\\') break;
        if (c < 0x20) {
            js_fail(j, IRON_ERR_JSON_BAD_STRING, i, "control character in string (escape it)");
            return 0;
        }
        if (c >= 0x80) {
            size_t n = json_utf8_len(p, i, len);
            if (!n) { js_fail(j, IRON_ERR_JSON_BAD_UTF8, i, "invalid UTF-8 in string"); return 0; }
            i += n;
            continue;
        }
        i++;
    }

    /* Slow path: escapes. The decoded text is never longer than the
     * remaining raw text, so the scratch buffer is sized once. */
    size_t need = len - start + 1;
    j->scratch = (char *)json_grow(j->scratch, &j->capscratch, need, 1, "json string");
    char *o = j->scratch;
    size_t on = i - start;
    memcpy(o, p + start, on);
    for (;;) {
        if (i >= len) { js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, "unterminated string"); return 0; }
        unsigned char c = p[i];
        if (c == '"') break;
        if (c == '\\') {
            if (i + 1 >= len) { js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, "unterminated string"); return 0; }
            unsigned char e = p[i + 1];
            switch (e) {
                case '"': o[on++] = '"'; i += 2; continue;
                case '\\': o[on++] = '\\'; i += 2; continue;
                case '/': o[on++] = '/'; i += 2; continue;
                case 'b': o[on++] = '\b'; i += 2; continue;
                case 'f': o[on++] = '\f'; i += 2; continue;
                case 'n': o[on++] = '\n'; i += 2; continue;
                case 'r': o[on++] = '\r'; i += 2; continue;
                case 't': o[on++] = '\t'; i += 2; continue;
                case 'u': {
                    unsigned cp;
                    if (!js_hex4(p, i + 2, len, &cp)) {
                        js_fail(j, IRON_ERR_JSON_BAD_STRING, i, "\\u must be followed by four hex digits");
                        return 0;
                    }
                    size_t at = i;
                    i += 6;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (i + 1 < len && p[i] == '\\' && p[i + 1] == 'u' &&
                            js_hex4(p, i + 2, len, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        } else {
                            js_fail(j, IRON_ERR_JSON_BAD_STRING, at,
                                    "lone high surrogate (\\uD800-\\uDBFF must be followed by a low surrogate)");
                            return 0;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        js_fail(j, IRON_ERR_JSON_BAD_STRING, at, "lone low surrogate");
                        return 0;
                    }
                    /* \uXXXX decodes to at most 3 bytes for 6 input bytes and
                     * a pair to 4 for 12, so the buffer bound holds. */
                    on += js_put_utf8(o + on, cp);
                    continue;
                }
                default:
                    js_fail(j, IRON_ERR_JSON_BAD_STRING, i, "invalid escape sequence");
                    return 0;
            }
        }
        if (c < 0x20) {
            js_fail(j, IRON_ERR_JSON_BAD_STRING, i, "control character in string (escape it)");
            return 0;
        }
        if (c >= 0x80) {
            size_t n = json_utf8_len(p, i, len);
            if (!n) { js_fail(j, IRON_ERR_JSON_BAD_UTF8, i, "invalid UTF-8 in string"); return 0; }
            memcpy(o + on, p + i, n);
            on += n;
            i += n;
            continue;
        }
        /* Copy a run of plain bytes. */
        size_t r = i + 1;
        while (r < len) {
            unsigned char d = p[r];
            if (d == '"' || d == '\\' || d < 0x20 || d >= 0x80) break;
            r++;
        }
        memcpy(o + on, p + i, r - i);
        on += r - i;
        i = r;
    }
    js_str(j, op, o, on);
    j->pos = i + 1;
    return 1;
}

static inline uint64_t json_umul128(uint64_t a, uint64_t b, uint64_t *hi);

static inline int json_clz64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(x);
#else
    int n = 0;
    while (!(x & (1ULL << 63))) { x <<= 1; n++; }
    return n;
#endif
}

/* Eisel-Lemire: the double nearest to w * 10^q for a nonzero w of at most
 * 19 decimal digits, exact without a big-number fallback (Lemire, "Number
 * Parsing at a Gigabyte per Second", 2021; Mushtak and Lemire, "Fast
 * Number Parsing Without Fallback", 2023); structure after fast_float.
 * Returns 0 when the caller should use strtod instead (q out of the table,
 * a subnormal result, or overflow). */
static int json_eisel_lemire(uint64_t w, int64_t q, int neg, double *out) {
    if (w == 0 || q < -342 || q > 308) return 0;
    int lz = json_clz64(w);
    w <<= lz;
    const uint64_t *t = json_pow5_128[q + 342];
    uint64_t hi;
    uint64_t lo = json_umul128(w, t[0], &hi);
    if ((hi & 0x1FF) == 0x1FF) {
        uint64_t hi2;
        (void)json_umul128(w, t[1], &hi2);
        lo += hi2;
        if (hi2 > lo) hi++;
    }
    int upperbit = (int)(hi >> 63);
    int shift = upperbit + 9;
    uint64_t mantissa = hi >> shift;
    int64_t power2 = (((152170 + 65536) * q) >> 16) + 63 + upperbit - lz + 1023;
    if (power2 <= 0) return 0;               /* subnormal: strtod */
    if (lo <= 1 && q >= -4 && q <= 23 && (mantissa & 3) == 1 &&
        (mantissa << shift) == hi)
        mantissa &= ~(uint64_t)1;            /* exactly halfway: to even */
    mantissa += mantissa & 1;
    mantissa >>= 1;
    if (mantissa >= (2ULL << 52)) {
        mantissa = 1ULL << 52;
        power2++;
    }
    mantissa &= ~(1ULL << 52);
    if (power2 >= 0x7FF) return 0;           /* overflow: reported by strtod path */
    uint64_t bits = mantissa | ((uint64_t)power2 << 52) | ((uint64_t)neg << 63);
    memcpy(out, &bits, sizeof bits);
    return 1;
}

static const double json_pow10[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static int js_number(JsonScan *j) {
    const unsigned char *p = j->p;
    size_t len = j->len, start = j->pos, i = start;
    int neg = 0;
    if (p[i] == '-') {
        neg = 1;
        i++;
        if (i >= len || p[i] < '0' || p[i] > '9') {
            js_fail(j, IRON_ERR_JSON_BAD_NUMBER, start, "expected a digit after '-'");
            return 0;
        }
    }
    uint64_t mant = 0;
    int digits = 0;        /* significant digits accumulated into mant */
    int64_t dropped = 0;   /* integer digits that did not fit in mant */
    int nonzero_dropped = 0;
    if (p[i] == '0') {
        i++;
        if (i < len && p[i] >= '0' && p[i] <= '9') {
            js_fail(j, IRON_ERR_JSON_BAD_NUMBER, start, "leading zeros are not allowed");
            return 0;
        }
    } else {
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            if (digits < 19) { mant = mant * 10 + (uint64_t)(p[i] - '0'); digits++; }
            else { dropped++; if (p[i] != '0') nonzero_dropped = 1; }
            i++;
        }
    }
    int is_float = 0;
    int64_t frac_digits = 0;
    if (i < len && p[i] == '.') {
        is_float = 1;
        i++;
        if (i >= len || p[i] < '0' || p[i] > '9') {
            js_fail(j, IRON_ERR_JSON_BAD_NUMBER, start, "expected a digit after '.'");
            return 0;
        }
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            if (digits < 19) {
                if (mant == 0 && p[i] == '0') { frac_digits++; i++; continue; }
                mant = mant * 10 + (uint64_t)(p[i] - '0');
                digits++;
                frac_digits++;
            } else if (p[i] != '0') {
                nonzero_dropped = 1;
            }
            i++;
        }
    }
    int64_t exp10 = 0;
    if (i < len && (p[i] == 'e' || p[i] == 'E')) {
        is_float = 1;
        i++;
        int eneg = 0;
        if (i < len && (p[i] == '+' || p[i] == '-')) { eneg = p[i] == '-'; i++; }
        if (i >= len || p[i] < '0' || p[i] > '9') {
            js_fail(j, IRON_ERR_JSON_BAD_NUMBER, start, "expected a digit in the exponent");
            return 0;
        }
        /* Saturate far beyond any digit count the input can hold, so that
         * exp10 - frac_digits + dropped below stays exact whenever it can
         * matter: a saturated exponent leaves |e| > 10^16, which only the
         * strtod path (on the full text) or the zero path handles. */
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            if (exp10 < 100000000000000000LL) exp10 = exp10 * 10 + (p[i] - '0');
            i++;
        }
        if (eneg) exp10 = -exp10;
    }
    j->pos = i;

    if (!is_float && dropped == 0) {
        if (!neg && mant <= (uint64_t)INT64_MAX) {
            js_op(j, IRON_JSON_OP_INT);
            js_op(j, (int64_t)mant);
            return 1;
        }
        if (neg && mant <= (uint64_t)INT64_MAX + 1) {
            js_op(j, IRON_JSON_OP_INT);
            js_op(j, (int64_t)(0 - mant));
            return 1;
        }
    }

    double d;
    int64_t e = exp10 - frac_digits + dropped;
    if (!nonzero_dropped && mant <= (1ULL << 53) && e >= -22 && e <= 22) {
        /* Exact: both operands are representable, IEEE rounds once. */
        d = (double)mant;
        d = e < 0 ? d / json_pow10[-e] : d * json_pow10[e];
        if (neg) d = -d;
    } else if (mant == 0) {
        d = neg ? -0.0 : 0.0;
    } else if (!nonzero_dropped && json_eisel_lemire(mant, e, neg, &d)) {
        /* 16 to 19 significant digits or a large exponent: still exact. */
    } else {
        char small[64];
        size_t n = i - start;
        char *buf = n < sizeof(small) ? small : (char *)malloc(n + 1);
        if (!buf) iron_oom_abort("json number");
        memcpy(buf, p + start, n);
        buf[n] = '\0';
        d = strtod(buf, NULL);
        if (buf != small) free(buf);
    }
    if (!isfinite(d)) {
        js_fail(j, IRON_ERR_JSON_BAD_NUMBER, start, "number is out of the range of a 64-bit float");
        return 0;
    }
    js_op(j, IRON_JSON_OP_FLOAT);
    if (j->nfl == j->capfl)
        j->fl = (double *)json_grow(j->fl, &j->capfl, j->nfl + 1, sizeof(double), "json floats");
    j->fl[j->nfl++] = d;
    return 1;
}

static int js_literal(JsonScan *j, const char *lit, size_t n, int64_t op) {
    if (j->len - j->pos >= n && memcmp(j->p + j->pos, lit, n) == 0) {
        j->pos += n;
        js_op(j, op);
        return 1;
    }
    js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, "invalid literal (expected true, false or null)");
    return 0;
}

static int js_push(JsonScan *j, unsigned char kind) {
    if ((int64_t)j->depth >= j->max_depth) {
        js_fail(j, IRON_ERR_JSON_DEPTH, j->pos, "nesting is deeper than the depth limit");
        return 0;
    }
    if (j->depth == j->capstack)
        j->stack = (unsigned char *)json_grow(j->stack, &j->capstack, j->depth + 1, 1, "json stack");
    j->stack[j->depth++] = kind;
    return 1;
}

static void js_describe(char *out, size_t cap, const unsigned char *p, size_t len, size_t pos) {
    if (pos >= len) { snprintf(out, cap, "end of input"); return; }
    unsigned char c = p[pos];
    if (c >= 0x20 && c < 0x7F) snprintf(out, cap, "'%c'", c);
    else snprintf(out, cap, "byte 0x%02X", c);
}

static void js_expected(JsonScan *j, const char *what) {
    char got[32], msg[160];
    js_describe(got, sizeof(got), j->p, j->len, j->pos);
    snprintf(msg, sizeof(msg), "expected %s, found %s", what, got);
    js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, msg);
}

static void js_run(JsonScan *j) {
    const unsigned char *p = j->p;
    js_ws(j);
    if (j->pos >= j->len) { js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, "empty input (expected a JSON value)"); return; }

value:
    if (j->pos >= j->len) { js_expected(j, "a value"); return; }
    switch (p[j->pos]) {
        case '{':
            if (!js_push(j, '{')) return;
            js_op(j, IRON_JSON_OP_OBJ);
            j->pos++;
            js_ws(j);
            if (j->pos < j->len && p[j->pos] == '}') {
                j->pos++;
                j->depth--;
                js_op(j, IRON_JSON_OP_END);
                goto after;
            }
            goto key;
        case '[':
            if (!js_push(j, '[')) return;
            js_op(j, IRON_JSON_OP_ARR);
            j->pos++;
            js_ws(j);
            if (j->pos < j->len && p[j->pos] == ']') {
                j->pos++;
                j->depth--;
                js_op(j, IRON_JSON_OP_END);
                goto after;
            }
            goto value;
        case '"':
            if (!js_string(j, IRON_JSON_OP_STR)) return;
            goto after;
        case 't': if (!js_literal(j, "true", 4, IRON_JSON_OP_TRUE)) return; goto after;
        case 'f': if (!js_literal(j, "false", 5, IRON_JSON_OP_FALSE)) return; goto after;
        case 'n': if (!js_literal(j, "null", 4, IRON_JSON_OP_NULL)) return; goto after;
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            if (!js_number(j)) return;
            goto after;
        case ']': case '}': {
            size_t b = j->pos;
            while (b > 0 && (p[b - 1] == ' ' || p[b - 1] == '\n' || p[b - 1] == '\r' || p[b - 1] == '\t')) b--;
            if (b > 0 && p[b - 1] == ',') {
                js_fail(j, IRON_ERR_JSON_SYNTAX, b - 1, "trailing comma before closing bracket");
                return;
            }
            js_expected(j, "a value");
            return;
        }
        default:
            js_expected(j, "a value");
            return;
    }

key:
    if (j->pos >= j->len || p[j->pos] != '"') {
        if (j->pos < j->len && p[j->pos] == '}') {
            js_fail(j, IRON_ERR_JSON_SYNTAX, j->pos, "trailing comma before closing brace");
            return;
        }
        js_expected(j, "a string key");
        return;
    }
    if (!js_string(j, IRON_JSON_OP_KEY)) return;
    js_ws(j);
    if (j->pos >= j->len || p[j->pos] != ':') { js_expected(j, "':' after object key"); return; }
    j->pos++;
    js_ws(j);
    goto value;

after:
    js_ws(j);
    if (j->depth == 0) {
        if (j->pos != j->len) js_expected(j, "end of input after the top-level value");
        return;
    }
    if (j->pos >= j->len) {
        js_expected(j, j->stack[j->depth - 1] == '{' ? "',' or '}'" : "',' or ']'");
        return;
    }
    {
        unsigned char c = p[j->pos];
        unsigned char top = j->stack[j->depth - 1];
        if (c == ',') {
            j->pos++;
            js_ws(j);
            if (top == '{') goto key;
            goto value;
        }
        if ((top == '{' && c == '}') || (top == '[' && c == ']')) {
            j->pos++;
            j->depth--;
            js_op(j, IRON_JSON_OP_END);
            goto after;
        }
        js_expected(j, top == '{' ? "',' or '}'" : "',' or ']'");
    }
}

/* 1-based line and column (in characters) of byte offset pos. */
static void js_locate(const unsigned char *p, size_t len, size_t pos, int64_t *line, int64_t *col) {
    int64_t l = 1, c = 1;
    if (pos > len) pos = len;
    for (size_t i = 0; i < pos; i++) {
        if (p[i] == '\n') { l++; c = 1; }
        else if ((p[i] & 0xC0) != 0x80) c++;
    }
    *line = l;
    *col = c;
}

Iron_JsonTape Iron_jsonnative_scan(Iron_String text, int64_t max_depth, int64_t max_bytes) {
    Iron_JsonTape t;
    memset(&t, 0, sizeof(t));
    JsonScan j;
    memset(&j, 0, sizeof(j));
    j.p = (const unsigned char *)iron_string_cstr(&text);
    j.len = iron_string_byte_len(&text);
    /* Limits below 1 mean 1; above IRON_JSON_MAX_DEPTH they are clamped so
     * that every operation on the resulting tree stays within the stack
     * (dropping a tree is the one recursive operation left). */
    j.max_depth = max_depth > 0 ? max_depth : 1;
    if (j.max_depth > IRON_JSON_MAX_DEPTH) j.max_depth = IRON_JSON_MAX_DEPTH;

    if (max_bytes >= 0 && (int64_t)j.len > max_bytes) {
        js_fail(&j, IRON_ERR_JSON_TOO_LARGE, 0, "input is larger than the size limit");
    } else {
        /* First reservation: one tape word per 4 input bytes covers most
         * documents without a grow (a grow copies the whole tape). */
        size_t guess = j.len / 4 + 16;
        j.ops = (int64_t *)json_grow(NULL, &j.capops, guess, sizeof(int64_t), "json tape");
        js_run(&j);
    }

    if (j.err) {
        for (size_t i = 0; i < j.nstrs; i++) iron_string_release(&j.strs[i]);
        iron_mem_free(j.ops);
        iron_mem_free(j.fl);
        iron_mem_free(j.strs);
        int64_t line = 1, col = 1;
        js_locate(j.p, j.len, j.err_pos, &line, &col);
        char msg[300];
        snprintf(msg, sizeof(msg), "%s at line %lld, column %lld", j.err_msg,
                 (long long)line, (long long)col);
        t.error = j.err;
        t.error_message = json_cstr(msg);
        t.line = line;
        t.column = col;
        t.offset = (int64_t)j.err_pos;
    } else {
        t.ops.items = j.ops;
        t.ops.count = (int64_t)j.nops;
        t.ops.capacity = (int64_t)j.capops;
        t.floats.items = j.fl;
        t.floats.count = (int64_t)j.nfl;
        t.floats.capacity = (int64_t)j.capfl;
        t.strs.items = j.strs;
        t.strs.count = (int64_t)j.nstrs;
        t.strs.capacity = (int64_t)j.capstrs;
        t.error_message = json_cstr("");
    }
    iron_mem_free(j.stack);
    iron_mem_free(j.scratch);
    return t;
}

/* ── Number formatting ──────────────────────────────────────────────────── */

/* Shortest round-trip digits: Ryu (Ulf Adams, PLDI 2018). For a finite,
 * nonzero double this finds the shortest decimal in the rounding interval
 * of v and, among those, the one closest to v: the digits Python's repr and
 * JavaScript print. The structure follows the reference implementation
 * (github.com/ulfjack/ryu, Apache-2.0 / Boost-1.0); the power-of-5 tables
 * are generated by scripts/gen_json_ryu_tables.py. */

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 json_u128;   /* -Wpedantic: GNU extension */
#endif

static inline uint64_t json_umul128(uint64_t a, uint64_t b, uint64_t *hi) {
#if defined(__SIZEOF_INT128__)
    json_u128 p = (json_u128)a * b;
    *hi = (uint64_t)(p >> 64);
    return (uint64_t)p;
#else
    uint64_t a_lo = (uint32_t)a, a_hi = a >> 32, b_lo = (uint32_t)b, b_hi = b >> 32;
    uint64_t b00 = a_lo * b_lo, b01 = a_lo * b_hi, b10 = a_hi * b_lo, b11 = a_hi * b_hi;
    uint64_t mid1 = b10 + (b00 >> 32);
    uint64_t mid2 = b01 + (uint32_t)mid1;
    *hi = b11 + (mid1 >> 32) + (mid2 >> 32);
    return (mid2 << 32) | (uint32_t)b00;
#endif
}

/* (m * mul) >> j for a 128-bit mul = {low, high} and 64 <= j < 128. */
static inline uint64_t json_mul_shift64(uint64_t m, const uint64_t *mul, int32_t j) {
    uint64_t high1;
    uint64_t low1 = json_umul128(m, mul[1], &high1);
    uint64_t high0;
    (void)json_umul128(m, mul[0], &high0);
    uint64_t sum = high0 + low1;
    if (sum < high0) high1++;
    int32_t dist = j - 64;
    if (dist <= 0) return sum;
    return (high1 << (64 - dist)) | (sum >> dist);
}

static inline uint32_t json_pow5bits(int32_t e) { return (uint32_t)(((e * 1217359) >> 19) + 1); }
static inline uint32_t json_log10_pow2(int32_t e) { return (uint32_t)((e * 78913) >> 18); }
static inline uint32_t json_log10_pow5(int32_t e) { return (uint32_t)((e * 732923) >> 20); }

static inline uint32_t json_pow5_factor(uint64_t v) {
    uint32_t count = 0;
    while (v % 5 == 0) { v /= 5; count++; }
    return count;
}

static int json_ryu(uint64_t ieee_mantissa, uint32_t ieee_exponent, uint64_t *out_digits, int32_t *out_exp) {
    int32_t e2;
    uint64_t m2;
    if (ieee_exponent == 0) {
        e2 = 1 - 1023 - 52 - 2;
        m2 = ieee_mantissa;
    } else {
        e2 = (int32_t)ieee_exponent - 1023 - 52 - 2;
        m2 = (1ULL << 52) | ieee_mantissa;
    }
    const int even = (m2 & 1) == 0;
    const int accept_bounds = even;
    const uint64_t mv = 4 * m2;
    const uint32_t mm_shift = ieee_mantissa != 0 || ieee_exponent <= 1;

    uint64_t vr, vp, vm;
    int32_t e10;
    int vm_trailing_zeros = 0, vr_trailing_zeros = 0;
    if (e2 >= 0) {
        const uint32_t q = json_log10_pow2(e2) - (e2 > 3);
        e10 = (int32_t)q;
        const int32_t k = 125 + (int32_t)json_pow5bits((int32_t)q) - 1;
        const int32_t i = -e2 + (int32_t)q + k;
        vr = json_mul_shift64(4 * m2, json_pow5_inv_split[q], i);
        vp = json_mul_shift64(4 * m2 + 2, json_pow5_inv_split[q], i);
        vm = json_mul_shift64(4 * m2 - 1 - mm_shift, json_pow5_inv_split[q], i);
        if (q <= 21) {
            if (mv % 5 == 0) vr_trailing_zeros = json_pow5_factor(mv) >= q;
            else if (accept_bounds) vm_trailing_zeros = json_pow5_factor(mv - 1 - mm_shift) >= q;
            else vp -= json_pow5_factor(mv + 2) >= q;
        }
    } else {
        const uint32_t q = json_log10_pow5(-e2) - (-e2 > 1);
        e10 = (int32_t)q + e2;
        const int32_t i = -e2 - (int32_t)q;
        const int32_t k = (int32_t)json_pow5bits(i) - 125;
        const int32_t j = (int32_t)q - k;
        vr = json_mul_shift64(4 * m2, json_pow5_split[i], j);
        vp = json_mul_shift64(4 * m2 + 2, json_pow5_split[i], j);
        vm = json_mul_shift64(4 * m2 - 1 - mm_shift, json_pow5_split[i], j);
        if (q <= 1) {
            vr_trailing_zeros = 1;
            if (accept_bounds) vm_trailing_zeros = mm_shift == 1;
            else vp--;
        } else if (q < 63) {
            vr_trailing_zeros = (mv & ((1ULL << q) - 1)) == 0;
        }
    }

    int32_t removed = 0;
    uint8_t last_removed = 0;
    uint64_t output;
    if (vm_trailing_zeros || vr_trailing_zeros) {
        for (;;) {
            const uint64_t vp10 = vp / 10, vm10 = vm / 10;
            if (vp10 <= vm10) break;
            const uint32_t vm_mod = (uint32_t)(vm - 10 * vm10);
            const uint64_t vr10 = vr / 10;
            const uint32_t vr_mod = (uint32_t)(vr - 10 * vr10);
            vm_trailing_zeros &= vm_mod == 0;
            vr_trailing_zeros &= last_removed == 0;
            last_removed = (uint8_t)vr_mod;
            vr = vr10; vp = vp10; vm = vm10;
            removed++;
        }
        if (vm_trailing_zeros) {
            for (;;) {
                const uint64_t vm10 = vm / 10;
                const uint32_t vm_mod = (uint32_t)(vm - 10 * vm10);
                if (vm_mod != 0) break;
                const uint64_t vp10 = vp / 10, vr10 = vr / 10;
                const uint32_t vr_mod = (uint32_t)(vr - 10 * vr10);
                vr_trailing_zeros &= last_removed == 0;
                last_removed = (uint8_t)vr_mod;
                vr = vr10; vp = vp10; vm = vm10;
                removed++;
            }
        }
        if (vr_trailing_zeros && last_removed == 5 && vr % 2 == 0) last_removed = 4;
        output = vr + ((vr == vm && (!accept_bounds || !vm_trailing_zeros)) || last_removed >= 5);
    } else {
        int round_up = 0;
        const uint64_t vp100 = vp / 100, vm100 = vm / 100;
        if (vp100 > vm100) {
            const uint64_t vr100 = vr / 100;
            const uint32_t vr_mod = (uint32_t)(vr - 100 * vr100);
            round_up = vr_mod >= 50;
            vr = vr100; vp = vp100; vm = vm100;
            removed += 2;
        }
        for (;;) {
            const uint64_t vp10 = vp / 10, vm10 = vm / 10;
            if (vp10 <= vm10) break;
            const uint64_t vr10 = vr / 10;
            const uint32_t vr_mod = (uint32_t)(vr - 10 * vr10);
            round_up = vr_mod >= 5;
            vr = vr10; vp = vp10; vm = vm10;
            removed++;
        }
        output = vr + (vr == vm || round_up);
    }
    *out_digits = output;
    *out_exp = e10 + removed;
    return 1;
}

/* Shortest decimal digits of finite, nonzero v: fills digits (no dot, no
 * trailing zeros), returns the count, and sets *exp to the decimal exponent
 * of the first digit (v = d.ddd * 10^exp). */
static int json_shortest(double v, char *digits, int *exp, int *neg) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    *neg = (int)(bits >> 63);
    uint64_t out;
    int32_t e10;
    json_ryu(bits & ((1ULL << 52) - 1), (uint32_t)((bits >> 52) & 0x7FF), &out, &e10);
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + out % 10); out /= 10; } while (out);
    for (int i = 0; i < n; i++) digits[i] = tmp[n - 1 - i];
    while (n > 1 && digits[n - 1] == '0') { n--; e10++; }
    digits[n] = '\0';
    *exp = e10 + n - 1;
    return n;
}

/* JSON text for a float into out (>= 40 bytes); returns the length.
 * A whole value keeps a ".0" so it reads back as a float, exponents
 * outside -7..20 use scientific notation, and NaN/Infinity, which JSON
 * cannot represent, are written as null. */
static size_t json_fmt_float(double v, char *out) {
    if (!isfinite(v)) { memcpy(out, "null", 4); return 4; }
    if (v == 0.0) {
        if (signbit(v)) { memcpy(out, "-0.0", 4); return 4; }
        memcpy(out, "0.0", 3);
        return 3;
    }
    char digits[24];
    int e, neg;
    int n = json_shortest(v, digits, &e, &neg);
    char *o = out;
    if (neg) *o++ = '-';
    if (e >= -7 && e < 21) {
        if (e >= 0) {
            for (int i = 0; i <= e; i++) *o++ = (i < n) ? digits[i] : '0';
            *o++ = '.';
            if (n > e + 1) {
                for (int i = e + 1; i < n; i++) *o++ = digits[i];
            } else {
                *o++ = '0';
            }
        } else {
            *o++ = '0';
            *o++ = '.';
            for (int i = 0; i < -e - 1; i++) *o++ = '0';
            for (int i = 0; i < n; i++) *o++ = digits[i];
        }
    } else {
        *o++ = digits[0];
        if (n > 1) {
            *o++ = '.';
            for (int i = 1; i < n; i++) *o++ = digits[i];
        }
        o += snprintf(o, 8, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    }
    return (size_t)(o - out);
}

Iron_String Iron_jsonnative_format_float(double f) {
    char buf[48];
    size_t n = json_fmt_float(f, buf);
    return iron_string_from_cstr(buf, n);
}

/* ── Writer ─────────────────────────────────────────────────────────────── */

enum { JW_OBJECT = 1, JW_HAS_ITEMS = 2, JW_WANT_VALUE = 4 };

typedef struct {
    char *d;
    size_t n, cap;
    unsigned char *st;
    size_t depth, capst;
    int indent;
    int root_done;
} JsonWriter;

static JsonWriter *jw(int64_t h) {
    return (JsonWriter *)(intptr_t)h;
}

static void jw_panic(const char *msg) {
    Iron_assert(false, json_cstr(msg));
}

static inline void jw_reserve(JsonWriter *w, size_t extra) {
    if (w->n + extra > w->cap) {
        size_t nc = w->cap ? w->cap : 256;
        while (nc < w->n + extra) nc *= 2;
        char *nd = (char *)iron_mem_realloc(w->d, nc);
        if (!nd) iron_oom_abort("json writer");
        w->d = nd;
        w->cap = nc;
    }
}

static inline void jw_put(JsonWriter *w, const char *s, size_t n) {
    jw_reserve(w, n);
    memcpy(w->d + w->n, s, n);
    w->n += n;
}

static inline void jw_ch(JsonWriter *w, char c) {
    jw_reserve(w, 1);
    w->d[w->n++] = c;
}

static void jw_newline(JsonWriter *w, size_t depth) {
    size_t k = (size_t)w->indent * depth;
    jw_reserve(w, k + 1);
    w->d[w->n++] = '\n';
    memset(w->d + w->n, ' ', k);
    w->n += k;
}

static const char json_hex[] = "0123456789abcdef";

/* Append s as a JSON string literal. Ill-formed UTF-8 (Iron strings are
 * byte strings) is replaced by U+FFFD so the output is always valid. */
static void jw_quote(JsonWriter *w, const unsigned char *p, size_t len) {
    jw_reserve(w, len + 2);
    w->d[w->n++] = '"';
    size_t i = 0;
    while (i < len) {
        size_t r = i;
        while (r + 8 <= len) {
            uint64_t x;
            memcpy(&x, p + r, 8);
            if (json_special8(x)) break;
            r += 8;
        }
        while (r < len) {
            unsigned char c = p[r];
            if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
            r++;
        }
        if (r > i) { jw_put(w, (const char *)p + i, r - i); i = r; }
        if (i >= len) break;
        unsigned char c = p[i];
        if (c >= 0x80) {
            size_t n = json_utf8_len(p, i, len);
            if (n) { jw_put(w, (const char *)p + i, n); i += n; }
            else { jw_put(w, "\xEF\xBF\xBD", 3); i++; }
            continue;
        }
        char esc[6];
        switch (c) {
            case '"': jw_put(w, "\\\"", 2); break;
            case '\\': jw_put(w, "\\\\", 2); break;
            case '\n': jw_put(w, "\\n", 2); break;
            case '\r': jw_put(w, "\\r", 2); break;
            case '\t': jw_put(w, "\\t", 2); break;
            case '\b': jw_put(w, "\\b", 2); break;
            case '\f': jw_put(w, "\\f", 2); break;
            default:
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = json_hex[c >> 4]; esc[5] = json_hex[c & 15];
                jw_put(w, esc, 6);
        }
        i++;
    }
    jw_ch(w, '"');
}

/* Separator and indentation before a value; panics on misuse. */
static void jw_before_value(JsonWriter *w) {
    if (w->depth == 0) {
        if (w->root_done)
            jw_panic("JsonWriter: a document has one top-level value; call finish() before writing another");
        return;
    }
    unsigned char *top = &w->st[w->depth - 1];
    if (*top & JW_OBJECT) {
        if (!(*top & JW_WANT_VALUE))
            jw_panic("JsonWriter: write key() before each value inside an object");
        *top &= (unsigned char)~JW_WANT_VALUE;
        return;
    }
    if (*top & JW_HAS_ITEMS) jw_ch(w, ',');
    *top |= JW_HAS_ITEMS;
    if (w->indent > 0) jw_newline(w, w->depth);
}

static void jw_after_value(JsonWriter *w) {
    if (w->depth == 0) w->root_done = 1;
}

int64_t Iron_jsonnative_w_new(int64_t indent) {
    JsonWriter *w = (JsonWriter *)calloc(1, sizeof(JsonWriter));
    if (!w) iron_oom_abort("json writer");
    w->indent = indent < 0 ? 0 : (indent > 16 ? 16 : (int)indent);
    return (int64_t)(intptr_t)w;
}

void Iron_jsonnative_w_free(int64_t h) {
    JsonWriter *w = jw(h);
    if (!w) return;
    iron_mem_free(w->d);
    iron_mem_free(w->st);
    free(w);
}

static void jw_open(JsonWriter *w, unsigned char kind, char c) {
    jw_before_value(w);
    jw_ch(w, c);
    if (w->depth == w->capst)
        w->st = (unsigned char *)json_grow(w->st, &w->capst, w->depth + 1, 1, "json writer");
    w->st[w->depth++] = kind;
}

static void jw_close(JsonWriter *w, int object, char c) {
    if (w->depth == 0 || ((w->st[w->depth - 1] & JW_OBJECT) != 0) != object)
        jw_panic(object ? "JsonWriter: end_object() without a matching begin_object()"
                        : "JsonWriter: end_array() without a matching begin_array()");
    unsigned char top = w->st[w->depth - 1];
    if (top & JW_WANT_VALUE) jw_panic("JsonWriter: key() without a value before end_object()");
    w->depth--;
    if (w->indent > 0 && (top & JW_HAS_ITEMS)) jw_newline(w, w->depth);
    jw_ch(w, c);
    jw_after_value(w);
}

void Iron_jsonnative_w_begin_object(int64_t h) { jw_open(jw(h), JW_OBJECT, '{'); }
void Iron_jsonnative_w_end_object(int64_t h) { jw_close(jw(h), 1, '}'); }
void Iron_jsonnative_w_begin_array(int64_t h) { jw_open(jw(h), 0, '['); }
void Iron_jsonnative_w_end_array(int64_t h) { jw_close(jw(h), 0, ']'); }

void Iron_jsonnative_w_key(int64_t h, Iron_String key) {
    JsonWriter *w = jw(h);
    if (w->depth == 0 || !(w->st[w->depth - 1] & JW_OBJECT))
        jw_panic("JsonWriter: key() outside an object");
    unsigned char *top = &w->st[w->depth - 1];
    if (*top & JW_WANT_VALUE) jw_panic("JsonWriter: two keys in a row (write the value first)");
    if (*top & JW_HAS_ITEMS) jw_ch(w, ',');
    *top |= JW_HAS_ITEMS | JW_WANT_VALUE;
    if (w->indent > 0) jw_newline(w, w->depth);
    jw_quote(w, (const unsigned char *)iron_string_cstr(&key), iron_string_byte_len(&key));
    if (w->indent > 0) jw_put(w, ": ", 2);
    else jw_ch(w, ':');
}

void Iron_jsonnative_w_string(int64_t h, Iron_String s) {
    JsonWriter *w = jw(h);
    jw_before_value(w);
    jw_quote(w, (const unsigned char *)iron_string_cstr(&s), iron_string_byte_len(&s));
    jw_after_value(w);
}

static void jw_int(JsonWriter *w, int64_t v) {
    char buf[24];
    char *e = buf + sizeof(buf), *b = e;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { *--b = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--b = '-';
    jw_put(w, b, (size_t)(e - b));
}

void Iron_jsonnative_w_int(int64_t h, int64_t v) {
    JsonWriter *w = jw(h);
    jw_before_value(w);
    jw_int(w, v);
    jw_after_value(w);
}

void Iron_jsonnative_w_float(int64_t h, double v) {
    JsonWriter *w = jw(h);
    jw_before_value(w);
    char buf[48];
    jw_put(w, buf, json_fmt_float(v, buf));
    jw_after_value(w);
}

void Iron_jsonnative_w_bool(int64_t h, bool v) {
    JsonWriter *w = jw(h);
    jw_before_value(w);
    if (v) jw_put(w, "true", 4);
    else jw_put(w, "false", 5);
    jw_after_value(w);
}

void Iron_jsonnative_w_null(int64_t h) {
    JsonWriter *w = jw(h);
    jw_before_value(w);
    jw_put(w, "null", 4);
    jw_after_value(w);
}

void Iron_jsonnative_w_raw(int64_t h, Iron_String json) {
    JsonWriter *w = jw(h);
    jw_before_value(w);
    jw_put(w, iron_string_cstr(&json), iron_string_byte_len(&json));
    jw_after_value(w);
}

void Iron_jsonnative_w_field_string(int64_t h, Iron_String key, Iron_String s) {
    Iron_jsonnative_w_key(h, key);
    Iron_jsonnative_w_string(h, s);
}

void Iron_jsonnative_w_field_int(int64_t h, Iron_String key, int64_t v) {
    Iron_jsonnative_w_key(h, key);
    Iron_jsonnative_w_int(h, v);
}

void Iron_jsonnative_w_field_float(int64_t h, Iron_String key, double v) {
    Iron_jsonnative_w_key(h, key);
    Iron_jsonnative_w_float(h, v);
}

void Iron_jsonnative_w_field_bool(int64_t h, Iron_String key, bool v) {
    Iron_jsonnative_w_key(h, key);
    Iron_jsonnative_w_bool(h, v);
}

void Iron_jsonnative_w_field_null(int64_t h, Iron_String key) {
    Iron_jsonnative_w_key(h, key);
    Iron_jsonnative_w_null(h);
}

int64_t Iron_jsonnative_w_len(int64_t h) { return (int64_t)jw(h)->n; }
int64_t Iron_jsonnative_w_depth(int64_t h) { return (int64_t)jw(h)->depth; }

void Iron_jsonnative_w_reset(int64_t h) {
    JsonWriter *w = jw(h);
    w->n = 0;
    w->depth = 0;
    w->root_done = 0;
}

Iron_String Iron_jsonnative_w_finish(int64_t h) {
    JsonWriter *w = jw(h);
    if (w->depth != 0) jw_panic("JsonWriter: finish() with an unclosed object or array");
    Iron_String s = iron_string_from_cstr(w->d ? w->d : "", w->n);
    Iron_jsonnative_w_reset(h);
    return s;
}

Iron_String Iron_jsonnative_quote(Iron_String s) {
    JsonWriter w;
    memset(&w, 0, sizeof(w));
    jw_quote(&w, (const unsigned char *)iron_string_cstr(&s), iron_string_byte_len(&s));
    Iron_String out = iron_string_from_cstr(w.d, w.n);
    iron_mem_free(w.d);
    return out;
}
