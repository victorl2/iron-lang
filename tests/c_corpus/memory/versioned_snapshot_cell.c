/*
 * title: Versioned config snapshots with refcounted readers
 * topic: memory
 * covers: publish/subscribe of immutable snapshots, refcounted snapshot lifetime, old versions outliving updates, arc-swap style
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int rc;
    int version;
    int nkeys;
    int keys[6];
    int vals[6];
} Snap;

typedef struct {
    Snap *cur;
    int published;
} Cell;

static int live_snaps;
static int destroyed_versions[32];
static int ndestroyed;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Snap *snap_retain(Snap *s) {
    s->rc++;
    return s;
}

static void snap_release(Snap *s) {
    if (--s->rc == 0) {
        destroyed_versions[ndestroyed++] = s->version;
        live_snaps--;
        free(s);
    }
}

static Snap *snap_new(int version) {
    Snap *s = calloc(1, sizeof *s);
    check(s != NULL, "alloc");
    s->rc = 1;
    s->version = version;
    live_snaps++;
    return s;
}

static int snap_get(const Snap *s, int key, int *out) {
    for (int i = 0; i < s->nkeys; i++)
        if (s->keys[i] == key) {
            *out = s->vals[i];
            return 1;
        }
    return 0;
}

/* Copy-update-publish: build a new immutable snapshot from the current one. */
static void cell_set(Cell *c, int key, int val) {
    Snap *old = c->cur;
    Snap *n = snap_new(old->version + 1);
    memcpy(n->keys, old->keys, sizeof old->keys);
    memcpy(n->vals, old->vals, sizeof old->vals);
    n->nkeys = old->nkeys;
    int found = 0;
    for (int i = 0; i < n->nkeys; i++)
        if (n->keys[i] == key) {
            n->vals[i] = val;
            found = 1;
        }
    if (!found) {
        check(n->nkeys < 6, "capacity");
        n->keys[n->nkeys] = key;
        n->vals[n->nkeys++] = val;
    }
    c->cur = n;
    c->published++;
    snap_release(old); /* the cell's own reference to the old version */
}

static Snap *cell_load(Cell *c) { return snap_retain(c->cur); }

static void show(const char *who, const Snap *s) {
    printf("%-7s v%d:", who, s->version);
    for (int i = 0; i < s->nkeys; i++)
        printf(" %d=%d", s->keys[i], s->vals[i]);
    printf(" (rc=%d)\n", s->rc);
}

int main(void) {
    Cell cell = {snap_new(0), 0};
    Snap *r1 = cell_load(&cell); /* reader 1 pins v0 */
    cell_set(&cell, 10, 100);
    cell_set(&cell, 20, 200);
    Snap *r2 = cell_load(&cell); /* reader 2 pins v2 */
    cell_set(&cell, 10, 111);
    cell_set(&cell, 30, 300);

    show("reader1", r1);
    show("reader2", r2);
    Snap *r3 = cell_load(&cell);
    show("reader3", r3);
    printf("live snapshots: %d (v1 died, v3 died)\n", live_snaps);
    printf("destroyed so far:");
    for (int i = 0; i < ndestroyed; i++)
        printf(" v%d", destroyed_versions[i]);
    printf("\n");

    int v1 = -1, v2 = -1, v3 = -1;
    int h1 = snap_get(r1, 10, &v1);
    int h2 = snap_get(r2, 10, &v2);
    int h3 = snap_get(r3, 10, &v3);
    printf("key 10 across versions: r1=%s r2=%d r3=%d\n", h1 ? "present" : "absent", v2, v3);
    check(!h1 && h2 && v2 == 100, "r2 sees old value");
    check(h3 && v3 == 111, "r3 sees new value");

    snap_release(r1);
    snap_release(r2);
    printf("after readers 1,2 done: live=%d destroyed count=%d\n", live_snaps, ndestroyed);
    for (int i = 0; i < 5; i++)
        cell_set(&cell, 40 + i % 2, i);
    snap_release(r3);
    printf("after churn: live=%d published=%d final v%d\n", live_snaps, cell.published,
           cell.cur->version);
    show("final", cell.cur);
    snap_release(cell.cur);
    check(live_snaps == 0, "leak");
    printf("total destroyed: %d\n", ndestroyed);
    return 0;
}
