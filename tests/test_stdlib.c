#include "unity.h"
#include "stdlib/iron_math.h"
#include "stdlib/iron_io.h"
#include "stdlib/iron_time.h"
#include "stdlib/iron_log.h"
#include "runtime/iron_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(); }
void tearDown(void) { iron_runtime_shutdown(); }

#define EPSILON 1e-9

/* ═══════════════════════════════════════════════════════════════════════════
 * Math tests
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_math_sin(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 0.0, Iron_math_sin(0.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0, Iron_math_sin(IRON_PI / 2.0));
}

void test_math_cos(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 1.0, Iron_math_cos(0.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, Iron_math_cos(IRON_PI / 2.0));
}

void test_math_sqrt(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 2.0, Iron_math_sqrt(4.0));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 0.0, Iron_math_sqrt(0.0));
}

void test_math_pow(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 1024.0, Iron_math_pow(2.0, 10.0));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 1.0,    Iron_math_pow(5.0, 0.0));
}

void test_math_lerp(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 5.0,  Iron_math_lerp(0.0, 10.0, 0.5));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 0.0,  Iron_math_lerp(0.0, 10.0, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 10.0, Iron_math_lerp(0.0, 10.0, 1.0));
}

void test_math_floor_ceil(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 2.0, Iron_math_floor(2.7));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 2.0, Iron_math_floor(2.0));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 3.0, Iron_math_ceil(2.3));
    TEST_ASSERT_DOUBLE_WITHIN(EPSILON, 2.0, Iron_math_ceil(2.0));
}

void test_math_constants(void) {
    TEST_ASSERT_DOUBLE_WITHIN(1e-10, 3.14159265358979, IRON_PI);
    TEST_ASSERT_DOUBLE_WITHIN(1e-10, 6.28318530717959, IRON_TAU);
    TEST_ASSERT_DOUBLE_WITHIN(1e-10, 2.71828182845905, IRON_E);
    /* TAU = 2 * PI */
    TEST_ASSERT_DOUBLE_WITHIN(1e-10, IRON_TAU, IRON_PI * 2.0);
}

void test_rng_create_next(void) {
    Iron_RNG rng = Iron_rng_create(42);
    uint64_t a = Iron_rng_next(&rng);
    uint64_t b = Iron_rng_next(&rng);
    /* Non-zero output */
    TEST_ASSERT_NOT_EQUAL_UINT64(0, a);
    /* Successive calls produce different values */
    TEST_ASSERT_NOT_EQUAL_UINT64(a, b);
}

void test_rng_next_int_range(void) {
    Iron_RNG rng = Iron_rng_create(12345);
    for (int i = 0; i < 100; i++) {
        int64_t v = Iron_rng_next_int(&rng, 1, 11); /* [1, 11) -> [1, 10] */
        TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, v);
        TEST_ASSERT_LESS_THAN_INT64(11, v);
    }
}

void test_math_random(void) {
    /* Verify 20 samples are all in [0.0, 1.0) */
    int in_range = 1;
    for (int i = 0; i < 20; i++) {
        double val = Iron_math_random();
        if (val < 0.0 || val >= 1.0) { in_range = 0; }
    }
    TEST_ASSERT_TRUE(in_range);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IO tests
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *s_tmp_file  = "/tmp/iron_test_io_file.txt";
static const char *s_tmp_file2 = "/tmp/iron_test_io_file2.txt";
static const char *s_tmp_dir   = "/tmp/iron_test_io_dir";

static Iron_String make_str(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

void test_io_write_and_read(void) {
    Iron_String path    = make_str(s_tmp_file);
    Iron_String content = make_str("hello, world!");

    Iron_Error werr = Iron_io_write_file(path, content);
    TEST_ASSERT_EQUAL_INT(0, werr.code);

    Iron_Result_String_Error result = Iron_io_read_file_result(path);
    TEST_ASSERT_EQUAL_INT(0, result.v1.code);
    TEST_ASSERT_EQUAL_STRING("hello, world!", iron_string_cstr(&result.v0));

    /* Clean up */
    remove(s_tmp_file);
    /* Free heap string if needed */
    if (result.v0.heap.flags & 0x01) free(result.v0.heap.data);
}

void test_io_read_nonexistent(void) {
    Iron_String path = make_str("/tmp/iron_test_nonexistent_xyz123.txt");
    Iron_Result_String_Error result = Iron_io_read_file_result(path);
    TEST_ASSERT_NOT_EQUAL_INT(0, result.v1.code);
}

void test_io_file_exists(void) {
    Iron_String path = make_str(s_tmp_file2);
    Iron_String content = make_str("exists test");

    /* Should not exist before write */
    TEST_ASSERT_FALSE(Iron_io_file_exists(path));

    Iron_io_write_file(path, content);
    TEST_ASSERT_TRUE(Iron_io_file_exists(path));

    remove(s_tmp_file2);
    TEST_ASSERT_FALSE(Iron_io_file_exists(path));
}

void test_io_create_dir(void) {
    Iron_String dir = make_str(s_tmp_dir);

    /* Remove if it already exists from a previous test run */
    rmdir(s_tmp_dir);

    Iron_Error err = Iron_io_create_dir(dir);
    TEST_ASSERT_EQUAL_INT(0, err.code);

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(s_tmp_dir, &st));
    TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));

    rmdir(s_tmp_dir);
}

