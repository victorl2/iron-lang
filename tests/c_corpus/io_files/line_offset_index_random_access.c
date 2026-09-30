/*
 * title: Line offset index for random access
 * topic: io_files
 * covers: building a line index with ftell, index file in binary, fseek by line number, binary search on sorted lines, CRLF-free byte offsets
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 250 };

static unsigned rs = 112358u;
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

static void put_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    check(fwrite(b, 1, 4, f) == 4, "put_u32");
}

static uint32_t get_u32(FILE *f) {
    unsigned char b[4];
    check(fread(b, 1, 4, f) == 4, "get_u32");
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* read line number k (0-based) using the index file */
static int read_line_at(FILE *data, FILE *idx, long k, char *buf, size_t cap) {
    check(fseek(idx, k * 4, SEEK_SET) == 0, "idx seek");
    uint32_t off = get_u32(idx);
    check(fseek(data, (long)off, SEEK_SET) == 0, "data seek");
    if (!fgets(buf, (int)cap, data))
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    return 1;
}

int main(void) {
    /* sorted keys of the form "k0000123 payload" so binary search works */
    FILE *f = fopen("data.txt", "w");
    check(f != NULL, "open");
    unsigned key = 0;
    for (int i = 0; i < N; i++) {
        key += 1 + rnd() % 9;
        int plen = (int)(rnd() % 30);
        fprintf(f, "k%07u ", key);
        for (int k = 0; k < plen; k++)
            fputc('a' + (int)((key + (unsigned)k * 3u) % 26u), f);
        fputc('\n', f);
    }
    fclose(f);

    /* pass 1: build the index */
    f = fopen("data.txt", "r");
    FILE *idx = fopen("data.idx", "wb");
    check(f && idx, "index open");
    char line[128];
    int n = 0;
    for (;;) {
        long pos = ftell(f);
        if (!fgets(line, sizeof line, f))
            break;
        put_u32(idx, (uint32_t)pos);
        n++;
    }
    check(fclose(idx) == 0, "idx close");
    printf("indexed %d lines\n", n);
    check(n == N, "line count");

    idx = fopen("data.idx", "rb");
    check(idx != NULL, "idx reopen");
    fseek(idx, 0, SEEK_END);
    printf("index file is %ld bytes\n", ftell(idx));
    check(ftell(idx) == N * 4, "idx size");

    /* random access by line number vs sequential reading */
    static char seq[N][128];
    rewind(f);
    for (int i = 0; i < N; i++) {
        check(fgets(seq[i], sizeof seq[i], f) != NULL, "seq");
        seq[i][strcspn(seq[i], "\n")] = 0;
    }
    int checked = 0;
    for (int t = 0; t < 60; t++) {
        long k = (long)(rnd() % N);
        char got[128];
        check(read_line_at(f, idx, k, got, sizeof got), "random read");
        check(strcmp(got, seq[k]) == 0, "random line matches");
        checked++;
    }
    printf("random access verified for %d lines\n", checked);
    char got[128];
    read_line_at(f, idx, 0, got, sizeof got);
    printf("first: %s\n", got);
    read_line_at(f, idx, N - 1, got, sizeof got);
    printf("last: %s\n", got);
    read_line_at(f, idx, 100, got, sizeof got);
    printf("line 100: %s\n", got);

    /* binary search by key using only the index and the data file */
    int found = 0, missing = 0, probes_total = 0;
    for (unsigned want = 1; want < 400; want += 5) {
        long lo = 0, hi = N - 1;
        int hit = 0, probes = 0;
        while (lo <= hi) {
            long mid = (lo + hi) / 2;
            read_line_at(f, idx, mid, got, sizeof got);
            probes++;
            unsigned have = (unsigned)atoi(got + 1);
            if (have == want) {
                hit = 1;
                break;
            }
            if (have < want)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        probes_total += probes;
        /* cross-check with a linear scan */
        int lin = 0;
        for (int i = 0; i < N; i++)
            if ((unsigned)atoi(seq[i] + 1) == want)
                lin = 1;
        check(lin == hit, "binary search agrees with scan");
        hit ? found++ : missing++;
    }
    printf("binary search: %d found, %d missing, %d probes total\n", found, missing, probes_total);

    fclose(idx);
    fclose(f);
    remove("data.txt");
    remove("data.idx");
    return 0;
}
