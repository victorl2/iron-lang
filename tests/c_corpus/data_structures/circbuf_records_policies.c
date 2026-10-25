/*
 * title: Circular buffer of records with full-buffer policies and two-segment views
 * topic: data_structures
 * covers: circular buffer, head/count representation, reject and overwrite policies, bulk push and pop across the wrap, contiguous segment views, indexed access, drop counters, shifting-array oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP 13

static unsigned long long rs = 0xC12CB0FULL * 0x9E37ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int id; short kind; unsigned char flags; long payload; } Rec;
typedef enum { REJECT_NEW, OVERWRITE_OLDEST } Policy;
typedef struct { Rec buf[CAP]; int head, count; Policy pol; long dropped_new, overwritten, pushed, popped; } Ring;

static void rinit(Ring *r, Policy p) { memset(r, 0, sizeof *r); r->pol = p; }
static int idx(const Ring *r, int i) { return (r->head + i) % CAP; }
static int push(Ring *r, Rec x) {
    if (r->count == CAP) {
        if (r->pol == REJECT_NEW) { r->dropped_new++; return 0; }
        r->head = (r->head + 1) % CAP; r->count--; r->overwritten++;
    }
    r->buf[idx(r, r->count)] = x; r->count++; r->pushed++;
    return 1;
}
static int pop(Ring *r, Rec *out) {
    if (!r->count) return 0;
    *out = r->buf[r->head]; r->head = (r->head + 1) % CAP; r->count--; r->popped++;
    return 1;
}
static const Rec *at(const Ring *r, int i) { return i >= 0 && i < r->count ? &r->buf[idx(r, i)] : NULL; }
/* bulk push: copies with at most two memcpy calls; returns number accepted */
static int push_bulk(Ring *r, const Rec *src, int n) {
    int accepted = 0;
    if (r->pol == REJECT_NEW) {
        int room = CAP - r->count; int take = n < room ? n : room;
        r->dropped_new += n - take; n = take; accepted = take;
    } else {
        if (n > CAP) { r->overwritten += n - CAP; r->pushed += n - CAP; src += n - CAP; n = CAP; }   /* only the newest CAP records can survive */
        int overflow = r->count + n - CAP;
        if (overflow > 0) { r->head = (r->head + overflow) % CAP; r->count -= overflow; r->overwritten += overflow; }
        accepted = n;
    }
    int tail = idx(r, r->count), first = CAP - tail < n ? CAP - tail : n;
    memcpy(&r->buf[tail], src, (size_t)first * sizeof(Rec));
    if (n > first) memcpy(&r->buf[0], src + first, (size_t)(n - first) * sizeof(Rec));
    r->count += n; r->pushed += n;
    return accepted;
}
/* contiguous readable segments: [seg1, seg2] */
static void segments(const Ring *r, const Rec **s1, int *n1, const Rec **s2, int *n2) {
    int first = CAP - r->head < r->count ? CAP - r->head : r->count;
    *s1 = &r->buf[r->head]; *n1 = first; *s2 = &r->buf[0]; *n2 = r->count - first;
}

/* oracle: shifting array */
typedef struct { Rec a[4 * CAP]; int n; Policy pol; long dropped_new, overwritten; } Ref;
static void rpush(Ref *o, Rec x) {
    if (o->n == CAP) {
        if (o->pol == REJECT_NEW) { o->dropped_new++; return; }
        memmove(&o->a[0], &o->a[1], (size_t)(o->n - 1) * sizeof(Rec)); o->n--; o->overwritten++;
    }
    o->a[o->n++] = x;
}
static int same(const Ring *r, const Ref *o) {
    if (r->count != o->n) return 0;
    for (int i = 0; i < o->n; i++) { const Rec *x = at(r, i), *y = &o->a[i]; if (x->id != y->id || x->payload != y->payload || x->kind != y->kind) return 0; }
    return 1;
}

int main(void) {
    for (int pi = 0; pi < 2; pi++) {
        Ring r; Ref o; memset(&o, 0, sizeof o);
        rinit(&r, (Policy)pi); o.pol = (Policy)pi;
        int next_id = 1; long popped_sum = 0, seg_wraps = 0;
        for (int step = 0; step < 20000; step++) {
            unsigned op = rnd() % 10;
            if (op < 4) {
                Rec x; x.id = next_id++; x.kind = (short)(rnd() % 5); x.flags = (unsigned char)(rnd() & 0xFF); x.payload = (long)rnd() * 3;
                int ok = push(&r, x); (void)ok;
                rpush(&o, x);
            } else if (op < 6) {
                int n = 1 + (int)(rnd() % 20);
                Rec batch[24];
                for (int i = 0; i < n; i++) { batch[i].id = next_id++; batch[i].kind = (short)(rnd() % 5); batch[i].flags = 1; batch[i].payload = (long)i; }
                push_bulk(&r, batch, n);
                for (int i = 0; i < n; i++) rpush(&o, batch[i]);
            } else if (op < 9) {
                Rec x = {0, 0, 0, 0}, y; int a = pop(&r, &x), b = o.n > 0;
                check(a == b, "pop presence");
                if (b) { y = o.a[0]; memmove(&o.a[0], &o.a[1], (size_t)(o.n - 1) * sizeof(Rec)); o.n--; check(x.id == y.id && x.payload == y.payload, "popped record"); popped_sum += x.kind; }
            } else {
                const Rec *s1, *s2; int n1, n2; segments(&r, &s1, &n1, &s2, &n2);
                check(n1 + n2 == r.count, "segments cover contents");
                for (int i = 0; i < n1; i++) check(s1[i].id == o.a[i].id, "segment 1");
                for (int i = 0; i < n2; i++) check(s2[i].id == o.a[n1 + i].id, "segment 2");
                seg_wraps += n2 > 0;
            }
            check(same(&r, &o), "contents match oracle");
            check(r.dropped_new == o.dropped_new && r.overwritten == o.overwritten, "drop counters match oracle");
        }
        /* order property: ids strictly increasing from oldest to newest */
        for (int i = 1; i < r.count; i++) check(at(&r, i - 1)->id < at(&r, i)->id, "ids increase");
        printf("%-16s pushed=%ld popped=%ld dropped_new=%ld overwritten=%ld count=%d head=%d wrapped_views=%ld kind_sum=%ld\n",
               pi == REJECT_NEW ? "reject-new" : "overwrite-oldest", r.pushed, r.popped, r.dropped_new, r.overwritten, r.count, r.head, seg_wraps, popped_sum);
        check(r.pushed - r.popped - r.overwritten == r.count, "conservation of records");
    }
    return 0;
}
