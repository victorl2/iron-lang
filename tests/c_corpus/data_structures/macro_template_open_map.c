/*
 * title: Macro-generated open-addressing maps with backshift delete
 * topic: data_structures
 * covers: code generation by macros, linear probing, backward-shift deletion, generated test driver
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 20240607u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

#define DEFINE_MAP(K, V, NAME, HASH, EQ)                                                   \
    typedef struct { K k; V v; unsigned char used; } NAME##_slot;                          \
    typedef struct { NAME##_slot *s; size_t cap, n; size_t max_probe; } NAME##_map;        \
    static void NAME##_init(NAME##_map *m) {                                               \
        m->cap = 8; m->n = 0; m->max_probe = 0;                                            \
        m->s = calloc(m->cap, sizeof(NAME##_slot));                                        \
        CHECK(m->s);                                                                       \
    }                                                                                      \
    static void NAME##_free(NAME##_map *m) { free(m->s); m->s = NULL; }                    \
    static size_t NAME##_home(const NAME##_map *m, K k) { return HASH(k) & (m->cap - 1); } \
    static V *NAME##_get(NAME##_map *m, K k) {                                             \
        size_t i = NAME##_home(m, k);                                                      \
        while (m->s[i].used) {                                                             \
            if (EQ(m->s[i].k, k)) return &m->s[i].v;                                       \
            i = (i + 1) & (m->cap - 1);                                                    \
        }                                                                                  \
        return NULL;                                                                       \
    }                                                                                      \
    static void NAME##_put(NAME##_map *m, K k, V v);                                       \
    static void NAME##_grow(NAME##_map *m) {                                               \
        NAME##_slot *old = m->s;                                                           \
        size_t oc = m->cap;                                                                \
        m->cap *= 2; m->n = 0;                                                             \
        m->s = calloc(m->cap, sizeof(NAME##_slot));                                        \
        CHECK(m->s);                                                                       \
        for (size_t i = 0; i < oc; i++) if (old[i].used) NAME##_put(m, old[i].k, old[i].v);\
        free(old);                                                                         \
    }                                                                                      \
    static void NAME##_put(NAME##_map *m, K k, V v) {                                      \
        if ((m->n + 1) * 10 > m->cap * 7) NAME##_grow(m);                                  \
        size_t i = NAME##_home(m, k), probe = 0;                                           \
        while (m->s[i].used) {                                                             \
            if (EQ(m->s[i].k, k)) { m->s[i].v = v; return; }                               \
            i = (i + 1) & (m->cap - 1);                                                    \
            probe++;                                                                       \
        }                                                                                  \
        if (probe > m->max_probe) m->max_probe = probe;                                    \
        m->s[i].used = 1; m->s[i].k = k; m->s[i].v = v; m->n++;                            \
    }                                                                                      \
    static int NAME##_del(NAME##_map *m, K k) {                                            \
        size_t mask = m->cap - 1, i = NAME##_home(m, k);                                   \
        while (m->s[i].used && !EQ(m->s[i].k, k)) i = (i + 1) & mask;                      \
        if (!m->s[i].used) return 0;                                                       \
        size_t j = i;                                                                      \
        for (;;) {                                                                         \
            j = (j + 1) & mask;                                                            \
            if (!m->s[j].used) break;                                                      \
            size_t h = NAME##_home(m, m->s[j].k);                                          \
            int stays = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);                  \
            if (stays) continue;                                                           \
            m->s[i] = m->s[j];                                                             \
            i = j;                                                                         \
        }                                                                                  \
        m->s[i].used = 0;                                                                  \
        m->n--;                                                                            \
        return 1;                                                                          \
    }                                                                                      \
    /* every used slot must be reachable from its home without crossing an empty slot */   \
    static int NAME##_valid(NAME##_map *m) {                                               \
        size_t cnt = 0;                                                                    \
        for (size_t i = 0; i < m->cap; i++) {                                              \
            if (!m->s[i].used) continue;                                                   \
            cnt++;                                                                         \
            size_t j = NAME##_home(m, m->s[i].k);                                          \
            while (j != i) { if (!m->s[j].used) return 0; j = (j + 1) & (m->cap - 1); }    \
        }                                                                                  \
        return cnt == m->n;                                                                \
    }

