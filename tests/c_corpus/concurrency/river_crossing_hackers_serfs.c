/*
 * title: River crossing with hackers and serfs
 * topic: concurrency
 * covers: river crossing puzzle, boat formation rules, captain thread, staged arrivals, stranded threads
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { PEOPLE = 27, MAXTRIPS = 16, HACKER = 0, SERF = 1 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static int queue[2][PEOPLE], qn[2];
static int trip_of[PEOPLE]; /* trip number + 1, 0 = still waiting */
static int captain_of_trip[MAXTRIPS];
static int trip_h[MAXTRIPS], trip_s[MAXTRIPS], trip_members[MAXTRIPS][4];
static int trips;
static int registered;
static int cancelled;
static int kind_of[PEOPLE];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void take(int t, int kind, int n, int *slot) {
    for (int i = 0; i < n; i++) {
        int id = queue[kind][0];
        for (int j = 1; j < qn[kind]; j++)
            queue[kind][j - 1] = queue[kind][j];
        qn[kind]--;
        trip_of[id] = t + 1;
        trip_members[t][(*slot)++] = id;
        if (kind == HACKER)
            trip_h[t]++;
        else
            trip_s[t]++;
    }
}

static void *person(void *arg) {
    int id = (int)(long)arg;
    int me = kind_of[id];
    pthread_mutex_lock(&mu);
    queue[me][qn[me]++] = id;
    registered++;
    int h = qn[HACKER], s = qn[SERF];
    int form = -1; /* 0: 4 hackers, 1: 4 serfs, 2: 2+2 */
    if (h >= 4)
        form = 0;
    else if (s >= 4)
        form = 1;
    else if (h >= 2 && s >= 2)
        form = 2;
    if (form >= 0) {
        int t = trips++;
        int slot = 0;
        captain_of_trip[t] = id; /* the thread that completes the party rows the boat */
        if (form == 0)
            take(t, HACKER, 4, &slot);
        else if (form == 1)
            take(t, SERF, 4, &slot);
        else {
            take(t, HACKER, 2, &slot);
            take(t, SERF, 2, &slot);
        }
    }
    pthread_cond_broadcast(&cv);
    while (!trip_of[id] && !cancelled)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    unsigned s = 4242u;
    int nh = 0, ns = 0;
    for (int i = 0; i < PEOPLE; i++) {
        s = s * 1103515245u + 12345u;
        kind_of[i] = (int)((s >> 16) & 1u);
        if (kind_of[i] == HACKER)
            nh++;
        else
            ns++;
    }
    pthread_t th[PEOPLE];
    for (long i = 0; i < PEOPLE; i++) {
        check(pthread_create(&th[i], NULL, person, (void *)i) == 0, "create");
        pthread_mutex_lock(&mu);
        while (registered < i + 1)
            pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    cancelled = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < PEOPLE; i++)
        pthread_join(th[i], NULL);

    printf("arrivals: ");
    for (int i = 0; i < PEOPLE; i++)
        putchar(kind_of[i] == HACKER ? 'H' : 'S');
    printf("\n");
    int boarded_h = 0, boarded_s = 0;
    for (int t = 0; t < trips; t++) {
        printf("trip %d: %dH+%dS captain=%d members=", t + 1, trip_h[t], trip_s[t], captain_of_trip[t]);
        for (int k = 0; k < 4; k++)
            printf("%s%d", k ? "," : "", trip_members[t][k]);
        printf("\n");
        check(trip_h[t] + trip_s[t] == 4, "boat holds four");
        check(trip_h[t] != 3 && trip_s[t] != 3, "never 3+1");
        boarded_h += trip_h[t];
        boarded_s += trip_s[t];
    }
    printf("stranded: %d hackers, %d serfs\n", qn[HACKER], qn[SERF]);
    check(boarded_h + qn[HACKER] == nh && boarded_s + qn[SERF] == ns, "everyone accounted for");
    check(qn[HACKER] < 4 && qn[SERF] < 4 && !(qn[HACKER] >= 2 && qn[SERF] >= 2), "no legal boat left");
    return 0;
}
