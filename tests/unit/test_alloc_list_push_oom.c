/* test_alloc_list_push_oom.c — FIX-01 rank 1 regression test.
 *
 * **Motivating Incident.** Phase 65 CORRECTNESS-AUDIT.md §3 row 5 and §6 row
 * 3 flagged src/runtime/iron_runtime.h:497 (IRON_LIST_IMPL _push macro body)
 * as an unchecked realloc. Under memory pressure the realloc could return
 * NULL and the following line `self->items[self->count++] = item;` would
 * dereference NULL, producing a SIGSEGV with no diagnostic. Iron_List_<T>
 * is the most exercised runtime path in any Iron program that uses arrays —
 * every `.push()` in user code expands through this macro.
 *
 * **Layout Diagram.** Pre-fix:
 *   void Iron_List_T_push(Iron_List_T *self, T item) {
 *       if (self->count >= self->capacity) {
 *           self->capacity = self->capacity ? self->capacity * 2 : 8;
 *           self->items = realloc(self->items, ...);   // may return NULL
 *       }
 *       self->items[self->count++] = item;             // NULL deref on OOM
 *   }
 *
 * Post-fix:
 *   void Iron_List_T_push(Iron_List_T *self, T item) {
 *       if (self->count >= self->capacity) {
 *           int64_t new_cap = self->capacity ? self->capacity * 2 : 8;
 *           if (new_cap < self->capacity) iron_oom_abort("...overflow");
 *           T *new_items = realloc(self->items, ...);
 *           if (!new_items) iron_oom_abort("Iron_List_T_push");
 *           self->items = new_items;
 *           self->capacity = new_cap;
 *       }
 *       self->items[self->count++] = item;
 *   }
 *
 * **Fix Summary.** Phase 67-02 rewrote the IRON_LIST_IMPL _push macro body
 * to (1) compute the new capacity into a local temporary, (2) check for
 * int64_t wraparound on the doubling, (3) capture realloc into a named
 * temporary, (4) NULL-check it, and (5) only then commit the new backing
 * pointer. Both the capacity-overflow path and the NULL-realloc path route
 * through iron_oom_abort which prints a bisectable "iron: out of memory at
 * Iron_List_T_push[: capacity overflow]" message before abort(3).
 *
 * **Strategy for this test.** We exercise the capacity-overflow path, not
 * the realloc-NULL path, because forcing realloc to return NULL portably
 * would require an LD_PRELOAD shim or link-time __wrap_realloc and the
 * existing test harness does neither. The capacity-overflow path produces
 * the same iron_oom_abort call under controlled conditions: we set
 * self->capacity to INT64_MAX/2 + 1 so the doubling wraps negative, and
 * the new guard fires before any realloc is attempted. That verifies both
 * (a) the new_cap < self->capacity wraparound detector, and (b) the
 * iron_oom_abort stderr contract, in one fork-and-assert cycle. The
 * realloc-NULL arm shares the same abort path one line away from the
 * overflow arm, so if the overflow arm is correct the NULL arm is too.
 *
 * We fork so the child's abort() doesn't kill the test binary, redirect
 * the child's stderr to a pipe so the parent can verify the diagnostic
 * message, then assert the child exited via SIGABRT and the captured
 * message contains the expected prefix.
 *
 * **Severity.** H — pre-fix every Iron program that used a List was one
 * OOM away from a silent segfault. Post-fix users get a clear diagnostic.
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

/* Iron_List_int64_t is already declared + implemented by
 * src/runtime/iron_collections.c, which iron_runtime links into. We just
 * use the symbols directly; no re-instantiation needed. */

void test_alloc_list_push_oom_aborts_on_capacity_overflow(void) {
    int stderr_pipe[2];
    TEST_ASSERT_EQUAL(0, pipe(stderr_pipe));

    pid_t child = fork();
    TEST_ASSERT_TRUE(child >= 0);

    if (child == 0) {
        /* Child: arrange for the next push to trigger the capacity-doubling
         * branch with a starting capacity that will wrap int64_t. */
        close(stderr_pipe[0]);
        dup2(stderr_pipe[1], 2);

        Iron_List_int64_t list = Iron_List_int64_t_create();
        list.capacity = INT64_MAX / 2 + 1;  /* doubling wraps negative */
        list.count    = list.capacity;       /* force into the grow branch */
        list.items    = NULL;                /* never read — we abort first */

        Iron_List_int64_t_push(&list, 42);   /* must call iron_oom_abort */
        _exit(99);                            /* unreached */
    }

    /* Parent: read the child's stderr, then reap it. */
    close(stderr_pipe[1]);

    char buf[512] = {0};
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(stderr_pipe[0], buf + total,
                          sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(child, &status, 0);

    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status),
                              "child must exit via signal (SIGABRT)");
    TEST_ASSERT_EQUAL_MESSAGE(SIGABRT, WTERMSIG(status),
                               "child must receive SIGABRT from iron_oom_abort");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: out of memory at"),
                                  "stderr must contain iron_oom_abort prefix");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Iron_List_int64_t_push"),
                                  "stderr must identify the list push call-site");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "capacity overflow"),
                                  "stderr must identify the overflow path");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_alloc_list_push_oom_aborts_on_capacity_overflow);
    return UNITY_END();
}
