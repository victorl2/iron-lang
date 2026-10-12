/*
 * title: First readers-writers problem with a lightswitch
 * topic: concurrency
 * covers: readers preference, lightswitch pattern, room-empty semaphore, torn-read detection
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

/* Lightswitch: the first thread in locks the room, the last one out unlocks it. */
typedef struct {
    pthread_mutex_t m;
    int count;
} Lightswitch;

static void ls_lock(Lightswitch *ls, Sem *room) {
    pthread_mutex_lock(&ls->m);
    if (++ls->count == 1)
        sem_p(room);
    pthread_mutex_unlock(&ls->m);
}
static void ls_unlock(Lightswitch *ls, Sem *room) {
    pthread_mutex_lock(&ls->m);
    if (--ls->count == 0)
        sem_v(room);
    pthread_mutex_unlock(&ls->m);
}

enum { CELLS = 16, READERS = 5, WRITERS = 3, READS = 400, WRITES = 120 };

static Sem room_empty;
static Lightswitch readers_switch = {PTHREAD_MUTEX_INITIALIZER, 0};
static int cells[CELLS]; /* invariant when nobody writes: all cells equal */

typedef struct {
    int id;
    int ops;
    int torn;
    long sum;
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
        ls_lock(&readers_switch, &room_empty);
        int first = cells[0];
        for (int j = 1; j < CELLS; j++)
            if (cells[j] != first)
                w->torn++;
        w->sum += first;
        w->ops++;
        ls_unlock(&readers_switch, &room_empty);
    }
    return NULL;
}

static void *writer(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < WRITES; i++) {
        sem_p(&room_empty);
        int delta = w->id + 1;
        /* cell by cell: readers must never see a half-applied update */
        for (int j = 0; j < CELLS; j++)
            cells[j] += delta;
        w->ops++;
        sem_v(&room_empty);
    }
    return NULL;
}

int main(void) {
    sem_make(&room_empty, 1);
    Worker rd[READERS] = {{0, 0, 0, 0}}, wr[WRITERS] = {{0, 0, 0, 0}};
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

    long expect = 0;
    for (int i = 0; i < WRITERS; i++)
        expect += (long)WRITES * (i + 1);
    long torn = 0, reads = 0, maxsum = 0;
    for (int i = 0; i < READERS; i++) {
        torn += rd[i].torn;
        reads += rd[i].ops;
        if (rd[i].sum > maxsum)
            maxsum = rd[i].sum;
        /* a reader only ever sees non-decreasing versions, so its sum is bounded */
        check(rd[i].sum <= (long)READS * expect, "reader sum bound");
    }
    check(torn == 0, "no torn reads");
    check(reads == (long)READERS * READS, "read count");
    for (int j = 0; j < CELLS; j++)
        check(cells[j] == expect, "final cell value");
    printf("readers=%d writers=%d\n", READERS, WRITERS);
    printf("reads=%ld torn reads=%ld\n", reads, torn);
    for (int i = 0; i < WRITERS; i++)
        printf("writer %d wrote %d times (delta %d)\n", i, wr[i].ops, i + 1);
    printf("final cell value=%d\n", cells[0]);
    printf("readers all saw a consistent snapshot\n");
    return 0;
}
