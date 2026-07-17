/* test_runtime_panic_stale.c — Phase 19 Plan 19-02 SAFE-03/04/06.
 *
 * Wave 0 RED→GREEN tests for the iron_panic_stale_pointer mechanism. Each
 * RUN_TEST forks a child process that triggers the panic; the parent waits
 * for SIGABRT (matches iron_oom_abort precedent) and reads piped child
 * stderr to assert exact format substrings. Pattern follows
 * tests/unit/test_runtime_threads.c (pthread harness) adapted for fork-per-
 * case panic-capture (Pitfall 9 — fork()-per-case captures abort cleanly).
 *
 * Coverage map:
 *   - SAFE-03 — deref-check fires panic on gen mismatch (2 tests)
 *   - SAFE-04 — release build retains the check (1 symbol-presence test;
 *               release-mode build via separate test_runtime_panic_stale_release
 *               CTest entry compiled with -DNDEBUG=1 -O2)
 *   - SAFE-06 — text format assertions (2 tests; alloc-site fields debug-only)
 *   - SAFE-06 — JSON format assertions (3 tests; alloc-site/allocation null in
 *               release; present in debug)
 *   - Pitfall 4 — env cached at iron_runtime_init, never re-read on panic path
 *                 (1 test)
 *
 * POSIX-only (uses fork/pipe/waitpid/SIGABRT). Win32 path stubs out cleanly
 * with a single TEST_IGNORE_MESSAGE.
 */

#include "unity.h"

#ifdef _WIN32

void setUp(void)    {}
void tearDown(void) {}

void test_phase19_panic_capture_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 19 panic-capture tests are POSIX-only "
                        "(fork/pipe/waitpid); Win32 path skipped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase19_panic_capture_posix_only);
    return UNITY_END();
}

#else  /* POSIX */

#include "runtime/iron_runtime.h"
#include "runtime/iron_heap_track.h"
#include "runtime/iron_panic.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Fork-per-case panic-capture helper ──────────────────────────────────── */

/* Runs `child_fn(child_arg)` in a child process; captures child stderr into
 * stderr_buf (max stderr_buf_cap-1 bytes, NUL-terminated). Returns child wait
 * status via *status_out. Asserts (parent side) that child exited via
 * SIGABRT.
 *
 * Pattern: Pitfall 9 fork()-per-case captures abort cleanly. The child must
 * NOT return — it must call iron_panic_stale_pointer (or similar) which
 * calls abort(). _exit(99) is the safety hatch that makes a misbehaving
 * test fail loudly rather than hang. */
static void run_panic_case(void (*child_fn)(void *),
                           void *child_arg,
                           char *stderr_buf,
                           size_t stderr_buf_cap,
                           int *status_out) {
    int err_pipe[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(err_pipe));
    pid_t pid = fork();
    TEST_ASSERT_NOT_EQUAL(-1, pid);
    if (pid == 0) {
        /* child */
        close(err_pipe[0]);
        dup2(err_pipe[1], 2);
        close(err_pipe[1]);
        child_fn(child_arg);
        _exit(99);  /* should never reach: child_fn must abort */
    }
    /* parent */
    close(err_pipe[1]);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < stderr_buf_cap &&
           (n = read(err_pipe[0], stderr_buf + total,
                     stderr_buf_cap - 1 - total)) > 0) {
        total += (size_t)n;
    }
    stderr_buf[total] = '\0';
    close(err_pipe[0]);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFSIGNALED(status));
    TEST_ASSERT_EQUAL_INT(SIGABRT, WTERMSIG(status));
    if (status_out) *status_out = status;
}

/* ── Child workloads ─────────────────────────────────────────────────────── */

/* Stale-deref via iron_check_pointer_gen after free. */
static void child_stale_deref(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 32);
    Iron_FatPtr fp_copy = fp;
    iron_heap_free(fp);
    /* Now fp_copy is stale; the check must panic. */
    iron_check_pointer_gen(fp_copy, "deref_test.c", 4242);
    /* Unreachable. */
}

/* NULL-fp dereference via iron_check_pointer_gen. */
static void child_null_deref(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    Iron_FatPtr null_fp = (Iron_FatPtr){NULL, 0};
    iron_check_pointer_gen(null_fp, "null_deref_test.c", 9999);
    /* Unreachable. */
}

/* Stale-deref under JSON env (set BEFORE iron_runtime_init). */
static void child_stale_deref_json(void *unused) {
    (void)unused;
    setenv("IRON_PANIC_FORMAT", "json", 1);
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 16);
    Iron_FatPtr fp_copy = fp;
    iron_heap_free(fp);
    iron_check_pointer_gen(fp_copy, "json_deref.c", 7777);
    /* Unreachable. */
}

/* Stale-deref under cache-at-init defense: env is set AFTER init.
 * Result must be TEXT (cached "text" at init wins; later setenv ignored). */
static void child_late_setenv_ignored(void *unused) {
    (void)unused;
    /* Ensure env is unset BEFORE init. */
    unsetenv("IRON_PANIC_FORMAT");
    iron_runtime_init(0, NULL);
    /* Now flip env after caching — must be ignored on the panic path. */
    setenv("IRON_PANIC_FORMAT", "json", 1);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 8);
    Iron_FatPtr fp_copy = fp;
    iron_heap_free(fp);
    iron_check_pointer_gen(fp_copy, "late_setenv.c", 1234);
    /* Unreachable. */
}

/* ── SAFE-03 — deref-check fires panic on gen mismatch ───────────────────── */

