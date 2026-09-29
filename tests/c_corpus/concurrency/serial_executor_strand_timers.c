/*
 * title: Serial executor (strand) with logical timers
 * topic: concurrency
 * covers: single-threaded executor, closure queue, post and post-and-wait, unlocked state owned by strand, timer heap, periodic timers
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Closure {
    void (*fn)(struct Closure *);
    long a, b;
    long result;
    int done;
    int wait;
    struct Closure *next;
} Closure;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static Closure *head, *tail;
static int stopping;
static pthread_t strand_th;
static int foreign_calls;

/* State owned by the strand: no locks are used to touch it. */
enum { KEYS = 16, MAXT = 64, LOGN = 200 };
static long hist[KEYS];
static long now_tick;
typedef struct {
    long due, id, period, fires;
    int live;
} Timer;
static Timer timers[MAXT];
static int ntimers;
static long fire_log[LOGN][2];
static int nlog;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void post(Closure *c) {
    pthread_mutex_lock(&mu);
    c->next = NULL;
    if (tail)
        tail->next = c;
    else
        head = c;
    tail = c;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

static void post_wait(Closure *c) {
    c->wait = 1;
    c->done = 0;
    post(c);
    pthread_mutex_lock(&mu);
    while (!c->done)
        pthread_cond_wait(&done_cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void *strand_main(void *arg) {
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!head && !stopping)
            pthread_cond_wait(&cv, &mu);
        if (!head) {
            pthread_mutex_unlock(&mu);
            return NULL;
        }
        Closure *c = head;
        head = c->next;
        if (!head)
            tail = NULL;
        pthread_mutex_unlock(&mu);
        if (!pthread_equal(pthread_self(), strand_th))
            foreign_calls++;
        int heap_owned = !c->wait;
        int w = c->wait;
        c->fn(c);
        if (w) {
            pthread_mutex_lock(&mu);
            c->done = 1;
            pthread_cond_broadcast(&done_cv);
            pthread_mutex_unlock(&mu);
        }
        if (heap_owned)
            free(c);
    }
}

static void add_hist(Closure *c) {
    hist[c->a % KEYS] += c->b;
}

static void add_timer(Closure *c) {
    check(ntimers < MAXT, "timer space");
    Timer *t = &timers[ntimers];
    t->id = ntimers++;
    t->due = now_tick + c->a;
    t->period = c->b;
    t->fires = 0;
    t->live = 1;
    c->result = t->id;
}

static void advance(Closure *c) {
    now_tick++;
    /* fire due timers ordered by (due, id); repeated scan handles timers that become due again */
    for (;;) {
        int best = -1;
        for (int i = 0; i < ntimers; i++)
            if (timers[i].live && timers[i].due <= now_tick &&
                (best < 0 || timers[i].due < timers[best].due))
                best = i;
        if (best < 0)
            break;
        Timer *t = &timers[best];
        if (nlog < LOGN) {
            fire_log[nlog][0] = now_tick;
            fire_log[nlog][1] = t->id;
            nlog++;
        }
        t->fires++;
        if (t->period > 0)
            t->due += t->period;
        else
            t->live = 0;
    }
    c->result = now_tick;
}

typedef struct {
    int id;
} PosterArg;

static void *poster(void *arg) {
    int id = ((PosterArg *)arg)->id;
    for (int i = 0; i < 300; i++) {
        Closure *c = calloc(1, sizeof *c);
        check(c != NULL, "alloc");
        c->fn = add_hist;
        c->a = id * 5 + i;
        c->b = i + 1;
        post(c);
    }
    return NULL;
}

int main(void) {
    check(pthread_create(&strand_th, NULL, strand_main, NULL) == 0, "strand");
    pthread_t pt[4];
    PosterArg pa[4];
    for (int i = 0; i < 4; i++) {
        pa[i].id = i;
        check(pthread_create(&pt[i], NULL, poster, &pa[i]) == 0, "poster");
    }
    /* timers are registered by the main thread in a fixed order: (delay, period) */
    long spec[6][2] = {{3, 0}, {5, 5}, {2, 4}, {5, 0}, {9, 3}, {1, 7}};
    for (int i = 0; i < 6; i++) {
        Closure c = {add_timer, spec[i][0], spec[i][1], 0, 0, 0, NULL};
        post_wait(&c);
        check(c.result == i, "timer ids are sequential");
    }
    for (int i = 0; i < 4; i++)
        pthread_join(pt[i], NULL);
    long last = 0;
    for (int t = 0; t < 20; t++) {
        Closure c = {advance, 0, 0, 0, 0, 0, NULL};
        post_wait(&c);
        last = c.result;
    }
    pthread_mutex_lock(&mu);
    stopping = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(strand_th, NULL);

    long hs = 0, expect = 0;
    for (int k = 0; k < KEYS; k++)
        hs += hist[k];
    for (int id = 0; id < 4; id++)
        for (int i = 0; i < 300; i++)
            expect += i + 1;
    check(hs == expect, "all posted closures ran");
    check(foreign_calls == 0, "closures ran only on the strand thread");
    check(last == 20, "clock");
    printf("histogram total %ld over %d keys, key 0 = %ld key 5 = %ld\n", hs, KEYS, hist[0], hist[5]);
    printf("timer firings (tick:timer):");
    for (int i = 0; i < nlog; i++)
        printf(" %ld:%ld", fire_log[i][0], fire_log[i][1]);
    printf("\n");
    for (int i = 0; i < ntimers; i++)
        printf("timer %d fired %ld times, %s\n", i, timers[i].fires, timers[i].live ? "still armed" : "one-shot done");
    return 0;
}
