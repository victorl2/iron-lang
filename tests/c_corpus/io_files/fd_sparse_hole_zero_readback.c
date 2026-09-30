/*
 * title: Sparse file holes read back as zeros
 * topic: io_files
 * covers: lseek past EOF, holes, pwrite far offsets, size vs content, zero fill verification
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

static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define CHUNK 4096

/* count zero and nonzero bytes in [from, to) using fixed-size reads */
static void census(int fd, long from, long to, long *zeros, long *nonzero) {
    unsigned char buf[CHUNK];
    *zeros = *nonzero = 0;
    long pos = from;
    while (pos < to) {
        size_t want = (size_t)(to - pos < CHUNK ? to - pos : CHUNK);
        ssize_t r = pread(fd, buf, want, pos);
        CHECK(r == (ssize_t)want);
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i]) (*nonzero)++;
            else (*zeros)++;
        }
        pos += r;
    }
}

int main(void) {
    int fd = open("sparse.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);

    /* three islands of data with holes between them */
    struct { long off; int len; unsigned char fill; } isl[] = {
        {0, 10, 0xA1},
        {300000, 5, 0xB2},
        {1000000, 100, 0xC3},
    };
    for (int i = 0; i < 3; i++) {
        unsigned char b[100];
        memset(b, isl[i].fill, sizeof b);
        CHECK(pwrite(fd, b, (size_t)isl[i].len, isl[i].off) == isl[i].len);
    }
    long size = fsize(fd);
    printf("logical size: %ld\n", size);
    CHECK(size == 1000100);

    long z, nz;
    census(fd, 0, size, &z, &nz);
    printf("whole file: zero=%ld nonzero=%ld\n", z, nz);
    CHECK(nz == 115 && z == size - 115);

    long zs[2], nzs[2];
    census(fd, 10, 300000, &zs[0], &nzs[0]);
    census(fd, 300005, 1000000, &zs[1], &nzs[1]);
    printf("hole 1: zero=%ld nonzero=%ld\n", zs[0], nzs[0]);
    printf("hole 2: zero=%ld nonzero=%ld\n", zs[1], nzs[1]);
    CHECK(nzs[0] == 0 && nzs[1] == 0);
    CHECK(zs[0] == 299990 && zs[1] == 699995);

    /* filling a hole leaves the rest intact */
    unsigned char mid[16];
    memset(mid, 0x5A, sizeof mid);
    CHECK(pwrite(fd, mid, sizeof mid, 150000) == 16);
    census(fd, 0, size, &z, &nz);
    printf("after filling 16 bytes in hole: nonzero=%ld\n", nz);
    CHECK(nz == 131);
    CHECK(fsize(fd) == size);

    /* extending with lseek + single byte write */
    CHECK(lseek(fd, 5000000, SEEK_SET) == 5000000);
    CHECK(write(fd, "\x7f", 1) == 1);
    printf("size after write at 5000000: %ld\n", fsize(fd));
    CHECK(fsize(fd) == 5000001);
    unsigned char probe[3] = {9, 9, 9};
    CHECK(pread(fd, probe, 3, 4999990) == 3);
    printf("probe in new hole: %d %d %d\n", probe[0], probe[1], probe[2]);
    CHECK(probe[0] == 0 && probe[1] == 0 && probe[2] == 0);

    /* random probes of the logical layout against a model */
    unsigned bad = 0;
    for (int i = 0; i < 500; i++) {
        long pos = (long)(xs() >> 20) % 5000001;
        unsigned char b = 1;
        CHECK(pread(fd, &b, 1, pos) == 1);
        unsigned char want = 0;
        for (int k = 0; k < 3; k++)
            if (pos >= isl[k].off && pos < isl[k].off + isl[k].len) want = isl[k].fill;
        if (pos >= 150000 && pos < 150016) want = 0x5A;
        if (pos == 5000000) want = 0x7f;
        if (b != want) bad++;
    }
    printf("random probe mismatches: %u\n", bad);
    CHECK(bad == 0);
    close(fd);
    unlink("sparse.bin");
    return 0;
}
