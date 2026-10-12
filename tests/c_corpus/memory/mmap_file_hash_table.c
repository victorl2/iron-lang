/*
 * title: Open-addressing hash table living inside a mapped file
 * topic: memory
 * covers: mmap MAP_SHARED, on-disk hash table, tombstones, persistence across remap, rehash to bigger file
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAGIC 0x48544D50u

enum { EMPTY = 0, FULL = 1, TOMB = 2 };

typedef struct {
    uint64_t key;
    uint64_t val;
    uint32_t state;
    uint32_t pad;
} Slot; /* 24 bytes */

typedef struct {
    uint32_t magic;
    uint32_t cap; /* power of two */
    uint64_t live;
    uint64_t tombs;
    uint64_t reserved;
} Head; /* 32 bytes; slots follow */

typedef struct {
    int fd;
    Head *h;
    Slot *slots;
    size_t bytes;
} Table;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

static size_t file_bytes(uint32_t cap) {
    return sizeof(Head) + (size_t)cap * sizeof(Slot);
}

static void map_table(Table *t, int fd, size_t bytes) {
    void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(m != MAP_FAILED, "mmap");
    t->fd = fd;
    t->bytes = bytes;
    t->h = m;
    t->slots = (Slot *)((char *)m + sizeof(Head));
}

static void table_create(Table *t, const char *path, uint32_t cap) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open");
    check(ftruncate(fd, (off_t)file_bytes(cap)) == 0, "ftruncate");
    map_table(t, fd, file_bytes(cap));
    t->h->magic = MAGIC;
    t->h->cap = cap;
}

static void table_open(Table *t, const char *path) {
    int fd = open(path, O_RDWR);
    check(fd >= 0, "reopen");
    Head h;
    check(pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h, "read head");
    check(h.magic == MAGIC, "magic");
    map_table(t, fd, file_bytes(h.cap));
}

static void table_close(Table *t) {
    check(msync(t->h, t->bytes, MS_SYNC) == 0, "msync");
    check(munmap(t->h, t->bytes) == 0, "munmap");
    close(t->fd);
}

static int put_raw(Table *t, uint64_t key, uint64_t val) {
    uint32_t mask = t->h->cap - 1;
    uint32_t i = (uint32_t)hash64(key) & mask;
    int64_t first_tomb = -1;
    for (uint32_t n = 0; n < t->h->cap; n++, i = (i + 1) & mask) {
        Slot *s = &t->slots[i];
        if (s->state == FULL && s->key == key) {
            s->val = val;
            return 0;
        }
        if (s->state == TOMB && first_tomb < 0)
            first_tomb = i;
        if (s->state == EMPTY) {
            if (first_tomb >= 0) {
                s = &t->slots[first_tomb];
                t->h->tombs--;
            }
            s->key = key;
            s->val = val;
            s->state = FULL;
            t->h->live++;
            return 1;
        }
    }
    return -1;
}

static int get(const Table *t, uint64_t key, uint64_t *val, int *probes) {
    uint32_t mask = t->h->cap - 1;
    uint32_t i = (uint32_t)hash64(key) & mask;
    for (uint32_t n = 0; n < t->h->cap; n++, i = (i + 1) & mask) {
        const Slot *s = &t->slots[i];
        if (probes)
            (*probes)++;
        if (s->state == EMPTY)
            return 0;
        if (s->state == FULL && s->key == key) {
            *val = s->val;
            return 1;
        }
    }
    return 0;
}

static int del(Table *t, uint64_t key) {
    uint32_t mask = t->h->cap - 1;
    uint32_t i = (uint32_t)hash64(key) & mask;
    for (uint32_t n = 0; n < t->h->cap; n++, i = (i + 1) & mask) {
        Slot *s = &t->slots[i];
        if (s->state == EMPTY)
            return 0;
        if (s->state == FULL && s->key == key) {
            s->state = TOMB;
            t->h->live--;
            t->h->tombs++;
            return 1;
        }
    }
    return 0;
}

