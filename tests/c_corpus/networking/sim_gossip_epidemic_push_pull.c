/*
 * title: Epidemic gossip: push, pull, push-pull and rumor mongering
 * topic: networking
 * covers: gossip dissemination rounds, log N spreading, pull endgame speedup, message loss, rumor mongering residue, seeded Monte Carlo
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

enum { N = 256, MAXR = 200, TRIALS = 60 };
enum { PUSH, PULL, PUSHPULL };
static const char *mname[] = {"push", "pull", "push-pull"};

typedef struct { int rounds_full, rounds_half, rounds_90; long msgs; } Trial;

static int peer(int self) {
    unsigned r = rnd();
    int p = (int)(r % (N - 1));
    return p >= self ? p + 1 : p; /* uniform over the other nodes */
}

static Trial spread(int mode, int fanout, int loss_pct) {
    unsigned char know[N], next[N];
    memset(know, 0, sizeof know);
    know[0] = 1;
    Trial t = {-1, -1, -1, 0};
    int informed = 1;
    for (int round = 1; round <= MAXR && informed < N; round++) {
        memcpy(next, know, sizeof next);
        for (int i = 0; i < N; i++) {
            for (int f = 0; f < fanout; f++) {
                if (mode == PUSH || mode == PUSHPULL) {
                    if (know[i]) {
                        int p = peer(i);
                        unsigned l = rnd();
                        t.msgs++;
                        if ((int)(l % 100) >= loss_pct) next[p] = 1;
                    }
                }
                if (mode == PULL || mode == PUSHPULL) {
                    if (!know[i]) {
                        int p = peer(i);
                        unsigned l = rnd();
                        t.msgs += 2; /* request and reply */
                        if ((int)(l % 100) >= loss_pct && know[p]) next[i] = 1;
                    }
                }
            }
        }
        memcpy(know, next, sizeof know);
        informed = 0;
        for (int i = 0; i < N; i++) informed += know[i];
        if (t.rounds_half < 0 && informed * 2 >= N) t.rounds_half = round;
        if (t.rounds_90 < 0 && informed * 10 >= N * 9) t.rounds_90 = round;
        if (informed == N) t.rounds_full = round;
    }
    return t;
}

/* rumor mongering with a counter: an infective gives up after k contacts with nodes that already know */
static int rumor_residue(int k, unsigned seed, int n) {
    unsigned char state[2048]; /* 0 susceptible, 1 infective, 2 removed */
    int cnt[2048];
    rng_state = seed;
    memset(state, 0, (size_t)n);
    memset(cnt, 0, sizeof(int) * (size_t)n);
    state[0] = 1;
    int active = 1;
    for (int round = 0; round < 500 && active > 0; round++) {
        unsigned char snap[2048];
        memcpy(snap, state, (size_t)n);
        for (int i = 0; i < n; i++) {
            if (snap[i] != 1) continue;
            unsigned r = rnd();
            int p = (int)(r % (unsigned)(n - 1));
            if (p >= i) p++;
            if (snap[p] == 0) state[p] = 1;
            else if (++cnt[i] >= k) state[i] = 2;
        }
        active = 0;
        for (int i = 0; i < n; i++) if (state[i] == 1) active++;
    }
    int res = 0;
    for (int i = 0; i < n; i++) if (state[i] == 0) res++;
    return res;
}

int main(void) {
    printf("N=%d nodes, fanout 1, %d trials per row (rounds are averages x10)\n", N, TRIALS);
    long avg_full[3][2];
    int losses[2] = {0, 20};
    for (int li = 0; li < 2; li++)
        for (int mode = 0; mode < 3; mode++) {
            long sfull = 0, shalf = 0, s90 = 0, smsg = 0;
            int mn = 1000, mx = 0;
            for (int tr = 0; tr < TRIALS; tr++) {
                rng_state = 1000u + (unsigned)(tr * 977 + mode * 31 + li * 7);
                Trial t = spread(mode, 1, losses[li]);
                check(t.rounds_full > 0, "epidemic reaches everyone");
                sfull += t.rounds_full; shalf += t.rounds_half; s90 += t.rounds_90; smsg += t.msgs;
                if (t.rounds_full < mn) mn = t.rounds_full;
                if (t.rounds_full > mx) mx = t.rounds_full;
            }
            avg_full[mode][li] = sfull * 10 / TRIALS;
            printf("loss=%2d%% %-9s: half=%3ld 90%%=%3ld full=%3ld (min %2d max %2d) messages/node=%3ld\n", losses[li], mname[mode],
                   shalf * 10 / TRIALS, s90 * 10 / TRIALS, sfull * 10 / TRIALS, mn, mx, smsg / TRIALS / N);
        }
    for (int li = 0; li < 2; li++) {
        check(avg_full[PUSHPULL][li] <= avg_full[PUSH][li], "push-pull is never slower than push");
        check(avg_full[PUSHPULL][li] <= avg_full[PULL][li], "push-pull is never slower than pull");
    }
    /* loss slows the epidemic */
    check(avg_full[PUSH][1] > avg_full[PUSH][0], "loss slows push");

    printf("rumor mongering, n=1000, average residue (nodes that never heard) over 20 runs:\n");
    long prev = 100000;
    for (int k = 1; k <= 4; k++) {
        long sum = 0;
        for (int r = 0; r < 20; r++) sum += rumor_residue(k, 900u + (unsigned)(r * 13 + k), 1000);
        long res = sum * 10000 / (20 * 1000);
        printf("  k=%d residue=%ld.%02ld%%\n", k, res / 100, res % 100);
        check(res < prev, "a larger counter leaves fewer uninformed nodes");
        prev = res;
    }
    return 0;
}
