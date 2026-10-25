/*
 * title: TTL cache with lazy expiry, sliding refresh and sweep queue
 * topic: data_structures
 * covers: TTL cache, virtual clock, absolute and sliding expiry, lazy deletion on access, versioned expiry queue, open addressing, oracle scan
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE 512
#define KEYS 200
#define QCAP 65536

static unsigned long long rs = 0x77121C4EULL * 91;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int state; int key, val; long expires; unsigned ver; int ttl; int sliding; } Ent;   /* state 0 empty, 1 live, 2 tombstone */
static Ent tab[TABLE];
static int live_count;
/* expiry queue: entries pushed in nondecreasing expiry only when TTL is constant per class; we use a
 * binary heap keyed by expiry so mixed TTLs work. Stale heap items (older version) are skipped. */
typedef struct { long exp; int key; unsigned ver; } Item;
static Item heap[QCAP]; static int hn;
static long swept, lazily_expired, refreshed, heap_peak;

static void hpush(Item it) {
    int i = hn++;
    while (i > 0) { int p = (i - 1) / 2; if (heap[p].exp <= it.exp) break; heap[i] = heap[p]; i = p; }
    heap[i] = it;
    if (hn > heap_peak) heap_peak = hn;
}
static Item hpop(void) {
    Item top = heap[0], last = heap[--hn];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= hn) break;
        if (c + 1 < hn && heap[c + 1].exp < heap[c].exp) c++;
        if (heap[c].exp >= last.exp) break;
        heap[i] = heap[c]; i = c;
    }
    if (hn) heap[i] = last;
    return top;
}

static unsigned h(int k) { return ((unsigned)k * 2654435761u) >> 20; }
static int find_slot(int key) {
    unsigned i = h(key) % TABLE;
    for (int n = 0; n < TABLE; n++, i = (i + 1) % TABLE) {
        if (tab[i].state == 0) return -1;
        if (tab[i].state == 1 && tab[i].key == key) return (int)i;
    }
    return -1;
}
static void remove_slot(int i) { tab[i].state = 2; live_count--; }
static long now;

static void put(int key, int val, int ttl, int sliding) {
    int i = find_slot(key);
    if (i < 0) {
        unsigned j = h(key) % TABLE;
        while (tab[j].state == 1) j = (j + 1) % TABLE;
        i = (int)j; tab[i].state = 1; tab[i].ver = 0; live_count++;
    }
    tab[i].key = key; tab[i].val = val; tab[i].ttl = ttl; tab[i].sliding = sliding;
    tab[i].expires = now + ttl; tab[i].ver++;
    Item it = { tab[i].expires, key, tab[i].ver }; hpush(it);
}
static int get(int key, int *val) {
    int i = find_slot(key);
    if (i < 0) return 0;
    if (tab[i].expires <= now) { remove_slot(i); lazily_expired++; return 0; }
    if (tab[i].sliding) {
        tab[i].expires = now + tab[i].ttl; tab[i].ver++; refreshed++;
        Item it = { tab[i].expires, key, tab[i].ver }; hpush(it);
    }
    *val = tab[i].val;
    return 1;
}
static void sweep(void) {
    while (hn && heap[0].exp <= now) {
        Item it = hpop();
        int i = find_slot(it.key);
        if (i >= 0 && tab[i].ver == it.ver && tab[i].expires <= now) { remove_slot(i); swept++; }
    }
}
static void compact_if_needed(void) {   /* rebuild table when tombstones pile up */
    int tomb = 0; for (int i = 0; i < TABLE; i++) tomb += tab[i].state == 2;
    if (tomb < TABLE / 4) return;
    Ent old[TABLE]; memcpy(old, tab, sizeof tab); memset(tab, 0, sizeof tab);
    for (int i = 0; i < TABLE; i++) if (old[i].state == 1) {
        unsigned j = h(old[i].key) % TABLE;
        while (tab[j].state == 1) j = (j + 1) % TABLE;
        tab[j] = old[i];
    }
}

/* oracle */
static int rlive[KEYS], rval[KEYS], rttl[KEYS], rsl[KEYS]; static long rexp[KEYS];

int main(void) {
    long hits = 0, misses = 0, expired_seen = 0;
    for (int step = 0; step < 40000; step++) {
        now += (long)(rnd() % 2);
        int k = (int)(rnd() % KEYS); unsigned op = rnd() % 10;
        if (op < 3) {
            int ttl = 20 + (int)(rnd() % 400), sl = (int)(rnd() & 1u);
            put(k, step, ttl, sl);
            rlive[k] = 1; rval[k] = step; rttl[k] = ttl; rsl[k] = sl; rexp[k] = now + ttl;
        } else if (op < 9) {
            int v = 0, got = get(k, &v);
            int want = rlive[k] && rexp[k] > now;
            if (rlive[k] && rexp[k] <= now) { rlive[k] = 0; expired_seen++; }
            check(got == want, "get presence with expiry");
            if (got) { check(v == rval[k], "value"); hits++; if (rsl[k]) rexp[k] = now + rttl[k]; } else misses++;
        } else {
            sweep();
            int expect = 0;
            for (int i = 0; i < KEYS; i++) { if (rlive[i] && rexp[i] <= now) rlive[i] = 0; expect += rlive[i]; }
            check(live_count == expect, "live count after sweep");
            compact_if_needed();
        }
    }
    sweep();
    int expect = 0; for (int i = 0; i < KEYS; i++) { if (rlive[i] && rexp[i] <= now) rlive[i] = 0; expect += rlive[i]; }
    check(live_count == expect, "final live count");
    printf("hits=%ld misses=%ld lazy_expired=%ld swept=%ld sliding_refreshes=%ld live=%d clock=%ld heap_peak_ok=%d\n",
           hits, misses, lazily_expired, swept, refreshed, live_count, now, heap_peak < QCAP);
    (void)expired_seen;
    return 0;
}
