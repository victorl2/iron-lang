/*
 * title: Content-addressed object store with trees, commits, checkout, fsck and gc
 * topic: io_files
 * covers: hash-named object files in fanout directories, blobs/trees/commits, deduplication, history walk, checkout to a fresh directory, integrity check, mark-and-sweep gc
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

static uint64_t fnv64(const unsigned char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 32;
    h *= 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 29);
}

static long objects_written, objects_deduped;

/* Object = "<type> <len>\0<body>". Returns its 16 hex digit id in `id`. */
static void put_object(const char *type, const unsigned char *body, size_t n, char *id) {
    unsigned char *buf = malloc(n + 32);
    check(buf != NULL, "buf");
    int h = snprintf((char *)buf, 32, "%s %zu", type, n);
    memcpy(buf + h + 1, body, n);
    size_t total = (size_t)h + 1 + n;
    snprintf(id, 17, "%016llx", (unsigned long long)fnv64(buf, total));
    char dir[32], path[64];
    snprintf(dir, sizeof dir, "objects/%.2s", id);
    snprintf(path, sizeof path, "%s/%s", dir, id + 2);
    if (file_size(path) >= 0) {
        objects_deduped++;
    } else {
        mkdir(dir, 0755);
        spit(path, buf, total);
        objects_written++;
    }
    free(buf);
}

/* Returns the body (malloc'd) and type; verifies the id against the content. */
static unsigned char *get_object(const char *id, char *type, size_t *n) {
    char path[64];
    snprintf(path, sizeof path, "objects/%.2s/%s", id, id + 2);
    size_t total;
    unsigned char *buf = slurp(path, &total);
    char idcheck[17];
    snprintf(idcheck, sizeof idcheck, "%016llx", (unsigned long long)fnv64(buf, total));
    check(strcmp(idcheck, id) == 0, "object hash matches its name");
    unsigned char *nul = memchr(buf, 0, total);
    check(nul != NULL, "header");
    sscanf((char *)buf, "%15s %zu", type, n);
    unsigned char *body = malloc(*n + 1);
    memcpy(body, nul + 1, *n);
    body[*n] = 0;
    free(buf);
    return body;
}

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* Store directory `dir` as a tree object: lines "<b|t> <id> <name>\n". */
static void put_tree(const char *dir, char *id) {
    DIR *d = opendir(dir);
    check(d != NULL, "opendir");
    char names[32][32];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            snprintf(names[n++], 32, "%s", e->d_name);
    closedir(d);
    qsort(names, (size_t)n, 32, cmp_name);
    char body[2048];
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        char full[128], cid[17];
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        struct stat st;
        check(stat(full, &st) == 0, "stat");
        char kind;
        if (S_ISDIR(st.st_mode)) {
            put_tree(full, cid);
            kind = 't';
        } else {
            size_t fl;
            unsigned char *b = slurp(full, &fl);
            put_object("blob", b, fl, cid);
            free(b);
            kind = 'b';
        }
        len += (size_t)snprintf(body + len, sizeof body - len, "%c %s %s\n", kind, cid, names[i]);
    }
    put_object("tree", (unsigned char *)body, len, id);
}

static void checkout_tree(const char *id, const char *dest) {
    char type[16];
    size_t n;
    unsigned char *body = get_object(id, type, &n);
    check(!strcmp(type, "tree"), "tree object");
    mkdir(dest, 0755);
    for (char *line = (char *)body; *line;) {
        char *nl = strchr(line, '\n');
        *nl = 0;
        char kind, cid[17], name[32];
        check(sscanf(line, "%c %16s %31s", &kind, cid, name) == 3, "tree line");
        char path[128];
        snprintf(path, sizeof path, "%s/%s", dest, name);
        if (kind == 't')
            checkout_tree(cid, path);
        else {
            char bt[16];
            size_t bn;
            unsigned char *b = get_object(cid, bt, &bn);
            spit(path, b, bn);
            free(b);
        }
        line = nl + 1;
    }
    free(body);
}

static void commit(const char *tree, const char *parent, const char *msg, char *id) {
    char body[256];
    int l = snprintf(body, sizeof body, "tree %s\nparent %s\n\n%s\n", tree, parent[0] ? parent : "-", msg);
    put_object("commit", (unsigned char *)body, (size_t)l, id);
}

/* Mark reachable objects from a commit (recursively) into a set. */
static char reach[256][17];
static int nreach;
static void mark(const char *id) {
    for (int i = 0; i < nreach; i++)
        if (!strcmp(reach[i], id))
            return;
    snprintf(reach[nreach++], 17, "%s", id);
    char type[16];
    size_t n;
    unsigned char *b = get_object(id, type, &n);
    if (!strcmp(type, "tree")) {
        for (char *line = (char *)b; *line;) {
            char *nl = strchr(line, '\n');
            *nl = 0;
            char kind, cid[17], name[32];
            sscanf(line, "%c %16s %31s", &kind, cid, name);
            mark(cid);
            line = nl + 1;
        }
    } else if (!strcmp(type, "commit")) {
        char t[17], p[17];
        sscanf((char *)b, "tree %16s\nparent %16s", t, p);
        mark(t);
        if (strcmp(p, "-"))
            mark(p);
    }
    free(b);
}

static int list_objects(char ids[][17], int cap) {
    DIR *d = opendir("objects");
    check(d != NULL, "opendir objects");
    struct dirent *e;
    int n = 0;
    char fan[64][4];
    int nf = 0;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            snprintf(fan[nf++], 4, "%s", e->d_name);
    closedir(d);
    for (int i = 0; i < nf; i++) {
        char dir[32];
        snprintf(dir, sizeof dir, "objects/%s", fan[i]);
        DIR *s = opendir(dir);
        while ((e = readdir(s)) != NULL)
            if (e->d_name[0] != '.') {
                check(n < cap, "objects");
                snprintf(ids[n++], 17, "%s%s", fan[i], e->d_name);
            }
        closedir(s);
    }
    qsort(ids, (size_t)n, 17, cmp_name);
    return n;
}

