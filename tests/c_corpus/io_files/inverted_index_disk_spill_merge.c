/*
 * title: On-disk inverted index built from spilled runs and queried with AND/OR
 * topic: io_files
 * covers: tokenization, memory-bounded posting runs, k-way run merge, delta+varint postings, term dictionary file, boolean queries, brute-force check
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

#include <ctype.h>

enum { NDOC = 150, VOCAB = 48, SPILL = 400, MAXRUN = 16 };

static const char *vocab[VOCAB];
static char vocab_buf[VOCAB][8];

static void make_vocab(void) {
    for (int i = 0; i < VOCAB; i++) {
        snprintf(vocab_buf[i], sizeof vocab_buf[i], "%c%c%c%c", 'a' + i % 7, 'k' + (i / 7) % 5, 'p' + i % 3,
                 'a' + (i * 5) % 11);
        vocab[i] = vocab_buf[i];
    }
}

typedef struct {
    uint32_t term, doc;
} Post;

static int cmp_post(const void *a, const void *b) {
    const Post *x = a, *y = b;
    if (x->term != y->term)
        return x->term < y->term ? -1 : 1;
    return x->doc < y->doc ? -1 : (x->doc > y->doc);
}

static int cmp_strp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static char docs[NDOC][160];
static unsigned char has_term[NDOC][VOCAB]; /* brute-force ground truth */

static int term_id(const char *w, const char *const *sorted) {
    int lo = 0, hi = VOCAB - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(w, sorted[mid]);
        if (c == 0)
            return mid;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}

static void put_varint(FILE *f, uint32_t v) {
    while (v >= 0x80) {
        fputc((int)((v & 0x7F) | 0x80), f);
        v >>= 7;
    }
    fputc((int)v, f);
}

static uint32_t read_varint(const unsigned char *b, size_t *p) {
    uint32_t v = 0;
    int sh = 0;
    for (;;) {
        unsigned char c = b[(*p)++];
        v |= (uint32_t)(c & 0x7F) << sh;
        sh += 7;
        if (!(c & 0x80))
            return v;
    }
}

static int nruns;
static void spill(Post *buf, int n) {
    qsort(buf, (size_t)n, sizeof buf[0], cmp_post);
    char nm[32];
    snprintf(nm, sizeof nm, "run%d.tmp", nruns++);
    FILE *f = fopen(nm, "wb");
    check(f != NULL, "run open");
    for (int i = 0; i < n; i++) {
        unsigned char r[8];
        put32(r, buf[i].term);
        put32(r + 4, buf[i].doc);
        fwrite(r, 1, 8, f);
    }
    fclose(f);
}

/* Postings for `term` from the on-disk index: dictionary entry = term id -> (offset, count). */
static int postings(const unsigned char *dict, const unsigned char *pl, int term, uint32_t *out) {
    uint32_t off = get32(dict + 8 * term), cnt = get32(dict + 8 * term + 4);
    size_t p = off;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        prev += read_varint(pl, &p);
        out[i] = prev;
    }
    return (int)cnt;
}

static int intersect(const uint32_t *a, int na, const uint32_t *b, int nb, uint32_t *out) {
    int i = 0, j = 0, n = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) { out[n++] = a[i]; i++; j++; }
        else if (a[i] < b[j]) i++;
        else j++;
    }
    return n;
}
static int unite(const uint32_t *a, int na, const uint32_t *b, int nb, uint32_t *out) {
    int i = 0, j = 0, n = 0;
    while (i < na || j < nb) {
        if (j >= nb || (i < na && a[i] < b[j])) out[n++] = a[i++];
        else if (i >= na || b[j] < a[i]) out[n++] = b[j++];
        else { out[n++] = a[i]; i++; j++; }
    }
    return n;
}

