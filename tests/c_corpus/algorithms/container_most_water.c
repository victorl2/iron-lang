/*
 * title: Container with most water
 * topic: algorithms
 * covers: two pointers greedy, pointer-move justification, area maximisation, brute-force verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 24680u;
static unsigned rnd(void) {
    st = st * 1103515245u + 12345u;
    return (st >> 8) & 0xFFFFFF;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    long area;
    int left, right;
} Best;

static Best two_ptr(const int *h, int n) {
    Best b = {0, 0, 0};
    int l = 0, r = n - 1;
    while (l < r) {
        int lower = h[l] < h[r] ? h[l] : h[r];
        long area = (long)lower * (r - l);
        if (area > b.area) {
            b.area = area;
            b.left = l;
            b.right = r;
        }
        /* moving the taller wall inward can never help */
        if (h[l] < h[r])
            l++;
        else
            r--;
    }
    return b;
}

static long brute(const int *h, int n) {
    long best = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            int lower = h[i] < h[j] ? h[i] : h[j];
            long a = (long)lower * (j - i);
            if (a > best)
                best = a;
        }
    return best;
}

int main(void) {
    int h[200];
    long sum = 0;
    for (int trial = 0; trial < 300; trial++) {
        int n = 2 + (int)(rnd() % 100);
        int cap = 1 + (int)(rnd() % 1000);
        for (int i = 0; i < n; i++)
            h[i] = (int)(rnd() % (unsigned)(cap + 1));
        Best b = two_ptr(h, n);
        if (b.area != brute(h, n))
            fail("area");
        if (b.area > 0) {
            int lower = h[b.left] < h[b.right] ? h[b.left] : h[b.right];
            if ((long)lower * (b.right - b.left) != b.area)
                fail("witness");
        }
        sum += b.area;
    }
    printf("300 random walls, area checksum %ld\n", sum);

    int demo[] = {1, 8, 6, 2, 5, 4, 8, 3, 7};
    Best b = two_ptr(demo, 9);
    printf("classic: area %ld between %d and %d\n", b.area, b.left, b.right);

    int stairs[100];
    for (int i = 0; i < 100; i++)
        stairs[i] = i + 1;
    b = two_ptr(stairs, 100);
    printf("ascending 1..100: area %ld between %d and %d\n", b.area, b.left, b.right);
    int flat[50];
    for (int i = 0; i < 50; i++)
        flat[i] = 7;
    b = two_ptr(flat, 50);
    printf("flat: area %ld between %d and %d\n", b.area, b.left, b.right);
    return 0;
}
