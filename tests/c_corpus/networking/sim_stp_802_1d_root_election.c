/*
 * title: Spanning tree protocol root election and port roles
 * topic: networking
 * covers: 802.1D bridge IDs, priority vectors, root port, designated port, blocking, message age, re-election after root failure
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

enum { NB = 6, NL = 7, NP = 15, MAXAGE = 6, HOLD = 3 };
enum { R_ROOT, R_DESIG, R_BLOCK };
static const char *rname[] = {"root", "desig", "block"};

typedef struct { int valid; long root; int cost; long sender; int sport; int age; int stale; } Bpdu;
typedef struct { int bridge, lan, cost, id; } Port;

static Port ports[NP];
static int np;
static long bid[NB];
static int alive[NB];
static Bpdu rx[NP];
static int role[NP];
static long b_root[NB];
static int b_cost[NB], b_rootport[NB];

static void add_port(int b, int lan, int cost) {
    ports[np].bridge = b; ports[np].lan = lan; ports[np].cost = cost;
    int cnt = 0;
    for (int i = 0; i < np; i++) if (ports[i].bridge == b) cnt++;
    ports[np].id = cnt + 1;
    np++;
}

/* compare priority vectors: negative if a is better (smaller) */
static int vec_cmp(long r1, int c1, long s1, int p1, long r2, int c2, long s2, int p2) {
    if (r1 != r2) return r1 < r2 ? -1 : 1;
    if (c1 != c2) return c1 < c2 ? -1 : 1;
    if (s1 != s2) return s1 < s2 ? -1 : 1;
    if (p1 != p2) return p1 < p2 ? -1 : 1;
    return 0;
}

static void recompute(void) {
    for (int b = 0; b < NB; b++) {
        if (!alive[b]) continue;
        int best = -1;
        for (int i = 0; i < np; i++) {
            if (ports[i].bridge != b || !rx[i].valid || rx[i].root >= bid[b]) continue;
            if (best < 0 ||
                vec_cmp(rx[i].root, rx[i].cost + ports[i].cost, rx[i].sender, rx[i].sport, rx[best].root,
                        rx[best].cost + ports[best].cost, rx[best].sender, rx[best].sport) < 0 ||
                (rx[i].root == rx[best].root && rx[i].cost + ports[i].cost == rx[best].cost + ports[best].cost &&
                 rx[i].sender == rx[best].sender && rx[i].sport == rx[best].sport && ports[i].id < ports[best].id))
                best = i;
        }
        if (best < 0) { b_root[b] = bid[b]; b_cost[b] = 0; b_rootport[b] = -1; }
        else { b_root[b] = rx[best].root; b_cost[b] = rx[best].cost + ports[best].cost; b_rootport[b] = best; }
        for (int i = 0; i < np; i++) {
            if (ports[i].bridge != b) continue;
            if (i == b_rootport[b]) { role[i] = R_ROOT; continue; }
            if (!rx[i].valid || vec_cmp(b_root[b], b_cost[b], bid[b], ports[i].id, rx[i].root, rx[i].cost, rx[i].sender, rx[i].sport) < 0)
                role[i] = R_DESIG;
            else role[i] = R_BLOCK;
        }
    }
}

static void round_(void) {
    Bpdu out[NP];
    memset(out, 0, sizeof out);
    for (int i = 0; i < np; i++) {
        int b = ports[i].bridge;
        if (!alive[b] || role[i] != R_DESIG) continue;
        int age = b_rootport[b] < 0 ? 0 : rx[b_rootport[b]].age + 1;
        if (age >= MAXAGE) continue;
        out[i].valid = 1; out[i].root = b_root[b]; out[i].cost = b_cost[b]; out[i].sender = bid[b];
        out[i].sport = ports[i].id; out[i].age = age;
    }
    for (int q = 0; q < np; q++) {
        if (!alive[ports[q].bridge]) continue;
        int best = -1;
        for (int i = 0; i < np; i++) {
            if (i == q || ports[i].lan != ports[q].lan || !out[i].valid) continue;
            if (best < 0 || vec_cmp(out[i].root, out[i].cost, out[i].sender, out[i].sport, out[best].root, out[best].cost,
                                    out[best].sender, out[best].sport) < 0)
                best = i;
        }
        if (best >= 0) { rx[q] = out[best]; rx[q].stale = 0; }
        else if (rx[q].valid && ++rx[q].stale > HOLD) rx[q].valid = 0;
    }
    recompute();
}

