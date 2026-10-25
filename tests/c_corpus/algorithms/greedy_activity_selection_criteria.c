/*
 * title: Activity selection and competing greedy criteria
 * topic: algorithms
 * covers: interval scheduling, earliest finish greedy, exchange argument, counter-examples, subset brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int s, f, id;
} Act;

static unsigned st = 8675309u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* All comparators are total orders (id breaks ties) so qsort output is deterministic. */
static int by_finish(const void *pa, const void *pb) {
    const Act *a = pa, *b = pb;
    if (a->f != b->f)
        return a->f - b->f;
    if (a->s != b->s)
        return a->s - b->s;
    return a->id - b->id;
}

static int by_start(const void *pa, const void *pb) {
    const Act *a = pa, *b = pb;
    if (a->s != b->s)
        return a->s - b->s;
    if (a->f != b->f)
        return a->f - b->f;
    return a->id - b->id;
}

static int by_length(const void *pa, const void *pb) {
    const Act *a = pa, *b = pb;
    int la = a->f - a->s, lb = b->f - b->s;
    if (la != lb)
        return la - lb;
    return a->id - b->id;
}

/* Generic greedy: scan in the given order, take an activity if it conflicts with nothing chosen. */
static int take_compatible(const Act *sorted, int n, int *chosen) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        int ok = 1;
        for (int j = 0; j < k && ok; j++) {
            const Act *c = &sorted[chosen[j]];
            if (sorted[i].s < c->f && c->s < sorted[i].f)
                ok = 0;
        }
        if (ok)
            chosen[k++] = i;
    }
    return k;
}

/* Earliest finish time in a single linear pass over the finish-sorted array. */
static int earliest_finish(const Act *sorted, int n, int *picked_ids) {
    int k = 0, last_end = -1;
    for (int i = 0; i < n; i++)
        if (sorted[i].s >= last_end) {
            picked_ids[k++] = sorted[i].id;
            last_end = sorted[i].f;
        }
    return k;
}

static int brute_max(const Act *a, int n) {
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        int cnt = 0, ok = 1;
        for (int i = 0; i < n && ok; i++) {
            if (!(mask >> i & 1))
                continue;
            cnt++;
            for (int j = i + 1; j < n; j++)
                if ((mask >> j & 1) && a[i].s < a[j].f && a[j].s < a[i].f) {
                    ok = 0;
                    break;
                }
        }
        if (ok && cnt > best)
            best = cnt;
    }
    return best;
}

int main(void) {
    /* classic textbook instance (11 activities), answer 4 */
    int cs[] = {1, 3, 0, 5, 3, 5, 6, 8, 8, 2, 12};
    int cf[] = {4, 5, 6, 7, 9, 9, 10, 11, 12, 14, 16};
    Act cl[11];
    for (int i = 0; i < 11; i++)
        cl[i] = (Act){cs[i], cf[i], i};
    qsort(cl, 11, sizeof(Act), by_finish);
    int ids[16];
    int k = earliest_finish(cl, 11, ids);
    printf("classic: %d activities:", k);
    for (int i = 0; i < k; i++)
        printf(" a%d", ids[i]);
    printf("\n");
    check(k == 4, "classic answer");

    int wins_start = 0, wins_len = 0, ties = 0;
    int worse_start = 0, worse_len = 0, total_opt = 0;
    for (int t = 0; t < 400; t++) {
        int n = 3 + (int)(rnd() % 11);
        Act a[16];
        for (int i = 0; i < n; i++) {
            int s = (int)(rnd() % 30), len = 1 + (int)(rnd() % 12);
            a[i] = (Act){s, s + len, i};
        }
        Act orig[16];
        for (int i = 0; i < n; i++)
            orig[i] = a[i];
        int opt = brute_max(orig, n);
        qsort(a, (size_t)n, sizeof(Act), by_finish);
        int pid[16];
        int ef = earliest_finish(a, n, pid);
        check(ef == opt, "earliest finish is optimal");
        int chosen[16];
        Act b[16];
        for (int i = 0; i < n; i++)
            b[i] = orig[i];
        qsort(b, (size_t)n, sizeof(Act), by_finish);
        check(take_compatible(b, n, chosen) == ef, "generic scan with finish order equals linear pass");
        qsort(b, (size_t)n, sizeof(Act), by_start);
        int es = take_compatible(b, n, chosen);
        qsort(b, (size_t)n, sizeof(Act), by_length);
        int sh = take_compatible(b, n, chosen);
        check(es <= opt && sh <= opt, "no heuristic beats the optimum");
        worse_start += es < opt;
        worse_len += sh < opt;
        wins_start += es == opt;
        wins_len += sh == opt;
        ties += es == sh;
        total_opt += opt;
    }
    printf("400 random instances, total optimum=%d\n", total_opt);
    printf("earliest-start optimal in %d, suboptimal in %d\n", wins_start, worse_start);
    printf("shortest-first optimal in %d, suboptimal in %d\n", wins_len, worse_len);
    printf("heuristics agreed with each other in %d\n", ties);
    /* explicit counter-examples */
    Act ces[] = {{0, 10, 0}, {1, 3, 1}, {4, 6, 2}, {7, 9, 3}};
    qsort(ces, 4, sizeof(Act), by_start);
    int chosen[8];
    printf("earliest-start counter-example picks %d (optimum 3)\n", take_compatible(ces, 4, chosen));
    Act cel[] = {{0, 5, 0}, {4, 7, 1}, {6, 11, 2}};
    qsort(cel, 3, sizeof(Act), by_length);
    printf("shortest-first counter-example picks %d (optimum 2)\n", take_compatible(cel, 3, chosen));
    check(worse_start > 0 && worse_len > 0, "heuristics fail sometimes");
    return 0;
}
