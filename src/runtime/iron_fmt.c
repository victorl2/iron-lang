/* src/runtime/iron_fmt.c — Phase 78 FMT: Int/Int32/Float → String conversion.
 *
 * Three runtime shims consumed by Iron's stdlib stubs in src/stdlib/int.iron
 * and src/stdlib/float.iron (landed in Plan 78-02). Each shim formats its
 * numeric argument into a stack buffer via snprintf, then wraps the result
 * in an Iron_String via iron_string_from_cstr.
 *
 * Semantics (per Phase 78 CONTEXT.md):
 *   Int   — signed 64-bit decimal; INT64_MIN → "-9223372036854775808" (20 chars)
 *   Int32 — signed 32-bit decimal; INT32_MIN → "-2147483648"          (11 chars)
 *   Float — shortest round-trip digits (see iron_fmt_float);
 *           special values: NaN → "NaN", +inf → "inf", -inf → "-inf",
 *           -0.0 → "0" (canonicalized after sign stripping for zero magnitude).
 *
 * Buffer sizes:
 *   Int   — 24 bytes (INT64_MIN needs 21 incl sign + nul; round up for margin)
 *   Int32 — 16 bytes (INT32_MIN needs 12 incl sign + nul)
 *   Float — IRON_FMT_FLOAT_BUF bytes (worst case "-0.0000001234567890123456"
 *           or "-1.2345678901234567e-308", under 32 chars)
 */

#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "iron_runtime.h"

Iron_String Iron_int_to_string(int64_t n) {
    char buf[24];
    /* PRId64 would be cleaner but requires <inttypes.h> + ensuring
     * the runtime builds against that header; %lld is universally
     * supported and matches int64_t on every platform we target
     * (macOS arm64/x86_64, Linux x86_64, Windows — verified against
     * gsd-tools emit_type_to_c mapping where IRON_TYPE_INT = int64_t). */
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)n);
    if (len < 0 || len >= (int)sizeof(buf)) {
        /* Unreachable: INT64_MIN fits in 20 chars + sign + nul = 21 < 24.
         * Fall back to empty string rather than risk undefined behavior. */
        return iron_string_from_cstr("", 0);
    }
    return iron_string_from_cstr(buf, (size_t)len);
}

Iron_String Iron_int32_to_string(int32_t n) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", (int)n);
    if (len < 0 || len >= (int)sizeof(buf)) {
        return iron_string_from_cstr("", 0);
    }
    return iron_string_from_cstr(buf, (size_t)len);
}

/* Shortest round-trip decimal form of v (the digits Python's repr and
 * JavaScript's Number.toString choose): the fewest significant digits that
 * parse back to exactly v, or to exactly (float)v when is_f32.  Layout
 * follows JavaScript: plain decimal for exponents -6..20 with no trailing
 * ".0" on whole values, scientific ("1e+21", "1.5e-7") outside that range.
 * NaN -> "NaN", infinities -> "inf" / "-inf", both zeros -> "0".
 * out must hold IRON_FMT_FLOAT_BUF bytes; returns out. */
const char *iron_fmt_float(double v, bool is_f32, char *out) {
    if (isnan(v))            { strcpy(out, "NaN");  return out; }
    if (isinf(v) && v > 0)   { strcpy(out, "inf");  return out; }
    if (isinf(v))            { strcpy(out, "-inf"); return out; }
    if (v == 0.0)            { strcpy(out, "0");    return out; }

    char sci[40];
    int max_p = is_f32 ? 9 : 17;
    for (int p = 1; p <= max_p; p++) {
        snprintf(sci, sizeof(sci), "%.*e", p - 1, v);
        double back = strtod(sci, NULL);
        if (is_f32 ? ((float)back == (float)v) : (back == v)) break;
    }

    /* sci is "[-]d[.ddd]e(+|-)XX": split into sign, digits, exponent. */
    const char *c = sci;
    bool neg = (*c == '-');
    if (neg) c++;
    char digits[24];
    int n = 0;
    for (; *c && *c != 'e'; c++) {
        if (*c >= '0' && *c <= '9' && n < (int)sizeof(digits) - 1)
            digits[n++] = *c;
    }
    int e = (*c == 'e') ? atoi(c + 1) : 0;
    while (n > 1 && digits[n - 1] == '0') n--;
    digits[n] = '\0';

    char *o = out;
    if (neg) *o++ = '-';
    if (e >= -6 && e < 21) {
        if (e >= 0) {
            for (int i = 0; i <= e; i++) *o++ = (i < n) ? digits[i] : '0';
            if (n > e + 1) {
                *o++ = '.';
                for (int i = e + 1; i < n; i++) *o++ = digits[i];
            }
        } else {
            *o++ = '0';
            *o++ = '.';
            for (int i = 0; i < -e - 1; i++) *o++ = '0';
            for (int i = 0; i < n; i++) *o++ = digits[i];
        }
        *o = '\0';
    } else {
        *o++ = digits[0];
        if (n > 1) {
            *o++ = '.';
            for (int i = 1; i < n; i++) *o++ = digits[i];
        }
        snprintf(o, 8, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    }
    return out;
}

Iron_String Iron_float_to_string(double f) {
    char buf[IRON_FMT_FLOAT_BUF];
    iron_fmt_float(f, false, buf);
    return iron_string_from_cstr(buf, strlen(buf));
}

/* ── printf-style formatting for generated code ─────────────────────────── */

char *iron_cstr_format(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) iron_oom_abort("iron_cstr_format");
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

Iron_String iron_string_format(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) iron_oom_abort("iron_string_format");
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    Iron_String s = iron_string_from_cstr(buf, (size_t)n);
    free(buf);
    return s;
}
