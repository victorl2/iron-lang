/*
 * Phase 69 Plan 04 (COV-05): targeted coverage tests for
 *   - src/runtime/iron_oom.c   (baseline 0.00% line — 0 / 5)
 *   - src/runtime/iron_string.c (baseline 36.09% line — 144 / 399)
 *
 * Motivating incident: Phase 69 Plan 03 baseline flagged both files below
 * the 50% line-coverage floor mandated by COV-05. iron_oom.c had no tests
 * at all (its sole symbol iron_oom_abort is only invoked from fatal paths
 * that kill any parent test binary). iron_string.c was covered only for
 * construction + SSO paths; the entire Iron_string_* builtin family
 * (upper/lower/trim/contains/starts_with/ends_with/index_of/rindex_of/
 * char_at/count/split/join/replace/substring/to_int/to_float/repeat/
 * pad_left/pad_right/byte_at/from_byte) was completely untested, along
 * with interning-free short strings and the non-SSO heap concat path
 * already touched in test_runtime_string.c.
 *
 * Scope: NOT 100% coverage — 50% floor per CONTEXT.md "50% floor, not
 * 100% ceiling". One targeted fork test for iron_oom_abort plus call-site
 * coverage of the previously-untested Iron_string_* builtins brings both
 * files comfortably above the floor.
 *
 * Strategy (iron_oom.c): fork, in the child redirect stderr to a pipe and
 * call iron_oom_abort directly, in the parent read the pipe and assert
 * that the child died on SIGABRT with "iron: out of memory at test-site"
 * on stderr. Mirrors the existing tests/unit/test_alloc_list_push_oom.c
 * pattern. We run the test twice: once with a non-NULL `where` label and
 * once with NULL to exercise the `? : "<unknown>"` ternary.
 *
 * Strategy (iron_string.c): call each previously-uncovered Iron_string_*
 * builtin with a small assortment of SSO and heap inputs, asserting on
 * observable output. We do NOT try to exercise the iron_oom_abort bail
 * paths inside iron_string_concat / _repeat / _replace / _pad_* etc. —
 * those require LD_PRELOAD shims and are out of scope for a 50% floor.
 */

#define _GNU_SOURCE
#include "runtime/iron_runtime.h"
#include "diagnostics/diagnostics.h"
#include "unity.h"

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* iron_oom_abort + coverage: why this test uses SIGABRT catch + longjmp
 * rather than fork-and-assert.
 *
 * Problem: iron_oom_abort calls abort(3), which bypasses the profile-write
 * atexit handler that clang's coverage runtime registers. A forked child
 * dies via SIGABRT and its profraw counters for iron_oom.c never make it
 * into the merged profdata → summary.json. That's why the obvious fork
 * harness (see tests/unit/test_alloc_list_push_oom.c) records zero
 * coverage for iron_oom.c even when every call site fires.
 *
 * On macOS the profile-flush helper `__llvm_profile_write_file` is a
 * private static symbol: `nm -g` doesn't list it, so dlsym(RTLD_DEFAULT)
 * cannot locate it from the test binary, and `__attribute__((weak))`
 * declarations fail to link because Mach-O weak references require a
 * dylib-hosted symbol.
 *
 * Solution: keep everything in the MAIN test process and skip the fork.
 * We install a SIGABRT handler that longjmps back to the test body
 * BEFORE the default SIGABRT terminates us. iron_oom_abort runs its
 * fprintf + fflush normally (those lines now count as covered), then
 * calls abort(3), which raises SIGABRT and invokes our handler, which
 * longjmps out. Execution returns to the test body which asserts on
 * the stderr content we captured via a pipe redirect. The test process
 * then exits normally — `__llvm_profile_write_file` fires via the
 * atexit hook, and the child counters for iron_oom.c lines 38-43 are
 * recorded into the merged profdata.
 *
 * Caveat: the SIGABRT handler runs on the same stack as the signaled
 * thread, and longjmp-from-signal-handler is only portable for signals
 * delivered by raise(), not for async signals. Since abort(3) delivers
 * SIGABRT via a synchronous raise() on this thread, the longjmp escape
 * is legal per the signal(7) "async-signal-safe" rules and standard
 * setjmp/longjmp semantics.
 */
