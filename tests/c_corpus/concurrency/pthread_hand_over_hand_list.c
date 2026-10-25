/*
 * title: Hand-over-hand locking on a sorted linked list
 * topic: concurrency
 * covers: fine-grained locking, lock coupling, sorted insert, concurrent delete
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int key;
    struct Node *next;
    pthread_mutex_t mu;
} Node;

static Node *head; /* sentinel with key INT_MIN semantics: never compared */

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
    n->next = NULL;
    pthread_mutex_init(&n->mu, NULL);
    return n;
}

/* returns 1 if inserted, 0 if duplicate */
static int list_insert(int key) {
    Node *prev = head;
    pthread_mutex_lock(&prev->mu);
    Node *cur = prev->next;
    while (cur) {
        pthread_mutex_lock(&cur->mu);
        if (cur->key >= key)
            break;
        pthread_mutex_unlock(&prev->mu);
        prev = cur;
        cur = cur->next;
    }
    int ok = 1;
    if (cur && cur->key == key) {
        ok = 0;
    } else {
        Node *n = node_new(key);
        n->next = cur;
        prev->next = n;
    }
    if (cur)
        pthread_mutex_unlock(&cur->mu);
    pthread_mutex_unlock(&prev->mu);
    return ok;
}

/* returns 1 if removed */
static int list_remove(int key) {
    Node *prev = head;
    pthread_mutex_lock(&prev->mu);
    Node *cur = prev->next;
    while (cur) {
        pthread_mutex_lock(&cur->mu);
        if (cur->key >= key)
            break;
        pthread_mutex_unlock(&prev->mu);
        prev = cur;
        cur = cur->next;
    }
    int ok = 0;
    if (cur && cur->key == key) {
        prev->next = cur->next;
        pthread_mutex_unlock(&cur->mu);
        pthread_mutex_destroy(&cur->mu);
        free(cur);
        ok = 1;
    } else if (cur) {
        pthread_mutex_unlock(&cur->mu);
    }
    pthread_mutex_unlock(&prev->mu);
    return ok;
}

typedef struct {
    int id;
    int inserted, dups, removed;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    unsigned s = 0xC0FFEEu + (unsigned)a->id * 101u;
    for (int i = 0; i < 400; i++) {
        s = s * 1103515245u + 12345u;
        int key = (int)((s >> 10) % 300);
        /* threads own keys congruent to their id mod 4 for removal: removals never race
         * with another thread's insert of the same key, so counts are deterministic */
        int mine = key - key % 4 + a->id;
        if (i % 3 == 2) {
            a->removed += list_remove(mine);
        } else if (list_insert(mine)) {
            a->inserted++;
        } else {
            a->dups++;
        }
    }
    return NULL;
}

int main(void) {
    head = node_new(0);
    enum { T = 4 };
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].inserted = args[i].dups = args[i].removed = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int len = 0, sorted = 1, prevk = -1;
    long sum = 0;
    for (Node *n = head->next; n; n = n->next) {
        if (n->key <= prevk)
            sorted = 0;
        prevk = n->key;
        sum += n->key;
        len++;
    }
    int ins = 0, rem = 0;
    for (int i = 0; i < T; i++) {
        printf("thread %d: inserted=%d dups=%d removed=%d\n", i, args[i].inserted, args[i].dups,
               args[i].removed);
        ins += args[i].inserted;
        rem += args[i].removed;
    }
    check(sorted, "strictly sorted");
    check(len == ins - rem, "length equals inserts minus removals");
    printf("length=%d sum=%ld sorted=%s\n", len, sum, sorted ? "yes" : "no");

    Node *n = head;
    while (n) {
        Node *nx = n->next;
        pthread_mutex_destroy(&n->mu);
        free(n);
        n = nx;
    }
    return 0;
}
