/*
 * title: Longest prefix match routing table on a binary trie
 * topic: networking
 * covers: binary trie, longest prefix match, default route, route withdrawal with pruning, dynamic node pool, linear scan cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct Node { struct Node *ch[2]; int nh; } Node; /* nh < 0: no route here */
typedef struct { uint32_t addr; int len; int nh; int live; } Route;

static int nodes_alive;

static Node *mk(void) {
    Node *n = malloc(sizeof *n);
    CHECK(n);
    n->ch[0] = n->ch[1] = NULL;
    n->nh = -1;
    nodes_alive++;
    return n;
}

static uint32_t mask_of(int len) { return len == 0 ? 0u : (0xffffffffu << (32 - len)); }

static void insert(Node *root, uint32_t a, int len, int nh) {
    Node *n = root;
    for (int i = 0; i < len; i++) {
        int b = (int)((a >> (31 - i)) & 1);
        if (!n->ch[b]) n->ch[b] = mk();
        n = n->ch[b];
    }
    n->nh = nh;
}

static int lookup(const Node *root, uint32_t a, int *depth) {
    const Node *n = root;
    int best = -1, bd = -1;
    for (int i = 0;; i++) {
        if (n->nh >= 0) { best = n->nh; bd = i; }
        if (i == 32) break;
        n = n->ch[(a >> (31 - i)) & 1];
        if (!n) break;
    }
    *depth = bd;
    return best;
}

/* returns 1 if the subtree became empty and was freed */
static int withdraw(Node **np, uint32_t a, int len, int i, int *found) {
    Node *n = *np;
    if (!n) return 0;
    if (i == len) {
        if (n->nh >= 0) { *found = 1; n->nh = -1; }
    } else {
        withdraw(&n->ch[(a >> (31 - i)) & 1], a, len, i + 1, found);
    }
    if (n->nh < 0 && !n->ch[0] && !n->ch[1]) {
        free(n);
        nodes_alive--;
        *np = NULL;
        return 1;
    }
    return 0;
}

static void destroy(Node *n) {
    if (!n) return;
    destroy(n->ch[0]);
    destroy(n->ch[1]);
    free(n);
    nodes_alive--;
}

static uint32_t rs = 0x600dcafeu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int linear(const Route *r, int n, uint32_t a) {
    int best = -1, bl = -1;
    for (int i = 0; i < n; i++)
        if (r[i].live && (a & mask_of(r[i].len)) == r[i].addr && r[i].len > bl) { bl = r[i].len; best = r[i].nh; }
    return best;
}

static void pr(uint32_t a) { printf("%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255)); }

int main(void) {
    Node *root = mk();
    static const struct { uint32_t a; int len; int nh; } fixed[] = {
        { 0x00000000u, 0, 1 }, { 0x0a000000u, 8, 2 }, { 0x0a010000u, 16, 3 }, { 0x0a010100u, 24, 4 },
        { 0x0a010180u, 25, 5 }, { 0xc0a80000u, 16, 6 }, { 0xc0a80164u, 32, 7 },
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) insert(root, fixed[i].a, fixed[i].len, fixed[i].nh);
    static const uint32_t probes[] = { 0x0a010101u, 0x0a010181u, 0x0a010201u, 0x0a630000u, 0xc0a80164u, 0xc0a80165u, 0x08080808u };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        int d;
        int nh = lookup(root, probes[i], &d);
        pr(probes[i]);
        printf(" -> nexthop %d (matched /%d)\n", nh, d);
    }
    printf("nodes after fixed table: %d\n", nodes_alive);

    /* Withdraw the /25 and the /32, verify fallbacks and pruning. */
    int f = 0;
    int before = nodes_alive;
    withdraw(&root, 0x0a010180u, 25, 0, &f);
    CHECK(f);
    f = 0;
    withdraw(&root, 0xc0a80164u, 32, 0, &f);
    CHECK(f);
    int d, nh = lookup(root, 0x0a010181u, &d);
    CHECK(nh == 4 && d == 24);
    nh = lookup(root, 0xc0a80164u, &d);
    CHECK(nh == 6 && d == 16);
    printf("withdrew 2 routes, freed %d nodes, now %d\n", before - nodes_alive, nodes_alive);
    f = 0;
    withdraw(&root, 0x0a020000u, 16, 0, &f);
    CHECK(!f);

    /* Random table versus linear scan. */
    destroy(root);
    root = mk();
    CHECK(nodes_alive == 1);
    enum { NR = 400 };
    Route rt[NR];
    for (int i = 0; i < NR; i++) {
        uint32_t r = rnd();
        uint32_t extra = rnd();
        int len = 8 + (int)(r % 21) + (int)(extra % 4);
        if (len > 32) len = 32;
        rt[i].len = len;
        rt[i].addr = (0x0a000000u | (rnd() & 0x00ffffffu)) & mask_of(len);
        rt[i].nh = i + 100;
        rt[i].live = 1;
        insert(root, rt[i].addr, len, rt[i].nh);
        /* later duplicate prefixes override earlier: mirror that in the linear table */
        for (int j = 0; j < i; j++) if (rt[j].live && rt[j].addr == rt[i].addr && rt[j].len == rt[i].len) rt[j].live = 0;
    }
    int hits = 0, misses = 0;
    for (int i = 0; i < 20000; i++) {
        uint32_t a = (i & 1) ? (0x0a000000u | (rnd() & 0x00ffffffu)) : rnd();
        int dd;
        int got = lookup(root, a, &dd);
        int want = linear(rt, NR, a);
        CHECK(got == want);
        if (got >= 0) hits++; else misses++;
    }
    printf("random table: 20000 lookups, %d hits, %d misses, %d nodes\n", hits, misses, nodes_alive);
    /* Withdraw every live route in random order; the trie must return to a single root. */
    for (int i = NR - 1; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); Route t = rt[i]; rt[i] = rt[j]; rt[j] = t; }
    for (int i = 0; i < NR; i++) {
        if (!rt[i].live) continue;
        f = 0;
        withdraw(&root, rt[i].addr, rt[i].len, 0, &f);
        CHECK(f);
        rt[i].live = 0;
    }
    printf("after full withdrawal nodes=%d root=%s\n", nodes_alive, root ? "present" : "freed");
    CHECK(nodes_alive == 0 && root == NULL);
    return 0;
}
