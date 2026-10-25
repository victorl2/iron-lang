/*
 * title: Bitemporal history table with as-of queries in valid time and system time
 * topic: data_structures
 * covers: bitemporal data, valid-time intervals, transaction-time versions, range splitting, time-travel queries
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 31337u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define INF 1000000
#define NENT 6
#define VT 90          /* valid time axis 0..VT-1 */
#define MAXTX 260
#define MAXROWS 20000

/* a row says: entity had `val` during valid time [vfrom, vto), as recorded from tx_start until tx_end */
typedef struct { int ent, vfrom, vto, val, tx_start, tx_end; } Row;
static Row rows[MAXROWS];
static int nrows;
static int clock_tx;

static void add_row(int ent, int vfrom, int vto, int val, int tx) {
    CHECK(nrows < MAXROWS);
    Row r = { ent, vfrom, vto, val, tx, INF };
    rows[nrows++] = r;
}
/* val < 0 erases the range: closes rows but writes no replacement */
static void bt_update(int ent, int vfrom, int vto, int val) {
    int tx = ++clock_tx;
    int n = nrows;
    for (int i = 0; i < n; i++) {
        Row *r = &rows[i];
        if (r->ent != ent || r->tx_end != INF || r->vto <= vfrom || r->vfrom >= vto) continue;
        r->tx_end = tx;
        int lf = r->vfrom, lt = r->vto, v = r->val;
        if (lf < vfrom) add_row(ent, lf, vfrom, v, tx);
        if (lt > vto) add_row(ent, vto, lt, v, tx);
    }
    if (val >= 0) add_row(ent, vfrom, vto, val, tx);
}
/* value of entity at valid time vt as the database knew it at system time tx, or -1 */
static int bt_asof(int ent, int vt, int tx) {
    for (int i = 0; i < nrows; i++) {
        const Row *r = &rows[i];
        if (r->ent == ent && r->vfrom <= vt && vt < r->vto && r->tx_start <= tx && tx < r->tx_end) return r->val;
    }
    return -1;
}
/* how many distinct values did the database report for (ent, vt) over the whole system-time history */
static int bt_belief_changes(int ent, int vt) {
    int last = -2, changes = 0;
    for (int tx = 0; tx <= clock_tx; tx++) {
        int v = bt_asof(ent, vt, tx);
        if (v != last) { changes++; last = v; }
    }
    return changes;
}

static signed char model[MAXTX][NENT][VT];

int main(void) {
    memset(model, -1, sizeof model);
    for (int step = 0; step < MAXTX - 2; step++) {
        int ent = (int)(rnd() % NENT);
        int a = (int)(rnd() % VT), len = 1 + (int)(rnd() % 30);
        int b = a + len > VT ? VT : a + len;
        int val = rnd() % 5 == 0 ? -1 : (int)(rnd() % 100);
        bt_update(ent, a, b, val);
        int tx = clock_tx;
        memcpy(model[tx], model[tx - 1], sizeof model[tx]);
        for (int t = a; t < b; t++) model[tx][ent][t] = (signed char)val;
    }
    /* full time-travel grid check */
    long checked = 0;
    for (int tx = 0; tx <= clock_tx; tx += 3)
        for (int e = 0; e < NENT; e++)
            for (int t = 0; t < VT; t++) {
                CHECK(bt_asof(e, t, tx) == model[tx][e][t]);
                checked++;
            }
    /* the current rows never overlap in valid time for one entity */
    int current = 0;
    for (int i = 0; i < nrows; i++) {
        if (rows[i].tx_end != INF) continue;
        current++;
        for (int j = i + 1; j < nrows; j++)
            if (rows[j].tx_end == INF && rows[j].ent == rows[i].ent)
                CHECK(rows[i].vto <= rows[j].vfrom || rows[j].vto <= rows[i].vfrom);
    }
    printf("transactions=%d rows stored=%d current rows=%d, as-of cells verified=%ld\n", clock_tx, nrows, current, checked);
    for (int e = 0; e < NENT; e++) {
        int known = 0, changes = 0;
        for (int t = 0; t < VT; t++) {
            if (bt_asof(e, t, clock_tx) >= 0) known++;
            if (t % 10 == 0) changes += bt_belief_changes(e, t);
        }
        printf("entity %d: %2d of %d valid instants known now, %3d belief changes sampled\n", e, known, VT, changes);
    }
    /* a correction story: what did we believe about entity 0 at valid time 45, and when did that change? */
    printf("entity 0 @ valid 45 over system time:");
    int last = -2;
    for (int tx = 0; tx <= clock_tx; tx++) {
        int v = bt_asof(0, 45, tx);
        if (v != last) { printf(" [tx %d: %d]", tx, v); last = v; }
    }
    printf("\n");
    return 0;
}
