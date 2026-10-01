/* test_runtime_hash.c
 *
 * The Map[K, V] / Set[T] hash tables (IRON_HMAP_DEFINE / IRON_HSET_DEFINE in
 * iron_runtime.h). The compiler instantiates them per key/value type with
 * six small helpers; this file instantiates the same shapes the compiler
 * emits for Map[Int, Int], Map[String, Int], Map[String, String] and
 * Set[Int] / Set[String], and checks behaviour that only shows up under
 * churn: growth through several rehashes, tombstone reuse after removes,
 * overwrite dropping the old value and the duplicate key, string keys
 * retained and released exactly once, clone independence, take and clear.
 */

#define _GNU_SOURCE
#include "runtime/iron_runtime.h"
#include "unity.h"

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String S(const char *cstr) {
    return iron_string_from_cstr(cstr, strlen(cstr));
}

/* ── Int -> Int: trivial keys and values ───────────────────────────────── */
static uint64_t Iron_Map_int64_t_int64_t_khash(const int64_t *k) { return iron_hash_u64((uint64_t)*k); }
static bool     Iron_Map_int64_t_int64_t_keq(const int64_t *a, const int64_t *b) { return *a == *b; }
static void     Iron_Map_int64_t_int64_t_kcopy(int64_t *k) { (void)k; }
static void     Iron_Map_int64_t_int64_t_kdrop(int64_t *k) { (void)k; }
static void     Iron_Map_int64_t_int64_t_vcopy(int64_t *v) { (void)v; }
static void     Iron_Map_int64_t_int64_t_vdrop(int64_t *v) { (void)v; }
IRON_HMAP_DEFINE(Iron_Map_int64_t_int64_t, int64_t, int64_t)

/* ── String -> Int: refcounted keys ────────────────────────────────────── */
static uint64_t Iron_Map_Iron_String_int64_t_khash(const Iron_String *k) { return iron_string_hash(k); }
static bool     Iron_Map_Iron_String_int64_t_keq(const Iron_String *a, const Iron_String *b) { return iron_string_equals(a, b); }
static void     Iron_Map_Iron_String_int64_t_kcopy(Iron_String *k) { iron_string_retain(k); }
static void     Iron_Map_Iron_String_int64_t_kdrop(Iron_String *k) { iron_string_release(k); }
static void     Iron_Map_Iron_String_int64_t_vcopy(int64_t *v) { (void)v; }
static void     Iron_Map_Iron_String_int64_t_vdrop(int64_t *v) { (void)v; }
IRON_HMAP_DEFINE(Iron_Map_Iron_String_int64_t, Iron_String, int64_t)

/* ── String -> String: refcounted keys and values; counts the drops ───── */
static int g_value_drops = 0;
static uint64_t Iron_Map_Iron_String_Iron_String_khash(const Iron_String *k) { return iron_string_hash(k); }
static bool     Iron_Map_Iron_String_Iron_String_keq(const Iron_String *a, const Iron_String *b) { return iron_string_equals(a, b); }
static void     Iron_Map_Iron_String_Iron_String_kcopy(Iron_String *k) { iron_string_retain(k); }
static void     Iron_Map_Iron_String_Iron_String_kdrop(Iron_String *k) { iron_string_release(k); }
static void     Iron_Map_Iron_String_Iron_String_vcopy(Iron_String *v) { iron_string_retain(v); }
static void     Iron_Map_Iron_String_Iron_String_vdrop(Iron_String *v) { g_value_drops++; iron_string_release(v); }
IRON_HMAP_DEFINE(Iron_Map_Iron_String_Iron_String, Iron_String, Iron_String)

/* ── Set[Int] and Set[String] ──────────────────────────────────────────── */
static uint64_t Iron_Set_int64_t_khash(const int64_t *k) { return iron_hash_u64((uint64_t)*k); }
static bool     Iron_Set_int64_t_keq(const int64_t *a, const int64_t *b) { return *a == *b; }
static void     Iron_Set_int64_t_kcopy(int64_t *k) { (void)k; }
static void     Iron_Set_int64_t_kdrop(int64_t *k) { (void)k; }
IRON_HSET_DEFINE(Iron_Set_int64_t, int64_t)

