/*
 * title: Link-state routing: LSA flooding and SPF
 * topic: networking
 * covers: LSA sequence numbers, reliable flooding, LSDB synchronization, Dijkstra SPF, two-way connectivity check, Floyd-Warshall cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 8192
static unsigned rng_state = 1u;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

typedef struct { long t, ord; int type, node, a, b, c, d; } Ev;
static Ev evq[EVCAP];
static int evn;
static long now, ordc;
static int ev_less(const Ev *x, const Ev *y) { return x->t != y->t ? x->t < y->t : x->ord < y->ord; }
static void ev_push(long t, int type, int node, int a, int b, int c, int d) {
    if (evn >= EVCAP) fail("event queue overflow");
    Ev e = {t, ordc++, type, node, a, b, c, d};
    int i = evn++;
    evq[i] = e;
    while (i > 0 && ev_less(&evq[i], &evq[(i - 1) / 2])) {
        Ev tmp = evq[i]; evq[i] = evq[(i - 1) / 2]; evq[(i - 1) / 2] = tmp;
        i = (i - 1) / 2;
    }
}
static Ev ev_pop(void) {
    Ev top = evq[0];
    evq[0] = evq[--evn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < evn && ev_less(&evq[l], &evq[m])) m = l;
        if (r < evn && ev_less(&evq[r], &evq[m])) m = r;
        if (m == i) break;
        Ev tmp = evq[i]; evq[i] = evq[m]; evq[m] = tmp;
        i = m;
    }
    now = top.t;
    return top;
}

enum { N = 10, INFC = 1000000, MAXSEQ = 4 };
enum { EV_LSA };

typedef struct { int seq; int cost[N]; } Lsa;
static int truth[N][N];               /* actual topology, 0 = no link */
static Lsa store[N][MAXSEQ + 1];      /* content of LSA (origin, seq) */
static Lsa lsdb[N][N];                /* lsdb[router][origin] */
static long msgs, accepted, dropped_old;

static void originate(int r, int seq) {
    Lsa *l = &store[r][seq];
    l->seq = seq;
    for (int j = 0; j < N; j++) l->cost[j] = truth[r][j];
    lsdb[r][r] = *l;
    for (int j = 0; j < N; j++)
        if (truth[r][j]) { ev_push(now + truth[r][j], EV_LSA, j, r, seq, r, 0); msgs++; }
}
static void flood_run(void) {
    while (evn > 0) {
        Ev e = ev_pop();
        int to = e.node, origin = e.a, seq = e.b, from = e.c;
        if (seq <= lsdb[to][origin].seq) { dropped_old++; continue; }
        lsdb[to][origin] = store[origin][seq];
        accepted++;
        for (int j = 0; j < N; j++)
            if (truth[to][j] && j != from) { ev_push(now + truth[to][j], EV_LSA, j, origin, seq, to, 0); msgs++; }
    }
}

/* Dijkstra over router r's LSDB. With twoway, link u-v counts only if both u's and v's LSAs list it. */
static void spf(int r, int twoway, int *dist, int *first) {
    int done[N], par[N];
    for (int i = 0; i < N; i++) { dist[i] = INFC; done[i] = 0; par[i] = -1; first[i] = -1; }
    dist[r] = 0;
    for (int it = 0; it < N; it++) {
        int u = -1;
        for (int i = 0; i < N; i++)
            if (!done[i] && dist[i] < INFC && (u < 0 || dist[i] < dist[u])) u = i;
        if (u < 0) break;
        done[u] = 1;
        for (int v = 0; v < N; v++) {
            int c = lsdb[r][u].cost[v];
            if (!c) continue;
            if (twoway && !lsdb[r][v].cost[u]) continue;
            if (dist[u] + c < dist[v]) { dist[v] = dist[u] + c; par[v] = u; }
        }
    }
    for (int d = 0; d < N; d++) {
        if (d == r || dist[d] >= INFC) continue;
        int x = d;
        while (par[x] != r) x = par[x];
        first[d] = x;
    }
}

static void floyd(int fw[N][N]) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) fw[i][j] = i == j ? 0 : truth[i][j] ? truth[i][j] : INFC;
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
                if (fw[i][k] + fw[k][j] < fw[i][j]) fw[i][j] = fw[i][k] + fw[k][j];
}

