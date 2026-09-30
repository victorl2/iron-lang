/*
 * title: Directory sync planner with content-based rename detection
 * topic: io_files
 * covers: tree walk, content hashing, minimal operation plan (mkdir, move, copy, update, delete, rmdir), plan ordering, idempotence
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

static inline uint32_t crc32_update(uint32_t crc, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
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
    char path[96];
    int is_dir;
    uint32_t hash;
    long size;
} Ent;

typedef struct {
    Ent e[128];
    int n;
} Tree;

static int cmp_ent(const void *a, const void *b) { return strcmp(((const Ent *)a)->path, ((const Ent *)b)->path); }

static void walk(const char *root, const char *rel, Tree *t) {
    char dir[192];
    snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dir);
    check(d != NULL, "opendir");
    struct dirent *de;
    char names[64][32];
    int nn = 0;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        check(nn < 64, "too many names");
        snprintf(names[nn++], 32, "%s", de->d_name);
    }
    closedir(d);
    for (int i = 0; i < nn; i++) {
        Ent *e = &t->e[t->n];
        snprintf(e->path, sizeof e->path, "%s%s%s", rel, rel[0] ? "/" : "", names[i]);
        char full[256];
        snprintf(full, sizeof full, "%s/%s", root, e->path);
        struct stat st;
        check(stat(full, &st) == 0, "stat");
        check(t->n < 127, "tree full");
        t->n++;
        if (S_ISDIR(st.st_mode)) {
            e->is_dir = 1;
            walk(root, e->path, t);
        } else {
            size_t len;
            unsigned char *b = slurp(full, &len);
            e->hash = crc32_update(0, b, len);
            e->size = (long)len;
            free(b);
        }
    }
}

static Tree *scan(const char *root) {
    Tree *t = calloc(1, sizeof *t);
    check(t != NULL, "tree alloc");
    walk(root, "", t);
    qsort(t->e, (size_t)t->n, sizeof t->e[0], cmp_ent);
    return t;
}

static const Ent *find(const Tree *t, const char *p) {
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->e[i].path, p))
            return &t->e[i];
    return NULL;
}

typedef struct {
    char kind[8];
    char a[96], b[96];
    long bytes;
} Op;

static void put_file(const char *root, const char *rel, const char *content) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    spit(p, content, strlen(content));
}
static void make_dir(const char *root, const char *rel) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    check(mkdir(p, 0755) == 0, "mkdir");
}

static int plan(const Tree *src, const Tree *dst, Op *ops) {
    int n = 0;
    int taken[128] = {0}; /* dst-only files consumed by a move */
    for (int i = 0; i < src->n; i++)
        if (src->e[i].is_dir && !find(dst, src->e[i].path)) {
            snprintf(ops[n].kind, 8, "MKDIR");
            snprintf(ops[n].a, 96, "%s", src->e[i].path);
            ops[n].b[0] = 0;
            ops[n++].bytes = 0;
        }
    for (int i = 0; i < src->n; i++) {
        const Ent *s = &src->e[i];
        if (s->is_dir)
            continue;
        const Ent *d = find(dst, s->path);
        if (d && !d->is_dir) {
            if (d->hash != s->hash || d->size != s->size) {
                snprintf(ops[n].kind, 8, "UPDATE");
                snprintf(ops[n].a, 96, "%s", s->path);
                ops[n].b[0] = 0;
                ops[n++].bytes = s->size;
            }
            continue;
        }
        int from = -1;
        for (int j = 0; j < dst->n; j++)
            if (!dst->e[j].is_dir && !taken[j] && !find(src, dst->e[j].path) && dst->e[j].hash == s->hash &&
                dst->e[j].size == s->size) {
                from = j;
                break;
            }
        if (from >= 0) {
            taken[from] = 1;
            snprintf(ops[n].kind, 8, "MOVE");
            snprintf(ops[n].a, 96, "%s", dst->e[from].path);
            snprintf(ops[n].b, 96, "%s", s->path);
            ops[n++].bytes = 0;
        } else {
            snprintf(ops[n].kind, 8, "COPY");
            snprintf(ops[n].a, 96, "%s", s->path);
            ops[n].b[0] = 0;
            ops[n++].bytes = s->size;
        }
    }
    for (int j = 0; j < dst->n; j++)
        if (!dst->e[j].is_dir && !taken[j] && !find(src, dst->e[j].path)) {
            snprintf(ops[n].kind, 8, "DELETE");
            snprintf(ops[n].a, 96, "%s", dst->e[j].path);
            ops[n].b[0] = 0;
            ops[n++].bytes = 0;
        }
    for (int j = dst->n - 1; j >= 0; j--) /* deepest first thanks to sorted paths */
        if (dst->e[j].is_dir && !find(src, dst->e[j].path)) {
            snprintf(ops[n].kind, 8, "RMDIR");
            snprintf(ops[n].a, 96, "%s", dst->e[j].path);
            ops[n].b[0] = 0;
            ops[n++].bytes = 0;
        }
    return n;
}

