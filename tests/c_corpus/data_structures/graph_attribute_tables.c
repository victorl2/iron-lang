/*
 * title: Property graph with columnar vertex and edge attribute tables
 * topic: data_structures
 * covers: property graph, struct of arrays, string interning, attribute filters, group-by aggregation, edge properties, join by id
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NV 60
#define NE 200

static unsigned long long rs = 0xA77121B0ULL * 4099;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* string interning for labels */
static const char *labels[] = {"person", "company", "city", "product"};
static const char *rels[] = {"knows", "works_at", "located_in", "makes", "buys"};
#define NL 4
#define NR 5

typedef struct {
    int n;
    unsigned char label[NV];
    int age[NV];
    int score[NV];
    char name[NV][8];
} VertexTable;
typedef struct {
    int m;
    int src[NE], dst[NE];
    unsigned char rel[NE];
    int weight[NE];
    int since[NE];
} EdgeTable;

static VertexTable vt;
static EdgeTable et;
/* secondary index: CSR by source over the edge table */
static int off[NV + 1], ord[NE];

static void build_index(void) {
    memset(off, 0, sizeof off);
    for (int i = 0; i < et.m; i++) off[et.src[i] + 1]++;
    for (int v = 0; v < NV; v++) off[v + 1] += off[v];
    int pos[NV]; memcpy(pos, off, sizeof pos);
    for (int i = 0; i < et.m; i++) ord[pos[et.src[i]]++] = i;
}

int main(void) {
    vt.n = NV;
    for (int v = 0; v < NV; v++) {
        vt.label[v] = (unsigned char)(rnd() % NL);
        vt.age[v] = 18 + (int)(rnd() % 60);
        vt.score[v] = (int)(rnd() % 1000);
        vt.name[v][0] = (char)('a' + rnd() % 26); vt.name[v][1] = (char)('a' + rnd() % 26);
        vt.name[v][2] = (char)('0' + v / 10); vt.name[v][3] = (char)('0' + v % 10); vt.name[v][4] = 0;
    }
    et.m = NE;
    for (int i = 0; i < NE; i++) {
        et.src[i] = (int)(rnd() % NV); et.dst[i] = (int)(rnd() % NV);
        et.rel[i] = (unsigned char)(rnd() % NR); et.weight[i] = 1 + (int)(rnd() % 20);
        et.since[i] = 1990 + (int)(rnd() % 35);
    }
    build_index();

    /* group by vertex label: count, mean age, max score */
    printf("vertices by label\n");
    int total = 0;
    for (int l = 0; l < NL; l++) {
        int c = 0, agesum = 0, best = -1, bestv = -1;
        for (int v = 0; v < NV; v++) if (vt.label[v] == l) {
            c++; agesum += vt.age[v];
            if (vt.score[v] > best) { best = vt.score[v]; bestv = v; }
        }
        total += c;
        if (c) printf("  %-8s n=%2d age_sum=%4d top=%s(%d)\n", labels[l], c, agesum, vt.name[bestv], best);
        else printf("  %-8s n=0\n", labels[l]);
    }
    check(total == NV, "label partition");

    /* edge filter with a join: relation r, both endpoints' labels, weight >= w */
    int hist[NL][NL]; memset(hist, 0, sizeof hist);
    for (int i = 0; i < NE; i++) hist[vt.label[et.src[i]]][vt.label[et.dst[i]]]++;
    printf("edge label matrix\n");
    int hsum = 0;
    for (int a = 0; a < NL; a++) {
        printf("  %-8s", labels[a]);
        for (int b = 0; b < NL; b++) { printf(" %3d", hist[a][b]); hsum += hist[a][b]; }
        printf("\n");
    }
    check(hsum == NE, "edge label matrix total");

    /* out-neighbourhood query through the index vs full scan */
    long idx_sum = 0, scan_sum = 0;
    for (int v = 0; v < NV; v++) {
        for (int k = off[v]; k < off[v + 1]; k++) {
            int e = ord[k];
            if (et.rel[e] == 0 && et.since[e] >= 2010) idx_sum += (long)et.weight[e] * vt.age[et.dst[e]];
        }
        for (int e = 0; e < NE; e++)
            if (et.src[e] == v && et.rel[e] == 0 && et.since[e] >= 2010) scan_sum += (long)et.weight[e] * vt.age[et.dst[e]];
    }
    check(idx_sum == scan_sum, "index vs scan");
    printf("weighted knows since 2010 (dst age): %ld\n", idx_sum);

    /* per-relation stats, weighted degree top 3 (ties by id) */
    for (int r = 0; r < NR; r++) {
        int c = 0, wsum = 0, minsince = 9999, maxsince = 0;
        for (int e = 0; e < NE; e++) if (et.rel[e] == r) {
            c++; wsum += et.weight[e];
            if (et.since[e] < minsince) minsince = et.since[e];
            if (et.since[e] > maxsince) maxsince = et.since[e];
        }
        printf("rel %-10s count=%2d weight=%3d since=[%d,%d]\n", rels[r], c, wsum, c ? minsince : 0, maxsince);
    }
    int wdeg[NV]; memset(wdeg, 0, sizeof wdeg);
    for (int e = 0; e < NE; e++) wdeg[et.src[e]] += et.weight[e];
    int used[NV] = {0};
    printf("top weighted out-degree:");
    for (int k = 0; k < 3; k++) {
        int b = -1;
        for (int v = 0; v < NV; v++) if (!used[v] && (b < 0 || wdeg[v] > wdeg[b])) b = v;
        used[b] = 1;
        printf(" %s=%d", vt.name[b], wdeg[b]);
    }
    printf("\n");
    return 0;
}