int main(void) {
    make_vocab();
    const char *sorted[VOCAB];
    memcpy(sorted, vocab, sizeof sorted);
    qsort(sorted, VOCAB, sizeof sorted[0], cmp_strp);
    for (int i = 1; i < VOCAB; i++)
        check(strcmp(sorted[i - 1], sorted[i]) != 0, "distinct vocabulary");
    /* Documents: 4..12 words, skewed toward low term numbers. */
    for (int d = 0; d < NDOC; d++) {
        int nw = 4 + (int)rndn(9);
        size_t l = 0;
        for (int w = 0; w < nw; w++) {
            uint32_t a = rndn(VOCAB), b = rndn(VOCAB);
            int t = (int)(a < b ? a : b);
            l += (size_t)snprintf(docs[d] + l, sizeof docs[d] - l, "%s%s", w ? " " : "", vocab[t]);
        }
    }
    /* Indexing: tokenize, emit (term, doc) postings, spill sorted runs when the buffer is full. */
    static Post buf[SPILL];
    int nbuf = 0;
    long total_tokens = 0;
    for (int d = 0; d < NDOC; d++) {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%s", docs[d]);
        for (char *w = strtok(tmp, " "); w; w = strtok(NULL, " ")) {
            int t = term_id(w, sorted);
            check(t >= 0, "known term");
            has_term[d][t] = 1;
            total_tokens++;
            buf[nbuf].term = (uint32_t)t;
            buf[nbuf].doc = (uint32_t)d;
            if (++nbuf == SPILL) {
                spill(buf, nbuf);
                nbuf = 0;
            }
        }
    }
    if (nbuf)
        spill(buf, nbuf);
    check(nruns <= MAXRUN, "run count");
    /* Merge runs: linear-select the smallest head, drop duplicate (term, doc) pairs, write postings. */
    FILE *rf[MAXRUN];
    Post head[MAXRUN];
    int live[MAXRUN];
    for (int r = 0; r < nruns; r++) {
        char nm[32];
        snprintf(nm, sizeof nm, "run%d.tmp", r);
        rf[r] = fopen(nm, "rb");
        unsigned char rec[8];
        live[r] = fread(rec, 1, 8, rf[r]) == 8;
        if (live[r]) {
            head[r].term = get32(rec);
            head[r].doc = get32(rec + 4);
        }
    }
    FILE *pf = fopen("postings.bin", "wb");
    unsigned char dict[VOCAB * 8];
    memset(dict, 0, sizeof dict);
    long pos = 0, npost = 0;
    int cur_term = -1;
    uint32_t last_doc = 0, cnt = 0, start_off = 0;
    Post prev = {0xFFFFFFFFu, 0};
    for (;;) {
        int best = -1;
        for (int r = 0; r < nruns; r++)
            if (live[r] && (best < 0 || cmp_post(&head[r], &head[best]) < 0))
                best = r;
        if (best < 0)
            break;
        Post p = head[best];
        unsigned char rec[8];
        live[best] = fread(rec, 1, 8, rf[best]) == 8;
        if (live[best]) {
            head[best].term = get32(rec);
            head[best].doc = get32(rec + 4);
        }
        if (p.term == prev.term && p.doc == prev.doc)
            continue;
        prev = p;
        if ((int)p.term != cur_term) {
            if (cur_term >= 0) {
                put32(dict + 8 * cur_term, start_off);
                put32(dict + 8 * cur_term + 4, cnt);
            }
            cur_term = (int)p.term;
            start_off = (uint32_t)pos;
            cnt = 0;
            last_doc = 0;
        }
        long before = ftell(pf);
        put_varint(pf, p.doc - last_doc);
        pos += ftell(pf) - before;
        last_doc = p.doc;
        cnt++;
        npost++;
    }
    put32(dict + 8 * cur_term, start_off);
    put32(dict + 8 * cur_term + 4, cnt);
    fclose(pf);
    for (int r = 0; r < nruns; r++) {
        fclose(rf[r]);
        char nm[32];
        snprintf(nm, sizeof nm, "run%d.tmp", r);
        unlink(nm);
    }
    spit("terms.dict", dict, sizeof dict);
    printf("docs=%d tokens=%ld runs=%d postings=%ld posting bytes=%ld (raw 4-byte ids: %ld)\n", NDOC, total_tokens,
           nruns, npost, pos, npost * 4);

    /* Query phase reads only the two files. */
    size_t dn, pn;
    unsigned char *d2 = slurp("terms.dict", &dn), *pl = slurp("postings.bin", &pn);
    static const char *ops[6] = {"AND", "OR", "AND", "AND", "OR", "AND"};
    for (int q = 0; q < 6; q++) {
        /* two terms picked by sorted position */
        int ta = (q * 7 + 1) % VOCAB, tb = (q * 11 + 5) % VOCAB;
        static uint32_t a[NDOC], b[NDOC], r[NDOC];
        int na = postings(d2, pl, ta, a), nb = postings(d2, pl, tb, b);
        int and_q = !strcmp(ops[q], "AND");
        int nr = and_q ? intersect(a, na, b, nb, r) : unite(a, na, b, nb, r);
        int brute = 0;
        for (int d = 0; d < NDOC; d++) {
            int m = and_q ? (has_term[d][ta] && has_term[d][tb]) : (has_term[d][ta] || has_term[d][tb]);
            if (m) {
                check(brute < nr && r[brute] == (uint32_t)d, "result doc id");
                brute++;
            }
        }
        check(brute == nr, "result count");
        printf("%-3s %s %s -> %d docs (df %d, %d)", ops[q], sorted[ta], sorted[tb], nr, na, nb);
        for (int i = 0; i < nr && i < 5; i++)
            printf(" %u", r[i]);
        printf("\n");
    }
    for (int t = 0; t < VOCAB; t++) {
        static uint32_t a[NDOC];
        int n = postings(d2, pl, t, a);
        int brute = 0;
        for (int d = 0; d < NDOC; d++)
            brute += has_term[d][t];
        check(n == brute, "document frequency");
    }
    free(d2);
    free(pl);
    unlink("terms.dict");
    unlink("postings.bin");
    return 0;
}
