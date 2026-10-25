/*
 * title: Skyline of buildings via sweep line and lazy-deletion heap
 * topic: algorithms
 * covers: sweep line, max-heap, lazy deletion, event sorting, key points, height map verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int l, r, h;
} Bld;

typedef struct {
    int x, h, r; /* heap entry: height and the right edge at which it expires */
} Node;

typedef struct {
    int x, y;
} Key;

typedef struct {
    int x, h, is_start, idx;
} Ev;

static unsigned s = 31337u;

static unsigned rnd(void) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Node heap[512];
static int hn;

static void hpush(Node v) {
    int i = hn++;
    heap[i] = v;
    while (i > 0 && heap[(i - 1) / 2].h < heap[i].h) {
        Node t = heap[i];
        heap[i] = heap[(i - 1) / 2];
        heap[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static void hpop(void) {
    heap[0] = heap[--hn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < hn && heap[l].h > heap[m].h)
            m = l;
        if (r < hn && heap[r].h > heap[m].h)
            m = r;
        if (m == i)
            break;
        Node t = heap[i];
        heap[i] = heap[m];
        heap[m] = t;
        i = m;
    }
}

static int cmp_ev(const void *pa, const void *pb) {
    const Ev *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->is_start != b->is_start)
        return b->is_start - a->is_start; /* starts first at the same x */
    if (a->is_start && a->h != b->h)
        return b->h - a->h;
    return a->idx - b->idx;
}

static int skyline(const Bld *b, int n, Key *out) {
    Ev ev[256];
    int ne = 0, k = 0;
    for (int i = 0; i < n; i++) {
        ev[ne++] = (Ev){b[i].l, b[i].h, 1, i};
        ev[ne++] = (Ev){b[i].r, b[i].h, 0, i};
    }
    qsort(ev, (size_t)ne, sizeof(Ev), cmp_ev);
    hn = 0;
    int prev = 0;
    for (int i = 0; i < ne;) {
        int x = ev[i].x;
        while (i < ne && ev[i].x == x) {
            if (ev[i].is_start)
                hpush((Node){x, ev[i].h, b[ev[i].idx].r});
            i++;
        }
        while (hn > 0 && heap[0].r <= x) /* lazily drop buildings that have ended */
            hpop();
        int cur = hn ? heap[0].h : 0;
        if (cur != prev) {
            out[k++] = (Key){x, cur};
            prev = cur;
        }
    }
    return k;
}

static int height_at(const Bld *b, int n, int x) {
    int best = 0;
    for (int i = 0; i < n; i++)
        if (b[i].l <= x && x < b[i].r && b[i].h > best)
            best = b[i].h;
    return best;
}

static void print_keys(const Key *k, int n) {
    for (int i = 0; i < n; i++)
        printf("%s(%d,%d)", i ? " " : "", k[i].x, k[i].y);
    printf("\n");
}

int main(void) {
    Bld classic[] = {{2, 9, 10}, {3, 7, 15}, {5, 12, 12}, {15, 20, 10}, {19, 24, 8}};
    Key out[256];
    int k = skyline(classic, 5, out);
    printf("classic:");
    printf(" ");
    print_keys(out, k);
    check(k == 7 && out[1].y == 15 && out[6].y == 0, "classic skyline");
    Bld touching[] = {{0, 5, 3}, {5, 10, 3}, {2, 8, 3}};
    k = skyline(touching, 3, out);
    printf("equal heights merge: ");
    print_keys(out, k);
    check(k == 2, "merged plateau");
    int sizes[] = {3, 8, 20, 60, 120};
    long total_keys = 0;
    for (int t = 0; t < 5; t++) {
        int n = sizes[t];
        Bld b[128];
        for (int i = 0; i < n; i++) {
            int l = (int)(rnd() % 200), w = 1 + (int)(rnd() % 30);
            b[i] = (Bld){l, l + w, 1 + (int)(rnd() % 50)};
        }
        k = skyline(b, n, out);
        for (int x = -1; x < 240; x++) {
            int want = height_at(b, n, x);
            int got = 0;
            for (int i = 0; i < k; i++)
                if (out[i].x <= x)
                    got = out[i].y;
            check(want == got, "height map matches skyline");
        }
        for (int i = 1; i < k; i++) {
            check(out[i].x > out[i - 1].x, "x strictly increasing");
            check(out[i].y != out[i - 1].y, "no redundant key points");
        }
        int peak = 0;
        long area = 0;
        for (int i = 0; i + 1 < k; i++) {
            area += (long)out[i].y * (out[i + 1].x - out[i].x);
            if (out[i].y > peak)
                peak = out[i].y;
        }
        printf("n=%3d keypoints=%3d peak=%2d area=%ld\n", n, k, peak, area);
        total_keys += k;
    }
    printf("total keypoints %ld\n", total_keys);
    return 0;
}
