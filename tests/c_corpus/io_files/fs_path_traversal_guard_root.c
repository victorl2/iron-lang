/*
 * title: Path traversal guard confining user paths to a root directory
 * topic: io_files
 * covers: dotdot clamping vs strict rejection, symlink expansion with re-rooting of absolute targets, loop limit, canonical prefix verification with realpath, naive join leaking outside data
 * deps: libc, posix
 */
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);       \
            exit(1);                                                   \
        }                                                              \
    } while (0)

static inline int put_bytes(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            close(fd);
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return close(fd);
}

static inline void put(const char *path, const char *data) {
    CHECK(put_bytes(path, data, strlen(data)) == 0);
}

/* whole file into a malloc'd buffer (NUL terminated); NULL on error */
static inline unsigned char *slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap + 1);
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) {
            free(b);
            close(fd);
            return NULL;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    if (len) *len = n;
    return b;
}

static inline void mkd(const char *path) { CHECK(mkdir(path, 0777) == 0); }

/* rm -rf through directory fds: never follows symlinks, never uses a path twice */
static inline int rm_at(int pfd, const char *name, long *count) {
    struct stat st;
    if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        if ((st.st_mode & 0700) != 0700 && fchmodat(pfd, name, 0700, 0) < 0) return -1;
        int fd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0) return -1;
        DIR *d = fdopendir(fd);
        if (!d) {
            close(fd);
            return -1;
        }
        char **names = NULL;
        size_t n = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                names = realloc(names, cap * sizeof *names);
            }
            names[n++] = strdup(e->d_name);
        }
        int rc = 0;
        for (size_t i = 0; i < n; i++) {
            if (rm_at(fd, names[i], count) < 0) rc = -1;
            free(names[i]);
        }
        free(names);
        closedir(d);
        if (rc < 0) return -1;
        if (unlinkat(pfd, name, AT_REMOVEDIR) < 0) return -1;
        if (count) (*count)++;
        return 0;
    }
    if (unlinkat(pfd, name, 0) < 0) return -1;
    if (count) (*count)++;
    return 0;
}

static inline int rm_rf(const char *path) { return rm_at(AT_FDCWD, path, NULL); }


typedef enum { G_OK, G_ESCAPE, G_NOENT, G_NOTDIR, G_LOOP } Verdict;

static const char *vname(Verdict v) {
    switch (v) {
    case G_OK: return "ok";
    case G_ESCAPE: return "ESCAPE";
    case G_NOENT: return "ENOENT";
    case G_NOTDIR: return "ENOTDIR";
    default: return "LOOP";
    }
}

#define MAXN 32

/* Resolve `user` inside directory `root`. strict: any attempt to leave the root (leading "/",
 * ".." above the root, absolute link target) is rejected. Otherwise it is clamped like a chroot.
 * On success `out` holds the path relative to root ("." for the root itself). */
static Verdict resolve(const char *root, const char *user, int strict, char *out, size_t cap) {
    char comps[MAXN][64];
    int depth = 0;
    char todo[512];
    if (user[0] == '/') {
        if (strict) return G_ESCAPE;
    }
    snprintf(todo, sizeof todo, "%s", user);
    const char *p = todo;
    int links = 0;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[64];
        size_t l = strcspn(p, "/");
        CHECK(l < sizeof comp);
        memcpy(comp, p, l);
        comp[l] = 0;
        p += l;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            if (depth == 0) {
                if (strict) return G_ESCAPE;
            } else depth--;
            continue;
        }
        CHECK(depth < MAXN);
        snprintf(comps[depth++], 64, "%s", comp);
        char full[512];
        size_t o = (size_t)snprintf(full, sizeof full, "%s", root);
        for (int i = 0; i < depth; i++) o += (size_t)snprintf(full + o, sizeof full - o, "/%s", comps[i]);
        struct stat st;
        if (lstat(full, &st) < 0) {
            /* a missing final component is reported as ENOENT too; callers decide what to do */
            return G_NOENT;
        }
        if (S_ISLNK(st.st_mode)) {
            if (++links > 16) return G_LOOP;
            char tgt[256];
            ssize_t n = readlink(full, tgt, sizeof tgt - 1);
            CHECK(n > 0);
            tgt[n] = 0;
            depth--; /* replace the link by its target text */
            if (tgt[0] == '/') {
                if (strict) return G_ESCAPE;
                depth = 0; /* absolute targets are re-rooted */
            }
            char rest[512];
            snprintf(rest, sizeof rest, "%s/%s", tgt, p);
            snprintf(todo, sizeof todo, "%s", rest);
            p = todo;
        } else if (!S_ISDIR(st.st_mode)) {
            const char *q = p;
            while (*q == '/') q++;
            if (*q) return G_NOTDIR;
        }
    }
    size_t o = 0;
    if (depth == 0) out[o++] = '.';
    for (int i = 0; i < depth; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? "/" : "", comps[i]);
    out[o] = 0;
    return G_OK;
}

