/*
 * title: Hoare monitor with urgent queue built from semaphores
 * topic: concurrency
 * covers: Hoare monitor, signal-and-urgent-wait, urgent queue, if instead of while, semaphore-built monitor
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Counting semaphore from mutex and condvar (unnamed POSIX semaphores are not portable). */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int v;
} Sem;

static void sem_init_(Sem *s, int v) {
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->v = v;
}
static void sem_destroy_(Sem *s) {
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
}
static void sem_wait_(Sem *s) {
    pthread_mutex_lock(&s->mu);
    while (s->v == 0)
        pthread_cond_wait(&s->cv, &s->mu);
    s->v--;
    pthread_mutex_unlock(&s->mu);
}
static void sem_post_(Sem *s) {
    pthread_mutex_lock(&s->mu);
    s->v++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

/* Hoare's construction: mutex, urgent semaphore "next", and one semaphore per condition. */
typedef struct {
    Sem mutex, next;
    int next_count;
} HMon;
typedef struct {
    Sem sem;
    int count;
} HCond;

static HMon mon;

static void mon_enter(void) { sem_wait_(&mon.mutex); }
static void mon_leave(void) {
    if (mon.next_count > 0)
        sem_post_(&mon.next);
    else
        sem_post_(&mon.mutex);
}
static void cond_wait_(HCond *c) {
    c->count++;
    mon_leave();
    sem_wait_(&c->sem);
    c->count--;
}
/* Signal hands the monitor to the woken waiter immediately; the signaller parks on `next`. */
static void cond_signal_(HCond *c) {
    if (c->count > 0) {
        mon.next_count++;
        sem_post_(&c->sem);
        sem_wait_(&mon.next);
        mon.next_count--;
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { CAP = 3, P = 3, C = 3, PER = 200 };

static HCond not_full, not_empty;
static int buf[CAP], head, count;
static atomic_int guarantee_broken;
static long produced_sum, consumed_sum_total;
static int consumed_total;

static void put(int v) {
    mon_enter();
    if (count == CAP) /* Hoare semantics: `if`, not `while`, is enough */
        cond_wait_(&not_full);
    if (count == CAP)
        atomic_fetch_add(&guarantee_broken, 1);
    buf[(head + count) % CAP] = v;
    count++;
    cond_signal_(&not_empty);
    mon_leave();
}
static int get(void) {
    mon_enter();
    if (count == 0)
        cond_wait_(&not_empty);
    if (count == 0)
        atomic_fetch_add(&guarantee_broken, 1);
    int v = buf[head];
    head = (head + 1) % CAP;
    count--;
    cond_signal_(&not_full);
    mon_leave();
    return v;
}

static void *producer(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++)
        put(id * 10000 + i + 1);
    return NULL;
}

static long csum[C];
static int cn[C];
static void *consumer(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++) {
        csum[id] += get();
        cn[id]++;
    }
    return NULL;
}

/* Deterministic Hoare order demo: the signalled thread runs BEFORE the signaller continues. */
static HCond gate;
static char trace[8];
static int trace_len;
static void *hoare_waiter(void *p) {
    (void)p;
    mon_enter();
    cond_wait_(&gate);
    trace[trace_len++] = 'W'; /* runs first, inside the monitor, right after the signal */
    mon_leave();
    return NULL;
}

int main(void) {
    sem_init_(&mon.mutex, 1);
    sem_init_(&mon.next, 0);
    mon.next_count = 0;
    sem_init_(&not_full.sem, 0);
    sem_init_(&not_empty.sem, 0);
    sem_init_(&gate.sem, 0);

    pthread_t hw;
    pthread_create(&hw, NULL, hoare_waiter, NULL);
    for (;;) {
        mon_enter();
        int w = gate.count;
        mon_leave();
        if (w == 1)
            break;
        sched_yield();
    }
    mon_enter();
    trace[trace_len++] = 'a'; /* before the signal */
    cond_signal_(&gate);
    trace[trace_len++] = 'S'; /* after: only reached once the waiter has left the monitor */
    mon_leave();
    pthread_join(hw, NULL);
    trace[trace_len] = 0;
    printf("signal handoff trace: %s\n", trace);
    check(trace[0] == 'a' && trace[1] == 'W' && trace[2] == 'S', "Hoare ordering");

    pthread_t pt[P], ct[C];
    for (int i = 0; i < C; i++)
        pthread_create(&ct[i], NULL, consumer, (void *)(intptr_t)i);
    for (int i = 0; i < P; i++)
        pthread_create(&pt[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < P; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < C; i++)
        pthread_join(ct[i], NULL);
    for (int i = 0; i < C; i++) {
        consumed_sum_total += csum[i];
        consumed_total += cn[i];
    }
    for (int id = 0; id < P; id++)
        for (int i = 0; i < PER; i++)
            produced_sum += id * 10000 + i + 1;
    printf("consumed %d items, sum %ld (produced sum %ld)\n", consumed_total, consumed_sum_total, produced_sum);
    printf("predicate violations after wakeup: %d\n", atomic_load(&guarantee_broken));
    printf("buffer empty at end: %s, urgent queue empty: %s\n", count == 0 ? "yes" : "no",
           mon.next_count == 0 ? "yes" : "no");
    check(consumed_total == P * PER && consumed_sum_total == produced_sum, "totals");
    check(atomic_load(&guarantee_broken) == 0, "Hoare guarantee");
    check(count == 0 && mon.next_count == 0 && not_full.count == 0 && not_empty.count == 0, "idle");
    sem_destroy_(&mon.mutex);
    sem_destroy_(&mon.next);
    sem_destroy_(&not_full.sem);
    sem_destroy_(&not_empty.sem);
    sem_destroy_(&gate.sem);
    return 0;
}
