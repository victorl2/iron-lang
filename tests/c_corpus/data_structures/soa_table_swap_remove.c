/*
 * title: Struct-of-arrays entity table with swap-remove and permutation sort
 * topic: data_structures
 * covers: struct of arrays, dense packing, id to row map, in-place permutation, columnar update loops
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 5551212u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define CAP 512
/* each column is its own array; row i of the table is index i in every column */
typedef struct {
    uint32_t id[CAP];
    int32_t x[CAP], y[CAP];
    int16_t vx[CAP], vy[CAP];
    int32_t hp[CAP];
    uint8_t team[CAP];
    int n;
    int row_of[CAP * 4];   /* id -> row or -1 */
    uint32_t next_id;
} Table;

static void t_init(Table *t) {
    t->n = 0; t->next_id = 1;
    for (int i = 0; i < CAP * 4; i++) t->row_of[i] = -1;
}
static uint32_t t_spawn(Table *t, int x, int y, int vx, int vy, int hp, int team) {
    CHECK(t->n < CAP);
    int r = t->n++;
    uint32_t id = t->next_id++;
    t->id[r] = id; t->x[r] = x; t->y[r] = y; t->vx[r] = (int16_t)vx; t->vy[r] = (int16_t)vy;
    t->hp[r] = hp; t->team[r] = (uint8_t)team;
    t->row_of[id] = r;
    return id;
}
static void move_row(Table *t, int dst, int src) {
    t->id[dst] = t->id[src]; t->x[dst] = t->x[src]; t->y[dst] = t->y[src];
    t->vx[dst] = t->vx[src]; t->vy[dst] = t->vy[src]; t->hp[dst] = t->hp[src]; t->team[dst] = t->team[src];
    t->row_of[t->id[dst]] = dst;
}
static void t_kill(Table *t, uint32_t id) {
    int r = t->row_of[id];
    CHECK(r >= 0);
    t->row_of[id] = -1;
    int last = t->n - 1;
    if (r != last) move_row(t, r, last);
    t->n--;
}
/* system 1: integrate positions (touches only four columns) */
static void t_step(Table *t) {
    for (int i = 0; i < t->n; i++) { t->x[i] += t->vx[i]; t->y[i] += t->vy[i]; }
}
/* system 2: damage by team, returns ids that died */
static int t_damage(Table *t, int team, int amount, uint32_t *dead) {
    int nd = 0;
    for (int i = 0; i < t->n; i++)
        if (t->team[i] == team) { t->hp[i] -= amount; if (t->hp[i] <= 0) dead[nd++] = t->id[i]; }
    return nd;
}
/* apply a permutation (new row i = old row perm[i]) to every column in place by following cycles */
static void apply_perm(Table *t, int *perm) {
    unsigned char done[CAP];
    memset(done, 0, sizeof done);
    for (int s = 0; s < t->n; s++) {
        if (done[s] || perm[s] == s) { done[s] = 1; continue; }
        uint32_t id = t->id[s]; int32_t x = t->x[s], y = t->y[s], hp = t->hp[s];
        int16_t vx = t->vx[s], vy = t->vy[s]; uint8_t team = t->team[s];
        int i = s;
        for (;;) {
            int j = perm[i];
            done[i] = 1;
            if (j == s) break;
            move_row(t, i, j);
            i = j;
        }
        t->id[i] = id; t->x[i] = x; t->y[i] = y; t->vx[i] = vx; t->vy[i] = vy; t->hp[i] = hp; t->team[i] = team;
        t->row_of[id] = i;
    }
}
static const int32_t *sort_key;
static int cmp_perm(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    if (sort_key[i] != sort_key[j]) return sort_key[i] < sort_key[j] ? -1 : 1;
    return 0;
}
static void t_sort_by_x(Table *t) {
    int perm[CAP];
    int32_t keys[CAP];
    /* make the order total: key = x, ties broken by id through a composite pass */
    for (int i = 0; i < t->n; i++) perm[i] = i;
    for (int i = 0; i < t->n; i++) keys[i] = t->x[i];
    sort_key = keys;
    /* insertion sort for a stable, libc-independent order */
    for (int i = 1; i < t->n; i++) {
        int v = perm[i], j = i - 1;
        while (j >= 0 && cmp_perm(&perm[j], &v) > 0) { perm[j + 1] = perm[j]; j--; }
        perm[j + 1] = v;
    }
    apply_perm(t, perm);
}

/* array-of-structs model keyed by id */
typedef struct { int alive; int32_t x, y, hp; int16_t vx, vy; uint8_t team; } Ent;

int main(void) {
    static Table t;
    static Ent model[CAP * 4];
    t_init(&t);
    long spawned = 0, killed = 0, steps = 0, damaged = 0;
    for (int round = 0; round < 3000; round++) {
        unsigned op = rnd() % 20;
        if (op < 8 && t.n < CAP - 1 && t.next_id < CAP * 4 - 1) {
            int x = (int)(rnd() % 2001) - 1000, y = (int)(rnd() % 2001) - 1000;
            int vx = (int)(rnd() % 21) - 10, vy = (int)(rnd() % 21) - 10;
            int hp = 10 + (int)(rnd() % 90), team = (int)(rnd() % 4);
            uint32_t id = t_spawn(&t, x, y, vx, vy, hp, team);
            Ent e = { 1, x, y, hp, (int16_t)vx, (int16_t)vy, (uint8_t)team };
            model[id] = e;
            spawned++;
        } else if (op < 11 && t.n > 0) {
            uint32_t id = t.id[rnd() % (unsigned)t.n];
            t_kill(&t, id);
            model[id].alive = 0;
            killed++;
        } else if (op < 15) {
            t_step(&t);
            for (uint32_t id = 1; id < t.next_id; id++)
                if (model[id].alive) { model[id].x += model[id].vx; model[id].y += model[id].vy; }
            steps++;
        } else if (op < 18) {
            int team = (int)(rnd() % 4), amount = 5 + (int)(rnd() % 20);
            uint32_t dead[CAP];
            int nd = t_damage(&t, team, amount, dead);
            for (uint32_t id = 1; id < t.next_id; id++)
                if (model[id].alive && model[id].team == team) model[id].hp -= amount;
            for (int i = 0; i < nd; i++) { t_kill(&t, dead[i]); model[dead[i]].alive = 0; killed++; }
            damaged += nd;
        } else {
            t_sort_by_x(&t);
            for (int i = 1; i < t.n; i++) CHECK(t.x[i - 1] <= t.x[i]);
        }
        if (round % 50 == 0) {
            int alive = 0;
            for (uint32_t id = 1; id < t.next_id; id++) {
                int r = t.row_of[id];
                CHECK((r >= 0) == model[id].alive);
                if (r < 0) continue;
                alive++;
                CHECK(t.id[r] == id && t.x[r] == model[id].x && t.y[r] == model[id].y);
                CHECK(t.hp[r] == model[id].hp && t.team[r] == model[id].team);
                CHECK(t.vx[r] == model[id].vx && t.vy[r] == model[id].vy);
            }
            CHECK(alive == t.n);
        }
    }
    long sumx = 0, sumhp = 0;
    int per_team[4] = { 0 };
    for (int i = 0; i < t.n; i++) { sumx += t.x[i]; sumhp += t.hp[i]; per_team[t.team[i]]++; }
    printf("spawned=%ld killed=%ld (by damage %ld) steps=%ld\n", spawned, killed, damaged, steps);
    printf("alive=%d sum x=%ld sum hp=%ld teams=%d/%d/%d/%d\n", t.n, sumx, sumhp, per_team[0], per_team[1], per_team[2], per_team[3]);
    return 0;
}