static uint64_t Iron_Set_Iron_String_khash(const Iron_String *k) { return iron_string_hash(k); }
static bool     Iron_Set_Iron_String_keq(const Iron_String *a, const Iron_String *b) { return iron_string_equals(a, b); }
static void     Iron_Set_Iron_String_kcopy(Iron_String *k) { iron_string_retain(k); }
static void     Iron_Set_Iron_String_kdrop(Iron_String *k) { iron_string_release(k); }
IRON_HSET_DEFINE(Iron_Set_Iron_String, Iron_String)

/* ══════════════════════════════════════════════════════════════════════════
 * Map[Int, Int]
 * ══════════════════════════════════════════════════════════════════════════ */

void test_map_int_empty(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    TEST_ASSERT_EQUAL_INT64(0, Iron_Map_int64_t_int64_t_len(&m));
    TEST_ASSERT_FALSE(Iron_Map_int64_t_int64_t_has(&m, 1));
    TEST_ASSERT_FALSE(Iron_Map_int64_t_int64_t_remove(&m, 1));
    TEST_ASSERT_EQUAL_INT64(7, Iron_Map_int64_t_int64_t_get_or(&m, 1, 7));
    Iron_Map_int64_t_int64_t_free(&m);
    TEST_ASSERT_NULL(m.keys);
    TEST_ASSERT_EQUAL_INT64(0, m.cap);
}

void test_map_int_put_get_overwrite(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    Iron_Map_int64_t_int64_t_put(&m, 1, 10);
    Iron_Map_int64_t_int64_t_put(&m, 2, 20);
    Iron_Map_int64_t_int64_t_put(&m, 1, 11);     /* overwrite keeps one entry */
    TEST_ASSERT_EQUAL_INT64(2, Iron_Map_int64_t_int64_t_len(&m));
    TEST_ASSERT_EQUAL_INT64(11, Iron_Map_int64_t_int64_t_get_at(&m, 1, __FILE__, __LINE__));
    TEST_ASSERT_EQUAL_INT64(20, Iron_Map_int64_t_int64_t_get_or(&m, 2, -1));
    TEST_ASSERT_EQUAL_INT64(-1, Iron_Map_int64_t_int64_t_get_or(&m, 3, -1));
    Iron_Map_int64_t_int64_t_free(&m);
}

