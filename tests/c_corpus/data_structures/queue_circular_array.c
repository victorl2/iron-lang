/*
 * title: Circular array queue with fixed capacity
 * topic: data_structures
 * covers: circular queue, head/tail wraparound, full/empty disambiguation, count field, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x1F2E3D4C5B6A7988ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }

#define CAP 13 /* deliberately not a power of two */
typedef struct { int buf[CAP]; int head, tail, count; long wraps; } Queue;

static int q_full(const Queue *q) { return q->count == CAP; }
static int q_empty(const Queue *q) { return q->count == 0; }
static int q_enqueue(Queue *q, int v) {
    if (q_full(q)) return 0;
    q->buf[q->tail] = v;
    q->tail = (q->tail + 1) % CAP;
    if (q->tail == 0) q->wraps++;
    q->count++;
    return 1;
}
static int q_dequeue(Queue *q, int *out) {
    if (q_empty(q)) return 0;
    *out = q->buf[q->head];
    q->head = (q->head + 1) % CAP;
    q->count--;
    return 1;
}
static int q_at(const Queue *q, int i) { CHECK(i >= 0 && i < q->count); return q->buf[(q->head + i) % CAP]; }

/* Alternative design: no count, one slot sacrificed. Verified in lockstep. */
typedef struct { int buf[CAP + 1]; int head, tail; } Queue1;
static int q1_enqueue(Queue1 *q, int v) {
    int nt = (q->tail + 1) % (CAP + 1);
    if (nt == q->head) return 0;
    q->buf[q->tail] = v; q->tail = nt; return 1;
}
static int q1_dequeue(Queue1 *q, int *out) {
    if (q->head == q->tail) return 0;
    *out = q->buf[q->head]; q->head = (q->head + 1) % (CAP + 1); return 1;
}

int main(void) {
    Queue q; memset(&q, 0, sizeof q);
    Queue1 q1; memset(&q1, 0, sizeof q1);
    int model[CAP], mn = 0;
    long enq_ok = 0, enq_full = 0, deq_ok = 0, deq_empty = 0;
    long checksum = 0;
    int next = 1;
    for (int step = 0; step < 40000; step++) {
        /* alternate bursts so the queue repeatedly fills and drains */
        int burst = (step / 50) % 4;
        int p_enq = burst == 0 ? 85 : burst == 1 ? 50 : burst == 2 ? 15 : 50;
        if ((int)(rnd() % 100) < p_enq) {
            int a = q_enqueue(&q, next), b = q1_enqueue(&q1, next);
            CHECK(a == b);
            if (mn < CAP) { CHECK(a); model[mn++] = next; enq_ok++; } else { CHECK(!a); enq_full++; }
            next++;
        } else {
            int x = -1, y = -2;
            int a = q_dequeue(&q, &x), b = q1_dequeue(&q1, &y);
            CHECK(a == b);
            if (mn) {
                CHECK(a && x == model[0] && y == model[0]);
                memmove(model, model + 1, (size_t)(--mn) * sizeof(int));
                deq_ok++; checksum = (checksum * 31 + x) % 1000000007L;
            } else { CHECK(!a); deq_empty++; }
        }
        CHECK(q.count == mn);
        for (int i = 0; i < mn; i++) CHECK(q_at(&q, i) == model[i]);
        CHECK(q_full(&q) == (mn == CAP) && q_empty(&q) == (mn == 0));
    }
    printf("enqueue ok=%ld full=%ld\n", enq_ok, enq_full);
    printf("dequeue ok=%ld empty=%ld\n", deq_ok, deq_empty);
    printf("wraps=%ld head=%d tail=%d count=%d checksum=%ld\n", q.wraps, q.head, q.tail, q.count, checksum);
    return 0;
}
