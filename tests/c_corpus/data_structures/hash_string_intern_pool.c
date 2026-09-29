/*
 * title: String interning pool with symbol ids
 * topic: data_structures
 * covers: string interning, arena storage, symbol table, pointer/id equality, hash of strings, dedup savings
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
typedef struct {
    uint32_t off, len, hash;
} Sym;
typedef struct {
    char *arena;
    size_t arena_len, arena_cap;
    Sym *syms;
    uint32_t nsyms, syms_cap;
    int32_t *table; /* symbol id or -1 */
    size_t tcap;
    long lookups, probes, bytes_requested;
} Pool;

static uint32_t hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return mix32(h);
}

static void pool_init(Pool *p) {
    memset(p, 0, sizeof *p);
    p->arena_cap = 64;
    p->arena = malloc(p->arena_cap);
    p->syms_cap = 8;
    p->syms = malloc(p->syms_cap * sizeof(Sym));
    p->tcap = 16;
    p->table = malloc(p->tcap * sizeof(int32_t));
    for (size_t i = 0; i < p->tcap; i++)
        p->table[i] = -1;
}

static const char *pool_str(const Pool *p, uint32_t id) { return p->arena + p->syms[id].off; }

static void pool_regrow(Pool *p) {
    size_t nc = p->tcap * 2;
    int32_t *nt = malloc(nc * sizeof(int32_t));
    for (size_t i = 0; i < nc; i++)
        nt[i] = -1;
    for (uint32_t id = 0; id < p->nsyms; id++) {
        size_t i = p->syms[id].hash & (nc - 1);
        while (nt[i] >= 0)
            i = (i + 1) & (nc - 1);
        nt[i] = (int32_t)id;
    }
    free(p->table);
    p->table = nt;
    p->tcap = nc;
}

static uint32_t intern(Pool *p, const char *s) {
    size_t n = strlen(s);
    uint32_t h = hash_bytes(s, n);
    p->lookups++;
    p->bytes_requested += (long)n;
    size_t i = h & (p->tcap - 1);
    for (;;) {
        p->probes++;
        int32_t id = p->table[i];
        if (id < 0)
            break;
        if (p->syms[id].hash == h && p->syms[id].len == n && memcmp(pool_str(p, (uint32_t)id), s, n) == 0)
            return (uint32_t)id;
        i = (i + 1) & (p->tcap - 1);
    }
    while (p->arena_len + n + 1 > p->arena_cap) {
        p->arena_cap *= 2;
        p->arena = realloc(p->arena, p->arena_cap);
    }
    if (p->nsyms == p->syms_cap) {
        p->syms_cap *= 2;
        p->syms = realloc(p->syms, p->syms_cap * sizeof(Sym));
    }
    memcpy(p->arena + p->arena_len, s, n + 1);
    p->syms[p->nsyms] = (Sym){(uint32_t)p->arena_len, (uint32_t)n, h};
    p->arena_len += n + 1;
    p->table[i] = (int32_t)p->nsyms;
    p->nsyms++;
    if (p->nsyms * 2 > p->tcap)
        pool_regrow(p);
    return p->nsyms - 1;
}

int main(void) {
    static const char *words[] = {"let", "fn", "if", "else", "while", "return", "struct", "enum", "match", "loop"};
    Pool p;
    pool_init(&p);
    enum { N = 6000 };
    static uint32_t ids[N];
    static char text[N][20];
    for (int i = 0; i < N; i++) {
        unsigned r = (unsigned)(rnd() % 100);
        if (r < 30)
            snprintf(text[i], sizeof text[i], "%s", words[rnd() % 10]);
        else if (r < 80)
            snprintf(text[i], sizeof text[i], "var%u", (unsigned)(rnd() % 400));
        else
            snprintf(text[i], sizeof text[i], "tmp_%u_%u", (unsigned)(rnd() % 30), (unsigned)(rnd() % 30));
        ids[i] = intern(&p, text[i]);
    }
    /* equal strings must have equal ids, distinct strings distinct ids; ids are dense in first-seen order */
    uint32_t next_new = 0;
    for (int i = 0; i < N; i++) {
        int first = -1;
        for (int j = 0; j < i && first < 0; j++)
            if (strcmp(text[i], text[j]) == 0)
                first = j;
        if (first >= 0)
            check(ids[i] == ids[first], "same string same id");
        else {
            check(ids[i] == next_new, "dense first-seen ids");
            next_new++;
        }
        check(strcmp(pool_str(&p, ids[i]), text[i]) == 0, "round trip");
    }
    check(p.nsyms == next_new, "symbol count");
    /* re-interning must not grow the arena */
    long requested = p.bytes_requested;
    size_t before = p.arena_len;
    for (int i = 0; i < N; i += 7)
        check(intern(&p, text[i]) == ids[i], "stable ids");
    check(p.arena_len == before, "no growth on hit");
    p.bytes_requested = requested;
    printf("interned %d strings into %u symbols; arena bytes=%zu vs %ld requested\n", N, p.nsyms, p.arena_len,
           p.bytes_requested);
    printf("keyword ids:");
    for (int i = 0; i < 10; i++)
        printf(" %s=%u", words[i], intern(&p, words[i]));
    printf("\n");
    printf("table=%zu slots load=%.4f avg probes=%.4f\n", p.tcap, (double)p.nsyms / (double)p.tcap,
           (double)p.probes / (double)p.lookups);
    free(p.arena);
    free(p.syms);
    free(p.table);
    return 0;
}