void test_map_int_growth_keeps_every_key(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    const int64_t n = 5000;                       /* ~10 rehashes from cap 8 */
    for (int64_t i = 0; i < n; i++) Iron_Map_int64_t_int64_t_put(&m, i * 7919, i);
    TEST_ASSERT_EQUAL_INT64(n, Iron_Map_int64_t_int64_t_len(&m));
    TEST_ASSERT_TRUE((m.cap & (m.cap - 1)) == 0);   /* power of two */
    TEST_ASSERT_TRUE(m.count * 10 <= m.cap * 7);     /* load factor honoured */
    for (int64_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_INT64(i, Iron_Map_int64_t_int64_t_get_at(&m, i * 7919, __FILE__, __LINE__));
    TEST_ASSERT_FALSE(Iron_Map_int64_t_int64_t_has(&m, 1));
    Iron_Map_int64_t_int64_t_free(&m);
}

void test_map_int_colliding_keys_probe(void) {
    /* Keys that hash to the same slot in a tiny table must all be found. */
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    int64_t keys[5];
    int found = 0;
    uint64_t want = iron_hash_u64(0) & 7;
    for (int64_t k = 0; found < 5 && k < 100000; k++)
        if ((iron_hash_u64((uint64_t)k) & 7) == want) keys[found++] = k;
    TEST_ASSERT_EQUAL_INT(5, found);
    for (int i = 0; i < 5; i++) Iron_Map_int64_t_int64_t_put(&m, keys[i], i);
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_INT64(i, Iron_Map_int64_t_int64_t_get_at(&m, keys[i], __FILE__, __LINE__));
    TEST_ASSERT_TRUE(Iron_Map_int64_t_int64_t_remove(&m, keys[1]));
    for (int i = 0; i < 5; i++) {
        if (i == 1) TEST_ASSERT_FALSE(Iron_Map_int64_t_int64_t_has(&m, keys[i]));
        else TEST_ASSERT_EQUAL_INT64(i, Iron_Map_int64_t_int64_t_get_at(&m, keys[i], __FILE__, __LINE__));
    }
    Iron_Map_int64_t_int64_t_free(&m);
}

void test_map_int_remove_reinsert_churn(void) {
    /* Repeated remove/put of the same keys must not grow the table without
     * bound: tombstones are recycled by the rehash at the load threshold. */
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    for (int round = 0; round < 2000; round++) {
        for (int64_t k = 0; k < 10; k++) Iron_Map_int64_t_int64_t_put(&m, k, round);
        for (int64_t k = 0; k < 10; k++) TEST_ASSERT_TRUE(Iron_Map_int64_t_int64_t_remove(&m, k));
        TEST_ASSERT_EQUAL_INT64(0, Iron_Map_int64_t_int64_t_len(&m));
    }
    TEST_ASSERT_TRUE(m.cap <= 64);
    for (int64_t k = 0; k < 10; k++) Iron_Map_int64_t_int64_t_put(&m, k, k);
    for (int64_t k = 0; k < 10; k++)
        TEST_ASSERT_EQUAL_INT64(k, Iron_Map_int64_t_int64_t_get_at(&m, k, __FILE__, __LINE__));
    Iron_Map_int64_t_int64_t_free(&m);
}

void test_map_int_clone_take_clear(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    for (int64_t k = 0; k < 100; k++) Iron_Map_int64_t_int64_t_put(&m, k, k * k);
    Iron_Map_int64_t_int64_t c = Iron_Map_int64_t_int64_t_clone(&m);
    Iron_Map_int64_t_int64_t_put(&c, 1000, 1);
    Iron_Map_int64_t_int64_t_remove(&c, 5);
    TEST_ASSERT_EQUAL_INT64(100, Iron_Map_int64_t_int64_t_len(&m));
    TEST_ASSERT_EQUAL_INT64(100, Iron_Map_int64_t_int64_t_len(&c));
    TEST_ASSERT_TRUE(Iron_Map_int64_t_int64_t_has(&m, 5));
    TEST_ASSERT_FALSE(Iron_Map_int64_t_int64_t_has(&m, 1000));
    for (int64_t k = 0; k < 100; k++)
        if (k != 5) TEST_ASSERT_EQUAL_INT64(k * k, Iron_Map_int64_t_int64_t_get_at(&c, k, __FILE__, __LINE__));

    Iron_Map_int64_t_int64_t t = Iron_Map_int64_t_int64_t_take(&m);
    TEST_ASSERT_EQUAL_INT64(0, Iron_Map_int64_t_int64_t_len(&m));
    TEST_ASSERT_NULL(m.keys);
    TEST_ASSERT_EQUAL_INT64(100, Iron_Map_int64_t_int64_t_len(&t));
    Iron_Map_int64_t_int64_t_put(&m, 1, 1);      /* the emptied map is usable */
    TEST_ASSERT_EQUAL_INT64(1, Iron_Map_int64_t_int64_t_len(&m));

    Iron_Map_int64_t_int64_t_clear(&t);
    TEST_ASSERT_EQUAL_INT64(0, Iron_Map_int64_t_int64_t_len(&t));
    TEST_ASSERT_FALSE(Iron_Map_int64_t_int64_t_has(&t, 3));
    Iron_Map_int64_t_int64_t_put(&t, 3, 3);
    TEST_ASSERT_EQUAL_INT64(3, Iron_Map_int64_t_int64_t_get_at(&t, 3, __FILE__, __LINE__));

    Iron_Map_int64_t_int64_t_free(&m);
    Iron_Map_int64_t_int64_t_free(&c);
    Iron_Map_int64_t_int64_t_free(&t);
}

void test_map_int_clone_empty(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    Iron_Map_int64_t_int64_t c = Iron_Map_int64_t_int64_t_clone(&m);
    TEST_ASSERT_EQUAL_INT64(0, c.cap);
    TEST_ASSERT_NULL(c.keys);
    Iron_Map_int64_t_int64_t_put(&c, 1, 2);
    TEST_ASSERT_EQUAL_INT64(2, Iron_Map_int64_t_int64_t_get_at(&c, 1, __FILE__, __LINE__));
    Iron_Map_int64_t_int64_t_free(&m);
    Iron_Map_int64_t_int64_t_free(&c);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Map[String, Int] and Map[String, String]: ownership of keys and values
 * ══════════════════════════════════════════════════════════════════════════ */

void test_map_string_keys_by_content(void) {
    Iron_Map_Iron_String_int64_t m = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&m, S("alpha"), 1);
    Iron_Map_Iron_String_int64_t_put(&m, S("beta"), 2);
    /* A different allocation with the same bytes is the same key. */
    TEST_ASSERT_TRUE(Iron_Map_Iron_String_int64_t_has(&m, S("alpha")));
    TEST_ASSERT_EQUAL_INT64(2, Iron_Map_Iron_String_int64_t_get_at(&m, S("beta"), __FILE__, __LINE__));
    TEST_ASSERT_FALSE(Iron_Map_Iron_String_int64_t_has(&m, S("gamma")));
    TEST_ASSERT_FALSE(Iron_Map_Iron_String_int64_t_has(&m, S("")));
    Iron_Map_Iron_String_int64_t_put(&m, S(""), 0);
    TEST_ASSERT_TRUE(Iron_Map_Iron_String_int64_t_has(&m, S("")));
    TEST_ASSERT_EQUAL_INT64(3, Iron_Map_Iron_String_int64_t_len(&m));
    Iron_Map_Iron_String_int64_t_free(&m);
}

void test_map_string_overwrite_releases_duplicate_key(void) {
    /* put with an existing key must release the incoming duplicate and keep
     * the stored one; a second put must not leak either. The leak tracker in
     * iron_runtime_shutdown (tearDown) aborts the test binary on a leaked
     * heap string, so this test passing is the assertion. */
    Iron_Map_Iron_String_int64_t m = Iron_Map_Iron_String_int64_t_create();
    for (int i = 0; i < 50; i++) Iron_Map_Iron_String_int64_t_put(&m, S("same"), i);
    TEST_ASSERT_EQUAL_INT64(1, Iron_Map_Iron_String_int64_t_len(&m));
    TEST_ASSERT_EQUAL_INT64(49, Iron_Map_Iron_String_int64_t_get_at(&m, S("same"), __FILE__, __LINE__));
    Iron_Map_Iron_String_int64_t_free(&m);
}

void test_map_string_string_values_dropped(void) {
    g_value_drops = 0;
    Iron_Map_Iron_String_Iron_String m = Iron_Map_Iron_String_Iron_String_create();
    Iron_Map_Iron_String_Iron_String_put(&m, S("k"), S("v1"));
    Iron_Map_Iron_String_Iron_String_put(&m, S("k"), S("v2"));   /* drops v1 */
    TEST_ASSERT_EQUAL_INT(1, g_value_drops);
    Iron_String got = Iron_Map_Iron_String_Iron_String_get_at(&m, S("k"), __FILE__, __LINE__);
    TEST_ASSERT_EQUAL_STRING("v2", iron_string_cstr(&got));
    iron_string_release(&got);                                   /* get hands out a retained copy */
    Iron_String dflt = Iron_Map_Iron_String_Iron_String_get_or(&m, S("missing"), S("d"));
    TEST_ASSERT_EQUAL_STRING("d", iron_string_cstr(&dflt));
    iron_string_release(&dflt);
    Iron_String present = Iron_Map_Iron_String_Iron_String_get_or(&m, S("k"), S("unused"));
    TEST_ASSERT_EQUAL_INT(2, g_value_drops);                     /* the unused default was dropped */
    iron_string_release(&present);
    TEST_ASSERT_TRUE(Iron_Map_Iron_String_Iron_String_remove(&m, S("k")));   /* drops v2 */
    TEST_ASSERT_EQUAL_INT(3, g_value_drops);
    Iron_Map_Iron_String_Iron_String_put(&m, S("a"), S("1"));
    Iron_Map_Iron_String_Iron_String_put(&m, S("b"), S("2"));
    Iron_Map_Iron_String_Iron_String_free(&m);                   /* drops the rest */
    TEST_ASSERT_EQUAL_INT(5, g_value_drops);
}

void test_map_string_clone_independent_strings(void) {
    Iron_Map_Iron_String_Iron_String m = Iron_Map_Iron_String_Iron_String_create();
    for (int i = 0; i < 200; i++) {
        char kb[16], vb[16];
        snprintf(kb, sizeof kb, "key%d", i);
        snprintf(vb, sizeof vb, "val%d", i);
        Iron_Map_Iron_String_Iron_String_put(&m, S(kb), S(vb));
    }
    Iron_Map_Iron_String_Iron_String c = Iron_Map_Iron_String_Iron_String_clone(&m);
    Iron_Map_Iron_String_Iron_String_free(&m);                   /* the clone must survive */
    TEST_ASSERT_EQUAL_INT64(200, Iron_Map_Iron_String_Iron_String_len(&c));
    Iron_String v = Iron_Map_Iron_String_Iron_String_get_at(&c, S("key123"), __FILE__, __LINE__);
    TEST_ASSERT_EQUAL_STRING("val123", iron_string_cstr(&v));
    iron_string_release(&v);
    Iron_Map_Iron_String_Iron_String_free(&c);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Set[Int] / Set[String]
 * ══════════════════════════════════════════════════════════════════════════ */

void test_set_int_add_has_remove(void) {
    Iron_Set_int64_t s = Iron_Set_int64_t_create();
    TEST_ASSERT_TRUE(Iron_Set_int64_t_add(&s, 3));
    TEST_ASSERT_FALSE(Iron_Set_int64_t_add(&s, 3));              /* already present */
    TEST_ASSERT_TRUE(Iron_Set_int64_t_add(&s, -3));
    TEST_ASSERT_EQUAL_INT64(2, Iron_Set_int64_t_len(&s));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_has(&s, 3));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_has(&s, -3));
    TEST_ASSERT_FALSE(Iron_Set_int64_t_has(&s, 0));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_remove(&s, 3));
    TEST_ASSERT_FALSE(Iron_Set_int64_t_remove(&s, 3));
    TEST_ASSERT_FALSE(Iron_Set_int64_t_has(&s, 3));
    TEST_ASSERT_EQUAL_INT64(1, Iron_Set_int64_t_len(&s));
    Iron_Set_int64_t_free(&s);
}

