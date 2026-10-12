/*
 * title: Stable priority queue with wrapping sequence numbers
 * topic: data_structures
 * covers: stable priority queue, FIFO tie-break, sequence numbers, serial number arithmetic, 16-bit wraparound, instability demonstration
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x57AB1Eull;
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

typedef struct {
    int prio;
    uint16_t seq;
    int tag; /* arrival number, only used by the checker */
} Job;

/* serial number arithmetic (RFC 1982 style): a precedes b if b is within 32767 ahead of a */
static int seq_before(uint16_t a, uint16_t b) { return a != b && (uint16_t)(b - a) < 32768u; }

typedef struct {
    Job *h;
    int n, cap;
    int use_seq;
} PQ;

static int job_less(const PQ *q, Job a, Job b) {
    if (a.prio != b.prio)
        return a.prio < b.prio;
    return q->use_seq ? seq_before(a.seq, b.seq) : 0;
}

static void push(PQ *q, Job j) {
    check(q->n < q->cap, "capacity");
    int i = q->n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!job_less(q, j, q->h[p]))
            break;
        q->h[i] = q->h[p];
        i = p;
    }
    q->h[i] = j;
}

static Job pop(PQ *q) {
    Job top = q->h[0], x = q->h[--q->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= q->n)
            break;
        if (c + 1 < q->n && job_less(q, q->h[c + 1], q->h[c]))
            c++;
        if (!job_less(q, q->h[c], x))
            break;
        q->h[i] = q->h[c];
        i = c;
    }
    if (q->n > 0)
        q->h[i] = x;
    return top;
}

/* count adjacent equal-priority pairs served out of arrival order */
static int out_of_order(const Job *out, int n) {
    int bad = 0;
    for (int i = 1; i < n; i++)
        if (out[i].prio == out[i - 1].prio && out[i].tag < out[i - 1].tag)
            bad++;
    return bad;
}

int main(void) {
    enum { N = 300 };
    Job out[N], all[N];
    for (int i = 0; i < N; i++) {
        all[i].prio = (int)(rng() % 6);
        all[i].tag = i;
        all[i].seq = (uint16_t)i;
    }
    for (int mode = 0; mode < 2; mode++) {
        PQ q = {malloc(sizeof(Job) * N), 0, N, mode};
        for (int i = 0; i < N; i++)
            push(&q, all[i]);
        for (int i = 0; i < N; i++)
            out[i] = pop(&q);
        int bad = out_of_order(out, N);
        printf("%s: equal-priority pairs out of arrival order = %d\n", mode ? "with sequence" : "plain heap  ", bad);
        if (mode)
            check(bad == 0, "sequence numbers make the queue stable");
        else
            check(bad > 0, "plain heap is unstable on this input");
        for (int i = 1; i < N; i++)
            check(out[i - 1].prio <= out[i].prio, "priority order");
        free(q.h);
    }

    /* long run: sequence counter wraps many times while at most ~200 jobs are live */
    PQ q = {malloc(sizeof(Job) * 512), 0, 512, 1};
    uint16_t next_seq = 65000; /* start near the wrap point */
    int tag = 0, live = 0, wraps = 0, served = 0;
    int last_tag_of_prio[4] = {-1, -1, -1, -1};
    long checksum = 0;
    for (int step = 0; step < 200000; step++) {
        if ((live < 200 && rng() % 100 < 52) || live == 0) {
            Job j;
            j.prio = (int)(rng() % 4);
            j.seq = next_seq++;
            if (next_seq == 0)
                wraps++;
            j.tag = tag++;
            push(&q, j);
            live++;
        } else {
            Job j = pop(&q);
            live--;
            served++;
            /* within one priority, tags must be served in increasing order only among jobs
             * that were simultaneously queued; so track the minimum tag still waiting */
            int min_waiting = 1 << 30;
            if (served % 500 == 0) {
                for (int i = 0; i < q.n; i++)
                    if (q.h[i].prio == j.prio && q.h[i].tag < min_waiting)
                        min_waiting = q.h[i].tag;
                check(min_waiting > j.tag, "no earlier job of the same priority is still waiting");
            }
            last_tag_of_prio[j.prio] = j.tag;
            checksum += (long)j.prio * 7 + (j.tag % 13);
        }
    }
    printf("wraps=%d served=%d live=%d checksum=%ld\n", wraps, served, live, checksum);
    printf("last tags by priority: %d %d %d %d\n", last_tag_of_prio[0] % 100, last_tag_of_prio[1] % 100, last_tag_of_prio[2] % 100, last_tag_of_prio[3] % 100);
    free(q.h);
    return 0;
}
