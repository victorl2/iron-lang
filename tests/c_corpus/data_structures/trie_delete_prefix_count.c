/*
 * title: Trie with deletion and prefix counting
 * topic: data_structures
 * covers: trie, node pruning, prefix counts, multiset semantics, brute force cross-check
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

#define AL 4
typedef struct Node {
    struct Node *kid[AL];
    int pass; /* words (with multiplicity) passing through this node */
    int end;  /* words ending exactly here */
} Node;

static int live_nodes = 0;

static Node *node_new(void) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    live_nodes++;
    return n;
}

static void trie_insert(Node *root, const char *w) {
    Node *n = root;
    n->pass++;
    for (; *w; w++) {
        int c = *w - 'a';
        if (!n->kid[c])
            n->kid[c] = node_new();
        n = n->kid[c];
        n->pass++;
    }
    n->end++;
}

static int trie_count(const Node *root, const char *w) {
    const Node *n = root;
    for (; *w && n; w++)
        n = n->kid[*w - 'a'];
    return n ? n->end : 0;
}

static int trie_prefix(const Node *root, const char *p) {
    const Node *n = root;
    for (; *p && n; p++)
        n = n->kid[*p - 'a'];
    return n ? n->pass : 0;
}

/* removes one copy; prunes emptied nodes; returns 1 if removed */
static int trie_erase(Node *root, const char *w) {
    if (trie_count(root, w) == 0)
        return 0;
    Node *n = root;
    n->pass--;
    for (; *w; w++) {
        int c = *w - 'a';
        Node *k = n->kid[c];
        k->pass--;
        if (k->pass == 0) {
            /* free the whole remaining chain */
            Node *cur = k;
            const char *r = w + 1;
            n->kid[c] = NULL;
            while (cur) {
                Node *next = *r ? cur->kid[*r - 'a'] : NULL;
                free(cur);
                live_nodes--;
                if (*r)
                    r++;
                cur = next;
            }
            return 1;
        }
        n = k;
    }
    n->end--;
    return 1;
}

static void trie_free(Node *n) {
    if (!n)
        return;
    for (int i = 0; i < AL; i++)
        trie_free(n->kid[i]);
    free(n);
    live_nodes--;
}

#define MAXW 400
static char store[MAXW][8];
static int nstore = 0;

static void rand_word(char *w) {
    int len = 1 + (int)rnd(6);
    for (int i = 0; i < len; i++)
        w[i] = (char)('a' + rnd(AL));
    w[len] = 0;
}

static int brute_count(const char *w) {
    int c = 0;
    for (int i = 0; i < nstore; i++)
        c += strcmp(store[i], w) == 0;
    return c;
}

static int brute_prefix(const char *p) {
    int c = 0;
    size_t l = strlen(p);
    for (int i = 0; i < nstore; i++)
        c += strncmp(store[i], p, l) == 0;
    return c;
}

int main(void) {
    Node *root = node_new();
    int ins = 0, del = 0, miss = 0, qsum = 0;
    for (int step = 0; step < 4000; step++) {
        char w[8];
        rand_word(w);
        unsigned op = rnd(10);
        if (op < 4 && nstore < MAXW) {
            trie_insert(root, w);
            strcpy(store[nstore++], w);
            ins++;
        } else if (op < 8) {
            int had = brute_count(w) > 0;
            int got = trie_erase(root, w);
            check(had == got, "erase result");
            if (got) {
                for (int i = 0; i < nstore; i++)
                    if (strcmp(store[i], w) == 0) {
                        memcpy(store[i], store[nstore - 1], 8);
                        nstore--;
                        break;
                    }
                del++;
            } else
                miss++;
        } else {
            check(trie_count(root, w) == brute_count(w), "count");
            int p = trie_prefix(root, w);
            check(p == brute_prefix(w), "prefix");
            qsum += p;
        }
        check(root->pass == nstore, "root pass");
    }
    printf("inserts=%d deletes=%d misses=%d prefix_sum=%d\n", ins, del, miss, qsum);
    printf("stored=%d nodes=%d\n", nstore, live_nodes);
    printf("prefix a=%d ab=%d abc=%d dd=%d\n", trie_prefix(root, "a"), trie_prefix(root, "ab"),
           trie_prefix(root, "abc"), trie_prefix(root, "dd"));
    while (nstore > 0) {
        check(trie_erase(root, store[nstore - 1]) == 1, "drain");
        nstore--;
    }
    check(live_nodes == 1, "all pruned");
    printf("after drain nodes=%d\n", live_nodes);
    trie_free(root);
    check(live_nodes == 0, "no leak");
    return 0;
}
