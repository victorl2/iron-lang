/*
 * title: Full snapshot plus incremental checkpoint chain with restore and chain merge
 * topic: io_files
 * covers: dirty page tracking, incremental checkpoint files, restore by chain replay, chain compaction, per-file checksums, corruption detection
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rndn(uint32_t n) {
    uint64_t v = rnd();
    return (uint32_t)((v >> 16) % n);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint32_t crc32_update(uint32_t crc, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read up to n bytes at offset; returns the number of bytes read (short at EOF). */
static inline size_t pread_upto(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        check(r >= 0, "pread");
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got;
}

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer (caller frees). */
static inline unsigned char *slurp(const char *path, size_t *len) {
    long n = file_size(path);
    check(n >= 0, "slurp stat");
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    check(b != NULL, "malloc");
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "slurp open");
    size_t got = pread_upto(fd, b, (size_t)n, 0);
    close(fd);
    check(got == (size_t)n, "slurp short");
    *len = (size_t)n;
    return b;
}

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

enum { PSZ = 32, NPG = 48, NCK = 9 };

static unsigned char live[NPG][PSZ];
static unsigned char dirty[NPG];
static unsigned char saved[NCK][NPG][PSZ]; /* ground truth after each checkpoint */

static void ck_name(char *o, size_t c, int seq) { snprintf(o, c, "ck%02d.snap", seq); }

/* Header: "SNP1" | seq u32 | base u32 (0xFFFFFFFF for full) | count u32 ; then count*(pg u32 + PSZ) ; crc u32 */
static long write_ckpt(int seq, int full) {
    unsigned char buf[16 + NPG * (4 + PSZ) + 4];
    size_t n = 16;
    uint32_t cnt = 0;
    memcpy(buf, "SNP1", 4);
    put32(buf + 4, (uint32_t)seq);
    put32(buf + 8, full ? 0xFFFFFFFFu : (uint32_t)(seq - 1));
    for (int p = 0; p < NPG; p++)
        if (full || dirty[p]) {
            put32(buf + n, (uint32_t)p);
            memcpy(buf + n + 4, live[p], PSZ);
            n += 4 + PSZ;
            cnt++;
            dirty[p] = 0;
        }
    put32(buf + 12, cnt);
    put32(buf + n, crc32_update(0, buf, n));
    n += 4;
    char nm[32];
    ck_name(nm, sizeof nm, seq);
    spit(nm, buf, n);
    return (long)n;
}

/* Apply one checkpoint file to `img`; returns 0 on corruption, else 1 and reports pages applied. */
static int apply_ckpt(const char *nm, unsigned char img[NPG][PSZ], int *pages, uint32_t *base) {
    size_t n;
    unsigned char *b = slurp(nm, &n);
    int ok = n >= 20 && memcmp(b, "SNP1", 4) == 0 && get32(b + n - 4) == crc32_update(0, b, n - 4);
    if (ok) {
        uint32_t cnt = get32(b + 12);
        ok = n == 20 + (size_t)cnt * (4 + PSZ);
        for (uint32_t i = 0; ok && i < cnt; i++) {
            uint32_t pg = get32(b + 16 + i * (4 + PSZ));
            if (pg >= NPG) {
                ok = 0;
                break;
            }
            memcpy(img[pg], b + 20 + i * (4 + PSZ), PSZ);
        }
        *pages = (int)cnt;
        *base = get32(b + 8);
    }
    free(b);
    return ok;
}

static int restore(int upto, unsigned char img[NPG][PSZ], int first_full, int *applied_pages) {
    memset(img, 0, (size_t)NPG * PSZ);
    *applied_pages = 0;
    for (int s = first_full; s <= upto; s++) {
        char nm[32];
        ck_name(nm, sizeof nm, s);
        int pg;
        uint32_t base;
        if (!apply_ckpt(nm, img, &pg, &base))
            return 0;
        *applied_pages += pg;
    }
    return 1;
}

int main(void) {
    memset(live, 0, sizeof live);
    long total_bytes = 0, full_bytes = 0;
    printf("checkpoint  kind  pages  bytes\n");
    for (int seq = 0; seq < NCK; seq++) {
        int nmod = seq == 0 ? NPG : 1 + (int)rndn(7);
        for (int m = 0; m < nmod; m++) {
            int p = seq == 0 ? m : (int)rndn(NPG);
            for (int i = 0; i < PSZ; i++)
                live[p][i] = (unsigned char)(rndn(256));
            dirty[p] = 1;
        }
        int full = seq == 0 || seq == 5; /* a second full snapshot in the middle */
        long n = write_ckpt(seq, full);
        memcpy(saved[seq], live, sizeof live);
        total_bytes += n;
        if (full)
            full_bytes += n;
        printf("%5d       %-4s  %5d  %5ld\n", seq, full ? "full" : "incr", (int)((n - 20) / (4 + PSZ)), n);
    }
    /* Restore every checkpoint by replaying from the nearest preceding full snapshot. */
    for (int seq = 0; seq < NCK; seq++) {
        int first = seq >= 5 ? 5 : 0, applied;
        unsigned char img[NPG][PSZ];
        check(restore(seq, img, first, &applied), "restore ok");
        check(memcmp(img, saved[seq], sizeof img) == 0, "restored image equals live state at checkpoint");
    }
    printf("all %d checkpoints restore exactly; bytes total=%ld of which full=%ld\n", NCK, total_bytes, full_bytes);

    /* Chain merge: fold incrementals 1..4 into a fresh full snapshot 4 (replace file). */
    unsigned char img[NPG][PSZ];
    int applied;
    check(restore(4, img, 0, &applied), "restore for merge");
    memcpy(live, img, sizeof live);
    long merged = write_ckpt(4, 1);
    for (int s = 0; s < 4; s++) {
        char nm[32];
        ck_name(nm, sizeof nm, s);
        unlink(nm);
    }
    unsigned char img2[NPG][PSZ];
    check(restore(4, img2, 4, &applied), "restore merged");
    check(memcmp(img2, saved[4], sizeof img2) == 0, "merged full equals checkpoint 4");
    printf("merged chain 0..4 into full snapshot of %ld bytes; replay pages for seq 4: %d\n", merged, applied);

    /* Corruption in one incremental is detected and stops restore at that link. */
    char nm[32];
    ck_name(nm, sizeof nm, 7);
    size_t n;
    unsigned char *b = slurp(nm, &n);
    b[16 + 4 + 3] ^= 0x40;
    spit(nm, b, n);
    free(b);
    unsigned char img3[NPG][PSZ];
    int r6 = restore(6, img3, 4, &applied);
    int r7 = restore(7, img3, 4, &applied);
    check(r6 == 1 && r7 == 0, "corruption detected");
    printf("after bit flip in checkpoint 7: restore(6)=%s restore(7)=%s\n", r6 ? "ok" : "fail", r7 ? "ok" : "fail");
    for (int s = 4; s < NCK; s++) {
        ck_name(nm, sizeof nm, s);
        unlink(nm);
    }
    return 0;
}
