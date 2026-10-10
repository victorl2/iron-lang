/*
 * title: Baboon crossing with a rope multiplex
 * topic: concurrency
 * covers: baboon crossing, directional exclusion, turnstile, multiplex of five, lightswitches
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
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
static void ls_lock(Lightswitch *l, Sem *s) {
    pthread_mutex_lock(&l->m);
    if (++l->count == 1)
        sem_p(s);
    pthread_mutex_unlock(&l->m);
}
static void ls_unlock(Lightswitch *l, Sem *s) {
    pthread_mutex_lock(&l->m);
    if (--l->count == 0)
        sem_v(s);
    pthread_mutex_unlock(&l->m);
}

enum { EAST, WEST, EACH = 7, CROSSINGS = 30, ROPE = 5 };

static Sem rope_free, turnstile, multiplex;
static Lightswitch dir_ls[2] = {{PTHREAD_MUTEX_INITIALIZER, 0}, {PTHREAD_MUTEX_INITIALIZER, 0}};
static atomic_int on_rope[2];
static atomic_int violations;

typedef struct {
    int id, dir;
    int crossings;
    long bananas;
} Baboon;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *baboon(void *arg) {
    Baboon *b = arg;
    int other = 1 - b->dir;
    for (int i = 0; i < CROSSINGS; i++) {
        sem_p(&turnstile); /* a waiting baboon of the other side blocks new arrivals here */
        ls_lock(&dir_ls[b->dir], &rope_free);
        sem_v(&turnstile);
        sem_p(&multiplex);
        int mine = atomic_fetch_add(&on_rope[b->dir], 1) + 1;
        if (mine > ROPE || atomic_load(&on_rope[other]) != 0)
            atomic_fetch_add(&violations, 1);
        b->crossings++;
        b->bananas += (b->id + 1) * (i % 5 + 1);
        atomic_fetch_sub(&on_rope[b->dir], 1);
        sem_v(&multiplex);
        ls_unlock(&dir_ls[b->dir], &rope_free);
    }
    return NULL;
}

int main(void) {
    sem_make(&rope_free, 1);
    sem_make(&turnstile, 1);
    sem_make(&multiplex, ROPE);
    Baboon bb[2 * EACH];
    pthread_t th[2 * EACH];
    for (int i = 0; i < 2 * EACH; i++) {
        bb[i] = (Baboon){i, i < EACH ? EAST : WEST, 0, 0};
        check(pthread_create(&th[i], NULL, baboon, &bb[i]) == 0, "create");
    }
    for (int i = 0; i < 2 * EACH; i++)
        pthread_join(th[i], NULL);

    long crossed[2] = {0, 0}, bananas[2] = {0, 0};
    for (int i = 0; i < 2 * EACH; i++) {
        check(bb[i].crossings == CROSSINGS, "crossings");
        crossed[bb[i].dir] += bb[i].crossings;
        bananas[bb[i].dir] += bb[i].bananas;
    }
    printf("east: baboons=%d crossings=%ld bananas=%ld\n", EACH, crossed[EAST], bananas[EAST]);
    printf("west: baboons=%d crossings=%ld bananas=%ld\n", EACH, crossed[WEST], bananas[WEST]);
    printf("rope violations=%d\n", atomic_load(&violations));
    check(atomic_load(&violations) == 0, "rope rules");
    check(atomic_load(&on_rope[0]) == 0 && atomic_load(&on_rope[1]) == 0, "rope empty");
    return 0;
}
