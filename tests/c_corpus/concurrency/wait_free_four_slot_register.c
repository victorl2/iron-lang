/*
 * title: Wait-free four-slot register with one writer and several readers
 * topic: concurrency
 * covers: Simpson four-slot algorithm, seq_cst control bits, torn-read freedom, per-reader registers, monotone versions
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Simpson's four-slot mechanism gives a wait-free single-writer single-reader register for
 * multi-word values with no locks and no retries:
 *   write(v): pair = !reading; index = !slot[pair]; data[pair][index] = v;
 *             slot[pair] = index; latest = pair;
 *   read():   pair = latest; reading = pair; index = slot[pair]; return data[pair][index];
 * The writer never touches the pair the reader announced, and within the reader's pair never
 * the slot the reader is going to use, so payload words are never written and read together.
 *
 * With R readers the writer keeps R independent registers (one per reader) and writes to each in
 * turn: every reader is still wait-free and the writer does R bounded writes per version.
 * All control variables are seq_cst atomics; the payload words are relaxed atomics only so the
 * program has no data race even if the algorithm were wrong.
 *
 * Oracle: each payload is 4 words that must be equal to each other (no torn read) and versions
 * seen by a reader never decrease.
 */
enum { READERS = 3, VERSIONS = 20000, W = 4 };

typedef struct {
    atomic_int data[2][2][W];
    atomic_int slot[2];
    atomic_int latest;
    atomic_int reading;
} Register;

static Register regs[READERS];
static atomic_int writer_done;
static long reads[READERS];
static long torn[READERS];
static int last_seen[READERS];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void reg_write(Register *r, int v) {
    int pair = !atomic_load(&r->reading);
    int index = !atomic_load(&r->slot[pair]);
    for (int i = 0; i < W; i++)
        atomic_store_explicit(&r->data[pair][index][i], v, memory_order_relaxed);
    atomic_store(&r->slot[pair], index);
    atomic_store(&r->latest, pair);
}

static int reg_read(Register *r, int *ok) {
    int pair = atomic_load(&r->latest);
    atomic_store(&r->reading, pair);
    int index = atomic_load(&r->slot[pair]);
    int first = atomic_load_explicit(&r->data[pair][index][0], memory_order_relaxed);
    *ok = 1;
    for (int i = 1; i < W; i++)
        if (atomic_load_explicit(&r->data[pair][index][i], memory_order_relaxed) != first)
            *ok = 0;
    return first;
}

static void *writer(void *p) {
    (void)p;
    for (int v = 1; v <= VERSIONS; v++)
        for (int r = 0; r < READERS; r++)
            reg_write(&regs[r], v);
    atomic_store(&writer_done, 1);
    return NULL;
}

static void *reader(void *p) {
    int id = (int)(size_t)p;
    int last = 0;
    for (;;) {
        int done = atomic_load(&writer_done);
        int ok;
        int v = reg_read(&regs[id], &ok);
        reads[id]++;
        if (!ok || v < last)
            torn[id]++;
        last = v;
        if (done)
            break;
    }
    last_seen[id] = last;
    return NULL;
}

int main(void) {
    pthread_t w, r[READERS];
    for (int i = 0; i < READERS; i++)
        check(pthread_create(&r[i], NULL, reader, (void *)(size_t)i) == 0, "create");
    check(pthread_create(&w, NULL, writer, NULL) == 0, "create");
    pthread_join(w, NULL);
    for (int i = 0; i < READERS; i++)
        pthread_join(r[i], NULL);
    long bad = 0;
    for (int i = 0; i < READERS; i++) {
        bad += torn[i];
        check(last_seen[i] == VERSIONS, "reader ends on the last version");
        check(reads[i] > 0, "reader ran");
    }
    check(bad == 0, "no torn or backwards reads");
    printf("versions written per register: %d\n", VERSIONS);
    printf("torn or backwards reads: %ld\n", bad);
    for (int i = 0; i < READERS; i++)
        printf("reader %d final version: %d\n", i, last_seen[i]);
    return 0;
}
