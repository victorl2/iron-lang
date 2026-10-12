/*
 * title: Huffman code lengths and canonical code assignment
 * topic: algorithms
 * covers: Huffman coding, priority queue, two-queue linear merge, Kraft equality, canonical codes, bit-level decode
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSYM 16

typedef struct {
    unsigned long long w;
    int left, right, sym; /* indices into node pool, or -1 */
} Node;

static Node pool[2 * NSYM];
static int npool;

static unsigned st = 4711u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int new_node(unsigned long long w, int l, int r, int sym) {
    pool[npool] = (Node){w, l, r, sym};
    return npool++;
}

static void depths(int node, int d, int *len) {
    if (pool[node].sym >= 0) {
        len[pool[node].sym] = d ? d : 1;
        return;
    }
    depths(pool[node].left, d + 1, len);
    depths(pool[node].right, d + 1, len);
}

/* Method 1: binary heap over node indices, ties broken by node index for determinism. */
static int heap[2 * NSYM], hn;

static int hless(int a, int b) {
    return pool[a].w < pool[b].w || (pool[a].w == pool[b].w && a < b);
}

static void hpush(int v) {
    int i = hn++;
    heap[i] = v;
    while (i > 0 && hless(heap[i], heap[(i - 1) / 2])) {
        int t = heap[i];
        heap[i] = heap[(i - 1) / 2];
        heap[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static int hpop(void) {
    int top = heap[0];
    heap[0] = heap[--hn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < hn && hless(heap[l], heap[m]))
            m = l;
        if (r < hn && hless(heap[r], heap[m]))
            m = r;
        if (m == i)
            break;
        int t = heap[i];
        heap[i] = heap[m];
        heap[m] = t;
        i = m;
    }
    return top;
}

static void lengths_heap(const unsigned long long *f, int n, int *len) {
    npool = 0;
    hn = 0;
    for (int i = 0; i < n; i++)
        hpush(new_node(f[i], -1, -1, i));
    if (n == 1) {
        len[0] = 1;
        return;
    }
    while (hn > 1) {
        int a = hpop(), b = hpop();
        hpush(new_node(pool[a].w + pool[b].w, a, b, -1));
    }
    depths(heap[0], 0, len);
}

/* Method 2: sort leaves once, then merge with two FIFO queues (linear after the sort). */
static const unsigned long long *cmp_f;

static int cmp_idx_by(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    if (cmp_f[a] != cmp_f[b])
        return cmp_f[a] < cmp_f[b] ? -1 : 1;
    return a - b;
}

static unsigned long long cost_two_queues(const unsigned long long *f, int n) {
    int idx[NSYM];
    for (int i = 0; i < n; i++)
        idx[i] = i;
    cmp_f = f;
    qsort(idx, (size_t)n, sizeof(int), cmp_idx_by);
    unsigned long long q1[NSYM], q2[NSYM], cost = 0;
    int h1 = 0, h2 = 0, t2 = 0;
    for (int i = 0; i < n; i++)
        q1[i] = f[idx[i]];
    for (int merges = 0; merges < n - 1; merges++) {
        unsigned long long pick[2];
        for (int k = 0; k < 2; k++) {
            if (h2 == t2 || (h1 < n && q1[h1] <= q2[h2]))
                pick[k] = q1[h1++];
            else
                pick[k] = q2[h2++];
        }
        q2[t2++] = pick[0] + pick[1];
        cost += pick[0] + pick[1];
    }
    return n == 1 ? f[0] : cost;
}

typedef struct {
    int sym, len;
    unsigned code;
} Canon;

static int cmp_canon(const void *pa, const void *pb) {
    const Canon *a = pa, *b = pb;
    if (a->len != b->len)
        return a->len - b->len;
    return a->sym - b->sym;
}

int main(void) {
    const char *text = "abracadabra alakazam! she sells sea shells by the sea shore";
    unsigned long long freq[256] = {0};
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        freq[*p]++;
    int syms[NSYM], ns = 0;
    unsigned long long f[NSYM];
    for (int c = 0; c < 256 && ns < NSYM; c++)
        if (freq[c] > 0 && (c == 'a' || c == 'b' || c == 'e' || c == 's' || c == 'h' || c == ' ' || c == 'l' ||
                            c == 'r' || c == 'c' || c == 'd' || c == 'k' || c == 'm' || c == 'o' || c == 'y' ||
                            c == 't' || c == 'z')) {
            syms[ns] = c;
            f[ns++] = freq[c];
        }
    int len[NSYM];
    lengths_heap(f, ns, len);
    /* Kraft equality: sum 2^-len == 1, computed in units of 2^-maxlen */
    int maxl = 0;
    for (int i = 0; i < ns; i++)
        if (len[i] > maxl)
            maxl = len[i];
    unsigned long long kraft = 0, cost = 0;
    for (int i = 0; i < ns; i++) {
        kraft += 1ULL << (maxl - len[i]);
        cost += f[i] * (unsigned long long)len[i];
    }
    check(kraft == (1ULL << maxl), "Kraft sum equals one for a full tree");
    check(cost == cost_two_queues(f, ns), "heap and two-queue costs agree");
    printf("symbols=%d longest code=%d weighted cost=%llu bits\n", ns, maxl, cost);
    Canon cn[NSYM];
    for (int i = 0; i < ns; i++)
        cn[i] = (Canon){i, len[i], 0};
    qsort(cn, (size_t)ns, sizeof(Canon), cmp_canon);
    unsigned code = 0;
    for (int i = 0; i < ns; i++) {
        if (i > 0)
            code = (code + 1) << (cn[i].len - cn[i - 1].len);
        cn[i].code = code;
        printf("  '%c' freq=%2llu len=%d code=", syms[cn[i].sym] == ' ' ? '_' : syms[cn[i].sym], f[cn[i].sym],
               cn[i].len);
        for (int b = cn[i].len - 1; b >= 0; b--)
            putchar('0' + (int)((cn[i].code >> b) & 1u));
        putchar('\n');
    }
    /* encode a message of generated symbols and decode it with the canonical table */
    unsigned char bits[4096];
    int nb = 0, msg[300];
    for (int i = 0; i < 300; i++) {
        msg[i] = (int)(rnd() % (unsigned)ns);
        for (int k = 0; k < ns; k++)
            if (cn[k].sym == msg[i])
                for (int b = cn[k].len - 1; b >= 0; b--)
                    bits[nb++] = (unsigned char)((cn[k].code >> b) & 1u);
    }
    int pos = 0, decoded = 0;
    while (pos < nb) {
        unsigned c = 0;
        int l = 0, hit = -1;
        while (hit < 0) {
            c = (c << 1) | bits[pos + l];
            l++;
            for (int k = 0; k < ns; k++)
                if (cn[k].len == l && cn[k].code == c)
                    hit = cn[k].sym;
            check(l <= maxl, "prefix code always resolves");
        }
        check(hit == msg[decoded], "round trip symbol");
        decoded++;
        pos += l;
    }
    printf("round trip: %d symbols in %d bits\n", decoded, nb);
    /* random frequency vectors: both constructions must agree on total cost */
    unsigned long long sum_cost = 0;
    for (int t = 0; t < 200; t++) {
        int n = 1 + (int)(rnd() % NSYM);
        unsigned long long ff[NSYM];
        int ll[NSYM];
        for (int i = 0; i < n; i++)
            ff[i] = 1 + rnd() % 1000;
        lengths_heap(ff, n, ll);
        unsigned long long c1 = 0;
        for (int i = 0; i < n; i++)
            c1 += ff[i] * (unsigned long long)ll[i];
        check(c1 == cost_two_queues(ff, n), "random cost agreement");
        sum_cost += c1;
    }
    printf("200 random alphabets, total cost=%llu\n", sum_cost);
    return 0;
}
