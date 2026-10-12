/*
 * title: Type-erased hash map with key callbacks
 * topic: data_structures
 * covers: void pointer keys, hash and equality callbacks, key ownership, chaining, rehash
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 314159u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Ent { void *key; void *val; uint32_t h; struct Ent *next; } Ent;
typedef struct {
    Ent **b;
    size_t nb, n, vsz;
    uint32_t (*hash)(const void *);
    int (*eq)(const void *, const void *);
    void *(*kdup)(const void *);   /* returns owned key copy */
    void (*kfree)(void *);
    size_t rehashes;
} Map;

static void map_init(Map *m, size_t vsz, uint32_t (*hash)(const void *), int (*eq)(const void *, const void *),
                     void *(*kdup)(const void *), void (*kfree)(void *)) {
    m->nb = 4; m->n = 0; m->vsz = vsz; m->rehashes = 0;
    m->b = calloc(m->nb, sizeof(Ent *));
    CHECK(m->b);
    m->hash = hash; m->eq = eq; m->kdup = kdup; m->kfree = kfree;
}
static void map_grow(Map *m) {
    size_t nn = m->nb * 2;
    Ent **nb = calloc(nn, sizeof(Ent *));
    CHECK(nb);
    for (size_t i = 0; i < m->nb; i++) {
        Ent *e = m->b[i];
        while (e) {
            Ent *nx = e->next;
            size_t j = e->h % nn;
            e->next = nb[j];
            nb[j] = e;
            e = nx;
        }
    }
    free(m->b);
    m->b = nb; m->nb = nn; m->rehashes++;
}
static void *map_find(Map *m, const void *key) {
    uint32_t h = m->hash(key);
    for (Ent *e = m->b[h % m->nb]; e; e = e->next)
        if (e->h == h && m->eq(e->key, key)) return e->val;
    return NULL;
}
/* returns 1 if a new key was added, 0 if an existing value was overwritten */
static int map_put(Map *m, const void *key, const void *val) {
    void *slot = map_find(m, key);
    if (slot) { memcpy(slot, val, m->vsz); return 0; }
    if (m->n >= m->nb * 2) map_grow(m);
    Ent *e = malloc(sizeof *e);
    CHECK(e);
    e->key = m->kdup(key);
    e->val = malloc(m->vsz);
    CHECK(e->val);
    memcpy(e->val, val, m->vsz);
    e->h = m->hash(key);
    size_t j = e->h % m->nb;
    e->next = m->b[j];
    m->b[j] = e;
    m->n++;
    return 1;
}
static int map_del(Map *m, const void *key) {
    uint32_t h = m->hash(key);
    Ent **pp = &m->b[h % m->nb];
    while (*pp) {
        Ent *e = *pp;
        if (e->h == h && m->eq(e->key, key)) {
            *pp = e->next;
            m->kfree(e->key);
            free(e->val);
            free(e);
            m->n--;
            return 1;
        }
        pp = &e->next;
    }
    return 0;
}
static void map_free(Map *m) {
    for (size_t i = 0; i < m->nb; i++) {
        Ent *e = m->b[i];
        while (e) {
            Ent *nx = e->next;
            m->kfree(e->key);
            free(e->val);
            free(e);
            e = nx;
        }
    }
    free(m->b);
}
static void map_each(Map *m, void (*fn)(const void *k, void *v, void *ctx), void *ctx) {
    for (size_t i = 0; i < m->nb; i++)
        for (Ent *e = m->b[i]; e; e = e->next) fn(e->key, e->val, ctx);
}

static uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
/* key kind 1: int */
static uint32_t h_int(const void *k) { int x; memcpy(&x, k, sizeof x); return mix((uint32_t)x); }
static int e_int(const void *a, const void *b) { return memcmp(a, b, sizeof(int)) == 0; }
static void *d_int(const void *k) { void *p = malloc(sizeof(int)); CHECK(p); memcpy(p, k, sizeof(int)); return p; }
/* key kind 2: C string */
static uint32_t h_str(const void *k) {
    const unsigned char *s = k;
    uint32_t h = 2166136261u;
    while (*s) { h ^= *s++; h *= 16777619u; }
    return h;
}
static int e_str(const void *a, const void *b) { return strcmp(a, b) == 0; }
static void *d_str(const void *k) {
    size_t n = strlen(k) + 1;
    char *p = malloc(n);
    CHECK(p);
    memcpy(p, k, n);
    return p;
}
/* key kind 3: point struct (compared by fields, padding ignored) */
typedef struct { short x, y; } Pt;
static uint32_t h_pt(const void *k) { const Pt *p = k; return mix((uint32_t)(p->x & 0xffff) * 65537u + (uint32_t)(p->y & 0xffff)); }
static int e_pt(const void *a, const void *b) { const Pt *p = a, *q = b; return p->x == q->x && p->y == q->y; }
static void *d_pt(const void *k) { Pt *p = malloc(sizeof *p); CHECK(p); *p = *(const Pt *)k; return p; }
static void f_free(void *k) { free(k); }

