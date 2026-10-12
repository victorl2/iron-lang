/*
 * title: Journaling directory operations with idempotent redo
 * topic: io_files
 * covers: metadata journal, transaction commit lines, torn journal, partial application, idempotent replay of mkdir/create/rename/unlink/rmdir
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

#include <dirent.h>

typedef struct {
    char op;       /* M mkdir, C create, R rename, U unlink, D rmdir */
    char a[40], b[40], data[24];
} Op;

typedef struct {
    Op ops[8];
    int n;
} Txn;

#define NTXN 6
static Txn txns[NTXN];

static void add(Txn *t, char op, const char *a, const char *b, const char *data) {
    Op *o = &t->ops[t->n++];
    o->op = op;
    snprintf(o->a, sizeof o->a, "%s", a);
    snprintf(o->b, sizeof o->b, "%s", b);
    snprintf(o->data, sizeof o->data, "%s", data);
}

static void build_txns(void) {
    add(&txns[0], 'M', "root/a", "", "");
    add(&txns[0], 'M', "root/a/b", "", "");
    add(&txns[0], 'C', "root/a/b/f1", "", "one");
    add(&txns[1], 'C', "root/a/f2", "", "two");
    add(&txns[1], 'R', "root/a/b/f1", "root/a/f1", "");
    add(&txns[1], 'M', "root/c", "", "");
    add(&txns[2], 'C', "root/c/f3", "", "three");
    add(&txns[2], 'C', "root/a/f2", "", "two-v2"); /* overwrite */
    add(&txns[3], 'U', "root/a/f1", "", "");
    add(&txns[3], 'D', "root/a/b", "", "");
    add(&txns[3], 'R', "root/c", "root/d", "");
    add(&txns[4], 'M', "root/e", "", "");
    add(&txns[4], 'R', "root/a/f2", "root/e/f2", "");
    add(&txns[4], 'C', "root/e/f4", "", "four");
    add(&txns[5], 'U', "root/d/f3", "", "");
    add(&txns[5], 'D', "root/d", "", "");
    add(&txns[5], 'C', "root/e/f5", "", "five");
}

/* Idempotent application: an operation whose effect is already present succeeds. */
static void apply(const Op *o) {
    switch (o->op) {
    case 'M':
        if (mkdir(o->a, 0755) != 0)
            check(errno == EEXIST, "mkdir");
        break;
    case 'C':
        spit(o->a, o->data, strlen(o->data));
        break;
    case 'R':
        if (rename(o->a, o->b) != 0)
            check(errno == ENOENT, "rename already done");
        break;
    case 'U':
        if (unlink(o->a) != 0)
            check(errno == ENOENT, "unlink");
        break;
    default:
        if (rmdir(o->a) != 0)
            check(errno == ENOENT, "rmdir");
    }
}

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void dump(const char *path, char *out, size_t *len, size_t cap) {
    DIR *d = opendir(path);
    check(d != NULL, "opendir");
    char names[32][32];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(names[n++], 32, "%s", e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, 32, cmp_name);
    for (int i = 0; i < n; i++) {
        char full[128];
        snprintf(full, sizeof full, "%s/%s", path, names[i]);
        struct stat st;
        check(stat(full, &st) == 0, "stat");
        int w;
        if (S_ISDIR(st.st_mode)) {
            w = snprintf(out + *len, cap - *len, "d %s\n", full);
            *len += (size_t)w;
            dump(full, out, len, cap);
        } else {
            size_t l;
            unsigned char *b = slurp(full, &l);
            w = snprintf(out + *len, cap - *len, "f %s %.*s\n", full, (int)l, (const char *)b);
            *len += (size_t)w;
            free(b);
        }
    }
}

static void rm_all(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        char names[32][32];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                snprintf(names[n++], 32, "%s", e->d_name);
        closedir(d);
        for (int i = 0; i < n; i++) {
            char full[128];
            snprintf(full, sizeof full, "%s/%s", path, names[i]);
            rm_all(full);
        }
        rmdir(path);
    } else
        unlink(path);
}