static sigjmp_buf s_abort_escape;
static volatile sig_atomic_t s_abort_reached;

static void cov_abort_handler(int sig) {
    (void)sig;
    s_abort_reached = 1;
    /* longjmp back to the sigsetjmp call site in the test body. */
    siglongjmp(s_abort_escape, 1);
}

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── iron_oom.c: iron_oom_abort fork harness ─────────────────────────────── */

/* Redirect stderr into a pipe so the test can capture iron_oom_abort's
 * diagnostic without polluting the ctest log. Returns the original
 * stderr fd (so the caller can restore it via dup2 after). */
static int redirect_stderr_to_pipe(int pipe_fds[2]) {
    TEST_ASSERT_EQUAL(0, pipe(pipe_fds));
    int saved = dup(2);
    TEST_ASSERT_TRUE(saved >= 0);
    dup2(pipe_fds[1], 2);
    return saved;
}

static void drain_pipe(int fd, char *out, size_t cap) {
    memset(out, 0, cap);
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, out + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= cap - 1) break;
    }
}

/* Drive iron_oom_abort with the supplied `where`, capture the diagnostic
 * line it writes to stderr, and recover via SIGABRT-longjmp. See the
 * banner comment at the top of this file for the why.
 *
 * Layout:
 *   1. Install SIGABRT → cov_abort_handler (longjmp target).
 *   2. Redirect stderr to a pipe so we can read back iron_oom_abort's
 *      fprintf output without noise on the test log.
 *   3. sigsetjmp — the zero-return branch is the "call iron_oom_abort"
 *      path, the non-zero branch is where longjmp lands us after abort.
 *   4. After recovery: restore stderr, read the pipe, restore the
 *      default SIGABRT handler, assert on the captured bytes.
 */
static void run_oom_abort_in_process(const char *where,
                                      char *out_stderr, size_t out_cap) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cov_abort_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_NODEFER so the handler can be re-entered if needed; not
     * strictly required here (one shot) but cheap insurance. */
    sa.sa_flags = SA_NODEFER;
    struct sigaction old_sa;
    TEST_ASSERT_EQUAL(0, sigaction(SIGABRT, &sa, &old_sa));

    int pipe_fds[2];
    int saved_stderr = redirect_stderr_to_pipe(pipe_fds);

    s_abort_reached = 0;

    if (sigsetjmp(s_abort_escape, /*savesigs*/ 1) == 0) {
        iron_oom_abort(where);  /* runs fprintf + fflush + abort */
        /* Unreached: abort raises SIGABRT which longjmps out. */
        TEST_FAIL_MESSAGE("iron_oom_abort returned without abort");
    }

    /* After longjmp: stderr still redirected to the pipe. Flush and
     * restore it so subsequent TEST_ASSERT messages land on the real
     * stderr. */
    fflush(stderr);
    dup2(saved_stderr, 2);
    close(saved_stderr);
    close(pipe_fds[1]);

    drain_pipe(pipe_fds[0], out_stderr, out_cap);
    close(pipe_fds[0]);

    /* Restore the original SIGABRT disposition so later tests in the
     * same binary behave normally. */
    sigaction(SIGABRT, &old_sa, NULL);

    TEST_ASSERT_TRUE_MESSAGE(s_abort_reached == 1,
                              "SIGABRT handler did not fire");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out_stderr, "iron: out of memory at"),
                                  "iron_oom_abort must print the standard prefix");
}

/* Case 1: explicit site label — exercises the `where` branch of the
 * ternary in iron_oom_abort. */
void test_iron_oom_abort_with_named_site(void) {
    char buf[512];
    run_oom_abort_in_process("test_cov_site", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "test_cov_site"),
                                  "stderr must contain the supplied `where`");
}

/* Case 2: NULL site label — exercises the `: "<unknown>"` branch of the
 * ternary in iron_oom_abort (currently uncovered at baseline). */