/* generated test driver: random ops against a linear-scan model */
#define DEFINE_DRIVER(K, V, NAME, MKKEY, MKVAL, EQ, VEQ, KEYSPACE)                         \
    static void NAME##_drive(const char *label) {                                          \
        NAME##_map m;                                                                      \
        NAME##_init(&m);                                                                   \
        K mk[KEYSPACE]; V mv[KEYSPACE];                                                    \
        int mn = 0;                                                                        \
        long puts_ = 0, dels = 0, hits = 0;                                                \
        for (int step = 0; step < 5000; step++) {                                          \
            unsigned r = rnd() % 100;                                                      \
            int kid = (int)(rnd() % (KEYSPACE));                                           \
            K k = MKKEY(kid);                                                              \
            int idx = -1;                                                                  \
            for (int i = 0; i < mn; i++) if (EQ(mk[i], k)) { idx = i; break; }             \
            if (r < 50) {                                                                  \
                V v = MKVAL((int)(rnd() % 100000));                                        \
                NAME##_put(&m, k, v);                                                      \
                if (idx < 0) { mk[mn] = k; mv[mn++] = v; puts_++; } else mv[idx] = v;      \
            } else if (r < 75) {                                                           \
                int d = NAME##_del(&m, k);                                                 \
                CHECK(d == (idx >= 0));                                                    \
                if (idx >= 0) { mk[idx] = mk[mn - 1]; mv[idx] = mv[mn - 1]; mn--; dels++; }\
            } else {                                                                       \
                V *p = NAME##_get(&m, k);                                                  \
                CHECK((p != NULL) == (idx >= 0));                                          \
                if (p) { CHECK(VEQ(*p, mv[idx])); hits++; }                                \
            }                                                                              \
            CHECK((int)m.n == mn);                                                         \
            if (step % 250 == 0) CHECK(NAME##_valid(&m));                                  \
        }                                                                                  \
        CHECK(NAME##_valid(&m));                                                           \
        printf("%-8s size=%d cap=%zu inserts=%ld deletes=%ld hits=%ld max probe=%zu\n",    \
               label, mn, m.cap, puts_, dels, hits, m.max_probe);                          \
        NAME##_free(&m);                                                                   \
    }

/* instance 1: int -> int */
#define H_INT(k) mix32((uint32_t)(k))
#define EQ_PRIM(a, b) ((a) == (b))
#define MK_INT(i) ((i) * 3 - 200)
#define MV_ID(i) (i)
DEFINE_MAP(int, int, imap, H_INT, EQ_PRIM)
DEFINE_DRIVER(int, int, imap, MK_INT, MV_ID, EQ_PRIM, EQ_PRIM, 400)

/* instance 2: uint64 -> double, with clustered keys (multiples of 1024) */
#define H_U64(k) mix32((uint32_t)((k) ^ ((k) >> 32)))
#define MK_U64(i) ((uint64_t)(i) * 1024u)
#define MV_DBL(i) ((double)(i) / 4.0)
DEFINE_MAP(uint64_t, double, umap, H_U64, EQ_PRIM)
DEFINE_DRIVER(uint64_t, double, umap, MK_U64, MV_DBL, EQ_PRIM, EQ_PRIM, 300)

/* instance 3: 8-byte tag struct -> char */
typedef struct { char s[8]; } Tag;
static Tag mk_tag(int i) {
    Tag t;
    memset(&t, 0, sizeof t);
    snprintf(t.s, sizeof t.s, "t%d", i);
    return t;
}
static uint32_t h_tag(Tag t) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof t.s && t.s[i]; i++) { h ^= (unsigned char)t.s[i]; h *= 16777619u; }
    return mix32(h);
}
static int eq_tag(Tag a, Tag b) { return strncmp(a.s, b.s, sizeof a.s) == 0; }
#define MV_CHAR(i) ((char)('a' + (i) % 26))
DEFINE_MAP(Tag, char, tmap, h_tag, eq_tag)
DEFINE_DRIVER(Tag, char, tmap, mk_tag, MV_CHAR, eq_tag, EQ_PRIM, 200)

int main(void) {
    imap_drive("int");
    umap_drive("u64");
    tmap_drive("tag");
    /* deterministic delete-heavy scenario: build then remove every other key in a dense cluster */
    imap_map m;
    imap_init(&m);
    for (int i = 0; i < 100; i++) imap_put(&m, i * 8, i);
    for (int i = 0; i < 100; i += 2) CHECK(imap_del(&m, i * 8));
    CHECK(imap_valid(&m));
    long sum = 0;
    for (int i = 0; i < 100; i++) {
        int *p = imap_get(&m, i * 8);
        CHECK((p != NULL) == (i % 2 == 1));
        if (p) sum += *p;
    }
    printf("cluster: %zu keys left, value sum %ld\n", m.n, sum);
    imap_free(&m);
    return 0;
}
