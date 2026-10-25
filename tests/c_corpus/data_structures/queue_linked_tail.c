/*
 * title: Linked queue with tail pointer and iterator removal
 * topic: data_structures
 * covers: linked queue, tail pointer maintenance, remove-if, pointer-to-pointer, priority lanes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x6C62272E07BB0142ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 19); }

typedef struct QNode { int id, prio; struct QNode *next; } QNode;
typedef struct { QNode *head, *tail; size_t len; } Queue;

static void enq(Queue *q, int id, int prio) {
    QNode *n = malloc(sizeof *n); CHECK(n);
    n->id = id; n->prio = prio; n->next = NULL;
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n; q->len++;
}
static int deq(Queue *q, int *prio) {
    CHECK(q->head);
    QNode *n = q->head; int id = n->id;
    if (prio) *prio = n->prio;
    q->head = n->next; if (!q->head) q->tail = NULL;
    free(n); q->len--;
    return id;
}
/* remove every node for which pred is true, return count; keeps tail correct */
static size_t remove_if(Queue *q, int (*pred)(const QNode *, int), int arg) {
    size_t removed = 0;
    QNode **pp = &q->head, *prev = NULL;
    while (*pp) {
        QNode *n = *pp;
        if (pred(n, arg)) { *pp = n->next; free(n); removed++; q->len--; }
        else { prev = n; pp = &n->next; }
    }
    q->tail = prev;
    return removed;
}
static int pred_prio_below(const QNode *n, int arg) { return n->prio < arg; }
static int pred_id_mod(const QNode *n, int arg) { return n->id % arg == 0; }

/* dequeue the earliest node with the highest priority (stable among equals) */
static int deq_best(Queue *q, int *prio) {
    CHECK(q->head);
    QNode **best = &q->head;
    for (QNode **pp = &q->head; *pp; pp = &(*pp)->next) if ((*pp)->prio > (*best)->prio) best = pp;
    QNode *n = *best; int id = n->id; if (prio) *prio = n->prio;
    if (n == q->tail) {
        QNode *p = q->head; if (p == n) p = NULL; else while (p->next != n) p = p->next;
        q->tail = p;
    }
    *best = n->next; free(n); q->len--;
    return id;
}

#define MAXN 1024
int main(void) {
    Queue q = {0};
    int mid[MAXN], mpr[MAXN], mn = 0;
    int next_id = 1;
    long cnt[5] = {0};
    for (int step = 0; step < 8000; step++) {
        unsigned op = rnd() % 100;
        if (mn > 600) op = 60;
        if (op < 50) {
            int pr = (int)(rnd() % 5);
            enq(&q, next_id, pr); mid[mn] = next_id; mpr[mn] = pr; mn++; next_id++; cnt[0]++;
        } else if (op < 70 && mn) {
            int pr; int id = deq(&q, &pr);
            CHECK(id == mid[0] && pr == mpr[0]);
            memmove(mid, mid + 1, (size_t)(mn - 1) * sizeof(int)); memmove(mpr, mpr + 1, (size_t)(mn - 1) * sizeof(int)); mn--; cnt[1]++;
        } else if (op < 85 && mn) {
            int pr; int id = deq_best(&q, &pr);
            int b = 0; for (int i = 1; i < mn; i++) if (mpr[i] > mpr[b]) b = i;
            CHECK(id == mid[b] && pr == mpr[b]);
            memmove(mid + b, mid + b + 1, (size_t)(mn - b - 1) * sizeof(int)); memmove(mpr + b, mpr + b + 1, (size_t)(mn - b - 1) * sizeof(int)); mn--; cnt[2]++;
        } else if (op < 92) {
            int th = (int)(rnd() % 3);
            size_t r = remove_if(&q, pred_prio_below, th);
            int m = 0; for (int i = 0; i < mn; i++) if (mpr[i] >= th) { mid[m] = mid[i]; mpr[m] = mpr[i]; m++; }
            CHECK(r == (size_t)(mn - m)); mn = m; cnt[3]++;
        } else if (op < 96) {
            int d = 3 + (int)(rnd() % 10);
            size_t r = remove_if(&q, pred_id_mod, d);
            int m = 0; for (int i = 0; i < mn; i++) if (mid[i] % d != 0) { mid[m] = mid[i]; mpr[m] = mpr[i]; m++; }
            CHECK(r == (size_t)(mn - m)); mn = m; cnt[4]++;
        }
        CHECK(q.len == (size_t)mn);
        QNode *p = q.head, *last = NULL; int i = 0;
        for (; p; p = p->next, i++) { CHECK(i < mn && p->id == mid[i] && p->prio == mpr[i]); last = p; }
        CHECK(i == mn && last == q.tail);
    }
    printf("enq=%ld deq=%ld deq_best=%ld drop_low=%ld drop_mod=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
    printf("ids issued=%d remaining=%zu\n", next_id - 1, q.len);
    long lane[5] = {0};
    for (QNode *p = q.head; p; p = p->next) lane[p->prio]++;
    printf("lanes: %ld %ld %ld %ld %ld\n", lane[0], lane[1], lane[2], lane[3], lane[4]);
    while (q.len) deq(&q, NULL);
    return 0;
}
