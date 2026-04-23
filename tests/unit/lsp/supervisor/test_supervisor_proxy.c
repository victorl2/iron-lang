/* Phase 7 Plan 07-01 Task 02 (HARD-13, D-01) -- Supervisor byte-forwarder
 * unit test.
 *
 * Exercises the non-blocking read/write helper used inside the parent's
 * poll() loop. We pipe bytes through a pair of pipes and assert byte-
 * for-byte fidelity. Full fork+exec integration is covered by the
 * CTest shell invariant tests/lsp/invariant/test_parent_death_detection.sh.
 */

#include "unity.h"
#include "lsp/supervisor/supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Test: forward a known byte stream ───────────────────────────── */

static void test_forward_bytes_copies_verbatim(void) {
    int src[2], dst[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(src));
    TEST_ASSERT_EQUAL_INT(0, pipe(dst));

    /* Make src[0] non-blocking for EAGAIN semantics mirror. */
    int fl = fcntl(src[0], F_GETFL, 0);
    fcntl(src[0], F_SETFL, fl | O_NONBLOCK);

    const char payload[] =
        "Content-Length: 40\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\"}";
    ssize_t w = write(src[1], payload, sizeof(payload) - 1);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(payload) - 1), (int)w);

    ssize_t r = ilsp_supervisor_forward_bytes_for_test(src[0], dst[1],
                                                       sizeof(payload) - 1);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(payload) - 1), (int)r);

    char rx[256] = {0};
    ssize_t rr = read(dst[0], rx, sizeof(rx) - 1);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(payload) - 1), (int)rr);
    TEST_ASSERT_EQUAL_STRING_LEN(payload, rx, sizeof(payload) - 1);

    close(src[0]); close(src[1]);
    close(dst[0]); close(dst[1]);
}

/* ── Test: EOF on source returns 0 ────────────────────────────────── */

static void test_forward_bytes_eof(void) {
    int src[2], dst[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(src));
    TEST_ASSERT_EQUAL_INT(0, pipe(dst));

    close(src[1]);  /* immediate EOF on read */
    ssize_t r = ilsp_supervisor_forward_bytes_for_test(src[0], dst[1], 128);
    TEST_ASSERT_EQUAL_INT(0, (int)r);

    close(src[0]);
    close(dst[0]); close(dst[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_forward_bytes_copies_verbatim);
    RUN_TEST(test_forward_bytes_eof);
    return UNITY_END();
}
