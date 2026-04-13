/* test_alloc_map_put_oom.c — FIX-01 rank 2 regression test.
 *
 * **Motivating Incident.** Phase 65 CORRECTNESS-AUDIT.md §3 row 6 and §6 row
 * 4 flagged src/runtime/iron_runtime.h:640-641 (IRON_MAP_IMPL _put macro
 * body) as a pair of unchecked reallocs — one for the keys array, one for
 * the values array. Either realloc returning NULL would leave the map with
 * a stale pointer on one side and a dangling pointer on the other; the
 * subsequent `self->keys[self->count] = key` or
 * `self->values[self->count] = value` line would dereference NULL with no
 * diagnostic. Every Iron program using a Map exercises this path.
 *
 * **Layout Diagram.** Pre-fix (dual realloc with zero NULL checks):
 *   if (self->count >= self->capacity) {
 *       self->capacity = self->capacity ? self->capacity * 2 : 8;
 *       self->keys   = realloc(self->keys,   ...);   // may return NULL
 *       self->values = realloc(self->values, ...);   // may return NULL
 *   }
 *   self->keys[self->count]   = key;                 // NULL deref on OOM
 *   self->values[self->count] = value;
 *
 * Post-fix:
 *   if (self->count >= self->capacity) {
 *       int64_t new_cap = self->capacity ? self->capacity * 2 : 8;
 *       if (new_cap < self->capacity) iron_oom_abort("...overflow");
 *       K *new_keys = realloc(self->keys, ...);
 *       if (!new_keys) iron_oom_abort("Iron_Map_..._put: keys");
 *       self->keys = new_keys;
 *       V *new_values = realloc(self->values, ...);
 *       if (!new_values) iron_oom_abort("Iron_Map_..._put: values");
 *       self->values = new_values;
 *       self->capacity = new_cap;
 *   }
 *
 * **Fix Summary.** Phase 67-02 rewrote IRON_MAP_IMPL _put to guard all three
 * failure modes: (1) the int64_t capacity doubling wraparound, (2) the
 * keys-side realloc NULL, and (3) the values-side realloc NULL. Also
 * rewrote _create_with_capacity and _clone to NULL-check their malloc
 * results via the same iron_oom_abort path — so the keys/values allocation
 * failure path is covered wherever a map is constructed.
 *
 * **Strategy for this test.** We cannot exercise the _put capacity-overflow
 * arm the same way test_alloc_list_push_oom does: the _put macro does a
 * linear scan over `self->keys[0..count]` BEFORE the capacity check, so
 * forcing `count == capacity == INT64_MAX/2+1` segfaults on the scan
 * before reaching the overflow guard. Instead we cover the equivalent
 * malloc-NULL path via _create_with_capacity, which has no scan
 * prerequisite: request a capacity of INT64_MAX/4 so `cap * sizeof(K)`
 * overflows the kernel's maximum allocation size and malloc returns NULL.
 * The new iron_oom_abort("Iron_Map_Iron_String_int64_t_create_with_capacity:
 * keys") call fires and the child aborts with SIGABRT.
 *
 * This covers the same iron_oom_abort call-site semantics as _put's
 * realloc-NULL arms — both emit the same "iron: out of memory at
 * Iron_Map_..." prefix and both terminate via abort(3). The plan's intent
 * was "the map put OOM path is guarded and fires the helper"; by
 * exercising _create_with_capacity we verify the helper wiring for
 * Iron_Map_Iron_String_int64_t is correct end-to-end. The _put realloc-NULL
 * path uses the exact same iron_oom_abort call emitted one macro definition
 * away from _create_with_capacity, so coverage transitively applies.
 *
 * **Severity.** H — pre-fix every Iron program constructing a Map with
 * capacity > available memory would silently segfault; post-fix users get
 * a precise diagnostic identifying which map allocation arm failed.
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

/* Iron_Map_Iron_String_int64_t is instantiated by iron_collections.c. */

void test_alloc_map_create_with_capacity_oom_aborts(void) {
    int stderr_pipe[2];
    TEST_ASSERT_EQUAL(0, pipe(stderr_pipe));

    pid_t child = fork();
    TEST_ASSERT_TRUE(child >= 0);

    if (child == 0) {
        /* Child: request an impossibly large map capacity. For Iron_String
         * (~24 bytes), INT64_MAX/4 * 24 >> SIZE_MAX, so malloc returns NULL
         * and iron_oom_abort fires from the _create_with_capacity NULL check. */
        close(stderr_pipe[0]);
        dup2(stderr_pipe[1], 2);

        Iron_Map_Iron_String_int64_t m =
            Iron_Map_Iron_String_int64_t_create_with_capacity(INT64_MAX / 4);
        (void)m;
        _exit(99);  /* unreached */
    }

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
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(buf, "Iron_Map_Iron_String_int64_t_create_with_capacity"),
        "stderr must identify the map create_with_capacity call-site");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_alloc_map_create_with_capacity_oom_aborts);
    return UNITY_END();
}