void test_set_int_growth_and_churn(void) {
    Iron_Set_int64_t s = Iron_Set_int64_t_create();
    for (int64_t i = 0; i < 4000; i++) TEST_ASSERT_TRUE(Iron_Set_int64_t_add(&s, i * 3));
    for (int64_t i = 0; i < 4000; i++) TEST_ASSERT_FALSE(Iron_Set_int64_t_add(&s, i * 3));
    TEST_ASSERT_EQUAL_INT64(4000, Iron_Set_int64_t_len(&s));
    for (int64_t i = 0; i < 4000; i += 2) TEST_ASSERT_TRUE(Iron_Set_int64_t_remove(&s, i * 3));
    TEST_ASSERT_EQUAL_INT64(2000, Iron_Set_int64_t_len(&s));
    for (int64_t i = 0; i < 4000; i++)
        TEST_ASSERT_EQUAL(i % 2 == 1, Iron_Set_int64_t_has(&s, i * 3));
    TEST_ASSERT_FALSE(Iron_Set_int64_t_has(&s, 1));
    Iron_Set_int64_t c = Iron_Set_int64_t_clone(&s);
    Iron_Set_int64_t_clear(&s);
    TEST_ASSERT_EQUAL_INT64(0, Iron_Set_int64_t_len(&s));
    TEST_ASSERT_EQUAL_INT64(2000, Iron_Set_int64_t_len(&c));
    Iron_Set_int64_t t = Iron_Set_int64_t_take(&c);
    TEST_ASSERT_EQUAL_INT64(0, Iron_Set_int64_t_len(&c));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_has(&t, 3));
    Iron_Set_int64_t_free(&s);
    Iron_Set_int64_t_free(&c);
    Iron_Set_int64_t_free(&t);
}

