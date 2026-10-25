/*
 * title: RCU-style config swap with epoch grace periods
 * topic: concurrency
 * covers: read-mostly data, pointer publication, per-reader epoch announcement, grace period wait, poison-after-retire audit
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Two config buffers alternate. A reader announces the global epoch in its slot (seq_cst), then
 * loads `current` (seq_cst) and uses that buffer, then marks itself idle. The writer fills the
 * spare buffer, publishes it, bumps the epoch, and waits until every reader is idle or has
 * announced the new epoch. Only then does it poison the retired buffer (a stand-in for freeing).
 *
 * Correctness argument: seq_cst orders "reader announce, reader load current" against "writer
 * publish, writer scan". Either the reader sees the new buffer, or the writer sees the reader's
 * announcement of the old epoch and waits for it. A reader that sees a poisoned or mixed buffer
 * would be a use-after-retire; the audit counts those and must find none.
 */
enum { READERS = 4, UPDATES = 400, FIELDS = 6, IDLE = -1 };

typedef struct {
    atomic_int f[FIELDS]; /* all equal to the version, or all -7 when poisoned */
} Config;

static Config cfg[2];
static atomic_int current;
static atomic_long global_epoch;
static atomic_long reader_epoch[READERS];
static atomic_int stop;
static long reads_ok[READERS];
static long reads_bad[READERS];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void fill(Config *c, int v) {
    for (int i = 0; i < FIELDS; i++)
        atomic_store_explicit(&c->f[i], v, memory_order_relaxed);
}

static void *reader(void *p) {
    int id = (int)(size_t)p;
    int last_version = 0;
    while (!atomic_load(&stop)) {
        long e = atomic_load(&global_epoch);
        atomic_store(&reader_epoch[id], e);          /* enter read-side section */
        int which = atomic_load(&current);           /* dereference the published pointer */
        int v0 = atomic_load_explicit(&cfg[which].f[0], memory_order_relaxed);
        int ok = v0 >= 0;
        for (int i = 1; i < FIELDS; i++)
            if (atomic_load_explicit(&cfg[which].f[i], memory_order_relaxed) != v0)
                ok = 0;
        atomic_store(&reader_epoch[id], IDLE);       /* leave */
        if (ok && v0 >= last_version) {
            reads_ok[id]++;
            last_version = v0;
        } else {
            reads_bad[id]++;
        }
    }
    return NULL;
}

static void synchronize(long new_epoch) {
    for (int i = 0; i < READERS; i++) {
        for (;;) {
            long e = atomic_load(&reader_epoch[i]);
            if (e == IDLE || e >= new_epoch)
                break;
            sched_yield();
        }
    }
}

int main(void) {
    fill(&cfg[0], 0);
    fill(&cfg[1], -7);
    atomic_store(&current, 0);
    atomic_store(&global_epoch, 0);
    for (int i = 0; i < READERS; i++)
        atomic_store(&reader_epoch[i], IDLE);
    pthread_t th[READERS];
    for (int i = 0; i < READERS; i++)
        check(pthread_create(&th[i], NULL, reader, (void *)(size_t)i) == 0, "create");
    int cur = 0;
    for (int v = 1; v <= UPDATES; v++) {
        int spare = 1 - cur;
        fill(&cfg[spare], v);                 /* spare is unreachable: it was poisoned after its grace period */
        atomic_store(&current, spare);        /* publish */
        cur = spare;
        long ne = atomic_fetch_add(&global_epoch, 1) + 1;
        synchronize(ne);                      /* grace period for the buffer just retired */
        fill(&cfg[1 - cur], -7);              /* reclaim: poison the retired buffer */
        if (v % 8 == 0)
            sched_yield();
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < READERS; i++)
        pthread_join(th[i], NULL);
    long ok = 0, bad = 0;
    for (int i = 0; i < READERS; i++) {
        ok += reads_ok[i];
        bad += reads_bad[i];
    }
    check(bad == 0, "no reader saw a retired or torn config");
    check(atomic_load(&cfg[cur].f[0]) == UPDATES, "final version live");
    printf("updates=%d final version=%d\n", UPDATES, atomic_load(&cfg[cur].f[0]));
    printf("bad reads (poisoned/mixed/backwards): %ld\n", bad);
    (void)ok; /* how many reads happened depends on scheduling, so it is not printed */
    printf("epochs advanced: %ld\n", atomic_load(&global_epoch));
    return 0;
}