static void verify_all(const char *phase) {
    int fw[N][N];
    floyd(fw);
    long total = 0;
    for (int r = 0; r < N; r++) {
        int dist[N], first[N];
        spf(r, 1, dist, first);
        for (int d = 0; d < N; d++) {
            check(dist[d] == fw[r][d], "Dijkstra on LSDB matches Floyd-Warshall");
            if (dist[d] < INFC) total += dist[d];
            if (d != r && dist[d] < INFC) {
                int f = first[d];
                check(truth[r][f] + fw[f][d] == fw[r][d], "first hop lies on a shortest path");
            }
        }
    }
    for (int r = 1; r < N; r++)
        for (int o = 0; o < N; o++) check(memcmp(&lsdb[r][o], &lsdb[0][o], sizeof(Lsa)) == 0, "LSDBs identical");
    printf("%s: msgs=%ld accepted=%ld stale-dropped=%ld sum of all-pairs distances=%ld\n", phase, msgs, accepted, dropped_old, total);
}

static void print_table(int r) {
    int dist[N], first[N];
    spf(r, 1, dist, first);
    printf("router %d table:", r);
    for (int d = 0; d < N; d++) {
        if (d == r) continue;
        if (dist[d] >= INFC) printf(" %d:unreach", d);
        else printf(" %d:via%d/%d", d, first[d], dist[d]);
    }
    printf("\n");
}

int main(void) {
    rng_state = 802u;
    for (int i = 0; i < N; i++) {
        int c = 1 + (int)(rnd() % 9);
        truth[i][(i + 1) % N] = truth[(i + 1) % N][i] = c;
    }
    int edges = N;
    while (edges < 16) {
        int a = (int)(rnd() % N);
        int b = (int)(rnd() % N);
        int c = 1 + (int)(rnd() % 9);
        if (a == b || truth[a][b]) continue;
        truth[a][b] = truth[b][a] = c;
        edges++;
    }
    printf("topology: %d routers, %d links\n", N, edges);
    for (int r = 0; r < N; r++) originate(r, 1);
    flood_run();
    verify_all("initial flood");
    print_table(0);

    /* kill two links, both endpoints re-originate with seq 2 */
    int killed = 0, ka[2], kb[2];
    for (int a = 0; a < N && killed < 2; a++)
        for (int b = a + 2; b < N && killed < 2; b++)
            if (truth[a][b] && !(a == 0 && b == N - 1)) { ka[killed] = a; kb[killed] = b; killed++; }
    for (int k = 0; k < killed; k++) truth[ka[k]][kb[k]] = truth[kb[k]][ka[k]] = 0;
    long m0 = msgs;
    for (int k = 0; k < killed; k++) { originate(ka[k], 2); originate(kb[k], 2); }
    flood_run();
    printf("killed links %d-%d and %d-%d\n", ka[0], kb[0], ka[1], kb[1]);
    verify_all("after failures");
    printf("reflood messages: %ld\n", msgs - m0);
    print_table(0);

    /* one-sided view: only one endpoint of a dead link re-originates (the other lost power), so its stale LSA still lists the link */
    /* search (deterministically) for a link and an observer where the stale entry changes some route */
    static Lsa saved_db[N][N];
    static int saved_truth[N][N];
    memcpy(saved_db, lsdb, sizeof lsdb);
    memcpy(saved_truth, truth, sizeof truth);
    int a = -1, b = -1, obs = -1;
    for (int x = 0; x < N && a < 0; x++)
        for (int y = x + 1; y < N && a < 0; y++) {
            if (!truth[x][y]) continue;
            truth[x][y] = truth[y][x] = 0;
            originate(y, 3);
            flood_run();
            for (int o = 0; o < N && a < 0; o++) {
                if (o == x || o == y) continue;
                int t1[N], t2[N], tf[N];
                spf(o, 1, t1, tf);
                spf(o, 0, t2, tf);
                for (int i = 0; i < N; i++)
                    if (t1[i] != t2[i]) { a = x; b = y; obs = o; break; }
            }
            memcpy(lsdb, saved_db, sizeof lsdb);
            memcpy(truth, saved_truth, sizeof truth);
        }
    check(a >= 0, "found a link whose one-sided failure matters");
    truth[a][b] = truth[b][a] = 0;
    originate(b, 3);
    flood_run();
    int d1[N], d2[N], f2[N];
    spf(obs, 1, d1, f2);
    spf(obs, 0, d2, f2);
    int diff = 0;
    for (int i = 0; i < N; i++) if (d1[i] != d2[i]) diff++;
    printf("one-sided failure %d-%d seen from router %d: dist to %d is %d with the two-way check, %d without\n", a, b, obs, b,
           d1[b], d2[b]);
    printf("  distances that differ: %d\n", diff);
    check(lsdb[obs][a].cost[b] != 0 && lsdb[obs][b].cost[a] == 0, "stale far-end advertisement");
    check(diff > 0, "ignoring the two-way check yields wrong routes");
    return 0;
}
