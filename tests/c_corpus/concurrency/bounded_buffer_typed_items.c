/*
 * title: Bounded buffer with several item types
 * topic: concurrency
 * covers: producer consumer, tagged unions, per-type bounded queues, monitor, checksums per type
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_INT, T_TEXT, T_PAIR, NTYPES, CAP = 3, PRODUCERS = 4, CONSUMERS = 3, PER_PRODUCER = 240 };

typedef struct {
    int type;
    union {
        long i;
        char text[8];
        struct {
            short a, b;
        } pair;
    } u;
} Item;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_full[NTYPES] = {PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};
static pthread_cond_t any_item = PTHREAD_COND_INITIALIZER;
static Item ring[NTYPES][CAP];
static int head[NTYPES], count[NTYPES];
static int producers_left = PRODUCERS;
static const char *const TNAME[NTYPES] = {"int", "text", "pair"};

typedef struct {
    long n[NTYPES];
    unsigned long sum[NTYPES];
} Tally;

static unsigned long item_value(const Item *it) {
    switch (it->type) {
    case T_INT:
        return (unsigned long)it->u.i;
    case T_TEXT: {
        unsigned long h = 5381;
        for (int k = 0; k < 8 && it->u.text[k]; k++)
            h = h * 33 + (unsigned char)it->u.text[k];
        return h;
    }
    default:
        return (unsigned long)(it->u.pair.a * 1000 + it->u.pair.b);
    }
}

static Item make_item(int producer, int seq) {
    Item it;
    memset(&it, 0, sizeof it);
    it.type = (seq + producer) % NTYPES;
    switch (it.type) {
    case T_INT:
        it.u.i = (long)producer * 100000 + seq * 7;
        break;
    case T_TEXT:
        snprintf(it.u.text, sizeof it.u.text, "p%ds%d", producer, seq % 100);
        break;
    default:
        it.u.pair.a = (short)(producer * 10 + 1);
        it.u.pair.b = (short)(seq % 500);
        break;
    }
    return it;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *producer(void *arg) {
    int p = (int)(long)arg;
    for (int s = 0; s < PER_PRODUCER; s++) {
        Item it = make_item(p, s);
        pthread_mutex_lock(&mu);
        while (count[it.type] == CAP)
            pthread_cond_wait(&not_full[it.type], &mu);
        ring[it.type][(head[it.type] + count[it.type]) % CAP] = it;
        count[it.type]++;
        pthread_cond_signal(&any_item);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    producers_left--;
    pthread_cond_broadcast(&any_item);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *consumer(void *arg) {
    Tally *t = arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        int pick = -1;
        for (int k = 0; k < NTYPES; k++)
            if (count[k] > 0) {
                pick = k;
                break;
            }
        if (pick < 0) {
            if (producers_left == 0)
                break;
            pthread_cond_wait(&any_item, &mu);
            continue;
        }
        Item it = ring[pick][head[pick]];
        head[pick] = (head[pick] + 1) % CAP;
        count[pick]--;
        pthread_cond_signal(&not_full[pick]);
        pthread_mutex_unlock(&mu);
        t->n[it.type]++;
        t->sum[it.type] += item_value(&it);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    pthread_t pt[PRODUCERS], ct[CONSUMERS];
    Tally tally[CONSUMERS];
    memset(tally, 0, sizeof tally);
    for (long c = 0; c < CONSUMERS; c++)
        check(pthread_create(&ct[c], NULL, consumer, &tally[c]) == 0, "consumer");
    for (long p = 0; p < PRODUCERS; p++)
        check(pthread_create(&pt[p], NULL, producer, (void *)p) == 0, "producer");
    for (int p = 0; p < PRODUCERS; p++)
        pthread_join(pt[p], NULL);
    for (int c = 0; c < CONSUMERS; c++)
        pthread_join(ct[c], NULL);

    long n[NTYPES] = {0};
    unsigned long sum[NTYPES] = {0};
    for (int c = 0; c < CONSUMERS; c++)
        for (int k = 0; k < NTYPES; k++) {
            n[k] += tally[c].n[k];
            sum[k] += tally[c].sum[k];
        }
    long en[NTYPES] = {0};
    unsigned long esum[NTYPES] = {0};
    for (int p = 0; p < PRODUCERS; p++)
        for (int s = 0; s < PER_PRODUCER; s++) {
            Item it = make_item(p, s);
            en[it.type]++;
            esum[it.type] += item_value(&it);
        }
    for (int k = 0; k < NTYPES; k++) {
        check(n[k] == en[k] && sum[k] == esum[k], "per-type totals");
        printf("%-4s items=%ld checksum=%lu\n", TNAME[k], n[k], sum[k]);
    }
    check(count[0] + count[1] + count[2] == 0, "buffers drained");
    printf("total=%ld\n", n[0] + n[1] + n[2]);
    return 0;
}