void test_iron_oom_abort_with_null_site(void) {
    char buf[512];
    run_oom_abort_in_process(NULL, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "<unknown>"),
                                  "NULL `where` must print '<unknown>'");
}

/* ── iron_string.c: Iron_string_* builtin coverage ───────────────────────── */

/* Build a (heap, non-literal) Iron_String from a C literal without going
 * through iron_string_from_literal — we want the concat/heap paths to
 * actually allocate each call, not hand back an interned shared pointer. */
static Iron_String make_str(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

void test_iron_string_upper_lower_trim(void) {
    Iron_String s = make_str("Hello, World");
    Iron_String u = Iron_string_upper(s);
    Iron_String l = Iron_string_lower(s);
    TEST_ASSERT_EQUAL_STRING("HELLO, WORLD", iron_string_cstr(&u));
    TEST_ASSERT_EQUAL_STRING("hello, world", iron_string_cstr(&l));

    /* trim exercises both leading and trailing whitespace skip loops */
    Iron_String padded = make_str("   hi there   ");
    Iron_String trimmed = Iron_string_trim(padded);
    TEST_ASSERT_EQUAL_STRING("hi there", iron_string_cstr(&trimmed));

    /* trim of all-whitespace yields empty string */
    Iron_String spaces = make_str("     ");
    Iron_String empty  = Iron_string_trim(spaces);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&empty));
}

void test_iron_string_contains_starts_ends(void) {
    Iron_String hay = make_str("the quick brown fox");
    Iron_String sub = make_str("quick");
    Iron_String miss = make_str("slow");
    TEST_ASSERT_TRUE(Iron_string_contains(hay, sub));
    TEST_ASSERT_FALSE(Iron_string_contains(hay, miss));

    Iron_String prefix = make_str("the");
    Iron_String wrong_prefix = make_str("fox");
    TEST_ASSERT_TRUE(Iron_string_starts_with(hay, prefix));
    TEST_ASSERT_FALSE(Iron_string_starts_with(hay, wrong_prefix));

    Iron_String suffix = make_str("fox");
    Iron_String wrong_suffix = make_str("the");
    TEST_ASSERT_TRUE(Iron_string_ends_with(hay, suffix));
    TEST_ASSERT_FALSE(Iron_string_ends_with(hay, wrong_suffix));

    /* A too-long prefix/suffix must short-circuit via the plen > slen guard */
    Iron_String tooLong = make_str("the quick brown fox jumps over");
    TEST_ASSERT_FALSE(Iron_string_starts_with(hay, tooLong));
    TEST_ASSERT_FALSE(Iron_string_ends_with(hay, tooLong));
}

void test_iron_string_index_of_rindex_of_char_at(void) {
    Iron_String s = make_str("foo.bar.baz");
    Iron_String dot = make_str(".");
    TEST_ASSERT_EQUAL_INT64(3,  Iron_string_index_of(s, dot));
    TEST_ASSERT_EQUAL_INT64(7,  Iron_string_rindex_of(s, dot));

    Iron_String miss = make_str("?");
    TEST_ASSERT_EQUAL_INT64(-1, Iron_string_index_of(s, miss));
    TEST_ASSERT_EQUAL_INT64(-1, Iron_string_rindex_of(s, miss));

    /* rindex_of guards: empty needle and needle longer than haystack both -1 */
    Iron_String empty_sub = make_str("");
    TEST_ASSERT_EQUAL_INT64(-1, Iron_string_rindex_of(s, empty_sub));
    Iron_String long_sub = make_str("this is much longer than the haystack");
    TEST_ASSERT_EQUAL_INT64(-1, Iron_string_rindex_of(s, long_sub));

    /* char_at: in-range and out-of-range */
    Iron_String ch0 = Iron_string_char_at(s, 0);
    TEST_ASSERT_EQUAL_STRING("f", iron_string_cstr(&ch0));
    Iron_String oob_neg = Iron_string_char_at(s, -1);
    Iron_String oob_pos = Iron_string_char_at(s, 9999);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&oob_neg));
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&oob_pos));
}

