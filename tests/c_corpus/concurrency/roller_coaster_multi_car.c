/*
 * title: Roller coaster with three cars taking turns
 * topic: concurrency
 * covers: roller coaster problem, ordered loading, car capacity, monitor with condition variable
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { CARS = 3, CAP = 6, PASSENGERS = 72, RIDES = PASSENGERS / CAP };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static int loading_ride = 0; /* index of the ride being loaded; cars load strictly in ride order */
static int loading_open;     /* boarding gate open */
static int boarded[RIDES];
static int unloaded[RIDES];
static int ride_done[RIDES];
static int ride_car[RIDES];
static int ride_of[PASSENGERS];
static int distance[CARS];
static int rides_run[CARS];
static int bad_state;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *car(void *arg) {
    int k = (int)(long)arg;
    for (int r = k; r < RIDES; r += CARS) {
        pthread_mutex_lock(&mu);
        while (loading_ride != r)
            pthread_cond_wait(&cv, &mu);
        loading_open = 1;
        ride_car[r] = k;
        pthread_cond_broadcast(&cv);
        while (boarded[r] < CAP)
            pthread_cond_wait(&cv, &mu);
        loading_open = 0;
        loading_ride = r + 1; /* the next car may start loading while this one runs */
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);

        int lap = 0; /* the "run": distance depends only on the ride number */
        for (int i = 0; i < 50; i++)
            lap += (r * 7 + i * 3) % 11;

        pthread_mutex_lock(&mu);
        ride_done[r] = 1;
        distance[k] += lap;
        rides_run[k]++;
        pthread_cond_broadcast(&cv);
        while (unloaded[r] < CAP)
            pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void *passenger(void *arg) {
    int p = (int)(long)arg;
    pthread_mutex_lock(&mu);
    while (!(loading_open && loading_ride < RIDES && boarded[loading_ride] < CAP))
        pthread_cond_wait(&cv, &mu);
    int r = loading_ride;
    ride_of[p] = r;
    boarded[r]++;
    pthread_cond_broadcast(&cv);
    while (!ride_done[r])
        pthread_cond_wait(&cv, &mu);
    if (boarded[r] != CAP)
        bad_state++;
    unloaded[r]++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    pthread_t ct[CARS], pt[PASSENGERS];
    for (long k = 0; k < CARS; k++)
        check(pthread_create(&ct[k], NULL, car, (void *)k) == 0, "car");
    for (long p = 0; p < PASSENGERS; p++)
        check(pthread_create(&pt[p], NULL, passenger, (void *)p) == 0, "passenger");
    for (int p = 0; p < PASSENGERS; p++)
        pthread_join(pt[p], NULL);
    for (int k = 0; k < CARS; k++)
        pthread_join(ct[k], NULL);

    int per_ride[RIDES] = {0};
    for (int p = 0; p < PASSENGERS; p++) {
        check(ride_of[p] >= 0 && ride_of[p] < RIDES, "ride index");
        per_ride[ride_of[p]]++;
    }
    for (int r = 0; r < RIDES; r++) {
        check(per_ride[r] == CAP, "full car");
        check(boarded[r] == CAP && unloaded[r] == CAP, "board/unload counts");
        check(ride_car[r] == r % CARS, "cars alternate");
        printf("ride %2d: car %d carried %d passengers\n", r, ride_car[r], per_ride[r]);
    }
    for (int k = 0; k < CARS; k++) {
        printf("car %d: rides=%d passengers=%d distance=%d\n", k, rides_run[k], rides_run[k] * CAP, distance[k]);
        check(rides_run[k] == RIDES / CARS, "rides per car");
    }
    check(bad_state == 0, "unload only after full ride");
    return 0;
}
