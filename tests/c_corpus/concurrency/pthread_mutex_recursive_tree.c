/*
 * title: Recursive mutex protecting a recursive tree walk
 * topic: concurrency
 * covers: PTHREAD_MUTEX_RECURSIVE, mutexattr, re-entrant locking, lock depth
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int key;
    struct Node *l, *r;
} Node;

typedef struct {
    pthread_mutex_t mu;
    Node *root;
    int size;
    int depth_now, depth_max; /* lock depth, tracked under the lock */
} Tree;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Every public operation locks; recursion re-enters the same lock. */
static void enter(Tree *t) {
    pthread_mutex_lock(&t->mu);
    if (++t->depth_now > t->depth_max)
        t->depth_max = t->depth_now;
}

static void leave(Tree *t) {
    t->depth_now--;
    pthread_mutex_unlock(&t->mu);
}

static Node *insert_rec(Tree *t, Node *n, int key, int *added) {
    enter(t);
    if (!n) {
        n = calloc(1, sizeof *n);
        n->key = key;
        *added = 1;
    } else if (key < n->key) {
        n->l = insert_rec(t, n->l, key, added);
    } else if (key > n->key) {
        n->r = insert_rec(t, n->r, key, added);
    }
    leave(t);
    return n;
}

static void tree_insert(Tree *t, int key) {
    enter(t);
    int added = 0;
    t->root = insert_rec(t, t->root, key, &added);
    t->size += added;
    leave(t);
}

static long sum_rec(Tree *t, Node *n) {
    if (!n)
        return 0;
    enter(t);
    long s = n->key + sum_rec(t, n->l) + sum_rec(t, n->r);
    leave(t);
    return s;
}

static long tree_sum(Tree *t) {
    enter(t);
    long s = sum_rec(t, t->root);
    leave(t);
    return s;
}

static void free_rec(Node *n) {
    if (!n)
        return;
    free_rec(n->l);
    free_rec(n->r);
    free(n);
}

typedef struct {
    Tree *t;
    int base;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < 100; i++) {
        int key = (a->base + i * 37) % 250; /* overlapping keys across threads */
        tree_insert(a->t, key);
    }
    return NULL;
}

int main(void) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    check(pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE) == 0, "settype");
    int type = -1;
    pthread_mutexattr_gettype(&at, &type);
    printf("type recursive: %s\n", type == PTHREAD_MUTEX_RECURSIVE ? "yes" : "no");
    Tree t = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0};
    check(pthread_mutex_init(&t.mu, &at) == 0, "init");
    pthread_mutexattr_destroy(&at);

    pthread_t th[4];
    Arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].t = &t;
        args[i].base = i * 11;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    /* expected: union of the key sets */
    char seen[250] = {0};
    int size = 0;
    long sum = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 100; k++) {
            int key = (i * 11 + k * 37) % 250;
            if (!seen[key]) {
                seen[key] = 1;
                size++;
                sum += key;
            }
        }
    check(t.size == size, "size");
    check(tree_sum(&t) == sum, "sum");
    check(t.depth_now == 0, "fully unlocked");
    check(t.depth_max >= 2, "re-entered");
    printf("size=%d sum=%ld\n", t.size, tree_sum(&t));
    printf("lock depth back to %d\n", t.depth_now);
    printf("re-entrant use observed: %s\n", t.depth_max >= 2 ? "yes" : "no");
    free_rec(t.root);
    pthread_mutex_destroy(&t.mu);
    return 0;
}
