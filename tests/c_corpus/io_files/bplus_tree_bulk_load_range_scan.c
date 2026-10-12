/*
 * title: B+ tree bulk-loaded bottom-up with a leaf chain and range scans
 * topic: io_files
 * covers: bulk loading, fill factor, leaf sibling links, internal separator keys, root-to-leaf descent, range scan via chain, page-read accounting
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
enum { PSZ = 128, LEAF_MAX = 14, INT_MAX_KEYS = 12 };
/* Leaf page: 'L' n next u32 then n x (key u32, val u32).
 * Internal page: 'I' n then (n+1) child u32 followed by n key u32; child i holds keys < key[i]. */

typedef struct {
    int fd;
    uint32_t root;
    int height; /* levels including leaves */
    uint32_t npages;
    long reads;
} Tree;

static void pg_read(Tree *t, uint32_t p, unsigned char *pg) {
    check(pread_upto(t->fd, pg, PSZ, (off_t)p * PSZ) == PSZ, "read page");
    t->reads++;
}

static uint32_t next_page(Tree *t) { return t->npages++; }

typedef struct {
    uint32_t page;
    uint32_t first_key; /* smallest key in the subtree */
} Ref;

/* Build leaves with `fill` entries each from sorted keys. */
static int build_leaves(Tree *t, const uint32_t *keys, int n, int fill, Ref *out) {
    int nleaf = (n + fill - 1) / fill;
    uint32_t first_page = t->npages;
    for (int l = 0; l < nleaf; l++) {
        unsigned char pg[PSZ];
        memset(pg, 0, PSZ);
        int lo = l * fill, hi = lo + fill < n ? lo + fill : n;
        pg[0] = 'L';
        pg[1] = (unsigned char)(hi - lo);
        put32(pg + 4, l + 1 < nleaf ? first_page + (uint32_t)l + 1 : 0);
        for (int i = lo; i < hi; i++) {
            put32(pg + 8 + 8 * (i - lo), keys[i]);
            put32(pg + 12 + 8 * (i - lo), keys[i] ^ 0xA5A5u);
        }
        uint32_t p = next_page(t);
        pwrite_all(t->fd, pg, PSZ, (off_t)p * PSZ);
        out[l].page = p;
        out[l].first_key = keys[lo];
    }
    return nleaf;
}

static int build_level(Tree *t, const Ref *in, int n, Ref *out) {
    int per = INT_MAX_KEYS + 1, nout = 0;
    for (int i = 0; i < n; i += per) {
        int cnt = n - i < per ? n - i : per;
        if (n - i - cnt == 1) { /* avoid a trailing node with a single child */
            cnt--;
        }
        unsigned char pg[PSZ];
        memset(pg, 0, PSZ);
        pg[0] = 'I';
        pg[1] = (unsigned char)(cnt - 1);
        for (int c = 0; c < cnt; c++)
            put32(pg + 4 + 4 * c, in[i + c].page);
        for (int c = 1; c < cnt; c++)
            put32(pg + 4 + 4 * cnt + 4 * (c - 1), in[i + c].first_key);
        uint32_t p = next_page(t);
        pwrite_all(t->fd, pg, PSZ, (off_t)p * PSZ);
        out[nout].page = p;
        out[nout].first_key = in[i].first_key;
        nout++;
        i -= per - cnt; /* when cnt was reduced, the loop increment must advance by cnt only */
    }
    return nout;
}

static void bulk_load(Tree *t, const uint32_t *keys, int n, int fill) {
    static Ref a[512], b[512];
    Ref *cur = a, *nxt = b;
    t->npages = 1; /* page 0 reserved for the header */
    int cnt = build_leaves(t, keys, n, fill, cur);
    t->height = 1;
    while (cnt > 1) {
        cnt = build_level(t, cur, cnt, nxt);
        Ref *tmp = cur;
        cur = nxt;
        nxt = tmp;
        t->height++;
    }
    t->root = cur[0].page;
    unsigned char h[PSZ];
    memset(h, 0, PSZ);
    memcpy(h, "BPT1", 4);
    put32(h + 4, t->root);
    put32(h + 8, (uint32_t)t->height);
    put32(h + 12, t->npages);
    pwrite_all(t->fd, h, PSZ, 0);
}

