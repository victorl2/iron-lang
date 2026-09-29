/*
 * title: Hash table with striped locks
 * topic: concurrency
 * covers: lock striping, chained hash table, concurrent insert and lookup, per-stripe counts
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { BUCKETS = 64, STRIPES = 8, T = 6, KEYS_PER = 500 };

typedef struct Entry {
    unsigned key;
    long value;
    struct Entry *next;
} Entry;

static Entry *table[BUCKETS];
static pthread_mutex_t stripe[STRIPES];
static long stripe_count[STRIPES];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned hash(unsigned k) {
    k ^= k >> 16;
    k *= 0x7feb352du;
    k ^= k >> 15;
    k *= 0x846ca68bu;
    k ^= k >> 16;
    return k;
}

/* stripe = bucket % STRIPES, so one lock guards every bucket in its class */
static void upsert_add(unsigned key, long delta) {
    unsigned b = hash(key) % BUCKETS;
    unsigned s = b % STRIPES;
    pthread_mutex_lock(&stripe[s]);
    Entry *e = table[b];
    while (e && e->key != key)
        e = e->next;
    if (e) {
        e->value += delta;
    } else {
        e = malloc(sizeof *e);
        check(e != NULL, "malloc");
        e->key = key;
        e->value = delta;
        e->next = table[b];
        table[b] = e;
        stripe_count[s]++;
    }
    pthread_mutex_unlock(&stripe[s]);
}

static int lookup(unsigned key, long *out) {
    unsigned b = hash(key) % BUCKETS;
    unsigned s = b % STRIPES;
    int found = 0;
    pthread_mutex_lock(&stripe[s]);
    for (Entry *e = table[b]; e; e = e->next)
        if (e->key == key) {
            *out = e->value;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&stripe[s]);
    return found;
}

typedef struct {
    int id;
} Arg;

/* keys come from a small universe so threads contend on the same entries */
static unsigned key_of(int id, int i) {
    unsigned x = (unsigned)(id * 7919 + i * 104729);
    return hash(x) % 300u;
}

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < KEYS_PER; i++)
        upsert_add(key_of(a->id, i), a->id + 1);
    return NULL;
}

int main(void) {
    for (int i = 0; i < STRIPES; i++)
        pthread_mutex_init(&stripe[i], NULL);
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    long expect[300] = {0};
    for (int id = 0; id < T; id++)
        for (int i = 0; i < KEYS_PER; i++)
            expect[key_of(id, i)] += id + 1;
    int distinct = 0;
    long total = 0;
    for (unsigned k = 0; k < 300; k++) {
        long v = -1;
        int f = lookup(k, &v);
        check(f == (expect[k] != 0), "presence");
        if (f) {
            check(v == expect[k], "value");
            distinct++;
            total += v;
        }
    }
    long counted = 0;
    for (int s = 0; s < STRIPES; s++)
        counted += stripe_count[s];
    check(counted == distinct, "stripe counts sum to distinct keys");
    long expect_total = 0;
    for (int id = 0; id < T; id++)
        expect_total += (long)KEYS_PER * (id + 1);
    check(total == expect_total, "grand total");
    printf("distinct keys=%d total=%ld\n", distinct, total);
    printf("key 0..4:");
    for (unsigned k = 0; k < 5; k++)
        printf(" %ld", expect[k]);
    printf("\n");

    for (int b = 0; b < BUCKETS; b++) {
        Entry *e = table[b];
        while (e) {
            Entry *n = e->next;
            free(e);
            e = n;
        }
    }
    for (int i = 0; i < STRIPES; i++)
        pthread_mutex_destroy(&stripe[i]);
    return 0;
}
