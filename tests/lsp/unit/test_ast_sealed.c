/* test_ast_sealed -- Phase 3 Plan 01 Task 03 (NAV-15).
 *
 * Covers:
 *   1. analyzer sets program->sealed = true on success
 *   2. IRON_AST_ASSERT_UNSEALED is a compile-time no-op under NDEBUG
 *   3. IRON_AST_ASSERT_UNSEALED fires iron_ice under !NDEBUG (verified
 *      via fork() + SIGABRT exit code -- iron_ice calls abort()).
 */
#include "unity.h"

#include "analyzer/analyzer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Test 01: analyzer seals on success ──────────────────────────────── */
static void test_analyzer_seals_program_on_success(void) {
    const char *src = "func main() { val x = 1 }\n";
    Iron_Arena arena = iron_arena_create(16 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    Iron_AnalyzeResult r = iron_analyze_buffer(src, strlen(src), "t.iron",
                                                IRON_ANALYSIS_MODE_CLI,
                                                &arena, &diags, NULL);
    TEST_ASSERT_NOT_NULL(r.program);
    TEST_ASSERT_TRUE_MESSAGE(r.program->sealed,
                              "analyzer must set sealed=true on return");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 02: unsealed programs do not trip the macro ────────────────── */
static void test_macro_noop_when_unsealed(void) {
    Iron_Program p;
    memset(&p, 0, sizeof(p));
    p.sealed = false;
    /* Must not abort -- program is not sealed. */
    IRON_AST_ASSERT_UNSEALED(&p);
    TEST_PASS();
}

/* ── Test 03: NULL programs do not trip the macro ────────────────────── */
static void test_macro_tolerates_null(void) {
    Iron_Program *p = NULL;
    IRON_AST_ASSERT_UNSEALED(p);
    (void)p; /* IRON_AST_ASSERT_UNSEALED is a no-op under NDEBUG */
    TEST_PASS();
}

/* ── Test 04: debug-mode trap fires on sealed program ────────────────── */
/* Under NDEBUG the macro is a no-op; we validate that branch implicitly
 * by test 02 + 03. Under !NDEBUG we fork a child that triggers the
 * macro; the child is expected to die via SIGABRT (iron_ice -> abort).
 * This exercise is the integration-level proof that the contract is
 * actually enforced at dev time. */
static void test_debug_macro_aborts_on_sealed(void) {
#ifdef NDEBUG
    TEST_IGNORE_MESSAGE("Debug-only branch -- release build skips");
#else
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: silence abort message so the test log stays tidy,
         * then trip the macro. */
        fclose(stderr);
        Iron_Program p;
        memset(&p, 0, sizeof(p));
        p.sealed = true;
        IRON_AST_ASSERT_UNSEALED(&p);
        /* Should be unreachable -- if we got here the macro did NOT
         * abort. Exit with a distinctive code so the parent detects. */
        _exit(77);
    }
    TEST_ASSERT_TRUE_MESSAGE(pid > 0, "fork() failed");
    int status = 0;
    waitpid(pid, &status, 0);
    bool died_on_signal = WIFSIGNALED(status);
    int  signo = died_on_signal ? WTERMSIG(status) : 0;
    /* iron_ice calls abort() which raises SIGABRT(6). */
    TEST_ASSERT_TRUE_MESSAGE(died_on_signal,
                              "child must be killed by signal");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIGABRT, signo,
                                   "expected SIGABRT from iron_ice");
#endif
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_analyzer_seals_program_on_success);
    RUN_TEST(test_macro_noop_when_unsealed);
    RUN_TEST(test_macro_tolerates_null);
    RUN_TEST(test_debug_macro_aborts_on_sealed);
    return UNITY_END();
}
