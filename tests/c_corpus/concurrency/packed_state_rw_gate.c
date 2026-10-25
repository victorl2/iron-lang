/*
 * title: Reader-writer gate packed in one atomic word
 * topic: concurrency
 * covers: bitfield-packed state, CAS reader admit, writer-waiting count, active-writer bit, writer preference, invariant audit
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * One 64-bit word packs the whole lock state:
 *   bits  0..15  active readers
 *   bits 16..31  writers waiting
 *   bit  32      writer active
 * Readers may enter only when no writer is waiting or active (CAS readers+1). A writer first
 * registers itself with fetch_add(waiting), then loops until readers == 0 and no writer is
 * active and CASes "waiting-1, active=1" in one step. Unlock subtracts the same amounts.
 *
 * Correctness argument: the CAS on the full word makes "no readers and no writer" and
 * "become the writer" one indivisible step, and reader admission checks the same word, so a
 * reader and a writer can never both be inside. Waiting writers block new readers, so writers
 * cannot starve (readers may wait, which is the chosen preference).
 *
 * Audit: the protected pair (a, b) is always changed together by writers; a reader that ever
 * sees a != b was admitted alongside a writer.
 */
#define READER_ONE ((uint64_t)1)
#define READER_MASK ((uint64_t)0xffff)
#define WAIT_ONE ((uint64_t)1 << 16)
#define WAIT_MASK ((uint64_t)0xffff << 16)
#define ACTIVE ((uint64_t)1 << 32)

enum { READERS = 4, WRITERS = 3, RITERS = 4000, WITERS = 800 };

static atomic_uint_least64_t gate;
static long a, b; /* protected */
static atomic_long torn, max_concurrent_readers, cur_readers;
static atomic_long writer_overlap;
static atomic_int in_writer;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void read_lock(void) {
    for (;;) {
        uint64_t s = atomic_load(&gate);
        if ((s & (WAIT_MASK | ACTIVE)) == 0 && atomic_compare_exchange_weak(&gate, &s, s + READER_ONE))
            return;
        sched_yield();
    }
}
static void read_unlock(void) { atomic_fetch_sub(&gate, READER_ONE); }

static void write_lock(void) {
    atomic_fetch_add(&gate, WAIT_ONE);
    for (;;) {
        uint64_t s = atomic_load(&gate);
        if ((s & (READER_MASK | ACTIVE)) == 0 && atomic_compare_exchange_weak(&gate, &s, s - WAIT_ONE + ACTIVE))
            return;
        sched_yield();
    }
}
static void write_unlock(void) { atomic_fetch_sub(&gate, ACTIVE); }

static void *reader(void *p) {
    (void)p;
    for (int i = 0; i < RITERS; i++) {
        read_lock();
        long now = atomic_fetch_add(&cur_readers, 1) + 1;
        long m = atomic_load(&max_concurrent_readers);
        while (now > m && !atomic_compare_exchange_weak(&max_concurrent_readers, &m, now))
            ;
        if (a != b)
            atomic_fetch_add(&torn, 1);
        if (atomic_load(&in_writer))
            atomic_fetch_add(&writer_overlap, 1);
        atomic_fetch_sub(&cur_readers, 1);
        read_unlock();
    }
    return NULL;
}

static void *writer(void *p) {
    (void)p;
    for (int i = 0; i < WITERS; i++) {
        write_lock();
        if (atomic_exchange(&in_writer, 1))
            atomic_fetch_add(&writer_overlap, 1);
        a += 1;
        sched_yield(); /* make a torn state observable to any wrongly admitted reader */
        b += 1;
        atomic_store(&in_writer, 0);
        write_unlock();
    }
    return NULL;
}

int main(void) {
    /* word layout unit checks */
    write_lock();
    check(gate == ACTIVE, "active writer only");
    write_unlock();
    read_lock();
    read_lock();
    check(gate == 2 * READER_ONE, "two readers");
    read_unlock();
    read_unlock();
    check(gate == 0, "idle");
    printf("state word: idle=%llu, writer active=%llu, two readers=%llu\n", 0ull,
           (unsigned long long)ACTIVE, (unsigned long long)(2 * READER_ONE));

    pthread_t r[READERS], w[WRITERS];
    for (int i = 0; i < READERS; i++)
        check(pthread_create(&r[i], NULL, reader, NULL) == 0, "create");
    for (int i = 0; i < WRITERS; i++)
        check(pthread_create(&w[i], NULL, writer, NULL) == 0, "create");
    for (int i = 0; i < WRITERS; i++)
        pthread_join(w[i], NULL);
    for (int i = 0; i < READERS; i++)
        pthread_join(r[i], NULL);
    check(atomic_load(&torn) == 0, "no torn reads");
    check(atomic_load(&writer_overlap) == 0, "no reader/writer or writer/writer overlap");
    check(a == WRITERS * WITERS && b == a, "all writes applied");
    check(atomic_load(&gate) == 0, "gate idle");
    printf("a=%ld b=%ld after %d writers x %d writes\n", a, b, WRITERS, WITERS);
    printf("torn reads=%ld overlaps=%ld gate idle=%s\n", atomic_load(&torn), atomic_load(&writer_overlap),
           atomic_load(&gate) == 0 ? "yes" : "no");
    return 0;
}
