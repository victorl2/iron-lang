/*
 * title: Resource pool with checkout leases
 * topic: memory
 * covers: object pool, lease tokens, double-return detection, leak report, LRU-ish reuse, exhaustion
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_N 4

typedef struct {
    int id;
    int uses;
    int dirty;
    char label[16];
} Conn;

typedef struct {
    int slot;
    unsigned lease_id; /* unique per checkout, so stale leases are detected */
} Lease;

typedef struct {
    Conn conns[POOL_N];
    int state[POOL_N]; /* 0 idle, 1 leased */
    unsigned lease_of[POOL_N];
    int idle_order[POOL_N]; /* FIFO of idle slots */
    int idle_n;
    unsigned next_lease;
    int resets;
} Pool;

enum { R_OK, R_EXHAUSTED, R_STALE, R_DOUBLE };

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void pool_init(Pool *p) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < POOL_N; i++) {
        p->conns[i].id = 100 + i;
        p->idle_order[i] = i;
    }
    p->idle_n = POOL_N;
    p->next_lease = 1;
}

static int pool_checkout(Pool *p, const char *label, Lease *out, Conn **conn) {
    if (p->idle_n == 0)
        return R_EXHAUSTED;
    int slot = p->idle_order[0];
    memmove(p->idle_order, p->idle_order + 1, (size_t)(p->idle_n - 1) * sizeof(int));
    p->idle_n--;
    p->state[slot] = 1;
    p->lease_of[slot] = p->next_lease++;
    p->conns[slot].uses++;
    snprintf(p->conns[slot].label, sizeof p->conns[slot].label, "%s", label);
    out->slot = slot;
    out->lease_id = p->lease_of[slot];
    *conn = &p->conns[slot];
    return R_OK;
}

static int pool_return(Pool *p, Lease l) {
    if (l.slot < 0 || l.slot >= POOL_N)
        return R_STALE;
    if (!p->state[l.slot])
        return R_DOUBLE;
    if (p->lease_of[l.slot] != l.lease_id)
        return R_STALE;
    if (p->conns[l.slot].dirty) {
        p->conns[l.slot].dirty = 0; /* reset before reuse */
        p->resets++;
    }
    p->conns[l.slot].label[0] = 0;
    p->state[l.slot] = 0;
    p->lease_of[l.slot] = 0;
    p->idle_order[p->idle_n++] = l.slot;
    return R_OK;
}

static const char *rn(int r) {
    static const char *n[] = {"ok", "exhausted", "stale", "double-return"};
    return n[r];
}

static int leaked(const Pool *p, char *names, size_t cap) {
    int n = 0;
    names[0] = 0;
    for (int i = 0; i < POOL_N; i++)
        if (p->state[i]) {
            n++;
            size_t len = strlen(names);
            snprintf(names + len, cap - len, "%s%s#%d", len ? "," : "", p->conns[i].label,
                     p->conns[i].id);
        }
    return n;
}

int main(void) {
    Pool pool;
    pool_init(&pool);
    Lease l[6];
    Conn *c[6];
    const char *labels[] = {"alpha", "beta", "gamma", "delta"};
    for (int i = 0; i < 4; i++) {
        check(pool_checkout(&pool, labels[i], &l[i], &c[i]) == R_OK, "checkout");
        c[i]->dirty = i & 1;
    }
    printf("fifth checkout: %s\n", rn(pool_checkout(&pool, "eps", &l[4], &c[4])));

    printf("return beta: %s\n", rn(pool_return(&pool, l[1])));
    printf("return beta again: %s\n", rn(pool_return(&pool, l[1])));
    Lease again;
    Conn *cc;
    check(pool_checkout(&pool, "zeta", &again, &cc) == R_OK, "reuse");
    printf("reused conn id %d (uses=%d), lease %u\n", cc->id, cc->uses, again.lease_id);
    printf("stale lease returns: %s\n", rn(pool_return(&pool, l[1])));
    printf("resets so far: %d\n", pool.resets);

    char names[128];
    int n = leaked(&pool, names, sizeof names);
    printf("outstanding: %d [%s]\n", n, names);
    check(n == 4, "4 outstanding");

    int r0 = pool_return(&pool, l[0]);
    int r2 = pool_return(&pool, l[2]);
    int r3 = pool_return(&pool, l[3]);
    printf("return alpha,gamma,delta: %s %s %s\n", rn(r0), rn(r2), rn(r3));
    n = leaked(&pool, names, sizeof names);
    printf("outstanding now: %d [%s]\n", n, names);

    /* churn: FIFO reuse spreads the load evenly */
    for (int i = 0; i < 40; i++) {
        Lease t;
        Conn *tc;
        check(pool_checkout(&pool, "churn", &t, &tc) == R_OK, "churn checkout");
        tc->dirty = 1;
        check(pool_return(&pool, t) == R_OK, "churn return");
    }
    check(pool_return(&pool, again) == R_OK, "return zeta");
    for (int i = 0; i < POOL_N; i++)
        printf("conn %d uses=%d\n", pool.conns[i].id, pool.conns[i].uses);
    check(leaked(&pool, names, sizeof names) == 0, "no leaks at end");
    printf("resets total: %d, idle=%d\n", pool.resets, pool.idle_n);
    return 0;
}
