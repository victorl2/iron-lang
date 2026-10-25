/*
 * title: Strict round-robin turn taking with one condvar
 * topic: concurrency
 * covers: condvar turn variable, forced sequence, recorded sequence buffer, broadcast wakeups
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 4, ROUNDS = 6 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int turn;
    char seq[T * ROUNDS + 1];
    int len;
} Table;

typedef struct {
    Table *t;
    int id;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *player(void *p) {
    Arg *a = p;
    Table *t = a->t;
    for (int r = 0; r < ROUNDS; r++) {
        pthread_mutex_lock(&t->mu);
        while (t->turn != a->id)
            pthread_cond_wait(&t->cv, &t->mu);
        t->seq[t->len++] = (char)('A' + a->id);
        t->turn = (t->turn + 1) % T;
        pthread_cond_broadcast(&t->cv);
        pthread_mutex_unlock(&t->mu);
    }
    return NULL;
}

int main(void) {
    Table t;
    memset(&t, 0, sizeof t);
    pthread_mutex_init(&t.mu, NULL);
    pthread_cond_init(&t.cv, NULL);
    Arg args[T];
    pthread_t th[T];
    /* start in reverse order so creation order cannot explain the result */
    for (int i = T - 1; i >= 0; i--) {
        args[i].t = &t;
        args[i].id = i;
        check(pthread_create(&th[i], NULL, player, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    t.seq[t.len] = 0;
    check(t.len == T * ROUNDS, "length");
    for (int i = 0; i < t.len; i++)
        check(t.seq[i] == 'A' + i % T, "strict alternation");
    printf("sequence: %s\n", t.seq);
    printf("turns taken=%d final turn=%d\n", t.len, t.turn);
    pthread_mutex_destroy(&t.mu);
    pthread_cond_destroy(&t.cv);
    return 0;
}
