/*
 * title: File-backed extendible hash table
 * topic: io_files
 * covers: directory doubling, bucket pages on disk, local/global depth, bucket split, persistent directory file, model check
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

/* Write all bytes at an offset; abort the program on failure. */
static inline void pwrite_all(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, off);
        check(w > 0, "pwrite");
        p += w;
        off += w;
        n -= (size_t)w;
    }
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

enum { PSZ = 64, CAP = 6, MAXDEPTH = 12 };

typedef struct {
    int fd;
    int gdepth;
    uint32_t *dir; /* 1 << gdepth entries: bucket page numbers */
    uint32_t nbuckets;
    long reads, writes, splits, doublings;
} Table;

typedef struct {
    int ldepth, n;
    uint32_t key[CAP], val[CAP];
} Bucket;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void b_load(Table *t, uint32_t pno, Bucket *b) {
    unsigned char pg[PSZ];
    check(pread_upto(t->fd, pg, PSZ, (off_t)pno * PSZ) == PSZ, "bucket read");
    b->ldepth = pg[0];
    b->n = pg[1];
    for (int i = 0; i < CAP; i++) {
        b->key[i] = get32(pg + 4 + 8 * i);
        b->val[i] = get32(pg + 8 + 8 * i);
    }
    t->reads++;
}
static void b_store(Table *t, uint32_t pno, const Bucket *b) {
    unsigned char pg[PSZ];
    memset(pg, 0, PSZ);
    pg[0] = (unsigned char)b->ldepth;
    pg[1] = (unsigned char)b->n;
    for (int i = 0; i < CAP; i++) {
        put32(pg + 4 + 8 * i, b->key[i]);
        put32(pg + 8 + 8 * i, b->val[i]);
    }
    pwrite_all(t->fd, pg, PSZ, (off_t)pno * PSZ);
    t->writes++;
}

static void dir_save(const Table *t) {
    size_t n = (size_t)1 << t->gdepth;
    unsigned char *buf = malloc(8 + n * 4);
    check(buf != NULL, "dir buf");
    put32(buf, (uint32_t)t->gdepth);
    put32(buf + 4, t->nbuckets);
    for (size_t i = 0; i < n; i++)
        put32(buf + 8 + 4 * i, t->dir[i]);
    spit("table.dir", buf, 8 + n * 4);
    free(buf);
}