void test_iron_string_count_and_len(void) {
    Iron_String s = make_str("abcabcabc");
    Iron_String abc = make_str("abc");
    TEST_ASSERT_EQUAL_INT64(3, Iron_string_count(s, abc));

    /* Empty needle short-circuits to 0 */
    Iron_String empty = make_str("");
    TEST_ASSERT_EQUAL_INT64(0, Iron_string_count(s, empty));

    TEST_ASSERT_EQUAL_INT64(9, Iron_string_len(s));
}

void test_iron_string_split_join(void) {
    Iron_String s = make_str("a,b,c,d");
    Iron_String sep = make_str(",");
    Iron_List_Iron_String parts = Iron_string_split(s, sep);
    TEST_ASSERT_EQUAL_INT64(4, parts.count);
    TEST_ASSERT_EQUAL_STRING("a", iron_string_cstr(&parts.items[0]));
    TEST_ASSERT_EQUAL_STRING("d", iron_string_cstr(&parts.items[3]));

    /* Empty separator path: one element per byte */
    Iron_String word = make_str("xyz");
    Iron_String empty_sep = make_str("");
    Iron_List_Iron_String bytes = Iron_string_split(word, empty_sep);
    TEST_ASSERT_EQUAL_INT64(3, bytes.count);
    TEST_ASSERT_EQUAL_STRING("y", iron_string_cstr(&bytes.items[1]));

    /* Join back together */
    Iron_String joined = Iron_string_join(sep, parts);
    TEST_ASSERT_EQUAL_STRING("a,b,c,d", iron_string_cstr(&joined));

    /* Join of empty list yields empty string */
    Iron_List_Iron_String empty_list = Iron_List_Iron_String_create();
    Iron_String nothing = Iron_string_join(sep, empty_list);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&nothing));
}

void test_iron_string_replace_substring(void) {
    Iron_String s = make_str("one fish two fish");
    Iron_String fish = make_str("fish");
    Iron_String shark = make_str("shark");
    Iron_String replaced = Iron_string_replace(s, fish, shark);
    TEST_ASSERT_EQUAL_STRING("one shark two shark", iron_string_cstr(&replaced));

    /* Empty old_s is a no-op early return */
    Iron_String empty = make_str("");
    Iron_String same = Iron_string_replace(s, empty, shark);
    TEST_ASSERT_EQUAL_STRING("one fish two fish", iron_string_cstr(&same));

    /* Substring clamping: start negative and end past length */
    Iron_String substr = Iron_string_substring(s, -5, 100);
    TEST_ASSERT_EQUAL_STRING("one fish two fish", iron_string_cstr(&substr));

    /* Substring normal */
    Iron_String middle = Iron_string_substring(s, 4, 8);
    TEST_ASSERT_EQUAL_STRING("fish", iron_string_cstr(&middle));

    /* Start > end collapses to empty */
    Iron_String collapsed = Iron_string_substring(s, 10, 5);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&collapsed));
}

void test_iron_string_to_int_to_float(void) {
    TEST_ASSERT_EQUAL_INT64(42,  Iron_string_to_int(make_str("42")));
    TEST_ASSERT_EQUAL_INT64(-7,  Iron_string_to_int(make_str("-7")));
    TEST_ASSERT_EQUAL_INT64(0,   Iron_string_to_int(make_str("notanumber")));

    TEST_ASSERT_EQUAL_DOUBLE(3.14, Iron_string_to_float(make_str("3.14")));
    TEST_ASSERT_EQUAL_DOUBLE(0.0,  Iron_string_to_float(make_str("zilch")));
}

