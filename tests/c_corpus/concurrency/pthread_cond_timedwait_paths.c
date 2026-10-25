/*
 * title: Condition-variable timed wait: timeout and signalled paths
 * topic: concurrency
 * covers: pthread_cond_timedwait, ETIMEDOUT, absolute deadlines, predicate re-check, named return codes
 * deps: libc, pthread, posix
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int ready;
    int waiting;
    int rc_final;
    int loops;
} Box;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static const char *rcname(int rc) {
    if (rc == 0)
        return "OK";
    if (rc == ETIMEDOUT)
        return "ETIMEDOUT";
    return "OTHER";
}

/* absolute CLOCK_REALTIME deadline, ms from now (gettimeofday is portable) */
static struct timespec deadline_ms(long ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct timespec ts;
    long long ns = (long long)tv.tv_usec * 1000 + (long long)(ms % 1000) * 1000000;
    ts.tv_sec = tv.tv_sec + ms / 1000 + (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    return ts;
}

static void *late_signaller(void *p) {
    Box *b = p;
    pthread_mutex_lock(&b->mu);
    while (!b->waiting)
        pthread_cond_wait(&b->cv, &b->mu);
    b->ready = 1;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
    return NULL;
}

int main(void) {
    Box b = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};

    /* 1. nobody signals: the wait must end with ETIMEDOUT and the predicate stays false */
    pthread_mutex_lock(&b.mu);
    struct timespec dl = deadline_ms(30);
    int rc = 0;
    while (!b.ready && rc == 0) {
        rc = pthread_cond_timedwait(&b.cv, &b.mu, &dl);
        b.loops++;
    }
    pthread_mutex_unlock(&b.mu);
    printf("unsignalled wait: %s ready=%d\n", rcname(rc), b.ready);
    check(rc == ETIMEDOUT && !b.ready, "timeout path");

    /* 2. deadline already in the past: immediate ETIMEDOUT */
    pthread_mutex_lock(&b.mu);
    struct timespec past = deadline_ms(0);
    past.tv_sec -= 5;
    rc = pthread_cond_timedwait(&b.cv, &b.mu, &past);
    pthread_mutex_unlock(&b.mu);
    printf("past deadline: %s\n", rcname(rc));
    check(rc == ETIMEDOUT, "past deadline");

    /* 3. signalled long before a generous deadline: the loop exits on the predicate */
    pthread_t t;
    check(pthread_create(&t, NULL, late_signaller, &b) == 0, "create");
    pthread_mutex_lock(&b.mu);
    b.waiting = 1;
    pthread_cond_broadcast(&b.cv); /* tell the signaller we are about to wait */
    dl = deadline_ms(20000);
    rc = 0;
    while (!b.ready && rc == 0)
        rc = pthread_cond_timedwait(&b.cv, &b.mu, &dl);
    int ready = b.ready;
    pthread_mutex_unlock(&b.mu);
    pthread_join(t, NULL);
    printf("signalled wait: %s ready=%d\n", rcname(rc), ready);
    check(ready == 1 && rc == 0, "signalled path");

    /* 4. many short timeouts in a row still time out one by one */
    int timeouts = 0;
    pthread_mutex_lock(&b.mu);
    b.ready = 0;
    for (int i = 0; i < 5; i++) {
        struct timespec d = deadline_ms(5);
        while (!b.ready) {
            rc = pthread_cond_timedwait(&b.cv, &b.mu, &d);
            if (rc == ETIMEDOUT) {
                timeouts++;
                break;
            }
        }
    }
    pthread_mutex_unlock(&b.mu);
    printf("consecutive timeouts: %d\n", timeouts);
    check(timeouts == 5, "five timeouts");
    pthread_mutex_destroy(&b.mu);
    pthread_cond_destroy(&b.cv);
    return 0;
}
