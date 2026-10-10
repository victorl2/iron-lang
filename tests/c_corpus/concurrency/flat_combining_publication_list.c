/*
 * title: Flat combining over a sequential min-heap
 * topic: concurrency
 * covers: publication slots, combiner election with atomic_flag, batched execution of others' requests, release/acquire result hand-off, heap invariants
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Each thread owns a publication slot. To run an operation it writes (op, arg) into its slot and
 * store-releases state=PENDING. Then it tries to become the combiner by test_and_set on a flag.
 * The combiner scans ALL slots, executes every pending request on the plain, single-threaded
 * data structure (a binary min-heap), writes each result and store-releases state=DONE, then
 * clears the flag. A thread that lost the election spins on its own slot until DONE.
 *
 * Correctness argument: only the flag holder touches the heap (acquire on take, release on
 * clear), so the heap needs no other synchronization. A request is executed exactly once by
 * whoever is the combiner when it is scanned; its owner sees the result via acquire on DONE.
 *
 * Oracle: every inserted value is either extracted or still in the heap (count and sum are
 * conserved), and draining the heap yields a nondecreasing sequence. How many extracts hit an
 * empty heap depends on the schedule, so only the schedule-independent totals are printed.
 */
enum { T = 5, OPS = 3000, HCAP = 20000, OP_INSERT = 1, OP_EXTRACT = 2, IDLE = 0, PENDING = 1, DONE = 2 };

typedef struct {
    atomic_int state;
    int op;
    long arg;
    long result;
    char pad[32];
} Slot;

static Slot slots[T];
static atomic_flag combiner_lock = ATOMIC_FLAG_INIT;
static long heap[HCAP];
static int heap_n;
static long combined_batches, combined_ops, min_violations;
static unsigned long extracted_sum[T];
static long extracted_count[T];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void heap_push(long v) {
    check(heap_n < HCAP, "heap capacity");
    int i = heap_n++;
    while (i > 0 && heap[(i - 1) / 2] > v) {
        heap[i] = heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap[i] = v;
}

static long heap_pop(void) {
    long top = heap[0];
    long v = heap[--heap_n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= heap_n)
            break;
        if (c + 1 < heap_n && heap[c + 1] < heap[c])
            c++;
        if (heap[c] >= v)
            break;
        heap[i] = heap[c];
        i = c;
    }
    if (heap_n > 0)
        heap[i] = v;
    return top;
}

static void combine(void) {
    combined_batches++;
    for (int i = 0; i < T; i++) {
        if (atomic_load_explicit(&slots[i].state, memory_order_acquire) != PENDING)
            continue;
        if (slots[i].op == OP_INSERT) {
            heap_push(slots[i].arg);
            slots[i].result = 0;
        } else {
            if (heap_n == 0) {
                slots[i].result = -1;
            } else {
                long m = heap_pop();
                slots[i].result = m;
            }
        }
        combined_ops++;
        atomic_store_explicit(&slots[i].state, DONE, memory_order_release);
    }
}

static long execute(int me, int op, long arg) {
    Slot *s = &slots[me];
    s->op = op;
    s->arg = arg;
    atomic_store_explicit(&s->state, PENDING, memory_order_release);
    for (;;) {
        if (atomic_load_explicit(&s->state, memory_order_acquire) == DONE)
            break;
        if (!atomic_flag_test_and_set_explicit(&combiner_lock, memory_order_acquire)) {
            combine();
            atomic_flag_clear_explicit(&combiner_lock, memory_order_release);
        } else {
            sched_yield();
        }
    }
    long r = s->result;
    atomic_store_explicit(&s->state, IDLE, memory_order_relaxed);
    return r;
}

static long value_of(int t, int i) {
    unsigned x = (unsigned)(t * 100003 + i) * 2654435761u;
    return (long)((x >> 12) % 100000u);
}

static void *worker(void *p) {
    int me = (int)(size_t)p;
    unsigned s = 0x1111u * (unsigned)(me + 1);
    unsigned long sum = 0;
    long n = 0;
    for (int i = 0; i < OPS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if (s % 3u != 0) {
            execute(me, OP_INSERT, value_of(me, i));
        } else {
            long r = execute(me, OP_EXTRACT, 0);
            if (r >= 0) {
                sum += (unsigned long)r;
                n++;
            }
        }
    }
    extracted_sum[me] = sum;
    extracted_count[me] = n;
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    /* each thread's insert/extract decisions are a pure function of its PRNG: replay them */
    unsigned long inserted_sum = 0;
    long inserted = 0;
    for (int t = 0; t < T; t++) {
        unsigned s = 0x1111u * (unsigned)(t + 1);
        for (int i = 0; i < OPS; i++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            if (s % 3u != 0) {
                inserted++;
                inserted_sum += (unsigned long)value_of(t, i);
            }
        }
    }
    unsigned long out_sum = 0;
    long out_n = 0;
    for (int t = 0; t < T; t++) {
        out_sum += extracted_sum[t];
        out_n += extracted_count[t];
    }
    long left = heap_n;
    long prev = -1;
    unsigned long left_sum = 0;
    while (heap_n > 0) {
        long v = heap_pop();
        if (v < prev)
            min_violations++;
        prev = v;
        left_sum += (unsigned long)v;
    }
    check(min_violations == 0, "heap drains in nondecreasing order");
    check(out_n + left == inserted, "count conserved");
    check(out_sum + left_sum == inserted_sum, "sum conserved");
    check(combined_ops == (long)T * OPS, "every request executed exactly once");
    check(combined_batches >= 1, "combiner ran");
    printf("requests executed once each=%ld, inserts=%ld\n", combined_ops, inserted);
    printf("sum inserted=%lu, extracted+left=%lu\n", inserted_sum, out_sum + left_sum);
    printf("heap drained in sorted order: yes\n");
    return 0;
}
