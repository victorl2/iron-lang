/*
 * title: Indexed priority queue with change-key and remove
 * topic: data_structures
 * covers: indexed heap, position map, change key up and down, arbitrary removal, contains, handle validation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 13579;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 31);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* items are small integer ids 0..M-1; heap[] holds ids, pos[id] is the heap slot or -1 */
enum { M = 200 };
typedef struct {
    int heap[M], pos[M], key[M], n;
    long swaps;
} IPQ;

static void ipq_init(IPQ *q) {
    q->n = 0;
    q->swaps = 0;
    for (int i = 0; i < M; i++)
        q->pos[i] = -1;
}

static int ipq_contains(const IPQ *q, int id) { return q->pos[id] >= 0; }

static void place(IPQ *q, int slot, int id) {
    q->heap[slot] = id;
    q->pos[id] = slot;
}

static void up(IPQ *q, int s) {
    int id = q->heap[s];
    while (s > 0) {
        int p = (s - 1) / 2;
        if (q->key[q->heap[p]] <= q->key[id])
            break;
        place(q, s, q->heap[p]);
        q->swaps++;
        s = p;
    }
    place(q, s, id);
}

static void down(IPQ *q, int s) {
    int id = q->heap[s];
    for (;;) {
        int c = 2 * s + 1;
        if (c >= q->n)
            break;
        if (c + 1 < q->n && q->key[q->heap[c + 1]] < q->key[q->heap[c]])
            c++;
        if (q->key[q->heap[c]] >= q->key[id])
            break;
        place(q, s, q->heap[c]);
        q->swaps++;
        s = c;
    }
    place(q, s, id);
}

static void ipq_insert(IPQ *q, int id, int key) {
    check(!ipq_contains(q, id), "insert: id absent");
    q->key[id] = key;
    place(q, q->n, id);
    q->n++;
    up(q, q->n - 1);
}

static void ipq_change(IPQ *q, int id, int key) {
    check(ipq_contains(q, id), "change: id present");
    int old = q->key[id];
    q->key[id] = key;
    if (key < old)
        up(q, q->pos[id]);
    else if (key > old)
        down(q, q->pos[id]);
}

static void ipq_remove(IPQ *q, int id) {
    check(ipq_contains(q, id), "remove: id present");
    int s = q->pos[id];
    q->pos[id] = -1;
    q->n--;
    if (s == q->n)
        return;
    int last = q->heap[q->n];
    place(q, s, last);
    up(q, s);
    down(q, q->pos[last]);
}

static int ipq_pop(IPQ *q) {
    int id = q->heap[0];
    ipq_remove(q, id);
    return id;
}

static void invariant(const IPQ *q) {
    int present = 0;
    for (int i = 0; i < M; i++)
        if (q->pos[i] >= 0) {
            present++;
            check(q->pos[i] < q->n && q->heap[q->pos[i]] == i, "pos and heap agree");
        }
    check(present == q->n, "present count");
    for (int s = 1; s < q->n; s++)
        check(q->key[q->heap[(s - 1) / 2]] <= q->key[q->heap[s]], "heap order");
}

int main(void) {
    IPQ q;
    ipq_init(&q);
    int mkey[M], mhas[M] = {0};
    int ins = 0, chg = 0, rem = 0, pops = 0;
    long popkeysum = 0;
    unsigned long long trace = 1469598103934665603ull;
    for (int op = 0; op < 20000; op++) {
        int id = (int)(rng() % M), r = (int)(rng() % 100);
        if (!mhas[id]) {
            if (r < 70) {
                int k = (int)(rng() % 1000);
                mkey[id] = k;
                mhas[id] = 1;
                ipq_insert(&q, id, k);
                ins++;
            }
        } else if (r < 45) {
            int k = (int)(rng() % 1000);
            mkey[id] = k;
            ipq_change(&q, id, k);
            chg++;
        } else if (r < 60) {
            mhas[id] = 0;
            ipq_remove(&q, id);
            rem++;
        } else if (r < 75 && q.n > 0) {
            int mk = 1 << 30;
            for (int i = 0; i < M; i++)
                if (mhas[i] && mkey[i] < mk)
                    mk = mkey[i];
            int top = ipq_pop(&q);
            check(mhas[top] && mkey[top] == mk, "pop is a minimum-key id");
            mhas[top] = 0;
            popkeysum += mk;
            pops++;
            trace = (trace ^ (unsigned long long)mk) * 1099511628211ull;
        }
        check(ipq_contains(&q, id) == mhas[id], "contains agrees with model");
        invariant(&q);
    }
    printf("insert=%d change=%d remove=%d pop=%d size=%d\n", ins, chg, rem, pops, q.n);
    printf("popkeysum=%ld swaps=%ld trace=%llu\n", popkeysum, q.swaps, trace);
    return 0;
}
