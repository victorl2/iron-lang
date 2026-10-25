/*
 * title: BFS shortest path on a walled grid
 * topic: algorithms
 * covers: breadth-first search, grid graph, parent pointers, path reconstruction, fixpoint cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { H = 12, W = 24, INF = 1 << 28 };

static unsigned st = 2463534242u;
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

static char grid[H][W + 1];
static int dist[H][W];
static int par[H * W];

static const int DR[4] = {-1, 0, 1, 0};
static const int DC[4] = {0, 1, 0, -1};

static void bfs(int sr, int sc) {
    int queue[H * W], head = 0, tail = 0;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            dist[r][c] = INF;
            par[r * W + c] = -1;
        }
    dist[sr][sc] = 0;
    queue[tail++] = sr * W + sc;
    while (head < tail) {
        int cur = queue[head++];
        int r = cur / W, c = cur % W;
        for (int d = 0; d < 4; d++) {
            int nr = r + DR[d], nc = c + DC[d];
            if (nr < 0 || nr >= H || nc < 0 || nc >= W) continue;
            if (grid[nr][nc] == '#' || dist[nr][nc] != INF) continue;
            dist[nr][nc] = dist[r][c] + 1;
            par[nr * W + nc] = cur;
            queue[tail++] = nr * W + nc;
        }
    }
}

int main(void) {
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++)
            grid[r][c] = (rnd() % 100 < 28) ? '#' : '.';
        grid[r][W] = 0;
    }
    grid[0][0] = '.';
    grid[H - 1][W - 1] = '.';
    bfs(0, 0);

    /* Cross-check: distances are the fixpoint of relaxation. */
    int changed = 1, sweeps = 0;
    static int alt[H][W];
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) alt[r][c] = INF;
    alt[0][0] = 0;
    while (changed) {
        changed = 0;
        sweeps++;
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++) {
                if (grid[r][c] == '#') continue;
                for (int d = 0; d < 4; d++) {
                    int nr = r + DR[d], nc = c + DC[d];
                    if (nr < 0 || nr >= H || nc < 0 || nc >= W) continue;
                    if (alt[nr][nc] + 1 < alt[r][c]) {
                        alt[r][c] = alt[nr][nc] + 1;
                        changed = 1;
                    }
                }
            }
    }
    int reach = 0, far = 0, hist[64];
    memset(hist, 0, sizeof hist);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            if (grid[r][c] == '#') check(dist[r][c] == INF, "wall unreached");
            check(alt[r][c] == dist[r][c], "fixpoint matches bfs");
            if (dist[r][c] != INF) {
                reach++;
                if (dist[r][c] > far) far = dist[r][c];
                hist[dist[r][c] < 63 ? dist[r][c] : 63]++;
            }
        }
    printf("reachable cells: %d\n", reach);
    printf("farthest distance: %d\n", far);
    printf("relaxation sweeps: %d\n", sweeps);

    int goal = (H - 1) * W + (W - 1);
    if (dist[H - 1][W - 1] == INF) {
        printf("goal unreachable\n");
    } else {
        int len = 0;
        for (int v = goal; v != -1; v = par[v]) {
            grid[v / W][v % W] = '*';
            len++;
        }
        check(len == dist[H - 1][W - 1] + 1, "path length");
        printf("goal distance: %d\n", dist[H - 1][W - 1]);
    }
    for (int r = 0; r < H; r++) printf("%s\n", grid[r]);
    for (int d = 0; d <= far; d += 4) {
        int s = 0;
        for (int k = d; k < d + 4 && k <= far; k++) s += hist[k];
        printf("dist %2d..%2d: %d cells\n", d, d + 3, s);
    }
    return 0;
}