static void rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        char names[64][32];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (e->d_name[0] != '.')
                snprintf(names[n++], 32, "%s", e->d_name);
        closedir(d);
        for (int i = 0; i < n; i++) {
            char full[128];
            snprintf(full, sizeof full, "%s/%s", path, names[i]);
            rm_tree(full);
        }
        rmdir(path);
    } else
        unlink(path);
}

static void write_file(const char *path, const char *text) { spit(path, text, strlen(text)); }

int main(void) {
    mkdir("objects", 0755);
    mkdir("work", 0755);
    mkdir("work/src", 0755);
    mkdir("work/docs", 0755);
    char treeid[3][17], cid[3][17];
    static const char *msgs[3] = {"initial import", "edit main, add notes", "drop docs, rename"};
    /* Revision 1 */
    write_file("work/README", "Object store demo\n");
    write_file("work/src/main.c", "int main(void){return 0;}\n");
    write_file("work/src/util.c", "int util(void){return 1;}\n");
    write_file("work/docs/copy.txt", "Object store demo\n"); /* identical to README: dedups */
    put_tree("work", treeid[0]);
    commit(treeid[0], "", msgs[0], cid[0]);
    printf("commit 1 %s objects=%ld dedup hits=%ld\n", cid[0], objects_written, objects_deduped);
    /* Revision 2 */
    write_file("work/src/main.c", "int main(void){puts(\"hi\");return 0;}\n");
    write_file("work/docs/notes.txt", "notes v1\n");
    put_tree("work", treeid[1]);
    commit(treeid[1], cid[0], msgs[1], cid[1]);
    printf("commit 2 %s objects=%ld dedup hits=%ld\n", cid[1], objects_written, objects_deduped);
    /* Revision 3 */
    rm_tree("work/docs");
    rename("work/README", "work/README.md");
    put_tree("work", treeid[2]);
    commit(treeid[2], cid[1], msgs[2], cid[2]);
    printf("commit 3 %s objects=%ld dedup hits=%ld\n", cid[2], objects_written, objects_deduped);

    /* History walk from the newest commit. */
    char cur[17];
    snprintf(cur, sizeof cur, "%s", cid[2]);
    int depth = 0;
    while (strcmp(cur, "-")) {
        char type[16];
        size_t n;
        unsigned char *b = get_object(cur, type, &n);
        char t[17], p[17];
        sscanf((char *)b, "tree %16s\nparent %16s", t, p);
        const char *msg = strstr((char *)b, "\n\n") + 2;
        printf("log: %.8s tree %.8s parent %.8s %s", cur, t, p, msg);
        free(b);
        snprintf(cur, sizeof cur, "%s", p);
        depth++;
    }
    check(depth == 3, "history depth");

    /* Checkout of commit 1 must reproduce the original working tree byte for byte. */
    char type[16];
    size_t n;
    unsigned char *cb = get_object(cid[0], type, &n);
    char t0[17];
    sscanf((char *)cb, "tree %16s", t0);
    free(cb);
    checkout_tree(t0, "restored");
    char reid[17];
    put_tree("restored", reid);
    check(strcmp(reid, treeid[0]) == 0, "checkout reproduces the tree id");
    printf("checkout of commit 1 reproduces tree %.8s\n", reid);
    rm_tree("restored");

    /* fsck + gc: forget commits 2 and 3 (reset to commit 1) and sweep unreachable objects. */
    static char ids[256][17];
    int total = list_objects(ids, 256);
    nreach = 0;
    mark(cid[0]);
    int removed = 0;
    for (int i = 0; i < total; i++) {
        int keep = 0;
        for (int k = 0; k < nreach; k++)
            if (!strcmp(ids[i], reach[k]))
                keep = 1;
        if (!keep) {
            char path[64];
            snprintf(path, sizeof path, "objects/%.2s/%s", ids[i], ids[i] + 2);
            check(unlink(path) == 0, "gc unlink");
            removed++;
        }
    }
    int left = list_objects(ids, 256);
    check(left == nreach, "only reachable objects remain");
    for (int i = 0; i < left; i++) {
        char t[16];
        size_t bn;
        free(get_object(ids[i], t, &bn)); /* fsck: hash verified inside */
    }
    printf("objects before gc=%d reachable from commit 1=%d removed=%d fsck ok\n", total, nreach, removed);
    /* corrupt one object: fsck must notice via the hash check */
    char path[64];
    snprintf(path, sizeof path, "objects/%.2s/%s", ids[0], ids[0] + 2);
    size_t fn;
    unsigned char *fb = slurp(path, &fn);
    fb[fn - 1] ^= 1;
    spit(path, fb, fn);
    free(fb);
    char idcheck[17];
    fb = slurp(path, &fn);
    snprintf(idcheck, sizeof idcheck, "%016llx", (unsigned long long)fnv64(fb, fn));
    free(fb);
    check(strcmp(idcheck, ids[0]) != 0, "corruption detected");
    printf("corrupted object detected by hash mismatch\n");
    for (int i = 0; i < left; i++) {
        snprintf(path, sizeof path, "objects/%.2s/%s", ids[i], ids[i] + 2);
        unlink(path);
        snprintf(path, sizeof path, "objects/%.2s", ids[i]);
        rmdir(path);
    }
    rmdir("objects");
    rm_tree("work");
    return 0;
}
