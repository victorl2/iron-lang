/*
 * title: Hierarchical timing wheel cross-checked against a heap scheduler
 * topic: data_structures
 * covers: hierarchical timing wheel, cascading, timer cancellation, binary heap scheduler, per-tick firing order, doubly linked slots
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x7157ull;
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

enum { LEVELS = 4, BITS = 4, SLOTS = 1 << BITS, MAXT = 20000, RANGE = 1 << (LEVELS * BITS) };

typedef struct {
    int id;
    unsigned expiry;
    int next, prev; /* intrusive list links, -1 terminated */
    int level, slot; /* where it lives, -1 if not in the wheel */
} Timer;

typedef struct {
    Timer t[MAXT];
    int head[LEVELS][SLOTS];
    unsigned now;
    long cascades, moved, inserts_by_level[LEVELS];
} Wheel;

static void w_init(Wheel *w) {
    for (int l = 0; l < LEVELS; l++)
        for (int s = 0; s < SLOTS; s++)
            w->head[l][s] = -1;
    w->now = 0;
    w->cascades = w->moved = 0;
    memset(w->inserts_by_level, 0, sizeof w->inserts_by_level);
    for (int i = 0; i < MAXT; i++)
        w->t[i].level = -1;
}

static void w_place(Wheel *w, int i) {
    Timer *t = &w->t[i];
    unsigned delta = t->expiry - w->now;
    int l = 0;
    while (l < LEVELS - 1 && delta >= (1u << (BITS * (l + 1))))
        l++;
    int s = (int)((t->expiry >> (BITS * l)) & (SLOTS - 1));
    t->level = l;
    t->slot = s;
    t->prev = -1;
    t->next = w->head[l][s];
    if (t->next >= 0)
        w->t[t->next].prev = i;
    w->head[l][s] = i;
}

static void w_unlink(Wheel *w, int i) {
    Timer *t = &w->t[i];
    if (t->prev >= 0)
        w->t[t->prev].next = t->next;
    else
        w->head[t->level][t->slot] = t->next;
    if (t->next >= 0)
        w->t[t->next].prev = t->prev;
    t->level = -1;
}

static void w_add(Wheel *w, int i, unsigned expiry) {
    w->t[i].id = i;
    w->t[i].expiry = expiry;
    w_place(w, i);
    w->inserts_by_level[w->t[i].level]++;
}

static void w_cancel(Wheel *w, int i) {
    check(w->t[i].level >= 0, "cancel a live timer");
    w_unlink(w, i);
}

/* advance one tick; ids of fired timers are appended to out (unsorted) */
static int w_tick(Wheel *w, int *out) {
    w->now++;
    for (int l = 1; l < LEVELS; l++) {
        if ((w->now & ((1u << (BITS * l)) - 1)) != 0)
            break;
        int s = (int)((w->now >> (BITS * l)) & (SLOTS - 1));
        int i = w->head[l][s];
        w->head[l][s] = -1;
        w->cascades++;
        while (i >= 0) {
            int nx = w->t[i].next;
            w->t[i].level = -1;
            w_place(w, i);
            w->moved++;
            i = nx;
        }
    }
    int n = 0, s0 = (int)(w->now & (SLOTS - 1));
    int i = w->head[0][s0];
    w->head[0][s0] = -1;
    while (i >= 0) {
        int nx = w->t[i].next;
        check(w->t[i].expiry == w->now, "fires exactly on its expiry tick");
        w->t[i].level = -1;
        out[n++] = i;
        i = nx;
    }
    return n;
}

/* heap scheduler oracle: min-heap on (expiry, id) with lazy cancellation */
typedef struct {
    unsigned expiry;
    int id;
} HE;
typedef struct {
    HE h[MAXT + 1];
    int n;
    unsigned char cancelled[MAXT];
} Heap;

static int hless(HE a, HE b) { return a.expiry < b.expiry || (a.expiry == b.expiry && a.id < b.id); }

static void h_push(Heap *h, HE e) {
    int i = h->n++;
    while (i > 0 && hless(e, h->h[(i - 1) / 2])) {
        h->h[i] = h->h[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h->h[i] = e;
}

static HE h_pop(Heap *h) {
    HE top = h->h[0], x = h->h[--h->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n)
            break;
        if (c + 1 < h->n && hless(h->h[c + 1], h->h[c]))
            c++;
        if (!hless(h->h[c], x))
            break;
        h->h[i] = h->h[c];
        i = c;
    }
    if (h->n > 0)
        h->h[i] = x;
    return top;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    static Wheel w;
    static Heap hp;
    w_init(&w);
    int next_id = 0, live = 0, cancels = 0;
    long fired = 0, checksum = 0;
    int max_per_tick = 0, busiest = 0;
    unsigned schedule_kind[3] = {SLOTS - 1, 300, 20000};
    int alive[MAXT] = {0};
    unsigned exp_of[MAXT];
    for (unsigned tick = 1; tick <= 40000; tick++) {
        /* schedule a few timers with short, medium and long delays */
        int k = rng() % 10 < 3 ? 1 + (int)(rng() % 2) : 0;
        for (int j = 0; j < k && next_id < MAXT; j++) {
            unsigned span = schedule_kind[rng() % 3];
            unsigned delta = 1 + rng() % span;
            unsigned expiry = w.now + delta;
            w_add(&w, next_id, expiry);
            h_push(&hp, (HE){expiry, next_id});
            alive[next_id] = 1;
            exp_of[next_id] = expiry;
            next_id++;
            live++;
        }
        /* cancel a random live timer now and then */
        if (rng() % 5 == 0 && next_id > 0) {
            int id = (int)(rng() % (unsigned)next_id);
            if (alive[id] && exp_of[id] > w.now) {
                w_cancel(&w, id);
                hp.cancelled[id] = 1;
                alive[id] = 0;
                live--;
                cancels++;
            }
        }
        int out[MAXT];
        int n = w_tick(&w, out);
        qsort(out, (size_t)n, sizeof(int), cmp_int);
        /* oracle: pop everything due at this tick */
        int m = 0, expect[MAXT];
        while (hp.n > 0 && hp.h[0].expiry <= w.now) {
            HE e = h_pop(&hp);
            check(e.expiry == w.now || hp.cancelled[e.id], "heap never lags behind the clock");
            if (!hp.cancelled[e.id])
                expect[m++] = e.id;
        }
        check(n == m, "same number of timers fired");
        for (int i = 0; i < n; i++) {
            check(out[i] == expect[i], "same timers fired, same order");
            alive[out[i]] = 0;
            checksum += (long)out[i] * (long)(w.now % 97);
        }
        live -= n;
        fired += n;
        if (n > max_per_tick) {
            max_per_tick = n;
            busiest = (int)w.now;
        }
    }
    /* drain what is still scheduled */
    long remaining = 0;
    for (int i = 0; i < MAXT; i++)
        remaining += alive[i];
    check(remaining == live, "live count");
    printf("scheduled=%d fired=%ld cancelled=%d pending=%ld\n", next_id, fired, cancels, remaining);
    printf("cascades=%ld moved=%ld checksum=%ld\n", w.cascades, w.moved, checksum);
    printf("inserts by level: %ld %ld %ld %ld\n", w.inserts_by_level[0], w.inserts_by_level[1], w.inserts_by_level[2], w.inserts_by_level[3]);
    printf("busiest tick=%d with %d timers\n", busiest, max_per_tick);
    return 0;
}