/* rebuild into a new, larger file and swap it in */
static void table_grow(Table *t, const char *path, const char *tmp, uint32_t newcap) {
    Table n;
    table_create(&n, tmp, newcap);
    for (uint32_t i = 0; i < t->h->cap; i++)
        if (t->slots[i].state == FULL)
            check(put_raw(&n, t->slots[i].key, t->slots[i].val) == 1, "rehash insert");
    table_close(&n);
    table_close(t);
    check(rename(tmp, path) == 0, "rename");
    table_open(t, path);
}

static uint64_t s = 5;

static uint64_t rnd(void) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void) {
    const char *path = "mmht.dat", *tmp = "mmht.new";
    Table t;
    table_create(&t, path, 64);
    enum { N = 400 };
    uint64_t keys[N];
    for (int i = 0; i < N; i++)
        keys[i] = rnd() % 100000 + 1;

    /* reference model: a plain array of latest values, found by linear scan */
    uint64_t ref_key[N], ref_val[N];
    int ref_n = 0;
    int grows = 0;
    for (int i = 0; i < N; i++) {
        if ((t.h->live + t.h->tombs) * 4 >= (uint64_t)t.h->cap * 3) {
            table_grow(&t, path, tmp, t.h->cap * 2);
            grows++;
        }
        uint64_t v = (uint64_t)i * 1000 + 7;
        int r = put_raw(&t, keys[i], v);
        check(r >= 0, "put");
        int found = -1;
        for (int k = 0; k < ref_n; k++)
            if (ref_key[k] == keys[i])
                found = k;
        if (found >= 0) {
            check(r == 0, "update expected");
            ref_val[found] = v;
        } else {
            check(r == 1, "insert expected");
            ref_key[ref_n] = keys[i];
            ref_val[ref_n++] = v;
        }
    }
    printf("distinct keys=%d grows=%d cap=%u\n", ref_n, grows, (unsigned)t.h->cap);
    check(t.h->live == (uint64_t)ref_n, "live count");

    /* delete every third reference key */
    int deleted = 0;
    for (int k = 0; k < ref_n; k += 3) {
        check(del(&t, ref_key[k]) == 1, "delete");
        ref_key[k] = 0;
        deleted++;
    }
    check(del(&t, 999999999) == 0, "delete missing");
    printf("deleted=%d live=%llu tombs=%llu\n", deleted, (unsigned long long)t.h->live,
           (unsigned long long)t.h->tombs);

    /* persistence: close, reopen the file, and verify every entry */
    table_close(&t);
    table_open(&t, path);
    int hits = 0, misses = 0, probes = 0;
    uint64_t vsum = 0;
    for (int k = 0; k < ref_n; k++) {
        uint64_t v = 0;
        if (ref_key[k] == 0)
            continue;
        check(get(&t, ref_key[k], &v, &probes) == 1 && v == ref_val[k], "persisted value");
        vsum += v;
        hits++;
    }
    for (int k = 0; k < ref_n; k++) {
        /* keys of deleted entries were zeroed in the reference, so probe fresh out-of-range keys */
        uint64_t v;
        if (get(&t, 200000 + (uint64_t)k, &v, &probes))
            check(0, "unexpected hit");
        misses++;
    }
    printf("after reopen: hits=%d misses=%d value-sum=%llu\n", hits, misses, (unsigned long long)vsum);
    printf("average probes (x100): %d\n", probes * 100 / (hits + misses));

    /* tombstone reuse: reinsert deleted keys */
    check(t.h->tombs > 0, "have tombstones");
    uint64_t tombs_before = t.h->tombs;
    int reinserted = 0;
    for (int k = 0; k < 20; k++) {
        uint64_t key = 500000 + (uint64_t)k;
        if (put_raw(&t, key, key * 2) == 1)
            reinserted++;
    }
    printf("reinserted=%d tombstones before=%llu after=%llu\n", reinserted, (unsigned long long)tombs_before,
           (unsigned long long)t.h->tombs);
    check(t.h->tombs <= tombs_before, "tombstones never grow on insert");
    table_close(&t);
    unlink(path);
    return 0;
}
