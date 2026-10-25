/*
 * title: AIMD fairness and efficiency between competing flows
 * topic: networking
 * covers: Chiu-Jain phase plane, AIMD vs AIAD vs MIMD, Jain fairness index, RTT bias, asynchronous congestion feedback
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
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

enum { CAP = 1000, MAXF = 4 };
enum { P_AIMD, P_AIAD, P_MIMD, P_MIAD };
static const char *pname[] = {"AIMD", "AIAD", "MIMD", "MIAD"};

static long jain_x10000(const long *x, int n) {
    long sum = 0, sq = 0;
    for (int i = 0; i < n; i++) { sum += x[i]; sq += x[i] * x[i]; }
    if (sq == 0) return 0;
    return sum * sum * 10000 / ((long)n * sq);
}
static long total(const long *x, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    return s;
}
static void increase(int p, long *x) {
    if (p == P_AIMD || p == P_AIAD) *x += 10;
    else *x += *x / 8 + 1;
}
static void decrease(int p, long *x) {
    if (p == P_AIMD || p == P_MIMD) *x = *x / 2;
    else { *x -= 40; if (*x < 1) *x = 1; }
}

static void synchronous(int p) {
    long x[2] = {800, 40};
    printf("%s from (800,40):", pname[p]);
    long jf = jain_x10000(x, 2);
    long cong = 0;
    for (int round = 1; round <= 600; round++) {
        if (total(x, 2) >= CAP) {
            for (int i = 0; i < 2; i++) decrease(p, &x[i]);
            cong++;
        } else {
            for (int i = 0; i < 2; i++) increase(p, &x[i]);
        }
        if (round == 10 || round == 50 || round == 150 || round == 600) {
            jf = jain_x10000(x, 2);
            printf("  r%d=%ld", round, jf);
        }
    }
    printf("  final=(%ld,%ld) congestion_events=%ld\n", x[0], x[1], cong);
    if (p == P_AIMD) check(jf >= 9900, "AIMD converges to fairness");
    if (p == P_MIMD) check(jf < 9900, "MIMD keeps the initial unfairness");
    if (p == P_AIAD) check(jf < 9900, "AIAD does not converge");
}

static void rtt_bias(void) {
    int rtt[3] = {1, 2, 4};
    long x[3] = {100, 100, 100};
    long acc[3] = {0, 0, 0};
    int rounds = 4000;
    for (int r = 1; r <= rounds; r++) {
        if (total(x, 3) >= CAP) {
            for (int i = 0; i < 3; i++) x[i] /= 2;
        } else {
            for (int i = 0; i < 3; i++) if (r % rtt[i] == 0) x[i] += 10;
        }
        if (r > 1000) for (int i = 0; i < 3; i++) acc[i] += x[i];
    }
    long tot = total(acc, 3);
    printf("RTT bias (rtt 1:2:4) shares permille:");
    for (int i = 0; i < 3; i++) printf(" %ld", acc[i] * 1000 / tot);
    printf("\n");
    check(acc[0] > acc[1] && acc[1] > acc[2], "shorter RTT gets more bandwidth");
}

static void asynchronous(void) {
    long x[3] = {700, 50, 10};
    rng_state = 4242u;
    long j0 = jain_x10000(x, 3);
    long accj = 0;
    int samples = 0;
    for (int r = 1; r <= 3000; r++) {
        long tot = total(x, 3);
        if (tot >= CAP) {
            unsigned roll = rnd();
            long pick = (long)(roll % (unsigned long)tot);
            int v = 0;
            while (pick >= x[v]) { pick -= x[v]; v++; }
            x[v] /= 2;
        } else {
            for (int i = 0; i < 3; i++) x[i] += 10;
        }
        if (r > 1000) { accj += jain_x10000(x, 3); samples++; }
    }
    printf("asynchronous feedback, 3 flows: initial fairness=%ld mean fairness after warmup=%ld\n", j0, accj / samples);
    check(accj / samples > j0, "random back-off of big flows improves fairness");
    check(accj / samples > 8000, "fairness ends high");
}

int main(void) {
    for (int p = 0; p < 4; p++) synchronous(p);
    rtt_bias();
    asynchronous();
    return 0;
}
