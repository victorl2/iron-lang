/*
 * title: Double-ended priority queue from twin heaps with cross pointers
 * topic: data_structures
 * covers: double-ended priority queue, min-heap and max-heap pair, cross-linked positions, element pool with free list
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x7817ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { CAP = 512 };

/* each element lives in a pool slot; both heaps store slot numbers and every slot remembers its two heap positions */
typedef struct {
    int key;
    int pos_min, pos_max;
    int next_free;
} Slot;

typedef struct {
    Slot s[CAP];
    int minh[CAP], maxh[CAP];
    int n, free_head;
    long moves;
} Twin;

static void twin_init(Twin *t) {
    t->n = 0;
    t->moves = 0;
    t->free_head = 0;
    for (int i = 0; i < CAP; i++)
        t->s[i].next_free = i + 1 < CAP ? i + 1 : -1;
}

static int key_at_min(const Twin *t, int i) { return t->s[t->minh[i]].key; }
static int key_at_max(const Twin *t, int i) { return t->s[t->maxh[i]].key; }

static void set_min(Twin *t, int i, int slot) {
    t->minh[i] = slot;
    t->s[slot].pos_min = i;
    t->moves++;
}

static void set_max(Twin *t, int i, int slot) {
    t->maxh[i] = slot;
    t->s[slot].pos_max = i;
    t->moves++;
}

static void min_up(Twin *t, int i) {
    int slot = t->minh[i], k = t->s[slot].key;
    while (i > 0 && k < key_at_min(t, (i - 1) / 2)) {
        set_min(t, i, t->minh[(i - 1) / 2]);
        i = (i - 1) / 2;
    }
    set_min(t, i, slot);
}

static void min_down(Twin *t, int i) {
    int slot = t->minh[i], k = t->s[slot].key;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= t->n)
            break;
        if (c + 1 < t->n && key_at_min(t, c + 1) < key_at_min(t, c))
            c++;
        if (key_at_min(t, c) >= k)
            break;
        set_min(t, i, t->minh[c]);
        i = c;
    }
    set_min(t, i, slot);
}

static void max_up(Twin *t, int i) {
    int slot = t->maxh[i], k = t->s[slot].key;
    while (i > 0 && k > key_at_max(t, (i - 1) / 2)) {
        set_max(t, i, t->maxh[(i - 1) / 2]);
        i = (i - 1) / 2;
    }
    set_max(t, i, slot);
}

static void max_down(Twin *t, int i) {
    int slot = t->maxh[i], k = t->s[slot].key;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= t->n)
            break;
        if (c + 1 < t->n && key_at_max(t, c + 1) > key_at_max(t, c))
            c++;
        if (key_at_max(t, c) <= k)
            break;
        set_max(t, i, t->maxh[c]);
        i = c;
    }
    set_max(t, i, slot);
}

static void insert(Twin *t, int key) {
    check(t->free_head >= 0, "pool not exhausted");
    int slot = t->free_head;
    t->free_head = t->s[slot].next_free;
    t->s[slot].key = key;
    t->minh[t->n] = t->maxh[t->n] = slot;
    t->s[slot].pos_min = t->s[slot].pos_max = t->n;
    t->n++;
    min_up(t, t->n - 1);
    max_up(t, t->n - 1);
}

/* remove the element in pool slot `slot` from both heaps */
static void remove_slot(Twin *t, int slot) {
    int pm = t->s[slot].pos_min, px = t->s[slot].pos_max;
    int last = t->n - 1;
    /* min heap */
    if (pm != last) {
        int moved = t->minh[last];
        set_min(t, pm, moved);
        t->n--; /* temporarily shrink so sifts see the new size */
        min_up(t, pm);
        min_down(t, t->s[moved].pos_min);
        t->n++;
    }
    /* max heap */
    if (px != last) {
        int moved = t->maxh[last];
        set_max(t, px, moved);
        t->n--;
        max_up(t, px);
        max_down(t, t->s[moved].pos_max);
        t->n++;
    }
    t->n--;
    t->s[slot].next_free = t->free_head;
    t->free_head = slot;
}

static int pop_min(Twin *t) {
    int slot = t->minh[0], k = t->s[slot].key;
    remove_slot(t, slot);
    return k;
}

static int pop_max(Twin *t) {
    int slot = t->maxh[0], k = t->s[slot].key;
    remove_slot(t, slot);
    return k;
}

static void invariant(const Twin *t) {
    for (int i = 0; i < t->n; i++) {
        check(t->s[t->minh[i]].pos_min == i, "min position back-pointer");
        check(t->s[t->maxh[i]].pos_max == i, "max position back-pointer");
        if (i > 0) {
            check(key_at_min(t, (i - 1) / 2) <= key_at_min(t, i), "min heap order");
            check(key_at_max(t, (i - 1) / 2) >= key_at_max(t, i), "max heap order");
        }
    }
    /* both heaps hold the same set of slots */
    int mark[CAP] = {0};
    for (int i = 0; i < t->n; i++)
        mark[t->minh[i]]++;
    for (int i = 0; i < t->n; i++)
        mark[t->maxh[i]]--;
    for (int i = 0; i < CAP; i++)
        check(mark[i] == 0, "same slot sets");
}

int main(void) {
    static Twin t;
    twin_init(&t);
    int model[CAP], mn = 0, pmin = 0, pmax = 0;
    long smin = 0, smax = 0;
    for (int op = 0; op < 9000; op++) {
        int r = (int)(rng() % 100);
        if ((r < 52 || mn == 0) && mn < CAP - 1) {
            int v = (int)(rng() % 300);
            model[mn++] = v;
            insert(&t, v);
        } else {
            int bi = 0, bx = 0;
            for (int i = 1; i < mn; i++) {
                if (model[i] < model[bi])
                    bi = i;
                if (model[i] > model[bx])
                    bx = i;
            }
            if (r < 76) {
                int got = pop_min(&t);
                check(got == model[bi], "pop_min");
                model[bi] = model[--mn];
                smin += got;
                pmin++;
            } else {
                int got = pop_max(&t);
                check(got == model[bx], "pop_max");
                model[bx] = model[--mn];
                smax += got;
                pmax++;
            }
        }
        check(t.n == mn, "size");
        invariant(&t);
    }
    printf("size=%d pop_min=%d pop_max=%d sum_min=%ld sum_max=%ld moves=%ld\n", t.n, pmin, pmax, smin, smax, t.moves);
    return 0;
}
