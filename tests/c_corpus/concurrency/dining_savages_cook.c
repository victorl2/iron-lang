/*
 * title: Dining savages with a cook
 * topic: concurrency
 * covers: dining savages, refilling shared pot, empty-pot signalling, condition variables
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { SAVAGES = 6, MEALS_EACH = 23, POT = 8 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t empty_cv = PTHREAD_COND_INITIALIZER; /* cook waits here */
static pthread_cond_t full_cv = PTHREAD_COND_INITIALIZER;  /* savages wait here */
static int servings;
static int cook_called; /* set when a savage has asked for a refill */
static int finished;
static int refills;
static int cook_bad;
static int eaten_total;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *cook(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!cook_called && !finished)
            pthread_cond_wait(&empty_cv, &mu);
        if (finished)
            break;
        if (servings != 0)
            cook_bad++;
        servings = POT;
        refills++;
        cook_called = 0;
        pthread_cond_broadcast(&full_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

typedef struct {
    int id, meals;
    long flavour;
} Savage;

static void *savage(void *arg) {
    Savage *s = arg;
    for (int m = 0; m < MEALS_EACH; m++) {
        pthread_mutex_lock(&mu);
        while (servings == 0) {
            if (!cook_called) {
                cook_called = 1;
                pthread_cond_signal(&empty_cv);
            }
            pthread_cond_wait(&full_cv, &mu);
        }
        servings--;
        eaten_total++;
        s->flavour += (refills * 3 + s->id) % 7; /* refills is stable while this savage holds mu */
        pthread_mutex_unlock(&mu);
        s->meals++;
    }
    return NULL;
}

int main(void) {
    pthread_t ct, st[SAVAGES];
    Savage sv[SAVAGES] = {{0, 0, 0}};
    check(pthread_create(&ct, NULL, cook, NULL) == 0, "cook");
    for (int i = 0; i < SAVAGES; i++) {
        sv[i].id = i;
        check(pthread_create(&st[i], NULL, savage, &sv[i]) == 0, "savage");
    }
    for (int i = 0; i < SAVAGES; i++)
        pthread_join(st[i], NULL);
    pthread_mutex_lock(&mu);
    finished = 1;
    pthread_cond_signal(&empty_cv);
    pthread_mutex_unlock(&mu);
    pthread_join(ct, NULL);

    int total = SAVAGES * MEALS_EACH;
    int expect_refills = (total + POT - 1) / POT;
    for (int i = 0; i < SAVAGES; i++) {
        check(sv[i].meals == MEALS_EACH, "meals each");
        printf("savage %d ate %d\n", i, sv[i].meals);
    }
    printf("total eaten=%d pot size=%d refills=%d leftover=%d\n", eaten_total, POT, refills, servings);
    check(eaten_total == total, "total");
    check(refills == expect_refills, "refills");
    check(servings == expect_refills * POT - total, "leftover");
    check(cook_bad == 0, "cook only refills an empty pot");
    return 0;
}
