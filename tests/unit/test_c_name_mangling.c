#include "unity.h"
#include "lir/emit_c.h"
#include "util/arena.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_safe_names_are_byte_identical(void) {
    Iron_Arena arena = iron_arena_create(1024);
    const char *safe[] = { "field", "snake_case", "camelCase", "x1", "_plain" };
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); i++) {
        const char *got = iron_lir_emit_c_user_identifier(safe[i], &arena);
        TEST_ASSERT_EQUAL_PTR(safe[i], got);
        TEST_ASSERT_EQUAL_STRING(safe[i], got);
    }
    iron_arena_free(&arena);
}

static void test_keywords_macros_and_prefixes_are_encoded(void) {
    Iron_Arena arena = iron_arena_create(4096);
    const char *reserved[] = {
        "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex",
        "_Decimal128", "_Decimal32", "_Decimal64", "_Generic", "_Imaginary",
        "_Noreturn", "_Pragma", "_Static_assert", "_Thread_local",
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
        "bitor", "bool", "break", "case", "char", "compl", "const",
        "constexpr", "continue", "default", "do", "double", "else", "enum",
        "extern", "false", "float", "for", "goto", "if", "inline", "int",
        "long", "not", "not_eq", "nullptr", "or", "or_eq", "register",
        "restrict", "return", "short", "signed", "sizeof", "static",
        "static_assert", "struct", "switch", "thread_local", "true", "typedef",
        "typeof", "typeof_unqual", "union", "unsigned", "void", "volatile",
        "while", "xor", "xor_eq",
        "NULL", "errno", "stdin", "stdout", "stderr", "assert", "offsetof",
        "min", "max", "near", "far", "interface", "_WIN32",
        "Iron_List", "iron_mem_alloc", "IRON_SITE", "__lambda_0", "_v12",
        "_ref_value"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        const char *got = iron_lir_emit_c_user_identifier(reserved[i], &arena);
        TEST_ASSERT_EQUAL_INT(0, strncmp(got, "Iron__id_", 9));
        TEST_ASSERT_NOT_EQUAL(0, strcmp(got, reserved[i]));
    }
    iron_arena_free(&arena);
}

static void test_encoding_is_stable_injective_and_self_reserving(void) {
    Iron_Arena arena = iron_arena_create(4096);
    const char *a = iron_lir_emit_c_user_identifier("default", &arena);
    const char *b = iron_lir_emit_c_user_identifier("register", &arena);
    const char *again = iron_lir_emit_c_user_identifier("default", &arena);
    TEST_ASSERT_EQUAL_STRING("Iron__id_64656661756c74", a);
    TEST_ASSERT_EQUAL_STRING(a, again);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
    TEST_ASSERT_NOT_EQUAL(0,
        strcmp(a, iron_lir_emit_c_user_identifier(a, &arena)));
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_safe_names_are_byte_identical);
    RUN_TEST(test_keywords_macros_and_prefixes_are_encoded);
    RUN_TEST(test_encoding_is_stable_injective_and_self_reserving);
    return UNITY_END();
}
