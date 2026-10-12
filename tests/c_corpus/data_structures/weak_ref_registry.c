/*
 * title: Strong and weak references with control blocks in an owner tree
 * topic: data_structures
 * covers: control block, strong and weak counts, weak lock, deferred block free, parent back-pointers without cycles, liveness oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 3141592u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define MAXKIDS 3
typedef struct Ctrl Ctrl;
typedef struct Node {
    int id;
    Ctrl *kid[MAXKIDS];     /* strong references to children */
    Ctrl *parent;           /* weak reference to the owner */
} Node;
/* strong counts references that keep the object alive; the object as a whole holds one weak count for the block */
struct Ctrl { int strong, weak; Node *obj; };

static long live_objects, live_blocks, destroyed;

static void weak_dec(Ctrl *c) {
    if (--c->weak == 0) { free(c); live_blocks--; }
}
static Ctrl *weak_inc(Ctrl *c) { c->weak++; return c; }
static void strong_dec(Ctrl *c);
static Ctrl *strong_inc(Ctrl *c) { c->strong++; return c; }
static Ctrl *weak_lock(Ctrl *c) { /* strong reference or NULL when the object is gone */
    if (!c || c->strong == 0) return NULL;
    c->strong++;
    return c;
}
static void destroy(Ctrl *c) {
    Node *n = c->obj;
    c->obj = NULL;
    for (int k = 0; k < MAXKIDS; k++) {
        if (n->kid[k]) { Ctrl *kid = n->kid[k]; n->kid[k] = NULL; strong_dec(kid); }
    }
    if (n->parent) weak_dec(n->parent);
    free(n);
    live_objects--;
    destroyed++;
    weak_dec(c);     /* drop the block's self weak count */
}
static void strong_dec(Ctrl *c) {
    if (--c->strong == 0) destroy(c);
}
static Ctrl *make(int id) {
    Node *n = calloc(1, sizeof *n);
    Ctrl *c = malloc(sizeof *c);
    CHECK(n && c);
    n->id = id;
    c->strong = 1; c->weak = 1; c->obj = n;
    live_objects++; live_blocks++;
    return c;
}

#define NH 10
#define MAXID 3000

/* model: children lists by id, held handles; alive = reachable from handles through child edges */
static int mkid[MAXID][MAXKIDS];
static int mparent[MAXID];

static int alive_count(const Ctrl *h[NH], int *held_id, unsigned char *alive) {
    memset(alive, 0, MAXID);
    int stack[MAXID], sp = 0, n = 0;
    for (int i = 0; i < NH; i++) if (h[i]) {
        int id = held_id[i];
        if (!alive[id]) { alive[id] = 1; stack[sp++] = id; n++; }
    }
    while (sp) {
        int id = stack[--sp];
        for (int k = 0; k < MAXKIDS; k++) {
            int c = mkid[id][k];
            if (c >= 0 && !alive[c]) { alive[c] = 1; stack[sp++] = c; n++; }
        }
    }
    return n;
}

