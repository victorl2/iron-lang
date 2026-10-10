/*
 * title: Multi-source BFS nearest facility map
 * topic: algorithms
 * covers: multi-source BFS, Voronoi labeling, tie-breaking by queue order, brute-force verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { H = 14, W = 30, K = 5 };

static unsigned st = 362436069u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

int main(void) {
    int sr[K], sc[K];
    for (int i = 0; i < K; i++) {
        for (;;) {
            sr[i] = (int)(rnd() % H);
            sc[i] = (int)(rnd() % W);
            int dup = 0;
            for (int j = 0; j < i; j++) dup |= sr[j] == sr[i] && sc[j] == sc[i];
            if (!dup) break;
        }
    }
    static int dist[H][W], owner[H][W];
    int q[H * W], head = 0, tail = 0;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) dist[r][c] = -1, owner[r][c] = -1;
    for (int i = 0; i < K; i++) {
        dist[sr[i]][sc[i]] = 0;
        owner[sr[i]][sc[i]] = i;
        q[tail++] = sr[i] * W + sc[i];
    }
    const int dr[4] = {-1, 0, 1, 0}, dc[4] = {0, 1, 0, -1};
    while (head < tail) {
        int cur = q[head++], r = cur / W, c = cur % W;
        for (int d = 0; d < 4; d++) {
            int nr = r + dr[d], nc = c + dc[d];
            if (nr < 0 || nr >= H || nc < 0 || nc >= W || dist[nr][nc] >= 0) continue;
            dist[nr][nc] = dist[r][c] + 1;
            owner[nr][nc] = owner[r][c];
            q[tail++] = nr * W + nc;
        }
    }
    int area[K] = {0}, maxd[K] = {0};
    long total = 0;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            int best = 1 << 20;
            for (int i = 0; i < K; i++) {
                int d = abs(r - sr[i]) + abs(c - sc[i]);
                if (d < best) best = d;
            }
            check(best == dist[r][c], "distance equals manhattan minimum");
            int o = owner[r][c];
            check(abs(r - sr[o]) + abs(c - sc[o]) == dist[r][c], "owner is a nearest source");
            area[o]++;
            if (dist[r][c] > maxd[o]) maxd[o] = dist[r][c];
            total += dist[r][c];
        }
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            if (dist[r][c] == 0)
                putchar('A' + owner[r][c]);
            else
                putchar('a' + owner[r][c]);
        }
        putchar('\n');
    }
    int sum = 0;
    for (int i = 0; i < K; i++) {
        printf("site %c at (%d,%d): area %d, radius %d\n", 'A' + i, sr[i], sc[i], area[i], maxd[i]);
        sum += area[i];
    }
    check(sum == H * W, "areas partition grid");
    printf("total distance: %ld\n", total);
    return 0;
}
