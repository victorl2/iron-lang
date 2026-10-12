/*
 * title: File-backed FIFO queue running a grid BFS
 * topic: io_files
 * covers: queue on a file, persistent head offset, compaction via copy and rename, breadth-first search, comparison with in-memory BFS
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)


static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned rnd_below(unsigned n) {
    unsigned v = (unsigned)(xs() >> 33);
    return v % n;
}

#define W 24
#define H 16
#define REC 8 /* x u16, y u16, dist u32 */

typedef struct {
    int fd;
    long head;  /* records consumed */
    long tail;  /* records stored */
    int compactions;
    long peak_len;
} FQueue;

typedef struct { unsigned x, y, d; } Node;

static void enc(unsigned char *b, Node n) {
    b[0] = (unsigned char)n.x; b[1] = (unsigned char)(n.x >> 8);
    b[2] = (unsigned char)n.y; b[3] = (unsigned char)(n.y >> 8);
    for (int i = 0; i < 4; i++) b[4 + i] = (unsigned char)(n.d >> (8 * i));
}
static Node dec(const unsigned char *b) {
    Node n;
    n.x = b[0] | ((unsigned)b[1] << 8);
    n.y = b[2] | ((unsigned)b[3] << 8);
    n.d = 0;
    for (int i = 0; i < 4; i++) n.d |= (unsigned)b[4 + i] << (8 * i);
    return n;
}

static void q_put(FQueue *q, Node n) {
    unsigned char b[REC];
    enc(b, n);
    CHECK(pwrite(q->fd, b, REC, (off_t)q->tail * REC) == REC);
    q->tail++;
    if (q->tail - q->head > q->peak_len) q->peak_len = q->tail - q->head;
}

/* Move live records to the front of a fresh file, rename it over the old one. */
static void compact(FQueue *q) {
    int nf = open("queue.tmp", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(nf >= 0);
    unsigned char b[REC];
    long n = q->tail - q->head;
    for (long i = 0; i < n; i++) {
        CHECK(pread(q->fd, b, REC, (off_t)(q->head + i) * REC) == REC);
        CHECK(write(nf, b, REC) == REC);
    }
    CHECK(rename("queue.tmp", "queue.dat") == 0);
    close(q->fd);
    q->fd = nf;
    q->head = 0;
    q->tail = n;
    q->compactions++;
}

static int q_get(FQueue *q, Node *out) {
    if (q->head == q->tail) return 0;
    unsigned char b[REC];
    CHECK(pread(q->fd, b, REC, (off_t)q->head * REC) == REC);
    *out = dec(b);
    q->head++;
    if (q->head >= 16 && q->head * 2 > q->tail) compact(q);
    return 1;
}

static unsigned char grid[H][W];

static void build_grid(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) grid[y][x] = rnd_below(100) < 20;
    grid[0][0] = 0;
    grid[H - 1][W - 1] = 0;
}

static int dist_file[H][W], dist_mem[H][W];

static void bfs_file(FQueue *q) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) dist_file[y][x] = -1;
    dist_file[0][0] = 0;
    q_put(q, (Node){0, 0, 0});
    Node n;
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    while (q_get(q, &n)) {
        for (int k = 0; k < 4; k++) {
            int nx = (int)n.x + dx[k], ny = (int)n.y + dy[k];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H || grid[ny][nx] || dist_file[ny][nx] >= 0) continue;
            dist_file[ny][nx] = (int)n.d + 1;
            q_put(q, (Node){(unsigned)nx, (unsigned)ny, n.d + 1});
        }
    }
}

static void bfs_mem(void) {
    static Node qu[W * H];
    int h = 0, t = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) dist_mem[y][x] = -1;
    dist_mem[0][0] = 0;
    qu[t++] = (Node){0, 0, 0};
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    while (h < t) {
        Node n = qu[h++];
        for (int k = 0; k < 4; k++) {
            int nx = (int)n.x + dx[k], ny = (int)n.y + dy[k];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H || grid[ny][nx] || dist_mem[ny][nx] >= 0) continue;
            dist_mem[ny][nx] = (int)n.d + 1;
            qu[t++] = (Node){(unsigned)nx, (unsigned)ny, n.d + 1};
        }
    }
}

int main(void) {
    build_grid();
    FQueue q;
    q.fd = open("queue.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(q.fd >= 0);
    q.head = q.tail = 0; q.compactions = 0; q.peak_len = 0;
    bfs_file(&q);
    bfs_mem();

    int reachable = 0, far = 0, mismatches = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (dist_file[y][x] != dist_mem[y][x]) mismatches++;
            if (dist_file[y][x] >= 0) {
                reachable++;
                if (dist_file[y][x] > far) far = dist_file[y][x];
            }
        }
    printf("grid %dx%d: reachable=%d farthest=%d\n", W, H, reachable, far);
    printf("distance to bottom-right: %d\n", dist_file[H - 1][W - 1]);
    printf("queue peak length=%ld compactions=%d\n", q.peak_len, q.compactions);
    printf("file BFS matches in-memory BFS: %s\n", mismatches == 0 ? "yes" : "NO");
    CHECK(mismatches == 0 && q.head == q.tail);
    CHECK(q.compactions > 0);

    for (int y = 0; y < H; y++) {
        char line[W + 1];
        for (int x = 0; x < W; x++) {
            int d = dist_file[y][x];
            line[x] = grid[y][x] ? '#' : d < 0 ? '?' : (char)"0123456789abcdefghijklmnopqrstuvwxyz"[d % 36];
        }
        line[W] = 0;
        printf("%s\n", line);
    }
    close(q.fd);
    unlink("queue.dat");
    return 0;
}
