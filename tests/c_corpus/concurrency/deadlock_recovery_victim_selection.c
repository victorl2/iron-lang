/*
 * title: Deadlock recovery by victim selection
 * topic: concurrency
 * covers: wait-for graph, cycle detection, victim choice by cost and age, rollback and restart, simulation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NP = 6, NR = 6, PLEN = 4, MAXROUNDS = 400 };

typedef struct {
    int prog[PLEN]; /* resources acquired in this order */
    int pc;         /* next acquisition to attempt; PLEN means holding everything, about to work */
    int work;       /* rounds of work left once everything is held */
    int holds[NR];
    int nheld;
    int waiting_for; /* resource id or -1 */
    int delay;       /* rounds to sleep after being aborted */
    int rollbacks;
    int done;
    int finished_round;
} Proc;

static Proc pr[NP];
static int holder[NR];

static unsigned rng_state = 8675309u;
static unsigned rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void make_program(Proc *p) {
    int used[NR] = {0};
    for (int i = 0; i < PLEN;) {
        int r = (int)(rng() % NR);
        if (!used[r]) {
            used[r] = 1;
            p->prog[i++] = r;
        }
    }
}

static void release_all(int i) {
    Proc *p = &pr[i];
    for (int r = 0; r < NR; r++)
        if (p->holds[r]) {
            p->holds[r] = 0;
            holder[r] = -1;
        }
    p->nheld = 0;
}

/* Finds a cycle in the wait-for graph (every blocked process has one outgoing edge). */
static int find_cycle(int *cyc) {
    for (int s = 0; s < NP; s++) {
        int seen[NP];
        memset(seen, 0, sizeof seen);
        int u = s;
        while (u >= 0 && !pr[u].done && pr[u].waiting_for >= 0 && !seen[u]) {
            seen[u] = 1;
            u = holder[pr[u].waiting_for];
        }
        if (u >= 0 && seen[u]) {
            int n = 0, x = u;
            do {
                cyc[n++] = x;
                x = holder[pr[x].waiting_for];
            } while (x != u);
            return n;
        }
    }
    return 0;
}

int main(void) {
    for (int r = 0; r < NR; r++)
        holder[r] = -1;
    for (int i = 0; i < NP; i++) {
        memset(&pr[i], 0, sizeof pr[i]);
        pr[i].waiting_for = -1;
        pr[i].work = 2;
        make_program(&pr[i]);
    }
    for (int i = 0; i < NP; i++) {
        printf("P%d wants:", i);
        for (int k = 0; k < PLEN; k++)
            printf(" R%d", pr[i].prog[k]);
        printf("\n");
    }

    int aborts = 0, deadlocks = 0, round = 0, remaining = NP;
    while (remaining > 0) {
        check(++round <= MAXROUNDS, "simulation terminates");
        for (int i = 0; i < NP; i++) {
            Proc *p = &pr[i];
            if (p->done)
                continue;
            if (p->delay > 0) {
                p->delay--;
                continue;
            }
            if (p->pc == PLEN) { /* all resources held: work, then finish */
                if (--p->work == 0) {
                    release_all(i);
                    p->done = 1;
                    p->finished_round = round;
                    remaining--;
                    printf("round %d: P%d finished\n", round, i);
                }
                continue;
            }
            int r = p->prog[p->pc];
            if (holder[r] < 0) {
                holder[r] = i;
                p->holds[r] = 1;
                p->nheld++;
                p->waiting_for = -1;
                p->pc++;
            } else {
                p->waiting_for = r;
            }
        }
        /* detect and resolve every deadlock present */
        int cyc[NP];
        int n;
        while ((n = find_cycle(cyc)) > 0) {
            deadlocks++;
            int victim = -1;
            long best = 0;
            printf("round %d: deadlock among", round);
            for (int k = 0; k < n; k++) {
                int q = cyc[k];
                printf(" P%d", q);
                /* cost: work already invested, discounted so that repeat victims get spared */
                long cost = (long)pr[q].nheld * 10 + pr[q].rollbacks * 25;
                if (victim < 0 || cost < best || (cost == best && q < victim)) {
                    victim = q;
                    best = cost;
                }
            }
            printf("; victim P%d (cost %ld)\n", victim, best);
            release_all(victim);
            pr[victim].pc = 0;
            pr[victim].waiting_for = -1;
            pr[victim].rollbacks++;
            pr[victim].delay = 1 + pr[victim].rollbacks;
            aborts++;
        }
        /* invariant: no resource is held by two processes */
        for (int r = 0; r < NR; r++) {
            int cnt = 0;
            for (int i = 0; i < NP; i++)
                cnt += pr[i].holds[r];
            check(cnt <= 1, "exclusive resources");
            check((holder[r] >= 0) == (cnt == 1), "holder table matches");
        }
    }
    printf("finished after %d rounds: deadlocks=%d aborts=%d\n", round, deadlocks, aborts);
    for (int i = 0; i < NP; i++)
        printf("P%d rollbacks=%d finished in round %d\n", i, pr[i].rollbacks, pr[i].finished_round);
    check(deadlocks == aborts, "one abort per deadlock");
    return 0;
}
