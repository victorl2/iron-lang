/*
 * title: Disk-based B-tree index with one node per file page
 * topic: io_files
 * covers: B-tree insert with proactive split, page (de)serialization, header page, reopen persistence, invariants check, range scan
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
enum { PSZ = 128, T = 4, MAXK = 2 * T - 1 };

typedef struct {
    int leaf, n;
    uint32_t key[MAXK], val[MAXK];
    uint32_t child[MAXK + 1];
} Node;

typedef struct {
    int fd;
    uint32_t root, npages;
    long reads, writes;
} Tree;

static void node_encode(const Node *nd, unsigned char *pg) {
    memset(pg, 0, PSZ);
    pg[0] = (unsigned char)nd->leaf;
    pg[1] = (unsigned char)nd->n;
    for (int i = 0; i < MAXK; i++) {
        put32(pg + 4 + 4 * i, nd->key[i]);
        put32(pg + 32 + 4 * i, nd->val[i]);
    }
    for (int i = 0; i <= MAXK; i++)
        put32(pg + 60 + 4 * i, nd->child[i]);
}
static void node_decode(Node *nd, const unsigned char *pg) {
    nd->leaf = pg[0];
    nd->n = pg[1];
    for (int i = 0; i < MAXK; i++) {
        nd->key[i] = get32(pg + 4 + 4 * i);
        nd->val[i] = get32(pg + 32 + 4 * i);
    }
    for (int i = 0; i <= MAXK; i++)
        nd->child[i] = get32(pg + 60 + 4 * i);
}

static void load(Tree *t, uint32_t pno, Node *nd) {
    unsigned char pg[PSZ];
    check(pread_upto(t->fd, pg, PSZ, (off_t)pno * PSZ) == PSZ, "load page");
    node_decode(nd, pg);
    t->reads++;
}
static void store(Tree *t, uint32_t pno, const Node *nd) {
    unsigned char pg[PSZ];
    node_encode(nd, pg);
    pwrite_all(t->fd, pg, PSZ, (off_t)pno * PSZ);
    t->writes++;
}
static void write_header(Tree *t) {
    unsigned char pg[PSZ];
    memset(pg, 0, PSZ);
    memcpy(pg, "BTRE", 4);
    put32(pg + 4, t->root);
    put32(pg + 8, t->npages);
    pwrite_all(t->fd, pg, PSZ, 0);
}
static uint32_t alloc_page(Tree *t) { return t->npages++; }

static void tree_create(Tree *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(t->fd >= 0, "create");
    t->npages = 1;
    Node r;
    memset(&r, 0, sizeof r);
    r.leaf = 1;
    t->root = alloc_page(t);
    store(t, t->root, &r);
    write_header(t);
}
static void tree_open(Tree *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->fd = open(path, O_RDWR);
    check(t->fd >= 0, "reopen");
    unsigned char pg[PSZ];
    check(pread_upto(t->fd, pg, PSZ, 0) == PSZ && memcmp(pg, "BTRE", 4) == 0, "header");
    t->root = get32(pg + 4);
    t->npages = get32(pg + 8);
}

static void split_child(Tree *t, Node *parent, uint32_t ppno, int i) {
    uint32_t cpno = parent->child[i];
    Node c, z;
    load(t, cpno, &c);
    memset(&z, 0, sizeof z);
    z.leaf = c.leaf;
    z.n = T - 1;
    for (int j = 0; j < T - 1; j++) {
        z.key[j] = c.key[j + T];
        z.val[j] = c.val[j + T];
    }
    if (!c.leaf)
        for (int j = 0; j < T; j++)
            z.child[j] = c.child[j + T];
    uint32_t zpno = alloc_page(t);
    for (int j = parent->n; j > i; j--) {
        parent->key[j] = parent->key[j - 1];
        parent->val[j] = parent->val[j - 1];
        parent->child[j + 1] = parent->child[j];
    }
    parent->key[i] = c.key[T - 1];
    parent->val[i] = c.val[T - 1];
    parent->child[i + 1] = zpno;
    parent->n++;
    c.n = T - 1;
    store(t, cpno, &c);
    store(t, zpno, &z);
    store(t, ppno, parent);
}

static int insert_nonfull(Tree *t, uint32_t pno, uint32_t key, uint32_t val) {
    Node x;
    load(t, pno, &x);
    int i = x.n - 1;
    for (int j = 0; j < x.n; j++)
        if (x.key[j] == key) { /* update in place */
            x.val[j] = val;
            store(t, pno, &x);
            return 0;
        }
    if (x.leaf) {
        while (i >= 0 && key < x.key[i]) {
            x.key[i + 1] = x.key[i];
            x.val[i + 1] = x.val[i];
            i--;
        }
        x.key[i + 1] = key;
        x.val[i + 1] = val;
        x.n++;
        store(t, pno, &x);
        return 1;
    }
    while (i >= 0 && key < x.key[i])
        i--;
    i++;
    Node c;
    load(t, x.child[i], &c);
    if (c.n == MAXK) {
        split_child(t, &x, pno, i);
        if (key == x.key[i]) {
            x.val[i] = val;
            store(t, pno, &x);
            return 0;
        }
        if (key > x.key[i])
            i++;
    }
    return insert_nonfull(t, x.child[i], key, val);
}

