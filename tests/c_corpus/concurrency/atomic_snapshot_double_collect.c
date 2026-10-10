/*
 * title: Atomic snapshot by double collect over versioned registers
 * topic: concurrency
 * covers: obstruction-free snapshot, (value,seq) packed registers, repeated collect, invariant across registers
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Registers hold (seq << 32 | value). A writer bumps seq on every write, so a register that is
 * unchanged between two reads was not written in between (no ABA on values).
 * scan() collects all registers twice and returns when both collects are identical: then no
 * register changed during the second collect, so the collected vector was the true state at one
 * instant (any instant inside the second collect).
 *
 * Invariant used as the oracle: register 0 is a monotone counter, and register i (i>0) is only
 * ever advanced while it is smaller than register 0. So at every real instant r[i] <= r[0]. A
 * plain one-pass collect that reads r[i] before r[0] can see r[i] > r[0]; a double-collect
 * snapshot never can. Successive snapshots by one scanner must also be componentwise monotone.
 */
enum { NREG = 4, WRITES = 3000, NSCAN = 2 };

static atomic_uint_least64_t reg[NREG];
static atomic_int writers_done;
static long snaps_taken[NSCAN];
static atomic_long violations;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void write_reg(int i, uint32_t v) {
    uint64_t old = atomic_load(&reg[i]);
    atomic_store(&reg[i], ((old & 0xffffffff00000000ull) + (1ull << 32)) | v);
}

static void collect(uint64_t *out) {
    for (int i = NREG - 1; i >= 0; i--) /* read r[0] last on purpose */
        out[i] = atomic_load(&reg[i]);
}

static void scan(uint32_t *vals) {
    uint64_t a[NREG], b[NREG];
    collect(a);
    for (;;) {
        collect(b);
        int same = 1;
        for (int i = 0; i < NREG; i++)
            if (a[i] != b[i])
                same = 0;
        if (same)
            break;
        for (int i = 0; i < NREG; i++)
            a[i] = b[i];
        sched_yield();
    }
    for (int i = 0; i < NREG; i++)
        vals[i] = (uint32_t)a[i];
}

static void *lead_writer(void *p) {
    (void)p;
    for (uint32_t k = 1; k <= WRITES; k++) {
        write_reg(0, k);
        if (k % 32 == 0)
            sched_yield();
    }
    atomic_fetch_add(&writers_done, 1);
    return NULL;
}

static void *follow_writer(void *p) {
    int i = (int)(size_t)p;
    uint32_t mine = 0;
    while (mine < WRITES) {
        uint32_t lead = (uint32_t)atomic_load(&reg[0]);
        if (mine < lead) {
            mine++;
            write_reg(i, mine); /* only advances while below register 0 */
        } else {
            sched_yield();
        }
    }
    atomic_fetch_add(&writers_done, 1);
    return NULL;
}

static void *scanner(void *p) {
    int id = (int)(size_t)p;
    uint32_t prev[NREG] = {0};
    long taken = 0, bad = 0;
    while (atomic_load(&writers_done) < NREG) {
        uint32_t v[NREG];
        scan(v);
        for (int i = 1; i < NREG; i++)
            if (v[i] > v[0])
                bad++;
        for (int i = 0; i < NREG; i++) {
            if (v[i] < prev[i])
                bad++;
            prev[i] = v[i];
        }
        taken++;
    }
    snaps_taken[id] = taken;
    atomic_fetch_add(&violations, bad);
    return NULL;
}

int main(void) {
    pthread_t w[NREG], s[NSCAN];
    for (int i = 0; i < NSCAN; i++)
        check(pthread_create(&s[i], NULL, scanner, (void *)(size_t)i) == 0, "create");
    check(pthread_create(&w[0], NULL, lead_writer, NULL) == 0, "create");
    for (int i = 1; i < NREG; i++)
        check(pthread_create(&w[i], NULL, follow_writer, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < NREG; i++)
        pthread_join(w[i], NULL);
    for (int i = 0; i < NSCAN; i++)
        pthread_join(s[i], NULL);
    long viol = atomic_load(&violations);
    check(viol == 0, "snapshot invariant");
    uint32_t fin[NREG];
    scan(fin);
    for (int i = 0; i < NREG; i++) {
        check(fin[i] == WRITES, "final value");
        check((atomic_load(&reg[i]) >> 32) == WRITES, "final seq");
    }
    printf("final snapshot:");
    for (int i = 0; i < NREG; i++)
        printf(" %u", fin[i]);
    printf("\n");
    printf("snapshot invariant violations: %ld\n", viol);
    return 0;
}