static void snapshot(int *sig) {
    for (int i = 0; i < np; i++) sig[i] = role[i];
    for (int b = 0; b < NB; b++) sig[np + b] = alive[b] ? (int)b_root[b] * 7 + b_cost[b] : -1;
}
static int run_until_stable(int *rounds_out) {
    int prev[NP + NB], cur[NP + NB], same = 0, r = 0;
    snapshot(prev);
    while (same < MAXAGE + HOLD + 2 && r < 200) {
        round_();
        r++;
        snapshot(cur);
        if (memcmp(prev, cur, sizeof cur) == 0) same++; else { same = 0; memcpy(prev, cur, sizeof cur); }
    }
    *rounds_out = r - same;
    return r;
}

static void show(const char *title) {
    printf("%s\n", title);
    for (int b = 0; b < NB; b++) {
        if (!alive[b]) { printf("  B%d down\n", b); continue; }
        printf("  B%d id=%ld root=%ld cost=%d ports:", b, bid[b], b_root[b], b_cost[b]);
        for (int i = 0; i < np; i++)
            if (ports[i].bridge == b) printf(" L%d/%s", ports[i].lan, rname[role[i]]);
        printf("\n");
    }
}

/* the forwarding graph (root + designated ports) must be a tree over alive bridges and their LANs */
static void check_tree(void) {
    int parent[NB + NL], nodes = 0, edges = 0;
    for (int i = 0; i < NB + NL; i++) parent[i] = i;
    int lan_used[NL] = {0};
    for (int i = 0; i < np; i++) {
        if (!alive[ports[i].bridge] || role[i] == R_BLOCK) continue;
        lan_used[ports[i].lan] = 1;
        int a = ports[i].bridge, l = NB + ports[i].lan;
        while (parent[a] != a) a = parent[a];
        while (parent[l] != l) l = parent[l];
        check(a != l, "forwarding ports create no loop");
        parent[a] = l;
        edges++;
    }
    for (int b = 0; b < NB; b++) if (alive[b]) nodes++;
    for (int l = 0; l < NL; l++) if (lan_used[l]) nodes++;
    check(edges == nodes - 1, "spanning tree edge count");
    int rootrep = -1;
    for (int b = 0; b < NB; b++) {
        if (!alive[b]) continue;
        int a = b;
        while (parent[a] != a) a = parent[a];
        check(rootrep < 0 || rootrep == a, "tree is connected");
        rootrep = a;
    }
}

int main(void) {
    long prio[NB] = {32768, 32768, 32768, 4096, 32768, 32768};
    for (int b = 0; b < NB; b++) { bid[b] = prio[b] * 100 + 10 + b; alive[b] = 1; }
    /* LAN membership: L0:B0,B1  L1:B0,B2  L2:B1,B2,B3  L3:B3,B4  L4:B2,B4,B5  L5:B4,B5  L6:B5 */
    add_port(0, 0, 19); add_port(1, 0, 19);
    add_port(0, 1, 4);  add_port(2, 1, 4);
    add_port(1, 2, 19); add_port(2, 2, 19); add_port(3, 2, 19);
    add_port(3, 3, 4);  add_port(4, 3, 4);
    add_port(2, 4, 100); add_port(4, 4, 100); add_port(5, 4, 100);
    add_port(4, 5, 19); add_port(5, 5, 19); add_port(5, 6, 100);
    check(np == NP, "port count");
    memset(role, 0, sizeof role);
    for (int b = 0; b < NB; b++) { b_root[b] = bid[b]; b_cost[b] = 0; b_rootport[b] = -1; }
    for (int i = 0; i < np; i++) role[i] = R_DESIG;
    int rounds;
    run_until_stable(&rounds);
    show("initial convergence");
    printf("  stable after %d rounds\n", rounds);
    check_tree();
    check(b_root[0] == bid[3] && b_root[5] == bid[3], "lowest priority bridge is root");
    check(b_cost[3] == 0 && b_rootport[3] < 0, "root has no root port");

    alive[3] = 0;
    for (int i = 0; i < np; i++) if (ports[i].bridge == 3) role[i] = R_BLOCK;
    run_until_stable(&rounds);
    show("root bridge B3 failed");
    printf("  stable after %d rounds\n", rounds);
    check_tree();
    long newroot = bid[0];
    check(b_root[0] == newroot && b_root[4] == newroot && b_root[5] == newroot, "B0 wins the election with the lowest remaining id");

    alive[3] = 1;
    for (int i = 0; i < np; i++) if (ports[i].bridge == 3) { role[i] = R_DESIG; rx[i].valid = 0; }
    b_root[3] = bid[3]; b_cost[3] = 0; b_rootport[3] = -1;
    run_until_stable(&rounds);
    show("B3 returns");
    check_tree();
    check(b_root[0] == bid[3] && b_root[5] == bid[3], "root reclaimed");
    int nblock = 0;
    for (int i = 0; i < np; i++) if (alive[ports[i].bridge] && role[i] == R_BLOCK) nblock++;
    printf("blocked ports in final topology: %d\n", nblock);
    check(nblock >= 1, "redundant links blocked");
    return 0;
}
