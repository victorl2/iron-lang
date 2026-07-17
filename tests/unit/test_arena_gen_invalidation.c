/* test_arena_gen_invalidation.c — Phase 28 headline soundness guarantee.
 *
 * Wave 0 RED for Plan 28-01 Task 1; flips GREEN when Plan 28-02 lands the
 * arena runtime (src/runtime/iron_arena_rt.{c,h}) AND the arena-aware deref
 * guard `iron_check_arena_pointer_gen(Iron_FatPtr, file, line)` (static-inline
 * in iron_runtime.h per 28-RESEARCH.md §A).
 *
 * THE soundness boundary (28-CONTEXT.md GA1 + <specifics>): a fat pointer
 * obtained BEFORE reset()/restore() and dereferenced AFTER must PANIC.
 * Arena reset()/restore() bump the arena-level shared generation counter once
 * for O(1) mass-invalidation; the fat pointer holds the gen snapshot taken at
 * allocation; iron_check_arena_pointer_gen compares snapshot vs the arena's
 * current generation and panics on mismatch.
 *
 *   - test_deref_after_reset_panics:   alloc fp; reset(); deref fp -> PANIC
 *   - test_deref_after_restore_panics: alloc base; save; alloc fp2;
 *                                      restore(save); deref fp2 -> PANIC;
 *                                      deref base (pre-save) still PASSES.
 *
 * Panic-capture mechanism (documented per <action>): fork()-per-case +
 * SIGABRT capture, identical to tests/unit/test_runtime_panic_stale.c
 * (Pitfall 9 — fork-per-case captures abort cleanly). The child triggers the
 * arena-stale deref via iron_check_arena_pointer_gen, which calls the
 * noreturn panic path (abort()); the parent asserts WIFSIGNALED + SIGABRT.
 * A "should pass" case runs the check in-process WITHOUT forking (it must NOT
 * abort) — if it did abort, the test binary itself would die and Unity would
 * report the failure.
 *
 * POSIX-only (fork/pipe/waitpid/SIGABRT); Win32 stubs out with TEST_IGNORE.
 */

#include "unity.h"

#ifdef _WIN32

void setUp(void)    {}
void tearDown(void) {}

void test_phase28_arena_panic_capture_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 28 arena gen-invalidation tests are POSIX-only "
                        "(fork/pipe/waitpid); Win32 path skipped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase28_arena_panic_capture_posix_only);
    return UNITY_END();
}

#else  /* POSIX */

#include "runtime/iron_runtime.h"
#include "runtime/iron_arena_rt.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── fork-per-case panic-capture helper (mirrors test_runtime_panic_stale.c) ─
 *
 * Runs child_fn in a child process; asserts the child died via SIGABRT
 * (the noreturn arena-stale panic path). _exit(99) is the safety hatch:
 * if child_fn returns WITHOUT aborting, the parent's WIFSIGNALED/SIGABRT
 * asserts fail loudly instead of the test silently passing. */
static void run_arena_panic_case(void (*child_fn)(void)) {
    pid_t pid = fork();
    TEST_ASSERT_NOT_EQUAL(-1, pid);
    if (pid == 0) {
        /* child */
        child_fn();
        _exit(99);  /* unreachable: child_fn must abort */
    }
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status),
        "arena-stale deref must abort (not return cleanly)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIGABRT, WTERMSIG(status),
        "arena-stale deref must panic via SIGABRT");
}

/* ── Child workloads ──────────────────────────────────────────────────── */

/* ARENA-06 — deref a pointer after arena.reset() must panic. */
static void child_deref_after_reset(void) {
    iron_runtime_init(0, NULL);
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "reset_arena");
    Iron_FatPtr fp = iron_arena_rt_alloc(a, 32);
    iron_arena_rt_reset(a);  /* bumps arena generation — fp now stale */
    iron_check_arena_pointer_gen(fp, __FILE__, __LINE__);  /* must PANIC */
    /* unreachable */
}

/* ARENA-07 — deref a pointer allocated after save(), after restore(),
 * must panic (restore bumps the generation). */
static void child_deref_after_restore(void) {
    iron_runtime_init(0, NULL);
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "restore_arena");
    (void)iron_arena_rt_alloc(a, 16);          /* baseline alloc (kept) */
    Iron_ArenaSave save = iron_arena_rt_save(a);
    Iron_FatPtr fp2 = iron_arena_rt_alloc(a, 16);  /* allocated AFTER save */
    iron_arena_rt_restore(a, save);            /* bumps generation — fp2 stale */
    iron_check_arena_pointer_gen(fp2, __FILE__, __LINE__);  /* must PANIC */
    /* unreachable */
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

void test_deref_after_reset_panics(void) {
    run_arena_panic_case(child_deref_after_reset);
}

void test_deref_after_restore_panics(void) {
    run_arena_panic_case(child_deref_after_restore);
}

/* Companion positive boundary: a pointer allocated BEFORE the save point is
 * NOT invalidated by restore() (its gen snapshot still matches), so deref
 * must NOT panic. Run in-process: if it aborts, this test binary dies and
 * Unity reports the failure — that is the intended negative-of-panic check. */
void test_deref_before_save_survives_restore(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "restore_arena");
    Iron_FatPtr base = iron_arena_rt_alloc(a, 16);   /* allocated BEFORE save */
    Iron_ArenaSave save = iron_arena_rt_save(a);
    (void)iron_arena_rt_alloc(a, 16);
    iron_arena_rt_restore(a, save);
    /* base predates the save point — restore must NOT invalidate it. */
    iron_check_arena_pointer_gen(base, __FILE__, __LINE__);  /* must NOT panic */
    TEST_PASS();
    iron_arena_rt_destroy(a);
}

/* ── Unity entrypoint ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_deref_after_reset_panics);
    RUN_TEST(test_deref_after_restore_panics);
    RUN_TEST(test_deref_before_save_survives_restore);
    return UNITY_END();
}

#endif  /* !_WIN32 */
