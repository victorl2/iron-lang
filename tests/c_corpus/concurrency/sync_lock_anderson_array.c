/*
 * title: Anderson array-based queue lock
 * topic: concurrency
 * covers: array queue lock, per-slot flags, slot recycling, FIFO slot order, local spinning, bounded waiters
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 5, PER = 250, SLOTS = 8 }; /* SLOTS must be >= number of concurrent contenders */

typedef struct {
    atomic_int can_go[SLOTS];
    atomic_uint tail;
} ALock;

static ALock al;

static void alock_init(ALock *l) {
    for (int i = 0; i < SLOTS; i++)
        atomic_store(&l->can_go[i], i == 0);
    atomic_store(&l->tail, 0);
}

static unsigned alock_acquire(ALock *l) {
    unsigned ticket = atomic_fetch_add_explicit(&l->tail, 1u, memory_order_relaxed);
    unsigned slot = ticket % SLOTS;
    while (!atomic_load_explicit(&l->can_go[slot], memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&l->can_go[slot], 0, memory_order_relaxed); /* recycle for a later lap */
    return ticket;
}

static void alock_release(ALock *l, unsigned ticket) {
    atomic_store_explicit(&l->can_go[(ticket + 1u) % SLOTS], 1, memory_order_release);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned seq[T * PER];
static int seq_len;
static atomic_int in_cs, violations;
static int a_field, b_field;

static void *worker(void *p) {
    (void)p;
    for (int i = 0; i < PER; i++) {
        unsigned t = alock_acquire(&al);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        seq[seq_len++] = t;
        a_field++;
        if (i % 5 == 0)
            sched_yield();
        b_field += 2;
        atomic_fetch_sub(&in_cs, 1);
        alock_release(&al, t);
    }
    return NULL;
}

int main(void) {
    alock_init(&al);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int in_order = 1;
    int slot_hist[SLOTS] = {0};
    for (int k = 0; k < seq_len; k++) {
        if (seq[k] != (unsigned)k)
            in_order = 0;
        slot_hist[seq[k] % SLOTS]++;
    }
    printf("threads=%d slots=%d acquisitions=%d\n", T, SLOTS, seq_len);
    printf("tickets granted in issue order: %s\n", in_order ? "yes" : "no");
    printf("slot usage:");
    for (int s = 0; s < SLOTS; s++)
        printf(" %d", slot_hist[s]);
    printf("\nfields a=%d b=%d violations=%d\n", a_field, b_field, atomic_load(&violations));
    printf("laps of the slot ring: %u\n", atomic_load(&al.tail) / SLOTS);
    check(seq_len == T * PER && in_order, "order");
    check(a_field == T * PER && b_field == 2 * a_field, "fields");
    check(atomic_load(&violations) == 0, "exclusion");
    /* after the last release exactly one slot is armed: that of the next ticket */
    int armed = 0;
    for (int s = 0; s < SLOTS; s++)
        armed += atomic_load(&al.can_go[s]);
    check(armed == 1 && atomic_load(&al.can_go[atomic_load(&al.tail) % SLOTS]) == 1, "armed slot");
    printf("exactly one slot armed at rest: yes\n");
    return 0;
}
