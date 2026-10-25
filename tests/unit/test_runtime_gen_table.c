/* test_runtime_gen_table.c: the generation table, arena segments and the
 * stack scope registry (iron_runtime.h, #413 #432 #537 #538 #540 #541).
 *
 * Every property here is one a dereference check relies on: an ended
 * generation is never live again, even after its slot is reused; an arena
 * value allocated after a save point stays stale after a restore however
 * many allocations follow; a scope's pointers die with it at any depth and
 * on any thread, and a thread number given to a later thread does not
 * revive the earlier thread's pointers. */
#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_arena_rt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static bool fp_live(Iron_FatPtr fp) {
    if (fp.gen & IRON_STACK_TAG) return iron_stack_gen_live(fp.gen);
    return iron_gen_live(fp.gen);
}

void test_gen_end_and_reuse(void) {
    uint64_t first = iron_gen_new(false);
    TEST_ASSERT_TRUE(iron_gen_live(first));
    TEST_ASSERT_TRUE(iron_gen_end(first));
    TEST_ASSERT_FALSE(iron_gen_live(first));
    TEST_ASSERT_FALSE(iron_gen_end(first));          /* a second end: a double free */
    /* Reuse the slot many times over: the first generation stays dead. */
    for (int i = 0; i < 100000; i++) {
        uint64_t g = iron_gen_new(i & 1);
        TEST_ASSERT_TRUE(iron_gen_live(g));
        TEST_ASSERT_TRUE(g != first);
        TEST_ASSERT_TRUE(iron_gen_end(g));
        TEST_ASSERT_FALSE(iron_gen_live(first));
    }
    TEST_ASSERT_FALSE(iron_gen_live(0));             /* a null pointer */
}

void test_heap_interior_pointer(void) {
    Iron_FatPtr h = iron_heap_alloc("t.c", 1, 64);
    Iron_FatPtr inner = { (char *)h.addr + 40, h.gen };
    TEST_ASSERT_TRUE(fp_live(inner));
    iron_heap_free(h);
    TEST_ASSERT_FALSE(fp_live(inner));               /* no header is read (#413, #540) */
}

void test_rc_field_pointer(void) {
    void *obj = iron_rc_alloc(32, NULL);
    uint64_t g = iron_rc_ptr_gen(obj);
    TEST_ASSERT_EQUAL_UINT64(g, iron_rc_ptr_gen(obj));   /* one generation per object */
    TEST_ASSERT_TRUE(iron_gen_live(g));
    iron_rc_retain(obj);
    iron_rc_release(obj);
    TEST_ASSERT_TRUE(iron_gen_live(g));
    iron_rc_release(obj);                            /* the last strong handle */
    TEST_ASSERT_FALSE(iron_gen_live(g));
}

void test_arena_restore_never_revives(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "t");
    Iron_FatPtr before = iron_arena_rt_alloc(a, 16);
    Iron_ArenaSave m1 = iron_arena_rt_save(a);
    Iron_FatPtr after = iron_arena_rt_alloc(a, 16);
    iron_arena_rt_restore(a, m1);
    TEST_ASSERT_TRUE(fp_live(before));
    TEST_ASSERT_FALSE(fp_live(after));
    for (int i = 0; i < 50; i++) {                   /* #432: new allocations reused it */
        Iron_FatPtr n = iron_arena_rt_alloc(a, 16);
        TEST_ASSERT_TRUE(fp_live(n));
        TEST_ASSERT_FALSE(fp_live(after));
    }
    iron_arena_rt_restore(a, m1);                    /* the same save point again */
    TEST_ASSERT_TRUE(fp_live(before));
    Iron_ArenaSave m2 = iron_arena_rt_save(a);
    Iron_FatPtr later = iron_arena_rt_alloc(a, 16);
    iron_arena_rt_restore(a, m2);
    TEST_ASSERT_FALSE(fp_live(later));
    TEST_ASSERT_TRUE(fp_live(before));
    iron_arena_rt_reset(a);
    TEST_ASSERT_FALSE(fp_live(before));
    Iron_FatPtr fresh = iron_arena_rt_alloc(a, 16);
    TEST_ASSERT_TRUE(fp_live(fresh));
    iron_arena_rt_destroy(a);
    TEST_ASSERT_FALSE(fp_live(fresh));               /* the arena's end (#540) */
}

void test_arena_save_without_allocation_shares_segment(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "t");
    Iron_ArenaSave m1 = iron_arena_rt_save(a);
    Iron_ArenaSave m2 = iron_arena_rt_save(a);       /* nothing allocated between */
    Iron_FatPtr x = iron_arena_rt_alloc(a, 16);
    iron_arena_rt_restore(a, m1);
    TEST_ASSERT_FALSE(fp_live(x));
    iron_arena_rt_restore(a, m2);                    /* still valid: same point */
    iron_arena_rt_destroy(a);
}

