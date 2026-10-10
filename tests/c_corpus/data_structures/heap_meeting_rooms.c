/*
 * title: Room assignment for intervals with a min-heap of end times
 * topic: data_structures
 * covers: interval scheduling, min-heap of end times, room reuse on half-open intervals, event sweep oracle, per-room overlap validation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x400Full;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int start, end, room;
    int id;
} Iv;

static int cmp_start(const void *x, const void *y) {
    const Iv *a = x, *b = y;
    if (a->start != b->start)
        return a->start < b->start ? -1 : 1;
    return a->id - b->id;
}

typedef struct {
    int end, room;
} HE;

static int hless(HE a, HE b) { return a.end < b.end || (a.end == b.end && a.room < b.room); }

static void hpush(HE *h, int *n, HE e) {
    int i = (*n)++;
    while (i > 0 && hless(e, h[(i - 1) / 2])) {
        h[i] = h[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h[i] = e;
}

static HE hpop(HE *h, int *n) {
    HE top = h[0], x = h[--(*n)];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= *n)
            break;
        if (c + 1 < *n && hless(h[c + 1], h[c]))
            c++;
        if (!hless(h[c], x))
            break;
        h[i] = h[c];
        i = c;
    }
    if (*n > 0)
        h[i] = x;
    return top;
}

/* assign rooms; returns the number of rooms used */
static int assign(Iv *iv, int n, long *reuse, int *max_heap) {
    HE *h = malloc(sizeof(HE) * (size_t)(n + 1));
    int hn = 0, rooms = 0;
    *reuse = 0;
    *max_heap = 0;
    for (int i = 0; i < n; i++) {
        if (hn > 0 && h[0].end <= iv[i].start) {
            HE e = hpop(h, &hn);
            iv[i].room = e.room;
            (*reuse)++;
        } else
            iv[i].room = rooms++;
        hpush(h, &hn, (HE){iv[i].end, iv[i].room});
        if (hn > *max_heap)
            *max_heap = hn;
    }
    free(h);
    return rooms;
}

static int cmp_int(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

/* oracle: sweep over sorted starts and ends, ends processed first at equal times */
static int peak_overlap(const Iv *iv, int n) {
    int *s = malloc(sizeof(int) * (size_t)n), *e = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        s[i] = iv[i].start;
        e[i] = iv[i].end;
    }
    qsort(s, (size_t)n, sizeof(int), cmp_int);
    qsort(e, (size_t)n, sizeof(int), cmp_int);
    int i = 0, j = 0, cur = 0, peak = 0;
    while (i < n) {
        if (e[j] <= s[i]) {
            cur--;
            j++;
        } else {
            cur++;
            i++;
            if (cur > peak)
                peak = cur;
        }
    }
    free(s);
    free(e);
    return peak;
}

static void validate_rooms(Iv *iv, int n, int rooms) {
    for (int r = 0; r < rooms; r++) {
        int last_end = -1;
        int used = 0;
        for (int i = 0; i < n; i++) /* iv is sorted by start */
            if (iv[i].room == r) {
                check(iv[i].start >= last_end, "no overlap inside one room");
                last_end = iv[i].end;
                used++;
            }
        check(used > 0, "every opened room is used");
    }
}

int main(void) {
    /* fixed case: [0,30) [5,10) [15,20) needs 2 rooms; touching intervals share a room */
    Iv fixed[] = {{0, 30, 0, 0}, {5, 10, 0, 1}, {15, 20, 0, 2}, {10, 15, 0, 3}, {20, 25, 0, 4}};
    long reuse;
    int mh;
    qsort(fixed, 5, sizeof(Iv), cmp_start);
    int rooms = assign(fixed, 5, &reuse, &mh);
    check(rooms == 2 && peak_overlap(fixed, 5) == 2, "fixed case needs two rooms");
    printf("fixed: rooms=%d reused=%ld\n", rooms, reuse);

    int counts[] = {50, 500, 2000};
    unsigned spans[] = {200, 1000, 5000};
    for (int c = 0; c < 3; c++) {
        for (int sp = 0; sp < 3; sp++) {
            int n = counts[c];
            Iv *iv = malloc(sizeof(Iv) * (size_t)n);
            for (int i = 0; i < n; i++) {
                iv[i].start = (int)(rng() % 10000);
                iv[i].end = iv[i].start + 1 + (int)(rng() % spans[sp]);
                iv[i].id = i;
                iv[i].room = -1;
            }
            qsort(iv, (size_t)n, sizeof(Iv), cmp_start);
            int r = assign(iv, n, &reuse, &mh);
            int peak = peak_overlap(iv, n);
            check(r == peak, "rooms used equals peak overlap");
            validate_rooms(iv, n, r);
            printf("n=%-5d max_len=%-5u rooms=%-4d reused=%-5ld max_heap=%d\n", n, spans[sp], r, reuse, mh);
            free(iv);
        }
    }
    return 0;
}
