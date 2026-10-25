/*
 * title: Byte-path trie map with owned void-pointer values and longest-prefix lookup
 * topic: data_structures
 * covers: first-child next-sibling trie, sorted sibling lists, value destructor callback, pruning on delete, longest prefix match, ordered iteration
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 5040u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct TNode {
    unsigned char ch;
    struct TNode *child, *sibling;   /* children sorted by ch through the sibling chain */
    void *val;                       /* owned; NULL means no key ends here */
} TNode;
typedef struct { TNode *root; void (*dtor)(void *); size_t n, nodes; } Trie;

static void t_init(Trie *t, void (*dtor)(void *)) {
    t->root = calloc(1, sizeof *t->root);
    CHECK(t->root);
    t->dtor = dtor; t->n = 0; t->nodes = 1;
}
static TNode *find_child(TNode *n, unsigned char ch) {
    for (TNode *c = n->child; c && c->ch <= ch; c = c->sibling) if (c->ch == ch) return c;
    return NULL;
}
/* returns previous value (ownership passes to the caller) or NULL */
static void *t_put(Trie *t, const char *key, void *val) {
    TNode *n = t->root;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        TNode **link = &n->child;
        while (*link && (*link)->ch < *p) link = &(*link)->sibling;
        if (!*link || (*link)->ch != *p) {
            TNode *c = calloc(1, sizeof *c);
            CHECK(c);
            c->ch = *p; c->sibling = *link;
            *link = c;
            t->nodes++;
        }
        n = *link;
    }
    void *old = n->val;
    n->val = val;
    if (!old) t->n++;
    return old;
}
static void *t_get(const Trie *t, const char *key) {
    TNode *n = t->root;
    for (const unsigned char *p = (const unsigned char *)key; *p && n; p++) n = find_child(n, *p);
    return n ? n->val : NULL;
}
/* longest stored key that is a prefix of the query; writes its length */
static void *t_longest_prefix(const Trie *t, const char *q, size_t *len) {
    TNode *n = t->root;
    void *best = NULL;
    size_t i = 0;
    *len = 0;
    for (const unsigned char *p = (const unsigned char *)q; *p; p++) {
        n = find_child(n, *p);
        if (!n) break;
        i++;
        if (n->val) { best = n->val; *len = i; }
    }
    return best;
}
static int prune(Trie *t, TNode *n, const unsigned char *p, int *found) {
    if (!*p) {
        if (n->val) { t->dtor(n->val); n->val = NULL; t->n--; *found = 1; }
    } else {
        TNode **link = &n->child;
        while (*link && (*link)->ch < *p) link = &(*link)->sibling;
        if (*link && (*link)->ch == *p) {
            TNode *c = *link;
            if (prune(t, c, p + 1, found)) {
                *link = c->sibling;
                free(c);
                t->nodes--;
            }
        }
    }
    return n->val == NULL && n->child == NULL;
}
static int t_del(Trie *t, const char *key) {
    int found = 0;
    prune(t, t->root, (const unsigned char *)key, &found);
    return found;
}
typedef void (*VisitFn)(const char *key, void *val, void *ctx);
static void walk(const TNode *n, char *buf, size_t depth, VisitFn fn, void *ctx) {
    if (n->val) { buf[depth] = 0; fn(buf, n->val, ctx); }
    for (const TNode *c = n->child; c; c = c->sibling) {
        buf[depth] = (char)c->ch;
        walk(c, buf, depth + 1, fn, ctx);
    }
}
static void t_each_prefix(const Trie *t, const char *prefix, VisitFn fn, void *ctx) {
    const TNode *n = t->root;
    char buf[64];
    size_t d = strlen(prefix);
    for (const unsigned char *p = (const unsigned char *)prefix; *p && n; p++) n = find_child((TNode *)n, *p);
    if (!n) return;
    memcpy(buf, prefix, d);
    walk(n, buf, d, fn, ctx);
}
static void free_nodes(TNode *n, void (*dtor)(void *)) {
    while (n) {
        TNode *sib = n->sibling;
        free_nodes(n->child, dtor);
        if (n->val) dtor(n->val);
        free(n);
        n = sib;
    }
}
static void t_free(Trie *t) { free_nodes(t->root, t->dtor); }

/* values: a route entry owning a heap string */
typedef struct { int id; char *label; } Route;
static long live_routes;
static Route *route_new(int id, const char *label) {
    Route *r = malloc(sizeof *r);
    CHECK(r);
    r->id = id;
    r->label = malloc(strlen(label) + 1);
    CHECK(r->label);
    strcpy(r->label, label);
    live_routes++;
    return r;
}
static void route_free(void *p) { Route *r = p; free(r->label); free(r); live_routes--; }

