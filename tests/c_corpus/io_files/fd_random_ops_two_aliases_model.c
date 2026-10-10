/*
 * title: Random file operations on aliased descriptors against a model
 * topic: io_files
 * covers: write/read/lseek/pread/pwrite/ftruncate mixed, dup shared offset, independent open offset, O_APPEND alias, byte-exact model, offsets tracked
 * deps: libc, posix
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
static inline unsigned rnd_below(unsigned n) {
    unsigned v = (unsigned)(xs() >> 33);
    return v % n;
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

static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define MAXLEN 4096

static unsigned char model[MAXLEN];
static long mlen = 0;

typedef struct { long off; } Model; /* offset of the shared description */

static void m_write(long off, const unsigned char *p, long n) {
    if (off + n > mlen) {
        memset(model + mlen, 0, (size_t)(off + n - mlen));
        mlen = off + n;
    }
    memcpy(model + off, p, (size_t)n);
}

static void verify(int fd) {
    CHECK(fsize(fd) == mlen);
    unsigned char *b = malloc((size_t)mlen + 1);
    CHECK(b != NULL);
    CHECK(pread(fd, b, (size_t)mlen, 0) == mlen);
    CHECK(memcmp(b, model, (size_t)mlen) == 0);
    free(b);
}

int main(void) {
    /* description A is reached through fd a and its dup a2; description B is a separate open with O_APPEND */
    int a = open("model.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int a2 = dup(a);
    int b = open("model.bin", O_RDWR | O_APPEND);
    CHECK(a >= 0 && a2 >= 0 && b >= 0);
    Model A = {0};

    long counts[7] = {0};
    for (int step = 0; step < 1500; step++) {
        unsigned op = rnd_below(14);
        unsigned char data[200];
        long n = 1 + rnd_below(150);
        for (long i = 0; i < n; i++) data[i] = (unsigned char)(1 + rnd_below(250));
        int use_a2 = rnd_below(2);
        int fd = use_a2 ? a2 : a;
        if (op < 3) { /* write through the shared-offset description */
            long off = A.off;
            if (off + n > MAXLEN) continue;
            CHECK(write(fd, data, (size_t)n) == n);
            m_write(off, data, n);
            A.off = off + n;
            counts[0]++;
        } else if (op < 5) { /* append through B */
            if (mlen + n > MAXLEN) continue;
            CHECK(write(b, data, (size_t)n) == n);
            m_write(mlen, data, n);
            counts[1]++;
        } else if (op < 7) { /* pwrite: offset untouched */
            long off = rnd_below(2500);
            if (off + n > MAXLEN) continue;
            CHECK(pwrite(fd, data, (size_t)n, off) == n);
            m_write(off, data, n);
            counts[2]++;
        } else if (op < 9) { /* seek A somewhere via either alias, then check the other alias agrees */
            long target = rnd_below((unsigned)mlen + 200);
            CHECK(lseek(fd, target, SEEK_SET) == target);
            A.off = target;
            CHECK(lseek(use_a2 ? a : a2, 0, SEEK_CUR) == target);
            counts[3]++;
        } else if (op < 11) { /* read through A and compare with the model */
            unsigned char rb[200];
            ssize_t r = read(fd, rb, (size_t)n);
            long expect = A.off >= mlen ? 0 : (mlen - A.off < n ? mlen - A.off : n);
            CHECK(r == expect);
            if (r > 0) CHECK(memcmp(rb, model + A.off, (size_t)r) == 0);
            A.off += r;
            counts[4]++;
        } else if (op < 12) { /* truncate */
            long nl = rnd_below(3000);
            CHECK(ftruncate(fd, nl) == 0);
            if (nl > mlen) memset(model + mlen, 0, (size_t)(nl - mlen));
            mlen = nl;
            counts[5]++;
        } else { /* seek relative to the end */
            long back = rnd_below(100);
            long target = mlen - back;
            if (target < 0) continue;
            CHECK(lseek(fd, -back, SEEK_END) == target);
            A.off = target;
            counts[6]++;
        }
        CHECK(lseek(a, 0, SEEK_CUR) == A.off);
        if (step % 100 == 99) verify(a);
    }
    verify(a);
    verify(b);
    printf("ops: write=%ld append=%ld pwrite=%ld seek=%ld read=%ld truncate=%ld seek_end=%ld\n",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
    printf("final length=%ld offset of shared description=%ld\n", mlen, A.off);
    printf("final content hash=%016llx\n", fnv(FNV0, model, (size_t)mlen));
    CHECK(lseek(a2, 0, SEEK_CUR) == A.off);
    close(a); close(a2); close(b);
    unlink("model.bin");
    return 0;
}
