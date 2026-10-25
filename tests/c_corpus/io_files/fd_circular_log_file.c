/*
 * title: Circular log in a fixed-size file
 * topic: io_files
 * covers: ring buffer on disk, wrap-around with split pwrite, header persistence, length-prefixed records, reopen recovery, model deque
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
static inline unsigned long long fnv(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV0 14695981039346656037ULL

static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define CAP 1024      /* data area bytes */
#define HDR 16        /* head u32, used u32, seq u32, magic u32 */
#define MAXREC 60

typedef struct {
    int fd;
    unsigned head;  /* offset of the oldest record in the data area */
    unsigned used;  /* bytes in use */
    unsigned seq;   /* next sequence number */
} Ring;

static void put32(unsigned char *p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static unsigned get32(const unsigned char *p) { unsigned v = 0; for (int i = 0; i < 4; i++) v |= (unsigned)p[i] << (8 * i); return v; }

static void save_hdr(Ring *r) {
    unsigned char h[HDR];
    put32(h, r->head); put32(h + 4, r->used); put32(h + 8, r->seq); put32(h + 12, 0x4c4f4721u);
    CHECK(pwrite(r->fd, h, HDR, 0) == HDR);
}

static void load_hdr(Ring *r) {
    unsigned char h[HDR];
    CHECK(pread(r->fd, h, HDR, 0) == HDR && get32(h + 12) == 0x4c4f4721u);
    r->head = get32(h); r->used = get32(h + 4); r->seq = get32(h + 8);
}

/* copy n bytes at logical ring offset `at`, splitting at the end of the data area */
static void ring_put(Ring *r, unsigned at, const unsigned char *p, unsigned n, int *splits) {
    at %= CAP;
    unsigned first = CAP - at < n ? CAP - at : n;
    CHECK(pwrite(r->fd, p, first, HDR + at) == (ssize_t)first);
    if (first < n) {
        CHECK(pwrite(r->fd, p + first, n - first, HDR) == (ssize_t)(n - first));
        (*splits)++;
    }
}

static void ring_get(Ring *r, unsigned at, unsigned char *p, unsigned n) {
    at %= CAP;
    unsigned first = CAP - at < n ? CAP - at : n;
    CHECK(pread(r->fd, p, first, HDR + at) == (ssize_t)first);
    if (first < n) CHECK(pread(r->fd, p + first, n - first, HDR) == (ssize_t)(n - first));
}

/* record = len u8, seq u32, payload */
static unsigned rec_size(unsigned len) { return 5 + len; }

static void drop_oldest(Ring *r) {
    unsigned char h[5];
    ring_get(r, r->head, h, 5);
    unsigned sz = rec_size(h[0]);
    r->head = (r->head + sz) % CAP;
    r->used -= sz;
}

static void append(Ring *r, const char *msg, int *splits, int *dropped) {
    unsigned len = (unsigned)strlen(msg);
    unsigned need = rec_size(len);
    while (r->used + need > CAP) { drop_oldest(r); (*dropped)++; }
    unsigned char rec[5 + MAXREC];
    rec[0] = (unsigned char)len;
    put32(rec + 1, r->seq);
    memcpy(rec + 5, msg, len);
    ring_put(r, r->head + r->used, rec, need, splits);
    r->used += need;
    r->seq++;
    save_hdr(r);
}

/* collect surviving sequence numbers and a digest of their payloads */
static int scan(Ring *r, unsigned *first_seq, unsigned *last_seq, unsigned long long *digest) {
    unsigned pos = r->head, left = r->used;
    int n = 0;
    *digest = FNV0;
    while (left > 0) {
        unsigned char h[5], payload[MAXREC];
        ring_get(r, pos, h, 5);
        unsigned len = h[0], sq = get32(h + 1);
        CHECK(len <= MAXREC && rec_size(len) <= left);
        ring_get(r, pos + 5, payload, len);
        *digest = fnv(*digest, payload, len);
        if (n == 0) *first_seq = sq;
        *last_seq = sq;
        pos = (pos + rec_size(len)) % CAP;
        left -= rec_size(len);
        n++;
    }
    return n;
}

int main(void) {
    Ring r;
    r.fd = open("ring.log", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(r.fd >= 0);
    r.head = 0; r.used = 0; r.seq = 0;
    CHECK(ftruncate(r.fd, HDR + CAP) == 0);
    save_hdr(&r);

    /* model of what should survive: the most recent messages that fit */
    static char msgs[400][MAXREC + 1];
    int splits = 0, dropped = 0;
    int total = 400;
    for (int i = 0; i < total; i++) {
        unsigned len = 8 + rnd_below(MAXREC - 8);
        for (unsigned k = 0; k < len; k++) msgs[i][k] = (char)('a' + (i * 7 + (int)k) % 26);
        msgs[i][len] = 0;
        append(&r, msgs[i], &splits, &dropped);
        if (i == 9 || i == 39) {
            unsigned f, l; unsigned long long d;
            int n = scan(&r, &f, &l, &d);
            printf("after %3d appends: kept=%d first=%u last=%u used=%u\n", i + 1, n, f, l, r.used);
        }
    }
    unsigned f, l; unsigned long long dig;
    int kept = scan(&r, &f, &l, &dig);
    printf("after %d appends: kept=%d first=%u last=%u used=%u head=%u\n", total, kept, f, l, r.used, r.head);
    printf("wrapping writes split in two: %d, dropped: %d\n", splits, dropped);
    CHECK(splits > 0 && dropped + kept == total && l == (unsigned)total - 1 && f == (unsigned)dropped);
    CHECK(fsize(r.fd) == HDR + CAP);

    /* the survivors must be exactly the newest suffix */
    unsigned long long want = FNV0;
    unsigned sum = 0;
    for (int i = total - kept; i < total; i++) {
        want = fnv(want, msgs[i], strlen(msgs[i]));
        sum += rec_size((unsigned)strlen(msgs[i]));
    }
    CHECK(want == dig && sum == r.used);
    /* and adding one more would not fit without dropping */
    CHECK(r.used <= CAP);
    printf("digest of surviving payloads: %016llx\n", dig);

    /* reopen from disk: header alone reconstructs the ring */
    close(r.fd);
    Ring q;
    q.fd = open("ring.log", O_RDWR);
    CHECK(q.fd >= 0);
    load_hdr(&q);
    unsigned f2, l2; unsigned long long dig2;
    int kept2 = scan(&q, &f2, &l2, &dig2);
    printf("after reopen: kept=%d first=%u last=%u same digest=%d\n", kept2, f2, l2, dig2 == dig);
    CHECK(kept2 == kept && f2 == f && l2 == l && dig2 == dig && q.seq == (unsigned)total);

    /* continue appending after recovery */
    int s2 = 0, d2 = 0;
    append(&q, "resumed after reopen", &s2, &d2);
    kept2 = scan(&q, &f2, &l2, &dig2);
    printf("after resume: last=%u kept=%d\n", l2, kept2);
    CHECK(l2 == (unsigned)total);
    close(q.fd);
    unlink("ring.log");
    return 0;
}
