/*
 * title: Lamport logical clocks between threads
 * topic: concurrency
 * covers: Lamport clocks, FIFO message queues, clock condition, total order by (timestamp, pid), trace replay
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NP = 4, NEV = 70, LOCAL = 0, SEND = 1, RECV = 2, MAXEV = 200 };

typedef struct {
    int type, peer;
} Ev;

static Ev prog[NP][MAXEV];
static int prog_len[NP];

/* one FIFO queue per ordered pair of processes */
typedef struct {
    long ts[MAXEV];
    int head, tail;
} Queue;
static Queue q[NP][NP];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static long stamp[NP][MAXEV];     /* Lamport timestamp of each event, written by its own thread */
static long msg_send_ts[NP][NP][MAXEV]; /* timestamp carried by the k-th message of a channel */

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *process(void *arg) {
    int me = (int)(long)arg;
    long clock = 0;
    for (int e = 0; e < prog_len[me]; e++) {
        Ev ev = prog[me][e];
        if (ev.type == LOCAL) {
            clock++;
        } else if (ev.type == SEND) {
            clock++;
            pthread_mutex_lock(&mu);
            Queue *c = &q[me][ev.peer];
            c->ts[c->tail++] = clock;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
        } else {
            pthread_mutex_lock(&mu);
            Queue *c = &q[ev.peer][me];
            while (c->head == c->tail)
                pthread_cond_wait(&cv, &mu);
            long t = c->ts[c->head++];
            pthread_mutex_unlock(&mu);
            clock = (t > clock ? t : clock) + 1;
        }
        stamp[me][e] = clock;
    }
    return NULL;
}

typedef struct {
    long ts;
    int pid, idx;
} Rec;

static int cmp_rec(const void *a, const void *b) {
    const Rec *x = a, *y = b;
    if (x->ts != y->ts)
        return x->ts < y->ts ? -1 : 1;
    if (x->pid != y->pid)
        return x->pid < y->pid ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx);
}

static unsigned rng_s = 20240607u;
static unsigned rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

int main(void) {
    /* Build one global trace serially, then split it into per-process programs. */
    int pending[NP][NP] = {{0}};
    for (int step = 0; step < NEV; step++) {
        int p = (int)(rnd() % NP);
        int r = (int)(rnd() % 10);
        int o = (int)(rnd() % (NP - 1));
        int peer = o >= p ? o + 1 : o;
        if (r < 3) {
            prog[p][prog_len[p]++] = (Ev){LOCAL, -1};
        } else if (r < 7) {
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
    for (int a = 0; a < NP; a++) /* drain: every message is eventually received */
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

    /* clock condition: every receive is later than its matching send (FIFO matching per channel) */
    int sent_seen[NP][NP], recv_seen[NP][NP];
    memset(sent_seen, 0, sizeof sent_seen);
    memset(recv_seen, 0, sizeof recv_seen);
    for (int a = 0; a < NP; a++)
        for (int e = 0; e < prog_len[a]; e++)
            if (prog[a][e].type == SEND) {
                int b = prog[a][e].peer;
                msg_send_ts[a][b][sent_seen[a][b]++] = stamp[a][e];
            }
    long messages = 0;
    for (int b = 0; b < NP; b++)
        for (int e = 0; e < prog_len[b]; e++)
            if (prog[b][e].type == RECV) {
                int a = prog[b][e].peer;
                long st = msg_send_ts[a][b][recv_seen[a][b]++];
                check(st < stamp[b][e], "clock condition");
                messages++;
            }
    for (int a = 0; a < NP; a++)
        for (int b = 0; b < NP; b++)
            check(sent_seen[a][b] == recv_seen[a][b], "all messages delivered");
    /* local order must be strictly increasing */
    for (int p = 0; p < NP; p++)
        for (int e = 1; e < prog_len[p]; e++)
            check(stamp[p][e] > stamp[p][e - 1], "local monotonic");

    Rec all[NP * MAXEV];
    int n = 0;
    for (int p = 0; p < NP; p++)
        for (int e = 0; e < prog_len[p]; e++)
            all[n++] = (Rec){stamp[p][e], p, e};
    qsort(all, (size_t)n, sizeof all[0], cmp_rec);
    for (int i = 1; i < n; i++)
        check(cmp_rec(&all[i - 1], &all[i]) < 0, "total order strict");

    static const char *const KIND[3] = {"local", "send", "recv"};
    for (int p = 0; p < NP; p++) {
        long sum = 0;
        for (int e = 0; e < prog_len[p]; e++)
            sum += stamp[p][e];
        printf("process %d: events=%d final clock=%ld timestamp sum=%ld\n", p, prog_len[p], stamp[p][prog_len[p] - 1], sum);
    }
    printf("messages=%ld events=%d\n", messages, n);
    printf("first 14 events in total order (timestamp, pid):\n");
    for (int i = 0; i < 14 && i < n; i++) {
        Ev ev = prog[all[i].pid][all[i].idx];
        printf("  (%ld,%d) %s", all[i].ts, all[i].pid, KIND[ev.type]);
        if (ev.type != LOCAL)
            printf(" %s %d", ev.type == SEND ? "to" : "from", ev.peer);
        printf("\n");
    }
    return 0;
}
