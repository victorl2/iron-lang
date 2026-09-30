/*
 * title: File-backed linear hashing with overflow page chains
 * topic: io_files
 * covers: linear hashing split pointer, incremental bucket growth, overflow chains, page free list, metadata persistence, model check
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
enum { PSZ = 64, CAP = 6, N0 = 4, MAXB = 1024, MAXP = 4096 };
/* Page: n u8 | pad | next u32 | CAP x (key u32, val u32).  next = 0xFFFFFFFF ends the chain. */
#define NONE 0xFFFFFFFFu
#define N0U ((uint32_t)N0)

typedef struct {
    int fd;
    uint32_t level, split; /* buckets = N0 << level + split */
    uint32_t nb;
    uint32_t bpage[MAXB];  /* primary page of each bucket */
    uint32_t npages;
    uint32_t freelist[MAXP];
    int nfree;
    long count;
    long splits, overflow_allocs;
} LH;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x45d9f3bu; x ^= x >> 16; x *= 0x45d9f3bu; x ^= x >> 16;
    return x;
}

typedef struct {
    int n;
    uint32_t next;
    uint32_t key[CAP], val[CAP];
} Page;

static void pg_read(LH *t, uint32_t p, Page *pg) {
    unsigned char b[PSZ];
    check(pread_upto(t->fd, b, PSZ, (off_t)p * PSZ) == PSZ, "page read");
    pg->n = b[0];
    pg->next = get32(b + 4);
    for (int i = 0; i < CAP; i++) {
        pg->key[i] = get32(b + 8 + 8 * i);
        pg->val[i] = get32(b + 12 + 8 * i);
    }
}
static void pg_write(LH *t, uint32_t p, const Page *pg) {
    unsigned char b[PSZ];
    memset(b, 0, PSZ);
    b[0] = (unsigned char)pg->n;
    put32(b + 4, pg->next);
    for (int i = 0; i < CAP; i++) {
        put32(b + 8 + 8 * i, pg->key[i]);
        put32(b + 12 + 8 * i, pg->val[i]);
    }
    pwrite_all(t->fd, b, PSZ, (off_t)p * PSZ);
}
static uint32_t pg_alloc(LH *t) {
    Page e;
    memset(&e, 0, sizeof e);
    e.next = NONE;
    uint32_t p;
    if (t->nfree > 0)
        p = t->freelist[--t->nfree];
    else {
        check(t->npages < MAXP, "page limit");
        p = t->npages++;
    }
    pg_write(t, p, &e);
    return p;
}

static uint32_t bucket_of(const LH *t, uint32_t key) {
    uint32_t h = hash32(key);
    uint32_t b = h % (N0U << t->level);
    if (b < t->split)
        b = h % (N0U << (t->level + 1));
    return b;
}

static int chain_find(LH *t, uint32_t p, uint32_t key, uint32_t *val) {
    while (p != NONE) {
        Page pg;
        pg_read(t, p, &pg);
        for (int i = 0; i < pg.n; i++)
            if (pg.key[i] == key) {
                if (val)
                    *val = pg.val[i];
                return 1;
            }
        p = pg.next;
    }
    return 0;
}

static void chain_append(LH *t, uint32_t head, uint32_t key, uint32_t val) {
    uint32_t p = head;
    for (;;) {
        Page pg;
        pg_read(t, p, &pg);
        if (pg.n < CAP) {
            pg.key[pg.n] = key;
            pg.val[pg.n] = val;
            pg.n++;
            pg_write(t, p, &pg);
            return;
        }
        if (pg.next == NONE) {
            uint32_t np = pg_alloc(t);
            pg.next = np;
            pg_write(t, p, &pg);
            t->overflow_allocs++;
        }
        p = pg.next;
    }
}