static void table_create(Table *t) {
    memset(t, 0, sizeof *t);
    t->fd = open("table.buckets", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(t->fd >= 0, "open buckets");
    t->gdepth = 0;
    t->dir = malloc(sizeof(uint32_t));
    t->dir[0] = 0;
    t->nbuckets = 1;
    Bucket b;
    memset(&b, 0, sizeof b);
    b_store(t, 0, &b);
    dir_save(t);
}

static void table_reopen(Table *t) {
    memset(t, 0, sizeof *t);
    t->fd = open("table.buckets", O_RDWR);
    check(t->fd >= 0, "reopen buckets");
    size_t n;
    unsigned char *d = slurp("table.dir", &n);
    t->gdepth = (int)get32(d);
    t->nbuckets = get32(d + 4);
    size_t ne = (size_t)1 << t->gdepth;
    check(n == 8 + ne * 4, "dir size");
    t->dir = malloc(ne * sizeof(uint32_t));
    for (size_t i = 0; i < ne; i++)
        t->dir[i] = get32(d + 8 + 4 * i);
    free(d);
}

static uint32_t dir_index(const Table *t, uint32_t h) { return h & (((uint32_t)1 << t->gdepth) - 1u); }

static int table_get(Table *t, uint32_t key, uint32_t *val) {
    Bucket b;
    b_load(t, t->dir[dir_index(t, hash32(key))], &b);
    for (int i = 0; i < b.n; i++)
        if (b.key[i] == key) {
            *val = b.val[i];
            return 1;
        }
    return 0;
}

static int table_del(Table *t, uint32_t key) {
    uint32_t pno = t->dir[dir_index(t, hash32(key))];
    Bucket b;
    b_load(t, pno, &b);
    for (int i = 0; i < b.n; i++)
        if (b.key[i] == key) {
            b.key[i] = b.key[b.n - 1];
            b.val[i] = b.val[b.n - 1];
            b.n--;
            b_store(t, pno, &b);
            return 1;
        }
    return 0;
}

/* Returns 1 for a new key, 0 for an update. */
static int table_put(Table *t, uint32_t key, uint32_t val) {
    for (;;) {
        uint32_t h = hash32(key);
        uint32_t di = dir_index(t, h);
        uint32_t pno = t->dir[di];
        Bucket b;
        b_load(t, pno, &b);
        for (int i = 0; i < b.n; i++)
            if (b.key[i] == key) {
                b.val[i] = val;
                b_store(t, pno, &b);
                return 0;
            }
        if (b.n < CAP) {
            b.key[b.n] = key;
            b.val[b.n] = val;
            b.n++;
            b_store(t, pno, &b);
            return 1;
        }
        check(b.ldepth < MAXDEPTH, "depth limit");
        if (b.ldepth == t->gdepth) { /* double the directory */
            size_t n = (size_t)1 << t->gdepth;
            uint32_t *nd = malloc(2 * n * sizeof(uint32_t));
            for (size_t i = 0; i < n; i++)
                nd[i] = nd[i + n] = t->dir[i];
            free(t->dir);
            t->dir = nd;
            t->gdepth++;
            t->doublings++;
        }
        /* split bucket pno on bit ldepth */
        Bucket lo, hi;
        memset(&lo, 0, sizeof lo);
        memset(&hi, 0, sizeof hi);
        lo.ldepth = hi.ldepth = b.ldepth + 1;
        uint32_t bit = (uint32_t)1 << b.ldepth;
        for (int i = 0; i < b.n; i++) {
            Bucket *dst = (hash32(b.key[i]) & bit) ? &hi : &lo;
            dst->key[dst->n] = b.key[i];
            dst->val[dst->n] = b.val[i];
            dst->n++;
        }
        uint32_t newp = t->nbuckets++;
        b_store(t, pno, &lo);
        b_store(t, newp, &hi);
        size_t n = (size_t)1 << t->gdepth;
        for (size_t i = 0; i < n; i++)
            if (t->dir[i] == pno && (i & bit))
                t->dir[i] = newp;
        t->splits++;
        dir_save(t);
    }
}

int main(void) {
    Table t;
    table_create(&t);
    enum { KEYSPACE = 5000 };
    static uint32_t model[KEYSPACE];
    static unsigned char has[KEYSPACE];
    int live = 0;
    for (int step = 0; step < 4000; step++) {
        uint32_t k = rndn(KEYSPACE);
        int op = (int)rndn(10);
        if (op < 7) {
            uint32_t v = (uint32_t)step + 100u;
            int fresh = table_put(&t, k, v);
            check(fresh == !has[k], "put freshness");
            if (fresh)
                live++;
            has[k] = 1;
            model[k] = v;
        } else if (op < 9) {
            int ok = table_del(&t, k);
            check(ok == has[k], "delete result");
            if (ok)
                live--;
            has[k] = 0;
        } else {
            uint32_t v = 0;
            int ok = table_get(&t, k, &v);
            check(ok == has[k] && (!ok || v == model[k]), "get");
        }
    }
    /* Directory persistence: reopen from the two files and verify everything. */
    close(t.fd);
    free(t.dir);
    table_reopen(&t);
    int found = 0;
    for (uint32_t k = 0; k < KEYSPACE; k++) {
        uint32_t v = 0;
        int ok = table_get(&t, k, &v);
        check(ok == has[k] && (!ok || v == model[k]), "reopened get");
        found += ok;
    }
    check(found == live, "live count");
    /* Structural invariants: every directory slot points to a bucket whose local depth <= global depth
     * and whose entries hash into that slot. */
    size_t ne = (size_t)1 << t.gdepth;
    int hist[MAXDEPTH + 1] = {0};
    uint32_t maxp = 0;
    for (size_t i = 0; i < ne; i++) {
        Bucket b;
        b_load(&t, t.dir[i], &b);
        check(b.ldepth <= t.gdepth, "local <= global");
        uint32_t mask = ((uint32_t)1 << b.ldepth) - 1u;
        for (int j = 0; j < b.n; j++)
            check((hash32(b.key[j]) & mask) == ((uint32_t)i & mask), "entry in right bucket");
        if (t.dir[i] > maxp)
            maxp = t.dir[i];
        if (((uint32_t)i >> b.ldepth) == 0)
            hist[b.ldepth]++; /* count each bucket once (lowest slot) */
    }
    printf("live keys=%d global depth=%d directory slots=%zu\n", live, t.gdepth, ne);
    printf("buckets=%u splits=%u max page=%u\n", t.nbuckets, t.nbuckets - 1, maxp);
    for (int d = 0; d <= t.gdepth; d++)
        if (hist[d])
            printf("  buckets with local depth %d: %d\n", d, hist[d]);
    printf("avg load=%.2f of %d\n", (double)live / t.nbuckets, CAP);
    close(t.fd);
    free(t.dir);
    unlink("table.buckets");
    unlink("table.dir");
    return 0;
}
