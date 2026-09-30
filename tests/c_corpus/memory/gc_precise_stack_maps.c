/*
 * title: Precise stack maps from liveness analysis versus scanning every slot
 * topic: memory
 * covers: straight-line function bodies with slots, backward liveness giving a live-slot mask per safepoint, masks at allocation sites and at suspended call sites, frames scanned by mask, two-pass dynamic-liveness oracle, retention of a conservative all-slots scan, exact survivor equality
 * deps: libc
 */
#define SEED 0x5EED0028ULL
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*@RNG*/
/* Deterministic splitmix64 generator so every platform sees the same run. */
static uint64_t rng_s = SEED;
static inline uint32_t rnd(void) {
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (uint32_t)(z >> 32);
}
static inline uint32_t rnd_n(uint32_t n) {
    uint32_t r = rnd();
    return r % n;
}

/*@ENDRNG*/
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__);    \
            exit(1);                                                          \
        }                                                                     \
    } while (0)



#define NF 5
#define NSLOT 5
#define MAXI 64
#define MAXOBJ 3000
#define MAXACT 3000
#define MAXEV 40000
enum { I_NEW, I_LINK, I_USE, I_CALL, I_NIL, I_COPY };
static const char *op_name[] = {"new", "link", "use", "call", "nil", "copy"};

typedef struct {
    int op, a, b;
} Ins;
static Ins body[NF][MAXI];
static int blen[NF];
static unsigned live_in[NF][MAXI + 1], live_out[NF][MAXI + 1];

typedef struct {
    int id, f[2];
    unsigned char used, mark;
} Obj;
static Obj heap[MAXOBJ];
static int heap_cap, freelist, nfree;
static int next_id;

typedef struct {
    int fn, pc, act;
    int slot[NSLOT];
} Frame;
static Frame frames[NF + 1];
static int depth;
static int now, next_act;
static int pass; /* 1 records dynamic events with a huge heap, 2 runs the real small heap */

/* dynamic events: read/def of (activation, slot) at time t */
typedef struct {
    int act, slot, t, kind; /* kind 0 read, 1 def */
} Ev;
static Ev evs[MAXEV];
static int nev;
static int ev_start[MAXACT * NSLOT + 1], ev_list[MAXEV];
static long instrs, allocs, gcs, retained_precise, retained_conservative, checks;

static unsigned use_mask(const Ins *i) {
    switch (i->op) {
    case I_LINK: return (1u << i->a) | (1u << i->b);
    case I_USE: return 1u << i->a;
    case I_CALL: return 1u << i->b;
    case I_COPY: return 1u << i->b;
    default: return 0;
    }
}
static unsigned def_mask(const Ins *i) {
    switch (i->op) {
    case I_NEW: case I_NIL: case I_COPY: return 1u << i->a;
    default: return 0;
    }
}

static void analyse(void) {
    for (int fn = 0; fn < NF; fn++) {
        unsigned live = 0;
        live_out[fn][blen[fn]] = live_in[fn][blen[fn]] = 0;
        for (int pc = blen[fn] - 1; pc >= 0; pc--) {
            live_out[fn][pc] = live;
            live = (live & ~def_mask(&body[fn][pc])) | use_mask(&body[fn][pc]);
            live_in[fn][pc] = live;
        }
    }
}

static void gen_program(void) {
    for (int fn = 0; fn < NF; fn++) {
        blen[fn] = fn == 0 ? 60 : 12 + (int)rnd_n(5);
        for (int pc = 0; pc < blen[fn]; pc++) {
            Ins *i = &body[fn][pc];
            unsigned r = rnd_n(20);
            i->a = (int)rnd_n(NSLOT);
            i->b = (int)rnd_n(NSLOT);
            if (r < 6)
                i->op = I_NEW;
            else if (r < 10)
                i->op = I_LINK;
            else if (r < 12)
                i->op = I_USE;
            else if (r < 14)
                i->op = I_COPY;
            else if (r < 16)
                i->op = I_NIL;
            else if (fn < NF - 1) {
                i->op = I_CALL;
                i->a = fn + 1 + (int)rnd_n((unsigned)(NF - 1 - fn)); /* callee index */
            } else
                i->op = I_NEW;
        }
    }
}

