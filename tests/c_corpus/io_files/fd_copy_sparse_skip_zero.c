/*
 * title: Sparse-aware file copy skipping zero blocks
 * topic: io_files
 * covers: read/write copy loop, zero block detection, lseek to create holes, ftruncate for trailing hole, checksum verification
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
static int all_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

typedef struct {
    long bytes;
    long blocks_written;
    long blocks_skipped;
} CopyStats;

/* Copy src to dst in blocks of bsz. Zero blocks become holes. */
static CopyStats sparse_copy(int in, int out, size_t bsz) {
    CopyStats st = {0, 0, 0};
    unsigned char *buf = malloc(bsz);
    CHECK(buf != NULL);
    for (;;) {
        long n = rd_full(in, buf, bsz);
        CHECK(n >= 0);
        if (n == 0) break;
        st.bytes += n;
        if (all_zero(buf, (size_t)n)) {
            CHECK(lseek(out, n, SEEK_CUR) >= 0);
            st.blocks_skipped++;
        } else {
            CHECK(wr_all(out, buf, (size_t)n) == 0);
            st.blocks_written++;
        }
        if ((size_t)n < bsz) break;
    }
    /* a trailing skipped block leaves the file short: fix its length */
    CHECK(ftruncate(out, st.bytes) == 0);
    free(buf);
    return st;
}

static unsigned long long checksum_fd(int fd, long *size) {
    unsigned char buf[1000];
    unsigned long long h = FNV0;
    long total = 0;
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    for (;;) {
        long n = rd_full(fd, buf, sizeof buf);
        CHECK(n >= 0);
        if (n == 0) break;
        h = fnv(h, buf, (size_t)n);
        total += n;
    }
    *size = total;
    return h;
}

int main(void) {
    /* source: random data islands separated by zero runs, ending in a zero tail */
    int src = open("src.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(src >= 0);
    unsigned char chunk[700];
    long len = 0;
    for (int seg = 0; seg < 40; seg++) {
        int zero_run = (seg % 3) != 0;
        unsigned n = 100 + rnd_below(600);
        for (unsigned i = 0; i < n; i++) chunk[i] = zero_run ? 0 : (unsigned char)(1 + rnd_below(255));
        CHECK(wr_all(src, chunk, n) == 0);
        len += n;
    }
    memset(chunk, 0, sizeof chunk);
    CHECK(wr_all(src, chunk, 300) == 0);
    len += 300;
    long size;
    unsigned long long want = checksum_fd(src, &size);
    printf("source: size=%ld checksum=%016llx\n", size, want);
    CHECK(size == len);

    static const size_t sizes[] = {1, 64, 256, 512, 1000, 4096};
    long prev_skipped = -1;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int dst = open("dst.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(dst >= 0);
        CHECK(lseek(src, 0, SEEK_SET) == 0);
        CopyStats st = sparse_copy(src, dst, sizes[k]);
        long dsize;
        unsigned long long got = checksum_fd(dst, &dsize);
        printf("block=%-5zu copied=%ld written=%ld skipped=%ld size=%ld %s\n", sizes[k], st.bytes,
               st.blocks_written, st.blocks_skipped, dsize, got == want ? "identical" : "DIFFERENT");
        CHECK(got == want && dsize == size && st.bytes == size);
        /* bytes accounted for */
        long blocks = (size + (long)sizes[k] - 1) / (long)sizes[k];
        CHECK(st.blocks_written + st.blocks_skipped == blocks);
        (void)prev_skipped;
        prev_skipped = st.blocks_skipped;
        close(dst);
    }

    /* a file that is entirely zeros ends up with only its length */
    int z = open("zeros.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(z >= 0);
    unsigned char zb[512];
    memset(zb, 0, sizeof zb);
    for (int i = 0; i < 20; i++) CHECK(wr_all(z, zb, sizeof zb) == 0);
    CHECK(lseek(z, 0, SEEK_SET) == 0);
    int zd = open("zeros.copy", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(zd >= 0);
    CopyStats zs = sparse_copy(z, zd, 512);
    printf("all-zero file: written=%ld skipped=%ld size=%ld\n", zs.blocks_written, zs.blocks_skipped, fsize(zd));
    CHECK(zs.blocks_written == 0 && zs.blocks_skipped == 20 && fsize(zd) == 10240);
    close(z);
    close(zd);

    /* an empty file copies to an empty file */
    int e1 = open("empty.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int e2 = open("empty.copy", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(e1 >= 0 && e2 >= 0);
    CopyStats es = sparse_copy(e1, e2, 128);
    printf("empty file: bytes=%ld size=%ld\n", es.bytes, fsize(e2));
    CHECK(es.bytes == 0 && fsize(e2) == 0);
    close(e1);
    close(e2);

    close(src);
    unlink("src.bin"); unlink("dst.bin"); unlink("zeros.bin");
    unlink("zeros.copy"); unlink("empty.bin"); unlink("empty.copy");
    return 0;
}
