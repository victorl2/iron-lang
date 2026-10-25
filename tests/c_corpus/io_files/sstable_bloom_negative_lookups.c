/*
 * title: Fixed-record sorted table with sparse index and Bloom filter
 * topic: io_files
 * covers: sorted file, sparse block index, bloom filter block, footer offsets, block-read accounting, false positive counting
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
enum { BLOCK_RECS = 16, REC = 8, BPK = 10, NHASH = 7 };

typedef struct {
    int fd;
    uint32_t nrec, nblocks, index_off, bloom_off, bloom_bytes;
    uint32_t *first; /* first key of each block */
    unsigned char *bloom;
    long block_reads;
} Table;

static uint32_t h1(uint32_t k) {
    k ^= k >> 16; k *= 0x85ebca6bu; k ^= k >> 13; k *= 0xc2b2ae35u; k ^= k >> 16;
    return k;
}
static uint32_t h2(uint32_t k) { return (h1(k ^ 0x5bd1e995u) << 1) | 1u; }

static void bloom_set(unsigned char *bits, uint32_t nbits, uint32_t key) {
    uint32_t a = h1(key), b = h2(key);
    for (int i = 0; i < NHASH; i++) {
        uint32_t bit = (a + (uint32_t)i * b) % nbits;
        bits[bit >> 3] |= (unsigned char)(1u << (bit & 7));
    }
}
static int bloom_test(const unsigned char *bits, uint32_t nbits, uint32_t key) {
    uint32_t a = h1(key), b = h2(key);
    for (int i = 0; i < NHASH; i++) {
        uint32_t bit = (a + (uint32_t)i * b) % nbits;
        if (!(bits[bit >> 3] & (1u << (bit & 7))))
            return 0;
    }
    return 1;
}

/* Footer (20 bytes): nrec, nblocks, index_off, bloom_off, bloom_bytes. */
static void write_table(const char *path, const uint32_t *keys, uint32_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create table");
    uint32_t nblocks = (n + BLOCK_RECS - 1) / BLOCK_RECS;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char r[REC];
        put32(r, keys[i]);
        put32(r + 4, keys[i] * 3u + 1u);
        write_all(fd, r, REC);
    }
    uint32_t index_off = n * REC;
    for (uint32_t b = 0; b < nblocks; b++) {
        unsigned char r[4];
        put32(r, keys[b * BLOCK_RECS]);
        write_all(fd, r, 4);
    }
    uint32_t bloom_off = index_off + nblocks * 4;
    uint32_t nbits = n * BPK, nbytes = (nbits + 7) / 8;
    unsigned char *bits = calloc(nbytes, 1);
    check(bits != NULL, "bloom alloc");
    for (uint32_t i = 0; i < n; i++)
        bloom_set(bits, nbits, keys[i]);
    write_all(fd, bits, nbytes);
    free(bits);
    unsigned char f[20];
    put32(f, n);
    put32(f + 4, nblocks);
    put32(f + 8, index_off);
    put32(f + 12, bloom_off);
    put32(f + 16, nbytes);
    write_all(fd, f, 20);
    close(fd);
}

static void open_table(Table *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->fd = open(path, O_RDONLY);
    check(t->fd >= 0, "open table");
    off_t sz = (off_t)file_size(path);
    unsigned char f[20];
    check(pread_upto(t->fd, f, 20, sz - 20) == 20, "footer");
    t->nrec = get32(f);
    t->nblocks = get32(f + 4);
    t->index_off = get32(f + 8);
    t->bloom_off = get32(f + 12);
    t->bloom_bytes = get32(f + 16);
    unsigned char *ib = malloc(t->nblocks * 4u);
    check(pread_upto(t->fd, ib, t->nblocks * 4u, t->index_off) == t->nblocks * 4u, "index");
    t->first = malloc(t->nblocks * sizeof(uint32_t));
    for (uint32_t i = 0; i < t->nblocks; i++)
        t->first[i] = get32(ib + 4 * i);
    free(ib);
    t->bloom = malloc(t->bloom_bytes);
    check(pread_upto(t->fd, t->bloom, t->bloom_bytes, t->bloom_off) == t->bloom_bytes, "bloom");
}
static void close_table(Table *t) {
    close(t->fd);
    free(t->first);
    free(t->bloom);
}

static int lookup(Table *t, uint32_t key, int use_bloom, uint32_t *val) {
    if (use_bloom && !bloom_test(t->bloom, t->nrec * BPK, key))
        return 0;
    /* binary search the sparse index for the last block whose first key <= key */
    if (t->nblocks == 0 || key < t->first[0])
        return 0;
    uint32_t lo = 0, hi = t->nblocks - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (t->first[mid] <= key)
            lo = mid;
        else
            hi = mid - 1;
    }
    unsigned char blk[BLOCK_RECS * REC];
    uint32_t start = lo * BLOCK_RECS;
    uint32_t cnt = t->nrec - start < BLOCK_RECS ? t->nrec - start : BLOCK_RECS;
    check(pread_upto(t->fd, blk, cnt * REC, (off_t)start * REC) == cnt * REC, "block read");
    t->block_reads++;
    for (uint32_t i = 0; i < cnt; i++)
        if (get32(blk + REC * i) == key) {
            *val = get32(blk + REC * i + 4);
            return 1;
        }
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y);
}

int main(void) {
    enum { N = 3000 };
    static uint32_t keys[N];
    /* distinct even keys, so odd numbers are guaranteed absent */
    uint32_t k = 0;
    for (int i = 0; i < N; i++) {
        k += 2u * (1u + rndn(20));
        keys[i] = k;
    }
    qsort(keys, N, sizeof keys[0], cmp_u32);
    write_table("t.sst", keys, N);
    Table t;
    open_table(&t, "t.sst");
    check(t.nrec == N && t.nblocks == (N + BLOCK_RECS - 1) / BLOCK_RECS, "footer values");
    printf("records=%u blocks=%u bloom bytes=%u file bytes=%ld\n", t.nrec, t.nblocks, t.bloom_bytes,
           file_size("t.sst"));

    /* Present keys: always found, exactly one block read each. */
    for (int i = 0; i < N; i += 7) {
        uint32_t v = 0;
        check(lookup(&t, keys[i], 1, &v) && v == keys[i] * 3u + 1u, "present key");
    }
    long present_reads = t.block_reads;
    printf("present lookups=%d block reads=%ld\n", (N + 6) / 7, present_reads);

    /* Absent keys (odd) inside the key range: compare with and without the filter. */
    int probes = 0, fp = 0;
    long with_reads = 0, without_reads = 0;
    for (uint32_t q = 1; q < keys[N - 1]; q += 2) {
        uint32_t v;
        long before = t.block_reads;
        int hit = lookup(&t, q, 1, &v);
        check(!hit, "absent with bloom");
        long d = t.block_reads - before;
        with_reads += d;
        fp += (int)d;
        before = t.block_reads;
        hit = lookup(&t, q, 0, &v);
        check(!hit, "absent without bloom");
        without_reads += t.block_reads - before;
        probes++;
    }
    printf("absent probes=%d\n", probes);
    printf("block reads without filter=%ld with filter=%ld (false positives=%d)\n", without_reads, with_reads, fp);
    printf("false positive rate=%.2f%% (theory for 10 bits/key, 7 hashes: about 0.8%%)\n", 100.0 * fp / probes);
    check(fp * 100 < probes * 3, "fp rate under 3%");
    close_table(&t);
    unlink("t.sst");
    return 0;
}