static int tree_put(Tree *t, uint32_t key, uint32_t val) {
    Node r;
    load(t, t->root, &r);
    if (r.n == MAXK) {
        Node s;
        memset(&s, 0, sizeof s);
        s.child[0] = t->root;
        uint32_t spno = alloc_page(t);
        store(t, spno, &s);
        t->root = spno;
        split_child(t, &s, spno, 0);
        write_header(t);
    }
    int added = insert_nonfull(t, t->root, key, val);
    write_header(t);
    return added;
}

static int tree_get(Tree *t, uint32_t key, uint32_t *val) {
    uint32_t pno = t->root;
    for (;;) {
        Node x;
        load(t, pno, &x);
        int i = 0;
        while (i < x.n && key > x.key[i])
            i++;
        if (i < x.n && x.key[i] == key) {
            *val = x.val[i];
            return 1;
        }
        if (x.leaf)
            return 0;
        pno = x.child[i];
    }
}

/* In-order walk over [lo, hi]; also validates ordering, occupancy, and uniform depth. */
static int walk(Tree *t, uint32_t pno, int depth, int *leaf_depth, uint32_t lo, uint32_t hi, uint32_t qlo,
                uint32_t qhi, long *count, long *inrange, uint32_t *prev, int is_root) {
    Node x;
    load(t, pno, &x);
    check(is_root || x.n >= T - 1, "min occupancy");
    check(x.n <= MAXK, "max occupancy");
    int nodes = 1;
    for (int i = 0; i <= x.n; i++) {
        if (!x.leaf)
            nodes += walk(t, x.child[i], depth + 1, leaf_depth, lo, hi, qlo, qhi, count, inrange, prev, 0);
        else if (*leaf_depth < 0)
            *leaf_depth = depth;
        else if (i == 0)
            check(*leaf_depth == depth, "uniform leaf depth");
        if (i < x.n) {
            check(x.key[i] >= lo && x.key[i] <= hi, "key within bounds");
            check(*count == 0 || x.key[i] > *prev, "in-order strictly increasing");
            *prev = x.key[i];
            (*count)++;
            if (x.key[i] >= qlo && x.key[i] <= qhi)
                (*inrange)++;
        }
    }
    return nodes;
}

int main(void) {
    Tree t;
    tree_create(&t, "index.bt");
    enum { N = 1500 };
    static uint32_t keys[N], vals[N];
    int distinct = 0;
    for (int i = 0; i < N; i++) {
        keys[i] = rndn(2000);
        vals[i] = (uint32_t)i * 7u + 1u;
        distinct += tree_put(&t, keys[i], vals[i]);
    }
    /* Later duplicates overwrite: expected value is that of the last insertion of the key. */
    static uint32_t expect[2000];
    static unsigned char present[2000];
    for (int i = 0; i < N; i++) {
        expect[keys[i]] = vals[i];
        present[keys[i]] = 1;
    }
    close(t.fd);
    tree_open(&t, "index.bt"); /* everything must survive a reopen */
    int found = 0;
    for (uint32_t k = 0; k < 2000; k++) {
        uint32_t v = 0;
        int ok = tree_get(&t, k, &v);
        check(ok == present[k], "presence");
        if (ok) {
            check(v == expect[k], "value");
            found++;
        }
    }
    check(found == distinct, "distinct count");
    int leaf_depth = -1;
    long count = 0, inrange = 0;
    uint32_t prev = 0;
    int nodes = walk(&t, t.root, 0, &leaf_depth, 0, 0xFFFFFFFFu, 500, 599, &count, &inrange, &prev, 1);
    long want = 0;
    for (uint32_t k = 500; k <= 599; k++)
        want += present[k];
    check(count == distinct && inrange == want, "walk counts");
    printf("keys inserted=%d distinct=%d found=%d\n", N, distinct, found);
    printf("nodes=%d file pages=%u height=%d\n", nodes, t.npages, leaf_depth + 1);
    printf("keys in [500,599]=%ld\n", inrange);
    long r0 = t.reads;
    uint32_t v;
    for (uint32_t k = 0; k < 2000; k += 5)
        tree_get(&t, k, &v);
    printf("page reads for 400 lookups (after walk)=%ld\n", t.reads - r0);
    close(t.fd);
    unlink("index.bt");
    return 0;
}
