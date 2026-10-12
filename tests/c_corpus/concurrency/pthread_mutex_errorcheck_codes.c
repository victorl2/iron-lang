/*
 * title: Error-checking mutex return codes
 * topic: concurrency
 * covers: PTHREAD_MUTEX_ERRORCHECK, EDEADLK, EPERM, EBUSY, return-code naming
 * deps: libc, pthread
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static const char *name(int rc) {
    if (rc == 0)
        return "OK";
    if (rc == EDEADLK)
        return "EDEADLK";
    if (rc == EPERM)
        return "EPERM";
    if (rc == EBUSY)
        return "EBUSY";
    if (rc == EINVAL)
        return "EINVAL";
    return "OTHER";
}

typedef struct {
    pthread_mutex_t *m;
    int unlock_rc;
    int lock_rc_after;
} Arg;

static void *foreign_unlock(void *p) {
    Arg *a = p;
    /* this thread does not own the mutex */
    a->unlock_rc = pthread_mutex_unlock(a->m);
    return NULL;
}

static void *try_from_other(void *p) {
    Arg *a = p;
    a->lock_rc_after = pthread_mutex_trylock(a->m);
    if (a->lock_rc_after == 0)
        pthread_mutex_unlock(a->m);
    return NULL;
}

int main(void) {
    pthread_mutexattr_t at;
    pthread_mutex_t m;
    pthread_mutexattr_init(&at);
    check(pthread_mutexattr_settype(&at, PTHREAD_MUTEX_ERRORCHECK) == 0, "settype");
    check(pthread_mutex_init(&m, &at) == 0, "init");
    pthread_mutexattr_destroy(&at);

    int rc;
    rc = pthread_mutex_unlock(&m);
    printf("unlock unlocked: %s\n", name(rc));
    check(rc == EPERM, "unlock of unlocked errorcheck mutex is EPERM");

    rc = pthread_mutex_lock(&m);
    printf("first lock: %s\n", name(rc));
    check(rc == 0, "lock");

    rc = pthread_mutex_lock(&m);
    printf("relock by owner: %s\n", name(rc));
    check(rc == EDEADLK, "relock is EDEADLK");

    rc = pthread_mutex_trylock(&m);
    printf("trylock by owner: %s\n", name(rc));
    check(rc == EBUSY, "trylock is EBUSY");

    Arg a = {&m, -1, -1};
    pthread_t t;
    pthread_create(&t, NULL, foreign_unlock, &a);
    pthread_join(t, NULL);
    printf("unlock by non-owner: %s\n", name(a.unlock_rc));
    check(a.unlock_rc == EPERM, "foreign unlock is EPERM");

    pthread_create(&t, NULL, try_from_other, &a);
    pthread_join(t, NULL);
    printf("trylock from other thread while held: %s\n", name(a.lock_rc_after));
    check(a.lock_rc_after == EBUSY, "other thread sees EBUSY");

    rc = pthread_mutex_unlock(&m);
    printf("owner unlock: %s\n", name(rc));
    check(rc == 0, "unlock");

    rc = pthread_mutex_unlock(&m);
    printf("double unlock: %s\n", name(rc));
    check(rc == EPERM, "double unlock is EPERM");

    pthread_create(&t, NULL, try_from_other, &a);
    pthread_join(t, NULL);
    printf("trylock from other thread when free: %s\n", name(a.lock_rc_after));
    check(a.lock_rc_after == 0, "other thread can lock free mutex");

    rc = pthread_mutex_destroy(&m);
    printf("destroy: %s\n", name(rc));
    check(rc == 0, "destroy");
    return 0;
}