static void do_split(LH *t) {
    uint32_t old = t->split;
    uint32_t nb_new = t->nb;
    check(nb_new < MAXB, "bucket limit");
    /* gather all entries of the bucket being split */
    uint32_t ks[256], vs[256];
    int n = 0;
    uint32_t p = t->bpage[old];
    uint32_t first = p;
    while (p != NONE) {
        Page pg;
        pg_read(t, p, &pg);
        for (int i = 0; i < pg.n; i++) {
            check(n < 256, "gather");
            ks[n] = pg.key[i];
            vs[n++] = pg.val[i];
        }
        uint32_t nx = pg.next;
        if (p != first) { /* recycle overflow pages */
            check(t->nfree < MAXP, "freelist");
            t->freelist[t->nfree++] = p;
        }
        p = nx;
    }
    Page empty;
    memset(&empty, 0, sizeof empty);
    empty.next = NONE;
    pg_write(t, first, &empty);
    t->bpage[nb_new] = pg_alloc(t);
    t->nb++;
    /* entries of bucket `old` go to `old` or `nb_new` according to the next-level hash */
    uint32_t next_mod = N0U << (t->level + 1);
    t->split++;
    if (t->split == (N0U << t->level)) {
        t->level++;
        t->split = 0;
    }
    for (int i = 0; i < n; i++) {
        uint32_t target = hash32(ks[i]) % next_mod;
        check(target == old || target == nb_new, "split targets");
        chain_append(t, t->bpage[target], ks[i], vs[i]);
    }
    t->splits++;
}

static int put(LH *t, uint32_t key, uint32_t val) {
    uint32_t b = bucket_of(t, key);
    /* update in place if present */
    uint32_t p = t->bpage[b];
    while (p != NONE) {
        Page pg;
        pg_read(t, p, &pg);
        for (int i = 0; i < pg.n; i++)
            if (pg.key[i] == key) {
                pg.val[i] = val;
                pg_write(t, p, &pg);
                return 0;
            }
        p = pg.next;
    }
    chain_append(t, t->bpage[b], key, val);
    t->count++;
    if (t->count * 10 > (long)t->nb * 35) /* load factor above 3.5 entries per bucket */
        do_split(t);
    return 1;
}

int main(void) {
    LH t;
    memset(&t, 0, sizeof t);
    t.fd = open("lh.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(t.fd >= 0, "open");
    t.nb = N0;
    for (uint32_t b = 0; b < N0; b++)
        t.bpage[b] = pg_alloc(&t);
    enum { KS = 6000 };
    static uint32_t model[KS];
    static unsigned char has[KS];
    long live = 0;
    for (int step = 0; step < 5000; step++) {
        uint32_t k = rndn(KS);
        uint32_t v = (uint32_t)step + 1u;
        int fresh = put(&t, k, v);
        check(fresh == !has[k], "fresh flag");
        live += fresh;
        has[k] = 1;
        model[k] = v;
        if (step % 500 == 499) { /* spot check the whole table periodically */
            for (uint32_t q = 0; q < KS; q += 13) {
                uint32_t got = 0;
                int ok = chain_find(&t, t.bpage[bucket_of(&t, q)], q, &got);
                check(ok == has[q] && (!ok || got == model[q]), "periodic lookup");
            }
        }
    }
    for (uint32_t q = 0; q < KS; q++) {
        uint32_t got = 0;
        int ok = chain_find(&t, t.bpage[bucket_of(&t, q)], q, &got);
        check(ok == has[q] && (!ok || got == model[q]), "final lookup");
    }
    check(t.count == live, "count");
    /* chain length distribution */
    int hist[8] = {0};
    long total_pages = 0;
    for (uint32_t b = 0; b < t.nb; b++) {
        int len = 0;
        uint32_t p = t.bpage[b];
        while (p != NONE) {
            Page pg;
            pg_read(&t, p, &pg);
            len++;
            p = pg.next;
        }
        total_pages += len;
        hist[len < 7 ? len : 7]++;
    }
    printf("entries=%ld buckets=%u level=%u split pointer=%u splits=%ld\n", t.count, t.nb, t.level, t.split, t.splits);
    printf("pages in chains=%ld file pages=%u free pages=%d overflow allocations=%ld\n", total_pages, t.npages,
           t.nfree, t.overflow_allocs);
    for (int l = 1; l < 8; l++)
        if (hist[l])
            printf("  chains of %d page(s): %d\n", l, hist[l]);
    printf("load factor=%.2f entries per bucket\n", (double)t.count / t.nb);
    close(t.fd);
    unlink("lh.dat");
    return 0;
}
