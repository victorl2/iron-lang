/*
 * title: Atomic reference counting with copy-on-write
 * topic: concurrency
 * covers: fetch_add relaxed retain, fetch_sub release, acquire fence before destroy, exactly-once destruction, COW when shared
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * retain: fetch_add(relaxed) is enough because the caller already holds a reference.
 * release: fetch_sub(release); the thread that sees the old value 1 issues an acquire fence and
 * destroys. The release decrements of all other holders happen-before that fence, so their
 * writes to the object are visible to the destructor, and nobody can still be using it.
 *
 * Objects carry a payload of eight words that every holder checks. The destructor poisons the
 * payload and counts itself, so a premature destroy (someone still reading) or a double destroy
 * shows up as a bad read or a wrong destroy count.
 */
enum { OBJS = 40, T = 6, STEPS = 4000 };

typedef struct {
    atomic_int rc;
    int id;
    int gid; /* graveyard slot: id for originals, OBJS for the COW copy */
    atomic_int payload[8];
    atomic_int destroyed;
} Obj;

static Obj *objs[OBJS];
static atomic_int destroy_count;
static atomic_int bad_reads;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Obj *obj_new(int id) {
    Obj *o = calloc(1, sizeof *o);
    check(o != NULL, "alloc");
    atomic_init(&o->rc, 1);
    o->id = id;
    o->gid = id;
    for (int i = 0; i < 8; i++)
        atomic_init(&o->payload[i], id * 10 + i);
    return o;
}

static Obj *retain(Obj *o) {
    atomic_fetch_add_explicit(&o->rc, 1, memory_order_relaxed);
    return o;
}

static int graveyard[OBJS + 1]; /* which ids got destroyed, written once by the destroyer */

static void release(Obj *o) {
    if (atomic_fetch_sub_explicit(&o->rc, 1, memory_order_release) == 1) {
        atomic_thread_fence(memory_order_acquire);
        for (int i = 0; i < 8; i++)
            atomic_store_explicit(&o->payload[i], -1, memory_order_relaxed);
        graveyard[o->gid]++;
        atomic_fetch_add(&destroy_count, 1);
        free(o);
    }
}

static int payload_ok(Obj *o) {
    for (int i = 0; i < 8; i++)
        if (atomic_load_explicit(&o->payload[i], memory_order_relaxed) != o->id * 10 + i)
            return 0;
    return 1;
}

static void *worker(void *p) {
    int t = (int)(size_t)p;
    unsigned s = 0xABCDEFu + 31u * (unsigned)t;
    Obj *mine[8] = {0};
    for (int i = 0; i < STEPS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int slot = (int)(s % 8u);
        if (mine[slot]) {
            if (!payload_ok(mine[slot]))
                atomic_fetch_add(&bad_reads, 1);
            if ((s >> 8) & 1u) { /* drop */
                release(mine[slot]);
                mine[slot] = NULL;
            } else { /* copy the handle into another slot */
                int to = (int)((s >> 12) % 8u);
                if (!mine[to])
                    mine[to] = retain(mine[slot]);
            }
        } else {
            /* borrow from the global table: only safe because main still holds its reference
             * until every thread has joined */
            mine[slot] = retain(objs[(s >> 4) % OBJS]);
        }
    }
    for (int i = 0; i < 8; i++)
        if (mine[i])
            release(mine[i]);
    return NULL;
}

/* copy-on-write: mutate in place only when we are the sole owner */
static Obj *make_mut(Obj *o, int *copied) {
    if (atomic_load_explicit(&o->rc, memory_order_acquire) == 1) {
        *copied = 0;
        return o;
    }
    Obj *n = obj_new(o->id);
    n->gid = OBJS;
    release(o);
    *copied = 1;
    return n;
}

int main(void) {
    for (int i = 0; i < OBJS; i++)
        objs[i] = obj_new(i);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < OBJS; i++)
        check(atomic_load(&objs[i]->rc) == 1, "only main's reference remains");
    check(atomic_load(&destroy_count) == 0, "nothing destroyed while main holds refs");
    check(atomic_load(&bad_reads) == 0, "payload always intact");
    /* COW demo, single threaded and deterministic */
    Obj *a = objs[3];
    Obj *b = retain(a);
    int c1, c2;
    Obj *b2 = make_mut(b, &c1); /* shared (rc 2): must copy */
    check(c1 == 1 && b2 != a, "copy when shared");
    Obj *a2 = make_mut(a, &c2); /* now unique again: in place */
    check(c2 == 0 && a2 == a, "in place when unique");
    printf("cow: shared -> copied=%d, unique -> copied=%d\n", c1, c2);
    objs[3] = a2;
    release(b2);
    for (int i = 0; i < OBJS; i++)
        release(objs[i]);
    int dc = atomic_load(&destroy_count);
    int once = 1;
    for (int i = 0; i < OBJS; i++)
        if (graveyard[i] != 1)
            once = 0;
    check(graveyard[OBJS] == 1, "COW copy destroyed once");
    check(dc == OBJS + 1, "every object destroyed once (plus the COW copy)");
    check(once, "each original destroyed exactly once");
    printf("objects=%d destroyed=%d exactly-once=%s\n", OBJS, dc, once ? "yes" : "no");
    printf("bad payload reads: %d\n", atomic_load(&bad_reads));
    return 0;
}
