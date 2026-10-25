/*
 * title: Queue built from two stacks with amortized cost
 * topic: data_structures
 * covers: queue from two stacks, amortized O(1), lazy transfer, transfer counting, peek
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x0F0E0D0C0B0A0908ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 14); }

typedef struct { int *a; size_t n, cap; } Stk;
static void s_push(Stk *s, int v) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 4; s->a = realloc(s->a, s->cap * sizeof(int)); CHECK(s->a); }
    s->a[s->n++] = v;
}
static int s_pop(Stk *s) { CHECK(s->n); return s->a[--s->n]; }

typedef struct { Stk in, out; long transfers, moved; size_t max_size; } Queue;

static size_t q_size(const Queue *q) { return q->in.n + q->out.n; }
static void q_push(Queue *q, int v) {
    s_push(&q->in, v);
    if (q_size(q) > q->max_size) q->max_size = q_size(q);
}
static void refill(Queue *q) {
    if (q->out.n) return;
    if (q->in.n) q->transfers++;
    while (q->in.n) { s_push(&q->out, s_pop(&q->in)); q->moved++; }
}
static int q_front(Queue *q) { refill(q); CHECK(q->out.n); return q->out.a[q->out.n - 1]; }
static int q_pop(Queue *q) { refill(q); return s_pop(&q->out); }
/* back element: top of the in stack, or bottom of the out stack */
static int q_back(const Queue *q) { CHECK(q_size(q)); return q->in.n ? q->in.a[q->in.n - 1] : q->out.a[0]; }
static void q_free(Queue *q) { free(q->in.a); free(q->out.a); memset(q, 0, sizeof *q); }

#define MAXM 5000
int main(void) {
    Queue q; memset(&q, 0, sizeof q);
    static int model[MAXM]; int head = 0, tail = 0;
    long pushes = 0, pops = 0, fronts = 0, backs = 0, sum = 0;
    for (int step = 0; step < 50000; step++) {
        int phase = (step / 1000) % 5;
        int ppush = phase == 0 ? 90 : phase == 1 ? 10 : 50;
        unsigned r = rnd() % 100;
        if (r < 4 && tail > head) { CHECK(q_front(&q) == model[head]); fronts++; }
        else if (r < 8 && tail > head) { CHECK(q_back(&q) == model[tail - 1]); backs++; }
        else if ((int)(rnd() % 100) < ppush && tail - head < 1500) {
            int v = (int)(rnd() % 1000000);
            q_push(&q, v);
            if (tail == MAXM) { memmove(model, model + head, (size_t)(tail - head) * sizeof(int)); tail -= head; head = 0; }
            model[tail++] = v; pushes++;
        } else if (tail > head) {
            int v = q_pop(&q);
            CHECK(v == model[head++]);
            pops++; sum += v % 1000;
        }
        CHECK(q_size(&q) == (size_t)(tail - head));
    }
    printf("push=%ld pop=%ld front=%ld back=%ld\n", pushes, pops, fronts, backs);
    printf("transfers=%ld moved=%ld max_size=%zu final=%zu sum=%ld\n", q.transfers, q.moved, q.max_size, q_size(&q), sum);
    /* each element moves at most once between the stacks */
    CHECK(q.moved <= pushes);
    printf("moved_per_push_x100=%ld\n", q.moved * 100 / pushes);
    q_free(&q);
    return 0;
}
