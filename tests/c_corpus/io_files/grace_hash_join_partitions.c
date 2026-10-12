/*
 * title: Grace hash join with on-disk partitions and recursive repartitioning
 * topic: io_files
 * covers: hash partitioning to files, build/probe, skew handling by repartition, order-independent digest, brute-force check
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

enum { NP = 4, MEM_BUILD = 120, MAXDEPTH = 4 };

typedef struct {
    uint32_t key, pay;
} Rec;

static uint32_t hash_key(uint32_t k, int depth) {
    uint32_t h = k * 2654435761u + (uint32_t)depth * 0x9E3779B9u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}

static long nfiles_made, parts_recursed;
static uint64_t sum_pay, xor_mix;
static long matches;

static void write_rel(const char *path, const Rec *r, int n) {
    unsigned char *b = malloc((size_t)n * 8 + 1);
    check(b != NULL, "malloc");
    for (int i = 0; i < n; i++) {
        put32(b + 8 * i, r[i].key);
        put32(b + 8 * i + 4, r[i].pay);
    }
    spit(path, b, (size_t)n * 8);
    free(b);
    nfiles_made++;
}

static Rec *read_rel(const char *path, int *n) {
    size_t len;
    unsigned char *b = slurp(path, &len);
    *n = (int)(len / 8);
    Rec *r = malloc((size_t)(*n + 1) * sizeof *r);
    check(r != NULL, "malloc rel");
    for (int i = 0; i < *n; i++) {
        r[i].key = get32(b + 8 * i);
        r[i].pay = get32(b + 8 * i + 4);
    }
    free(b);
    return r;
}

static void emit_match(const Rec *r, const Rec *s) {
    matches++;
    sum_pay += (uint64_t)r->pay * 3u + s->pay;
    xor_mix ^= (uint64_t)hash_key(r->key ^ (r->pay << 7), 9) * 0x100000001B3ULL + hash_key(s->pay, 5);
}

/* Partition file `path` into NP files named base_i. */
static void partition(const char *path, const char *base, int depth) {
    int n;
    Rec *r = read_rel(path, &n);
    Rec *bucket[NP];
    int cnt[NP] = {0};
    for (int p = 0; p < NP; p++) {
        bucket[p] = malloc((size_t)(n + 1) * sizeof(Rec));
        check(bucket[p] != NULL, "bucket");
    }
    for (int i = 0; i < n; i++) {
        int p = (int)(hash_key(r[i].key, depth) % NP);
        bucket[p][cnt[p]++] = r[i];
    }
    for (int p = 0; p < NP; p++) {
        char nm[64];
        snprintf(nm, sizeof nm, "%s_%d", base, p);
        write_rel(nm, bucket[p], cnt[p]);
        free(bucket[p]);
    }
    free(r);
}

static void join_files(const char *rpath, const char *spath, int depth, int tag) {
    int nr, ns;
    Rec *r = read_rel(rpath, &nr);
    if (nr > MEM_BUILD && depth < MAXDEPTH) {
        free(r);
        parts_recursed++;
        char rb[64], sb[64];
        snprintf(rb, sizeof rb, "r%d_%d", depth + 1, tag);
        snprintf(sb, sizeof sb, "s%d_%d", depth + 1, tag);
        partition(rpath, rb, depth + 1);
        partition(spath, sb, depth + 1);
        for (int p = 0; p < NP; p++) {
            char rn[64], sn[64];
            snprintf(rn, sizeof rn, "%s_%d", rb, p);
            snprintf(sn, sizeof sn, "%s_%d", sb, p);
            join_files(rn, sn, depth + 1, tag * NP + p + 1);
            unlink(rn);
            unlink(sn);
        }
        return;
    }
    Rec *s = read_rel(spath, &ns);
    /* in-memory chained hash table over the build side */
    int nb = 64;
    int *head = malloc((size_t)nb * sizeof(int)), *next = malloc((size_t)(nr + 1) * sizeof(int));
    check(head != NULL && next != NULL, "table");
    for (int i = 0; i < nb; i++)
        head[i] = -1;
    for (int i = 0; i < nr; i++) {
        uint32_t h = hash_key(r[i].key, 77) % (uint32_t)nb;
        next[i] = head[h];
        head[h] = i;
    }
    for (int j = 0; j < ns; j++) {
        uint32_t h = hash_key(s[j].key, 77) % (uint32_t)nb;
        for (int i = head[h]; i >= 0; i = next[i])
            if (r[i].key == s[j].key)
                emit_match(&r[i], &s[j]);
    }
    free(head);
    free(next);
    free(r);
    free(s);
}

int main(void) {
    enum { NR = 600, NS = 900 };
    static Rec R[NR], S[NS];
    for (int i = 0; i < NR; i++) {
        /* skewed build side: 30% of rows share 3 hot keys */
        R[i].key = rndn(10) < 3 ? 1000u + rndn(3) : rndn(500);
        R[i].pay = (uint32_t)i;
    }
    for (int i = 0; i < NS; i++) {
        S[i].key = rndn(10) < 2 ? 1000u + rndn(3) : rndn(700);
        S[i].pay = (uint32_t)(i * 7 + 1);
    }
    /* brute force reference */
    long bm = 0;
    uint64_t bsum = 0, bxor = 0;
    for (int i = 0; i < NR; i++)
        for (int j = 0; j < NS; j++)
            if (R[i].key == S[j].key) {
                bm++;
                bsum += (uint64_t)R[i].pay * 3u + S[j].pay;
                bxor ^= (uint64_t)hash_key(R[i].key ^ (R[i].pay << 7), 9) * 0x100000001B3ULL + hash_key(S[j].pay, 5);
            }
    write_rel("R.dat", R, NR);
    write_rel("S.dat", S, NS);
    partition("R.dat", "r0", 0);
    partition("S.dat", "s0", 0);
    for (int p = 0; p < NP; p++) {
        char rn[64], sn[64];
        snprintf(rn, sizeof rn, "r0_%d", p);
        snprintf(sn, sizeof sn, "s0_%d", p);
        long before = matches;
        join_files(rn, sn, 0, p + 1);
        printf("partition %d: build=%ld probe=%ld matches=%ld\n", p, file_size(rn) / 8, file_size(sn) / 8,
               matches - before);
        unlink(rn);
        unlink(sn);
    }
    unlink("R.dat");
    unlink("S.dat");
    check(matches == bm && sum_pay == bsum && xor_mix == bxor, "join equals nested loop");
    printf("total matches=%ld recursive repartitions=%ld files written=%ld\n", matches, parts_recursed, nfiles_made);
    printf("payload checksum=%llu\n", (unsigned long long)sum_pay);
    return 0;
}
