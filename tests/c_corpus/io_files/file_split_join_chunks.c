/*
 * title: Split a file into chunks and join them back
 * topic: io_files
 * covers: fixed-size splitting, numbered part names, join by append, last short chunk, checksum equality
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rs = 31337u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint32_t fnv_file(const char *name, long *size) {
    FILE *f = fopen(name, "rb");
    check(f != NULL, "fnv open");
    uint32_t h = 2166136261u;
    int c;
    long n = 0;
    while ((c = fgetc(f)) != EOF) {
        h ^= (uint32_t)c;
        h *= 16777619u;
        n++;
    }
    fclose(f);
    *size = n;
    return h;
}

/* split src into parts of at most chunk bytes; returns number of parts */
static int split(const char *src, size_t chunk) {
    FILE *in = fopen(src, "rb");
    check(in != NULL, "split open");
    unsigned char *buf = malloc(chunk);
    check(buf != NULL, "alloc");
    int parts = 0;
    size_t n;
    while ((n = fread(buf, 1, chunk, in)) > 0) {
        char name[32];
        snprintf(name, sizeof name, "part.%03d", parts);
        FILE *out = fopen(name, "wb");
        check(out != NULL, "part open");
        check(fwrite(buf, 1, n, out) == n, "part write");
        check(fclose(out) == 0, "part close");
        parts++;
    }
    free(buf);
    fclose(in);
    return parts;
}

static void join(const char *dst, int parts) {
    FILE *out = fopen(dst, "wb");
    check(out != NULL, "join open");
    for (int i = 0; i < parts; i++) {
        char name[32];
        snprintf(name, sizeof name, "part.%03d", i);
        FILE *in = fopen(name, "rb");
        check(in != NULL, "join part");
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            check(fwrite(buf, 1, n, out) == n, "join write");
        fclose(in);
    }
    check(fclose(out) == 0, "join close");
}

int main(void) {
    enum { SIZE = 10007 };
    FILE *f = fopen("whole.bin", "wb");
    check(f != NULL, "open whole");
    for (int i = 0; i < SIZE; i++)
        check(fputc((int)(rnd() & 0xFF), f) != EOF, "gen");
    fclose(f);
    long osize;
    uint32_t oh = fnv_file("whole.bin", &osize);
    printf("original: %ld bytes, fnv1a %08x\n", osize, (unsigned)oh);

    size_t chunks[] = {1000, 4096, 10007, 10008, 333, 5000};
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        size_t chunk = chunks[i];
        int parts = split("whole.bin", chunk);
        long lastsize = 0, tsize = 0;
        for (int p = 0; p < parts; p++) {
            char name[32];
            long sz;
            snprintf(name, sizeof name, "part.%03d", p);
            fnv_file(name, &sz);
            tsize += sz;
            if (p == parts - 1)
                lastsize = sz;
        }
        join("joined.bin", parts);
        long jsize;
        uint32_t jh = fnv_file("joined.bin", &jsize);
        printf("chunk %5zu: %2d parts, last part %5ld bytes, joined ok: %s\n", chunk, parts,
               lastsize, (jh == oh && jsize == osize) ? "yes" : "NO");
        check(jh == oh && jsize == osize && tsize == osize, "roundtrip");
        check(parts == (int)((SIZE + chunk - 1) / chunk), "part count");
        for (int p = 0; p < parts; p++) {
            char name[32];
            snprintf(name, sizeof name, "part.%03d", p);
            remove(name);
        }
    }

    /* joining in the wrong order must give a different checksum */
    int parts = split("whole.bin", 2500);
    for (int p = 0; p < parts / 2; p++) {
        char a[32], b[32], t[32];
        snprintf(a, sizeof a, "part.%03d", p);
        snprintf(b, sizeof b, "part.%03d", parts - 1 - p);
        snprintf(t, sizeof t, "part.tmp");
        check(rename(a, t) == 0 && rename(b, a) == 0 && rename(t, b) == 0, "swap");
    }
    join("joined.bin", parts);
    long jsize;
    uint32_t jh = fnv_file("joined.bin", &jsize);
    printf("reversed part order: size %ld, checksum differs: %s\n", jsize, jh != oh ? "yes" : "NO");
    check(jsize == osize && jh != oh, "wrong order detected");
    for (int p = 0; p < parts; p++) {
        char name[32];
        snprintf(name, sizeof name, "part.%03d", p);
        remove(name);
    }
    remove("whole.bin");
    remove("joined.bin");
    return 0;
}
