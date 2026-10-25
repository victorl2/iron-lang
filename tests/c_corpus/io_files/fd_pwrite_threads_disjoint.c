/*
 * title: Concurrent pwrite and pread on disjoint regions
 * topic: io_files
 * covers: pwrite, pread, pthreads, shared descriptor without shared offset, striped layout, checksum
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


static inline unsigned long long fnv(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV0 14695981039346656037ULL

/* read until n bytes or EOF; returns count or -1 */
static inline long rd_full(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (long)got;
}
static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define THREADS 6
#define BLOCK 512
#define BLOCKS 240

typedef struct {
    int fd;
    int id;
    int failed;
    unsigned written;
    unsigned verified;
} Job;

static unsigned char pattern_byte(int block, int i) {
    unsigned v = (unsigned)block * 2654435761u + (unsigned)i * 40503u;
    return (unsigned char)(v >> 13);
}

/* thread t owns every block whose index is congruent to t modulo THREADS */
static void *writer(void *arg) {
    Job *j = arg;
    unsigned char buf[BLOCK];
    for (int b = j->id; b < BLOCKS; b += THREADS) {
        for (int i = 0; i < BLOCK; i++) buf[i] = pattern_byte(b, i);
        size_t done = 0;
        while (done < BLOCK) {
            ssize_t w = pwrite(j->fd, buf + done, BLOCK - done, (off_t)b * BLOCK + (off_t)done);
            if (w < 0) { j->failed = 1; return NULL; }
            done += (size_t)w;
        }
        j->written++;
    }
    return NULL;
}

/* thread t re-reads the blocks owned by the NEXT thread, exercising cross-region reads */
static void *reader(void *arg) {
    Job *j = arg;
    unsigned char buf[BLOCK];
    int other = (j->id + 1) % THREADS;
    for (int b = other; b < BLOCKS; b += THREADS) {
        ssize_t r = pread(j->fd, buf, BLOCK, (off_t)b * BLOCK);
        if (r != BLOCK) { j->failed = 1; return NULL; }
        for (int i = 0; i < BLOCK; i++)
            if (buf[i] != pattern_byte(b, i)) { j->failed = 1; return NULL; }
        j->verified++;
    }
    return NULL;
}

static void run_phase(void *(*fn)(void *), Job *jobs) {
    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++) CHECK(pthread_create(&th[t], NULL, fn, &jobs[t]) == 0);
    for (int t = 0; t < THREADS; t++) CHECK(pthread_join(th[t], NULL) == 0);
}

int main(void) {
    int fd = open("stripes.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    /* pre-extend so that readers never see EOF inside the range */
    CHECK(ftruncate(fd, (off_t)BLOCK * BLOCKS) == 0);

    Job jobs[THREADS];
    for (int t = 0; t < THREADS; t++) {
        jobs[t].fd = fd;
        jobs[t].id = t;
        jobs[t].failed = 0;
        jobs[t].written = jobs[t].verified = 0;
    }
    run_phase(writer, jobs);
    for (int t = 0; t < THREADS; t++) {
        printf("writer %d: blocks=%u failed=%d\n", t, jobs[t].written, jobs[t].failed);
        CHECK(!jobs[t].failed);
    }
    run_phase(reader, jobs);
    for (int t = 0; t < THREADS; t++) {
        printf("reader %d: verified=%u failed=%d\n", t, jobs[t].verified, jobs[t].failed);
        CHECK(!jobs[t].failed);
    }

    /* the descriptor's own offset was never touched by pread/pwrite */
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    puts("shared offset untouched: 0");

    /* sequential read of the whole file and compare with the model checksum */
    unsigned long long want = FNV0, got = FNV0;
    unsigned char buf[BLOCK];
    for (int b = 0; b < BLOCKS; b++) {
        for (int i = 0; i < BLOCK; i++) buf[i] = pattern_byte(b, i);
        want = fnv(want, buf, BLOCK);
    }
    for (int b = 0; b < BLOCKS; b++) {
        CHECK(rd_full(fd, buf, BLOCK) == BLOCK);
        got = fnv(got, buf, BLOCK);
    }
    printf("file size=%ld checksum=%016llx\n", fsize(fd), got);
    CHECK(got == want);
    CHECK(fsize(fd) == (long)BLOCK * BLOCKS);
    close(fd);
    unlink("stripes.bin");
    return 0;
}
