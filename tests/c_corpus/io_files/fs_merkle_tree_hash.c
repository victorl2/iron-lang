/*
 * title: Merkle hash over a directory tree (SHA-256)
 * topic: io_files
 * covers: SHA-256 with known answer, canonical tree encoding, sorted children, file/dir/symlink node types, independence from creation order, sensitivity to name, content, type and mode changes
 * deps: libc, posix
 */
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

static inline int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sorted names in a directory, without "." and ".."; NULL on error */
static inline char **ls_dir(const char *path, int *count) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    char **v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            v = realloc(v, (size_t)cap * sizeof *v);
        }
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n > 1) qsort(v, (size_t)n, sizeof *v, cmp_str);
    if (!v) v = malloc(sizeof *v);
    *count = n;
    return v;
}

static inline void ls_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static inline char *join2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb + 1);
    return r;
}


typedef struct {
    uint32_t h[8];
    unsigned char buf[64];
    uint64_t len;
    size_t fill;
} Sha;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(Sha *s, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6],
             h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->fill = 0;
}

static void sha_update(Sha *s, const void *data, size_t n) {
    const unsigned char *p = data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha_final(Sha *s, unsigned char out[32]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha_update(s, &pad, 1);
    unsigned char z = 0;
    while (s->fill != 56) sha_update(s, &z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_update(s, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

static void put_u32(Sha *s, uint32_t v) {
    unsigned char b[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
    sha_update(s, b, 4);
}

static void hex(const unsigned char *h, char *out, int nbytes) {
    for (int i = 0; i < nbytes; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

/* node encoding: "F" len content | "L" len target | "D" count { namelen name hash[32] }* ; optional mode */
static void tree_hash(const char *path, int with_mode, unsigned char out[32]) {
    struct stat st;
    CHECK(lstat(path, &st) == 0);
    Sha s;
    sha_init(&s);
    if (S_ISREG(st.st_mode)) {
        size_t n;
        unsigned char *b = slurp(path, &n);
        CHECK(b);
        sha_update(&s, "F", 1);
        if (with_mode) put_u32(&s, (uint32_t)(st.st_mode & 0777));
        put_u32(&s, (uint32_t)n);
        sha_update(&s, b, n);
        free(b);
    } else if (S_ISLNK(st.st_mode)) {
        char t[128];
        ssize_t n = readlink(path, t, sizeof t);
        CHECK(n >= 0);
        sha_update(&s, "L", 1);
        put_u32(&s, (uint32_t)n);
        sha_update(&s, t, (size_t)n);
    } else {
        int n;
        char **v = ls_dir(path, &n);
        CHECK(v);
        sha_update(&s, "D", 1);
        put_u32(&s, (uint32_t)n);
        for (int i = 0; i < n; i++) {
            char *p = join2(path, v[i]);
            unsigned char ch[32];
            tree_hash(p, with_mode, ch);
            put_u32(&s, (uint32_t)strlen(v[i]));
            sha_update(&s, v[i], strlen(v[i]));
            sha_update(&s, ch, 32);
            free(p);
        }
        ls_free(v, n);
    }
    sha_final(&s, out);
}

static void build(const char *root, int reverse) {
    static const char *dirs[] = {"", "/lib", "/lib/x", "/bin"};
    static const char *files[][2] = {
        {"/bin/tool", "run"}, {"/lib/a.txt", "alpha"}, {"/lib/b.txt", "alpha"},
        {"/lib/x/c.txt", "gamma"}, {"/top", "top"},
    };
    for (int i = 0; i < 4; i++) {
        char p[64];
        snprintf(p, sizeof p, "%s%s", root, dirs[i]);
        mkd(p);
    }
    for (int i = 0; i < 5; i++) {
        int k = reverse ? 4 - i : i;
        char p[64];
        snprintf(p, sizeof p, "%s%s", root, files[k][0]);
        put(p, files[k][1]);
    }
    char p[64];
    snprintf(p, sizeof p, "%s/lib/link", root);
    CHECK(symlink("a.txt", p) == 0);
}

int main(void) {
    umask(022);
    /* known answers: FIPS 180-4 examples */
    Sha s;
    unsigned char d[32];
    char hx[65];
    sha_init(&s);
    sha_update(&s, "abc", 3);
    sha_final(&s, d);
    hex(d, hx, 32);
    printf("sha256(abc) = %s\n", hx);
    CHECK(strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    sha_init(&s);
    sha_update(&s, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    sha_final(&s, d);
    hex(d, hx, 32);
    printf("sha256(448-bit) = %s\n", hx);
    CHECK(strcmp(hx, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    build("A", 0);
    build("B", 1); /* same tree, files created in the opposite order */
    unsigned char ha[32], hb[32];
    tree_hash("A", 0, ha);
    tree_hash("B", 0, hb);
    hex(ha, hx, 8);
    printf("A root = %s...\n", hx);
    CHECK(memcmp(ha, hb, 32) == 0);
    printf("B built in reverse order: same root\n");

    unsigned char cur[32], prev[32];
    memcpy(prev, hb, 32);

    /* every kind of edit changes the root; subtree hashes only change along the edited path */
    unsigned char lib_before[32], bin_before[32], lib_after[32], bin_after[32];
    tree_hash("B/lib", 0, lib_before);
    tree_hash("B/bin", 0, bin_before);
    put("B/lib/x/c.txt", "gammA");
    tree_hash("B", 0, cur);
    tree_hash("B/lib", 0, lib_after);
    tree_hash("B/bin", 0, bin_after);
    CHECK(memcmp(cur, prev, 32) != 0 && memcmp(lib_before, lib_after, 32) != 0 && memcmp(bin_before, bin_after, 32) == 0);
    hex(cur, hx, 8);
    printf("content edit deep in lib/x  -> %s... (bin subtree unchanged: yes)\n", hx);
    put("B/lib/x/c.txt", "gamma");
    tree_hash("B", 0, cur);
    CHECK(memcmp(cur, ha, 32) == 0);
    printf("edit reverted               -> root restored\n");

    CHECK(rename("B/lib/b.txt", "B/lib/bb.txt") == 0);
    tree_hash("B", 0, cur);
    CHECK(memcmp(cur, ha, 32) != 0);
    hex(cur, hx, 8);
    printf("rename b.txt -> bb.txt      -> %s...\n", hx);
    CHECK(rename("B/lib/bb.txt", "B/lib/b.txt") == 0);

    CHECK(unlink("B/lib/link") == 0 && symlink("b.txt", "B/lib/link") == 0);
    tree_hash("B", 0, cur);
    CHECK(memcmp(cur, ha, 32) != 0);
    hex(cur, hx, 8);
    printf("retarget symlink            -> %s...\n", hx);
    CHECK(unlink("B/lib/link") == 0 && symlink("a.txt", "B/lib/link") == 0);

    CHECK(unlink("B/top") == 0);
    mkd("B/top");
    tree_hash("B", 0, cur);
    hex(cur, hx, 8);
    printf("file replaced by empty dir  -> %s...\n", hx);
    CHECK(memcmp(cur, ha, 32) != 0);
    CHECK(rmdir("B/top") == 0);
    put("B/top", "top");

    /* mode is ignored by the plain hash and captured by the mode-aware one */
    unsigned char ma[32], mb[32];
    tree_hash("A", 1, ma);
    CHECK(chmod("B/bin/tool", 0755) == 0);
    tree_hash("B", 0, cur);
    tree_hash("B", 1, mb);
    CHECK(memcmp(cur, ha, 32) == 0 && memcmp(ma, mb, 32) != 0);
    hex(ma, hx, 8);
    printf("mode-aware root A = %s...\n", hx);
    hex(mb, hx, 8);
    printf("after chmod 755 on B/bin/tool: plain equal, mode-aware = %s...\n", hx);

    tree_hash("A/lib", 0, cur);
    hex(cur, hx, 8);
    printf("A/lib subtree = %s...\n", hx);
    CHECK(rm_rf("A") == 0 && rm_rf("B") == 0);
    return 0;
}
