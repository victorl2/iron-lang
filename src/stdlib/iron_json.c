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
    while (i < n) {
        unsigned char c = p[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') i++;
        else break;
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
    int dropped = 0;       /* integer digits that did not fit in mant */
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
    int frac_digits = 0;
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
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            if (exp10 < 100000) exp10 = exp10 * 10 + (p[i] - '0');
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
    j.max_depth = max_depth > 0 ? max_depth : 1;

    if (max_bytes >= 0 && (int64_t)j.len > max_bytes) {
        js_fail(&j, IRON_ERR_JSON_TOO_LARGE, 0, "input is larger than the size limit");
    } else {
        /* Rough first reservation: one tape word per 6 input bytes. */
        size_t guess = j.len / 6 + 16;
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

/* Shortest decimal digits of finite, nonzero v that read back to v:
 * fills digits (no dot, no trailing zeros), returns the count and sets
 * *exp so that v = 0.d1d2... * 10^(exp+1), i.e. exp is the decimal
 * exponent of the first digit. Any decimal of at most 15 significant
 * digits survives a round trip (DBL_DIG), and the correctly rounded
 * 16- and 17-digit forms are the closest of their length, so the first
 * of the three that reads back is the shortest. */
static int json_shortest(double v, char *digits, int *exp, int *neg) {
    char sci[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(sci, sizeof(sci), "%.*e", prec - 1, v);
        if (prec == 17 || strtod(sci, NULL) == v) break;
    }
    const char *c = sci;
    *neg = (*c == '-');
    if (*neg) c++;
    int n = 0;
    for (; *c && *c != 'e'; c++) {
        if (*c >= '0' && *c <= '9') digits[n++] = *c;
    }
    *exp = (*c == 'e') ? atoi(c + 1) : 0;
    while (n > 1 && digits[n - 1] == '0') n--;
    digits[n] = '\0';
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