void test_iron_string_repeat_pad(void) {
    /* repeat n<=0 returns empty string */
    Iron_String neg = Iron_string_repeat(make_str("abc"), -2);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&neg));

    /* repeat len==0 returns empty string */
    Iron_String empty_rep = Iron_string_repeat(make_str(""), 5);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&empty_rep));

    /* Heap-sized repeat: 8 copies of "abcdefghij" = 80 bytes */
    Iron_String rep = Iron_string_repeat(make_str("abcdefghij"), 8);
    TEST_ASSERT_EQUAL_size_t(80, iron_string_byte_len(&rep));
    /* Check pattern preserved */
    TEST_ASSERT_EQUAL_INT('a', iron_string_cstr(&rep)[0]);
    TEST_ASSERT_EQUAL_INT('j', iron_string_cstr(&rep)[9]);
    TEST_ASSERT_EQUAL_INT('a', iron_string_cstr(&rep)[10]);

    /* pad_left to width larger than the string */
    Iron_String p = Iron_string_pad_left(make_str("7"), 4, make_str("0"));
    TEST_ASSERT_EQUAL_STRING("0007", iron_string_cstr(&p));

    /* pad_right mirror */
    Iron_String q = Iron_string_pad_right(make_str("x"), 5, make_str("."));
    TEST_ASSERT_EQUAL_STRING("x....", iron_string_cstr(&q));

    /* Width <= current length: returns self (early exit) */
    Iron_String wide = make_str("already wide enough");
    Iron_String noop = Iron_string_pad_left(wide, 3, make_str(" "));
    TEST_ASSERT_EQUAL_STRING("already wide enough", iron_string_cstr(&noop));

    /* Empty pad string: falls back to space */
    Iron_String default_pad = Iron_string_pad_left(make_str("a"), 3,
                                                    make_str(""));
    TEST_ASSERT_EQUAL_STRING("  a", iron_string_cstr(&default_pad));
}

void test_iron_string_byte_at_from_byte(void) {
    Iron_String s = make_str("abc");
    TEST_ASSERT_EQUAL_INT64((int64_t)'a', Iron_string_byte_at(s, 0));
    TEST_ASSERT_EQUAL_INT64((int64_t)'c', Iron_string_byte_at(s, 2));
    /* Out-of-range guards */
    TEST_ASSERT_EQUAL_INT64(-1, Iron_string_byte_at(s, -1));
    TEST_ASSERT_EQUAL_INT64(-1, Iron_string_byte_at(s, 10));

    Iron_String ch = Iron_string_from_byte(0x41);  /* 'A' */
    TEST_ASSERT_EQUAL_STRING("A", iron_string_cstr(&ch));
    TEST_ASSERT_EQUAL_size_t(1, iron_string_byte_len(&ch));
}

/* Heap concat path exercises the non-SSO branch of iron_string_concat and
 * the non-literal iron_string_from_cstr heap path. */
void test_iron_string_concat_heap_path(void) {
    const char *sa = "abcdefghijklmnopqrstuvwxyz";  /* 26 */
    const char *sb = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";  /* 26 */
    Iron_String a = iron_string_from_cstr(sa, 26);
    Iron_String b = iron_string_from_cstr(sb, 26);
    Iron_String cat = iron_string_concat(&a, &b);
    TEST_ASSERT_EQUAL_size_t(52, iron_string_byte_len(&cat));
    TEST_ASSERT_EQUAL_UINT8(1, cat.heap.flags & 0x01);
    TEST_ASSERT_EQUAL_INT(0, strncmp(iron_string_cstr(&cat),
                                      "abcdefghijklmnopqrstuvwxyz"
                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 52));
    free(cat.heap.data);
    free(a.heap.data);
    free(b.heap.data);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* iron_oom.c */
    RUN_TEST(test_iron_oom_abort_with_named_site);
    RUN_TEST(test_iron_oom_abort_with_null_site);

    /* iron_string.c — builtin family */
    RUN_TEST(test_iron_string_upper_lower_trim);
    RUN_TEST(test_iron_string_contains_starts_ends);
    RUN_TEST(test_iron_string_index_of_rindex_of_char_at);
    RUN_TEST(test_iron_string_count_and_len);
    RUN_TEST(test_iron_string_split_join);
    RUN_TEST(test_iron_string_replace_substring);
    RUN_TEST(test_iron_string_to_int_to_float);
    RUN_TEST(test_iron_string_repeat_pad);
    RUN_TEST(test_iron_string_byte_at_from_byte);
    RUN_TEST(test_iron_string_concat_heap_path);

    return UNITY_END();
}