/* Journal text: "T<i>\n" op lines "<op> a b data\n" then "COMMIT <i>\n". Returns committed txn count. */
static int parse_journal(const char *j, size_t n, int *op_counts) {
    int committed = 0;
    size_t pos = 0;
    int pending = 0;
    while (pos < n) {
        const char *nl = memchr(j + pos, '\n', n - pos);
        if (!nl)
            break; /* torn line */
        size_t l = (size_t)(nl - (j + pos));
        if (l >= 6 && !memcmp(j + pos, "COMMIT", 6)) {
            op_counts[committed++] = pending;
            pending = 0;
        } else if (l > 0 && j[pos] != 'T')
            pending++;
        pos += l + 1;
    }
    return committed;
}

int main(void) {
    build_txns();
    char journal[4096];
    size_t jl = 0;
    for (int t = 0; t < NTXN; t++) {
        jl += (size_t)snprintf(journal + jl, sizeof journal - jl, "T%d\n", t);
        for (int i = 0; i < txns[t].n; i++) {
            const Op *o = &txns[t].ops[i];
            jl += (size_t)snprintf(journal + jl, sizeof journal - jl, "%c %s %s %s\n", o->op, o->a, o->b[0] ? o->b : "-",
                                   o->data[0] ? o->data : "-");
        }
        jl += (size_t)snprintf(journal + jl, sizeof journal - jl, "COMMIT %d\n", t);
    }
    printf("journal bytes=%zu transactions=%d\n", jl, NTXN);

    /* Model: full replay of the first k transactions on a clean tree. */
    static char model[NTXN + 1][2048];
    for (int k = 0; k <= NTXN; k++) {
        rm_all("root");
        check(mkdir("root", 0755) == 0, "mkdir root");
        for (int t = 0; t < k; t++)
            for (int i = 0; i < txns[t].n; i++)
                apply(&txns[t].ops[i]);
        size_t l = 0;
        model[k][0] = 0;
        dump("root", model[k], &l, sizeof model[k]);
    }
    int by_committed[NTXN + 1] = {0};
    int scenarios = 0;
    for (size_t cut = 0; cut <= jl; cut++) {
        int counts[NTXN + 1] = {0};
        int nc = parse_journal(journal, cut, counts);
        int total_ops = 0;
        for (int t = 0; t < nc; t++)
            total_ops += txns[t].n;
        int apply_prefix[3] = {0, total_ops / 2, total_ops};
        for (int v = 0; v < 4; v++) {
            if (v > 0 && (cut % 5) != 0)
                continue; /* keep runtime modest: sample the partial-application variants */
            rm_all("root");
            check(mkdir("root", 0755) == 0, "mkdir root");
            /* the file system got `pre` operations before the crash; the progress marker may lag by one */
            int pre = apply_prefix[v == 3 ? 1 : v];
            int left = pre;
            for (int t = 0; t < nc && left > 0; t++)
                for (int i = 0; i < txns[t].n && left > 0; i++, left--)
                    apply(&txns[t].ops[i]);
            int mark = (v == 3 && pre > 0) ? pre - 1 : pre;
            char mk[16];
            int ml = snprintf(mk, sizeof mk, "%d", mark);
            spit("progress.mark", mk, (size_t)ml);
            /* restart: resume the journal from the progress marker */
            size_t mlen;
            unsigned char *mb = slurp("progress.mark", &mlen);
            char rb[16];
            check(mlen < sizeof rb, "marker size");
            memcpy(rb, mb, mlen);
            rb[mlen] = 0;
            int resume = atoi(rb);
            free(mb);
            int g = 0;
            for (int t = 0; t < nc; t++)
                for (int i = 0; i < txns[t].n; i++, g++)
                    if (g >= resume)
                        apply(&txns[t].ops[i]);
            char now[2048];
            size_t l = 0;
            now[0] = 0;
            dump("root", now, &l, sizeof now);
            check(strcmp(now, model[nc]) == 0, "replayed tree equals model of committed txns");
            scenarios++;
        }
        by_committed[nc]++;
    }
    printf("crash scenarios verified=%d\n", scenarios);
    for (int k = 0; k <= NTXN; k++)
        printf("  journal cuts with %d committed txns: %d\n", k, by_committed[k]);
    printf("final tree:\n%s", model[NTXN]);
    rm_all("root");
    unlink("progress.mark");
    return 0;
}
