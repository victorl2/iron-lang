/*
 * title: Line-oriented stream processing with chained filters
 * topic: io_files
 * covers: filter chain, function-pointer vtables, pipeline spec parsing, streaming line reader, end-of-stream flush
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
#include <ctype.h>

typedef struct Filter Filter;
struct Filter {
    const char *name;
    Filter *next;
    void (*line)(Filter *self, const char *s);
    void (*finish)(Filter *self);
    char arg[48];
    long count;
    char prev[128];
    int have_prev;
    FILE *out; /* only for sink */
};

static void emit(Filter *f, const char *s) {
    if (f->next)
        f->next->line(f->next, s);
}
static void finish_chain(Filter *f) {
    if (f->finish)
        f->finish(f);
    if (f->next)
        finish_chain(f->next);
}

static void f_grep(Filter *f, const char *s) { if (strstr(s, f->arg)) emit(f, s); }
static void f_vgrep(Filter *f, const char *s) { if (!strstr(s, f->arg)) emit(f, s); }
static void f_upper(Filter *f, const char *s) {
    char t[160];
    size_t i = 0;
    for (; s[i] && i < sizeof t - 1; i++)
        t[i] = (char)toupper((unsigned char)s[i]);
    t[i] = 0;
    emit(f, t);
}
static void f_head(Filter *f, const char *s) {
    if (f->count < atol(f->arg)) {
        f->count++;
        emit(f, s);
    }
}
static void f_uniq(Filter *f, const char *s) {
    if (!f->have_prev || strcmp(f->prev, s) != 0) {
        snprintf(f->prev, sizeof f->prev, "%s", s);
        f->have_prev = 1;
        emit(f, s);
    }
}
static void f_nl(Filter *f, const char *s) {
    char t[200];
    f->count++;
    snprintf(t, sizeof t, "%3ld  %s", f->count, s);
    emit(f, t);
}
/* Replace every occurrence of arg "a=b" (single chars) in the line. */
static void f_tr(Filter *f, const char *s) {
    char t[160];
    size_t i = 0;
    for (; s[i] && i < sizeof t - 1; i++)
        t[i] = s[i] == f->arg[0] ? f->arg[2] : s[i];
    t[i] = 0;
    emit(f, t);
}
/* Keep only the Nth space-separated field. */
static void f_field(Filter *f, const char *s) {
    int want = atoi(f->arg), idx = 0;
    const char *p = s;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        if (++idx == want) {
            char t[80];
            size_t l = (size_t)(e - p) < sizeof t - 1 ? (size_t)(e - p) : sizeof t - 1;
            memcpy(t, p, l);
            t[l] = 0;
            emit(f, t);
            return;
        }
        p = *e ? e + 1 : e;
    }
}
/* Buffers everything, emits a sorted-unique-count summary at end: "count value". */
static char *bag[512];
static int nbag;
static void f_count_line(Filter *f, const char *s) {
    (void)f;
    check(nbag < 512, "bag");
    bag[nbag++] = strdup(s);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void f_count_finish(Filter *f) {
    qsort(bag, (size_t)nbag, sizeof bag[0], cmp_str);
    for (int i = 0; i < nbag;) {
        int j = i;
        while (j < nbag && strcmp(bag[j], bag[i]) == 0)
            j++;
        char t[120];
        snprintf(t, sizeof t, "%d %s", j - i, bag[i]);
        emit(f, t);
        i = j;
    }
    for (int i = 0; i < nbag; i++)
        free(bag[i]);
    nbag = 0;
}
static void f_sink(Filter *f, const char *s) {
    f->count++;
    fprintf(f->out, "%s\n", s);
}

static Filter *build(const char *spec, Filter *pool, int *np, FILE *out) {
    Filter *first = NULL, *last = NULL;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, "|"); tok; tok = strtok(NULL, "|")) {
        Filter *f = &pool[(*np)++];
        memset(f, 0, sizeof *f);
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = 0;
            snprintf(f->arg, sizeof f->arg, "%s", colon + 1);
        }
        f->name = tok;
        if (!strcmp(tok, "grep")) f->line = f_grep;
        else if (!strcmp(tok, "vgrep")) f->line = f_vgrep;
        else if (!strcmp(tok, "upper")) f->line = f_upper;
        else if (!strcmp(tok, "head")) f->line = f_head;
        else if (!strcmp(tok, "uniq")) f->line = f_uniq;
        else if (!strcmp(tok, "nl")) f->line = f_nl;
        else if (!strcmp(tok, "tr")) f->line = f_tr;
        else if (!strcmp(tok, "field")) f->line = f_field;
        else if (!strcmp(tok, "count")) { f->line = f_count_line; f->finish = f_count_finish; }
        else check(0, "unknown filter");
        f->name = NULL;
        if (last) last->next = f; else first = f;
        last = f;
    }
    Filter *sink = &pool[(*np)++];
    memset(sink, 0, sizeof *sink);
    sink->line = f_sink;
    sink->out = out;
    last->next = sink;
    return first;
}

/* Streaming line reader: 32-byte reads, lines may straddle chunks. */
static long run_file(const char *path, Filter *head) {
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "open src");
    char line[160];
    size_t ll = 0;
    long nlines = 0;
    unsigned char chunk[32];
    size_t got;
    off_t off = 0;
    while ((got = pread_upto(fd, chunk, sizeof chunk, off)) > 0) {
        off += (off_t)got;
        for (size_t i = 0; i < got; i++) {
            if (chunk[i] == '\n') {
                line[ll] = 0;
                head->line(head, line);
                nlines++;
                ll = 0;
            } else if (ll < sizeof line - 1) {
                line[ll++] = (char)chunk[i];
            }
        }
    }
    close(fd);
    finish_chain(head);
    return nlines;
}

int main(void) {
    static const char *lv[4] = {"INFO", "WARN", "ERROR", "DEBUG"};
    static const char *ev[6] = {"login", "logout", "upload", "delete", "timeout", "sync"};
    FILE *f = fopen("app.log", "w");
    check(f != NULL, "create log");
    for (int i = 0; i < 60; i++) {
        unsigned a = rndn(4), b = rndn(6), c = rndn(3);
        fprintf(f, "%02d %s %s user%u\n", i / 3, lv[a], ev[b], c);
        if (rndn(5) == 0)
            fprintf(f, "%02d %s %s user%u\n", i / 3, lv[a], ev[b], c); /* adjacent duplicate */
    }
    fclose(f);
    static const char *specs[] = {
        "grep:ERROR|nl|head:4",
        "vgrep:DEBUG|field:3|count",
        "field:2|count",
        "grep:user1|uniq|tr:0=_|head:3",
        "grep:WARN|field:3|upper|uniq|nl",
        "grep:zzz|nl",
    };
    for (unsigned s = 0; s < sizeof specs / sizeof specs[0]; s++) {
        Filter pool[16];
        int np = 0;
        FILE *out = fopen("out.txt", "w");
        check(out != NULL, "out");
        Filter *head = build(specs[s], pool, &np, out);
        long nin = run_file("app.log", head);
        long nout = pool[np - 1].count;
        fclose(out);
        printf("== %s (in=%ld out=%ld)\n", specs[s], nin, nout);
        size_t n;
        unsigned char *b = slurp("out.txt", &n);
        int shown = 0;
        for (size_t i = 0; i < n && shown < 6;) {
            size_t e = i;
            while (e < n && b[e] != '\n')
                e++;
            printf("   %.*s\n", (int)(e - i), (const char *)b + i);
            i = e + 1;
            shown++;
        }
        free(b);
    }
    unlink("app.log");
    unlink("out.txt");
    return 0;
}