static const char *naive_read(const char *root, const char *user) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", root, user);
    size_t n;
    unsigned char *b = slurp(path, &n);
    if (!b) return "fails";
    const char *r = strncmp((char *)b, "TOP SECRET", 10) == 0 ? "LEAKS SECRET" : "reads jail data";
    free(b);
    return r;
}

int main(void) {
    umask(022);
    char base[PATH_MAX];
    CHECK(realpath(".", base) != NULL);
    mkd("jail");
    mkd("jail/public");
    put("jail/public/hello.txt", "hello");
    mkd("jail/outside");
    put("jail/outside/secret.txt", "decoy inside the jail\n");
    mkd("outside");
    put("outside/secret.txt", "TOP SECRET plans\n");
    CHECK(symlink("public", "jail/pub_link") == 0);
    CHECK(symlink("../outside", "jail/up_link") == 0);
    char abs_target[PATH_MAX + 32];
    snprintf(abs_target, sizeof abs_target, "%s/outside", base);
    CHECK(symlink(abs_target, "jail/abs_link") == 0);
    CHECK(symlink("/public", "jail/root_abs") == 0);
    CHECK(symlink("loop_b", "jail/loop_a") == 0);
    CHECK(symlink("loop_a", "jail/loop_b") == 0);

    static const char *attempts[] = {
        "public/hello.txt", "public/../public/hello.txt", "../outside/secret.txt", "public/../../outside/secret.txt",
        "/outside/secret.txt", "/public/hello.txt", "up_link/secret.txt", "abs_link/secret.txt",
        "root_abs/hello.txt", "pub_link/hello.txt", "loop_a", "public/hello.txt/x", "public/missing.txt",
        "outside/secret.txt", "pub_link/../../outside/secret.txt", ".", "..",
    };
    char jail_real[PATH_MAX];
    CHECK(realpath("jail", jail_real) != NULL);
    size_t jl = strlen(jail_real);
    int leaks = 0, guarded_ok = 0;
    for (size_t i = 0; i < sizeof attempts / sizeof attempts[0]; i++) {
        char s_out[512] = "", c_out[512] = "";
        Verdict vs = resolve("jail", attempts[i], 1, s_out, sizeof s_out);
        Verdict vc = resolve("jail", attempts[i], 0, c_out, sizeof c_out);
        const char *nv = naive_read("jail", attempts[i]);
        if (!strcmp(nv, "LEAKS SECRET")) leaks++;
        printf("%-30s naive: %-15s strict: %-7s", attempts[i], nv, vname(vs));
        if (vs == G_OK) printf("(%s)", s_out);
        printf(" chroot: %s", vname(vc));
        if (vc == G_OK) printf("(%s)", c_out);
        printf("\n");
        /* defence in depth: whatever the guard accepts must canonicalize below the jail */
        Verdict vs_all[2] = {vs, vc};
        const char *outs[2] = {s_out, c_out};
        for (int k = 0; k < 2; k++) {
            if (vs_all[k] != G_OK) continue;
            char full[1200], canon[PATH_MAX];
            snprintf(full, sizeof full, "jail/%s", outs[k]);
            if (realpath(full, canon) == NULL) continue; /* target may not exist, e.g. the jail root link */
            CHECK(!strcmp(canon, jail_real) || (strncmp(canon, jail_real, jl) == 0 && canon[jl] == '/'));
            guarded_ok++;
        }
    }
    printf("naive joins that leaked the secret: %d\n", leaks);
    CHECK(leaks >= 3);
    printf("guard results verified inside the jail by realpath: %d\n", guarded_ok);
    CHECK(rm_rf("jail") == 0 && rm_rf("outside") == 0);
    return 0;
}
