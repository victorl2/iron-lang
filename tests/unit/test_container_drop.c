/* test_container_drop.c — Phase 33 Wave 0 RED anchor (STDLIB-02 element dtors).
 *
 * Asserts the container element-destructor codegen contract: when a List[T]'s
 * element type T has a user `drop` (od_has_drop_lir true), the monomorphized
 * `_free` must iterate and call `Iron_<Elem>_drop` per element (and `_clone`
 * must call `Iron_<Elem>_copy`); primitive / trivial element types keep the
 * fast `free(items)` / memcpy path (Pitfall 5).
 *
 * Intentionally RED until Wave 4 lands the element-destructor-aware
 * monomorphization in emit_helpers.c / emit_c.c. The harness compiles + links
 * today; Wave 4 replaces the TEST_FAIL bodies with emit-side assertions that
 * grep the generated C for the per-element Iron_<Elem>_drop loop.
 */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* RED: List[T] _free must call the element drop for managed element types. */
static void test_list_drop_invokes_element_destructor(void) {
    TEST_FAIL_MESSAGE(
        "RED: Wave 4 must emit a per-element Iron_<Elem>_drop loop in the "
        "List[T] _free monomorphization when od_has_drop_lir(elem) is true.");
}

/* RED: List[T] _clone must deep-copy managed elements via Iron_<Elem>_copy. */
static void test_list_copy_invokes_element_copy(void) {
    TEST_FAIL_MESSAGE(
        "RED: Wave 4 must emit a per-element Iron_<Elem>_copy loop in the "
        "List[T] _clone monomorphization when the element has a copy block.");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_list_drop_invokes_element_destructor);
    RUN_TEST(test_list_copy_invokes_element_copy);
    return UNITY_END();
}