static void apply(const Op *op, const char *src, const char *dst) {
    char a[256], b[256];
    if (!strcmp(op->kind, "MKDIR")) {
        snprintf(a, sizeof a, "%s/%s", dst, op->a);
        check(mkdir(a, 0755) == 0, "apply mkdir");
    } else if (!strcmp(op->kind, "UPDATE") || !strcmp(op->kind, "COPY")) {
        snprintf(a, sizeof a, "%s/%s", src, op->a);
        snprintf(b, sizeof b, "%s/%s", dst, op->a);
        size_t n;
        unsigned char *buf = slurp(a, &n);
        spit(b, buf, n);
        free(buf);
    } else if (!strcmp(op->kind, "MOVE")) {
        snprintf(a, sizeof a, "%s/%s", dst, op->a);
        snprintf(b, sizeof b, "%s/%s", dst, op->b);
        check(rename(a, b) == 0, "apply move");
    } else if (!strcmp(op->kind, "DELETE")) {
        snprintf(a, sizeof a, "%s/%s", dst, op->a);
        check(unlink(a) == 0, "apply delete");
    } else {
        snprintf(a, sizeof a, "%s/%s", dst, op->a);
        check(rmdir(a) == 0, "apply rmdir");
    }
}

static void rm_tree(const char *root) {
    Tree *t = scan(root);
    char p[256];
    for (int i = t->n - 1; i >= 0; i--) {
        snprintf(p, sizeof p, "%s/%s", root, t->e[i].path);
        if (t->e[i].is_dir)
            rmdir(p);
        else
            unlink(p);
    }
    free(t);
    rmdir(root);
}

int main(void) {
    mkdir("A", 0755);
    mkdir("B", 0755);
    /* Destination B is the stale copy; A is the truth. */
    make_dir("A", "docs");
    make_dir("A", "docs/img");
    make_dir("A", "src");
    make_dir("A", "newdir");
    put_file("A", "readme.txt", "This project does things.\n");
    put_file("A", "docs/guide.md", "# Guide\nstep one\nstep two\n");
    put_file("A", "docs/img/logo.dat", "LOGO-BYTES-0123456789-LOGO-BYTES-0123456789");
    put_file("A", "src/main.c", "int main(void){return 0;}\n");
    put_file("A", "src/util.c", "/* util v2 */\nint util(void){return 2;}\n");
    put_file("A", "newdir/moved_report.txt", "quarterly numbers: 1 2 3 4\n");
    put_file("A", "newdir/fresh.txt", "brand new file\n");
    make_dir("B", "docs");
    make_dir("B", "old");
    make_dir("B", "old/deep");
    make_dir("B", "src");
    put_file("B", "readme.txt", "This project does things.\n");
    put_file("B", "docs/guide.md", "# Guide\nstep one\n");
    put_file("B", "docs/logo.dat", "LOGO-BYTES-0123456789-LOGO-BYTES-0123456789");
    put_file("B", "src/main.c", "int main(void){return 0;}\n");
    put_file("B", "src/util.c", "/* util v1 */\nint util(void){return 1;}\n");
    put_file("B", "old/report.txt", "quarterly numbers: 1 2 3 4\n");
    put_file("B", "old/deep/junk.tmp", "junk");
    put_file("B", "src/stale.o", "object");
    Tree *ts = scan("A"), *td = scan("B");
    static Op ops[128];
    int n = plan(ts, td, ops);
    long copy_bytes = 0;
    int counts[6] = {0};
    static const char *kinds[6] = {"MKDIR", "MOVE", "COPY", "UPDATE", "DELETE", "RMDIR"};
    for (int i = 0; i < n; i++) {
        printf("%-6s %s%s%s\n", ops[i].kind, ops[i].a, ops[i].b[0] ? " -> " : "", ops[i].b);
        copy_bytes += ops[i].bytes;
        for (int k = 0; k < 6; k++)
            if (!strcmp(ops[i].kind, kinds[k]))
                counts[k]++;
    }
    for (int i = 0; i < n; i++)
        apply(&ops[i], "A", "B");
    free(ts);
    free(td);
    ts = scan("A");
    td = scan("B");
    check(ts->n == td->n, "same entry count");
    for (int i = 0; i < ts->n; i++)
        check(!strcmp(ts->e[i].path, td->e[i].path) && ts->e[i].is_dir == td->e[i].is_dir &&
                  ts->e[i].hash == td->e[i].hash,
              "trees identical after sync");
    int again = plan(ts, td, ops);
    check(again == 0, "second plan is empty");
    printf("ops=%d (mkdir=%d move=%d copy=%d update=%d delete=%d rmdir=%d) bytes copied=%ld\n", n, counts[0],
           counts[1], counts[2], counts[3], counts[4], counts[5], copy_bytes);
    printf("entries after sync=%d, second plan=%d ops\n", td->n, again);
    free(ts);
    free(td);
    rm_tree("A");
    rm_tree("B");
    return 0;
}