static void record(int act, int slot, int kind) {
    if (pass != 1)
        return;
    CHECK(nev < MAXEV);
    evs[nev].act = act;
    evs[nev].slot = slot;
    evs[nev].t = now;
    evs[nev].kind = kind;
    nev++;
}

static void build_index(void) {
    int count[MAXACT * NSLOT] = {0};
    for (int e = 0; e < nev; e++)
        count[evs[e].act * NSLOT + evs[e].slot]++;
    ev_start[0] = 0;
    for (int k = 0; k < MAXACT * NSLOT; k++)
        ev_start[k + 1] = ev_start[k] + count[k];
    int fillc[MAXACT * NSLOT] = {0};
    for (int e = 0; e < nev; e++) {
        int k = evs[e].act * NSLOT + evs[e].slot;
        ev_list[ev_start[k] + fillc[k]++] = e;
    }
}

/* First event on (act, slot) at time >= t decides: a read means the slot is still needed. */
static int dyn_live(int act, int slot, int t) {
    int k = act * NSLOT + slot;
    for (int i = ev_start[k]; i < ev_start[k + 1]; i++) {
        const Ev *e = &evs[ev_list[i]];
        if (e->t >= t)
            return e->kind == 0;
    }
    return 0;
}

static void mark_obj(int o) {
    if (o < 0 || heap[o].mark)
        return;
    heap[o].mark = 1;
    mark_obj(heap[o].f[0]);
    mark_obj(heap[o].f[1]);
}

static void gc(void) {
    /* expected survivors from the recorded dynamic liveness */
    unsigned char expect[MAXOBJ], seen[MAXOBJ];
    memset(seen, 0, sizeof seen);
    int stack[MAXOBJ], sp = 0;
    for (int d = 0; d < depth; d++)
        for (int s = 0; s < NSLOT; s++) {
            int o = frames[d].slot[s];
            if (o >= 0 && dyn_live(frames[d].act, s, now) && !seen[o]) {
                seen[o] = 1;
                stack[sp++] = o;
            }
        }
    while (sp > 0) {
        int o = stack[--sp];
        for (int k = 0; k < 2; k++)
            if (heap[o].f[k] >= 0 && !seen[heap[o].f[k]]) {
                seen[heap[o].f[k]] = 1;
                stack[sp++] = heap[o].f[k];
            }
    }
    memcpy(expect, seen, sizeof expect);
    /* what scanning every slot would retain, for comparison */
    for (int d = 0; d < depth; d++)
        for (int s = 0; s < NSLOT; s++)
            mark_obj(frames[d].slot[s] >= 0 && heap[frames[d].slot[s]].used ? frames[d].slot[s] : -1);
    int cons = 0;
    for (int i = 0; i < heap_cap; i++) {
        cons += heap[i].mark;
        heap[i].mark = 0;
    }
    /* the collector: mask-directed root scan */
    for (int d = 0; d < depth; d++) {
        unsigned mask = d == depth - 1 ? live_in[frames[d].fn][frames[d].pc] : live_out[frames[d].fn][frames[d].pc];
        for (int s = 0; s < NSLOT; s++)
            if ((mask >> s) & 1)
                mark_obj(frames[d].slot[s]);
    }
    int prec = 0;
    freelist = -1;
    nfree = 0;
    for (int i = heap_cap - 1; i >= 0; i--) {
        if (heap[i].used && !heap[i].mark)
            heap[i].used = 0;
        CHECK((heap[i].used != 0) == (expect[i] != 0)); /* exactly the dynamically live closure survives */
        prec += heap[i].used;
        heap[i].mark = 0;
        if (!heap[i].used) {
            heap[i].f[0] = freelist;
            freelist = i;
            nfree++;
        }
    }
    retained_precise += prec;
    retained_conservative += cons;
    gcs++;
    checks++;
}

