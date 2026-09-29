/*
 * title: Parallel make-like DAG executor over processes
 * topic: concurrency
 * covers: dependency graph, topological order, cycle detection, bounded process parallelism, failed target skips dependents
 * deps: libc, posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { MAXT = 16, MAXD = 4, JOBS = 3 };

typedef struct {
    const char *name;
    int deps[MAXD]; /* -1 terminated */
    int fails;      /* the "compiler" exits with status 1 */
} Target;

static const Target G[] = {
    {"lex", {-1}, 0},           {"parse", {0, -1}, 0},       {"types", {1, -1}, 0},      {"codegen", {2, -1}, 0},
    {"runtime", {-1}, 0},       {"stdlib", {4, -1}, 0},      {"link", {3, 5, -1}, 0},    {"docs", {1, -1}, 1},
    {"tests", {6, -1}, 0},      {"package", {6, 7, -1}, 0},  {"lint", {1, -1}, 0},       {"all", {8, 9, 10, -1}, 0},
};
static const int NT = (int)(sizeof G / sizeof G[0]);

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Kahn's algorithm: returns the number of nodes ordered (== n when the graph is acyclic) */
static int topo(int n, const int deps[][MAXD], int *order) {
    int indeg[MAXT] = {0}, done = 0;
    for (int t = 0; t < n; t++)
        for (int k = 0; deps[t][k] >= 0; k++)
            indeg[t]++;
    int queue[MAXT], qh = 0, qt = 0;
    for (int t = 0; t < n; t++)
        if (!indeg[t])
            queue[qt++] = t;
    while (qh < qt) {
        int u = queue[qh++];
        order[done++] = u;
        for (int t = 0; t < n; t++)
            for (int k = 0; deps[t][k] >= 0; k++)
                if (deps[t][k] == u && --indeg[t] == 0)
                    queue[qt++] = t;
    }
    return done;
}

static unsigned long build(const char *name, const unsigned long *dep_vals, int nd) {
    unsigned long h = 1469598103934665603ul;
    for (const char *c = name; *c; c++)
        h = (h ^ (unsigned char)*c) * 1099511628211ul;
    for (int i = 0; i < nd; i++)
        h = (h ^ dep_vals[i]) * 1099511628211ul;
    return h % 1000000007ul;
}

int main(void) {
    int deps[MAXT][MAXD];
    for (int t = 0; t < NT; t++)
        memcpy(deps[t], G[t].deps, sizeof deps[t]);
    int order[MAXT];
    check(topo(NT, (const int (*)[MAXD])deps, order) == NT, "graph is acyclic");

    /* a cyclic graph must be rejected before anything runs */
    int bad[4][MAXD] = {{2, -1}, {0, -1}, {1, -1}, {-1}};
    int scratch[MAXT];
    int ordered = topo(4, (const int (*)[MAXD])bad, scratch);
    printf("cyclic graph (3-cycle plus one free node): ordered %d of 4 nodes, refusing to run\n", ordered);
    check(ordered == 1, "cycle detected");

    int level[MAXT];
    for (int i = 0; i < NT; i++) {
        int t = order[i];
        level[t] = 0;
        for (int k = 0; deps[t][k] >= 0; k++)
            if (level[deps[t][k]] + 1 > level[t])
                level[t] = level[deps[t][k]] + 1;
    }

    enum { PENDING, RUNNING, OK, FAILED, SKIPPED };
    int state[MAXT] = {PENDING};
    unsigned long value[MAXT] = {0};
    pid_t pid_of[MAXT];
    int fd_of[MAXT];
    int running = 0, max_running = 0, finished = 0, spawned = 0;
    while (finished < NT) {
        /* skip targets whose dependency failed or was skipped */
        for (int t = 0; t < NT; t++) {
            if (state[t] != PENDING)
                continue;
            for (int k = 0; deps[t][k] >= 0; k++)
                if (state[deps[t][k]] == FAILED || state[deps[t][k]] == SKIPPED) {
                    state[t] = SKIPPED;
                    finished++;
                    break;
                }
        }
        for (int t = 0; t < NT && running < JOBS; t++) {
            if (state[t] != PENDING)
                continue;
            int ready = 1;
            unsigned long dv[MAXD];
            int nd = 0;
            for (int k = 0; deps[t][k] >= 0; k++) {
                if (state[deps[t][k]] != OK)
                    ready = 0;
                else
                    dv[nd++] = value[deps[t][k]];
            }
            if (!ready)
                continue;
            int fds[2];
            check(pipe(fds) == 0, "pipe");
            fflush(stdout);
            pid_t pid = fork();
            check(pid >= 0, "fork");
            if (pid == 0) {
                close(fds[0]);
                if (G[t].fails)
                    _exit(1);
                unsigned long v = build(G[t].name, dv, nd);
                ssize_t w = write(fds[1], &v, sizeof v);
                _exit(w == (ssize_t)sizeof v ? 0 : 2);
            }
            close(fds[1]);
            pid_of[t] = pid;
            fd_of[t] = fds[0];
            state[t] = RUNNING;
            running++;
            spawned++;
            if (running > max_running)
                max_running = running;
        }
        if (running == 0)
            continue; /* only skips happened this round */
        int st = 0;
        pid_t w = wait(&st);
        check(w > 0, "wait");
        for (int t = 0; t < NT; t++)
            if (state[t] == RUNNING && pid_of[t] == w) {
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                    unsigned long v = 0;
                    check(read(fd_of[t], &v, sizeof v) == (ssize_t)sizeof v, "read result");
                    value[t] = v;
                    state[t] = OK;
                } else {
                    state[t] = FAILED;
                }
                close(fd_of[t]);
                running--;
                finished++;
            }
    }
    check(max_running <= JOBS, "parallelism bound");

    /* reference: rebuild everything serially in topological order */
    unsigned long ref[MAXT];
    int ref_ok[MAXT];
    for (int i = 0; i < NT; i++) {
        int t = order[i];
        unsigned long dv[MAXD];
        int nd = 0, all = !G[t].fails;
        for (int k = 0; deps[t][k] >= 0; k++) {
            if (!ref_ok[deps[t][k]])
                all = 0;
            else
                dv[nd++] = ref[deps[t][k]];
        }
        ref_ok[t] = all;
        ref[t] = all ? build(G[t].name, dv, nd) : 0;
    }
    static const char *const ST[5] = {"pending", "running", "built", "FAILED", "skipped"};
    int built = 0;
    for (int t = 0; t < NT; t++) {
        check((state[t] == OK) == ref_ok[t], "state agrees with reference");
        if (state[t] == OK)
            check(value[t] == ref[t], "value agrees with reference");
        built += state[t] == OK;
        printf("%-8s level %d  %-7s", G[t].name, level[t], ST[state[t]]);
        if (state[t] == OK)
            printf(" value=%lu", value[t]);
        printf("\n");
    }
    printf("built=%d of %d, at most %d jobs at once\n", built, NT, JOBS);
    check(spawned == built + 1, "one failing job spawned");
    return 0;
}
