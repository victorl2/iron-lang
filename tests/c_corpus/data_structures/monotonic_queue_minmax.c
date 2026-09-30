/*
 * title: Monotonic queue with O(1) min and max
 * topic: data_structures
 * covers: monotonic queue, min-max queue, amortized deque, push/pop by sequence number, brute force check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x9B05688C2B3E6C1FULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 27); }

#define CAP 4096
/* A ring of (seq, value); each helper deque stores candidates in monotone order. */
typedef struct { long seq[CAP]; int val[CAP]; int head, n; } Mono;
static void mono_push(Mono *d, long seq, int v, int keep_max) {
    while (d->n) {
        int last = d->val[(d->head + d->n - 1) % CAP];
        if (keep_max ? last <= v : last >= v) d->n--; else break;
    }
    CHECK(d->n < CAP);
    int at = (d->head + d->n) % CAP; d->seq[at] = seq; d->val[at] = v; d->n++;
}
static void mono_expire(Mono *d, long seq) { if (d->n && d->seq[d->head] == seq) { d->head = (d->head + 1) % CAP; d->n--; } }

typedef struct { int items[CAP]; int head, n; long first_seq, next_seq; Mono mx, mn; } MinMaxQueue;

static void q_push(MinMaxQueue *q, int v) {
    CHECK(q->n < CAP);
    q->items[(q->head + q->n) % CAP] = v; q->n++;
    mono_push(&q->mx, q->next_seq, v, 1);
    mono_push(&q->mn, q->next_seq, v, 0);
    q->next_seq++;
}
static int q_pop(MinMaxQueue *q) {
    CHECK(q->n);
    int v = q->items[q->head]; q->head = (q->head + 1) % CAP; q->n--;
    mono_expire(&q->mx, q->first_seq); mono_expire(&q->mn, q->first_seq);
    q->first_seq++;
    return v;
}
static int q_max(const MinMaxQueue *q) { CHECK(q->n); return q->mx.val[q->mx.head]; }
static int q_min(const MinMaxQueue *q) { CHECK(q->n); return q->mn.val[q->mn.head]; }

int main(void) {
    static MinMaxQueue q;
    memset(&q, 0, sizeof q);
    static int m[CAP * 2]; int mh = 0, mt = 0;
    long pushes = 0, pops = 0, spread_sum = 0;
    int widest = 0;
    long cand_max = 0, cand_min = 0;
    for (int step = 0; step < 40000; step++) {
        int phase = (step / 800) % 4;
        int pp = phase == 0 ? 70 : phase == 1 ? 30 : 50;
        if ((int)(rnd() % 100) < pp && q.n < 3000) {
            int v;
            switch (phase) { /* patterns: random, descending trend, ascending trend, random */
            case 1: v = 100000 - step % 800 * 100 + (int)(rnd() % 50); break;
            case 2: v = step % 800 * 100 + (int)(rnd() % 50); break;
            default: v = (int)(rnd() % 100000); break;
            }
            q_push(&q, v);
            if (mt == CAP * 2) { memmove(m, m + mh, (size_t)(mt - mh) * sizeof(int)); mt -= mh; mh = 0; }
            m[mt++] = v; pushes++;
        } else if (q.n) {
            CHECK(q_pop(&q) == m[mh++]); pops++;
        }
        CHECK(q.n == mt - mh);
        if (q.n) {
            int lo = m[mh], hi = m[mh];
            for (int i = mh; i < mt; i++) { if (m[i] < lo) lo = m[i]; if (m[i] > hi) hi = m[i]; }
            CHECK(q_min(&q) == lo && q_max(&q) == hi);
            spread_sum += hi - lo;
            if (hi - lo > widest) widest = hi - lo;
            cand_max += q.mx.n; cand_min += q.mn.n;
        }
    }
    printf("push=%ld pop=%ld final=%d\n", pushes, pops, q.n);
    printf("spread_sum=%ld widest=%d\n", spread_sum, widest);
    printf("avg candidates max=%ld min=%ld (x100)\n", cand_max * 100 / 40000, cand_min * 100 / 40000);
    return 0;
}