/* Descend to the leaf that may contain `key`; returns the leaf page number. */
static uint32_t find_leaf(Tree *t, uint32_t key) {
    uint32_t p = t->root;
    for (int lvl = 1; lvl < t->height; lvl++) {
        unsigned char pg[PSZ];
        pg_read(t, p, pg);
        check(pg[0] == 'I', "internal page");
        int n = pg[1], c = 0;
        while (c < n && key >= get32(pg + 4 + 4 * (n + 1) + 4 * c))
            c++;
        p = get32(pg + 4 + 4 * c);
    }
    return p;
}

static int lookup(Tree *t, uint32_t key, uint32_t *val) {
    unsigned char pg[PSZ];
    pg_read(t, find_leaf(t, key), pg);
    check(pg[0] == 'L', "leaf page");
    for (int i = 0; i < pg[1]; i++)
        if (get32(pg + 8 + 8 * i) == key) {
            *val = get32(pg + 12 + 8 * i);
            return 1;
        }
    return 0;
}

/* Count keys in [lo, hi] by following the leaf chain; sums keys for verification. */
static long range_scan(Tree *t, uint32_t lo, uint32_t hi, uint64_t *sum, long *leaves) {
    uint32_t p = find_leaf(t, lo);
    long n = 0;
    *sum = 0;
    *leaves = 0;
    while (p) {
        unsigned char pg[PSZ];
        pg_read(t, p, pg);
        (*leaves)++;
        for (int i = 0; i < pg[1]; i++) {
            uint32_t k = get32(pg + 8 + 8 * i);
            if (k > hi)
                return n;
            if (k >= lo) {
                n++;
                *sum += k;
                check(get32(pg + 12 + 8 * i) == (k ^ 0xA5A5u), "value stored with key");
            }
        }
        p = get32(pg + 4);
    }
    return n;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y);
}

int main(void) {
    enum { N = 2500 };
    static uint32_t keys[N];
    for (int i = 0; i < N; i++)
        keys[i] = rndn(60000);
    qsort(keys, N, sizeof keys[0], cmp_u32);
    int m = 0;
    for (int i = 0; i < N; i++) /* dedupe */
        if (i == 0 || keys[i] != keys[i - 1])
            keys[m++] = keys[i];
    static const int fills[3] = {LEAF_MAX, 10, 7};
    for (int f = 0; f < 3; f++) {
        Tree t;
        memset(&t, 0, sizeof t);
        t.fd = open("bpt.idx", O_RDWR | O_CREAT | O_TRUNC, 0644);
        check(t.fd >= 0, "open");
        bulk_load(&t, keys, m, fills[f]);
        long ok = 0;
        for (int i = 0; i < m; i += 3) {
            uint32_t v = 0;
            check(lookup(&t, keys[i], &v) && v == (keys[i] ^ 0xA5A5u), "point lookup");
            ok++;
        }
        /* absent probes */
        int absent = 0;
        for (uint32_t k = 1; k < 60000 && absent < 300; k += 197) {
            uint32_t v;
            int found = lookup(&t, k, &v);
            int expect = bsearch(&k, keys, (size_t)m, sizeof keys[0], cmp_u32) != NULL;
            check(found == expect, "membership");
            absent++;
        }
        long lookup_reads = t.reads;
        /* range scans vs brute force */
        long scanned_total = 0, leaves_total = 0;
        for (int q = 0; q < 40; q++) {
            uint32_t lo = rndn(60000), span = rndn(4000);
            uint32_t hi = lo + span;
            uint64_t sum;
            long leaves;
            long n = range_scan(&t, lo, hi, &sum, &leaves);
            long bn = 0;
            uint64_t bs = 0;
            for (int i = 0; i < m; i++)
                if (keys[i] >= lo && keys[i] <= hi) {
                    bn++;
                    bs += keys[i];
                }
            check(n == bn && sum == bs, "range scan");
            scanned_total += n;
            leaves_total += leaves;
        }
        printf("fill=%2d: keys=%d pages=%u height=%d point lookups=%ld reads/lookup=%.2f\n", fills[f], m, t.npages,
               t.height, ok, (double)lookup_reads / (double)(ok + absent));
        printf("         40 range scans returned %ld keys over %ld leaf pages\n", scanned_total, leaves_total);
        close(t.fd);
    }
    unlink("bpt.idx");
    return 0;
}
