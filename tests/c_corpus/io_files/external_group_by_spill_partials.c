/*
 * title: External hash aggregation that spills partial aggregates to partition files
 * topic: io_files
 * covers: group-by with bounded memory, partial aggregate spill, hash partition files, re-aggregation per partition, min/max/sum/count, streaming input file
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

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

enum { MEMGROUPS = 48, NPART = 4, NKEYS = 700, NROWS = 12000 };

typedef struct {
    uint32_t key;
    int64_t sum;
    uint32_t cnt;
    uint32_t min, max;
} Agg;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

enum { AGGSZ = 24 };
static void enc(unsigned char *p, const Agg *a) {
    put32(p, a->key);
    put64(p + 4, (uint64_t)a->sum);
    put32(p + 12, a->cnt);
    put32(p + 16, a->min);
    put32(p + 20, a->max);
}
static Agg dec(const unsigned char *p) {
    Agg a;
    a.key = get32(p);
    a.sum = (int64_t)get64(p + 4);
    a.cnt = get32(p + 12);
    a.min = get32(p + 16);
    a.max = get32(p + 20);
    return a;
}

static void merge_into(Agg *dst, const Agg *src) {
    dst->sum += src->sum;
    dst->cnt += src->cnt;
    if (src->min < dst->min) dst->min = src->min;
    if (src->max > dst->max) dst->max = src->max;
}

/* Small open-addressing table holding at most MEMGROUPS groups. */
typedef struct {
    Agg slot[MEMGROUPS * 2];
    unsigned char used[MEMGROUPS * 2];
    int n;
} Table;

static Agg *tab_find(Table *t, uint32_t key, int create) {
    uint32_t i = hash32(key) % (MEMGROUPS * 2);
    while (t->used[i]) {
        if (t->slot[i].key == key)
            return &t->slot[i];
        i = (i + 1) % (MEMGROUPS * 2);
    }
    if (!create)
        return NULL;
    t->used[i] = 1;
    t->slot[i].key = key;
    t->slot[i].sum = 0;
    t->slot[i].cnt = 0;
    t->slot[i].min = 0xFFFFFFFFu;
    t->slot[i].max = 0;
    t->n++;
    return &t->slot[i];
}

static long spilled_aggs, spill_events;
static int part_fd[NPART];

static void spill_table(Table *t) {
    for (int i = 0; i < MEMGROUPS * 2; i++)
        if (t->used[i]) {
            unsigned char rec[AGGSZ];
            enc(rec, &t->slot[i]);
            write_all(part_fd[hash32(t->slot[i].key ^ 0x9999u) % NPART], rec, AGGSZ);
            spilled_aggs++;
        }
    memset(t, 0, sizeof *t);
    spill_events++;
}

static int cmp_agg(const void *a, const void *b) {
    uint32_t x = ((const Agg *)a)->key, y = ((const Agg *)b)->key;
    return x < y ? -1 : (x > y);
}

