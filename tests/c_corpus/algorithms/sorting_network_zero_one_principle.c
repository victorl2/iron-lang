/*
 * title: Verifying optimal small sorting networks with the 0-1 principle
 * topic: algorithms
 * covers: sorting networks, 0-1 principle, bitmask state, comparator removal (mutation testing), static tables
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned char i, j;
} Cx;

/* Known optimal-size networks (Knuth). */
static const Cx n3[] = {{0, 2}, {0, 1}, {1, 2}};
static const Cx n4[] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}, {1, 2}};
static const Cx n5[] = {{0, 3}, {1, 4}, {0, 2}, {1, 3}, {0, 1}, {2, 4}, {1, 2}, {3, 4}, {2, 3}};
static const Cx n6[] = {{1, 2}, {4, 5}, {0, 2}, {3, 5}, {0, 1}, {3, 4}, {2, 5}, {0, 3}, {1, 4}, {2, 4}, {1, 3}, {2, 3}};
static const Cx n7[] = {{1, 2}, {3, 4}, {5, 6}, {0, 2}, {3, 5}, {4, 6}, {0, 1}, {4, 5}, {2, 6}, {0, 4}, {1, 5}, {0, 3}, {2, 5}, {1, 3}, {2, 4}, {2, 3}};
static const Cx n8[] = {{0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}, {0, 1}, {2, 3}, {4, 5}, {6, 7}, {2, 4}, {3, 5}, {1, 4}, {3, 6}, {1, 2}, {3, 4}, {5, 6}};

typedef struct {
    int n;
    const Cx *c;
    int len;
} Net;

#define NET(k, a) {k, a, (int)(sizeof(a) / sizeof(a[0]))}

/* Count 0-1 inputs (of 2^n) that the network (skipping comparator `skip`) fails to sort. */
static unsigned failures(const Net *w, int skip) {
    unsigned bad = 0;
    for (unsigned m = 0; m < (1u << w->n); m++) {
        unsigned v = m;
        for (int k = 0; k < w->len; k++) {
            if (k == skip)
                continue;
            unsigned bi = (v >> w->c[k].i) & 1u, bj = (v >> w->c[k].j) & 1u;
            if (bi > bj)
                v ^= (1u << w->c[k].i) | (1u << w->c[k].j);
        }
        /* sorted ascending by index: zeros in low positions, so v must be 1...10...0 shape from the top */
        unsigned ones = 0;
        for (unsigned t = v; t; t >>= 1)
            ones += t & 1u;
        unsigned want = ((1u << ones) - 1u) << (w->n - ones);
        if (v != want)
            bad++;
    }
    return bad;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

int main(void) {
    const Net nets[] = {NET(3, n3), NET(4, n4), NET(5, n5), NET(6, n6), NET(7, n7), NET(8, n8)};
    for (int k = 0; k < 6; k++) {
        const Net *w = &nets[k];
        unsigned f = failures(w, -1);
        check(f == 0, "network sorts all 0-1 inputs");
        int essential = 0;
        for (int s = 0; s < w->len; s++)
            if (failures(w, s) > 0)
                essential++;
        printf("n=%d comparators=%-2d inputs=%-3u failures=%u essential=%d/%d\n", w->n, w->len, 1u << w->n, f,
               essential, w->len);
        check(essential == w->len, "every comparator of an optimal network is needed");
    }
    /* A too-short network must fail: drop the last comparator of the 5-input network and print how badly. */
    Net bad = {5, n5, 8};
    printf("truncated n=5 network fails on %u of 32 inputs\n", failures(&bad, -1));
    check(failures(&bad, -1) > 0, "truncated network fails");
    return 0;
}