void test_set_string_dedup_by_content(void) {
    Iron_Set_Iron_String s = Iron_Set_Iron_String_create();
    TEST_ASSERT_TRUE(Iron_Set_Iron_String_add(&s, S("x")));
    TEST_ASSERT_FALSE(Iron_Set_Iron_String_add(&s, S("x")));     /* duplicate released */
    TEST_ASSERT_TRUE(Iron_Set_Iron_String_add(&s, S("y")));
    TEST_ASSERT_TRUE(Iron_Set_Iron_String_has(&s, S("x")));
    TEST_ASSERT_FALSE(Iron_Set_Iron_String_has(&s, S("z")));
    Iron_Set_Iron_String c = Iron_Set_Iron_String_clone(&s);
    TEST_ASSERT_TRUE(Iron_Set_Iron_String_remove(&s, S("x")));
    TEST_ASSERT_TRUE(Iron_Set_Iron_String_has(&c, S("x")));
    Iron_Set_Iron_String_free(&s);
    Iron_Set_Iron_String_free(&c);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Failure paths: a missing key panics, an impossible rehash aborts
 * ══════════════════════════════════════════════════════════════════════════ */

static int run_child_capture(void (*body)(void), char *buf, size_t buflen) {
    int stderr_pipe[2];
    TEST_ASSERT_EQUAL(0, pipe(stderr_pipe));
    pid_t child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        close(stderr_pipe[0]);
        dup2(stderr_pipe[1], 2);
        body();
        _exit(99);  /* unreached */
    }
    close(stderr_pipe[1]);
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(stderr_pipe[0], buf + total, buflen - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= buflen - 1) break;
    }
    buf[total] = '\0';
    close(stderr_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    return status;
}

