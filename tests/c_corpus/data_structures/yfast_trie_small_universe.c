/*
 * title: Y-fast trie over a small universe
 * topic: data_structures
 * covers: y-fast trie, bucketed sorted arrays, x-fast representative index, split and merge, predecessor/successor
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 12
#define U (1u << W)
#define BMAX (2 * W)
#define BMIN (W / 2)
#define HB 12

/* ---- top level: x-fast style prefix hash over bucket representatives ---- */
typedef struct E { unsigned key, mn, mx, cnt; struct E *next; } E;
static E *tab[1u << HB];
static int prv[U], nxt[U];
static unsigned nreps;

static unsigned long long rs = 0x7F4A7C15ULL * 31337;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static unsigned pkey(unsigned x, int len) { return (1u << len) | (x >> (W - len)); }
static unsigned hsh(unsigned k) { return (k * 2654435761u) >> (32 - HB); }
static E *find(unsigned key) { for (E *e = tab[hsh(key)]; e; e = e->next) if (e->key == key) return e; return NULL; }
static E *add(unsigned key) { E *e = calloc(1, sizeof *e); e->key = key; e->next = tab[hsh(key)]; tab[hsh(key)] = e; return e; }
static void drop(unsigned key) { E **p = &tab[hsh(key)]; while (*p && (*p)->key != key) p = &(*p)->next; E *e = *p; *p = e->next; free(e); }
static int has_rep(unsigned x) { return find(pkey(x, W)) != NULL; }
static int deepest(unsigned q) {
    int lo = 0, hi = W;
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (find(pkey(q, mid))) lo = mid; else hi = mid; }
    return lo;
}
static long rep_succ_ge(unsigned q) {
    if (!nreps) return -1;
    if (has_rep(q)) return q;
    int L = deepest(q); int b = (q >> (W - L - 1)) & 1;
    if (b == 0) return (long)find(pkey(q, L + 1) ^ 1u)->mn;
    return nxt[find(pkey(q, L + 1) ^ 1u)->mx];
}
static long rep_pred_le(unsigned q) {
    if (!nreps) return -1;
    if (has_rep(q)) return q;
    int L = deepest(q); int b = (q >> (W - L - 1)) & 1;
    if (b == 1) return (long)find(pkey(q, L + 1) ^ 1u)->mx;
    return prv[find(pkey(q, L + 1) ^ 1u)->mn];
}
static void rep_insert(unsigned x) {
    long p = rep_pred_le(x);
    long s = p >= 0 ? nxt[p] : (nreps ? (long)find(1)->mn : -1L);
    for (int len = 0; len <= W; len++) {
        E *e = find(pkey(x, len));
        if (!e) { e = add(pkey(x, len)); e->mn = e->mx = x; } else { if (x < e->mn) e->mn = x; if (x > e->mx) e->mx = x; }
        e->cnt++;
    }
    prv[x] = (int)p; nxt[x] = (int)s;
    if (p >= 0) nxt[p] = (int)x;
    if (s >= 0) prv[s] = (int)x;
    nreps++;
}
static void rep_erase(unsigned x) {
    int p = prv[x], s = nxt[x];
    if (p >= 0) nxt[p] = s;
    if (s >= 0) prv[s] = p;
    for (int len = 0; len <= W; len++) {
        E *e = find(pkey(x, len));
        if (--e->cnt == 0) drop(pkey(x, len)); else { if (e->mn == x) e->mn = (unsigned)s; if (e->mx == x) e->mx = (unsigned)p; }
    }
    nreps--;
}

/* ---- bottom level: sorted-array buckets; a bucket is keyed by its maximum element ---- */
typedef struct { int n; unsigned a[2 * BMAX + 2]; } Bucket; /* room for a merged bucket before it is re-split */
static Bucket *bucket_of[U];
static int nsplit, nmerge;

