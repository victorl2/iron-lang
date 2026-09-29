/*
 * title: Dynamic 3D k-d tree with k nearest neighbours
 * topic: data_structures
 * covers: k-d tree insertion, lazy deletion with tombstones, rebuild on imbalance, k-NN with bounded max-heap, total-order tie-break
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define DIM 3
#define MAXP 3000

typedef struct {
    int c[DIM];
    int id;
    int dead;
    int left, right;
} KNode;

static KNode nodes[MAXP];
static int nn_, root_ = -1, live_, rebuilds;

static int cmp_axis_id;
static int cmp_pts(const void *a, const void *b) {
    const int *x = a, *y = b; /* indices into nodes */
    int u = nodes[*x].c[cmp_axis_id], v = nodes[*y].c[cmp_axis_id];
    if (u != v)
        return u < v ? -1 : 1;
    return nodes[*x].id - nodes[*y].id;
}

static int insert_node(int t, int me, int axis) {
    if (t < 0)
        return me;
    if (nodes[me].c[axis] < nodes[t].c[axis])
        nodes[t].left = insert_node(nodes[t].left, me, (axis + 1) % DIM);
    else
        nodes[t].right = insert_node(nodes[t].right, me, (axis + 1) % DIM);
    return t;
}

static int tree_depth(int t) {
    if (t < 0)
        return 0;
    int a = tree_depth(nodes[t].left), b = tree_depth(nodes[t].right);
    return 1 + (a > b ? a : b);
}

static int build_from(int *idx, int lo, int hi, int axis) {
    if (lo >= hi)
        return -1;
    cmp_axis_id = axis;
    qsort(idx + lo, (size_t)(hi - lo), sizeof(int), cmp_pts);
    int mid = (lo + hi) / 2;
    int me = idx[mid];
    nodes[me].left = build_from(idx, lo, mid, (axis + 1) % DIM);
    nodes[me].right = build_from(idx, mid + 1, hi, (axis + 1) % DIM);
    return me;
}

/* drop tombstones and rebuild a balanced tree from the live nodes */
static void rebuild(void) {
    int idx[MAXP], n = 0, w = 0;
    KNode copy[MAXP];
    for (int i = 0; i < nn_; i++)
        if (!nodes[i].dead)
            idx[n++] = i;
    for (int i = 0; i < n; i++)
        copy[i] = nodes[idx[i]];
    for (int i = 0; i < n; i++) {
        nodes[i] = copy[i];
        idx[w++] = i;
    }
    nn_ = n;
    root_ = build_from(idx, 0, n, 0);
    rebuilds++;
}

static int add_point(const int *c, int id) {
    if (nn_ == MAXP)
        rebuild();
    int me = nn_++;
    memcpy(nodes[me].c, c, sizeof(int) * DIM);
    nodes[me].id = id;
    nodes[me].dead = 0;
    nodes[me].left = nodes[me].right = -1;
    root_ = insert_node(root_, me, 0);
    live_++;
    return me;
}

static int remove_id(int id) {
    for (int i = 0; i < nn_; i++)
        if (!nodes[i].dead && nodes[i].id == id) {
            nodes[i].dead = 1;
            live_--;
            return 1;
        }
    return 0;
}

typedef struct {
    long d;
    int id;
} Cand;

static Cand heap[16];
static int hn, K;

static int worse(Cand a, Cand b) { return a.d > b.d || (a.d == b.d && a.id > b.id); }

static void heap_offer(Cand c) {
    if (hn < K) {
        int i = hn++;
        while (i > 0 && worse(c, heap[(i - 1) / 2])) {
            heap[i] = heap[(i - 1) / 2];
            i = (i - 1) / 2;
        }
        heap[i] = c;
    } else if (worse(heap[0], c)) {
        int i = 0;
        for (;;) {
            int ch = 2 * i + 1;
            if (ch >= hn)
                break;
            if (ch + 1 < hn && worse(heap[ch + 1], heap[ch]))
                ch++;
            if (!worse(heap[ch], c))
                break;
            heap[i] = heap[ch];
            i = ch;
        }
        heap[i] = c;
    }
}

static long visits;

static void knn(int t, int axis, const int *q) {
    if (t < 0)
        return;
    visits++;
    if (!nodes[t].dead) {
        long d = 0;
        for (int k = 0; k < DIM; k++) {
            long dd = nodes[t].c[k] - q[k];
            d += dd * dd;
        }
        Cand c = {d, nodes[t].id};
        heap_offer(c);
    }
    long diff = q[axis] - nodes[t].c[axis];
    int near = diff < 0 ? nodes[t].left : nodes[t].right;
    int far = diff < 0 ? nodes[t].right : nodes[t].left;
    knn(near, (axis + 1) % DIM, q);
    if (hn < K || diff * diff <= heap[0].d)
        knn(far, (axis + 1) % DIM, q);
}

static int cmp_cand(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    if (x->d != y->d)
        return x->d < y->d ? -1 : 1;
    return x->id - y->id;
}

int main(void) {
    static int coords[6000][DIM];
    static int alive[6000];
    int next_id = 0;
    long dsum = 0, isum = 0;
    int nq = 0, depth_before = 0;
    for (int step = 0; step < 5000; step++) {
        unsigned op = rnd(10);
        if (op < 5 && next_id < 6000) {
            for (int k = 0; k < DIM; k++)
                coords[next_id][k] = (int)rnd(200);
            alive[next_id] = 1;
            add_point(coords[next_id], next_id);
            next_id++;
        } else if (op < 7 && next_id > 0) {
            int id = (int)rnd((unsigned)next_id);
            int had = alive[id];
            int got = remove_id(id);
            check(got == had, "remove result");
            alive[id] = 0;
            if (live_ * 4 < nn_ * 3 && nn_ > 50) {
                depth_before = tree_depth(root_);
                rebuild();
            }
        } else {
            int q[DIM];
            for (int k = 0; k < DIM; k++)
                q[k] = (int)rnd(200);
            K = 1 + (int)rnd(8);
            hn = 0;
            knn(root_, 0, q);
            Cand got[16];
            int ng = hn;
            memcpy(got, heap, sizeof(Cand) * (size_t)hn);
            qsort(got, (size_t)ng, sizeof(Cand), cmp_cand);
            Cand all[6000];
            int na = 0;
            for (int i = 0; i < next_id; i++)
                if (alive[i]) {
                    long d = 0;
                    for (int k = 0; k < DIM; k++) {
                        long dd = coords[i][k] - q[k];
                        d += dd * dd;
                    }
                    Cand c = {d, i};
                    all[na++] = c;
                }
            qsort(all, (size_t)na, sizeof(Cand), cmp_cand);
            int want = na < K ? na : K;
            check(ng == want, "knn size");
            for (int i = 0; i < ng; i++)
                check(got[i].d == all[i].d && got[i].id == all[i].id, "knn result");
            for (int i = 0; i < ng; i++) {
                dsum += got[i].d;
                isum += got[i].id;
            }
            nq++;
        }
    }
    printf("points inserted=%d live=%d nodes=%d rebuilds=%d\n", next_id, live_, nn_, rebuilds);
    printf("knn queries=%d distance digest=%ld id digest=%ld visits=%ld\n", nq, dsum, isum, visits);
    printf("last pre-rebuild depth=%d final depth=%d\n", depth_before, tree_depth(root_));
    return 0;
}
