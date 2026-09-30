/*
 * title: Epoch-based reclamation simulation
 * topic: memory
 * covers: epoch reclamation, per-reader pinned epochs, limbo lists, grace period, safe retirement of a shared linked list
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NREADERS 3
#define NEPOCH 3

typedef struct Node Node;
struct Node {
    int key;
    int poisoned;
    Node *next;
};

typedef struct {
    int active;
    unsigned epoch; /* epoch observed at pin time */
    Node *cursor;   /* the node this reader is currently looking at */
} Reader;

static unsigned global_epoch;
static Reader readers[NREADERS];
static Node *limbo[NEPOCH]; /* retired lists bucketed by epoch % 3 */
static Node *head;
static int reclaimed, retired, live;
static int uaf_detected;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Node *node_new(int key, Node *next) {
    Node *n = malloc(sizeof *n);
    check(n != NULL, "alloc");
    n->key = key;
    n->poisoned = 0;
    n->next = next;
    live++;
    return n;
}

static void pin(int r) {
    readers[r].active = 1;
    readers[r].epoch = global_epoch;
    readers[r].cursor = head;
}

static void unpin(int r) {
    readers[r].active = 0;
    readers[r].cursor = NULL;
}

static void retire(Node *n) {
    unsigned b = global_epoch % NEPOCH;
    n->next = limbo[b]; /* reuse next link, list is private to the limbo bucket */
    limbo[b] = n;
    retired++;
}

static int try_advance(void) {
    for (int i = 0; i < NREADERS; i++)
        if (readers[i].active && readers[i].epoch != global_epoch)
            return 0;
    global_epoch++;
    /* bucket (epoch+1)%3 == (epoch-2)%3 is now two epochs old: safe to free */
    unsigned b = (global_epoch + 1) % NEPOCH;
    int n = 0;
    while (limbo[b]) {
        Node *x = limbo[b];
        limbo[b] = x->next;
        x->poisoned = 1;
        /* a reader still pointing here would be a use-after-free */
        for (int i = 0; i < NREADERS; i++)
            if (readers[i].cursor == x)
                uaf_detected++;
        free(x);
        live--;
        reclaimed++;
        n++;
    }
    return n;
}

/* Unlink the node with the given key (head list only) and retire it. */
static int remove_key(int key) {
    Node **pp = &head;
    while (*pp && (*pp)->key != key)
        pp = &(*pp)->next;
    if (!*pp)
        return 0;
    Node *victim = *pp;
    *pp = victim->next;
    /* readers that already hold victim may still read it, so it goes to limbo, not free() */
    retire(victim);
    return 1;
}

int main(void) {
    for (int k = 5; k >= 1; k--)
        head = node_new(k, head);

    pin(0);
    pin(1);
    printf("epoch %u readers pinned at %u %u\n", global_epoch, readers[0].epoch, readers[1].epoch);

    check(remove_key(3) == 1, "remove 3");
    check(remove_key(1) == 1, "remove 1");
    printf("retired=%d reclaimed=%d live=%d\n", retired, reclaimed, live);

    int f;
    f = try_advance();
    printf("advance with readers at current epoch: freed %d, epoch %u\n", f, global_epoch);
    f = try_advance();
    printf("advance blocked (readers stale): freed %d, epoch %u\n", f, global_epoch);
    unpin(0);
    f = try_advance();
    printf("reader1 still stale: freed %d, epoch %u\n", f, global_epoch);
    unpin(1);
    pin(2);
    f = try_advance();
    printf("advance after unpin: freed %d, epoch %u\n", f, global_epoch);
    f = try_advance();
    printf("advance again: freed %d, epoch %u\n", f, global_epoch);
    unpin(2);
    f = try_advance();
    printf("advance again: freed %d, epoch %u\n", f, global_epoch);
    printf("retired=%d reclaimed=%d live=%d uaf=%d\n", retired, reclaimed, live, uaf_detected);

    /* steady state: writers churn while readers pin/unpin, memory stays bounded */
    unsigned s = 88172645u;
    int max_live = 0, next_key = 100;
    for (int round = 0; round < 60; round++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int r = (int)(s % NREADERS);
        if (readers[r].active)
            unpin(r);
        else
            pin(r);
        head = node_new(next_key++, head);
        Node *cur = head->next;
        if (cur) {
            int k = cur->key;
            check(remove_key(k) == 1, "remove");
        }
        try_advance();
        if (live > max_live)
            max_live = live;
    }
    for (int i = 0; i < NREADERS; i++)
        unpin(i);
    for (int i = 0; i < 4; i++)
        try_advance();
    printf("max live nodes %d, retired=%d reclaimed=%d uaf=%d\n", max_live, retired, reclaimed,
           uaf_detected);
    check(uaf_detected == 0, "no reader saw freed node");
    /* free the remaining list */
    while (head) {
        Node *n = head;
        head = n->next;
        free(n);
        live--;
    }
    for (int b = 0; b < NEPOCH; b++)
        while (limbo[b]) {
            Node *n = limbo[b];
            limbo[b] = n->next;
            free(n);
            live--;
        }
    check(live == 0, "leak");
    return 0;
}
