/*
 * title: External merge sort of a file larger than the memory buffer
 * topic: io_files
 * covers: run generation, multi-pass k-way merge, buffered run readers, stability via sequence keys, file cleanup
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
typedef struct {
    uint32_t key;
    uint32_t seq;
} Rec; /* 8 bytes on disk, little endian */

enum { MEM_RECS = 200, FANIN = 4, NREC = 5000, RBUF = 16 };

typedef struct {
    int fd;
    off_t off;
    Rec buf[RBUF];
    int n, i;
    int done;
} Reader;

static long recs_read, recs_written;

static void rd_open(Reader *r, const char *path) {
    r->fd = open(path, O_RDONLY);
    check(r->fd >= 0, "open run");
    r->off = 0;
    r->n = r->i = 0;
    r->done = 0;
}
static int rd_next(Reader *r, Rec *out) {
    if (r->i == r->n) {
        unsigned char raw[RBUF * 8];
        size_t got = pread_upto(r->fd, raw, sizeof raw, r->off);
        r->off += (off_t)got;
        r->n = (int)(got / 8);
        r->i = 0;
        for (int k = 0; k < r->n; k++) {
            r->buf[k].key = get32(raw + 8 * k);
            r->buf[k].seq = get32(raw + 8 * k + 4);
        }
        if (r->n == 0)
            return 0;
    }
    *out = r->buf[r->i++];
    recs_read++;
    return 1;
}

typedef struct {
    int fd;
    unsigned char buf[RBUF * 8];
    int n;
} Writer;
static void wr_flush(Writer *w) {
    write_all(w->fd, w->buf, (size_t)w->n * 8);
    w->n = 0;
}
static void wr_put(Writer *w, Rec r) {
    put32(w->buf + 8 * w->n, r.key);
    put32(w->buf + 8 * w->n + 4, r.seq);
    if (++w->n == RBUF)
        wr_flush(w);
    recs_written++;
}

static int cmp_rec(const void *a, const void *b) {
    const Rec *x = a, *y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}
static int less(Rec a, Rec b) { return cmp_rec(&a, &b) < 0; }

static void run_name(char *out, size_t cap, int pass, int idx) { snprintf(out, cap, "run_%d_%d.tmp", pass, idx); }

/* Merge runs [first, first+cnt) of `pass` into one run of pass+1 named idx. */
static void merge_group(int pass, int first, int cnt, int outidx) {
    Reader rd[FANIN];
    Rec head[FANIN];
    int live[FANIN];
    char nm[48];
    for (int k = 0; k < cnt; k++) {
        run_name(nm, sizeof nm, pass, first + k);
        rd_open(&rd[k], nm);
        live[k] = rd_next(&rd[k], &head[k]);
    }
    run_name(nm, sizeof nm, pass + 1, outidx);
    Writer w;
    w.fd = open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(w.fd >= 0, "open merged");
    w.n = 0;
    for (;;) {
        int best = -1;
        for (int k = 0; k < cnt; k++)
            if (live[k] && (best < 0 || less(head[k], head[best])))
                best = k;
        if (best < 0)
            break;
        wr_put(&w, head[best]);
        live[best] = rd_next(&rd[best], &head[best]);
    }
    wr_flush(&w);
    close(w.fd);
    for (int k = 0; k < cnt; k++) {
        close(rd[k].fd);
        run_name(nm, sizeof nm, pass, first + k);
        unlink(nm);
    }
}

int main(void) {
    static Rec all[NREC];
    unsigned char raw[8];
    int fd = open("input.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 0; i < NREC; i++) {
        all[i].key = rndn(1000); /* many duplicate keys */
        all[i].seq = (uint32_t)i;
        put32(raw, all[i].key);
        put32(raw + 4, all[i].seq);
        write_all(fd, raw, 8);
    }
    close(fd);

    /* Phase 1: read MEM_RECS records at a time, sort in memory, write a run. */
    fd = open("input.dat", O_RDONLY);
    int nruns = 0;
    static Rec mem[MEM_RECS];
    for (off_t off = 0;; off += MEM_RECS * 8) {
        unsigned char chunk[MEM_RECS * 8];
        size_t got = pread_upto(fd, chunk, sizeof chunk, off);
        int n = (int)(got / 8);
        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            mem[i].key = get32(chunk + 8 * i);
            mem[i].seq = get32(chunk + 8 * i + 4);
        }
        qsort(mem, (size_t)n, sizeof mem[0], cmp_rec);
        char nm[48];
        run_name(nm, sizeof nm, 0, nruns++);
        Writer w;
        w.fd = open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        w.n = 0;
        for (int i = 0; i < n; i++)
            wr_put(&w, mem[i]);
        wr_flush(&w);
        close(w.fd);
    }
    close(fd);
    printf("records=%d memory=%d initial runs=%d fan-in=%d\n", NREC, MEM_RECS, nruns, FANIN);

    int pass = 0, runs = nruns;
    while (runs > 1) {
        int out = 0;
        for (int first = 0; first < runs; first += FANIN) {
            int cnt = runs - first < FANIN ? runs - first : FANIN;
            merge_group(pass, first, cnt, out++);
        }
        pass++;
        runs = out;
        printf("pass %d -> %d runs\n", pass, runs);
    }
    char final[48];
    run_name(final, sizeof final, pass, 0);
    size_t n;
    unsigned char *b = slurp(final, &n);
    check(n == (size_t)NREC * 8, "sorted size");
    qsort(all, NREC, sizeof all[0], cmp_rec);
    uint32_t dig = 0;
    for (int i = 0; i < NREC; i++) {
        check(get32(b + 8 * i) == all[i].key && get32(b + 8 * i + 4) == all[i].seq, "matches in-memory sort");
        dig = dig * 16777619u ^ (get32(b + 8 * i) + 3u * get32(b + 8 * i + 4));
    }
    printf("first=(%u,%u) last=(%u,%u)\n", get32(b), get32(b + 4), get32(b + 8 * (NREC - 1)),
           get32(b + 8 * (NREC - 1) + 4));
    printf("records written=%ld read during merge=%ld\n", recs_written, recs_read);
    printf("digest=%08x\n", dig);
    free(b);
    unlink(final);
    unlink("input.dat");
    return 0;
}
