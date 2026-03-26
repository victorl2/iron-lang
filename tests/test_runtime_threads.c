/* test_runtime_threads.c — Unity tests for Iron threading primitives.
 *
 * Tests Iron_Pool, Iron_Handle, Iron_Channel, Iron_Mutex, and the global pool.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Helpers ─────────────────────────────────────────────────────────────── */

typedef struct {
    atomic_int *counter;
    int         increment;
} IncrArgs;

static void increment_fn(void *arg) {
    IncrArgs *a = (IncrArgs *)arg;
    atomic_fetch_add(a->counter, a->increment);
}

/* Small busy-work to simulate non-trivial work items */
static void busy_work_fn(void *arg) {
    volatile int *done = (volatile int *)arg;
    /* ~1000 iterations of trivial arithmetic */
    volatile long x = 1;
    for (int i = 0; i < 1000; i++) { x *= 2; x /= 2; }
    (void)x;
    atomic_store((atomic_int *)done, 1);
}

/* ── Iron_Pool tests ─────────────────────────────────────────────────────── */

void test_pool_create_destroy(void) {
    Iron_Pool *pool = Iron_pool_create("test-cd", 2);
    TEST_ASSERT_NOT_NULL(pool);
    Iron_pool_destroy(pool);
    /* Should not crash */
}

void test_pool_submit_single(void) {
    Iron_Pool  *pool = Iron_pool_create("test-single", 2);
    atomic_int  counter;
    atomic_init(&counter, 0);

    IncrArgs args = { &counter, 1 };
    Iron_pool_submit(pool, increment_fn, &args);
    Iron_pool_barrier(pool);

    TEST_ASSERT_EQUAL_INT(1, atomic_load(&counter));
    Iron_pool_destroy(pool);
}

void test_pool_submit_many(void) {
    Iron_Pool  *pool = Iron_pool_create("test-many", 4);
    atomic_int  counter;
    atomic_init(&counter, 0);

    /* We need 100 separate arg structs since each points to counter */
    static IncrArgs all_args[100];
    for (int i = 0; i < 100; i++) {
        all_args[i].counter   = &counter;
        all_args[i].increment = 1;
        Iron_pool_submit(pool, increment_fn, &all_args[i]);
    }
    Iron_pool_barrier(pool);

    TEST_ASSERT_EQUAL_INT(100, atomic_load(&counter));
    Iron_pool_destroy(pool);
}

void test_pool_barrier_waits(void) {
    Iron_Pool  *pool = Iron_pool_create("test-barrier", 2);
    static atomic_int done_flags[10];

    for (int i = 0; i < 10; i++) {
        atomic_init(&done_flags[i], 0);
        Iron_pool_submit(pool, busy_work_fn, &done_flags[i]);
    }
    Iron_pool_barrier(pool);

    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT(1, atomic_load(&done_flags[i]));
    }
    Iron_pool_destroy(pool);
}

void test_pool_thread_count(void) {
    Iron_Pool *pool = Iron_pool_create("test-count", 4);
    TEST_ASSERT_EQUAL_INT(4, Iron_pool_thread_count(pool));
    Iron_pool_destroy(pool);
}

/* ── Iron_Channel tests ──────────────────────────────────────────────────── */

typedef struct {
    Iron_Channel *ch;
    void         *value;
} ChanSendArgs;

static void channel_sender_fn(void *arg) {
    ChanSendArgs *a = (ChanSendArgs *)arg;
    Iron_channel_send(a->ch, a->value);
}

void test_channel_send_recv(void) {
    Iron_Channel *ch = Iron_channel_create(1);
    int value = 42;

    /* Send from a separate thread, recv on main thread */
    ChanSendArgs args = { ch, &value };
    Iron_Handle *h = Iron_handle_create(channel_sender_fn, &args);

    void *received = Iron_channel_recv(ch);
    Iron_handle_wait(h);
    Iron_handle_destroy(h);

    TEST_ASSERT_EQUAL_PTR(&value, received);
    Iron_channel_destroy(ch);
}

void test_channel_blocking(void) {
    Iron_Channel *ch = Iron_channel_create(2);
    int a = 100, b = 200;

    /* Send two items from two threads */
    ChanSendArgs args_a = { ch, &a };
    ChanSendArgs args_b = { ch, &b };
    Iron_Handle *ha = Iron_handle_create(channel_sender_fn, &args_a);
    Iron_Handle *hb = Iron_handle_create(channel_sender_fn, &args_b);

    Iron_handle_wait(ha);
    Iron_handle_wait(hb);
    Iron_handle_destroy(ha);
    Iron_handle_destroy(hb);

    void *r1 = Iron_channel_recv(ch);
    void *r2 = Iron_channel_recv(ch);

    /* Both a and b should have arrived (order not guaranteed) */
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_NOT_NULL(r2);
    int v1 = *(int *)r1;
    int v2 = *(int *)r2;
    TEST_ASSERT_TRUE((v1 == 100 && v2 == 200) || (v1 == 200 && v2 == 100));

    Iron_channel_destroy(ch);
}

