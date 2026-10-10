/*
 * title: Fibonacci heap with cascading cuts
 * topic: data_structures
 * covers: Fibonacci heap, circular doubly linked lists, lazy consolidation, cascading cut, decrease-key, delete, size bound
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 777001;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct FNode {
    int key, id, degree, mark;
    struct FNode *parent, *child, *left, *right;
} FNode;

typedef struct {
    FNode *min;
    long n, cuts, links;
} Fib;

enum { MAXID = 3000, NEG_INF = -1000000 };
static FNode *where[MAXID];

static void ring_add(FNode **head, FNode *x) {
    if (!*head) {
        x->left = x->right = x;
        *head = x;
    } else {
        x->right = (*head)->right;
        x->left = *head;
        (*head)->right->left = x;
        (*head)->right = x;
    }
}

static void ring_remove(FNode **head, FNode *x) {
    if (x->right == x)
        *head = NULL;
    else {
        x->left->right = x->right;
        x->right->left = x->left;
        if (*head == x)
            *head = x->right;
    }
    x->left = x->right = x;
}

static void fib_insert(Fib *f, int key, int id) {
    FNode *x = calloc(1, sizeof(FNode));
    x->key = key;
    x->id = id;
    where[id] = x;
    ring_add(&f->min, x);
    if (x->key < f->min->key)
        f->min = x;
    f->n++;
}

static void consolidate(Fib *f) {
    FNode *tab[64] = {0};
    int cnt = 0;
    for (FNode *x = f->min; ; x = x->right) {
        cnt++;
        if (x->right == f->min)
            break;
    }
    FNode *roots[4096];
    FNode *x = f->min;
    for (int i = 0; i < cnt; i++, x = x->right)
        roots[i] = x;
    for (int i = 0; i < cnt; i++) {
        FNode *a = roots[i];
        while (tab[a->degree]) {
            FNode *b = tab[a->degree];
            tab[a->degree] = NULL;
            if (b->key < a->key) {
                FNode *t = a;
                a = b;
                b = t;
            }
            /* b becomes child of a */
            b->left->right = b->right;
            b->right->left = b->left;
            b->left = b->right = b;
            b->parent = a;
            b->mark = 0;
            ring_add(&a->child, b);
            a->degree++;
            f->links++;
        }
        tab[a->degree] = a;
    }
    f->min = NULL;
    for (int d = 0; d < 64; d++)
        if (tab[d]) {
            tab[d]->left = tab[d]->right = tab[d];
            ring_add(&f->min, tab[d]);
        }
    FNode *m = f->min;
    for (FNode *y = m->right; y != m; y = y->right)
        if (y->key < f->min->key)
            f->min = y;
}

static void fib_extract_min(Fib *f, int *key, int *id) {
    FNode *z = f->min;
    while (z->child) {
        FNode *c = z->child;
        ring_remove(&z->child, c);
        c->parent = NULL;
        c->mark = 0;
        ring_add(&f->min, c);
    }
    /* f->min is a ring containing z; take z out and consolidate */
    ring_remove(&f->min, z);
    *key = z->key;
    *id = z->id;
    where[z->id] = NULL;
    free(z);
    f->n--;
    if (f->min)
        consolidate(f);
}

static void cut(Fib *f, FNode *x) {
    FNode *p = x->parent;
    ring_remove(&p->child, x);
    p->degree--;
    x->parent = NULL;
    x->mark = 0;
    ring_add(&f->min, x);
    f->cuts++;
}

static void fib_decrease(Fib *f, FNode *x, int nk) {
    check(nk <= x->key, "decrease only");
    x->key = nk;
    FNode *p = x->parent;
    if (p && x->key < p->key) {
        cut(f, x);
        while (p->parent) {
            if (!p->mark) {
                p->mark = 1;
                break;
            }
            FNode *pp = p->parent;
            cut(f, p);
            p = pp;
        }
    }
    if (x->key < f->min->key)
        f->min = x;
}

static long node_check(const FNode *x, const FNode *parent, long *fibmin) {
    long size = 1;
    int c = 0;
    if (x->child) {
        const FNode *y = x->child;
        do {
            check(y->parent == x, "parent link");
            check(y->key >= x->key, "heap order");
            check(y->right->left == y, "ring links");
            size += node_check(y, x, fibmin);
            c++;
            y = y->right;
        } while (y != x->child);
    }
    check(c == x->degree, "degree equals child count");
    check(where[x->id] == x, "handle");
    static const long F[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597, 2584};
    if (x->degree < 17)
        check(size >= F[x->degree], "subtree size >= Fibonacci(degree+2)");
    return size;
}

static long fib_check(const Fib *f) {
    if (!f->min) {
        check(f->n == 0, "empty");
        return 0;
    }
    long total = 0, dummy = 0;
    const FNode *x = f->min;
    do {
        check(!x->parent, "root has no parent");
        check(x->key >= f->min->key, "min pointer is minimum root");
        total += node_check(x, NULL, &dummy);
        x = x->right;
    } while (x != f->min);
    check(total == f->n, "node count");
    return total;
}

static void destroy(FNode *x) {
    if (!x)
        return;
    FNode *s = x;
    do {
        FNode *nx = x->right;
        destroy(x->child);
        free(x);
        x = nx;
    } while (x != s);
}

int main(void) {
    Fib f = {NULL, 0, 0, 0};
    int state[MAXID] = {0}, key[MAXID] = {0}, next_id = 0;
    int pops = 0, decs = 0, dels = 0;
    long popsum = 0;
    for (int op = 0; op < 3500; op++) {
        int r = (int)(rng() % 100);
        if ((r < 45 || f.n == 0) && next_id < MAXID) {
            int k = (int)(rng() % 20000);
            key[next_id] = k;
            state[next_id] = 1;
            fib_insert(&f, k, next_id++);
        } else if (r < 58 && f.n > 0) {
            int mk = 1 << 30;
            for (int i = 0; i < next_id; i++)
                if (state[i] && key[i] < mk)
                    mk = key[i];
            int k, id;
            fib_extract_min(&f, &k, &id);
            check(k == mk && state[id], "extract-min equals model");
            state[id] = 0;
            pops++;
            popsum += k;
        } else if (r < 90 && f.n > 0) {
            int id = (int)(rng() % (unsigned)next_id);
            while (!state[id])
                id = (id + 1) % next_id;
            int nk = key[id] - (int)(rng() % 3000);
            key[id] = nk;
            fib_decrease(&f, where[id], nk);
            decs++;
        } else if (f.n > 0) {
            int id = (int)(rng() % (unsigned)next_id);
            while (!state[id])
                id = (id + 1) % next_id;
            fib_decrease(&f, where[id], NEG_INF);
            int k, got;
            fib_extract_min(&f, &k, &got);
            check(got == id, "delete removes chosen id");
            state[id] = 0;
            dels++;
        }
        long live = 0;
        for (int i = 0; i < next_id; i++)
            live += state[i];
        check(fib_check(&f) == live, "live count");
    }
    printf("inserted=%d pops=%d decrease=%d deletes=%d\n", next_id, pops, decs, dels);
    printf("popsum=%ld remaining=%ld cuts=%ld links=%ld\n", popsum, f.n, f.cuts, f.links);
    int prev = -(1 << 30), k, id;
    long n = 0;
    while (f.n > 0) {
        fib_extract_min(&f, &k, &id);
        check(k >= prev, "drain ascending");
        prev = k;
        n++;
    }
    printf("drained=%ld\n", n);
    destroy(f.min);
    return 0;
}