void test_iron_check_pointer_gen_panics_on_stale(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_stale_deref, NULL, buf, sizeof(buf), &status);
    /* Default is text; assert the spec-locked header. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: stale pointer dereference"),
                                 "stderr must contain text-format header");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "deref_test.c:4242"),
                                 "stderr must contain caller's deref site");
}

void test_iron_check_pointer_gen_panics_on_null(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_null_deref, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: stale pointer dereference"),
                                 "NULL fp must trigger the same panic header");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "null_deref_test.c:9999"),
                                 "NULL fp panic must carry deref site");
}

/* ── SAFE-04 — release build retains the check ──────────────────────────── */

void test_iron_panic_stale_pointer_symbol_present(void) {
    /* Taking the address verifies the symbol is linked into both Debug and
     * Release builds. The release-mode CTest entry
     * `test_runtime_panic_stale_release` compiles this same source file
     * with -DNDEBUG=1 -O2 and runs the panic cases — exercising SAFE-04
     * end-to-end (no --release-skip-checks flag exists).
     *
     * We compare via a function-pointer typedef rather than casting to
     * void* — ISO C 6.3.2.3 forbids the latter conversion (clang/gcc emit
     * -Wpedantic on the cast). */
    typedef void (*panic_fn_t)(const char *, int, const struct IronAllocHdr *);
    panic_fn_t fp = &iron_panic_stale_pointer;
    TEST_ASSERT_TRUE(fp != NULL);
}

/* ── SAFE-06 — text format assertions (default channel) ─────────────────── */

void test_iron_panic_stale_text_deref_site(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_stale_deref, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: stale pointer dereference\n"),
                                 "text header must end in newline (multi-line block)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "  deref site: "),
                                 "text format must have indented deref-site line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "deref_test.c:4242"),
                                 "text deref-site must carry file:line");
}

void test_iron_panic_stale_text_alloc_site_debug_only(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_stale_deref, NULL, buf, sizeof(buf), &status);
#ifdef IRON_DEBUG_ALLOCATOR
    /* Debug build: assert allocation-site + size present. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "  allocation site: "),
                                 "debug build must emit allocation-site line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "  allocation: "),
                                 "debug build must emit allocation id+size line");
#else
    /* Release build: assert allocation-site + size ABSENT. */
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "  allocation site: "),
                             "release build must NOT emit allocation-site line");
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "  allocation: "),
                             "release build must NOT emit allocation id+size line");
#endif
}

/* ── SAFE-06 — JSON format assertions (env-overridden channel) ──────────── */

void test_iron_panic_stale_json_kind_field(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_stale_deref_json, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"kind\":\"stale_pointer\""),
                                 "JSON output must carry kind=stale_pointer");
    /* Single-line: only one trailing newline at the end (no embedded \n
     * before final '}'). */
    char *first_nl = strchr(buf, '\n');
    TEST_ASSERT_NOT_NULL_MESSAGE(first_nl,
                                 "JSON output must end with a newline");
    /* Trailing newline must be at-or-after the closing '}'. */
    char *last_brace = strrchr(buf, '}');
    TEST_ASSERT_NOT_NULL_MESSAGE(last_brace,
                                 "JSON output must contain a closing brace");
    TEST_ASSERT_TRUE_MESSAGE(first_nl > last_brace - 1,
                             "JSON must be single-line (newline at/after '}')");
}

void test_iron_panic_stale_json_deref_site_object(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_stale_deref_json, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"deref_site\":{\"file\":\""),
                                 "JSON deref_site must be an object with file field");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "json_deref.c"),
                                 "JSON deref_site must carry caller filename");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"line\":7777"),
                                 "JSON deref_site must carry caller line as integer");
}

void test_iron_panic_stale_json_alloc_site_payload(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_stale_deref_json, NULL, buf, sizeof(buf), &status);
#ifdef IRON_DEBUG_ALLOCATOR
    /* Debug build: alloc_site + allocation are objects with fields. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"alloc_site\":{\"file\":\""),
                                 "debug build JSON must include alloc_site object");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"allocation\":{"),
                                 "debug build JSON must include allocation object");
#else
    /* Release build: alloc_site + allocation are JSON null. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"alloc_site\":null"),
                                 "release build JSON must serialize alloc_site as null");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"allocation\":null"),
                                 "release build JSON must serialize allocation as null");
#endif
}

/* ── Pitfall 4 — env cached at init, not per-panic ──────────────────────── */

void test_iron_panic_format_cached_at_init_not_per_panic(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_late_setenv_ignored, NULL, buf, sizeof(buf), &status);
    /* Late setenv MUST be ignored — output must be TEXT, not JSON. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: stale pointer dereference"),
                                 "late setenv must be ignored — output must be TEXT");
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "\"kind\":\"stale_pointer\""),
                             "late setenv must NOT trigger JSON output");
}

/* ── Test runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_check_pointer_gen_panics_on_stale);
    RUN_TEST(test_iron_check_pointer_gen_panics_on_null);
    RUN_TEST(test_iron_panic_stale_pointer_symbol_present);
    RUN_TEST(test_iron_panic_stale_text_deref_site);
    RUN_TEST(test_iron_panic_stale_text_alloc_site_debug_only);
    RUN_TEST(test_iron_panic_stale_json_kind_field);
    RUN_TEST(test_iron_panic_stale_json_deref_site_object);
    RUN_TEST(test_iron_panic_stale_json_alloc_site_payload);
    RUN_TEST(test_iron_panic_format_cached_at_init_not_per_panic);
    return UNITY_END();
}

#endif  /* !_WIN32 */
