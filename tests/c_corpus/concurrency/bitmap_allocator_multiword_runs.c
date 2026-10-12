/*
 * title: Multi-word bitmap allocator for contiguous runs with CAS
 * topic: concurrency
 * covers: run allocation inside a 64-bit word, CAS mask install, fetch_and free, first-fit fragmentation, ownership audit
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * 256 blocks live in four 64-bit words. A run of n <= 32 blocks never crosses a word boundary.
 * alloc_run scans each word for n adjacent zero bits (first fit) and installs the mask with
 * CAS(word, old, old | mask); a failed CAS reloads the word and rescans. free_run clears the
 * mask with fetch_and.
 *
 * Correctness argument: the CAS succeeds only if the word still equals the value in which the
 * run was found free, so two threads cannot install overlapping masks. The per-block owner
 * table audits this: a block must be unowned when claimed and owned by the caller when freed.
 */
enum { WORDS = 4, T = 5, ROUNDS = 2500, MAXHOLD = 6 };

static atomic_uint_least64_t bm[WORDS];
static atomic_int owner[WORDS * 64];
static atomic_int bad;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint64_t run_mask(int n) {
    return n == 64 ? UINT64_MAX : (((uint64_t)1 << n) - 1);
}

/* returns first block index of the run or -1 */
static int alloc_run(int n) {
    for (int w = 0; w < WORDS; w++) {
        uint64_t cur = atomic_load(&bm[w]);
        for (;;) {
            int pos = -1;
            for (int b = 0; b + n <= 64; b++)
                if (((cur >> b) & run_mask(n)) == 0) {
                    pos = b;
                    break;
                }
            if (pos < 0)
                break;
            uint64_t m = run_mask(n) << pos;
            if (atomic_compare_exchange_weak(&bm[w], &cur, cur | m))
                return w * 64 + pos;
            /* cur was refreshed by the failed CAS: rescan this word */
        }
    }
    return -1;
}

static void free_run(int start, int n) {
    atomic_fetch_and(&bm[start / 64], ~(run_mask(n) << (start % 64)));
}

typedef struct {
    int start, n;
} Run;

static void *worker(void *p) {
    int id = (int)(size_t)p + 1;
    unsigned s = 0x12345u * (unsigned)id;
    Run held[MAXHOLD];
    int nh = 0;
    for (int r = 0; r < ROUNDS; r++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if (nh < MAXHOLD && (s & 3u) != 0) {
            int n = 1 + (int)((s >> 4) % 12u);
            int at = alloc_run(n);
            if (at >= 0) {
                for (int i = 0; i < n; i++)
                    if (atomic_exchange(&owner[at + i], id) != 0)
                        atomic_fetch_add(&bad, 1);
                held[nh].start = at;
                held[nh].n = n;
                nh++;
            }
        } else if (nh > 0) {
            int k = (int)((s >> 9) % (unsigned)nh);
            Run x = held[k];
            held[k] = held[--nh];
            for (int i = 0; i < x.n; i++)
                if (atomic_exchange(&owner[x.start + i], 0) != id)
                    atomic_fetch_add(&bad, 1);
            free_run(x.start, x.n);
        }
        if ((s & 31u) == 0)
            sched_yield();
    }
    while (nh > 0) {
        Run x = held[--nh];
        for (int i = 0; i < x.n; i++)
            atomic_exchange(&owner[x.start + i], 0);
        free_run(x.start, x.n);
    }
    return NULL;
}

static void show_map(void) {
    for (int w = 0; w < WORDS; w++) {
        uint64_t v = atomic_load(&bm[w]);
        printf("word %d: ", w);
        for (int b = 0; b < 32; b++)
            putchar(((v >> b) & 1u) ? '#' : '.');
        printf("\n");
    }
}

int main(void) {
    /* Deterministic first-fit walk-through. */
    int a = alloc_run(10), b = alloc_run(20), c = alloc_run(2);
    check(a == 0 && b == 10 && c == 30, "first fit layout");
    free_run(b, 20);
    int d = alloc_run(5), e = alloc_run(16);
    check(d == 10 && e == 32, "hole reuse then skip to next fit");
    printf("runs at %d %d %d, then after freeing the 20-run: %d %d\n", a, b, c, d, e);
    /* a 16-run does not fit in the 15-block hole left at 15..29, so it lands at block 32 */
    show_map();
    free_run(a, 10);
    free_run(d, 5);
    free_run(c, 2);
    free_run(e, 16);
    for (int w = 0; w < WORDS; w++)
        check(atomic_load(&bm[w]) == 0, "clean after walk-through");
    /* a full word cannot serve a 33-run request; 32-runs fit exactly twice per word */
    int big[8], nb = 0;
    for (;;) {
        int r = alloc_run(32);
        if (r < 0)
            break;
        big[nb++] = r;
    }
    check(nb == 8, "eight 32-runs fit");
    printf("32-runs allocated: %d (starts %d %d ... %d)\n", nb, big[0], big[1], big[7]);
    for (int i = 0; i < nb; i++)
        free_run(big[i], 32);

    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&bad) == 0, "no overlapping runs");
    for (int w = 0; w < WORDS; w++)
        check(atomic_load(&bm[w]) == 0, "bitmap empty");
    printf("concurrent overlaps: %d, bitmap empty: yes\n", atomic_load(&bad));
    return 0;
}