static int new_obj(void) {
    if (freelist < 0)
        gc();
    CHECK(freelist >= 0);
    int o = freelist;
    freelist = heap[o].f[0];
    nfree--;
    heap[o].used = 1;
    heap[o].id = next_id++;
    heap[o].f[0] = heap[o].f[1] = -1;
    allocs++;
    return o;
}

static void exec_fn(int fn, int arg) {
    Frame *f = &frames[depth++];
    f->fn = fn;
    f->act = next_act++;
    for (int s = 0; s < NSLOT; s++)
        f->slot[s] = -1;
    f->slot[0] = arg;
    record(f->act, 0, 1);
    for (int pc = 0; pc < blen[fn]; pc++) {
        Ins *i = &body[fn][pc];
        f->pc = pc;
        instrs++;
        unsigned rd = use_mask(i);
        for (int s = 0; s < NSLOT; s++)
            if ((rd >> s) & 1)
                record(f->act, s, 0);
        switch (i->op) {
        case I_NEW: {
            int o = new_obj(); /* may collect; f->slot[a] still holds its old, dead value */
            f->slot[i->a] = o;
            break;
        }
        case I_LINK:
            if (f->slot[i->a] >= 0 && f->slot[i->b] >= 0) {
                CHECK(heap[f->slot[i->a]].used && heap[f->slot[i->b]].used);
                heap[f->slot[i->a]].f[i->a & 1] = f->slot[i->b];
            }
            break;
        case I_USE:
            if (f->slot[i->a] >= 0)
                CHECK(heap[f->slot[i->a]].used);
            break;
        case I_NIL:
            f->slot[i->a] = -1;
            break;
        case I_COPY:
            f->slot[i->a] = f->slot[i->b];
            break;
        case I_CALL:
            now++;
            exec_fn(i->a, f->slot[i->b]);
            now--;
            break;
        }
        unsigned df = def_mask(i);
        for (int s = 0; s < NSLOT; s++)
            if ((df >> s) & 1)
                record(f->act, s, 1);
        now++;
    }
    depth--;
}

static void run(int cap) {
    heap_cap = cap;
    memset(heap, 0, sizeof heap);
    freelist = -1;
    nfree = 0;
    for (int i = cap - 1; i >= 0; i--) {
        heap[i].f[0] = freelist;
        freelist = i;
        nfree++;
    }
    next_id = 0;
    next_act = 0;
    now = 0;
    depth = 0;
    instrs = allocs = gcs = 0;
    exec_fn(0, -1);
}

int main(void) {
    gen_program();
    analyse();
    pass = 1;
    nev = 0;
    run(MAXOBJ);
    long total_instrs = instrs, total_allocs = allocs;
    build_index();
    CHECK(gcs == 0);
    pass = 2;
    run(44);
    CHECK(instrs == total_instrs && allocs == total_allocs);
    printf("function 1 body with live-slot masks (slot 0 is the argument; bit s set = slot s live before the instruction):\n");
    for (int pc = 0; pc < blen[1]; pc++) {
        char m[NSLOT + 1];
        for (int s = 0; s < NSLOT; s++)
            m[s] = ((live_in[1][pc] >> s) & 1) ? '1' : '.';
        m[NSLOT] = 0;
        Ins *i = &body[1][pc];
        if (i->op == I_CALL)
            printf("  %2d %-4s f%d(slot %d)     %s\n", pc, op_name[i->op], i->a, i->b, m);
        else
            printf("  %2d %-4s %d %d           %s\n", pc, op_name[i->op], i->a, i->b, m);
    }
    printf("instructions: %ld, allocations: %ld, collections: %ld\n", instrs, allocs, gcs);
    printf("survivors summed over collections: masks %ld, all slots scanned %ld\n", retained_precise,
           retained_conservative);
    printf("garbage a slot-conservative scan would retain: %ld\n", retained_conservative - retained_precise);
    printf("oracle checks: %ld\n", checks);
    return 0;
}