int main(void) {
    Ctrl *h[NH] = { NULL };           /* external strong handles */
    int hid[NH] = { 0 };
    Ctrl *ctrl_of[MAXID];             /* the block for each id, kept only through weak refs held below */
    Ctrl *weak_of[MAXID];             /* one extra weak reference per id, like an observer registry */
    int next_id = 0;
    memset(ctrl_of, 0, sizeof ctrl_of);
    memset(weak_of, 0, sizeof weak_of);
    long created = 0, attached = 0, detached = 0, locks_ok = 0, locks_dead = 0;
    static unsigned char alive[MAXID];
    for (int step = 0; step < 6000 && next_id < MAXID - 2; step++) {
        unsigned op = rnd() % 12;
        int i = (int)(rnd() % NH);
        if (op < 3) {
            if (h[i] && rnd() % 3 != 0) continue;
            int id = next_id++;
            for (int k = 0; k < MAXKIDS; k++) mkid[id][k] = -1;
            mparent[id] = -1;
            Ctrl *c = make(id);
            weak_of[id] = weak_inc(c);       /* registry keeps a weak ref forever */
            ctrl_of[id] = c;
            if (h[i]) strong_dec(h[i]);
            h[i] = c; hid[i] = id;
            created++;
        } else if (op < 6) {
            /* attach the object held in slot j as a child of the object held in slot i */
            int j = (int)(rnd() % NH), k = (int)(rnd() % MAXKIDS);
            if (h[i] && h[j] && i != j && mparent[hid[j]] < 0) {
                Node *p = h[i]->obj, *c = h[j]->obj;
                /* refuse attaching an ancestor below its own descendant (would create a strong cycle) */
                int cyc = 0;
                for (int a = hid[i]; a >= 0; a = mparent[a]) if (a == hid[j]) cyc = 1;
                if (!cyc && !p->kid[k]) {
                    p->kid[k] = strong_inc(h[j]);
                    c->parent = weak_inc(h[i]);
                    mkid[hid[i]][k] = hid[j];
                    mparent[hid[j]] = hid[i];
                    attached++;
                }
            }
        } else if (op < 8) {
            /* detach a child from a held parent */
            int k = (int)(rnd() % MAXKIDS);
            if (h[i] && h[i]->obj->kid[k]) {
                Ctrl *kid = h[i]->obj->kid[k];
                int kid_id = mkid[hid[i]][k];
                h[i]->obj->kid[k] = NULL;
                weak_dec(kid->obj->parent);
                kid->obj->parent = NULL;
                mkid[hid[i]][k] = -1;
                mparent[kid_id] = -1;
                strong_dec(kid);
                detached++;
            }
        } else if (op < 10) {
            if (h[i] && rnd() % 3 == 0) { strong_dec(h[i]); h[i] = NULL; }
        } else {
            /* observers: lock a weak reference to a random id and compare with the oracle */
            if (next_id > 0) {
                int id = (int)(rnd() % (unsigned)next_id);
                alive_count((const Ctrl **)h, hid, alive);
                Ctrl *s = weak_lock(weak_of[id]);
                CHECK((s != NULL) == (alive[id] != 0));
                if (s) { CHECK(s->obj->id == id); strong_dec(s); locks_ok++; } else locks_dead++;
                /* the parent link reads the same way */
                if (alive[id]) {
                    Ctrl *p = weak_lock(weak_of[id] ? weak_of[id]->obj->parent : NULL);
                    int expect_parent = mparent[id];
                    CHECK((p != NULL) == (expect_parent >= 0 && alive[expect_parent]));
                    if (p) { CHECK(p->obj->id == expect_parent); strong_dec(p); }
                }
            }
        }
        int n_alive = alive_count((const Ctrl **)h, hid, alive);
        CHECK(live_objects == n_alive);
    }
    /* final consistency: model alive set equals live objects; count edges */
    int n_alive = alive_count((const Ctrl **)h, hid, alive);
    long edges = 0;
    for (int id = 0; id < next_id; id++) if (alive[id]) for (int k = 0; k < MAXKIDS; k++) edges += mkid[id][k] >= 0;
    printf("created=%ld attached=%ld detached=%ld destroyed=%ld\n", created, attached, detached, destroyed);
    printf("weak locks: %ld succeeded, %ld found the object gone\n", locks_ok, locks_dead);
    printf("alive now=%d edges=%ld control blocks kept by observers=%ld\n", n_alive, edges, live_blocks);
    /* release everything: objects die, blocks die when the registry drops its weak refs */
    for (int i = 0; i < NH; i++) if (h[i]) { strong_dec(h[i]); h[i] = NULL; }
    CHECK(live_objects == 0);
    long blocks_before = live_blocks;
    for (int id = 0; id < next_id; id++) if (weak_of[id]) weak_dec(weak_of[id]);
    CHECK(live_blocks == 0);
    printf("after releasing handles: objects=%ld; dropping registry freed %ld blocks, blocks left=%ld\n", live_objects, blocks_before, live_blocks);
    return 0;
}
