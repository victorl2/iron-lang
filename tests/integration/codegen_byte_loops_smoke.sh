#!/usr/bin/env bash
# The generated C of byte scanning loops and constant tables (#658), through
# the real CLI: xs.len() reads the list count in place (no out-of-line
# Iron_List_<T>_len call), String byte accessors are the inline by-address
# forms, bounds checks compare unsigned, and a global val of constant data
# is a static const with its initializer and no startup assignment.
#
# Usage: codegen_byte_loops_smoke.sh [IRONC_BIN]
set -euo pipefail
IRONC="${1:-./build/ironc}"
IRONC="$(cd "$(dirname "${IRONC}")" && pwd)/$(basename "${IRONC}")"
SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT
cd "${SANDBOX}"
cat > loops.iron <<'IRON'
val POW: [UInt64; 4] = [1, 10, 100, 1000]
val SMALL: [UInt8; <=4] = [1, 2]
val L = [1, 2, 3]
val NAMES = ["a", "a string longer than twenty three bytes"]
val S = "hello"
val N = 64
var COUNTER = 0

func sum(xs: [UInt8]) -> Int {
    var t = 0
    var i = 0
    while i < xs.len() {
        t += Int(xs[i])
        i += 1
    }
    return t
}

func quotes(s: String) -> Int {
    var c = 0
    var i = 0
    while i < s.byte_len() {
        if s.byte_at(i) == 34 {
            c += 1
        }
        i += 1
    }
    return c + s.len()
}

func pick(b: [Int; <=4], i: Int) -> Int {
    return b[i]
}

func main() {
    var xs: [UInt8] = [1, 2, 3]
    xs.push(4)
    var b: [Int; <=4] = [5]
    b.push(6)
    COUNTER += 1
    println("{sum(xs)} {quotes(S)} {pick(b, 1)} {POW[2]} {SMALL[1]} {L[1]} {NAMES[1]} {N} {COUNTER}")
}
IRON
fail=0
want() { grep -qF -- "$1" .iron-build/loops.c || { echo "FAIL: missing in generated C: $1"; fail=1; }; }
lack() { ! grep -qF -- "$1" .iron-build/loops.c || { echo "FAIL: unexpected in generated C: $1"; fail=1; }; }
"${IRONC}" build --release --emit-c loops.iron >/dev/null 2>&1
want ")->count;"
lack "Iron_List_uint8_t_len("
want "iron_string_byte_at_p(&"
want "iron_string_byte_len_p(&"
want "iron_string_len_p(&"
lack "Iron_string_byte_at("
want ") >= (uint64_t)"
want "static const Iron_BVec_uint64_t_4 Iron_g_POW = { .len = 4, .data = {"
want "static const Iron_BVec_uint8_t_4 Iron_g_SMALL = { .len = 2, .data = {"
want "static const int64_t Iron_g_L__items[3] = {"
want "static const Iron_List_int64_t Iron_g_L = {"
want "static const Iron_String Iron_g_NAMES__items[2] = {"
want ".flags = 0x03 } }"
want "static const Iron_String Iron_g_S = { .sso = { .data = \"hello\", .len = 5 } };"
want "static const int64_t Iron_g_N = "
want "static int64_t Iron_g_COUNTER;"
lack "Iron_List_int64_t_push(&"
out="$("${IRONC}" run loops.iron)"
[ "${out}" = "10 5 6 100 2 2 a string longer than twenty three bytes 64 1" ] ||
    { echo "FAIL: program printed: ${out}"; fail=1; }
[ "${fail}" -eq 0 ] && echo "codegen_byte_loops_smoke: PASS"
exit "${fail}"
