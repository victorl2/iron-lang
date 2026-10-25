/*
 * title: Sparse matrix stored in a coordinate-keyed hash map
 * topic: data_structures
 * covers: sparse matrix, packed 64-bit coordinate keys, open addressing with backward shift, zero pruning, add/multiply/transpose, dense cross-check
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
enum { N = 48 };
typedef struct {
    uint64_t *key; /* (row<<32 | col) + 1, 0 = empty */
    int64_t *val;
    size_t cap, nnz;
} Sparse;

static uint64_t pack(uint32_t r, uint32_t c) { return (((uint64_t)r << 32) | c) + 1; }

static void sp_init(Sparse *m, size_t cap) {
    m->key = calloc(cap, sizeof(uint64_t));
    m->val = calloc(cap, sizeof(int64_t));
    m->cap = cap;
    m->nnz = 0;
}
static void sp_free(Sparse *m) {
    free(m->key);
    free(m->val);
}

static long sp_slot(const Sparse *m, uint64_t k) {
    size_t i = mix64(k) & (m->cap - 1);
    while (m->key[i]) {
        if (m->key[i] == k)
            return (long)i;
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

static int64_t sp_get(const Sparse *m, uint32_t r, uint32_t c) {
    long s = sp_slot(m, pack(r, c));
    return s < 0 ? 0 : m->val[s];
}

static void sp_erase_slot(Sparse *m, size_t hole) {
    size_t mask = m->cap - 1, j = hole;
    for (;;) {
        j = (j + 1) & mask;
        if (!m->key[j])
            break;
        size_t h = mix64(m->key[j]) & mask;
        if (((j - h) & mask) >= ((j - hole) & mask)) {
            m->key[hole] = m->key[j];
            m->val[hole] = m->val[j];
            hole = j;
        }
    }
    m->key[hole] = 0;
    m->val[hole] = 0;
    m->nnz--;
}

/* add delta to entry (r,c); entries that reach zero are removed so nnz stays honest */
static void sp_add(Sparse *m, uint32_t r, uint32_t c, int64_t delta) {
    if (!delta)
        return;
    uint64_t k = pack(r, c);
    long s = sp_slot(m, k);
    if (s >= 0) {
        m->val[s] += delta;
        if (m->val[s] == 0)
            sp_erase_slot(m, (size_t)s);
        return;
    }
    if ((m->nnz + 1) * 2 > m->cap) {
        Sparse n;
        sp_init(&n, m->cap * 2);
        for (size_t i = 0; i < m->cap; i++)
            if (m->key[i])
                sp_add(&n, (uint32_t)((m->key[i] - 1) >> 32), (uint32_t)((m->key[i] - 1) & 0xffffffffu), m->val[i]);
        sp_free(m);
        *m = n;
    }
    size_t i = mix64(k) & (m->cap - 1);
    while (m->key[i])
        i = (i + 1) & (m->cap - 1);
    m->key[i] = k;
    m->val[i] = delta;
    m->nnz++;
}

static void sp_mul(Sparse *out, const Sparse *a, const Sparse *b) {
    /* index b by row so that each nonzero of a touches only matching rows */
    static int64_t brow[N][N];
    static int cnt[N];
    static uint32_t bcol[N][N];
    memset(cnt, 0, sizeof cnt);
    for (size_t i = 0; i < b->cap; i++)
        if (b->key[i]) {
            uint32_t r = (uint32_t)((b->key[i] - 1) >> 32), c = (uint32_t)((b->key[i] - 1) & 0xffffffffu);
            brow[r][cnt[r]] = b->val[i];
            bcol[r][cnt[r]++] = c;
        }
    sp_init(out, 16);
    for (size_t i = 0; i < a->cap; i++)
        if (a->key[i]) {
            uint32_t r = (uint32_t)((a->key[i] - 1) >> 32), k = (uint32_t)((a->key[i] - 1) & 0xffffffffu);
            for (int j = 0; j < cnt[k]; j++)
                sp_add(out, r, bcol[k][j], a->val[i] * brow[k][j]);
        }
}

static void sp_transpose(Sparse *out, const Sparse *a) {
    sp_init(out, 16);
    for (size_t i = 0; i < a->cap; i++)
        if (a->key[i])
            sp_add(out, (uint32_t)((a->key[i] - 1) & 0xffffffffu), (uint32_t)((a->key[i] - 1) >> 32), a->val[i]);
}

static void to_dense(const Sparse *m, int64_t d[N][N]) {
    memset(d, 0, sizeof(int64_t) * N * N);
    for (size_t i = 0; i < m->cap; i++)
        if (m->key[i])
            d[(m->key[i] - 1) >> 32][(m->key[i] - 1) & 0xffffffffu] = m->val[i];
}

static void fill(Sparse *m, int64_t d[N][N], int density_pct) {
    sp_init(m, 16);
    memset(d, 0, sizeof(int64_t) * N * N);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if ((int)(rnd() % 100) < density_pct) {
                int64_t v = (int64_t)(rnd() % 19) - 9;
                d[r][c] = v;
                sp_add(m, (uint32_t)r, (uint32_t)c, v);
            }
    size_t nz = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            nz += d[r][c] != 0;
    check(m->nnz == nz, "nnz after fill");
}

int main(void) {
    static int64_t da[N][N], db[N][N], dc[N][N], dref[N][N];
    Sparse a, b, prod, tr, sum;
    fill(&a, da, 6);
    fill(&b, db, 6);
    /* product */
    sp_mul(&prod, &a, &b);
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int64_t s = 0;
            for (int k = 0; k < N; k++)
                s += da[i][k] * db[k][j];
            dref[i][j] = s;
        }
    to_dense(&prod, dc);
    check(memcmp(dc, dref, sizeof dc) == 0, "sparse product equals dense product");
    /* transpose of product equals product of transposes reversed */
    sp_transpose(&tr, &prod);
    Sparse at, bt, btat;
    sp_transpose(&at, &a);
    sp_transpose(&bt, &b);
    sp_mul(&btat, &bt, &at);
    int64_t d1[N][N], d2[N][N];
    to_dense(&tr, d1);
    to_dense(&btat, d2);
    check(memcmp(d1, d2, sizeof d1) == 0, "(AB)^T = B^T A^T");
    /* A + (-A) erases everything and leaves an empty table */
    sp_init(&sum, 16);
    for (size_t i = 0; i < a.cap; i++)
        if (a.key[i]) {
            uint32_t r = (uint32_t)((a.key[i] - 1) >> 32), c = (uint32_t)((a.key[i] - 1) & 0xffffffffu);
            sp_add(&sum, r, c, a.val[i]);
            sp_add(&sum, r, c, -a.val[i]);
        }
    check(sum.nnz == 0, "cancellation removes entries");
    size_t nnz_before = a.nnz;
    /* random sequence of updates against the dense reference */
    for (int step = 0; step < 20000; step++) {
        uint32_t r = (uint32_t)(rnd() % N), c = (uint32_t)(rnd() % N);
        int64_t delta = (int64_t)(rnd() % 7) - 3;
        sp_add(&a, r, c, delta);
        da[r][c] += delta;
        check(sp_get(&a, r, c) == da[r][c], "entry after update");
    }
    size_t nz = 0;
    int64_t checksum = 0, trace = 0;
    for (int i = 0; i < N; i++) {
        trace += sp_get(&a, (uint32_t)i, (uint32_t)i);
        for (int j = 0; j < N; j++) {
            nz += da[i][j] != 0;
            checksum += da[i][j] * (i * 31 + j * 17 + 1);
        }
    }
    check(a.nnz == nz, "nnz after updates");
    printf("A nnz=%zu B nnz=%zu (AB) nnz=%zu of %d entries\n", nnz_before, b.nnz, prod.nnz, N * N);
    printf("after 20000 random updates: nnz=%zu table cap=%zu trace=%lld checksum=%lld\n", a.nnz, a.cap, (long long)trace,
           (long long)checksum);
    long long psum = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            psum += dref[i][j];
    printf("sum of entries of A*B=%lld\n", psum);
    sp_free(&a);
    sp_free(&b);
    sp_free(&prod);
    sp_free(&tr);
    sp_free(&at);
    sp_free(&bt);
    sp_free(&btat);
    sp_free(&sum);
    return 0;
}
