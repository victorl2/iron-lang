/*
 * title: Replacement selection run generation
 * topic: algorithms
 * covers: replacement selection, external sorting, heap with run tags, average run length 2x memory, presorted inputs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 44444u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int run;
    int key;
} Item;

static int less(const Item *a, const Item *b) {
    if (a->run != b->run)
        return a->run < b->run;
    return a->key < b->key;
}

static void sift_down(Item *h, int n, int i) {
    for (;;) {
        int l = 2 * i + 1, m = i;
        if (l < n && less(&h[l], &h[m]))
            m = l;
        if (l + 1 < n && less(&h[l + 1], &h[m]))
            m = l + 1;
        if (m == i)
            return;
        Item t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

/* Generates runs into out[] (concatenated), storing run lengths. Returns number of runs. */
static int replacement_selection(const int *in, int n, int mem, int *out, int *run_len) {
    Item *h = malloc(sizeof(Item) * (size_t)mem);
    check(h != NULL, "alloc");
    int hn = 0, pos = 0;
    while (hn < mem && pos < n) {
        h[hn].run = 0;
        h[hn].key = in[pos++];
        hn++;
    }
    for (int i = hn / 2 - 1; i >= 0; i--)
        sift_down(h, hn, i);
    int nruns = 0, o = 0;
    while (hn > 0) {
        Item top = h[0];
        while (nruns <= top.run) {
            run_len[nruns++] = 0;
        }
        out[o++] = top.key;
        run_len[top.run]++;
        if (pos < n) {
            int k = in[pos++];
            h[0].key = k;
            /* a key smaller than the one just written must wait for the next run */
            h[0].run = k < top.key ? top.run + 1 : top.run;
        } else {
            h[0] = h[--hn];
        }
        sift_down(h, hn, 0);
    }
    free(h);
    return nruns;
}

int main(void) {
    enum { N = 20000 };
    static int in[N], out[N], rl[N];
    static const char *names[] = {"random", "sorted", "reversed", "nearly sorted"};
    static const int mems[] = {10, 100, 1000};
    for (int s = 0; s < 4; s++) {
        for (int i = 0; i < N; i++) {
            switch (s) {
            case 0: in[i] = (int)(rng() % 1000000); break;
            case 1: in[i] = i; break;
            case 2: in[i] = N - i; break;
            default: in[i] = i + (int)(rng() % 200); break;
            }
        }
        for (int m = 0; m < 3; m++) {
            int mem = mems[m];
            int nr = replacement_selection(in, N, mem, out, rl);
            /* verify each run is ascending and lengths add to N; multiset preserved via sums */
            int o = 0;
            long sum_in = 0, sum_out = 0;
            for (int i = 0; i < N; i++)
                sum_in += in[i];
            for (int r = 0; r < nr; r++) {
                for (int k = 1; k < rl[r]; k++)
                    check(out[o + k - 1] <= out[o + k], "run ascending");
                o += rl[r];
            }
            check(o == N, "lengths add up");
            for (int i = 0; i < N; i++)
                sum_out += out[i];
            check(sum_in == sum_out, "sum preserved");
            if (s == 1)
                check(nr == 1, "sorted input gives one run");
            printf("%-14s mem=%-5d runs=%-5d avg_len=%-8.1f first_run=%d\n", names[s], mem, nr, (double)N / nr,
                   rl[0]);
        }
    }
    return 0;
}