int main(void) {
    /* Input file of (key, value) rows with a skewed key distribution. */
    static uint32_t keys[NROWS], vals[NROWS];
    unsigned char *raw = malloc((size_t)NROWS * 8);
    check(raw != NULL, "raw");
    for (int i = 0; i < NROWS; i++) {
        uint32_t a = rndn(NKEYS), b = rndn(NKEYS);
        keys[i] = a < b ? a : b; /* skew toward small keys */
        vals[i] = rndn(10000);
        put32(raw + 8 * i, keys[i]);
        put32(raw + 8 * i + 4, vals[i]);
    }
    spit("rows.dat", raw, (size_t)NROWS * 8);
    free(raw);
    /* Reference: full in-memory aggregation. */
    static Agg ref[NKEYS];
    for (int k = 0; k < NKEYS; k++) {
        ref[k].key = (uint32_t)k;
        ref[k].min = 0xFFFFFFFFu;
    }
    for (int i = 0; i < NROWS; i++)
        merge_into(&ref[keys[i]], &(Agg){keys[i], vals[i], 1, vals[i], vals[i]});

    for (int p = 0; p < NPART; p++) {
        char nm[24];
        snprintf(nm, sizeof nm, "part%d.spill", p);
        part_fd[p] = open(nm, O_RDWR | O_CREAT | O_TRUNC, 0644);
        check(part_fd[p] >= 0, "part open");
    }
    /* Pass 1: stream the file through a bounded table; spill everything when it overflows. */
    int fd = open("rows.dat", O_RDONLY);
    static Table tab;
    memset(&tab, 0, sizeof tab);
    unsigned char chunk[512 * 8];
    off_t off = 0;
    size_t got;
    while ((got = pread_upto(fd, chunk, sizeof chunk, off)) > 0) {
        off += (off_t)got;
        for (size_t i = 0; i + 8 <= got; i += 8) {
            uint32_t k = get32(chunk + i), v = get32(chunk + i + 4);
            Agg *a = tab_find(&tab, k, 0);
            if (!a) {
                if (tab.n >= MEMGROUPS)
                    spill_table(&tab);
                a = tab_find(&tab, k, 1);
            }
            Agg row = {k, v, 1, v, v};
            merge_into(a, &row);
        }
    }
    close(fd);
    spill_table(&tab); /* final flush */
    printf("rows=%d distinct keys=%d memory groups=%d spill events=%ld partial aggregates spilled=%ld\n", NROWS,
           NKEYS, MEMGROUPS, spill_events, spilled_aggs);

    /* Pass 2: per partition, combine partials (a key appears in exactly one partition). */
    static Agg out[NKEYS];
    int nout = 0;
    long partial_in[NPART];
    for (int p = 0; p < NPART; p++) {
        off_t sz = lseek(part_fd[p], 0, SEEK_END);
        unsigned char *b = malloc((size_t)sz + 1);
        check(pread_upto(part_fd[p], b, (size_t)sz, 0) == (size_t)sz, "part read");
        partial_in[p] = (long)(sz / AGGSZ);
        Agg *big = malloc(sizeof(Agg) * NKEYS);
        int nb = 0;
        static int idx_of[NKEYS];
        for (int i = 0; i < NKEYS; i++)
            idx_of[i] = -1;
        for (off_t i = 0; i + AGGSZ <= sz; i += AGGSZ) {
            Agg a = dec(b + i);
            if (idx_of[a.key] < 0) {
                idx_of[a.key] = nb;
                big[nb++] = a;
            } else
                merge_into(&big[idx_of[a.key]], &a);
        }
        memcpy(out + nout, big, sizeof(Agg) * (size_t)nb);
        nout += nb;
        free(big);
        free(b);
        close(part_fd[p]);
        char nm[24];
        snprintf(nm, sizeof nm, "part%d.spill", p);
        unlink(nm);
    }
    qsort(out, (size_t)nout, sizeof out[0], cmp_agg);
    int expect_groups = 0;
    for (int k = 0; k < NKEYS; k++)
        expect_groups += ref[k].cnt > 0;
    check(nout == expect_groups, "group count");
    int64_t total = 0;
    uint32_t tot_cnt = 0;
    for (int i = 0; i < nout; i++) {
        const Agg *r = &ref[out[i].key];
        check(out[i].sum == r->sum && out[i].cnt == r->cnt && out[i].min == r->min && out[i].max == r->max,
              "aggregate equals in-memory reference");
        total += out[i].sum;
        tot_cnt += out[i].cnt;
    }
    check(tot_cnt == NROWS, "row count preserved");
    printf("partition partial counts: %ld %ld %ld %ld\n", partial_in[0], partial_in[1], partial_in[2], partial_in[3]);
    printf("groups=%d total sum=%lld\n", nout, (long long)total);
    for (int i = 0; i < 3; i++)
        printf("key %u: count=%u sum=%lld min=%u max=%u avg=%lld\n", out[i].key, out[i].cnt, (long long)out[i].sum,
               out[i].min, out[i].max, (long long)(out[i].sum / out[i].cnt));
    unlink("rows.dat");
    return 0;
}
