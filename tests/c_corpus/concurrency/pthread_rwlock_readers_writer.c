/*
 * title: Reader-writer lock protecting a versioned table
 * topic: concurrency
 * covers: pthread_rwlock, concurrent readers, exclusive writer, torn-read detection
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { COLS = 8, WRITERS = 2, READERS = 4, WRITES = 400, READS = 3000 };

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static long table[COLS];   /* all columns equal the version number when consistent */
static long version;

typedef struct {
    int id;
    long torn;
    long reads;
    long last_version;
    int went_backwards;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *writer(void *p) {
    Arg *a = p;
    for (int i = 0; i < WRITES; i++) {
        pthread_rwlock_wrlock(&rw);
        long v = ++version;
        for (int c = 0; c < COLS; c++) {
            table[c] = v; /* readers must never see a half-written row */
            a->reads++;
        }
        pthread_rwlock_unlock(&rw);
    }
    return NULL;
}

static void *reader(void *p) {
    Arg *a = p;
    for (int i = 0; i < READS; i++) {
        pthread_rwlock_rdlock(&rw);
        long first = table[0];
        for (int c = 1; c < COLS; c++)
            if (table[c] != first)
                a->torn++;
        long v = version;
        pthread_rwlock_unlock(&rw);
        if (v != first)
            a->torn++;
        if (first < a->last_version)
            a->went_backwards++;
        a->last_version = first;
        a->reads++;
    }
    return NULL;
}

int main(void) {
    Arg wa[WRITERS], ra[READERS];
    pthread_t wt[WRITERS], rt[READERS];
    for (int i = 0; i < READERS; i++) {
        ra[i] = (Arg){i, 0, 0, 0, 0};
        check(pthread_create(&rt[i], NULL, reader, &ra[i]) == 0, "create reader");
    }
    for (int i = 0; i < WRITERS; i++) {
        wa[i] = (Arg){i, 0, 0, 0, 0};
        check(pthread_create(&wt[i], NULL, writer, &wa[i]) == 0, "create writer");
    }
    for (int i = 0; i < WRITERS; i++)
        pthread_join(wt[i], NULL);
    for (int i = 0; i < READERS; i++)
        pthread_join(rt[i], NULL);
    long torn = 0, back = 0, reads = 0;
    for (int i = 0; i < READERS; i++) {
        torn += ra[i].torn;
        back += ra[i].went_backwards;
        reads += ra[i].reads;
    }
    check(torn == 0, "no torn reads");
    check(back == 0, "versions monotonic per reader");
    check(version == WRITERS * WRITES, "version count");
    for (int c = 0; c < COLS; c++)
        check(table[c] == version, "final row");
    check(pthread_rwlock_trywrlock(&rw) == 0, "lock is free at the end");
    pthread_rwlock_unlock(&rw);
    printf("final version=%ld\n", version);
    printf("reads=%ld torn=%ld backwards=%ld\n", reads, torn, back);
    pthread_rwlock_destroy(&rw);
    return 0;
}
