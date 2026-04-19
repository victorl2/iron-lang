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
 *   Float — libc %.6g semantics (6 significant digits, trailing zeros trimmed);
 *           special values: NaN → "NaN", +inf → "inf", -inf → "-inf",
 *           -0.0 → "0" (canonicalized after sign stripping for zero magnitude).
 *
 * Buffer sizes:
 *   Int   — 24 bytes (INT64_MIN needs 21 incl sign + nul; round up for margin)
 *   Int32 — 16 bytes (INT32_MIN needs 12 incl sign + nul)
 *   Float — 32 bytes (%.6g worst case ~14 chars: "-1.23457e-308" + margin)
 */

#include <stdio.h>
#include <math.h>
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

Iron_String Iron_float_to_string(double f) {
    /* Special values first — libc's %g handling is platform-dependent
     * for NaN/Inf (glibc emits "nan"/"inf", musl "nan"/"inf", Windows
     * historically "1.#QNAN"). Normalize to Iron's convention per
     * CONTEXT.md: "NaN" / "inf" / "-inf". */
    if (isnan(f))                 return iron_string_from_cstr("NaN",  3);
    if (isinf(f) && f > 0)        return iron_string_from_cstr("inf",  3);
    if (isinf(f) && f < 0)        return iron_string_from_cstr("-inf", 4);

    /* -0.0 canonicalization: trim the sign so Iron_float_to_string(-0.0)
     * returns "0" (matches CONTEXT.md's "trim trailing zeros rule applies
     * after sign stripping for zero magnitude"). IEEE 754 §5.11 guarantees
     * that the equality `f == 0.0` also matches -0.0, so this single branch
     * handles both zero encodings. */
    if (f == 0.0) return iron_string_from_cstr("0", 1);

    char buf[32];
    /* %.6g — 6 significant digits, trailing zeros trimmed, scientific
     * form for magnitudes < 1e-4 or >= 1e+6 (standard libc behavior).
     * This matches the v2.2 CONTEXT.md decision: "libc %g semantics for
     * C-backend parity". No precision parameter this phase. */
    int len = snprintf(buf, sizeof(buf), "%.6g", f);
    if (len < 0 || len >= (int)sizeof(buf)) {
        return iron_string_from_cstr("", 0);
    }
    return iron_string_from_cstr(buf, (size_t)len);
}
