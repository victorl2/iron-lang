/*
 * title: Test-and-set spinlock guarding a bank with an auditor
 * topic: concurrency
 * covers: test-and-set spinlock, atomic_flag, conservation invariant, auditor thread, mutual exclusion counter
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    atomic_flag flag;
} TasLock;

static TasLock lock = {ATOMIC_FLAG_INIT};

static void tas_lock(TasLock *l) {
    while (atomic_flag_test_and_set_explicit(&l->flag, memory_order_acquire))
        sched_yield();
}
static int tas_trylock(TasLock *l) {
    return !atomic_flag_test_and_set_explicit(&l->flag, memory_order_acquire);
}
static void tas_unlock(TasLock *l) { atomic_flag_clear_explicit(&l->flag, memory_order_release); }

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { ACCOUNTS = 8, MOVERS = 4, MOVES = 1500, INITIAL = 1000, AUDITS = 300 };

static long balance[ACCOUNTS];
static atomic_int in_cs, violations, audit_failures;
static long transfers_done[MOVERS];
static long rejected[MOVERS];

static unsigned next_rand(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void *mover(void *p) {
    int id = (int)(intptr_t)p;
    unsigned rng = 0x1234567u + (unsigned)id * 7919u;
    for (int i = 0; i < MOVES; i++) {
        unsigned r1 = next_rand(&rng);
        unsigned r2 = next_rand(&rng);
        unsigned r3 = next_rand(&rng);
        int from = (int)(r1 % ACCOUNTS), to = (int)(r2 % ACCOUNTS);
        long amount = (long)(r3 % 300);
        tas_lock(&lock);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        if (from != to && balance[from] >= amount) {
            balance[from] -= amount;
            if (i % 4 == 0)
                sched_yield(); /* money is "in flight" here: only the lock hides it */
            balance[to] += amount;
            transfers_done[id]++;
        } else {
            rejected[id]++;
        }
        atomic_fetch_sub(&in_cs, 1);
        tas_unlock(&lock);
    }
    return NULL;
}

static void *auditor(void *p) {
    (void)p;
    for (int i = 0; i < AUDITS; i++) {
        tas_lock(&lock);
        long sum = 0;
        for (int a = 0; a < ACCOUNTS; a++) {
            sum += balance[a];
            if (balance[a] < 0)
                atomic_fetch_add(&audit_failures, 1);
        }
        if (sum != (long)ACCOUNTS * INITIAL)
            atomic_fetch_add(&audit_failures, 1);
        tas_unlock(&lock);
        sched_yield();
    }
    return NULL;
}

int main(void) {
    for (int a = 0; a < ACCOUNTS; a++)
        balance[a] = INITIAL;
    pthread_t th[MOVERS + 1];
    for (int i = 0; i < MOVERS; i++)
        pthread_create(&th[i], NULL, mover, (void *)(intptr_t)i);
    pthread_create(&th[MOVERS], NULL, auditor, NULL);
    for (int i = 0; i <= MOVERS; i++)
        pthread_join(th[i], NULL);

    long total = 0, done = 0, rej = 0;
    for (int a = 0; a < ACCOUNTS; a++)
        total += balance[a];
    for (int i = 0; i < MOVERS; i++) {
        done += transfers_done[i];
        rej += rejected[i];
    }
    printf("accounts=%d movers=%d attempts=%d\n", ACCOUNTS, MOVERS, MOVERS * MOVES);
    printf("attempts accounted for: %s\n", done + rej == (long)MOVERS * MOVES ? "yes" : "no");
    printf("final total %ld (initial %d)\n", total, ACCOUNTS * INITIAL);
    printf("mutual exclusion violations %d, audit failures %d\n", atomic_load(&violations),
           atomic_load(&audit_failures));
    check(total == (long)ACCOUNTS * INITIAL, "conservation");
    check(done + rej == (long)MOVERS * MOVES, "attempts");
    check(atomic_load(&violations) == 0, "exclusion");
    check(atomic_load(&audit_failures) == 0, "audit");
    check(done > 0, "some transfers succeeded");

    /* trylock semantics */
    int first = tas_trylock(&lock);
    int second = tas_trylock(&lock);
    tas_unlock(&lock);
    int third = tas_trylock(&lock);
    tas_unlock(&lock);
    printf("trylock sequence: %d %d %d\n", first, second, third);
    check(first == 1 && second == 0 && third == 1, "trylock");
    return 0;
}
