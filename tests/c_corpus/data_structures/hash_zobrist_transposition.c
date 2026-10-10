/*
 * title: Zobrist hashing with incremental update and transposition table
 * topic: data_structures
 * covers: zobrist keys, incremental xor update, make/unmake symmetry, transposition table replacement, position repetition, collision audit
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
enum { SQ = 64, PIECES = 6, NP = 4, WALK = 30000, TT_SIZE = 1024 };
static uint64_t zob[PIECES][SQ];
static uint64_t zob_side;

typedef struct {
    int8_t sq[SQ]; /* -1 empty else piece type */
    int pos[NP];   /* where each of the NP pieces stands */
    int type[NP];
    uint64_t hash;
    int side;
} Board;

static uint64_t full_hash(const Board *b) {
    uint64_t h = b->side ? zob_side : 0;
    for (int s = 0; s < SQ; s++)
        if (b->sq[s] >= 0)
            h ^= zob[b->sq[s]][s];
    return h;
}

typedef struct {
    int piece, from, to;
} Move;

static void make(Board *b, Move m) {
    b->hash ^= zob[b->type[m.piece]][m.from] ^ zob[b->type[m.piece]][m.to] ^ zob_side; /* incremental */
    b->sq[m.from] = -1;
    b->sq[m.to] = (int8_t)b->type[m.piece];
    b->pos[m.piece] = m.to;
    b->side ^= 1;
}
static void unmake(Board *b, Move m) {
    b->hash ^= zob[b->type[m.piece]][m.from] ^ zob[b->type[m.piece]][m.to] ^ zob_side;
    b->sq[m.to] = -1;
    b->sq[m.from] = (int8_t)b->type[m.piece];
    b->pos[m.piece] = m.from;
    b->side ^= 1;
}

typedef struct {
    uint64_t key;
    int visits;
    int used;
} TT;

/* exact reference: positions compared byte by byte, indexed by an unrelated FNV hash */
enum { EX = 16384 };
typedef struct {
    uint8_t cells[SQ];
    int side, count, used;
} Exact;
static Exact ex[EX];
static int exact_visit(const Board *b) {
    uint32_t h = 2166136261u;
    for (int s = 0; s < SQ; s++)
        h = (h ^ (uint8_t)(b->sq[s] + 1)) * 16777619u;
    h = (h ^ (uint32_t)b->side) * 16777619u;
    size_t i = h & (EX - 1);
    for (;;) {
        if (!ex[i].used) {
            for (int s = 0; s < SQ; s++)
                ex[i].cells[s] = (uint8_t)(b->sq[s] + 1);
            ex[i].side = b->side;
            ex[i].used = 1;
            ex[i].count = 1;
            return 1;
        }
        int same = ex[i].side == b->side;
        for (int s = 0; s < SQ && same; s++)
            same = ex[i].cells[s] == (uint8_t)(b->sq[s] + 1);
        if (same) {
            ex[i].count++;
            return ex[i].count;
        }
        i = (i + 1) & (EX - 1);
    }
}

int main(void) {
    for (int p = 0; p < PIECES; p++)
        for (int s = 0; s < SQ; s++)
            zob[p][s] = rnd();
    zob_side = rnd();
    Board b;
    memset(&b, 0, sizeof b);
    for (int s = 0; s < SQ; s++)
        b.sq[s] = -1;
    int start[NP] = {0, 9, 18, 1};
    for (int i = 0; i < NP; i++) {
        b.type[i] = i % PIECES;
        b.pos[i] = start[i];
        b.sq[start[i]] = (int8_t)b.type[i];
    }
    b.hash = full_hash(&b);
    uint64_t initial = b.hash;
    static Move hist[WALK];
    static TT tt[TT_SIZE];
    static uint64_t seen[EX * 2]; /* keys of distinct positions per the zobrist view, open addressing */
    int distinct_z = 0, distinct_exact = 0, reps3 = 0;
    long tt_hits = 0, tt_replace = 0, tt_index_conflicts = 0;
    int nmoves = 0, maxcount = 0;
    while (nmoves < WALK) {
        int pc = (int)(rnd() % NP);
        int dir = (int)(rnd() % 4);
        int r = b.pos[pc] / 8, c = b.pos[pc] % 8;
        r += dir == 0 ? -1 : dir == 1 ? 1 : 0;
        c += dir == 2 ? -1 : dir == 3 ? 1 : 0;
        if (r < 0 || r > 2 || c < 0 || c > 2 || b.sq[r * 8 + c] >= 0) /* keep pieces in the top-left 3x3 corner */
            continue;
        Move m = {pc, b.pos[pc], r * 8 + c};
        make(&b, m);
        hist[nmoves++] = m;
        check(b.hash == full_hash(&b), "incremental hash equals full recompute");
        /* transposition table: replace on mismatch, count visits on match */
        TT *e = &tt[b.hash % TT_SIZE];
        if (e->used && e->key == b.hash) {
            e->visits++;
            tt_hits++;
        } else {
            tt_index_conflicts += e->used;
            tt_replace++;
            e->used = 1;
            e->key = b.hash;
            e->visits = 1;
        }
        /* distinct positions by zobrist key */
        size_t i = (size_t)(b.hash % (EX * 2));
        while (seen[i] && seen[i] != b.hash)
            i = (i + 1) % (EX * 2);
        if (!seen[i]) {
            seen[i] = b.hash;
            distinct_z++;
        }
        int cnt = exact_visit(&b);
        if (cnt == 1)
            distinct_exact++;
        if (cnt == 3)
            reps3++;
        if (cnt > maxcount)
            maxcount = cnt;
    }
    check(distinct_z == distinct_exact, "zobrist key collisions absent: distinct counts agree");
    uint64_t final_hash = b.hash;
    /* undo the whole game: hash must return to the initial value exactly */
    for (int i = nmoves - 1; i >= 0; i--)
        unmake(&b, hist[i]);
    check(b.hash == initial && b.hash == full_hash(&b), "unmake restores hash");
    printf("moves=%d distinct positions=%d (exact) = %d (zobrist) max repetitions of one position=%d, third repetitions=%d\n", nmoves,
           distinct_exact, distinct_z, maxcount, reps3);
    printf("transposition table %d entries: hits=%ld replacements=%ld of which evictions=%ld\n", TT_SIZE, tt_hits, tt_replace,
           tt_index_conflicts);
    printf("hash restored after unmake; final position hash low 16 bits=%04x\n", (unsigned)(final_hash & 0xffff));
    return 0;
}
