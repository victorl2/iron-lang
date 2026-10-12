/*
 * title: Search in a rotated sorted array
 * topic: algorithms
 * covers: binary search, rotation pivot, minimum finding, all rotations of distinct values
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* index of the minimum (the rotation offset) in a rotated array of distinct values */
static int find_min(const int *a, int n) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] > a[hi])
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int search(const int *a, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == key)
            return mid;
        if (a[lo] <= a[mid]) { /* left half sorted */
            if (a[lo] <= key && key < a[mid])
                hi = mid - 1;
            else
                lo = mid + 1;
        } else { /* right half sorted */
            if (a[mid] < key && key <= a[hi])
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    return -1;
}

int main(void) {
    int checked = 0;
    for (int n = 1; n <= 40; n++) {
        int base[40], a[40];
        for (int i = 0; i < n; i++)
            base[i] = i * 3 + 1;
        for (int rot = 0; rot < n; rot++) {
            for (int i = 0; i < n; i++)
                a[i] = base[(i + rot) % n];
            if (find_min(a, n) != (n - rot) % n)
                fail("find_min");
            for (int k = 0; k <= 3 * n + 2; k++) {
                int got = search(a, n, k), want = -1;
                for (int i = 0; i < n; i++)
                    if (a[i] == k)
                        want = i;
                if (got != want)
                    fail("search");
                checked++;
            }
        }
    }
    printf("queries verified: %d\n", checked);

    int demo[] = {40, 44, 51, 60, 72, 5, 9, 13, 20, 33};
    int n = 10;
    printf("demo min index: %d\n", find_min(demo, n));
    int keys[] = {72, 5, 33, 40, 6, 100};
    for (int i = 0; i < 6; i++)
        printf("search %d -> %d\n", keys[i], search(demo, n, keys[i]));
    return 0;
}