typedef struct { char keys[3000][24]; int ids[3000]; int n; } Seen;
static void collect(const char *key, void *val, void *ctx) {
    Seen *s = ctx;
    snprintf(s->keys[s->n], sizeof s->keys[0], "%s", key);
    s->ids[s->n++] = ((Route *)val)->id;
}

static void random_key(char *out) {
    static const char alpha[] = "ab/.";
    int n = 1 + (int)(rnd() % 7);
    for (int i = 0; i < n; i++) out[i] = alpha[rnd() % 4];
    out[n] = 0;
}

int main(void) {
    Trie t;
    t_init(&t, route_free);
    static char mkeys[3000][24];
    static int mid[3000];
    int mn = 0;
    long puts_ = 0, replaced = 0, dels = 0, lookups = 0;
    for (int step = 0; step < 5000; step++) {
        char key[24];
        random_key(key);
        int idx = -1;
        for (int i = 0; i < mn; i++) if (strcmp(mkeys[i], key) == 0) { idx = i; break; }
        unsigned op = rnd() % 10;
        if (op < 5) {
            int id = step;
            char label[16];
            snprintf(label, sizeof label, "r%d", id);
            void *old = t_put(&t, key, route_new(id, label));
            CHECK((old != NULL) == (idx >= 0));
            if (old) { CHECK(((Route *)old)->id == mid[idx]); route_free(old); mid[idx] = id; replaced++; }
            else { strcpy(mkeys[mn], key); mid[mn++] = id; }
            puts_++;
        } else if (op < 7) {
            int r = t_del(&t, key);
            CHECK(r == (idx >= 0));
            if (idx >= 0) { if (idx != mn - 1) strcpy(mkeys[idx], mkeys[mn - 1]); mid[idx] = mid[mn - 1]; mn--; dels++; }
        } else if (op < 9) {
            Route *r = t_get(&t, key);
            CHECK((r != NULL) == (idx >= 0));
            if (r) CHECK(r->id == mid[idx]);
            lookups++;
        } else {
            size_t len;
            Route *r = t_longest_prefix(&t, key, &len);
            int best = -1;
            size_t bl = 0;
            for (int i = 0; i < mn; i++) {
                size_t kl = strlen(mkeys[i]);
                if (kl <= strlen(key) && strncmp(mkeys[i], key, kl) == 0 && kl > bl) { bl = kl; best = i; }
            }
            CHECK((r != NULL) == (best >= 0));
            if (r) CHECK(r->id == mid[best] && len == bl);
        }
        CHECK(t.n == (size_t)mn);
    }
    /* ordered iteration under a prefix equals the sorted matching model keys */
    static const char *prefixes[] = { "", "a", "ab", "b/", "a.b" };
    for (int pi = 0; pi < 5; pi++) {
        static Seen seen;
        seen.n = 0;
        t_each_prefix(&t, prefixes[pi], collect, &seen);
        char sorted[3000][24];
        int sn = 0;
        for (int i = 0; i < mn; i++) if (strncmp(mkeys[i], prefixes[pi], strlen(prefixes[pi])) == 0) strcpy(sorted[sn++], mkeys[i]);
        for (int a = 1; a < sn; a++) {
            char tmp[24];
            strcpy(tmp, sorted[a]);
            int b = a - 1;
            while (b >= 0 && strcmp(sorted[b], tmp) > 0) { strcpy(sorted[b + 1], sorted[b]); b--; }
            strcpy(sorted[b + 1], tmp);
        }
        CHECK(seen.n == sn);
        for (int i = 0; i < sn; i++) CHECK(strcmp(seen.keys[i], sorted[i]) == 0);
        printf("prefix \"%s\": %d keys", prefixes[pi], sn);
        if (sn) printf(", first %s last %s", sorted[0], sorted[sn - 1]);
        printf("\n");
    }
    printf("puts=%ld replaced=%ld deletes=%ld lookups=%ld keys=%zu nodes=%zu\n", puts_, replaced, dels, lookups, t.n, t.nodes);
    CHECK(live_routes == (long)mn);
    while (mn > 0) { CHECK(t_del(&t, mkeys[--mn])); }
    CHECK(t.n == 0 && t.nodes == 1 && t.root->child == NULL);
    printf("all keys deleted, trie pruned back to the root, live values=%ld\n", live_routes);
    t_free(&t);
    return 0;
}