/* Scopes: a pointer dies with its scope at every depth, including the
 * chunks past 1024 and the saturated 16 bit depth field. */
static Iron_FatPtr s_at[70000];

void test_stack_deep_scopes(void) {
    int local = 1;
    const int depth = 66000;
    uint64_t *saved = (uint64_t *)malloc(sizeof(uint64_t) * depth);
    TEST_ASSERT_NOT_NULL(saved);
    for (int d = 0; d < depth; d++) {
        saved[d] = iron_stack_enter();
        s_at[d] = (Iron_FatPtr){ &local, iron_stack_gen };
    }
    TEST_ASSERT_TRUE(fp_live(s_at[0]));
    TEST_ASSERT_TRUE(fp_live(s_at[1500]));
    TEST_ASSERT_TRUE(fp_live(s_at[65600]));          /* saturated depth: found by id */
    TEST_ASSERT_TRUE(fp_live(s_at[depth - 1]));
    for (int d = depth - 1; d >= 1200; d--) iron_stack_leave(saved[d]);
    TEST_ASSERT_TRUE(fp_live(s_at[1199]));
    TEST_ASSERT_FALSE(fp_live(s_at[1200]));
    TEST_ASSERT_FALSE(fp_live(s_at[65600]));
    /* re-enter the same depths: the old pointers stay dead */
    for (int d = 1200; d < 3000; d++) saved[d] = iron_stack_enter();
    TEST_ASSERT_FALSE(fp_live(s_at[1200]));
    TEST_ASSERT_FALSE(fp_live(s_at[2999]));
    for (int d = 2999; d >= 0; d--) iron_stack_leave(saved[d]);
    TEST_ASSERT_FALSE(fp_live(s_at[0]));
    free(saved);
}

/* Threads: a pointer into a thread's scope, read by another thread. */
typedef struct {
    Iron_FatPtr  ptr;          /* taken by the worker */
    Iron_FatPtr  check;        /* checked by the worker */
    bool         live;
    bool         check_live;
} ThreadCase;

static void *worker_take_and_exit(void *arg) {
    ThreadCase *c = (ThreadCase *)arg;
    static int cell;
    uint64_t s = iron_stack_enter();
    c->ptr = (Iron_FatPtr){ &cell, iron_stack_gen };
    c->live = fp_live(c->ptr);
    iron_stack_leave(s);
    s = iron_stack_enter();                      /* a scope left open at exit */
    c->ptr = (Iron_FatPtr){ &cell, iron_stack_gen };
    (void)s;
    return NULL;
}

static void *worker_check(void *arg) {
    ThreadCase *c = (ThreadCase *)arg;
    c->check_live = fp_live(c->check);
    return NULL;
}

void test_stack_cross_thread(void) {
    ThreadCase c;
    memset(&c, 0, sizeof c);
    iron_thread_t t;
    TEST_ASSERT_EQUAL_INT(0, iron_thread_create(&t, worker_take_and_exit, &c));
    iron_thread_join(t);
    TEST_ASSERT_TRUE(c.live);
    TEST_ASSERT_FALSE(fp_live(c.ptr));               /* its thread has ended */

    /* A later thread that takes the same number does not revive it. */
    ThreadCase c2;
    memset(&c2, 0, sizeof c2);
    TEST_ASSERT_EQUAL_INT(0, iron_thread_create(&t, worker_take_and_exit, &c2));
    iron_thread_join(t);
    TEST_ASSERT_FALSE(fp_live(c.ptr));
    TEST_ASSERT_FALSE(fp_live(c2.ptr));

    /* A pointer into this thread's open scope, checked on another thread. */
    int local = 3;
    uint64_t s = iron_stack_enter();
    ThreadCase c3;
    memset(&c3, 0, sizeof c3);
    c3.check = (Iron_FatPtr){ &local, iron_stack_gen };
    TEST_ASSERT_EQUAL_INT(0, iron_thread_create(&t, worker_check, &c3));
    iron_thread_join(t);
    TEST_ASSERT_TRUE(c3.check_live);
    iron_stack_leave(s);
    c3.check_live = true;
    TEST_ASSERT_EQUAL_INT(0, iron_thread_create(&t, worker_check, &c3));
    iron_thread_join(t);
    TEST_ASSERT_FALSE(c3.check_live);                /* the scope has ended */
}

