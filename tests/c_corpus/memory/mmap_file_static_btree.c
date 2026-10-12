/*
 * title: Bulk-loaded B+ tree stored in fixed pages of a mapped file
 * topic: memory
 * covers: page-structured file, mmap, bulk loading, page index links, point lookup, leaf chain range scan
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FANOUT 15
#define LEAFCAP 14
#define PAGE 256u

typedef struct {
    uint16_t is_leaf;
    uint16_t n;
    uint32_t next; /* leaf chain: next leaf page number, 0 = none */
    union {
        struct {
            uint32_t keys[LEAFCAP];
            uint32_t vals[LEAFCAP];
        } leaf;
        struct {
            uint32_t keys[FANOUT - 1]; /* keys[i] = smallest key under child[i+1] */
            uint32_t child[FANOUT];
        } inner;
    } u;
} Page;

typedef struct {
    uint32_t magic;
    uint32_t root;
    uint32_t height;
    uint32_t npages;
    uint32_t nkeys;
} Meta; /* lives in page 0 */

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t rs = 77;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static unsigned char *g_map;

static Page *page_at(uint32_t no) {
    return (Page *)(g_map + (size_t)no * PAGE);
}

static size_t pages_needed(uint32_t nkeys) {
    size_t total = 1;
    size_t level = (nkeys + LEAFCAP - 1) / LEAFCAP;
    total += level;
    while (level > 1) {
        level = (level + FANOUT - 1) / FANOUT;
        total += level;
    }
    return total;
}

/* Build bottom-up. Returns metadata. Keys must be sorted and unique. */
static Meta build(const uint32_t *keys, uint32_t n) {
    uint32_t next_page = 1;
    uint32_t level_first = next_page, level_count = 0;
    uint32_t low[4096]; /* smallest key of each node at the current level */
    /* leaves */
    for (uint32_t i = 0; i < n; i += LEAFCAP) {
        Page *p = page_at(next_page);
        memset(p, 0, PAGE);
        p->is_leaf = 1;
        p->n = (uint16_t)((n - i) < LEAFCAP ? (n - i) : LEAFCAP);
        for (uint16_t k = 0; k < p->n; k++) {
            p->u.leaf.keys[k] = keys[i + k];
            p->u.leaf.vals[k] = keys[i + k] * 3u + 1u;
        }
        low[level_count] = keys[i];
        level_count++;
        next_page++;
        p->next = (i + LEAFCAP < n) ? next_page : 0;
    }
    uint32_t height = 1;
    while (level_count > 1) {
        uint32_t new_first = next_page, new_count = 0;
        uint32_t newlow[4096];
        for (uint32_t i = 0; i < level_count; i += FANOUT) {
            Page *p = page_at(next_page);
            memset(p, 0, PAGE);
            uint32_t c = (level_count - i) < FANOUT ? (level_count - i) : FANOUT;
            p->is_leaf = 0;
            p->n = (uint16_t)c; /* number of children */
            for (uint32_t k = 0; k < c; k++) {
                p->u.inner.child[k] = level_first + i + k;
                if (k > 0)
                    p->u.inner.keys[k - 1] = low[i + k];
            }
            newlow[new_count++] = low[i];
            next_page++;
        }
        memcpy(low, newlow, sizeof(uint32_t) * new_count);
        level_first = new_first;
        level_count = new_count;
        height++;
    }
    Meta m = {0x42545245u, level_first, height, next_page, n};
    return m;
}

static int lookup(const Meta *m, uint32_t key, uint32_t *val, int *touched) {
    uint32_t no = m->root;
    for (;;) {
        const Page *p = page_at(no);
        (*touched)++;
        if (p->is_leaf) {
            int lo = 0, hi = p->n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                uint32_t k = p->u.leaf.keys[mid];
                if (k == key) {
                    *val = p->u.leaf.vals[mid];
                    return 1;
                }
                if (k < key)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
            return 0;
        }
        int c = 0;
        while (c < p->n - 1 && key >= p->u.inner.keys[c])
            c++;
        no = p->u.inner.child[c];
    }
}

