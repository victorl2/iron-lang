/*
 * title: Crit-bit (Patricia) tree over byte strings
 * topic: data_structures
 * covers: crit-bit tree, patricia trie, prefix iteration, deletion, sorted-array cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CB {
    /* internal: byte index + mask of critical bit; leaf: key != NULL */
    char *key;
    size_t byte;
    unsigned char mask; /* single bit set within byte */
    struct CB *child[2];
} CB;

static unsigned long long rs = 0x0F1E2D3C4B5A6978ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int dir(const CB *n, const unsigned char *k, size_t len) {
    unsigned char c = n->byte < len ? k[n->byte] : 0;
    return (c & n->mask) != 0; /* virtual NUL terminator gives 0 */
}
static CB *walk(CB *root, const char *key) {
    CB *p = root;
    size_t len = strlen(key);
    while (!p->key) p = p->child[dir(p, (const unsigned char *)key, len)];
    return p;
}
static int contains(CB *root, const char *key) { return root && strcmp(walk(root, key)->key, key) == 0; }

static int insert(CB **root, const char *key) {
    if (!*root) {
        CB *n = calloc(1, sizeof *n); n->key = strdup(key); *root = n; return 1;
    }
    const unsigned char *k = (const unsigned char *)key;
    size_t len = strlen(key);
    CB *best = walk(*root, key);
    const unsigned char *b = (const unsigned char *)best->key;
    size_t i = 0;
    while (b[i] == (i < len ? k[i] : 0) && (i < len || b[i])) i++;
    if (i >= len && b[i] == 0) return 0; /* present */
    unsigned char kc = i < len ? k[i] : 0, bc = b[i];
    unsigned char diff = kc ^ bc;
    unsigned char m = 0x80;
    while (!(diff & m)) m >>= 1;
    int newdir = (kc & m) != 0;
    CB *leaf = calloc(1, sizeof *leaf); leaf->key = strdup(key);
    CB *in = calloc(1, sizeof *in); in->byte = i; in->mask = m;
    CB **where = root;
    while (!(*where)->key) {
        CB *p = *where;
        if (p->byte > i || (p->byte == i && p->mask < m)) break;
        where = &p->child[dir(p, k, len)];
    }
    in->child[newdir] = leaf; in->child[1 - newdir] = *where;
    *where = in;
    return 1;
}
static int erase(CB **root, const char *key) {
    if (!*root) return 0;
    size_t len = strlen(key);
    CB **where = root, **parent_slot = NULL; CB *parent = NULL; int d = 0;
    while (!(*where)->key) {
        parent = *where; parent_slot = where;
        d = dir(parent, (const unsigned char *)key, len);
        where = &parent->child[d];
    }
    if (strcmp((*where)->key, key) != 0) return 0;
    CB *leaf = *where;
    free(leaf->key); free(leaf);
    if (!parent) { *root = NULL; return 1; }
    CB *sib = parent->child[1 - d];
    *parent_slot = sib;
    free(parent);
    return 1;
}
static void destroy(CB *n) { if (!n) return; if (n->key) free(n->key); else { destroy(n->child[0]); destroy(n->child[1]); } free(n); }

typedef struct { char **v; int n; } Vec;
static void walk_all(const CB *n, Vec *o) { if (n->key) { o->v[o->n++] = n->key; return; } walk_all(n->child[0], o); walk_all(n->child[1], o); }
/* iterate keys with a prefix: descend until subtree's crit byte > prefix len */
static void with_prefix(const CB *root, const char *pre, Vec *o) {
    if (!root) return;
    size_t pl = strlen(pre);
    const CB *p = root, *top = root;
    while (!p->key) {
        p = p->child[dir(p, (const unsigned char *)pre, pl)];
        if (!p->key && p->byte < pl) top = p;
        if (p->key) break;
    }
    if (strncmp(walk((CB *)root, pre)->key, pre, pl) != 0) return;
    /* collect from top: all keys under top with prefix (filter to be safe) */
    Vec tmp; tmp.v = malloc(sizeof(char *) * 512); tmp.n = 0;
    walk_all(top, &tmp);
    for (int i = 0; i < tmp.n; i++) if (strncmp(tmp.v[i], pre, pl) == 0) o->v[o->n++] = tmp.v[i];
    free(tmp.v);
}
static int depth(const CB *n) { if (n->key) return 0; int a = depth(n->child[0]), b = depth(n->child[1]); return 1 + (a > b ? a : b); }
static int count_internal(const CB *n) { return n->key ? 0 : 1 + count_internal(n->child[0]) + count_internal(n->child[1]); }

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void rand_key(char *o) {
    int len = 1 + (int)(rnd() % 6);
    for (int i = 0; i < len; i++) o[i] = (char)('a' + rnd() % 4);
    o[len] = 0;
}

int main(void) {
    CB *root = NULL;
    char *ref[600]; int nref = 0;
    int ins = 0, dup = 0, del = 0, miss = 0;
    for (int step = 0; step < 900; step++) {
        char key[16]; rand_key(key);
        int idx = -1;
        for (int i = 0; i < nref; i++) if (strcmp(ref[i], key) == 0) idx = i;
        if (rnd() % 3) {
            int r = insert(&root, key);
            check(r == (idx < 0), "insert result matches reference");
            check(contains(root, key), "present after insert");
            if (r) { ref[nref++] = strdup(key); ins++; } else dup++;
        } else {
            int r = erase(&root, key);
            check(r == (idx >= 0), "erase result matches reference");
            check(!contains(root, key), "absent after erase");
            if (r) { free(ref[idx]); ref[idx] = ref[--nref]; del++; } else miss++;
        }
        if (step % 50 == 49) {
            /* full in-order equals sorted reference; n leaves means n-1 internal nodes */
            Vec v; v.v = malloc(sizeof(char *) * 600); v.n = 0;
            if (root) walk_all(root, &v);
            check(v.n == nref, "size");
            char *sorted[600]; memcpy(sorted, ref, sizeof(char *) * (size_t)nref);
            qsort(sorted, (size_t)nref, sizeof(char *), cmp_str);
            for (int i = 0; i < nref; i++) check(strcmp(v.v[i], sorted[i]) == 0, "in-order sorted");
            if (root) check(count_internal(root) == nref - 1, "internal count");
            for (int i = 0; i < nref; i++) check(contains(root, ref[i]), "contains member");
            free(v.v);
        }
    }
    const char *prefixes[] = { "a", "ab", "ba", "dd", "cdc", "" };
    for (int p = 0; p < 6; p++) {
        Vec v; v.v = malloc(sizeof(char *) * 600); v.n = 0;
        with_prefix(root, prefixes[p], &v);
        int expect = 0;
        for (int i = 0; i < nref; i++) if (strncmp(ref[i], prefixes[p], strlen(prefixes[p])) == 0) expect++;
        check(v.n == expect, "prefix count");
        printf("prefix '%s': %d keys", prefixes[p], v.n);
        for (int i = 0; i < v.n && i < 4; i++) printf(" %s", v.v[i]);
        printf("\n");
        free(v.v);
    }
    printf("inserted %d duplicates %d deleted %d missing %d\n", ins, dup, del, miss);
    printf("final keys %d, depth %d, internal nodes %d\n", nref, root ? depth(root) : 0, root ? count_internal(root) : 0);
    for (int i = 0; i < nref; i++) free(ref[i]);
    destroy(root);
    return 0;
}
