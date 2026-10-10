/*
 * title: Cheney copying collector over two semispaces
 * topic: memory
 * covers: semispace copying, forwarding pointers, scan and free cursors, tagged words, sharing and cycle preservation, address-independent graph digest checked across every collection
 * deps: libc
 */
#define SEED 0xC4E7E9ULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}

#define SEMI 3072u
#define NROOT 12
#define FWD 0xFFFFFFFFu
enum { T_CONS = 1, T_BOX = 2, T_VEC = 3 };

static uint32_t space_a[SEMI], space_b[SEMI];
static uint32_t *from = space_a, *to_sp = space_b;
static unsigned hp = 1;                        /* index 0 is reserved so 0 can mean nil */
static uint32_t roots[NROOT];

static unsigned long n_gc, copied_words_total, oom, allocs, max_live;

/* tagged values: 0 = nil, odd = integer (v >> 1), even nonzero = object index << 1 */
static uint32_t mk_int(uint32_t n) { return (n << 1) | 1u; }
static uint32_t mk_ref(unsigned i) { return (uint32_t)(i << 1); }
static int is_ref(uint32_t v) { return v != 0 && (v & 1u) == 0; }
static unsigned ref_idx(uint32_t v) { return v >> 1; }

static uint32_t digest_graph(const uint32_t *sp, unsigned *nobj, unsigned *nwords) {
    static int32_t num[SEMI];
    static unsigned queue[SEMI];
    memset(num, 0, sizeof num);
    unsigned qh = 0, qt = 0, count = 0, words = 0;
    uint32_t h = 2166136261u;
#define MIX(x) do { h ^= (uint32_t)(x); h *= 16777619u; } while (0)
    for (int r = 0; r < NROOT; r++) {
        uint32_t v = roots[r];
        if (is_ref(v)) {
            unsigned i = ref_idx(v);
            if (!num[i]) { num[i] = (int32_t)++count; queue[qt++] = i; }
            MIX(((uint32_t)num[i] << 2) | 2u);
        } else MIX(v == 0 ? 0u : (v << 2) | 1u);
    }
    while (qh < qt) {
        unsigned i = queue[qh++];
        uint32_t hd = sp[i];
        CHECK(hd != FWD);
        unsigned n = hd >> 4;
        words += 1 + n;
        MIX(hd);
        for (unsigned j = 0; j < n; j++) {
            uint32_t v = sp[i + 1 + j];
            if (is_ref(v)) {
                unsigned t = ref_idx(v);
                if (!num[t]) { num[t] = (int32_t)++count; queue[qt++] = t; }
                MIX(((uint32_t)num[t] << 2) | 2u);
            } else MIX(v == 0 ? 0u : (v << 2) | 1u);
        }
    }
#undef MIX
    *nobj = count; *nwords = words;
    return h;
}

static unsigned free_ptr;

static uint32_t evacuate(uint32_t v) {
    if (!is_ref(v)) return v;
    unsigned i = ref_idx(v);
    if (from[i] == FWD) return mk_ref(from[i + 1]);
    unsigned n = from[i] >> 4;
    unsigned dst = free_ptr;
    memcpy(&to_sp[dst], &from[i], (size_t)(1 + n) * sizeof(uint32_t));
    free_ptr += 1 + n;
    from[i] = FWD;
    from[i + 1] = dst;
    return mk_ref(dst);
}

static void collect(void) {
    unsigned nobj0, nw0, nobj1, nw1;
    uint32_t d0 = digest_graph(from, &nobj0, &nw0);
    n_gc++;
    free_ptr = 1;
    unsigned scan = 1;
    for (int r = 0; r < NROOT; r++) roots[r] = evacuate(roots[r]);
    while (scan < free_ptr) {
        unsigned n = to_sp[scan] >> 4;
        for (unsigned j = 0; j < n; j++) to_sp[scan + 1 + j] = evacuate(to_sp[scan + 1 + j]);
        scan += 1 + n;
    }
    CHECK(scan == free_ptr);
    copied_words_total += free_ptr - 1;
    uint32_t *t = from; from = to_sp; to_sp = t;
    hp = free_ptr;
    memset(to_sp, 0xAB, sizeof(space_a));
    uint32_t d1 = digest_graph(from, &nobj1, &nw1);
    CHECK(d0 == d1 && nobj0 == nobj1 && nw0 == nw1);   /* same graph, same sharing, exact compaction */
    CHECK(hp - 1 == nw1);
    if (nw1 > max_live) max_live = nw1;
}

