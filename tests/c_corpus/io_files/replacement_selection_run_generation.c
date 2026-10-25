/*
 * title: Replacement selection run generation for external sorting
 * topic: io_files
 * covers: tournament heap with run tags, run length of about twice memory on random input, sorted and reversed input extremes, streaming file input/output, run merge check
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rndn(uint32_t n) {
    uint64_t v = rnd();
    return (uint32_t)((v >> 16) % n);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read up to n bytes at offset; returns the number of bytes read (short at EOF). */
static inline size_t pread_upto(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        check(r >= 0, "pread");
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got;
}

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer (caller frees). */
static inline unsigned char *slurp(const char *path, size_t *len) {
    long n = file_size(path);
    check(n >= 0, "slurp stat");
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    check(b != NULL, "malloc");
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "slurp open");
    size_t got = pread_upto(fd, b, (size_t)n, 0);
    close(fd);
    check(got == (size_t)n, "slurp short");
    *len = (size_t)n;
    return b;
}

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

enum { M = 64 };

typedef struct {
    uint32_t run, key;
} Item;

static Item heap[M];
static int hn;

static int lt(Item a, Item b) { return a.run != b.run ? a.run < b.run : a.key < b.key; }

static void sift_down(int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < hn && lt(heap[l], heap[m])) m = l;
        if (r < hn && lt(heap[r], heap[m])) m = r;
        if (m == i)
            return;
        Item t = heap[i];
        heap[i] = heap[m];
        heap[m] = t;
        i = m;
    }
}
static void sift_up(int i) {
    while (i > 0 && lt(heap[i], heap[(i - 1) / 2])) {
        Item t = heap[i];
        heap[i] = heap[(i - 1) / 2];
        heap[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

typedef struct {
    int fd;
    unsigned char buf[256];
    size_t len;
} Out;
static void out_flush(Out *o) {
    if (o->len)
        write_all(o->fd, o->buf, o->len);
    o->len = 0;
}
static void out_put(Out *o, uint32_t k) {
    if (o->len + 4 > sizeof o->buf)
        out_flush(o);
    put32(o->buf + o->len, k);
    o->len += 4;
}

/* Reads keys from `in.dat`, writes runs run0.dat, run1.dat...; returns number of runs, fills lengths. */
static int generate_runs(uint32_t *run_len, int max_runs) {
    size_t n;
    unsigned char *in = slurp("in.dat", &n);
    size_t total = n / 4, next = 0;
    hn = 0;
    while (hn < M && next < total) {
        heap[hn].run = 0;
        heap[hn].key = get32(in + 4 * next++);
        sift_up(hn++);
    }
    int cur_run = -1;
    Out out;
    memset(&out, 0, sizeof out);
    out.fd = -1;
    int nruns = 0;
    while (hn > 0) {
        Item top = heap[0];
        if ((int)top.run != cur_run) {
            if (out.fd >= 0) {
                out_flush(&out);
                close(out.fd);
            }
            cur_run = (int)top.run;
            char nm[32];
            snprintf(nm, sizeof nm, "run%d.dat", cur_run);
            out.fd = open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            check(out.fd >= 0, "run open");
            check(nruns < max_runs, "too many runs");
            run_len[nruns++] = 0;
        }
        out_put(&out, top.key);
        run_len[nruns - 1]++;
        if (next < total) {
            uint32_t k = get32(in + 4 * next++);
            /* a key smaller than the one just output must wait for the next run */
            heap[0].key = k;
            heap[0].run = k < top.key ? top.run + 1 : top.run;
            sift_down(0);
        } else {
            heap[0] = heap[--hn];
            sift_down(0);
        }
    }
    out_flush(&out);
    if (out.fd >= 0)
        close(out.fd);
    free(in);
    return nruns;
}

int main(void) {
    enum { N = 4000 };
    static const char *shapes[4] = {"random", "sorted", "reverse sorted", "sawtooth (period 200)"};
    for (int shape = 0; shape < 4; shape++) {
        static uint32_t keys[N];
        for (int i = 0; i < N; i++) {
            switch (shape) {
            case 0: keys[i] = rndn(1000000); break;
            case 1: keys[i] = (uint32_t)i * 3u; break;
            case 2: keys[i] = (uint32_t)(N - i) * 3u; break;
            default: keys[i] = (uint32_t)(i % 200) * 1000u + rndn(1000);
            }
        }
        unsigned char *raw = malloc((size_t)N * 4);
        for (int i = 0; i < N; i++)
            put32(raw + 4 * i, keys[i]);
        spit("in.dat", raw, (size_t)N * 4);
        free(raw);
        uint32_t run_len[64];
        int nruns = generate_runs(run_len, 64);
        /* Verify: each run sorted, total count preserved, multiset equal to input (checksum + sum). */
        uint64_t sum_in = 0, sum_out = 0;
        uint32_t x_in = 0, x_out = 0;
        for (int i = 0; i < N; i++) {
            sum_in += keys[i];
            x_in ^= keys[i] * 2654435761u;
        }
        long total = 0;
        uint32_t minlen = 0xFFFFFFFFu, maxlen = 0;
        for (int r = 0; r < nruns; r++) {
            char nm[32];
            snprintf(nm, sizeof nm, "run%d.dat", r);
            size_t n;
            unsigned char *b = slurp(nm, &n);
            check(n == (size_t)run_len[r] * 4, "run size");
            for (size_t i = 0; i < n / 4; i++) {
                uint32_t k = get32(b + 4 * i);
                if (i)
                    check(get32(b + 4 * i - 4) <= k, "run sorted");
                sum_out += k;
                x_out ^= k * 2654435761u;
            }
            total += (long)(n / 4);
            if (run_len[r] < minlen) minlen = run_len[r];
            if (run_len[r] > maxlen) maxlen = run_len[r];
            free(b);
        }
        check(total == N && sum_in == sum_out && x_in == x_out, "runs are a permutation of the input");
        /* Merge runs with a simple selection and confirm the result is globally sorted. */
        int pos[64];
        unsigned char *rb[64];
        size_t rn[64];
        for (int r = 0; r < nruns; r++) {
            char nm[32];
            snprintf(nm, sizeof nm, "run%d.dat", r);
            rb[r] = slurp(nm, &rn[r]);
            pos[r] = 0;
        }
        uint32_t prev = 0;
        for (int i = 0; i < N; i++) {
            int best = -1;
            for (int r = 0; r < nruns; r++)
                if ((size_t)pos[r] * 4 < rn[r] &&
                    (best < 0 || get32(rb[r] + 4 * pos[r]) < get32(rb[best] + 4 * pos[best])))
                    best = r;
            uint32_t k = get32(rb[best] + 4 * pos[best]++);
            check(i == 0 || prev <= k, "merged output sorted");
            prev = k;
        }
        for (int r = 0; r < nruns; r++) {
            free(rb[r]);
            char nm[32];
            snprintf(nm, sizeof nm, "run%d.dat", r);
            unlink(nm);
        }
        printf("%-22s runs=%2d shortest=%4u longest=%4u avg=%.1f (memory %d)\n", shapes[shape], nruns, minlen, maxlen,
               (double)N / nruns, M);
    }
    unlink("in.dat");
    return 0;
}