/* range scan [lo, hi] via the leaf chain */
static uint32_t range_count(const Meta *m, uint32_t lo, uint32_t hi, uint64_t *sum) {
    uint32_t no = m->root;
    for (;;) {
        const Page *p = page_at(no);
        if (p->is_leaf)
            break;
        int c = 0;
        while (c < p->n - 1 && lo >= p->u.inner.keys[c])
            c++;
        no = p->u.inner.child[c];
    }
    uint32_t cnt = 0;
    *sum = 0;
    while (no) {
        const Page *p = page_at(no);
        for (int k = 0; k < p->n; k++) {
            uint32_t key = p->u.leaf.keys[k];
            if (key > hi)
                return cnt;
            if (key >= lo) {
                cnt++;
                *sum += key;
            }
        }
        no = p->next;
    }
    return cnt;
}

int main(void) {
    check(sizeof(Page) <= PAGE, "page fits");
    enum { N = 3000 };
    static uint32_t keys[N];
    for (int i = 0; i < N; i++)
        keys[i] = rnd() % 200000;
    qsort(keys, N, sizeof keys[0], cmp_u32);
    uint32_t n = 0;
    for (int i = 0; i < N; i++)
        if (n == 0 || keys[n - 1] != keys[i])
            keys[n++] = keys[i];
    printf("unique keys: %u\n", (unsigned)n);

    char path[] = "mmbt_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t npages = pages_needed(n);
    size_t bytes = npages * PAGE;
    check(ftruncate(fd, (off_t)bytes) == 0, "ftruncate");
    g_map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(g_map != MAP_FAILED, "mmap");
    Meta m = build(keys, n);
    check(m.npages == npages, "page count prediction");
    memset(g_map, 0, PAGE);
    memcpy(g_map, &m, sizeof m);
    check(msync(g_map, bytes, MS_SYNC) == 0, "msync");
    printf("pages=%u height=%u root=%u\n", (unsigned)m.npages, (unsigned)m.height, (unsigned)m.root);
    check(munmap(g_map, bytes) == 0, "munmap");

    /* reopen read-only and read the meta page back from the file */
    g_map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    check(g_map != MAP_FAILED, "mmap ro");
    Meta rm;
    memcpy(&rm, g_map, sizeof rm);
    check(rm.magic == 0x42545245u && rm.nkeys == n, "meta");

    /* every key found with the right value; touched pages equal the height */
    int touched = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        int before = touched;
        check(lookup(&rm, keys[i], &v, &touched) == 1, "found");
        check(v == keys[i] * 3u + 1u, "value");
        check(touched - before == (int)rm.height, "path length equals height");
    }
    printf("all keys found, pages touched per lookup: %u\n", (unsigned)rm.height);

    /* absent keys: every gap, verified against binary search */
    int absent = 0;
    for (uint32_t probe = 0; probe < 2000; probe++) {
        uint32_t k = rnd() % 200000;
        uint32_t v;
        int t = 0;
        int got = lookup(&rm, k, &v, &t);
        int expect = bsearch(&k, keys, n, sizeof keys[0], cmp_u32) != NULL;
        check(got == expect, "membership agrees with bsearch");
        if (!got)
            absent++;
    }
    printf("absent probes among 2000: %d\n", absent);

    /* range scans against a linear count */
    uint32_t ranges[][2] = {{0, 1000}, {5000, 5100}, {99000, 101000}, {0, 199999}, {150000, 150000}, {199990, 300000}};
    for (size_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
        uint64_t sum = 0, want_sum = 0;
        uint32_t want = 0;
        for (uint32_t i = 0; i < n; i++)
            if (keys[i] >= ranges[r][0] && keys[i] <= ranges[r][1]) {
                want++;
                want_sum += keys[i];
            }
        uint32_t got = range_count(&rm, ranges[r][0], ranges[r][1], &sum);
        check(got == want && sum == want_sum, "range");
        printf("range [%u,%u]: count=%u sum=%llu\n", (unsigned)ranges[r][0], (unsigned)ranges[r][1], (unsigned)got,
               (unsigned long long)sum);
    }
    munmap(g_map, bytes);
    close(fd);
    unlink(path);
    return 0;
}