/* returns object index or 0 on OOM; may collect, so callers must re-read roots afterwards */
static unsigned alloc_obj(unsigned type, unsigned n) {
    if (hp + 1 + n > SEMI) {
        collect();
        if (hp + 1 + n > SEMI) { oom++; return 0; }
    }
    unsigned i = hp;
    hp += 1 + n;
    from[i] = (n << 4) | type;
    for (unsigned j = 0; j < n; j++) from[i + 1 + j] = 0;
    allocs++;
    return i;
}

static uint32_t *field_ptr(uint32_t v, unsigned k) { unsigned i = ref_idx(v); return &from[i + 1 + k % (from[i] >> 4)]; }

int main(void) {
    memset(to_sp, 0xAB, sizeof(space_a));
    unsigned long ops_done[4] = {0};
    uint32_t last_digest = 0;
    for (int step = 0; step < 40000; step++) {
        unsigned op = rnd() % 100;
        unsigned a = rnd() % NROOT, b = rnd() % NROOT, k = rnd() % NROOT;
        if (op < 45) {
            unsigned i = alloc_obj(T_CONS, 2);
            if (!i) { roots[rnd() % NROOT] = 0; continue; }
            from[i + 1] = roots[a]; from[i + 2] = roots[b];       /* roots read after any collection */
            roots[k] = mk_ref(i);
            ops_done[0]++;
        } else if (op < 58) {
            unsigned i = alloc_obj(T_BOX, 1);
            if (!i) continue;
            from[i + 1] = mk_int(rnd() % 1000);
            roots[k] = mk_ref(i);
            ops_done[1]++;
        } else if (op < 70) {
            unsigned n = 1 + rnd() % 6;
            unsigned i = alloc_obj(T_VEC, n);
            if (!i) continue;
            for (unsigned j = 0; j < n; j++) from[i + 1 + j] = (j & 1) ? roots[(a + j) % NROOT] : mk_int(j * 7 + 1);
            roots[k] = mk_ref(i);
            ops_done[2]++;
        } else if (op < 92) {
            /* mutate: walk a few pointer hops from root a, then overwrite a field with root b */
            uint32_t v = roots[a];
            for (int hop = 0; hop < 3 && is_ref(v); hop++) {
                uint32_t nx = *field_ptr(v, rnd() % 4);
                if (!is_ref(nx)) break;
                v = nx;
            }
            if (is_ref(v)) { *field_ptr(v, rnd() % 6) = roots[b]; ops_done[3]++; }
        } else if (op < 99) {
            roots[a] = 0;
        } else if (rnd() % 4 == 0) {
            for (int r = 0; r < NROOT; r += 2) roots[r] = 0;
        }
        if (step % 5000 == 0) { unsigned no, nw; last_digest = digest_graph(from, &no, &nw); }
    }
    unsigned no, nw;
    last_digest = digest_graph(from, &no, &nw);
    printf("allocs=%lu (cons=%lu box=%lu vec=%lu) mutations=%lu\n", allocs, ops_done[0], ops_done[1], ops_done[2], ops_done[3]);
    printf("collections=%lu words copied=%lu (avg %lu per collection) max live=%lu of %u\n", n_gc, copied_words_total, n_gc ? copied_words_total / n_gc : 0, max_live, SEMI - 1);
    printf("out-of-memory events=%lu\n", oom);
    printf("final: %u reachable objects, %u words, heap pointer %u, digest %u\n", no, nw, hp, last_digest);
    collect();
    CHECK(hp - 1 == nw);
    return 0;
}
