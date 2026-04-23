/* Phase 7 Plan 07-01 Task 02 (HARD-13, D-01) -- Unity tests for the
 * supervisor backoff table + 5-crash-in-60s bailout.
 *
 * The supervisor TU is compiled with ILSP_SUPERVISOR_TESTING=1 for these
 * tests, which exposes the internal helpers used below. */

#include "unity.h"
#include "lsp/supervisor/supervisor.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {
    ilsp_supervisor_reset_bailout_for_test();
}
void tearDown(void) {}

/* ── Test: backoff sequence is {1, 2, 4, 8, 16} with 16s cap ───────── */

static void test_backoff_sequence_16s_cap(void) {
    TEST_ASSERT_EQUAL_INT(1,  ilsp_supervisor_backoff_secs_for_test(0));
    TEST_ASSERT_EQUAL_INT(2,  ilsp_supervisor_backoff_secs_for_test(1));
    TEST_ASSERT_EQUAL_INT(4,  ilsp_supervisor_backoff_secs_for_test(2));
    TEST_ASSERT_EQUAL_INT(8,  ilsp_supervisor_backoff_secs_for_test(3));
    TEST_ASSERT_EQUAL_INT(16, ilsp_supervisor_backoff_secs_for_test(4));
    TEST_ASSERT_EQUAL_INT(16, ilsp_supervisor_backoff_secs_for_test(5));
    TEST_ASSERT_EQUAL_INT(16, ilsp_supervisor_backoff_secs_for_test(100));
}

/* ── Test: 5 crashes in under 60s => bailout; spaced out => no bailout ── */

static void test_bailout_5_crashes_in_60s(void) {
    /* Feed 5 crashes at t=0,1,3,7,15 -- all within a 60s window. */
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(0));
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(1));
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(3));
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(7));
    /* 5th crash -> bailout. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, ilsp_supervisor_record_crash_for_test(15),
        "5th crash within 60s triggers bailout");
}

static void test_no_bailout_when_spread_out(void) {
    ilsp_supervisor_reset_bailout_for_test();
    /* 5 crashes at 0, 30, 65, 95, 130 -- oldest + newest gap > 60s. */
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(0));
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(30));
    /* The 3rd crash @t=65 already evicts t=0? No -- we have only 3,
     * need 5 to check. Keep going. */
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(65));
    TEST_ASSERT_EQUAL_INT(0, ilsp_supervisor_record_crash_for_test(95));
    /* Now the window is {0, 30, 65, 95} and we add 130. Before the insert
     * the oldest is 0; after the insert the window becomes {30, 65, 95, 130}
     * because the shift drops the head. The oldest after shift is 30 and
     * the newest is 130; 130-30 = 100s > 60s => no bailout. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, ilsp_supervisor_record_crash_for_test(130),
        "5 crashes across >60s should NOT bail out");
}

/* ── Test: synthetic window/showMessage frame ─────────────────────── */

static void test_showmessage_frame_well_formed(void) {
    char frame[2048];
    size_t n = ilsp_supervisor_build_showmessage_for_test(frame, sizeof(frame),
                                                          NULL);
    TEST_ASSERT_GREATER_THAN_UINT(32u, n);

    /* Parse the Content-Length prefix. */
    TEST_ASSERT_EQUAL_MEMORY("Content-Length: ", frame, 16);
    char *crlf = strstr(frame, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL(crlf);
    /* Body must start with '{"jsonrpc":"2.0"'. */
    const char *body = crlf + 4;
    TEST_ASSERT_EQUAL_STRING_LEN("{\"jsonrpc\":\"2.0\"", body, 16);
    TEST_ASSERT_NOT_NULL(strstr(body, "window/showMessage"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"type\":2"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_backoff_sequence_16s_cap);
    RUN_TEST(test_bailout_5_crashes_in_60s);
    RUN_TEST(test_no_bailout_when_spread_out);
    RUN_TEST(test_showmessage_frame_well_formed);
    return UNITY_END();
}
