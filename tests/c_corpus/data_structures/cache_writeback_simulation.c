/*
 * title: Write-back versus write-through cache simulation
 * topic: data_structures
 * covers: set-associative cache, dirty bits, write-allocate, write-back on eviction, write-through no-allocate, memory traffic, flat-memory oracle, flush
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS 4096
#define LINE 4
#define SETS 16
#define WAYS 4

static unsigned long long rs = 0x8B17E4ACULL * 0x51;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int valid, dirty, tag; long lru; unsigned data[LINE]; } Line;
typedef struct {
    Line set[SETS][WAYS];
    unsigned mem[WORDS];
    int write_back;       /* 1: write-back + write-allocate, 0: write-through + no-allocate */
    long tick;
    long rd, wr, rhit, whit, line_fills, line_writebacks, word_writes;
} Cache;

static void cinit(Cache *c, int wb) {
    memset(c, 0, sizeof *c); c->write_back = wb;
    for (int i = 0; i < WORDS; i++) c->mem[i] = (unsigned)i * 2654435761u;
}
static Line *lookup(Cache *c, int addr, int *set_out) {
    int blk = addr / LINE, s = blk % SETS, tag = blk / SETS;
    *set_out = s;
    for (int w = 0; w < WAYS; w++) if (c->set[s][w].valid && c->set[s][w].tag == tag) return &c->set[s][w];
    return NULL;
}
static Line *fill(Cache *c, int addr, int s) {
    int blk = addr / LINE, victim = 0;
    for (int w = 0; w < WAYS; w++) {
        if (!c->set[s][w].valid) { victim = w; break; }
        if (c->set[s][w].lru < c->set[s][victim].lru) victim = w;
    }
    Line *l = &c->set[s][victim];
    if (l->valid && l->dirty) {
        int base = (l->tag * SETS + s) * LINE;
        memcpy(&c->mem[base], l->data, sizeof l->data); c->line_writebacks++;
    }
    memcpy(l->data, &c->mem[blk * LINE], sizeof l->data); c->line_fills++;
    l->valid = 1; l->dirty = 0; l->tag = blk / SETS;
    return l;
}
static unsigned cread(Cache *c, int addr) {
    int s; c->rd++;
    Line *l = lookup(c, addr, &s);
    if (l) c->rhit++; else l = fill(c, addr, s);
    l->lru = ++c->tick;
    return l->data[addr % LINE];
}
static void cwrite(Cache *c, int addr, unsigned v) {
    int s; c->wr++;
    Line *l = lookup(c, addr, &s);
    if (c->write_back) {
        if (l) c->whit++; else l = fill(c, addr, s);
        l->data[addr % LINE] = v; l->dirty = 1; l->lru = ++c->tick;
    } else {
        c->mem[addr] = v; c->word_writes++;
        if (l) { c->whit++; l->data[addr % LINE] = v; l->lru = ++c->tick; }
    }
}
static void flush(Cache *c) {
    for (int s = 0; s < SETS; s++) for (int w = 0; w < WAYS; w++) {
        Line *l = &c->set[s][w];
        if (l->valid && l->dirty) { memcpy(&c->mem[(l->tag * SETS + s) * LINE], l->data, sizeof l->data); l->dirty = 0; c->line_writebacks++; }
    }
}
static void verify_clean_lines(const Cache *c) {
    for (int s = 0; s < SETS; s++) for (int w = 0; w < WAYS; w++) {
        const Line *l = &c->set[s][w];
        if (l->valid && !l->dirty) check(memcmp(l->data, &c->mem[(l->tag * SETS + s) * LINE], sizeof l->data) == 0, "clean line equals memory");
    }
}

static int pick_addr(int phase) {
    unsigned r = rnd() % 100;
    switch (phase) {
    case 0: return (int)(rnd() % 256);                                     /* small working set */
    case 1: { static int a; a = (a + 1) % 1024; return a; }                /* streaming */
    default: return r < 80 ? (int)(rnd() % 128) : (int)(rnd() % WORDS);    /* hot + noise */
    }
}

int main(void) {
    for (int wb = 1; wb >= 0; wb--) {
        Cache *c = malloc(sizeof *c); cinit(c, wb);
        unsigned *model = malloc(WORDS * sizeof *model); memcpy(model, c->mem, WORDS * sizeof *model);
        rs = 0x8B17E4ACULL * 0x51;
        for (int phase = 0; phase < 3; phase++) {
            for (int i = 0; i < 20000; i++) {
                int a = pick_addr(phase);
                if (rnd() % 100 < 35) { unsigned v = rnd(); cwrite(c, a, v); model[a] = v; }
                else check(cread(c, a) == model[a], "read returns latest write");
            }
            verify_clean_lines(c);
        }
        if (wb) check(memcmp(c->mem, model, WORDS * sizeof *model) != 0, "write-back leaves memory stale before flush");
        flush(c);
        check(memcmp(c->mem, model, WORDS * sizeof *model) == 0, "memory equals model after flush");
        long traffic = c->line_fills * LINE + c->line_writebacks * LINE + c->word_writes;
        printf("%-13s reads=%ld (hit %ld) writes=%ld (hit %ld) fills=%ld writebacks=%ld word_writes=%ld traffic_words=%ld\n",
               wb ? "write-back" : "write-through", c->rd, c->rhit, c->wr, c->whit, c->line_fills, c->line_writebacks, c->word_writes, traffic);
        free(c); free(model);
    }
    return 0;
}
