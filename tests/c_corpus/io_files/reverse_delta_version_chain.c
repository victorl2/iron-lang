/*
 * title: RCS-style version store: head kept whole, older versions as reverse line deltas
 * topic: io_files
 * covers: line diff via LCS, copy/delete/insert edit scripts, reverse delta chain in one history file, checkout of any revision, storage accounting
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

enum { MAXL = 80, LEN = 24, NREV = 14 };

typedef struct {
    int n;
    char l[MAXL][LEN];
} Doc;

/* Script text: "C k", "D k", "I k" followed by k lines. Turns `from` into `to`. */
static size_t make_script(const Doc *from, const Doc *to, char *out, size_t cap) {
    static int lcs[MAXL + 1][MAXL + 1];
    for (int i = from->n; i >= 0; i--)
        for (int j = to->n; j >= 0; j--) {
            if (i == from->n || j == to->n)
                lcs[i][j] = 0;
            else if (!strcmp(from->l[i], to->l[j]))
                lcs[i][j] = 1 + lcs[i + 1][j + 1];
            else
                lcs[i][j] = lcs[i + 1][j] >= lcs[i][j + 1] ? lcs[i + 1][j] : lcs[i][j + 1];
        }
    size_t len = 0;
    int i = 0, j = 0;
    char kind = 0;
    int run = 0, start_j = 0;
#define FLUSH()                                                                                       \
    do {                                                                                              \
        if (run) {                                                                                    \
            len += (size_t)snprintf(out + len, cap - len, "%c %d\n", kind, run);                      \
            if (kind == 'I')                                                                          \
                for (int q = 0; q < run; q++)                                                         \
                    len += (size_t)snprintf(out + len, cap - len, "%s\n", to->l[start_j + q]);        \
        }                                                                                             \
        run = 0;                                                                                      \
        kind = 0;                                                                                     \
    } while (0)
    while (i < from->n || j < to->n) {
        char k;
        if (i < from->n && j < to->n && !strcmp(from->l[i], to->l[j]) && lcs[i][j] == 1 + lcs[i + 1][j + 1])
            k = 'C';
        else if (i < from->n && (j == to->n || lcs[i + 1][j] >= lcs[i][j + 1]))
            k = 'D';
        else
            k = 'I';
        if (k != kind) {
            FLUSH();
            kind = k;
            start_j = j;
        }
        run++;
        if (k == 'C') { i++; j++; }
        else if (k == 'D') i++;
        else j++;
    }
    FLUSH();
#undef FLUSH
    return len;
}

static void apply_script(const Doc *from, const char *script, Doc *to) {
    to->n = 0;
    int i = 0;
    const char *p = script;
    while (*p) {
        char kind = p[0];
        int k = atoi(p + 2);
        p = strchr(p, '\n') + 1;
        if (kind == 'C') {
            for (int q = 0; q < k; q++)
                strcpy(to->l[to->n++], from->l[i++]);
        } else if (kind == 'D') {
            i += k;
        } else {
            for (int q = 0; q < k; q++) {
                const char *e = strchr(p, '\n');
                size_t l = (size_t)(e - p);
                memcpy(to->l[to->n], p, l);
                to->l[to->n++][l] = 0;
                p = e + 1;
            }
        }
    }
    check(i == from->n, "script consumed the source document");
}

static void script_stats(const char *sc, int *c, int *d, int *ins) {
    *c = *d = *ins = 0;
    for (const char *p = sc; *p;) {
        int k = atoi(p + 2);
        char kind = *p;
        p = strchr(p, '\n') + 1;
        if (kind == 'C')
            *c += k;
        else if (kind == 'D')
            *d += k;
        else {
            *ins += k;
            for (int q = 0; q < k; q++)
                p = strchr(p, '\n') + 1;
        }
    }
}

static void doc_to_text(const Doc *d, char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < d->n; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s\n", d->l[i]);
    if (d->n == 0)
        out[0] = 0;
}

static int doc_eq(const Doc *a, const Doc *b) {
    if (a->n != b->n)
        return 0;
    for (int i = 0; i < a->n; i++)
        if (strcmp(a->l[i], b->l[i]))
            return 0;
    return 1;
}

