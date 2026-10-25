/*
 * title: Second readers-writers problem, writer priority
 * topic: concurrency
 * covers: writers preference, read-try gate, two lightswitches, sequence-numbered log
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int v;
} Sem;

static void sem_make(Sem *s, int v) {
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->v = v;
}
static void sem_p(Sem *s) {
    pthread_mutex_lock(&s->m);
    while (s->v == 0)
        pthread_cond_wait(&s->c, &s->m);
    s->v--;
    pthread_mutex_unlock(&s->m);
}
static void sem_v(Sem *s) {
    pthread_mutex_lock(&s->m);
    s->v++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

typedef struct {
    pthread_mutex_t m;
    int count;
} Lightswitch;

static void ls_lock(Lightswitch *ls, Sem *s) {
    pthread_mutex_lock(&ls->m);
    if (++ls->count == 1)
        sem_p(s);
    pthread_mutex_unlock(&ls->m);
}
static void ls_unlock(Lightswitch *ls, Sem *s) {
    pthread_mutex_lock(&ls->m);
    if (--ls->count == 0)
        sem_v(s);
    pthread_mutex_unlock(&ls->m);
}

enum { LOG = 24, READERS = 6, WRITERS = 3, READS = 300, WRITES = 100 };

static Lightswitch readers_ls = {PTHREAD_MUTEX_INITIALIZER, 0};
static Lightswitch writers_ls = {PTHREAD_MUTEX_INITIALIZER, 0};
static Sem no_readers, no_writers, read_try;

/* log[i] must equal log[0] + i whenever a reader looks; writers advance log[0] by 1 per write */
static long logbuf[LOG];

typedef struct {
    int id, ops, bad;
    long last_seen;
    int went_back;
} Worker;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *reader(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < READS; i++) {
        sem_p(&read_try); /* closed while any writer is waiting or writing */
        ls_lock(&readers_ls, &no_readers);
        sem_v(&read_try);
        long base = logbuf[0];
        for (int j = 1; j < LOG; j++)
            if (logbuf[j] != base + j)
                w->bad++;
        if (base < w->last_seen)
            w->went_back++;
        w->last_seen = base;
        w->ops++;
        ls_unlock(&readers_ls, &no_readers);
    }
    return NULL;
}

static void *writer(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < WRITES; i++) {
        ls_lock(&writers_ls, &read_try); /* first waiting writer shuts the readers out */
        sem_p(&no_writers);
        sem_p(&no_readers);
        for (int j = 0; j < LOG; j++)
            logbuf[j] += 1;
        w->ops++;
        sem_v(&no_readers);
        sem_v(&no_writers);
        ls_unlock(&writers_ls, &read_try);
    }
    return NULL;
}

int main(void) {
    sem_make(&no_readers, 1);
    sem_make(&no_writers, 1);
    sem_make(&read_try, 1);
    for (int j = 0; j < LOG; j++)
        logbuf[j] = 100 + j;
    Worker rd[READERS] = {{0, 0, 0, 0, 0}}, wr[WRITERS] = {{0, 0, 0, 0, 0}};
    pthread_t rt[READERS], wt[WRITERS];
    for (int i = 0; i < READERS; i++) {
        rd[i].id = i;
        check(pthread_create(&rt[i], NULL, reader, &rd[i]) == 0, "create reader");
    }
    for (int i = 0; i < WRITERS; i++) {
        wr[i].id = i;
        check(pthread_create(&wt[i], NULL, writer, &wr[i]) == 0, "create writer");
    }
    for (int i = 0; i < READERS; i++)
        pthread_join(rt[i], NULL);
    for (int i = 0; i < WRITERS; i++)
        pthread_join(wt[i], NULL);

    int bad = 0, back = 0;
    long reads = 0;
    for (int i = 0; i < READERS; i++) {
        bad += rd[i].bad;
        back += rd[i].went_back;
        reads += rd[i].ops;
    }
    check(bad == 0, "log consistent");
    check(back == 0, "versions monotonic per reader");
    check(reads == (long)READERS * READS, "reads");
    long writes = 0;
    for (int i = 0; i < WRITERS; i++)
        writes += wr[i].ops;
    check(writes == (long)WRITERS * WRITES, "writes");
    check(logbuf[0] == 100 + writes, "final version");
    printf("reads=%ld inconsistent=%d version regressions=%d\n", reads, bad, back);
    printf("writes=%ld final log head=%ld tail=%ld\n", writes, logbuf[0], logbuf[LOG - 1]);
    return 0;
}
