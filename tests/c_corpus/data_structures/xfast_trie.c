/*
 * title: X-fast trie with hashed levels
 * topic: data_structures
 * covers: x-fast trie, prefix hash tables, binary search on levels, predecessor/successor, leaf linked list
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 16
#define U (1u << W)
#define HB 15

typedef struct E {
    unsigned key, mn, mx, cnt;
    struct E *next;
} E;
static E *tab[1u << HB];
static int prv[U], nxt[U];
static unsigned nelem;
static long probes;

static unsigned long long rs = 0x5DEECE66DULL * 977;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static unsigned pkey(unsigned x, int len) { return (1u << len) | (x >> (W - len)); }
static unsigned hsh(unsigned k) { return (k * 2654435761u) >> (32 - HB); }
static E *find(unsigned key) {
    probes++;
    for (E *e = tab[hsh(key)]; e; e = e->next) if (e->key == key) return e;
    return NULL;
}
static E *add(unsigned key) {
    E *e = calloc(1, sizeof *e);
    e->key = key; e->next = tab[hsh(key)]; tab[hsh(key)] = e;
    return e;
}
static void drop(unsigned key) {
    E **p = &tab[hsh(key)];
    while (*p && (*p)->key != key) p = &(*p)->next;
    E *e = *p; *p = e->next; free(e);
}
static int member(unsigned x) { return find(pkey(x, W)) != NULL; }

/* deepest present prefix length for q (which is not a member) */
static int deepest(unsigned q) {
    int lo = 0, hi = W; /* prefix of length lo present, length hi absent */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (find(pkey(q, mid))) lo = mid; else hi = mid;
    }
    return lo;
}
/* smallest element >= q, or -1 */
static long succ_ge(unsigned q) {
    if (!nelem) return -1;
    if (member(q)) return q;
    int L = deepest(q);
    int b = (q >> (W - L - 1)) & 1;
    if (b == 0) return (long)find(pkey(q, L + 1) ^ 1u)->mn; /* sibling (right) child */
    E *left = find(pkey(q, L + 1) ^ 1u);
    return nxt[left->mx];
}
/* largest element <= q, or -1 */
static long pred_le(unsigned q) {
    if (!nelem) return -1;
    if (member(q)) return q;
    int L = deepest(q);
    int b = (q >> (W - L - 1)) & 1;
    if (b == 1) return (long)find(pkey(q, L + 1) ^ 1u)->mx;
    E *right = find(pkey(q, L + 1) ^ 1u);
    return prv[right->mn];
}
static int insert(unsigned x) {
    if (member(x)) return 0;
    long p = pred_le(x);
    long s = p >= 0 ? nxt[p] : (nelem ? (long)find(1)->mn : -1L);
    for (int len = 0; len <= W; len++) {
        E *e = find(pkey(x, len));
        if (!e) { e = add(pkey(x, len)); e->mn = e->mx = x; }
        else { if (x < e->mn) e->mn = x; if (x > e->mx) e->mx = x; }
        e->cnt++;
    }
    prv[x] = (int)p; nxt[x] = (int)s;
    if (p >= 0) nxt[p] = (int)x;
    if (s >= 0) prv[s] = (int)x;
    nelem++;
    return 1;
}
static int erase(unsigned x) {
    if (!member(x)) return 0;
    int p = prv[x], s = nxt[x];
    if (p >= 0) nxt[p] = s;
    if (s >= 0) prv[s] = p;
    for (int len = 0; len <= W; len++) {
        E *e = find(pkey(x, len));
        if (--e->cnt == 0) drop(pkey(x, len));
        else { if (e->mn == x) e->mn = (unsigned)s; if (e->mx == x) e->mx = (unsigned)p; }
    }
    nelem--;
    return 1;
}
static unsigned nodes_alive(void) {
    unsigned c = 0;
    for (unsigned i = 0; i < (1u << HB); i++) for (E *e = tab[i]; e; e = e->next) c++;
    return c;
}
static void free_all(void) {
    for (unsigned i = 0; i < (1u << HB); i++) { E *e = tab[i]; while (e) { E *n = e->next; free(e); e = n; } tab[i] = NULL; }
}

static unsigned char bits[U];

int main(void) {
    long maxprobe = 0, queries = 0;
    int ins = 0, del = 0;
    for (int step = 0; step < 30000; step++) {
        unsigned x;
        unsigned r = rnd();
        if (r % 4 == 0) x = (rnd() % 64) + 30000; /* dense cluster */
        else x = rnd() % U;
        unsigned op = (r >> 8) % 8;
        if (op < 4) { int a = insert(x); check(a == !bits[x], "insert result"); bits[x] = 1; ins += a; }
        else if (op < 6) { int a = erase(x); check(a == bits[x], "erase result"); bits[x] = 0; del += a; }
        else {
            probes = 0;
            long sp = succ_ge(x), pp = pred_le(x);
            long ps = probes; queries += 2;
            if (ps > maxprobe) maxprobe = ps;
            long es = -1, ep = -1;
            for (unsigned v = x; v < U; v++) if (bits[v]) { es = v; break; }
            for (long v = x; v >= 0; v--) if (bits[v]) { ep = v; break; }
            check(sp == es, "successor vs brute force");
            check(pp == ep, "predecessor vs brute force");
        }
        if (step % 5000 == 4999) {
            /* list order equals bitmap order */
            unsigned cnt = 0; long cur = nelem ? (long)find(1)->mn : -1L; long last = -1;
            while (cur >= 0) {
                check(bits[cur], "list node present"); check(prv[cur] == last, "prev link");
                last = cur; cur = nxt[cur]; cnt++;
            }
            unsigned tot = 0; for (unsigned v = 0; v < U; v++) tot += bits[v];
            check(cnt == tot && tot == nelem, "list covers all");
            printf("step %5d: n=%5u trie nodes=%6u max probes/query pair=%ld\n", step + 1, nelem, nodes_alive(), maxprobe);
        }
    }
    printf("inserted %d erased %d, queries %ld, levels W=%d\n", ins, del, queries, W);
    /* drain: erase everything and check the trie empties */
    for (unsigned v = 0; v < U; v++) if (bits[v]) erase(v);
    check(nelem == 0 && nodes_alive() == 0, "drained trie is empty");
    printf("drained: nodes=%u\n", nodes_alive());
    free_all();
    return 0;
}