static void child_get_missing(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    Iron_Map_int64_t_int64_t_put(&m, 1, 1);
    (void)Iron_Map_int64_t_int64_t_get_at(&m, 2, "test.iron", 42);
}

void test_map_get_missing_key_panics(void) {
    char buf[512] = {0};
    int status = run_child_capture(child_get_missing, buf, sizeof buf);
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                             "get on a missing key must abort");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "key not found in map"), buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "test.iron:42"), buf);
}

static void child_rehash_oom(void) {
    Iron_Map_int64_t_int64_t m = Iron_Map_int64_t_int64_t_create();
    /* volatile: GCC would otherwise fold the size into the inlined malloc
     * and reject the constant with -Walloc-size-larger-than. */
    volatile int64_t huge = INT64_C(1) << 60;
    Iron_Map_int64_t_int64_t_rehash(&m, huge);
}

void test_map_rehash_oom_aborts(void) {
    char buf[512] = {0};
    int status = run_child_capture(child_rehash_oom, buf, sizeof buf);
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                             "an impossible rehash must abort through iron_oom_abort");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: out of memory at"), buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Iron_Map_int64_t_int64_t_rehash"), buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Hash helpers
 * ══════════════════════════════════════════════════════════════════════════ */

void test_hash_helpers(void) {
    TEST_ASSERT_EQUAL_UINT64(iron_hash_f64(0.0), iron_hash_f64(-0.0));
    TEST_ASSERT_NOT_EQUAL(iron_hash_u64(1), iron_hash_u64(2));
    Iron_String a = S("hello"), b = S("hello"), c = S("hellp");
    TEST_ASSERT_EQUAL_UINT64(iron_string_hash(&a), iron_string_hash(&b));
    TEST_ASSERT_NOT_EQUAL(iron_string_hash(&a), iron_string_hash(&c));
    iron_string_release(&a); iron_string_release(&b); iron_string_release(&c);
    /* Low bits must vary for sequential keys: the table masks the hash. */
    int distinct = 0;
    bool seen[64] = {0};
    for (uint64_t i = 0; i < 64; i++) {
        unsigned slot = (unsigned)(iron_hash_u64(i) & 63);
        if (!seen[slot]) { seen[slot] = true; distinct++; }
    }
    TEST_ASSERT_TRUE(distinct > 32);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_map_int_empty);
    RUN_TEST(test_map_int_put_get_overwrite);
    RUN_TEST(test_map_int_growth_keeps_every_key);
    RUN_TEST(test_map_int_colliding_keys_probe);
    RUN_TEST(test_map_int_remove_reinsert_churn);
    RUN_TEST(test_map_int_clone_take_clear);
    RUN_TEST(test_map_int_clone_empty);
    RUN_TEST(test_map_string_keys_by_content);
    RUN_TEST(test_map_string_overwrite_releases_duplicate_key);
    RUN_TEST(test_map_string_string_values_dropped);
    RUN_TEST(test_map_string_clone_independent_strings);
    RUN_TEST(test_set_int_add_has_remove);
    RUN_TEST(test_set_int_growth_and_churn);
    RUN_TEST(test_set_string_dedup_by_content);
    RUN_TEST(test_map_get_missing_key_panics);
    RUN_TEST(test_map_rehash_oom_aborts);
    RUN_TEST(test_hash_helpers);
    return UNITY_END();
}
