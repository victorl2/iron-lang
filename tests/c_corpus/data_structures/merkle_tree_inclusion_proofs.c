/*
 * title: Merkle tree with inclusion proofs
 * topic: data_structures
 * covers: merkle tree, inclusion proof, domain-separated hashing, odd-leaf promotion, tamper detection, update path
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint64_t H;

static uint64_t rs = 0xA5A5A5A5DEADBEEFULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* FNV-1a 64 with a final avalanche; not cryptographic, only for structure */
static H mix(H h) { h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33; return h; }
static H fnv(H h, const unsigned char *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
static H leaf_hash(const unsigned char *d, size_t n) {
    unsigned char pre = 0x00;
    return mix(fnv(fnv(1469598103934665603ULL, &pre, 1), d, n));
}
static H node_hash(H l, H r) {
    unsigned char b[17]; b[0] = 0x01;
    for (int i = 0; i < 8; i++) { b[1 + i] = (unsigned char)(l >> (8 * i)); b[9 + i] = (unsigned char)(r >> (8 * i)); }
    return mix(fnv(1469598103934665603ULL, b, 17));
}

#define MAXL 64
typedef struct {
    int n;
    H lvl[8][MAXL]; /* lvl[0] = leaves */
    int cnt[8];
    int levels;
} Merkle;

static void rebuild(Merkle *m) {
    m->levels = 1; m->cnt[0] = m->n;
    while (m->cnt[m->levels - 1] > 1) {
        int c = m->cnt[m->levels - 1], p = m->levels - 1;
        int nc = (c + 1) / 2;
        for (int i = 0; i < nc; i++)
            m->lvl[p + 1][i] = (2 * i + 1 < c) ? node_hash(m->lvl[p][2 * i], m->lvl[p][2 * i + 1]) : m->lvl[p][2 * i]; /* promote odd */
        m->cnt[p + 1] = nc; m->levels++;
    }
}
static H root(const Merkle *m) { return m->lvl[m->levels - 1][0]; }

typedef struct { H sib; int right; int present; } Step; /* right: sibling is on the right */
static int prove(const Merkle *m, int idx, Step *out) {
    int k = 0;
    for (int l = 0; l < m->levels - 1; l++) {
        int sib = idx ^ 1;
        if (sib < m->cnt[l]) { out[k].sib = m->lvl[l][sib]; out[k].right = (idx & 1) == 0; out[k].present = 1; }
        else { out[k].present = 0; out[k].sib = 0; out[k].right = 0; }
        k++; idx >>= 1;
    }
    return k;
}
static H apply_proof(H leaf, const Step *s, int k) {
    H h = leaf;
    for (int i = 0; i < k; i++) {
        if (!s[i].present) continue;
        h = s[i].right ? node_hash(h, s[i].sib) : node_hash(s[i].sib, h);
    }
    return h;
}
/* brute-force recursive root: split at largest power of two < n only when it matches level pairing */
static H brute(const H *leaves, int n) {
    /* level-by-level pairing using a temp array, independent code path */
    H tmp[MAXL]; memcpy(tmp, leaves, sizeof(H) * (size_t)n);
    while (n > 1) {
        int j = 0;
        for (int i = 0; i < n; i += 2) tmp[j++] = i + 1 < n ? node_hash(tmp[i], tmp[i + 1]) : tmp[i];
        n = j;
    }
    return tmp[0];
}

int main(void) {
    int sizes[] = { 1, 2, 3, 4, 5, 7, 8, 13, 31, 64 };
    for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        Merkle m; m.n = sizes[si];
        unsigned char data[MAXL][6];
        for (int i = 0; i < m.n; i++) {
            for (int j = 0; j < 6; j++) data[i][j] = (unsigned char)(rnd() & 0xff);
            m.lvl[0][i] = leaf_hash(data[i], 6);
        }
        rebuild(&m);
        H r = root(&m);
        check(r == brute(m.lvl[0], m.n), "root equals brute-force reduction");
        int maxproof = 0, verified = 0;
        for (int i = 0; i < m.n; i++) {
            Step st[8];
            int k = prove(&m, i, st);
            int used = 0; for (int j = 0; j < k; j++) used += st[j].present;
            if (used > maxproof) maxproof = used;
            check(apply_proof(leaf_hash(data[i], 6), st, k) == r, "valid proof reaches root");
            verified++;
            /* tampered leaf must fail */
            unsigned char bad[6]; memcpy(bad, data[i], 6); unsigned pos = rnd() % 6; bad[pos] ^= (unsigned char)(1u << (rnd() % 8));
            check(apply_proof(leaf_hash(bad, 6), st, k) != r, "tampered leaf rejected");
            /* proof for a different index must fail on this leaf (when n>1) */
            if (m.n > 1) {
                int other = (i + 1) % m.n;
                check(apply_proof(leaf_hash(data[other], 6), st, k) != r || used == 0, "wrong leaf rejected");
            }
        }
        /* update one leaf: only its path changes, root differs */
        H old = r;
        data[m.n / 2][0] ^= 0x5a;
        m.lvl[0][m.n / 2] = leaf_hash(data[m.n / 2], 6);
        rebuild(&m);
        check(m.n == 1 || root(&m) != old, "update changes root");
        check(root(&m) == brute(m.lvl[0], m.n), "root after update");
        printf("n=%2d levels=%d root=%016llx max-proof=%d verified=%d\n", m.n, m.levels, (unsigned long long)old, maxproof, verified);
    }
    /* second-preimage style check: leaf vs node domain separation */
    unsigned char two[16];
    for (int i = 0; i < 8; i++) { two[i] = (unsigned char)i; two[8 + i] = (unsigned char)(i * 3); }
    H a = leaf_hash(two, 8), b = leaf_hash(two + 8, 8);
    check(node_hash(a, b) != leaf_hash(two, 16), "domain separation");
    printf("domain separation ok: %016llx\n", (unsigned long long)node_hash(a, b));
    return 0;
}
