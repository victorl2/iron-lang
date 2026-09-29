/*
 * title: Search-insert-delete problem on a sorted list
 * topic: concurrency
 * covers: search-insert-delete, categorical locks, lightswitches, atomic next pointers, exclusive deleters
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int v;
} Sem;

static void sem_make(Sem *s, int v) {
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->v = v;
}
static void sem_p(Sem *s) {
    pthread_mutex_lock(&s->m);
    while (s->v == 0)
        pthread_cond_wait(&s->c, &s->m);
    s->v--;
    pthread_mutex_unlock(&s->m);
}
static void sem_v(Sem *s) {
    pthread_mutex_lock(&s->m);
    s->v++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

typedef struct {
    pthread_mutex_t m;
    int count;
} Lightswitch;
static void ls_lock(Lightswitch *l, Sem *s) {
    pthread_mutex_lock(&l->m);
    if (++l->count == 1)
        sem_p(s);
    pthread_mutex_unlock(&l->m);
}
static void ls_unlock(Lightswitch *l, Sem *s) {
    pthread_mutex_lock(&l->m);
    if (--l->count == 0)
        sem_v(s);
    pthread_mutex_unlock(&l->m);
}

typedef struct Node Node;
struct Node {
    int key;
    _Atomic(Node *) next;
};

static Node *head; /* sentinel with key -1 */
static Sem no_searcher, no_inserter, insert_mutex;
static Lightswitch search_ls = {PTHREAD_MUTEX_INITIALIZER, 0};
static Lightswitch insert_ls = {PTHREAD_MUTEX_INITIALIZER, 0};

enum { SEARCHERS = 4, INSERTERS = 3, DELETERS = 2, SPAN = 120, ROUNDS = 40 };

typedef struct {
    int id;
    int found, missed, unsorted, ops;
} Worker;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Node *node_new(int key) {
    Node *n = malloc(sizeof *n);
    check(n != NULL, "malloc");
    n->key = key;
    atomic_init(&n->next, NULL);
    return n;
}

static void insert_sorted(int key) {
    Node *prev = head;
    Node *cur = atomic_load_explicit(&prev->next, memory_order_acquire);
    while (cur && cur->key < key) {
        prev = cur;
        cur = atomic_load_explicit(&cur->next, memory_order_acquire);
    }
    Node *n = node_new(key);
    atomic_store_explicit(&n->next, cur, memory_order_relaxed);
    atomic_store_explicit(&prev->next, n, memory_order_release);
}

static void delete_key(int key) {
    Node *prev = head;
    Node *cur = atomic_load(&prev->next);
    while (cur && cur->key < key) {
        prev = cur;
        cur = atomic_load(&cur->next);
    }
    check(cur && cur->key == key, "deleter target exists");
    atomic_store(&prev->next, atomic_load(&cur->next));
    free(cur);
}

static int search(int key, int *sorted_ok) {
    Node *cur = atomic_load_explicit(&head->next, memory_order_acquire);
    int last = -1;
    int hit = 0;
    while (cur) {
        if (cur->key <= last)
            *sorted_ok = 0;
        last = cur->key;
        if (cur->key == key)
            hit = 1;
        cur = atomic_load_explicit(&cur->next, memory_order_acquire);
    }
    return hit;
}

static void *searcher(void *arg) {
    Worker *w = arg;
    for (int r = 0; r < ROUNDS; r++) {
        for (int key = 4 + (w->id % 2) * 2; key < SPAN; key += 8) { /* keys 4 or 6 mod 8 are never deleted */
            ls_lock(&search_ls, &no_searcher);
            int ok = 1;
            int hit = search(key, &ok);
            if (!ok)
                w->unsorted++;
            if (hit)
                w->found++;
            else
                w->missed++;
            w->ops++;
            ls_unlock(&search_ls, &no_searcher);
        }
        /* a key that is never in the list */
        ls_lock(&search_ls, &no_searcher);
        int ok = 1;
        if (search(SPAN + 50 + w->id, &ok))
            w->unsorted += 1000;
        w->ops++;
        ls_unlock(&search_ls, &no_searcher);
    }
    return NULL;
}