void test_io_delete_file(void) {
    Iron_String path    = make_str(s_tmp_file);
    Iron_String content = make_str("to be deleted");

    Iron_io_write_file(path, content);
    TEST_ASSERT_TRUE(Iron_io_file_exists(path));

    Iron_Error err = Iron_io_delete_file(path);
    TEST_ASSERT_EQUAL_INT(0, err.code);
    TEST_ASSERT_FALSE(Iron_io_file_exists(path));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Time tests
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_time_now_ms(void) {
    int64_t t1 = Iron_time_now_ms();
    int64_t t2 = Iron_time_now_ms();
    /* Monotonic: second call >= first call */
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(t1, t2);
}

void test_time_sleep(void) {
    int64_t before = Iron_time_now_ms();
    Iron_time_sleep(10);
    int64_t after = Iron_time_now_ms();
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(10, after - before);
}

void test_timer_create_since(void) {
    Iron_Timer t = Iron_timer_create();
    Iron_time_sleep(5);
    int64_t elapsed = Iron_timer_since(t);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(5, elapsed);
}

void test_timer_reset(void) {
    Iron_Timer t = Iron_timer_create();
    Iron_time_sleep(10);
    int64_t before_reset = Iron_timer_since(t);

    t = Iron_timer_reset(t);
    int64_t after_reset = Iron_timer_since(t);

    TEST_ASSERT_GREATER_OR_EQUAL_INT64(10, before_reset);
    TEST_ASSERT_LESS_THAN_INT64(before_reset, after_reset + 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Log tests
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_log_set_level(void) {
    /* Set level to WARN — should not crash; INFO messages silently filtered */
    Iron_log_set_level(IRON_LOG_WARN);
    Iron_String msg = make_str("this info message is filtered");
    Iron_log_info(msg);  /* should be filtered (no crash) */

    Iron_String warn_msg = make_str("this warn message passes");
    Iron_log_warn(warn_msg);  /* should emit to stderr (no crash) */

    /* Reset to default for subsequent tests */
    Iron_log_set_level(IRON_LOG_INFO);
}

void test_log_debug_info_warn_error(void) {
    /* Set level to DEBUG so all messages pass */
    Iron_log_set_level(IRON_LOG_DEBUG);

    Iron_String dbg  = make_str("debug test message");
    Iron_String info = make_str("info test message");
    Iron_String warn = make_str("warn test message");
    Iron_String err  = make_str("error test message");

    /* All calls must complete without crash */
    Iron_log_debug(dbg);
    Iron_log_info(info);
    Iron_log_warn(warn);
    Iron_log_error(err);

    /* Reset to default */
    Iron_log_set_level(IRON_LOG_INFO);

    TEST_PASS_MESSAGE("log functions completed without crash");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test runner
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    UNITY_BEGIN();

    /* Math */
    RUN_TEST(test_math_sin);
    RUN_TEST(test_math_cos);
    RUN_TEST(test_math_sqrt);
    RUN_TEST(test_math_pow);
    RUN_TEST(test_math_lerp);
    RUN_TEST(test_math_floor_ceil);
    RUN_TEST(test_math_constants);
    RUN_TEST(test_rng_create_next);
    RUN_TEST(test_rng_next_int_range);
    RUN_TEST(test_math_random);

    /* IO */
    RUN_TEST(test_io_write_and_read);
    RUN_TEST(test_io_read_nonexistent);
    RUN_TEST(test_io_file_exists);
    RUN_TEST(test_io_create_dir);
    RUN_TEST(test_io_delete_file);

    /* Time */
    RUN_TEST(test_time_now_ms);
    RUN_TEST(test_time_sleep);
    RUN_TEST(test_timer_create_since);
    RUN_TEST(test_timer_reset);

    /* Log */
    RUN_TEST(test_log_set_level);
    RUN_TEST(test_log_debug_info_warn_error);

    return UNITY_END();
}