static void sum_vals(const void *k, void *v, void *ctx) { (void)k; *(long *)ctx += *(int *)v; }

#define N 300
int main(void) {
    /* int -> int */
    {
        Map m;
        map_init(&m, sizeof(int), h_int, e_int, d_int, f_free);
        int mk[N], mv[N], mn = 0;
        long adds = 0, overwrites = 0, dels = 0;
        for (int step = 0; step < 6000; step++) {
            int k = (int)(rnd() % 500) - 250;
            int v = (int)(rnd() % 10000);
            unsigned op = rnd() % 10;
            int idx = -1;
            for (int i = 0; i < mn; i++) if (mk[i] == k) { idx = i; break; }
            if (op < 6) {
                if (idx < 0 && mn >= N) continue;
                int added = map_put(&m, &k, &v);
                CHECK(added == (idx < 0));
                if (idx < 0) { mk[mn] = k; mv[mn] = v; mn++; adds++; }
                else { mv[idx] = v; overwrites++; }
            } else if (op < 8) {
                int r = map_del(&m, &k);
                CHECK(r == (idx >= 0));
                if (idx >= 0) { mk[idx] = mk[mn - 1]; mv[idx] = mv[mn - 1]; mn--; dels++; }
            } else {
                int *p = map_find(&m, &k);
                CHECK((p != NULL) == (idx >= 0));
                if (p) CHECK(*p == mv[idx]);
            }
            CHECK((int)m.n == mn);
        }
        long s = 0, ms = 0;
        map_each(&m, sum_vals, &s);
        for (int i = 0; i < mn; i++) ms += mv[i];
        CHECK(s == ms);
        printf("int map: size=%zu adds=%ld overwrites=%ld deletes=%ld sum=%ld buckets=%zu\n", m.n, adds, overwrites, dels, s, m.nb);
        map_free(&m);
    }
    /* string -> int, keys are built in a scratch buffer and must be copied */
    {
        Map m;
        map_init(&m, sizeof(int), h_str, e_str, d_str, f_free);
        static char keys[N][12];
        int mv[N], mn = 0;
        for (int step = 0; step < 4000; step++) {
            char buf[12];
            unsigned r = rnd() % 400;
            snprintf(buf, sizeof buf, "k%u", r * 7u);
            int idx = -1;
            for (int i = 0; i < mn; i++) if (strcmp(keys[i], buf) == 0) { idx = i; break; }
            int v = step;
            if (rnd() % 3) {
                if (idx < 0 && mn >= N) continue;
                map_put(&m, buf, &v);
                if (idx < 0) { strcpy(keys[mn], buf); mv[mn++] = v; } else mv[idx] = v;
            } else {
                int r2 = map_del(&m, buf);
                CHECK(r2 == (idx >= 0));
                if (idx >= 0) { if (idx != mn - 1) strcpy(keys[idx], keys[mn - 1]); mv[idx] = mv[mn - 1]; mn--; }
            }
            memset(buf, 'x', sizeof buf - 1); /* scribble on the caller's buffer */
            buf[sizeof buf - 1] = 0;
            CHECK((int)m.n == mn);
        }
        for (int i = 0; i < mn; i++) {
            int *p = map_find(&m, keys[i]);
            CHECK(p && *p == mv[i]);
        }
        CHECK(map_find(&m, "nope") == NULL);
        printf("string map: size=%zu rehashes=%zu\n", m.n, m.rehashes);
        map_free(&m);
    }
    /* point -> int (grid cells) */
    {
        Map m;
        map_init(&m, sizeof(int), h_pt, e_pt, d_pt, f_free);
        static int grid[41][41];
        for (int step = 0; step < 3000; step++) {
            Pt p;
            memset(&p, 0, sizeof p);
            p.x = (short)((int)(rnd() % 41) - 20);
            p.y = (short)((int)(rnd() % 41) - 20);
            int cnt = 1;
            int *e = map_find(&m, &p);
            if (e) cnt = *e + 1;
            map_put(&m, &p, &cnt);
            grid[p.x + 20][p.y + 20]++;
        }
        int distinct = 0, maxc = 0;
        for (int x = 0; x < 41; x++)
            for (int y = 0; y < 41; y++) {
                Pt p = { (short)(x - 20), (short)(y - 20) };
                int *e = map_find(&m, &p);
                CHECK((e != NULL) == (grid[x][y] > 0));
                if (e) { CHECK(*e == grid[x][y]); distinct++; if (*e > maxc) maxc = *e; }
            }
        CHECK((int)m.n == distinct);
        printf("point map: %d distinct cells of 1681, hottest cell hit %d times\n", distinct, maxc);
        map_free(&m);
    }
    return 0;
}
