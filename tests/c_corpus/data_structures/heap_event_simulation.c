/*
 * title: Discrete-event queue simulation on a future-event heap
 * topic: data_structures
 * covers: future event list, event ordering with tie rules, multi-server FIFO queue, ring buffer queue, independent recurrence oracle
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0xE7E27ull;
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

enum { JOBS = 3000, MAXC = 4 };

typedef enum { EV_DEPARTURE = 0, EV_ARRIVAL = 1 } EvType;

typedef struct {
    long time;
    EvType type;
    int job;
} Event;

/* order: time, then departures before arrivals, then job number */
static int ev_less(Event a, Event b) {
    if (a.time != b.time)
        return a.time < b.time;
    if (a.type != b.type)
        return a.type < b.type;
    return a.job < b.job;
}

typedef struct {
    Event *a;
    int n, cap, peak;
} FEL;

static void fel_push(FEL *f, Event e) {
    if (f->n == f->cap) {
        f->cap = f->cap ? 2 * f->cap : 32;
        f->a = realloc(f->a, sizeof(Event) * (size_t)f->cap);
    }
    int i = f->n++;
    while (i > 0 && ev_less(e, f->a[(i - 1) / 2])) {
        f->a[i] = f->a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    f->a[i] = e;
    if (f->n > f->peak)
        f->peak = f->n;
}

static Event fel_pop(FEL *f) {
    Event top = f->a[0], x = f->a[--f->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= f->n)
            break;
        if (c + 1 < f->n && ev_less(f->a[c + 1], f->a[c]))
            c++;
        if (!ev_less(f->a[c], x))
            break;
        f->a[i] = f->a[c];
        i = c;
    }
    if (f->n > 0)
        f->a[i] = x;
    return top;
}

static long arrival[JOBS], service[JOBS];

typedef struct {
    long total_wait, max_wait, makespan;
    int max_queue;
    long peak_fel;
    unsigned long long hash;
    long wait[JOBS];
} Result;

static void simulate(int servers, Result *r) {
    FEL f = {0};
    static int queue[JOBS];
    int qh = 0, qt = 0, qn = 0, busy = 0;
    r->total_wait = r->max_wait = r->makespan = 0;
    r->max_queue = 0;
    r->hash = 1469598103934665603ull;
    /* arrivals are scheduled one at a time, as a generator would */
    fel_push(&f, (Event){arrival[0], EV_ARRIVAL, 0});
    while (f.n > 0) {
        Event e = fel_pop(&f);
        if (e.type == EV_ARRIVAL) {
            if (e.job + 1 < JOBS)
                fel_push(&f, (Event){arrival[e.job + 1], EV_ARRIVAL, e.job + 1});
            if (busy < servers) {
                busy++;
                r->wait[e.job] = 0;
                fel_push(&f, (Event){e.time + service[e.job], EV_DEPARTURE, e.job});
            } else {
                queue[qt] = e.job;
                qt = (qt + 1) % JOBS;
                qn++;
                if (qn > r->max_queue)
                    r->max_queue = qn;
            }
        } else {
            if (e.time > r->makespan)
                r->makespan = e.time;
            if (qn > 0) {
                int j = queue[qh];
                qh = (qh + 1) % JOBS;
                qn--;
                r->wait[j] = e.time - arrival[j];
                fel_push(&f, (Event){e.time + service[j], EV_DEPARTURE, j});
            } else
                busy--;
        }
    }
    check(qn == 0 && busy == 0, "everything served");
    for (int i = 0; i < JOBS; i++) {
        r->total_wait += r->wait[i];
        if (r->wait[i] > r->max_wait)
            r->max_wait = r->wait[i];
        r->hash = (r->hash ^ (unsigned long long)r->wait[i]) * 1099511628211ull;
    }
    r->peak_fel = f.peak;
    free(f.a);
}

/* oracle: process jobs in arrival order, each takes the server that frees up first */
static void oracle(int servers, Result *r) {
    long free_at[MAXC] = {0};
    r->total_wait = r->max_wait = r->makespan = 0;
    r->hash = 1469598103934665603ull;
    for (int i = 0; i < JOBS; i++) {
        int b = 0;
        for (int s = 1; s < servers; s++)
            if (free_at[s] < free_at[b])
                b = s;
        long start = arrival[i] > free_at[b] ? arrival[i] : free_at[b];
        r->wait[i] = start - arrival[i];
        free_at[b] = start + service[i];
        if (free_at[b] > r->makespan)
            r->makespan = free_at[b];
        r->total_wait += r->wait[i];
        if (r->wait[i] > r->max_wait)
            r->max_wait = r->wait[i];
        r->hash = (r->hash ^ (unsigned long long)r->wait[i]) * 1099511628211ull;
    }
}

int main(void) {
    long t = 0;
    for (int i = 0; i < JOBS; i++) {
        t += (long)(rng() % 7); /* zero gaps give simultaneous arrivals */
        arrival[i] = t;
        service[i] = 1 + (long)(rng() % 12);
    }
    static Result a, b;
    printf("%-8s %-11s %-9s %-9s %-9s %-8s\n", "servers", "total_wait", "max_wait", "makespan", "max_queue", "peak_fel");
    for (int c = 1; c <= MAXC; c++) {
        simulate(c, &a);
        oracle(c, &b);
        check(a.total_wait == b.total_wait && a.max_wait == b.max_wait, "waits agree with the recurrence");
        check(a.makespan == b.makespan, "makespan agrees");
        check(a.hash == b.hash, "every job's wait agrees");
        printf("%-8d %-11ld %-9ld %-9ld %-9d %-8ld\n", c, a.total_wait, a.max_wait, a.makespan, a.max_queue, a.peak_fel);
    }
    return 0;
}
