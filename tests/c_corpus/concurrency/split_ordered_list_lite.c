/*
 * title: Split-ordered list hash set with lazily initialized bucket sentinels
 * topic: concurrency
 * covers: bit-reversed keys, sorted lock-free list, dummy nodes, recursive bucket init, table doubling by CAS, insert-only
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * All keys live in ONE sorted lock-free list ordered by bit-reversed key. A regular key k sorts
 * by reverse(k | 2^31) (low bit 1 after reversal); the sentinel for bucket b sorts by reverse(b)
 * (low bit 0). Bucket b's chain therefore starts at its sentinel and runs until the next
 * bucket's sentinel. Doubling the table size only adds sentinels: no key ever moves.
 *
 * Sentinel for bucket b is inserted starting from the sentinel of its parent bucket (b with its
 * top set bit cleared), initializing the parent first if needed.
 *
 * Correctness argument: list insertion is the usual sorted CAS at the predecessor. The set is
 * insert-only, so nodes never vanish and re-scanning from the predecessor after a failed CAS
 * is safe. A duplicate has the same split-order key, so the second inserter finds it.
 */
enum { MAXB = 1024, POOL = 20000, T = 4, PER = 2500, NIL = 0 };

typedef struct {
    uint32_t so;
    uint32_t key;
    atomic_uint next;
} Node;

static Node pool[POOL];
static atomic_uint pool_top = 2; /* node 0 = nil, node 1 = sentinel of bucket 0 */
static atomic_uint bucket[MAXB];
static atomic_uint table_size = 4;
static atomic_uint item_count;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint32_t reverse32(uint32_t x) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (x & 1u);
        x >>= 1;
    }
    return r;
}
static uint32_t so_regular(uint32_t k) { return reverse32(k | 0x80000000u); }
static uint32_t so_dummy(uint32_t b) { return reverse32(b); }

static unsigned new_node(uint32_t so, uint32_t key) {
    unsigned n = atomic_fetch_add(&pool_top, 1u);
    check(n < POOL, "node pool");
    pool[n].so = so;
    pool[n].key = key;
    atomic_store(&pool[n].next, NIL);
    return n;
}

/* insert node n (with pool[n].so) into the sorted list starting after 'start'.
 * returns n on success or the existing node with the same so-key. */
static unsigned list_insert(unsigned start, unsigned n) {
    unsigned pred = start;
    for (;;) {
        unsigned curr = atomic_load(&pool[pred].next);
        while (curr != NIL && pool[curr].so < pool[n].so) {
            pred = curr;
            curr = atomic_load(&pool[pred].next);
        }
        if (curr != NIL && pool[curr].so == pool[n].so)
            return curr;
        atomic_store(&pool[n].next, curr);
        if (atomic_compare_exchange_strong(&pool[pred].next, &curr, n))
            return n;
        /* someone inserted after pred: retry from pred */
    }
}

static unsigned get_bucket(uint32_t b) {
    unsigned d = atomic_load(&bucket[b]);
    if (d != 0)
        return d;
    uint32_t parent = b;
    for (uint32_t bit = 0x80000000u; bit; bit >>= 1)
        if (parent & bit) {
            parent &= ~bit;
            break;
        }
    unsigned pd = get_bucket(parent);
    unsigned n = new_node(so_dummy(b), b);
    unsigned got = list_insert(pd, n);
    unsigned zero = 0;
    atomic_compare_exchange_strong(&bucket[b], &zero, got);
    return atomic_load(&bucket[b]);
}

static int insert(uint32_t key) {
    unsigned sz = atomic_load(&table_size);
    unsigned start = get_bucket(key & (sz - 1));
    unsigned n = new_node(so_regular(key), key);
    unsigned got = list_insert(start, n);
    if (got != n)
        return 0;
    unsigned cnt = atomic_fetch_add(&item_count, 1u) + 1;
    if (cnt / sz > 2 && sz < MAXB) {
        unsigned expect = sz;
        atomic_compare_exchange_strong(&table_size, &expect, sz * 2);
    }
    return 1;
}

static int contains(uint32_t key) {
    unsigned sz = atomic_load(&table_size);
    unsigned curr = get_bucket(key & (sz - 1));
    uint32_t so = so_regular(key);
    while (curr != NIL && pool[curr].so < so)
        curr = atomic_load(&pool[curr].next);
    return curr != NIL && pool[curr].so == so;
}

static uint32_t key_of(int t, int i) {
    uint32_t x = (uint32_t)(t * 7 + i * 13 + 5) * 2654435761u;
    x ^= x >> 15;
    return (x % 6000u) + 1u; /* many duplicates across threads */
}

static atomic_int inserted_total;

static void *worker(void *p) {
    int t = (int)(size_t)p;
    int ins = 0;
    for (int i = 0; i < PER; i++) {
        if (insert(key_of(t % 2, i + (t / 2) * 700)))
            ins++;
        (void)contains(key_of(t, i));
    }
    atomic_fetch_add(&inserted_total, ins);
    return NULL;
}

int main(void) {
    atomic_store(&bucket[0], 1);
    pool[1].so = 0;
    pool[1].key = 0;
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    static unsigned char ref[6002];
    int distinct = 0;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < PER; i++) {
            uint32_t k = key_of(t % 2, i + (t / 2) * 700);
            if (!ref[k]) {
                ref[k] = 1;
                distinct++;
            }
        }
    /* walk the whole list: strictly increasing so-keys, regular keys match the reference */
    int regulars = 0, dummies = 0;
    uint32_t prev = 0;
    int first = 1;
    unsigned char seen[6002] = {0};
    for (unsigned n = 1; n != NIL; n = atomic_load(&pool[n].next)) {
        if (!first)
            check(pool[n].so > prev, "list sorted by split order");
        first = 0;
        prev = pool[n].so;
        if (pool[n].so & 1u) {
            regulars++;
            check(pool[n].key >= 1 && pool[n].key <= 6001, "key range");
            check(!seen[pool[n].key], "no duplicate keys");
            seen[pool[n].key] = 1;
            check(ref[pool[n].key], "key was inserted");
        } else {
            dummies++;
        }
    }
    check(regulars == distinct, "distinct count");
    check((int)atomic_load(&inserted_total) == distinct, "insert successes equal distinct");
    for (uint32_t k = 1; k <= 6001; k++)
        check(contains(k) == (ref[k] != 0), "contains matches reference");
    unsigned sz = atomic_load(&table_size);
    check(dummies >= 1 && (unsigned)dummies <= sz, "sentinels bounded by table size");
    printf("distinct keys=%d table size power of two: %s\n", distinct, (sz & (sz - 1)) == 0 ? "yes" : "no");
    printf("list sorted by bit-reversed key: yes\n");
    check(sz > 4, "table grew");
    printf("table grew beyond its initial 4 buckets: yes\n");
    printf("sentinel for bucket 0 is the list head: %s\n", pool[1].so == 0 ? "yes" : "no");
    return 0;
}