int main(void) {
    static Doc rev[NREV];
    /* Revision 0: 30 lines; each later revision edits, inserts and deletes a few lines. */
    rev[0].n = 30;
    for (int i = 0; i < 30; i++)
        snprintf(rev[0].l[i], LEN, "line %02d base", i);
    for (int r = 1; r < NREV; r++) {
        rev[r] = rev[r - 1];
        int edits = 1 + (int)rndn(4);
        for (int e = 0; e < edits; e++) {
            int op = (int)rndn(3);
            int at = (int)rndn((uint32_t)rev[r].n);
            if (op == 0) {
                snprintf(rev[r].l[at], LEN, "edit r%d #%u", r, rndn(100));
            } else if (op == 1 && rev[r].n < MAXL - 1) {
                for (int q = rev[r].n; q > at; q--)
                    strcpy(rev[r].l[q], rev[r].l[q - 1]);
                snprintf(rev[r].l[at], LEN, "new r%d #%u", r, rndn(100));
                rev[r].n++;
            } else if (rev[r].n > 10) {
                for (int q = at; q < rev[r].n - 1; q++)
                    strcpy(rev[r].l[q], rev[r].l[q + 1]);
                rev[r].n--;
            }
        }
    }
    /* Build the history file: after each commit, append the reverse delta (new -> old) and rewrite head. */
    unlink("hist.rcs");
    long full_bytes = 0;
    int hist_fd = open("hist.rcs", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(hist_fd >= 0, "hist open");
    static char script[8192], text[4096];
    long offs[NREV], lens[NREV];
    doc_to_text(&rev[0], text, sizeof text);
    spit("head.txt", text, strlen(text));
    full_bytes += (long)strlen(text);
    long pos = 0;
    for (int r = 1; r < NREV; r++) {
        size_t sl = make_script(&rev[r], &rev[r - 1], script, sizeof script);
        offs[r] = pos;
        lens[r] = (long)sl;
        write_all(hist_fd, script, sl);
        pos += (long)sl;
        doc_to_text(&rev[r], text, sizeof text);
        spit("head.txt", text, strlen(text)); /* head is always the newest full text */
        full_bytes += (long)strlen(text);
    }
    close(hist_fd);
    /* Checkout any revision by walking reverse deltas from the head. */
    size_t hn;
    unsigned char *head_raw = slurp("head.txt", &hn);
    Doc cur;
    cur.n = 0;
    for (size_t i = 0; i < hn;) {
        const unsigned char *e = memchr(head_raw + i, '\n', hn - i);
        size_t l = (size_t)(e - (head_raw + i));
        memcpy(cur.l[cur.n], head_raw + i, l);
        cur.l[cur.n++][l] = 0;
        i += l + 1;
    }
    free(head_raw);
    check(doc_eq(&cur, &rev[NREV - 1]), "head is newest revision");
    int hist_size = (int)file_size("hist.rcs");
    unsigned char *hist = slurp("hist.rcs", &hn);
    printf("revisions=%d head bytes=%zu history bytes=%d\n", NREV, strlen(text), hist_size);
    for (int r = NREV - 2; r >= 0; r--) {
        char sc[8192];
        memcpy(sc, hist + offs[r + 1], (size_t)lens[r + 1]);
        sc[lens[r + 1]] = 0;
        Doc prev;
        apply_script(&cur, sc, &prev);
        check(doc_eq(&prev, &rev[r]), "checkout matches original revision");
        int c, d, ins;
        script_stats(sc, &c, &d, &ins);
        printf("rev %2d <- rev %2d: keep %2d delete %d insert %d (%ld bytes)\n", r, r + 1, c, d, ins, lens[r + 1]);
        cur = prev;
    }
    free(hist);
    printf("all %d older revisions reproduced exactly from reverse deltas\n", NREV - 1);
    printf("storing every full text would cost %ld bytes; head+deltas cost %ld\n", full_bytes, (long)hist_size + (long)strlen(text));
    unlink("hist.rcs");
    unlink("head.txt");
    return 0;
}
