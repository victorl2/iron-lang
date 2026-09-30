/*
 * title: Actors with mailboxes scheduled on a worker pool
 * topic: concurrency
 * covers: actor model, ready queue of actors, serial per-actor execution, message forwarding, per-sender FIFO, quiescence
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { NACT = 6, NW = 4, MBOX = 2048, RQ = 4096 };

typedef struct {
    int sender, seq, ttl;
    long val;
} Msg;

typedef struct {
    Msg box[MBOX];
    int head, tail;
    int scheduled;
    /* actor-private state: only touched by whichever worker currently runs this actor */
    long sum, count;
    int next_seq[NACT + 1];
    int last_seq[NACT + 1];
    int order_bad;
    int running;
    int overlap_bad;
} Actor;

static Actor act[NACT];
static int ready[RQ], rh, rt;
static long pending; /* messages sent but not yet fully handled */
static int stop;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void send_msg(int to, Msg m) {
    pthread_mutex_lock(&mu);
    Actor *a = &act[to];
    check(a->tail - a->head < MBOX && a->tail < MBOX * 100, "mailbox space");
    a->box[a->tail % MBOX] = m;
    a->tail++;
    pending++;
    if (!a->scheduled) {
        a->scheduled = 1;
        check(rt - rh < RQ, "ready queue space");
        ready[rt++ % RQ] = to;
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void handle(int id, Msg m) {
    Actor *a = &act[id];
    if (m.seq <= a->last_seq[m.sender])
        a->order_bad++;
    a->last_seq[m.sender] = m.seq;
    a->sum += m.val;
    a->count++;
    if (m.ttl > 0) {
        int to = (id + 1 + id % 3) % NACT;
        Msg n = {id, ++a->next_seq[to], m.ttl - 1, m.val + id + 1};
        send_msg(to, n);
    }
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (rh == rt && !stop)
            pthread_cond_wait(&cv, &mu);
        if (rh == rt && stop)
            break;
        int id = ready[rh++ % RQ];
        Actor *a = &act[id];
        for (;;) {
            if (a->head == a->tail) {
                a->scheduled = 0;
                break;
            }
            Msg m = a->box[a->head % MBOX];
            a->head++;
            pthread_mutex_unlock(&mu);
            if (a->running)
                a->overlap_bad++;
            a->running = 1;
            handle(id, m);
            a->running = 0;
            pthread_mutex_lock(&mu);
            pending--;
            if (pending == 0)
                pthread_cond_broadcast(&cv);
        }
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    for (int i = 0; i < NACT; i++)
        for (int s = 0; s <= NACT; s++)
            act[i].last_seq[s] = -1;
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    /* the main thread acts as sender id NACT */
    int mseq[NACT] = {0};
    for (int i = 0; i < 30; i++) {
        int to = i % NACT;
        Msg m = {NACT, mseq[to]++, 40, i};
        send_msg(to, m);
    }
    pthread_mutex_lock(&mu);
    while (pending > 0)
        pthread_cond_wait(&cv, &mu);
    stop = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    long total = 0, sum = 0;
    for (int i = 0; i < NACT; i++) {
        check(act[i].order_bad == 0, "per-sender FIFO");
        check(act[i].overlap_bad == 0, "an actor never runs on two workers at once");
        check(act[i].head == act[i].tail && !act[i].scheduled, "mailbox drained");
        printf("actor %d handled %ld messages, state sum %ld\n", i, act[i].count, act[i].sum);
        total += act[i].count;
        sum += act[i].sum;
    }
    check(total == 30 * 41, "every hop handled exactly once");
    printf("total messages %ld sum %ld\n", total, sum);
    return 0;
}
