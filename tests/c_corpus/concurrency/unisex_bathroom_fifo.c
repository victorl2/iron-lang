/*
 * title: Unisex bathroom with FIFO tickets
 * topic: concurrency
 * covers: unisex bathroom, category exclusion, capacity limit, ticket fairness, atomic occupancy checks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAPACITY = 3, MEN = 5, WOMEN = 4, VISITS = 60 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int next_ticket, now_serving;
static int inside, who; /* who: 0 empty, 1 men, 2 women */

static atomic_int occupancy[3];
static atomic_int violations;

typedef struct {
    int id, gender, visits;
    long minutes;
    unsigned rng;
} Person;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void enter(int g) {
    pthread_mutex_lock(&mu);
    int t = next_ticket++;
    /* strictly in ticket order: nobody overtakes, so neither gender starves */
    while (!(t == now_serving && (who == 0 || who == g) && inside < CAPACITY))
        pthread_cond_wait(&cv, &mu);
    who = g;
    inside++;
    now_serving++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void leave(void) {
    pthread_mutex_lock(&mu);
    if (--inside == 0)
        who = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void *person(void *arg) {
    Person *p = arg;
    for (int v = 0; v < VISITS; v++) {
        enter(p->gender);
        int mine = atomic_fetch_add(&occupancy[p->gender], 1) + 1;
        if (mine > CAPACITY || atomic_load(&occupancy[3 - p->gender]) != 0)
            atomic_fetch_add(&violations, 1);
        unsigned r = next(&p->rng);
        p->minutes += (long)(r % 9) + 1;
        p->visits++;
        atomic_fetch_sub(&occupancy[p->gender], 1);
        leave();
    }
    return NULL;
}

int main(void) {
    Person ppl[MEN + WOMEN];
    pthread_t th[MEN + WOMEN];
    for (int i = 0; i < MEN + WOMEN; i++) {
        ppl[i] = (Person){i, i < MEN ? 1 : 2, 0, 0, 0x9e3779b9u + (unsigned)i * 2654435761u};
        check(pthread_create(&th[i], NULL, person, &ppl[i]) == 0, "create");
    }
    for (int i = 0; i < MEN + WOMEN; i++)
        pthread_join(th[i], NULL);

    long visits[3] = {0, 0, 0}, minutes[3] = {0, 0, 0};
    for (int i = 0; i < MEN + WOMEN; i++) {
        check(ppl[i].visits == VISITS, "visits");
        visits[ppl[i].gender] += ppl[i].visits;
        minutes[ppl[i].gender] += ppl[i].minutes;
        /* replay the person's private generator to verify its total */
        unsigned s = 0x9e3779b9u + (unsigned)i * 2654435761u;
        long m = 0;
        for (int v = 0; v < VISITS; v++)
            m += (long)(next(&s) % 9) + 1;
        check(m == ppl[i].minutes, "minutes replay");
    }
    printf("capacity=%d men=%d women=%d\n", CAPACITY, MEN, WOMEN);
    printf("men: visits=%ld minutes=%ld\n", visits[1], minutes[1]);
    printf("women: visits=%ld minutes=%ld\n", visits[2], minutes[2]);
    printf("mixed-gender or over-capacity moments: %d\n", atomic_load(&violations));
    check(atomic_load(&violations) == 0, "no violations");
    check(inside == 0 && who == 0, "empty at end");
    check(next_ticket == now_serving && next_ticket == (MEN + WOMEN) * VISITS, "tickets served");
    return 0;
}
