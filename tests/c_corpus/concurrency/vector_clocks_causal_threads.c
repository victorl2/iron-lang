/*
 * title: Vector clocks and causal order detection
 * topic: concurrency
 * covers: vector clocks, happened-before, concurrent events, transitive closure cross-check, message passing threads
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NP = 4, NEV = 48, LOCAL = 0, SEND = 1, RECV = 2, MAXEV = 100, MAXALL = NP * MAXEV };

typedef struct {
    int type, peer;
} Ev;

static Ev prog[NP][MAXEV];
static int prog_len[NP];

typedef struct {
    int vc[MAXEV][NP];
    int head, tail;
} Queue;
static Queue q[NP][NP];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static int vstamp[NP][MAXEV][NP];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *process(void *arg) {
    int me = (int)(long)arg;
    int vc[NP] = {0};
    for (int e = 0; e < prog_len[me]; e++) {
        Ev ev = prog[me][e];
        if (ev.type == RECV) {
            pthread_mutex_lock(&mu);
            Queue *c = &q[ev.peer][me];
            while (c->head == c->tail)
                pthread_cond_wait(&cv, &mu);
            int in[NP];
            memcpy(in, c->vc[c->head++], sizeof in);
            pthread_mutex_unlock(&mu);
            for (int k = 0; k < NP; k++)
                if (in[k] > vc[k])
                    vc[k] = in[k];
        }
        vc[me]++;
        if (ev.type == SEND) {
            pthread_mutex_lock(&mu);
            Queue *c = &q[me][ev.peer];
            memcpy(c->vc[c->tail++], vc, sizeof vc);
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
        }
        memcpy(vstamp[me][e], vc, sizeof vc);
    }
    return NULL;
}

static int id_of[NP][MAXEV];
static char reach[MAXALL][MAXALL];

/* a -> b iff a's vector is <= b's and they differ */
static int vc_before(const int *a, const int *b) {
    int le = 1, ne = 0;
    for (int k = 0; k < NP; k++) {
        if (a[k] > b[k])
            le = 0;
        if (a[k] != b[k])
            ne = 1;
    }
    return le && ne;
}

static unsigned rng_s = 555777u;
static unsigned rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

int main(void) {
    int pending[NP][NP] = {{0}};
    for (int step = 0; step < NEV; step++) {
        int p = (int)(rnd() % NP);
        int r = (int)(rnd() % 10);
        int o = (int)(rnd() % (NP - 1));
        int peer = o >= p ? o + 1 : o;
        if (r < 3) {
            prog[p][prog_len[p]++] = (Ev){LOCAL, -1};
        } else if (r < 6) {
            prog[p][prog_len[p]++] = (Ev){SEND, peer};
            pending[p][peer]++;
        } else {
            int from = -1;
            for (int k = 0; k < NP && from < 0; k++) {
                int cand = (peer + k) % NP;
                if (cand != p && pending[cand][p] > 0)
                    from = cand;
            }
            if (from >= 0) {
                prog[p][prog_len[p]++] = (Ev){RECV, from};
                pending[from][p]--;
            } else {
                prog[p][prog_len[p]++] = (Ev){LOCAL, -1};
            }
        }
    }
    for (int a = 0; a < NP; a++)
        for (int b = 0; b < NP; b++)
            while (pending[a][b] > 0) {
                prog[b][prog_len[b]++] = (Ev){RECV, a};
                pending[a][b]--;
            }

    pthread_t th[NP];
    for (long i = 0; i < NP; i++)
        check(pthread_create(&th[i], NULL, process, (void *)i) == 0, "create");
    for (int i = 0; i < NP; i++)
        pthread_join(th[i], NULL);

    /* ground truth: program order plus send->receive edges, then transitive closure */
    int n = 0;
    for (int p = 0; p < NP; p++)
        for (int e = 0; e < prog_len[p]; e++)
            id_of[p][e] = n++;
    int sends[NP][NP][MAXEV], nsend[NP][NP], nrecv[NP][NP];
    memset(nsend, 0, sizeof nsend);
    memset(nrecv, 0, sizeof nrecv);
    for (int p = 0; p < NP; p++)
        for (int e = 0; e < prog_len[p]; e++)
            if (prog[p][e].type == SEND) {
                int b = prog[p][e].peer;
                sends[p][b][nsend[p][b]++] = id_of[p][e];
            }
    for (int p = 0; p < NP; p++)
        for (int e = 0; e < prog_len[p]; e++) {
            if (e > 0)
                reach[id_of[p][e - 1]][id_of[p][e]] = 1;
            if (prog[p][e].type == RECV) {
                int a = prog[p][e].peer;
                reach[sends[a][p][nrecv[a][p]++]][id_of[p][e]] = 1;
            }
        }
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++)
            if (reach[i][k])
                for (int j = 0; j < n; j++)
                    if (reach[k][j])
                        reach[i][j] = 1;

    int flat_p[MAXALL], flat_e[MAXALL];
    for (int p = 0; p < NP; p++)
        for (int e = 0; e < prog_len[p]; e++) {
            flat_p[id_of[p][e]] = p;
            flat_e[id_of[p][e]] = e;
        }
    long ordered = 0, concurrent = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            const int *vi = vstamp[flat_p[i]][flat_e[i]];
            const int *vj = vstamp[flat_p[j]][flat_e[j]];
            int i_before_j = vc_before(vi, vj), j_before_i = vc_before(vj, vi);
            check(i_before_j == reach[i][j] && j_before_i == reach[j][i], "vector clocks match happened-before");
            if (i_before_j || j_before_i)
                ordered++;
            else
                concurrent++;
        }
    for (int p = 0; p < NP; p++) {
        printf("process %d: events=%d final vector=[", p, prog_len[p]);
        for (int k = 0; k < NP; k++)
            printf("%s%d", k ? "," : "", vstamp[p][prog_len[p] - 1][k]);
        printf("]\n");
    }
    printf("events=%d pairs=%ld causally ordered=%ld concurrent=%ld\n", n, ordered + concurrent, ordered, concurrent);
    check(ordered + concurrent == (long)n * (n - 1) / 2, "pair count");
    /* an example of concurrency: first pair found on different processes */
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = i + 1; j < n && !found; j++)
            if (!reach[i][j] && !reach[j][i]) {
                printf("first concurrent pair: P%d.e%d || P%d.e%d\n", flat_p[i], flat_e[i], flat_p[j], flat_e[j]);
                found = 1;
            }
        if (found)
            break;
    }
    return 0;
}