static int bpos(const Bucket *b, unsigned x) { int i = 0; while (i < b->n && b->a[i] < x) i++; return i; }
static void set_rep(Bucket *b, unsigned oldmax, unsigned newmax) {
    if (oldmax == newmax) return;
    rep_erase(oldmax); bucket_of[oldmax] = NULL;
    rep_insert(newmax); bucket_of[newmax] = b;
}
static int yf_size;
static int member(unsigned x) {
    long r = rep_succ_ge(x);
    if (r < 0) return 0;
    Bucket *b = bucket_of[r];
    int i = bpos(b, x);
    return i < b->n && b->a[i] == x;
}
static void split(Bucket *b) {
    Bucket *c = malloc(sizeof *c);
    int h = b->n / 2;
    c->n = b->n - h;
    memcpy(c->a, b->a + h, sizeof(unsigned) * (size_t)c->n);
    unsigned oldmax = b->a[b->n - 1];
    b->n = h;
    rep_insert(b->a[h - 1]); bucket_of[b->a[h - 1]] = b;
    bucket_of[oldmax] = c;
    nsplit++;
}
static int insert(unsigned x) {
    if (member(x)) return 0;
    long r = rep_succ_ge(x);
    Bucket *b;
    if (r < 0) {
        long last = rep_pred_le(U - 1);
        if (last < 0) { b = malloc(sizeof *b); b->n = 1; b->a[0] = x; rep_insert(x); bucket_of[x] = b; yf_size++; return 1; }
        b = bucket_of[last];
        b->a[b->n++] = x;          /* new maximum: becomes the representative */
        set_rep(b, (unsigned)last, x);
    } else {
        b = bucket_of[r];
        int i = bpos(b, x);
        memmove(b->a + i + 1, b->a + i, sizeof(unsigned) * (size_t)(b->n - i));
        b->a[i] = x; b->n++;
    }
    if (b->n > BMAX) split(b);
    yf_size++;
    return 1;
}
static int erase(unsigned x) {
    if (!member(x)) return 0;
    long r = rep_succ_ge(x);
    Bucket *b = bucket_of[r];
    int i = bpos(b, x);
    unsigned oldmax = b->a[b->n - 1];
    memmove(b->a + i, b->a + i + 1, sizeof(unsigned) * (size_t)(b->n - i - 1));
    b->n--;
    yf_size--;
    if (b->n == 0) { rep_erase(oldmax); bucket_of[oldmax] = NULL; free(b); return 1; }
    if (i == b->n) set_rep(b, oldmax, b->a[b->n - 1]);
    if (b->n < BMIN) {
        unsigned myrep = b->a[b->n - 1];
        long nr = nxt[myrep];
        if (nr >= 0) { /* merge with the next bucket */
            Bucket *c = bucket_of[nr];
            memmove(c->a + b->n, c->a, sizeof(unsigned) * (size_t)c->n);
            memcpy(c->a, b->a, sizeof(unsigned) * (size_t)b->n);
            c->n += b->n;
            rep_erase(myrep); bucket_of[myrep] = NULL; free(b);
            nmerge++;
            if (c->n > BMAX) split(c);
        }
    }
    return 1;
}
static long succ_ge(unsigned x) {
    long r = rep_succ_ge(x);
    if (r < 0) return -1;
    Bucket *b = bucket_of[r];
    return b->a[bpos(b, x)];
}
static long pred_le(unsigned x) {
    long r = rep_succ_ge(x);
    if (r >= 0) {
        Bucket *b = bucket_of[r];
        int i = bpos(b, x);
        if (i < b->n && b->a[i] == x) return x;
        if (i > 0) return b->a[i - 1];
        if (b->a[0] == 0) return -1;
        long q = rep_pred_le(b->a[0] - 1);
        return q; /* previous bucket's maximum */
    }
    return rep_pred_le(U - 1);
}
static void check_structure(void) {
    long r = nreps ? (long)find(1)->mn : -1; unsigned total = 0; long prev = -1;
    while (r >= 0) {
        Bucket *b = bucket_of[r];
        check(b && b->n >= 1 && b->n <= BMAX, "bucket size bound");
        check(b->a[b->n - 1] == (unsigned)r, "representative is bucket max");
        for (int i = 1; i < b->n; i++) check(b->a[i - 1] < b->a[i], "bucket sorted");
        check((long)b->a[0] > prev, "buckets ordered");
        prev = r; total += (unsigned)b->n; r = nxt[r];
    }
    check((int)total == yf_size, "element count");
}
static void free_all(void) {
    for (unsigned i = 0; i < (1u << HB); i++) { E *e = tab[i]; while (e) { E *n = e->next; free(e); e = n; } tab[i] = NULL; }
    for (unsigned i = 0; i < U; i++) if (bucket_of[i]) { free(bucket_of[i]); bucket_of[i] = NULL; }
}

static unsigned char bits[U];

int main(void) {
    for (int phase = 0; phase < 3; phase++) {
        int ins = 0, del = 0, q = 0;
        unsigned pins = phase == 0 ? 6 : phase == 1 ? 3 : 4; /* out of 8 */
        for (int step = 0; step < 20000; step++) {
            unsigned x = phase == 2 ? (rnd() % 300) * 13 % U : rnd() % U;
            unsigned r = rnd() % 8;
            if (r < pins) { int a = insert(x); check(a == !bits[x], "insert"); bits[x] = 1; ins += a; }
            else if (r < 7) { int a = erase(x); check(a == bits[x], "erase"); bits[x] = 0; del += a; }
            else {
                long es = -1, ep = -1;
                for (unsigned v = x; v < U; v++) if (bits[v]) { es = v; break; }
                for (long v = x; v >= 0; v--) if (bits[v]) { ep = v; break; }
                check(succ_ge(x) == es, "succ"); check(pred_le(x) == ep, "pred");
                check(member(x) == bits[x], "member");
                q++;
            }
            if (step % 1000 == 999) check_structure();
        }
        unsigned cnt = 0; for (unsigned v = 0; v < U; v++) cnt += bits[v];
        check((int)cnt == yf_size, "final count");
        printf("phase %d: size %d buckets %u inserts %d erases %d queries %d splits %d merges %d\n", phase, yf_size, nreps, ins, del, q, nsplit, nmerge);
    }
    free_all();
    return 0;
}
