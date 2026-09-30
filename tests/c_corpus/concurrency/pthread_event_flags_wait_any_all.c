/*
 * title: Event flag group with wait-any and wait-all
 * topic: concurrency
 * covers: bit masks under a condvar, wait any/all semantics, clear-on-exit, broadcast on set
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    EV_A = 1u << 0,
    EV_B = 1u << 1,
    EV_C = 1u << 2,
    EV_D = 1u << 3,
    EV_E = 1u << 4
};

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned bits;
    int waiters;
} Events;

static Events ev = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void ev_set(unsigned m) {
    pthread_mutex_lock(&ev.mu);
    ev.bits |= m;
    pthread_cond_broadcast(&ev.cv);
    pthread_mutex_unlock(&ev.mu);
}

/* returns the bits that satisfied the wait; clears them when clear != 0 */
static unsigned ev_wait(unsigned mask, int all, int clear) {
    pthread_mutex_lock(&ev.mu);
    ev.waiters++;
    for (;;) {
        unsigned got = ev.bits & mask;
        int ok = all ? (got == mask) : (got != 0);
        if (ok) {
            if (clear)
                ev.bits &= ~got;
            ev.waiters--;
            pthread_mutex_unlock(&ev.mu);
            return got;
        }
        pthread_cond_wait(&ev.cv, &ev.mu);
    }
}

static void wait_for_waiters(int n) {
    for (;;) {
        pthread_mutex_lock(&ev.mu);
        int w = ev.waiters;
        pthread_mutex_unlock(&ev.mu);
        if (w >= n)
            return;
        sched_yield();
    }
}

typedef struct {
    unsigned mask;
    int all;
    int clear;
    unsigned got;
} Arg;

static void *waiter(void *p) {
    Arg *a = p;
    a->got = ev_wait(a->mask, a->all, a->clear);
    return NULL;
}

int main(void) {
    Arg any_ab = {EV_A | EV_B, 0, 0, 0};
    Arg all_cde = {EV_C | EV_D | EV_E, 1, 0, 0};
    Arg any_e = {EV_E, 0, 0, 0};
    pthread_t t1, t2, t3;
    check(pthread_create(&t1, NULL, waiter, &any_ab) == 0, "c1");
    check(pthread_create(&t2, NULL, waiter, &all_cde) == 0, "c2");
    wait_for_waiters(2);
    ev_set(EV_B); /* releases the any-of-A|B waiter only */
    pthread_join(t1, NULL);
    printf("any(A|B) woke with mask %#x\n", any_ab.got);
    check(any_ab.got == EV_B, "any got B");

    ev_set(EV_C);
    ev_set(EV_D);
    /* all(C|D|E) is still pending: give it a moment by checking state, not time */
    pthread_mutex_lock(&ev.mu);
    check(ev.bits == (EV_B | EV_C | EV_D), "bits before E");
    pthread_mutex_unlock(&ev.mu);
    check(pthread_create(&t3, NULL, waiter, &any_e) == 0, "c3");
    wait_for_waiters(2);
    ev_set(EV_E);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    printf("all(C|D|E) woke with mask %#x\n", all_cde.got);
    printf("any(E) woke with mask %#x\n", any_e.got);
    check(all_cde.got == (EV_C | EV_D | EV_E), "all got CDE");
    check(any_e.got == EV_E, "any E");
    /* consume C and D with a clearing wait-all; B must remain, E was never cleared */
    unsigned cd = ev_wait(EV_C | EV_D, 1, 1);
    check(cd == (EV_C | EV_D), "clearing wait got C|D");
    pthread_mutex_lock(&ev.mu);
    unsigned rest = ev.bits & (EV_A | EV_B | EV_C | EV_D);
    unsigned e_left = ev.bits & EV_E;
    pthread_mutex_unlock(&ev.mu);
    check(e_left == EV_E, "E still set");
    printf("leftover A..D bits: %#x\n", rest);
    check(rest == EV_B, "only B left");
    return 0;
}
