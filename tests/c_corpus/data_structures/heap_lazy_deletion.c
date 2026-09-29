/*
 * title: Lazy deletion heap with version stamps and compaction
 * topic: data_structures
 * covers: lazy deletion, tombstones, version stamps, update by re-insertion, threshold compaction, Floyd rebuild
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x1A2Bull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { M = 300 };

typedef struct {
    int prio, id;
    unsigned ver;
} Ent;

typedef struct {
    Ent *a;
    int n, cap;
    unsigned cur_ver[M]; /* current version of each id; 0 means absent */
    int live;
    long compactions, stale_skipped, max_size;
} LH;

static int less(Ent x, Ent y) { return x.prio < y.prio || (x.prio == y.prio && x.id < y.id); }

static void up(LH *h, int i) {
    Ent x = h->a[i];
    while (i > 0 && less(x, h->a[(i - 1) / 2])) {
        h->a[i] = h->a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h->a[i] = x;
}

static void down(LH *h, int i) {
    Ent x = h->a[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n)
            break;
        if (c + 1 < h->n && less(h->a[c + 1], h->a[c]))
            c++;
        if (!less(h->a[c], x))
            break;
        h->a[i] = h->a[c];
        i = c;
    }
    h->a[i] = x;
}

static int valid(const LH *h, Ent e) { return h->cur_ver[e.id] == e.ver; }

static void compact(LH *h) {
    int j = 0;
    for (int i = 0; i < h->n; i++)
        if (valid(h, h->a[i]))
            h->a[j++] = h->a[i];
    h->n = j;
    for (int i = h->n / 2 - 1; i >= 0; i--)
        down(h, i);
    h->compactions++;
}

static void maybe_compact(LH *h) {
    if (h->n > 32 && h->n > 2 * h->live + 8)
        compact(h);
}

static void push(LH *h, int id, int prio) {
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->a = realloc(h->a, sizeof(Ent) * (size_t)h->cap);
    }
    if (!h->cur_ver[id])
        h->live++;
    static unsigned stamp = 0;
    h->cur_ver[id] = ++stamp;
    h->a[h->n] = (Ent){prio, id, stamp};
    up(h, h->n++);
    if (h->n > h->max_size)
        h->max_size = h->n;
    maybe_compact(h);
}

static void remove_id(LH *h, int id) {
    check(h->cur_ver[id], "remove present id");
    h->cur_ver[id] = 0;
    h->live--;
    maybe_compact(h);
}

static Ent pop(LH *h) {
    for (;;) {
        Ent top = h->a[0];
        h->a[0] = h->a[--h->n];
        if (h->n > 0)
            down(h, 0);
        if (valid(h, top)) {
            h->cur_ver[top.id] = 0;
            h->live--;
            return top;
        }
        h->stale_skipped++;
    }
}

static void invariant(const LH *h) {
    for (int i = 1; i < h->n; i++)
        check(!less(h->a[i], h->a[(i - 1) / 2]), "heap order including stale entries");
    int valid_count = 0;
    for (int i = 0; i < h->n; i++)
        valid_count += valid(h, h->a[i]);
    check(valid_count == h->live, "exactly one valid entry per live id");
    check(h->n <= 2 * h->live + 9 + 1, "tombstones stay bounded");
}

int main(void) {
    LH h = {0};
    int mprio[M], mhas[M] = {0};
    int pushes = 0, updates = 0, removes = 0, pops = 0;
    long popsum = 0;
    for (int op = 0; op < 6000; op++) {
        int id = (int)(rng() % M), r = (int)(rng() % 100);
        if (!mhas[id]) {
            if (r < 60) {
                mprio[id] = (int)(rng() % 10000);
                mhas[id] = 1;
                push(&h, id, mprio[id]);
                pushes++;
            }
        } else if (r < 40) {
            mprio[id] = (int)(rng() % 10000);
            push(&h, id, mprio[id]);
            updates++;
        } else if (r < 65) {
            mhas[id] = 0;
            remove_id(&h, id);
            removes++;
        } else if (r < 80 && h.live > 0) {
            int bi = -1;
            for (int i = 0; i < M; i++)
                if (mhas[i] && (bi < 0 || mprio[i] < mprio[bi]))
                    bi = i;
            Ent e = pop(&h);
            check(e.id == bi && e.prio == mprio[bi], "pop equals model (ties by id)");
            mhas[e.id] = 0;
            popsum += e.prio;
            pops++;
        }
        if (h.n > 0 && op % 3 == 0)
            invariant(&h);
    }
    int live_model = 0;
    for (int i = 0; i < M; i++)
        live_model += mhas[i];
    check(live_model == h.live, "live count equals model");
    printf("push=%d update=%d remove=%d pop=%d live=%d\n", pushes, updates, removes, pops, h.live);
    printf("popsum=%ld compactions=%ld stale_skipped=%ld max_heap_size=%ld final_heap_size=%d\n", popsum, h.compactions, h.stale_skipped, h.max_size, h.n);
    free(h.a);
    return 0;
}
