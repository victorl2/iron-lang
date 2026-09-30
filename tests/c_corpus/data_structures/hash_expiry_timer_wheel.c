/*
 * title: Expiring key-value store with hashed timing wheel
 * topic: data_structures
 * covers: TTL cache, hashed timing wheel, rounds counter, lazy plus active expiry, hash index, logical clock
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
enum { WHEEL = 64, CAP = 4096 };
typedef struct {
    uint32_t key;
    int val;
    uint64_t expiry;
    int next_hash, next_wheel, prev_wheel; /* index chain, doubly linked wheel slot */
    int used;
} Ent;
typedef struct {
    Ent e[CAP];
    int hash[1024];
    int wheel[WHEEL];
    int free_head, n;
    uint64_t now, swept;
    long expired_by_tick, lazy_miss;
} Store;

static void st_init(Store *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 1024; i++)
        s->hash[i] = -1;
    for (int i = 0; i < WHEEL; i++)
        s->wheel[i] = -1;
    for (int i = 0; i < CAP; i++)
        s->e[i].next_hash = i + 1 < CAP ? i + 1 : -1;
    s->free_head = 0;
}

static int st_find(const Store *s, uint32_t k) {
    for (int i = s->hash[mix32(k) & 1023]; i >= 0; i = s->e[i].next_hash)
        if (s->e[i].key == k)
            return i;
    return -1;
}

static void wheel_unlink(Store *s, int id) {
    Ent *e = &s->e[id];
    if (e->prev_wheel >= 0)
        s->e[e->prev_wheel].next_wheel = e->next_wheel;
    else
        s->wheel[e->expiry % WHEEL] = e->next_wheel;
    if (e->next_wheel >= 0)
        s->e[e->next_wheel].prev_wheel = e->prev_wheel;
}
static void wheel_link(Store *s, int id) {
    Ent *e = &s->e[id];
    int slot = (int)(e->expiry % WHEEL);
    e->prev_wheel = -1;
    e->next_wheel = s->wheel[slot];
    if (e->next_wheel >= 0)
        s->e[e->next_wheel].prev_wheel = id;
    s->wheel[slot] = id;
}

static void st_remove(Store *s, int id) {
    wheel_unlink(s, id);
    int *pp = &s->hash[mix32(s->e[id].key) & 1023];
    while (*pp != id)
        pp = &s->e[*pp].next_hash;
    *pp = s->e[id].next_hash;
    s->e[id].used = 0;
    s->e[id].next_hash = s->free_head;
    s->free_head = id;
    s->n--;
}

static void st_set(Store *s, uint32_t k, int v, uint64_t ttl) {
    int id = st_find(s, k);
    if (id >= 0) {
        wheel_unlink(s, id);
    } else {
        check(s->free_head >= 0, "capacity");
        id = s->free_head;
        s->free_head = s->e[id].next_hash;
        s->e[id].key = k;
        s->e[id].used = 1;
        size_t h = mix32(k) & 1023;
        s->e[id].next_hash = s->hash[h];
        s->hash[h] = id;
        s->n++;
    }
    s->e[id].val = v;
    s->e[id].expiry = s->now + ttl;
    wheel_link(s, id);
}

/* lazy check: an entry is alive while now < expiry */
static int st_get(Store *s, uint32_t k, int *v) {
    int id = st_find(s, k);
    if (id < 0)
        return 0;
    if (s->e[id].expiry <= s->now) {
        s->lazy_miss++; /* the tick has not swept it yet, but it is logically gone */
        return 0;
    }
    *v = s->e[id].val;
    return 1;
}

/* the clock moves on its own; the wheel cursor catches up only when a sweep runs */
static void st_sweep(Store *s) {
    while (s->swept < s->now) {
        s->swept++;
        int slot = (int)(s->swept % WHEEL);
        for (int id = s->wheel[slot], nx; id >= 0; id = nx) {
            nx = s->e[id].next_wheel;
            if (s->e[id].expiry <= s->swept) { /* entries from a later round stay in the slot */
                st_remove(s, id);
                s->expired_by_tick++;
            }
        }
    }
}

int main(void) {
    static Store s;
    st_init(&s);
    static uint32_t rk[CAP];
    static int rv[CAP];
    static uint64_t rexp[CAP];
    int rn = 0;
    long sets = 0, gets = 0, live_hits = 0;
    for (int step = 0; step < 50000; step++) {
        uint32_t k = (uint32_t)(rnd() % 600);
        int op = (int)(rnd() % 10);
        if (op < 3) {
            int v = (int)(rnd() % 1000);
            uint64_t ttl = 1 + rnd() % 200; /* longer than the wheel: uses multiple rounds */
            if (s.n < CAP - 1 || st_find(&s, k) >= 0) {
                st_set(&s, k, v, ttl);
                int ri = -1;
                for (int i = 0; i < rn; i++)
                    if (rk[i] == k)
                        ri = i;
                if (ri < 0)
                    ri = rn++;
                rk[ri] = k;
                rv[ri] = v;
                rexp[ri] = s.now + ttl;
                sets++;
            }
        } else if (op < 8) {
            int v = 0, got = st_get(&s, k, &v), ri = -1;
            for (int i = 0; i < rn; i++)
                if (rk[i] == k)
                    ri = i;
            int want = ri >= 0 && rexp[ri] > s.now;
            check(got == want, "get agrees with reference clock");
            if (got)
                check(v == rv[ri], "value");
            gets++;
            live_hits += got;
        } else if (op == 8) {
            s.now += 1 + rnd() % 4; /* time passes, nothing is swept yet */
        } else {
            st_sweep(&s);
            /* after a sweep the wheel must have removed every entry whose expiry has passed */
            for (int i = 0; i < CAP; i++)
                if (s.e[i].used)
                    check(s.e[i].expiry > s.now, "no expired entry survives a sweep");
        }
    }
    int alive = 0;
    for (int i = 0; i < CAP; i++)
        alive += s.e[i].used;
    check(alive == s.n, "used count");
    printf("clock=%llu sets=%ld gets=%ld live hits=%ld\n", (unsigned long long)s.now, sets, gets, live_hits);
    printf("stored entries=%d expired by wheel=%ld lazy misses before sweep=%ld\n", s.n, s.expired_by_tick, s.lazy_miss);
    int maxslot = 0;
    for (int w = 0; w < WHEEL; w++) {
        int c = 0;
        for (int id = s.wheel[w]; id >= 0; id = s.e[id].next_wheel)
            c++;
        if (c > maxslot)
            maxslot = c;
    }
    printf("fullest wheel slot=%d of %d slots\n", maxslot, WHEEL);
    return 0;
}
