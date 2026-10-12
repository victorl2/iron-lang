/*
 * title: Interval partitioning into the fewest rooms
 * topic: algorithms
 * covers: min-heap, interval scheduling, depth equals rooms, room assignment, sweep counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int s, e, id, room;
} Lec;

typedef struct {
    int end, room;
} Slot;

static unsigned st = 1234567u;

static unsigned rnd(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 10;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int cmp_start(const void *pa, const void *pb) {
    const Lec *a = pa, *b = pb;
    if (a->s != b->s)
        return a->s - b->s;
    if (a->e != b->e)
        return a->e - b->e;
    return a->id - b->id;
}

static int cmp_id(const void *pa, const void *pb) {
    return ((const Lec *)pa)->id - ((const Lec *)pb)->id;
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* min-heap keyed by (end, room) so the earliest-freed, lowest-numbered room is reused */
static int less(Slot a, Slot b) {
    return a.end < b.end || (a.end == b.end && a.room < b.room);
}

static void sift_down(Slot *h, int n, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && less(h[l], h[m]))
            m = l;
        if (r < n && less(h[r], h[m]))
            m = r;
        if (m == i)
            return;
        Slot t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

static void sift_up(Slot *h, int i) {
    while (i > 0 && less(h[i], h[(i - 1) / 2])) {
        Slot t = h[i];
        h[i] = h[(i - 1) / 2];
        h[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

/* Half-open intervals [s, e). Returns rooms used; fills room in each lecture. */
static int partition(Lec *l, int n) {
    qsort(l, (size_t)n, sizeof(Lec), cmp_start);
    Slot *heap = malloc(sizeof(Slot) * (size_t)(n + 1));
    int hn = 0, rooms = 0;
    for (int i = 0; i < n; i++) {
        if (hn > 0 && heap[0].end <= l[i].s) {
            l[i].room = heap[0].room;
            heap[0].end = l[i].e;
            sift_down(heap, hn, 0);
        } else {
            l[i].room = rooms++;
            heap[hn] = (Slot){l[i].e, l[i].room};
            sift_up(heap, hn++);
        }
    }
    free(heap);
    return rooms;
}

/* Maximum overlap depth by sorting endpoints separately. */
static int max_depth(const Lec *l, int n) {
    int *s = malloc(sizeof(int) * (size_t)n), *e = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        s[i] = l[i].s;
        e[i] = l[i].e;
    }
    qsort(s, (size_t)n, sizeof(int), cmp_int);
    qsort(e, (size_t)n, sizeof(int), cmp_int);
    int best = 0, cur = 0, j = 0;
    for (int i = 0; i < n; i++) {
        while (e[j] <= s[i]) {
            j++;
            cur--;
        }
        cur++;
        if (cur > best)
            best = cur;
    }
    free(s);
    free(e);
    return best;
}

int main(void) {
    int sizes[] = {1, 4, 10, 25, 100, 500};
    int spans[] = {10, 20, 30, 60, 100, 400};
    for (int t = 0; t < 6; t++) {
        int n = sizes[t];
        Lec *l = malloc(sizeof(Lec) * (size_t)n);
        for (int i = 0; i < n; i++) {
            int s = (int)(rnd() % (unsigned)spans[t]), len = 1 + (int)(rnd() % 12);
            l[i] = (Lec){s, s + len, i, -1};
        }
        int depth = max_depth(l, n);
        int rooms = partition(l, n);
        check(rooms == depth, "rooms used equals maximum depth");
        /* no two lectures in one room overlap */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (l[i].room == l[j].room)
                    check(l[i].e <= l[j].s || l[j].e <= l[i].s, "room exclusive");
        int *use = calloc((size_t)rooms, sizeof(int));
        for (int i = 0; i < n; i++)
            use[l[i].room]++;
        printf("n=%3d rooms=%2d load:", n, rooms);
        for (int r = 0; r < rooms && r < 12; r++)
            printf(" %d", use[r]);
        printf(rooms > 12 ? " ...\n" : "\n");
        free(use);
        qsort(l, (size_t)n, sizeof(Lec), cmp_id);
        if (n == 10) {
            printf("  assignment by id:");
            for (int i = 0; i < n; i++)
                printf(" %d->R%d", l[i].id, l[i].room);
            printf("\n");
        }
        free(l);
    }
    Lec back_to_back[] = {{0, 5, 0, -1}, {5, 10, 1, -1}, {10, 15, 2, -1}};
    printf("back to back needs %d room\n", partition(back_to_back, 3));
    Lec same[] = {{2, 4, 0, -1}, {2, 4, 1, -1}, {2, 4, 2, -1}, {3, 5, 3, -1}};
    printf("stacked needs %d rooms\n", partition(same, 4));
    return 0;
}
