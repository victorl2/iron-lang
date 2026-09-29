/*
 * title: Ping-pong volley between two threads
 * topic: concurrency
 * covers: two-party handshake, condvar hand-off of a value, forced interleaving, transcript buffer
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VOLLEYS = 12 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int who;      /* 0: ping's turn, 1: pong's turn */
    long ball;    /* value passed back and forth */
    int hits;
    char log[VOLLEYS * 2][24];
    int nlog;
} Court;

typedef struct {
    Court *c;
    int me;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *player(void *p) {
    Arg *a = p;
    Court *c = a->c;
    for (int i = 0; i < VOLLEYS; i++) {
        pthread_mutex_lock(&c->mu);
        while (c->who != a->me)
            pthread_cond_wait(&c->cv, &c->mu);
        if (a->me == 0)
            c->ball = c->ball * 3 + 1; /* ping */
        else
            c->ball = c->ball ^ (long)(i * 5 + 2); /* pong */
        snprintf(c->log[c->nlog++], sizeof c->log[0], "%s %ld", a->me ? "pong" : "ping", c->ball);
        c->hits++;
        c->who = 1 - a->me;
        pthread_cond_signal(&c->cv);
        pthread_mutex_unlock(&c->mu);
    }
    return NULL;
}

int main(void) {
    Court c = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 1, 0, {{0}}, 0};
    Arg a0 = {&c, 0}, a1 = {&c, 1};
    pthread_t t0, t1;
    check(pthread_create(&t1, NULL, player, &a1) == 0, "create pong");
    check(pthread_create(&t0, NULL, player, &a0) == 0, "create ping");
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    check(c.hits == 2 * VOLLEYS && c.nlog == 2 * VOLLEYS, "hits");

    long ball = 1;
    for (int i = 0; i < VOLLEYS; i++) {
        char want[24];
        ball = ball * 3 + 1;
        snprintf(want, sizeof want, "ping %ld", ball);
        check(strcmp(c.log[2 * i], want) == 0, "ping entry");
        ball = ball ^ (long)(i * 5 + 2);
        snprintf(want, sizeof want, "pong %ld", ball);
        check(strcmp(c.log[2 * i + 1], want) == 0, "pong entry");
    }
    for (int i = 0; i < c.nlog; i++)
        printf("%2d: %s\n", i, c.log[i]);
    printf("final ball=%ld hits=%d\n", c.ball, c.hits);
    return 0;
}
