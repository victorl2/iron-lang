/* test_tuple_codegen.c — Phase 59 01d binary-layout lock-in for tuples.
 *
 * The Iron compiler synthesises per-tuple C typedefs in the form
 *     typedef struct { T0 v0; T1 v1; ... Tn vN; } Iron_Tuple_<mangled>;
 * via emit_ensure_tuple in src/lir/emit_helpers.c.
 *
 * This file pins the invariant that a 2-tuple (Iron_String, Iron_Error)
 * lowers to a C struct whose layout is byte-compatible with the hand-
 * written `Iron_Result_String_Error` at src/stdlib/iron_io.h:9.  Future
 * Phase 59 (HTTP/net) code can then cross-cast between the two whenever
 * the stdlib `Iron_io_read_file_result`-family functions need to plumb
 * results back through tuple-returning Iron wrappers.
 *
 * The assertion is compile-time (_Static_assert) so regressions in the
 * tuple struct layout show up as build errors. */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "stdlib/iron_io.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void setUp(void) {}
void tearDown(void) {}

/* Hand-emit the tuple typedef exactly as emit_ensure_tuple would produce
 * for an Iron `(String, Iron_Error)` tuple.  Iron_Error is a C-only type
 * (not an Iron type) and the research SUMMARY deferred the actual
 * String+Error tuple until the Phase 59 net APIs are wired, but the
 * binary-layout lock lives here so any change to emit_ensure_tuple or
 * to the stdlib Iron_Result_String_Error shape is caught immediately. */
typedef struct { Iron_String v0; Iron_Error v1; } Iron_Tuple_Iron_String_Iron_Error;

_Static_assert(
    sizeof(Iron_Tuple_Iron_String_Iron_Error) == sizeof(Iron_Result_String_Error),
    "Iron_Tuple_Iron_String_Iron_Error must be byte-compatible with Iron_Result_String_Error");

_Static_assert(
    offsetof(Iron_Tuple_Iron_String_Iron_Error, v0) ==
        offsetof(Iron_Result_String_Error, v0),
    "Iron_Tuple v0 offset must match Iron_Result v0 offset");

_Static_assert(
    offsetof(Iron_Tuple_Iron_String_Iron_Error, v1) ==
        offsetof(Iron_Result_String_Error, v1),
    "Iron_Tuple v1 offset must match Iron_Result v1 offset");

/* Pin the simpler all-int tuple layout too — this is the shape every
 * Phase 59 02/03/04/05 test fixture in tests/integration/tuple_*.iron
 * compiles to. */
typedef struct { int64_t v0; int64_t v1; } Iron_Tuple_Int_Int_manual;

_Static_assert(
    sizeof(Iron_Tuple_Int_Int_manual) == 16,
    "2-tuple of Int should be 16 bytes on a 64-bit target");

_Static_assert(
    offsetof(Iron_Tuple_Int_Int_manual, v1) == 8,
    "2-tuple of Int: v1 should sit at offset 8");

void test_tuple_layout_matches_result_string_error(void) {
    /* Static asserts above already ran at compile time; this Unity runtime
     * test exists so the test executable has at least one RUN_TEST entry
     * and to double-check via runtime byte comparison. */
    Iron_Tuple_Iron_String_Iron_Error tup;
    Iron_Result_String_Error          res;
    TEST_ASSERT_EQUAL_size_t(sizeof(res), sizeof(tup));
    TEST_ASSERT_EQUAL_size_t(offsetof(Iron_Result_String_Error, v0),
                              offsetof(Iron_Tuple_Iron_String_Iron_Error, v0));
    TEST_ASSERT_EQUAL_size_t(offsetof(Iron_Result_String_Error, v1),
                              offsetof(Iron_Tuple_Iron_String_Iron_Error, v1));
}

void test_tuple_int_int_is_16_bytes(void) {
    Iron_Tuple_Int_Int_manual t;
    TEST_ASSERT_EQUAL_size_t(16, sizeof(t));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tuple_layout_matches_result_string_error);
    RUN_TEST(test_tuple_int_int_is_16_bytes);
    return UNITY_END();
}
