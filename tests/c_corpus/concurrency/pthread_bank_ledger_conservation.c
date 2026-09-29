/*
 * title: Bank ledger with conservation invariant
 * topic: concurrency
 * covers: per-account mutexes, ordered double locking, conservation of money, audit trail
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { ACCTS = 8, T = 6, XFERS = 3000, START = 10000 };

typedef struct {
    pthread_mutex_t mu;
    long balance;
    long in_count, out_count;
} Account;

static Account acct[ACCTS];

typedef struct {
    int id;
    unsigned seed;
    long done, refused;
    long moved;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned next(unsigned *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* returns 1 on success, 0 if insufficient funds */
static int transfer(int from, int to, long amt) {
    int lo = from < to ? from : to;
    int hi = from < to ? to : from;
    pthread_mutex_lock(&acct[lo].mu);
    pthread_mutex_lock(&acct[hi].mu);
    int ok = 0;
    if (acct[from].balance >= amt) {
        acct[from].balance -= amt;
        acct[to].balance += amt;
        acct[from].out_count++;
        acct[to].in_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&acct[hi].mu);
    pthread_mutex_unlock(&acct[lo].mu);
    return ok;
}

static long audit_total(void) {
    /* lock everything in index order for a consistent snapshot */
    long sum = 0;
    for (int i = 0; i < ACCTS; i++)
        pthread_mutex_lock(&acct[i].mu);
    for (int i = 0; i < ACCTS; i++)
        sum += acct[i].balance;
    for (int i = ACCTS - 1; i >= 0; i--)
        pthread_mutex_unlock(&acct[i].mu);
    return sum;
}

static int audit_bad;

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < XFERS; i++) {
        unsigned r1 = next(&a->seed);
        unsigned r2 = next(&a->seed);
        unsigned r3 = next(&a->seed);
        int from = (int)(r1 % ACCTS);
        int to = (int)(r2 % (ACCTS - 1));
        if (to >= from)
            to++;
        long amt = (long)(r3 % 900) + 1;
        if (transfer(from, to, amt)) {
            a->done++;
            a->moved += amt;
        } else {
            a->refused++;
        }
        if (a->id == 0 && i % 100 == 0 && audit_total() != (long)ACCTS * START)
            audit_bad++;
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < ACCTS; i++) {
        pthread_mutex_init(&acct[i].mu, NULL);
        acct[i].balance = START;
    }
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].seed = 0xABCD1234u + (unsigned)i * 977u;
        args[i].done = args[i].refused = args[i].moved = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    long attempts = 0;
    long done = 0;
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        attempts += args[i].done + args[i].refused;
        done += args[i].done;
    }
    long total = audit_total();
    long ins = 0, outs = 0;
    int negative = 0;
    for (int i = 0; i < ACCTS; i++) {
        ins += acct[i].in_count;
        outs += acct[i].out_count;
        if (acct[i].balance < 0)
            negative++;
    }
    check(audit_bad == 0, "mid-run audits");
    check(total == (long)ACCTS * START, "conservation");
    check(negative == 0, "no overdrafts");
    check(attempts == (long)T * XFERS, "attempts");
    check(ins == done && outs == done, "ledger counts");
    printf("total money=%ld (expected %ld)\n", total, (long)ACCTS * START);
    printf("attempts=%ld\n", attempts);
    printf("overdrawn accounts=%d\n", negative);
    printf("mid-run audit failures=%d\n", audit_bad);
    for (int i = 0; i < ACCTS; i++)
        pthread_mutex_destroy(&acct[i].mu);
    return 0;
}
