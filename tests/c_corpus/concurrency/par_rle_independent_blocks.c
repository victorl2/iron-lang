/*
 * title: Parallel RLE compression of independent blocks
 * topic: concurrency
 * covers: block framing, per-block compressed buffers, ordered concatenation, decompress round trip, escape-free run encoding
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*ParFn)(void *ctx, int tid, int nt);
typedef struct {
    ParFn fn;
    void *ctx;
    int tid;
    int nt;
} ParJob;

static void *par_tramp(void *p) {
    ParJob *j = p;
    j->fn(j->ctx, j->tid, j->nt);
    return NULL;
}

/* Run fn on nt threads (nt <= 8) and join them all. */
static inline void par_run(int nt, ParFn fn, void *ctx) {
    pthread_t th[8];
    ParJob jobs[8];
    for (int i = 0; i < nt; i++) {
        jobs[i].fn = fn;
        jobs[i].ctx = ctx;
        jobs[i].tid = i;
        jobs[i].nt = nt;
        if (pthread_create(&th[i], NULL, par_tramp, &jobs[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < nt; i++)
        pthread_join(th[i], NULL);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { BLOCK = 1000, N = 12345, NT = 5 };

typedef struct {
    const unsigned char *in;
    int nblocks;
    unsigned char *out[64];
    int outlen[64];
} Rle;

/* Pairs of (count 1..255, byte). Runs never cross a block boundary, so blocks
 * are independent. */
static int encode_block(const unsigned char *p, int n, unsigned char *out) {
    int o = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && p[j] == p[i] && j - i < 255)
            j++;
        out[o++] = (unsigned char)(j - i);
        out[o++] = p[i];
        i = j;
    }
    return o;
}

static void enc_worker(void *ctx, int tid, int nt) {
    Rle *r = ctx;
    for (int b = tid; b < r->nblocks; b += nt) {
        int lo = b * BLOCK, hi = lo + BLOCK > N ? N : lo + BLOCK;
        r->out[b] = malloc((size_t)(hi - lo) * 2);
        check(r->out[b] != NULL, "alloc");
        r->outlen[b] = encode_block(r->in + lo, hi - lo, r->out[b]);
    }
}

static int decode(const unsigned char *in, int inlen, unsigned char *out) {
    int o = 0;
    for (int i = 0; i + 1 < inlen; i += 2)
        for (int k = 0; k < in[i]; k++)
            out[o++] = in[i + 1];
    return o;
}

int main(void) {
    static unsigned char data[N];
    uint64_t seed = 42;
    /* mixture of long runs, short runs, and noise */
    int i = 0;
    while (i < N) {
        uint64_t r = sm64(&seed);
        int kind = (int)(r % 4u);
        int len = 1 + (int)((r >> 8) % (kind == 0 ? 600u : kind == 1 ? 12u : 3u));
        unsigned char v = (unsigned char)(r >> 32);
        for (int k = 0; k < len && i < N; k++, i++)
            data[i] = kind == 3 ? (unsigned char)(v + k * 37) : v;
    }
    Rle r;
    memset(&r, 0, sizeof r);
    r.in = data;
    r.nblocks = (N + BLOCK - 1) / BLOCK;
    par_run(NT, enc_worker, &r);
    /* ordered concatenation with a length-prefixed frame per block */
    static unsigned char stream[N * 2 + 64 * 4];
    int slen = 0;
    for (int b = 0; b < r.nblocks; b++) {
        stream[slen++] = (unsigned char)(r.outlen[b] >> 8);
        stream[slen++] = (unsigned char)(r.outlen[b] & 255);
        memcpy(stream + slen, r.out[b], (size_t)r.outlen[b]);
        slen += r.outlen[b];
    }
    /* sequential single-block-at-a-time encoding must produce the same bytes */
    static unsigned char stream2[N * 2 + 64 * 4];
    int slen2 = 0;
    for (int b = 0; b < r.nblocks; b++) {
        int lo = b * BLOCK, hi = lo + BLOCK > N ? N : lo + BLOCK;
        unsigned char tmp[BLOCK * 2];
        int l = encode_block(data + lo, hi - lo, tmp);
        stream2[slen2++] = (unsigned char)(l >> 8);
        stream2[slen2++] = (unsigned char)(l & 255);
        memcpy(stream2 + slen2, tmp, (size_t)l);
        slen2 += l;
    }
    check(slen == slen2 && memcmp(stream, stream2, (size_t)slen) == 0, "parallel stream equals sequential");
    /* decode */
    static unsigned char back[N];
    int blen = 0, pos = 0;
    while (pos < slen) {
        int l = stream[pos] << 8 | stream[pos + 1];
        pos += 2;
        blen += decode(stream + pos, l, back + blen);
        pos += l;
    }
    check(blen == N && memcmp(back, data, N) == 0, "round trip");
    printf("input=%d compressed=%d blocks=%d ratio*100=%d\n", N, slen, r.nblocks, slen * 100 / N);
    for (int b = 0; b < r.nblocks; b++) {
        if (b < 4 || b == r.nblocks - 1)
            printf("block %2d -> %d bytes\n", b, r.outlen[b]);
        free(r.out[b]);
    }
    return 0;
}
