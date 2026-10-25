/*
 * title: Third readers-writers problem with a turnstile
 * topic: concurrency
 * covers: starvation freedom, turnstile, lightswitch, audited account totals
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

enum { ACCOUNTS = 10, INITIAL = 1000, READERS = 4, WRITERS = 4, AUDITS = 300, TRANSFERS = 250 };

static Sem turnstile, room_empty;
static pthread_mutex_t rd_mu = PTHREAD_MUTEX_INITIALIZER;
static int rd_count;
static int balance[ACCOUNTS];

typedef struct {
    int id;
    int ops;
    int bad;
    unsigned rng;
} Worker;

static unsigned next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *auditor(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < AUDITS; i++) {
        /* pass through the turnstile so a waiting writer blocks new readers */
        sem_p(&turnstile);
        sem_v(&turnstile);
        pthread_mutex_lock(&rd_mu);
        if (++rd_count == 1)
            sem_p(&room_empty);
        pthread_mutex_unlock(&rd_mu);

        long total = 0;
        for (int a = 0; a < ACCOUNTS; a++)
            total += balance[a];
        if (total != (long)ACCOUNTS * INITIAL)
            w->bad++;
        w->ops++;

        pthread_mutex_lock(&rd_mu);
        if (--rd_count == 0)
            sem_v(&room_empty);
        pthread_mutex_unlock(&rd_mu);
    }
    return NULL;
}

static void *teller(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < TRANSFERS; i++) {
        unsigned r1 = next(&w->rng);
        unsigned r2 = next(&w->rng);
        unsigned r3 = next(&w->rng);
        int from = (int)(r1 % ACCOUNTS);
        int to = (int)(r2 % ACCOUNTS);
        int amt = (int)(r3 % 50) + 1;
        sem_p(&turnstile); /* hold the turnstile while waiting for the room */
        sem_p(&room_empty);
        balance[from] -= amt; /* deliberately unbalanced in between */
        balance[to] += amt;
        w->ops++;
        sem_v(&room_empty);
        sem_v(&turnstile);
    }
    return NULL;
}

int main(void) {
    sem_make(&turnstile, 1);
    sem_make(&room_empty, 1);
    for (int a = 0; a < ACCOUNTS; a++)
        balance[a] = INITIAL;
    Worker rd[READERS], wr[WRITERS];
    pthread_t rt[READERS], wt[WRITERS];
    for (int i = 0; i < READERS; i++) {
        rd[i] = (Worker){i, 0, 0, 0};
        check(pthread_create(&rt[i], NULL, auditor, &rd[i]) == 0, "create auditor");
    }
    for (int i = 0; i < WRITERS; i++) {
        wr[i] = (Worker){i, 0, 0, 0x1234567u + (unsigned)i * 7919u};
        check(pthread_create(&wt[i], NULL, teller, &wr[i]) == 0, "create teller");
    }
    for (int i = 0; i < READERS; i++)
        pthread_join(rt[i], NULL);
    for (int i = 0; i < WRITERS; i++)
        pthread_join(wt[i], NULL);

    /* replay every teller's transfers serially: addition commutes, so the final state is fixed */
    int expect[ACCOUNTS];
    for (int a = 0; a < ACCOUNTS; a++)
        expect[a] = INITIAL;
    for (int i = 0; i < WRITERS; i++) {
        unsigned rng = 0x1234567u + (unsigned)i * 7919u;
        for (int t = 0; t < TRANSFERS; t++) {
            unsigned r1 = next(&rng);
            unsigned r2 = next(&rng);
            unsigned r3 = next(&rng);
            expect[r1 % ACCOUNTS] -= (int)(r3 % 50) + 1;
            expect[r2 % ACCOUNTS] += (int)(r3 % 50) + 1;
        }
    }
    int bad = 0;
    long audits = 0;
    for (int i = 0; i < READERS; i++) {
        bad += rd[i].bad;
        audits += rd[i].ops;
    }
    check(bad == 0, "audits consistent");
    check(audits == (long)READERS * AUDITS, "audit count");
    long total = 0;
    for (int a = 0; a < ACCOUNTS; a++) {
        check(balance[a] == expect[a], "final balances");
        total += balance[a];
        printf("account %d balance %d\n", a, balance[a]);
    }
    printf("total=%ld audits=%ld inconsistent audits=%d\n", total, audits, bad);
    for (int i = 0; i < WRITERS; i++)
        check(wr[i].ops == TRANSFERS, "teller ops");
    printf("no writer starved: %d tellers finished %d transfers each\n", WRITERS, TRANSFERS);
    return 0;
}
