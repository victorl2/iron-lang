/*
 * title: Delay queue driven by a logical clock
 * topic: concurrency
 * covers: min-heap of deadlines, logical time instead of wall clock, workers gated by clock, quiescent ticks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 120, NW = 3, HORIZON = 40 };

typedef struct {
    int deadline;
    int id;
} Item;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static Item heap[N];
static int hn;
static int now, closed, completed;
static int fired_at[N];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int lt(Item a, Item b) {
    return a.deadline != b.deadline ? a.deadline < b.deadline : a.id < b.id;
}

static void push_locked(Item it) {
    int i = hn++;
    while (i > 0 && lt(it, heap[(i - 1) / 2])) {
        heap[i] = heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap[i] = it;
}

static Item pop_locked(void) {
    Item top = heap[0], last = heap[--hn];
    int i = 0;
    while (2 * i + 1 < hn) {
        int c = 2 * i + 1;
        if (c + 1 < hn && lt(heap[c + 1], heap[c]))
            c++;
        if (!lt(heap[c], last))
            break;
        heap[i] = heap[c];
        i = c;
    }
    if (hn > 0)
        heap[i] = last;
    return top;
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!closed && (hn == 0 || heap[0].deadline > now))
            pthread_cond_wait(&work_cv, &mu);
        if (hn == 0 || heap[0].deadline > now) {
            if (closed)
                break;
            continue;
        }
        Item it = pop_locked();
        fired_at[it.id] = now;
        completed++;
        pthread_cond_broadcast(&done_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    unsigned s = 99991u;
    int deadline[N];
    for (int i = 0; i < N; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        deadline[i] = (int)(s % 30u);
        fired_at[i] = -1;
    }
    /* a batch already due at time zero plus future items; a second wave is added mid-run */
    int wave2_at = 10;
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");

    int inserted = 0;
    int per_tick[HORIZON + 1] = {0};
    pthread_mutex_lock(&mu);
    for (int i = 0; i < N / 2; i++) {
        Item it = {deadline[i], i};
        push_locked(it);
        inserted++;
    }
    pthread_mutex_unlock(&mu);

    int expected_done = 0;
    for (int t = 0; t <= HORIZON; t++) {
        pthread_mutex_lock(&mu);
        if (t == wave2_at) {
            for (int i = N / 2; i < N; i++) {
                deadline[i] = t + deadline[i] % 20; /* relative delay from the current logical time */
                Item it = {deadline[i], i};
                push_locked(it);
                inserted++;
            }
        }
        now = t;
        expected_done = 0;
        for (int i = 0; i < inserted; i++)
            if (deadline[i] <= t)
                expected_done++;
        int before = completed;
        pthread_cond_broadcast(&work_cv);
        while (completed < expected_done)
            pthread_cond_wait(&done_cv, &mu);
        per_tick[t] = completed - before;
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    closed = 1;
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    long total = 0, lateness = 0;
    for (int i = 0; i < N; i++) {
        check(fired_at[i] == deadline[i], "fired exactly at its deadline");
        lateness += fired_at[i] - deadline[i];
        total++;
    }
    check(hn == 0 && completed == N, "all fired");
    printf("items %ld total lateness %ld\n", total, lateness);
    for (int t = 0; t <= HORIZON; t++)
        if (per_tick[t] > 0)
            printf("tick %2d fired %d\n", t, per_tick[t]);
    return 0;
}
