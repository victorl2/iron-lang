/*
 * title: pthread_once initialization of a shared lookup table
 * topic: concurrency
 * covers: pthread_once, one-time init, racing first users, static init flag
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 8, SZ = 256 };

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t count_mu = PTHREAD_MUTEX_INITIALIZER;
static int init_calls = 0;
static unsigned crc_table[SZ];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void build_table(void) {
    /* deliberately slow-ish so that racing threads pile up on pthread_once */
    pthread_mutex_lock(&count_mu);
    init_calls++;
    pthread_mutex_unlock(&count_mu);
    for (unsigned n = 0; n < SZ; n++) {
        unsigned c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
}

static unsigned crc32(const unsigned char *p, size_t n) {
    pthread_once(&once, build_table);
    unsigned c = 0xffffffffu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

typedef struct {
    int id;
    unsigned crc_of_check;
    unsigned crc_of_own;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    a->crc_of_check = crc32((const unsigned char *)"123456789", 9);
    unsigned char buf[32];
    for (int i = 0; i < 32; i++)
        buf[i] = (unsigned char)(a->id * 31 + i);
    a->crc_of_own = crc32(buf, sizeof buf);
    return NULL;
}

int main(void) {
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(init_calls == 1, "init ran exactly once");
    for (int i = 0; i < T; i++) {
        check(args[i].crc_of_check == 0xCBF43926u, "crc32 check value");
        unsigned char buf[32];
        for (int k = 0; k < 32; k++)
            buf[k] = (unsigned char)(i * 31 + k);
        check(crc32(buf, sizeof buf) == args[i].crc_of_own, "own crc stable");
        printf("thread %d crc=%08x\n", i, args[i].crc_of_own);
    }
    printf("init calls=%d\n", init_calls);
    printf("crc32(123456789)=%08x\n", args[0].crc_of_check);
    return 0;
}
