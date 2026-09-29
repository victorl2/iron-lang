/*
 * title: Parallel chunk checksums with pread from a shared descriptor
 * topic: io_files
 * covers: pread from many threads on one fd, work distribution by stride and by atomic-free claim under mutex, per-chunk results, combine in order, sequential cross-check
 * deps: libc, posix, pthread
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)


static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned long long fnv(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV0 14695981039346656037ULL

/* write everything, retrying short writes; 0 on success, -1 on error */
static inline int wr_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
#define CHUNK 4096
#define FILE_SIZE (CHUNK * 61 + 1234) /* last chunk is short */
#define NCHUNKS ((FILE_SIZE + CHUNK - 1) / CHUNK)
#define THREADS 5

typedef struct {
    int fd;
    pthread_mutex_t *mu;
    int *next;                       /* work queue cursor, protected by mu */
    unsigned long long *sums;        /* one slot per chunk, each written by exactly one thread */
    unsigned *lens;
    int done_by[THREADS];
    int id;
    int failed;
} Ctx;

static void chunk_sum(int fd, int idx, unsigned long long *sum, unsigned *len, int *failed) {
    unsigned char buf[CHUNK];
    off_t off = (off_t)idx * CHUNK;
    size_t want = (size_t)(FILE_SIZE - off < CHUNK ? FILE_SIZE - off : CHUNK);
    size_t got = 0;
    while (got < want) {
        ssize_t r = pread(fd, buf + got, want - got, off + (off_t)got);
        if (r <= 0) { *failed = 1; return; }
        got += (size_t)r;
    }
    *sum = fnv(FNV0, buf, got);
    *len = (unsigned)got;
}

/* dynamic scheduling: grab the next unclaimed chunk */
static void *worker_dynamic(void *arg) {
    Ctx *c = arg;
    for (;;) {
        pthread_mutex_lock(c->mu);
        int idx = (*c->next)++;
        pthread_mutex_unlock(c->mu);
        if (idx >= NCHUNKS) break;
        chunk_sum(c->fd, idx, &c->sums[idx], &c->lens[idx], &c->failed);
        c->done_by[c->id]++;
    }
    return NULL;
}

/* static scheduling: thread i takes chunks i, i+T, i+2T, ... */
static void *worker_strided(void *arg) {
    Ctx *c = arg;
    for (int idx = c->id; idx < NCHUNKS; idx += THREADS) {
        chunk_sum(c->fd, idx, &c->sums[idx], &c->lens[idx], &c->failed);
        c->done_by[c->id]++;
    }
    return NULL;
}

static unsigned long long combine(const unsigned long long *sums, const unsigned *lens) {
    unsigned long long h = FNV0;
    for (int i = 0; i < NCHUNKS; i++) {
        h = fnv(h, &sums[i], sizeof sums[i]);
        h = fnv(h, &lens[i], sizeof lens[i]);
    }
    return h;
}

int main(void) {
    int fd = open("big.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    unsigned char *data = malloc(FILE_SIZE);
    CHECK(data != NULL);
    for (int i = 0; i < FILE_SIZE; i++) data[i] = (unsigned char)(xs() >> 21);
    CHECK(wr_all(fd, data, FILE_SIZE) == 0);

    /* reference: sequential pass over the in-memory copy */
    unsigned long long ref_sums[NCHUNKS];
    unsigned ref_lens[NCHUNKS];
    for (int i = 0; i < NCHUNKS; i++) {
        size_t off = (size_t)i * CHUNK;
        size_t n = FILE_SIZE - off < CHUNK ? FILE_SIZE - off : CHUNK;
        ref_sums[i] = fnv(FNV0, data + off, n);
        ref_lens[i] = (unsigned)n;
    }
    unsigned long long ref = combine(ref_sums, ref_lens);
    printf("file size=%d chunks=%d (last chunk %u bytes)\n", FILE_SIZE, NCHUNKS, ref_lens[NCHUNKS - 1]);
    printf("reference combined checksum: %016llx\n", ref);

    for (int mode = 0; mode < 2; mode++) {
        pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        int next = 0;
        unsigned long long sums[NCHUNKS];
        unsigned lens[NCHUNKS];
        memset(sums, 0, sizeof sums);
        memset(lens, 0, sizeof lens);
        Ctx ctx[THREADS];
        pthread_t th[THREADS];
        for (int t = 0; t < THREADS; t++) {
            memset(&ctx[t], 0, sizeof ctx[t]);
            ctx[t].fd = fd; ctx[t].mu = &mu; ctx[t].next = &next;
            ctx[t].sums = sums; ctx[t].lens = lens; ctx[t].id = t;
            CHECK(pthread_create(&th[t], NULL, mode ? worker_strided : worker_dynamic, &ctx[t]) == 0);
        }
        int total = 0, failed = 0;
        for (int t = 0; t < THREADS; t++) {
            CHECK(pthread_join(th[t], NULL) == 0);
            total += ctx[t].done_by[t];
            failed |= ctx[t].failed;
        }
        unsigned long long got = combine(sums, lens);
        printf("%s scheduling: chunks processed=%d failed=%d combined=%016llx %s\n",
               mode ? "strided" : "dynamic", total, failed, got, got == ref ? "match" : "MISMATCH");
        CHECK(total == NCHUNKS && !failed && got == ref);
        if (mode == 1) {
            for (int t = 0; t < THREADS; t++) {
                int expect = (NCHUNKS - t + THREADS - 1) / THREADS;
                CHECK(ctx[t].done_by[t] == expect);
            }
            puts("strided scheduling gives each thread its exact share");
        }
        pthread_mutex_destroy(&mu);
    }

    /* the descriptor's offset was never used */
    CHECK(lseek(fd, 0, SEEK_CUR) == FILE_SIZE);
    puts("shared descriptor offset still at end of the initial write");
    free(data);
    close(fd);
    unlink("big.bin");
    return 0;
}
