/*
 * title: Unbounded linked queue with condvar and batch drain
 * topic: concurrency
 * covers: linked queue, condvar wait, splice-all drain, node accounting, try_pop, close
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    unsigned val;
    struct Node *next;
} Node;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Node *head, *tail;
    long len;
    int closed;
    long allocs, frees;
} Queue;

static Queue q = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0, 0, 0, 0};

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void push(unsigned v) {
    Node *n = malloc(sizeof *n);
    check(n != NULL, "malloc");
    n->val = v;
    n->next = NULL;
    pthread_mutex_lock(&q.mu);
    q.allocs++;
    if (q.tail)
        q.tail->next = n;
    else
        q.head = n;
    q.tail = n;
    q.len++;
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.mu);
}

/* Non blocking pop. Returns 1 if an item was taken. */
static int try_pop(unsigned *out) {
    int ok = 0;
    pthread_mutex_lock(&q.mu);
    if (q.head) {
        Node *n = q.head;
        q.head = n->next;
        if (!q.head)
            q.tail = NULL;
        q.len--;
        q.frees++;
        *out = n->val;
        free(n);
        ok = 1;
    }
    pthread_mutex_unlock(&q.mu);
    return ok;
}

/* Blocks until at least one item exists, then takes the whole list in one step. */
static Node *drain_all(void) {
    pthread_mutex_lock(&q.mu);
    while (!q.head && !q.closed)
        pthread_cond_wait(&q.cv, &q.mu);
    Node *list = q.head;
    q.head = q.tail = NULL;
    q.len = 0;
    pthread_mutex_unlock(&q.mu);
    return list;
}

static void close_queue(void) {
    pthread_mutex_lock(&q.mu);
    q.closed = 1;
    pthread_cond_broadcast(&q.cv);
    pthread_mutex_unlock(&q.mu);
}

typedef struct {
    unsigned long long sum;
    long count, batches;
    unsigned long long xr;
} Acc;

static void *producer(void *p) {
    unsigned base = (unsigned)(long)p * 1000000u;
    for (unsigned i = 0; i < 1000; i++)
        push(base + i);
    return NULL;
}

static void *drainer(void *p) {
    Acc *a = p;
    for (;;) {
        Node *l = drain_all();
        if (!l)
            break; /* closed and empty */
        a->batches++;
        while (l) {
            Node *n = l->next;
            a->sum += l->val;
            a->xr ^= (unsigned long long)l->val * 2654435761u;
            a->count++;
            pthread_mutex_lock(&q.mu);
            q.frees++;
            pthread_mutex_unlock(&q.mu);
            free(l);
            l = n;
        }
    }
    return NULL;
}

int main(void) {
    unsigned v = 0;
    check(!try_pop(&v), "empty try_pop");
    push(7);
    push(9);
    check(try_pop(&v) && v == 7, "fifo first");
    check(try_pop(&v) && v == 9, "fifo second");
    check(!try_pop(&v), "empty again");
    printf("single thread fifo ok, len %ld\n", q.len);

    enum { NP = 3, ND = 2 };
    pthread_t pt[NP], dt[ND];
    Acc acc[ND] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    for (int i = 0; i < ND; i++)
        check(pthread_create(&dt[i], NULL, drainer, &acc[i]) == 0, "create drainer");
    for (long i = 0; i < NP; i++)
        check(pthread_create(&pt[i], NULL, producer, (void *)(i + 1)) == 0, "create producer");
    for (int i = 0; i < NP; i++)
        pthread_join(pt[i], NULL);
    close_queue();
    for (int i = 0; i < ND; i++)
        pthread_join(dt[i], NULL);

    unsigned long long sum = 0, xr = 0;
    long count = 0;
    for (int i = 0; i < ND; i++) {
        sum += acc[i].sum;
        xr ^= acc[i].xr;
        count += acc[i].count;
        check(acc[i].batches >= 0, "batches");
    }
    unsigned long long expect = 0, expect_xr = 0;
    for (unsigned p = 1; p <= NP; p++)
        for (unsigned i = 0; i < 1000; i++) {
            unsigned x = p * 1000000u + i;
            expect += x;
            expect_xr ^= (unsigned long long)x * 2654435761u;
        }
    check(count == NP * 1000, "count");
    check(sum == expect, "sum");
    check(xr == expect_xr, "xor");
    check(q.allocs == q.frees, "every node freed");
    check(q.len == 0 && q.head == NULL, "empty at end");
    printf("items %ld sum %llu\n", count, sum);
    printf("xor %llu\n", xr);
    printf("allocs equal frees: yes\n");
    return 0;
}
