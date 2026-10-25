/*
 * title: Dependency task graph executed by a pool
 * topic: concurrency
 * covers: DAG scheduling, indegree counters, ready queue, release of successors, happens-before verification
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 48, MAXD = 4, NW = 4 };

static int ndeps[N], deps[N][MAXD];
static int nsucc[N], succ[N][N];
static int remaining[N];
static long val[N];
static long start_seq[N], finish_seq[N];
static int ready[N], rhead, rtail, finished;
static long clock_seq;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long compute(int i, const long *v) {
    long s = i * 7 + 1;
    for (int d = 0; d < ndeps[i]; d++)
        s += v[deps[i][d]] * 3;
    return s % 1000003;
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (rhead == rtail && finished < N)
            pthread_cond_wait(&cv, &mu);
        if (rhead == rtail)
            break;
        int i = ready[rhead++];
        start_seq[i] = ++clock_seq;
        pthread_mutex_unlock(&mu);
        long r = compute(i, val); /* reads only values of finished dependencies */
        pthread_mutex_lock(&mu);
        val[i] = r;
        finish_seq[i] = ++clock_seq;
        finished++;
        for (int k = 0; k < nsucc[i]; k++) {
            int s = succ[i][k];
            if (--remaining[s] == 0)
                ready[rtail++] = s;
        }
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    unsigned s = 777u;
    for (int i = 1; i < N; i++) {
        for (int j = 0; j < i && ndeps[i] < MAXD; j++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            /* prefer near predecessors so the graph has depth */
            int near = (i - j) <= 6;
            if (s % (near ? 4u : 24u) == 0) {
                deps[i][ndeps[i]++] = j;
                succ[j][nsucc[j]++] = i;
            }
        }
    }
    int level[N], maxlevel = 0, sources = 0, sinks = 0, edges = 0;
    for (int i = 0; i < N; i++) {
        level[i] = 0;
        for (int d = 0; d < ndeps[i]; d++)
            if (level[deps[i][d]] + 1 > level[i])
                level[i] = level[deps[i][d]] + 1;
        if (level[i] > maxlevel)
            maxlevel = level[i];
        remaining[i] = ndeps[i];
        edges += ndeps[i];
        if (ndeps[i] == 0) {
            ready[rtail++] = i;
            sources++;
        }
        if (nsucc[i] == 0)
            sinks++;
    }

    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    long ref[N];
    for (int i = 0; i < N; i++)
        ref[i] = compute(i, ref); /* index order is a topological order */
    long chk = 0;
    for (int i = 0; i < N; i++) {
        check(val[i] == ref[i], "value matches sequential evaluation");
        check(start_seq[i] > 0, "node ran");
        for (int d = 0; d < ndeps[i]; d++)
            check(finish_seq[deps[i][d]] < start_seq[i], "dependency finished before start");
        chk = (chk * 131 + val[i]) % 1000000007L;
    }
    check(finished == N, "all nodes finished");
    int per_level[N] = {0};
    for (int i = 0; i < N; i++)
        per_level[level[i]]++;
    printf("nodes %d edges %d sources %d sinks %d\n", N, edges, sources, sinks);
    printf("critical path length %d levels\n", maxlevel + 1);
    for (int l = 0; l <= maxlevel; l++)
        printf("level %d: %d nodes\n", l, per_level[l]);
    printf("values hash %ld, last node value %ld\n", chk, val[N - 1]);
    return 0;
}