static void *inserter(void *arg) {
    Worker *w = arg;
    for (int k = 0; k < SPAN / 2 / INSERTERS; k++) {
        int key = 1 + 2 * (w->id + INSERTERS * k); /* odd keys, disjoint between inserters */
        ls_lock(&insert_ls, &no_inserter);
        sem_p(&insert_mutex);
        insert_sorted(key);
        w->ops++;
        sem_v(&insert_mutex);
        ls_unlock(&insert_ls, &no_inserter);
    }
    return NULL;
}

static void *deleter(void *arg) {
    Worker *w = arg;
    for (int key = w->id * 2; key < SPAN; key += 8) { /* keys 0 mod 8 and 2 mod 8 */
        sem_p(&no_searcher);
        sem_p(&no_inserter);
        delete_key(key);
        w->ops++;
        sem_v(&no_inserter);
        sem_v(&no_searcher);
    }
    return NULL;
}

int main(void) {
    sem_make(&no_searcher, 1);
    sem_make(&no_inserter, 1);
    sem_make(&insert_mutex, 1);
    head = node_new(-1);
    for (int key = SPAN - 2; key >= 0; key -= 2) { /* base set: every even key */
        Node *n = node_new(key);
        atomic_store(&n->next, atomic_load(&head->next));
        atomic_store(&head->next, n);
    }
    Worker sw[SEARCHERS] = {{0, 0, 0, 0, 0}}, iw[INSERTERS] = {{0, 0, 0, 0, 0}}, dw[DELETERS] = {{0, 0, 0, 0, 0}};
    pthread_t st[SEARCHERS], it[INSERTERS], dt[DELETERS];
    for (int i = 0; i < SEARCHERS; i++) {
        sw[i].id = i;
        check(pthread_create(&st[i], NULL, searcher, &sw[i]) == 0, "searcher");
    }
    for (int i = 0; i < INSERTERS; i++) {
        iw[i].id = i;
        check(pthread_create(&it[i], NULL, inserter, &iw[i]) == 0, "inserter");
    }
    for (int i = 0; i < DELETERS; i++) {
        dw[i].id = i;
        check(pthread_create(&dt[i], NULL, deleter, &dw[i]) == 0, "deleter");
    }
    for (int i = 0; i < SEARCHERS; i++)
        pthread_join(st[i], NULL);
    for (int i = 0; i < INSERTERS; i++)
        pthread_join(it[i], NULL);
    for (int i = 0; i < DELETERS; i++)
        pthread_join(dt[i], NULL);

    long found = 0, missed = 0, bad = 0;
    for (int i = 0; i < SEARCHERS; i++) {
        found += sw[i].found;
        missed += sw[i].missed;
        bad += sw[i].unsorted;
    }
    check(bad == 0, "list stayed sorted, absent key never found");
    /* stable keys (4 mod 8, 6 mod 8) must always be found; searchers only ask for stable keys (plus an absent probe that is not counted) */
    check(missed == 0, "stable keys always found");
    check(found == (long)SEARCHERS * ROUNDS * 15, "search count");

    int n = 0;
    long sum = 0;
    int prev = -1;
    for (Node *c = atomic_load(&head->next); c; c = atomic_load(&c->next)) {
        check(c->key > prev, "final sorted");
        prev = c->key;
        n++;
        sum += c->key;
    }
    printf("searches=%ld found=%ld missed=%ld\n", found + missed, found, missed);
    printf("final length=%d key sum=%ld\n", n, sum);
    printf("first keys:");
    Node *c = atomic_load(&head->next);
    for (int i = 0; i < 10 && c; i++, c = atomic_load(&c->next))
        printf(" %d", c->key);
    printf("\n");
    /* 30 stable evens + 60 odds */
    check(n == 90, "final length");
    while (head) {
        Node *nx = atomic_load(&head->next);
        free(head);
        head = nx;
    }
    return 0;
}