void test_list_pins(void) {
    int buf[4] = {0};
    TEST_ASSERT_FALSE(iron_list_is_pinned(buf));
    uint64_t g1 = iron_list_pin(buf);
    uint64_t g2 = iron_list_pin(buf);                /* a nested borrow */
    TEST_ASSERT_TRUE(iron_list_is_pinned(buf));
    TEST_ASSERT_TRUE(iron_gen_live(g1) && iron_gen_live(g2));
    iron_list_unpin(buf, g2);
    TEST_ASSERT_TRUE(iron_list_is_pinned(buf));
    TEST_ASSERT_FALSE(iron_gen_live(g2));
    iron_list_unpin(buf, g1);
    TEST_ASSERT_FALSE(iron_list_is_pinned(buf));
    TEST_ASSERT_FALSE(iron_gen_live(g1));
    uint64_t g3 = iron_list_pin(NULL);               /* a stack array: no pin, a generation */
    TEST_ASSERT_TRUE(iron_gen_live(g3));
    iron_list_unpin(NULL, g3);
    TEST_ASSERT_FALSE(iron_gen_live(g3));
}

/* A thread that uses up the 2^35 scope ids of its thread number takes a
 * fresh number: scopes opened before stay live under the old one, a scope
 * that ended is never matched by a later scope at the same depth, and the
 * used-up number is not given out again. */
void test_stack_ids_never_repeat(void) {
    uint64_t s0 = iron_stack_enter();              /* keeps the thread attached */
    uint64_t g0 = iron_stack_gen;
    uint32_t old_no = iron_stack_thread_no();
    TEST_ASSERT_TRUE(old_no != 0);

    /* The last ids of this number. */
    iron_stk.next_id = IRON_STACK_ID_MASK - 2;
    uint64_t s1 = iron_stack_enter();
    uint64_t g1 = iron_stack_gen;                  /* id MASK-1, stays open */
    uint64_t s2 = iron_stack_enter();
    uint64_t g2 = iron_stack_gen;                  /* id MASK, the last one */
    iron_stack_leave(s2);
    TEST_ASSERT_FALSE(iron_stack_gen_live(g2));
    TEST_ASSERT_EQUAL_UINT32(old_no, iron_stack_thread_no());

    /* The next scope needs a new number; it sits at g2's depth. */
    uint64_t s3 = iron_stack_enter();
    uint64_t g3 = iron_stack_gen;
    uint32_t new_no = iron_stack_thread_no();
    TEST_ASSERT_TRUE(new_no != old_no);
    TEST_ASSERT_TRUE(iron_stack_gen_live(g3));
    TEST_ASSERT_FALSE(iron_stack_gen_live(g2));    /* same depth, ended */
    TEST_ASSERT_TRUE(iron_stack_gen_live(g1));     /* opened under the old number */
    TEST_ASSERT_TRUE(iron_stack_gen_live(g0));

    /* Every id the old number could give at this depth is gone: no later
     * scope of the new number has g2's key, whatever its id. */
    iron_stack_leave(s3);
    for (int i = 0; i < 1000; i++) {
        uint64_t s = iron_stack_enter();
        TEST_ASSERT_TRUE((iron_stack_gen & IRON_STACK_KEY_MASK) != (g2 & IRON_STACK_KEY_MASK));
        TEST_ASSERT_FALSE(iron_stack_gen_live(g2));
        TEST_ASSERT_FALSE(iron_stack_gen_live(g3));
        iron_stack_leave(s);
    }
    iron_stack_leave(s1);
    TEST_ASSERT_FALSE(iron_stack_gen_live(g1));
    TEST_ASSERT_TRUE(iron_stack_gen_live(g0));
    iron_stack_leave(s0);
    TEST_ASSERT_FALSE(iron_stack_gen_live(g0));

    /* After the thread lets go of its numbers, the used-up one is retired:
     * a new attach never gets it back. */
    iron_stack_thread_exit();
    for (int i = 0; i < 8; i++) {
        uint64_t s = iron_stack_enter();
        TEST_ASSERT_TRUE(iron_stack_thread_no() != old_no);
        TEST_ASSERT_FALSE(iron_stack_gen_live(g1));
        TEST_ASSERT_FALSE(iron_stack_gen_live(g2));
        iron_stack_leave(s);
        iron_stack_thread_exit();
    }
}

int main(void) {
    iron_runtime_init(0, NULL);
    UNITY_BEGIN();
    RUN_TEST(test_gen_end_and_reuse);
    RUN_TEST(test_heap_interior_pointer);
    RUN_TEST(test_rc_field_pointer);
    RUN_TEST(test_arena_restore_never_revives);
    RUN_TEST(test_arena_save_without_allocation_shares_segment);
    RUN_TEST(test_stack_deep_scopes);
    RUN_TEST(test_stack_cross_thread);
    RUN_TEST(test_list_pins);
    RUN_TEST(test_stack_ids_never_repeat);
    return UNITY_END();
}
