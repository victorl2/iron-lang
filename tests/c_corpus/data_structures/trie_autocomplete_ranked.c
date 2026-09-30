/*
 * title: Trie autocomplete with weight-ranked top-k
 * topic: data_structures
 * covers: trie, subtree max annotation, best-first search with binary heap, weight updates, brute force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define AL 5
typedef struct Node {
    struct Node *kid[AL];
    long own;  /* key of the word ending here, or -1 */
    long best; /* maximum key in this subtree, or -1 */
    int id;
} Node;

/* key = weight * 4096 + (4095 - id): unique, larger is better */
static long make_key(int weight, int id) { return (long)weight * 4096 + (4095 - id); }
static int key_id(long k) { return 4095 - (int)(k % 4096); }
static int key_weight(long k) { return (int)(k / 4096); }

static Node *nnew(void) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    n->own = -1;
    n->best = -1;
    return n;
}

static long recompute(Node *n) {
    long b = n->own;
    for (int i = 0; i < AL; i++)
        if (n->kid[i] && n->kid[i]->best > b)
            b = n->kid[i]->best;
    n->best = b;
    return b;
}

static void put(Node *n, const char *w, long key) {
    if (*w) {
        int c = *w - 'a';
        if (!n->kid[c])
            n->kid[c] = nnew();
        put(n->kid[c], w + 1, key);
    } else
        n->own = key;
    recompute(n);
}

typedef struct {
    long key;
    const Node *node; /* NULL for a terminal entry */
} Item;

static Item heap[4096];
static int hn;

static void hpush(Item it) {
    int i = hn++;
    while (i > 0 && heap[(i - 1) / 2].key < it.key) {
        heap[i] = heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap[i] = it;
}

static Item hpop(void) {
    Item top = heap[0], last = heap[--hn];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= hn)
            break;
        if (c + 1 < hn && heap[c + 1].key > heap[c].key)
            c++;
        if (heap[c].key <= last.key)
            break;
        heap[i] = heap[c];
        i = c;
    }
    if (hn > 0)
        heap[i] = last;
    return top;
}

static int expansions;

static int complete(const Node *root, const char *prefix, int k, long *out) {
    const Node *n = root;
    for (const char *p = prefix; *p && n; p++)
        n = n->kid[*p - 'a'];
    if (!n || n->best < 0)
        return 0;
    hn = 0;
    Item first = {n->best, n};
    hpush(first);
    int got = 0;
    while (hn > 0 && got < k) {
        Item it = hpop();
        if (!it.node) {
            out[got++] = it.key;
            continue;
        }
        expansions++;
        const Node *cur = it.node;
        if (cur->own >= 0) {
            Item t = {cur->own, NULL};
            hpush(t);
        }
        for (int i = 0; i < AL; i++)
            if (cur->kid[i] && cur->kid[i]->best >= 0) {
                Item c = {cur->kid[i]->best, cur->kid[i]};
                hpush(c);
            }
    }
    return got;
}

static void nfree(Node *n) {
    for (int i = 0; i < AL; i++)
        if (n->kid[i])
            nfree(n->kid[i]);
    free(n);
}

#define MAXW 1500
static char words[MAXW][8];
static long keys[MAXW];
static int nw;

static int find_word(const char *w) {
    for (int i = 0; i < nw; i++)
        if (strcmp(words[i], w) == 0)
            return i;
    return -1;
}

int main(void) {
    Node *root = nnew();
    for (int i = 0; i < 1200; i++) {
        char w[8];
        int len = 2 + (int)rnd(5);
        for (int j = 0; j < len; j++)
            w[j] = (char)('a' + rnd(AL));
        w[len] = 0;
        int weight = 1 + (int)rnd(1000);
        int idx = find_word(w);
        if (idx < 0) {
            check(nw < MAXW, "capacity");
            idx = nw++;
            strcpy(words[idx], w);
        }
        keys[idx] = make_key(weight, idx);
        put(root, w, keys[idx]);
    }
    printf("distinct words=%d root best weight=%d\n", nw, key_weight(root->best));
    int total = 0;
    for (int q = 0; q < 600; q++) {
        char p[5];
        int len = (int)rnd(4);
        for (int j = 0; j < len; j++)
            p[j] = (char)('a' + rnd(AL));
        p[len] = 0;
        int k = 1 + (int)rnd(6);
        long got[8];
        int ng = complete(root, p, k, got);
        /* brute force: repeated max selection */
        long want[8];
        int nwant = 0;
        long prev = -1;
        for (int r = 0; r < k; r++) {
            long bestk = -1;
            for (int i = 0; i < nw; i++)
                if (strncmp(words[i], p, strlen(p)) == 0 && keys[i] > bestk &&
                    (prev < 0 || keys[i] < prev))
                    bestk = keys[i];
            if (bestk < 0)
                break;
            want[nwant++] = bestk;
            prev = bestk;
        }
        check(ng == nwant, "result count");
        for (int i = 0; i < ng; i++)
            check(got[i] == want[i], "ranked result");
        total += ng;
        if (q < 6) {
            printf("complete '%s' k=%d:", p, k);
            for (int i = 0; i < ng; i++)
                printf(" %s(%d)", words[key_id(got[i])], key_weight(got[i]));
            printf("\n");
        }
    }
    printf("total results=%d heap expansions=%d\n", total, expansions);
    /* boost a word and confirm it rises to the top of its prefix */
    int target = 17 % nw;
    keys[target] = make_key(5000, target);
    put(root, words[target], keys[target]);
    long top[1];
    check(complete(root, "", 1, top) == 1 && top[0] == keys[target], "boosted word tops list");
    char pre[2] = {words[target][0], 0};
    check(complete(root, pre, 1, top) == 1 && top[0] == keys[target], "boosted tops prefix");
    printf("boosted %s to weight 5000\n", words[target]);
    /* demote it and the old ranking must return */
    keys[target] = make_key(1, target);
    put(root, words[target], keys[target]);
    long a[3];
    int ng = complete(root, "", 3, a);
    check(ng == 3 && key_weight(a[0]) >= key_weight(a[1]), "after demotion");
    printf("top after demotion: %s(%d)\n", words[key_id(a[0])], key_weight(a[0]));
    nfree(root);
    return 0;
}
