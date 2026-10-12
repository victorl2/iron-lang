/*
 * title: Atomic weak-to-strong upgrade across threads
 * topic: memory
 * covers: CAS loop upgrade (increment only if nonzero), strong/weak atomic counts, races between drop and lock, deferred object destruction
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    atomic_int strong;
    atomic_int weak; /* weak refs plus one for the strong group */
    atomic_int payload_alive;
    int payload;
} Ctl;

static atomic_int payloads_destroyed;
static atomic_int ctls_freed;
static atomic_int bad_access;

static void ctl_weak_release(Ctl *c) {
    if (atomic_fetch_sub(&c->weak, 1) == 1) {
        free(c);
        atomic_fetch_add(&ctls_freed, 1);
    }
}

static void strong_release(Ctl *c) {
    if (atomic_fetch_sub(&c->strong, 1) == 1) {
        atomic_store(&c->payload_alive, 0); /* "destroy" the payload */
        c->payload = -1;
        atomic_fetch_add(&payloads_destroyed, 1);
        ctl_weak_release(c); /* the collective weak ref of the strong group */
    }
}

/* Upgrade: increment strong only while it is still > 0. */
static int try_upgrade(Ctl *c) {
    int n = atomic_load(&c->strong);
    while (n != 0) {
        if (atomic_compare_exchange_weak(&c->strong, &n, n + 1))
            return 1;
    }
    return 0;
}

#define NOBJ 64
#define NUP 3

static Ctl *ctls[NOBJ];
static int upgrades_ok[NUP];
static int upgrades_fail[NUP];

static void *dropper(void *arg) {
    (void)arg;
    for (int i = 0; i < NOBJ; i++)
        strong_release(ctls[i]); /* drop the owner's strong ref */
    return NULL;
}

static void *upgrader(void *arg) {
    int id = (int)(size_t)arg;
    for (int round = 0; round < 50; round++)
        for (int i = 0; i < NOBJ; i++) {
            Ctl *c = ctls[i]; /* every upgrader holds its own weak ref (taken in main) */
            if (try_upgrade(c)) {
                if (!atomic_load(&c->payload_alive) || c->payload != i)
                    atomic_fetch_add(&bad_access, 1);
                upgrades_ok[id]++;
                strong_release(c);
            } else {
                upgrades_fail[id]++;
            }
        }
    return NULL;
}

int main(void) {
    for (int i = 0; i < NOBJ; i++) {
        Ctl *c = malloc(sizeof *c);
        if (!c)
            abort();
        atomic_init(&c->strong, 1);
        /* 1 group weak + one weak per upgrader thread */
        atomic_init(&c->weak, 1 + NUP);
        atomic_init(&c->payload_alive, 1);
        c->payload = i;
        ctls[i] = c;
    }
    pthread_t up[NUP], dr;
    for (int i = 0; i < NUP; i++)
        pthread_create(&up[i], NULL, upgrader, (void *)(size_t)i);
    pthread_create(&dr, NULL, dropper, NULL);
    pthread_join(dr, NULL);
    for (int i = 0; i < NUP; i++)
        pthread_join(up[i], NULL);

    /* each upgrader now drops its weak references (after all its work) */
    Ctl *saved[NOBJ];
    for (int i = 0; i < NOBJ; i++)
        saved[i] = ctls[i];
    for (int t = 0; t < NUP; t++)
        for (int i = 0; i < NOBJ; i++)
            ctl_weak_release(saved[i]);

    int destroyed = atomic_load(&payloads_destroyed);
    int freed = atomic_load(&ctls_freed);
    int ok = 0, fail = 0;
    for (int i = 0; i < NUP; i++) {
        ok += upgrades_ok[i];
        fail += upgrades_fail[i];
    }
    printf("objects: %d, payloads destroyed: %d, control blocks freed: %d\n", NOBJ, destroyed, freed);
    printf("bad accesses through upgraded refs: %d\n", atomic_load(&bad_access));
    printf("upgrade attempts accounted: %d\n", ok + fail == NUP * 50 * NOBJ);
    if (destroyed != NOBJ || freed != NOBJ || atomic_load(&bad_access) != 0 ||
        ok + fail != NUP * 50 * NOBJ) {
        fprintf(stderr, "check failed: weak upgrade\n");
        return 1;
    }
    return 0;
}
