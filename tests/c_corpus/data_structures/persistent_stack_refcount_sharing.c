/*
 * title: Persistent stack with shared tails and refcounts
 * topic: data_structures
 * covers: persistent list, structural sharing, reference counting, branching versions
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 2463534242u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Node {
    int val;
    int rc;
    int mark;
    struct Node *next;
} Node;

static int live_nodes;

/* Returns an owned reference; the new node takes its own reference on tail. */
static Node *cons(int v, Node *tail) {
    Node *n = malloc(sizeof *n);
    CHECK(n);
    n->val = v;
    n->rc = 1;
    n->mark = 0;
    n->next = tail;
    if (tail) tail->rc++;
    live_nodes++;
    return n;
}

static Node *retain(Node *n) {
    if (n) n->rc++;
    return n;
}

static void release(Node *n) {
    while (n) {
        if (--n->rc > 0) return;
        Node *nx = n->next;
        free(n);
        live_nodes--;
        n = nx;
    }
}

#define SLOTS 16
#define MAXLEN 128

static Node *head[SLOTS];
static int model[SLOTS][MAXLEN];
static int mlen[SLOTS];

static int reachable_distinct(int epoch) {
    int count = 0;
    for (int i = 0; i < SLOTS; i++)
        for (Node *n = head[i]; n && n->mark != epoch; n = n->next) {
            n->mark = epoch;
            count++;
        }
    return count;
}

static void verify_all(void) {
    for (int i = 0; i < SLOTS; i++) {
        Node *n = head[i];
        for (int k = 0; k < mlen[i]; k++) {
            CHECK(n);
            CHECK(n->val == model[i][mlen[i] - 1 - k]);
            n = n->next;
        }
        CHECK(n == NULL);
    }
}

static int shared_suffix(Node *a, Node *b, int la, int lb) {
    /* count trailing nodes that are pointer-identical */
    int c = 0;
    while (la > lb) { a = a->next; la--; }
    while (lb > la) { b = b->next; lb--; }
    while (a && b) {
        if (a == b) {
            while (a) { c++; a = a->next; }
            return c;
        }
        a = a->next;
        b = b->next;
    }
    return 0;
}

int main(void) {
    int epoch = 0;
    long pushes = 0, pops = 0, branches = 0, forks = 0, drops = 0;
    for (int step = 1; step <= 4000; step++) {
        int i = (int)(rnd() % SLOTS);
        int j = (int)(rnd() % SLOTS);
        unsigned op = rnd() % 10;
        if (op < 4) {
            if (mlen[i] < MAXLEN) {
                int v = (int)(rnd() % 1000);
                Node *nn = cons(v, head[i]);
                release(head[i]);
                head[i] = nn;
                model[i][mlen[i]++] = v;
                pushes++;
            }
        } else if (op < 6) {
            if (mlen[i] > 0) {
                Node *nx = retain(head[i]->next);
                release(head[i]);
                head[i] = nx;
                mlen[i]--;
                pops++;
            }
        } else if (op < 8) {
            if (i != j) {
                Node *c = retain(head[j]);
                release(head[i]);
                head[i] = c;
                for (int k = 0; k < mlen[j]; k++) model[i][k] = model[j][k];
                mlen[i] = mlen[j];
                forks++;
            }
        } else if (op < 9) {
            if (i != j && mlen[j] < MAXLEN) {
                int v = (int)(rnd() % 1000);
                Node *nn = cons(v, head[j]);
                release(head[i]);
                head[i] = nn;
                for (int k = 0; k < mlen[j]; k++) model[i][k] = model[j][k];
                model[i][mlen[j]] = v;
                mlen[i] = mlen[j] + 1;
                branches++;
            }
        } else {
            release(head[i]);
            head[i] = NULL;
            mlen[i] = 0;
            drops++;
        }
        if (step % 100 == 0) {
            verify_all();
            int distinct = reachable_distinct(++epoch);
            CHECK(distinct == live_nodes);
            long total = 0;
            for (int k = 0; k < SLOTS; k++) total += mlen[k];
            if (step % 800 == 0)
                printf("step %4d: versions hold %ld cells, %d live nodes\n", step, total, live_nodes);
        }
    }
    printf("pushes=%ld pops=%ld forks=%ld branches=%ld drops=%ld\n", pushes, pops, forks, branches, drops);
    int best = 0, bi = 0, bj = 0;
    for (int a = 0; a < SLOTS; a++)
        for (int b = a + 1; b < SLOTS; b++) {
            int s = shared_suffix(head[a], head[b], mlen[a], mlen[b]);
            if (s > best) { best = s; bi = a; bj = b; }
        }
    printf("largest shared tail: %d nodes between versions %d and %d\n", best, bi, bj);
    for (int k = 0; k < SLOTS; k++) {
        release(head[k]);
        head[k] = NULL;
    }
    CHECK(live_nodes == 0);
    printf("all released, live nodes = %d\n", live_nodes);
    return 0;
}
