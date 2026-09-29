/*
 * title: Slot map with generational handles
 * topic: data_structures
 * covers: slot map, generation counters, stale handle detection, dense array, swap-remove, handle packing
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x243F6A8885A308D3ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 29); }

typedef uint32_t Handle; /* 20 bits of slot, 12 bits of generation */
#define SLOT_BITS 20u
#define GEN_MASK 0xFFFu
static Handle make(uint32_t slot, uint32_t gen) { return (gen << SLOT_BITS) | slot; }
static uint32_t h_slot(Handle h) { return h & ((1u << SLOT_BITS) - 1u); }
static uint32_t h_gen(Handle h) { return h >> SLOT_BITS; }

typedef struct { int hp; int x, y; } Entity;
typedef struct {
    uint32_t dense_to_slot;   /* which slot owns each dense entry */
    Entity e;
} Dense;
typedef struct { uint32_t gen; uint32_t dense; uint32_t next_free; int alive; } Slot;

typedef struct {
    Slot *slots; size_t nslots, cap_slots;
    Dense *dense; size_t ndense, cap_dense;
    uint32_t free_head; /* index+1, 0 for none */
    long stale_hits;
} SlotMap;

#define NOFREE 0u
static void sm_init(SlotMap *m) { memset(m, 0, sizeof *m); }
static void sm_free(SlotMap *m) { free(m->slots); free(m->dense); }
static Handle sm_insert(SlotMap *m, Entity e) {
    uint32_t s;
    if (m->free_head) { s = m->free_head - 1; m->free_head = m->slots[s].next_free; }
    else {
        if (m->nslots == m->cap_slots) { m->cap_slots = m->cap_slots ? m->cap_slots * 2 : 8; m->slots = realloc(m->slots, m->cap_slots * sizeof(Slot)); CHECK(m->slots); }
        s = (uint32_t)m->nslots++; m->slots[s].gen = 0;
    }
    if (m->ndense == m->cap_dense) { m->cap_dense = m->cap_dense ? m->cap_dense * 2 : 8; m->dense = realloc(m->dense, m->cap_dense * sizeof(Dense)); CHECK(m->dense); }
    m->slots[s].alive = 1; m->slots[s].dense = (uint32_t)m->ndense;
    m->dense[m->ndense].dense_to_slot = s; m->dense[m->ndense].e = e; m->ndense++;
    return make(s, m->slots[s].gen);
}
static int valid(const SlotMap *m, Handle h) {
    uint32_t s = h_slot(h);
    return s < m->nslots && m->slots[s].alive && m->slots[s].gen == h_gen(h);
}
static Entity *sm_get(SlotMap *m, Handle h) {
    if (!valid(m, h)) { m->stale_hits++; return NULL; }
    return &m->dense[m->slots[h_slot(h)].dense].e;
}
static int sm_remove(SlotMap *m, Handle h) {
    if (!valid(m, h)) { m->stale_hits++; return 0; }
    uint32_t s = h_slot(h), d = m->slots[s].dense, last = (uint32_t)m->ndense - 1;
    if (d != last) { m->dense[d] = m->dense[last]; m->slots[m->dense[d].dense_to_slot].dense = d; }
    m->ndense--;
    m->slots[s].alive = 0; m->slots[s].gen = (m->slots[s].gen + 1) & GEN_MASK;
    m->slots[s].next_free = m->free_head; m->free_head = s + 1;
    return 1;
}

#define MAXH 4000
int main(void) {
    SlotMap m; sm_init(&m);
    static Handle hs[MAXH]; static Entity ms_[MAXH]; static int alive[MAXH]; int nh = 0;
    long ins = 0, rem = 0, get = 0, stale_expected = 0, reused = 0;
    uint32_t max_gen = 0;
    long live = 0;
    for (int step = 0; step < 50000; step++) {
        unsigned op = rnd() % 100;
        if (live > 1500) op = 70;
        if (op < 40 && nh < MAXH) {
            Entity e = { (int)(rnd() % 100), (int)(rnd() % 1000), (int)(rnd() % 1000) };
            Handle h = sm_insert(&m, e);
            if (h_gen(h) > 0) reused++;
            if (h_gen(h) > max_gen) max_gen = h_gen(h);
            hs[nh] = h; ms_[nh] = e; alive[nh] = 1; nh++; live++; ins++;
        } else if (op < 75 && nh) {
            int i = (int)(rnd() % (unsigned)nh);
            int ok = sm_remove(&m, hs[i]);
            CHECK(ok == alive[i]);
            if (ok) { alive[i] = 0; live--; rem++; } else stale_expected++;
        } else if (nh) {
            int i = (int)(rnd() % (unsigned)nh);
            Entity *e = sm_get(&m, hs[i]);
            CHECK((e != NULL) == alive[i]);
            if (e) { CHECK(e->hp == ms_[i].hp && e->x == ms_[i].x && e->y == ms_[i].y); e->hp++; ms_[i].hp++; }
            get++;
        }
        CHECK(m.ndense == (size_t)live);
        if (nh == MAXH) { /* compact the handle table: keep live ones only */
            int k = 0; for (int i = 0; i < nh; i++) if (alive[i]) { hs[k] = hs[i]; ms_[k] = ms_[i]; alive[k] = 1; k++; }
            /* stale handles are dropped from the tracking table, so they are no longer tested */
            nh = k;
        }
    }
    /* dense array holds exactly the live entities; slots back-reference correctly */
    for (size_t d = 0; d < m.ndense; d++) { CHECK(m.slots[m.dense[d].dense_to_slot].dense == d && m.slots[m.dense[d].dense_to_slot].alive); }
    printf("insert=%ld remove=%ld get=%ld stale_removes=%ld\n", ins, rem, get, stale_expected);
    printf("live=%ld slots=%zu reused_slots=%ld max_generation=%u stale_lookups=%ld\n", live, m.nslots, reused, max_gen, m.stale_hits);
    long hp = 0; for (size_t d = 0; d < m.ndense; d++) hp += m.dense[d].e.hp;
    printf("total hp=%ld\n", hp);
    /* generation wraparound: a handle from 4096 generations ago collides again (documented limit) */
    SlotMap w; sm_init(&w);
    Entity z = {1, 0, 0}; Handle first = sm_insert(&w, z), h = first;
    for (int i = 0; i < 4096; i++) { CHECK(sm_remove(&w, h)); h = sm_insert(&w, z); }
    printf("after 4096 reuse cycles the first handle is %s\n", valid(&w, first) ? "valid again (gen wrapped)" : "invalid");
    sm_free(&w); sm_free(&m);
    return 0;
}
