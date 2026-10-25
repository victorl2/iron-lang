/*
 * title: Bank transfers on random pairs without deadlock
 * topic: concurrency
 * covers: lock ordering by account id, random pairs, commutative replay, consistent audit thread
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { ACCOUNTS = 12, TELLERS = 6, TRANSFERS = 2500, START = 500, AUDITS = 200 };

static pthread_mutex_t acct_mu[ACCOUNTS];
static long balance[ACCOUNTS];

typedef struct {
    int id;
    unsigned rng;
    int done, self_skipped;
    int bad_audits;
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

/* Two locks are always taken in ascending account order, whichever side is the source. */
static void transfer(int from, int to, long amt) {
    int a = from < to ? from : to;
    int b = from < to ? to : from;
    pthread_mutex_lock(&acct_mu[a]);
    pthread_mutex_lock(&acct_mu[b]);
    balance[from] -= amt;
    balance[to] += amt;
    pthread_mutex_unlock(&acct_mu[b]);
    pthread_mutex_unlock(&acct_mu[a]);
}

static void *teller(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < TRANSFERS; i++) {
        unsigned r1 = next(&w->rng);
        unsigned r2 = next(&w->rng);
        unsigned r3 = next(&w->rng);
        int from = (int)(r1 % ACCOUNTS), to = (int)(r2 % ACCOUNTS);
        long amt = (long)(r3 % 200) + 1;
        if (from == to) {
            w->self_skipped++;
            continue;
        }
        transfer(from, to, amt);
        w->done++;
    }
    return NULL;
}

static void *auditor(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < AUDITS; i++) {
        long total = 0;
        for (int a = 0; a < ACCOUNTS; a++)
            pthread_mutex_lock(&acct_mu[a]);
        for (int a = 0; a < ACCOUNTS; a++)
            total += balance[a];
        for (int a = ACCOUNTS - 1; a >= 0; a--)
            pthread_mutex_unlock(&acct_mu[a]);
        if (total != (long)ACCOUNTS * START)
            w->bad_audits++;
    }
    return NULL;
}

int main(void) {
    for (int a = 0; a < ACCOUNTS; a++) {
        pthread_mutex_init(&acct_mu[a], NULL);
        balance[a] = START;
    }
    Worker w[TELLERS + 1];
    pthread_t th[TELLERS + 1];
    for (int i = 0; i < TELLERS; i++) {
        w[i] = (Worker){i, 0xC0FFEEu + (unsigned)i * 104729u, 0, 0, 0};
        check(pthread_create(&th[i], NULL, teller, &w[i]) == 0, "teller");
    }
    w[TELLERS] = (Worker){TELLERS, 0, 0, 0, 0};
    check(pthread_create(&th[TELLERS], NULL, auditor, &w[TELLERS]) == 0, "auditor");
    for (int i = 0; i <= TELLERS; i++)
        pthread_join(th[i], NULL);

    long expect[ACCOUNTS];
    for (int a = 0; a < ACCOUNTS; a++)
        expect[a] = START;
    long done = 0, skipped = 0;
    for (int i = 0; i < TELLERS; i++) {
        unsigned rng = 0xC0FFEEu + (unsigned)i * 104729u;
        for (int t = 0; t < TRANSFERS; t++) {
            unsigned r1 = next(&rng);
            unsigned r2 = next(&rng);
            unsigned r3 = next(&rng);
            int from = (int)(r1 % ACCOUNTS), to = (int)(r2 % ACCOUNTS);
            if (from == to)
                continue;
            expect[from] -= (long)(r3 % 200) + 1;
            expect[to] += (long)(r3 % 200) + 1;
        }
        done += w[i].done;
        skipped += w[i].self_skipped;
    }
    long total = 0;
    for (int a = 0; a < ACCOUNTS; a++) {
        check(balance[a] == expect[a], "replayed balances");
        total += balance[a];
        printf("account %2d: %ld\n", a, balance[a]);
    }
    check(w[TELLERS].bad_audits == 0, "audits saw conservation");
    check(total == (long)ACCOUNTS * START, "conservation");
    printf("transfers=%ld skipped(self)=%ld total=%ld bad audits=%d\n", done, skipped, total, w[TELLERS].bad_audits);
    for (int a = 0; a < ACCOUNTS; a++)
        pthread_mutex_destroy(&acct_mu[a]);
    return 0;
}