void test_channel_try_recv_empty(void) {
    Iron_Channel *ch = Iron_channel_create(4);
    void *out = NULL;
    bool got = Iron_channel_try_recv(ch, &out);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_NULL(out);
    Iron_channel_destroy(ch);
}

void test_channel_try_recv_has_item(void) {
    Iron_Channel *ch = Iron_channel_create(4);
    int value = 77;
    Iron_channel_send(ch, &value);

    void *out = NULL;
    bool got = Iron_channel_try_recv(ch, &out);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_PTR(&value, out);

    Iron_channel_destroy(ch);
}

/* ── Iron_Mutex tests ────────────────────────────────────────────────────── */

void test_mutex_lock_unlock(void) {
    int initial = 42;
    Iron_Mutex *m = Iron_mutex_create(&initial, sizeof(int));
    TEST_ASSERT_NOT_NULL(m);

    int *val = (int *)Iron_mutex_lock(m);
    TEST_ASSERT_EQUAL_INT(42, *val);
    *val = 99;
    Iron_mutex_unlock(m);

    val = (int *)Iron_mutex_lock(m);
    TEST_ASSERT_EQUAL_INT(99, *val);
    Iron_mutex_unlock(m);

    Iron_mutex_destroy(m);
}

typedef struct {
    Iron_Mutex *mutex;
    int         increments;
} MutexIncrArgs;

static void mutex_incr_fn(void *arg) {
    MutexIncrArgs *a = (MutexIncrArgs *)arg;
    for (int i = 0; i < a->increments; i++) {
        int *val = (int *)Iron_mutex_lock(a->mutex);
        (*val)++;
        Iron_mutex_unlock(a->mutex);
    }
}

void test_mutex_concurrent(void) {
    int initial = 0;
    Iron_Mutex *m = Iron_mutex_create(&initial, sizeof(int));

    /* 10 threads each increment the counter 100 times */
    const int NTHREADS = 10;
    const int ITERS    = 100;
    Iron_Handle *handles[10];
    MutexIncrArgs args[10];

    for (int i = 0; i < NTHREADS; i++) {
        args[i].mutex      = m;
        args[i].increments = ITERS;
        handles[i]         = Iron_handle_create(mutex_incr_fn, &args[i]);
    }

    for (int i = 0; i < NTHREADS; i++) {
        Iron_handle_wait(handles[i]);
        Iron_handle_destroy(handles[i]);
    }

    int *final = (int *)Iron_mutex_lock(m);
    int result = *final;
    Iron_mutex_unlock(m);

    TEST_ASSERT_EQUAL_INT(NTHREADS * ITERS, result);
    Iron_mutex_destroy(m);
}

/* ── Iron_Handle tests ───────────────────────────────────────────────────── */

static atomic_int g_handle_executed;

static void handle_task_fn(void *arg) {
    (void)arg;
    atomic_store(&g_handle_executed, 1);
}

void test_handle_create_wait(void) {
    atomic_init(&g_handle_executed, 0);
    Iron_Handle *h = Iron_handle_create(handle_task_fn, NULL);
    TEST_ASSERT_NOT_NULL(h);
    Iron_handle_wait(h);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_handle_executed));
    Iron_handle_destroy(h);
}

/* ── Global pool tests ───────────────────────────────────────────────────── */

void test_global_pool_exists(void) {
    /* iron_runtime_init() (setUp) should have created the global pool */
    TEST_ASSERT_NOT_NULL(Iron_global_pool);
    TEST_ASSERT_GREATER_THAN(0, Iron_pool_thread_count(Iron_global_pool));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Iron_Pool */
    RUN_TEST(test_pool_create_destroy);
    RUN_TEST(test_pool_submit_single);
    RUN_TEST(test_pool_submit_many);
    RUN_TEST(test_pool_barrier_waits);
    RUN_TEST(test_pool_thread_count);

    /* Iron_Channel */
    RUN_TEST(test_channel_send_recv);
    RUN_TEST(test_channel_blocking);
    RUN_TEST(test_channel_try_recv_empty);
    RUN_TEST(test_channel_try_recv_has_item);

    /* Iron_Mutex */
    RUN_TEST(test_mutex_lock_unlock);
    RUN_TEST(test_mutex_concurrent);

    /* Iron_Handle */
    RUN_TEST(test_handle_create_wait);

    /* Global pool */
    RUN_TEST(test_global_pool_exists);

    return UNITY_END();
}
