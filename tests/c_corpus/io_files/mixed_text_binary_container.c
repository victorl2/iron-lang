/*
 * title: Container file mixing text header and binary payload
 * topic: io_files
 * covers: text header parsing with fgets, binary sections, length-prefixed blobs, offsets via ftell, checksum trailer
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rs = 777u;
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

static uint32_t adler(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

int main(void) {
    enum { NBLOB = 4 };
    size_t sizes[NBLOB] = {10, 0, 300, 41};
    unsigned char *blob[NBLOB];
    for (int i = 0; i < NBLOB; i++) {
        blob[i] = malloc(sizes[i] + 1);
        check(blob[i] != NULL, "malloc");
        for (size_t k = 0; k < sizes[i]; k++)
            blob[i][k] = (unsigned char)rnd();
        for (size_t k = 5; k < sizes[i]; k += 37)
            blob[i][k] = '\n'; /* embedded newlines must not confuse the parser */
        for (size_t k = 9; k < sizes[i]; k += 53)
            blob[i][k] = 0; /* nor embedded NUL bytes */
    }

    FILE *f = fopen("box.dat", "wb");
    check(f != NULL, "open w");
    fprintf(f, "BOX1\ncount: %d\ncreator: reference\n\n", NBLOB);
    for (int i = 0; i < NBLOB; i++) {
        fprintf(f, "blob %d %zu\n", i, sizes[i]);
        if (sizes[i] > 0)
            check(fwrite(blob[i], 1, sizes[i], f) == sizes[i], "blob write");
        fputc('\n', f);
    }
    fputs("END\n", f);
    long end_pos = ftell(f);
    fclose(f);

    f = fopen("box.dat", "rb");
    check(f != NULL, "open r");
    char line[128];
    check(fgets(line, sizeof line, f) != NULL, "magic");
    check(strcmp(line, "BOX1\n") == 0, "magic value");
    int count = -1;
    char creator[32] = "";
    while (fgets(line, sizeof line, f) && line[0] != '\n') {
        if (sscanf(line, "count: %d", &count) == 1)
            continue;
        if (sscanf(line, "creator: %31s", creator) == 1)
            continue;
    }
    printf("header: count=%d creator=%s, body starts at %ld\n", count, creator, ftell(f));
    check(count == NBLOB, "count");

    long total = 0;
    for (int i = 0; i < count; i++) {
        int idx;
        size_t len;
        check(fgets(line, sizeof line, f) != NULL, "blob header");
        check(sscanf(line, "blob %d %zu", &idx, &len) == 2, "blob fields");
        long at = ftell(f);
        unsigned char *data = malloc(len + 1);
        check(data != NULL, "alloc data");
        check(fread(data, 1, len, f) == len, "blob read");
        check(fgetc(f) == '\n', "blob terminator");
        check(idx == i && len == sizes[i] && memcmp(data, blob[i], len) == 0, "blob content");
        int nls = 0, zeros = 0;
        for (size_t k = 0; k < len; k++) {
            nls += data[k] == '\n';
            zeros += data[k] == 0;
        }
        printf("blob %d: %3zu bytes at offset %4ld, adler32 %08x, newlines %d, zeros %d\n", idx,
               len, at, (unsigned)adler(data, len), nls, zeros);
        total += (long)len;
        free(data);
    }
    check(fgets(line, sizeof line, f) != NULL && strcmp(line, "END\n") == 0, "trailer");
    check(ftell(f) == end_pos && fgetc(f) == EOF, "exact end");
    printf("payload total %ld bytes, file length %ld\n", total, end_pos);
    fclose(f);

    for (int i = 0; i < NBLOB; i++)
        free(blob[i]);
    remove("box.dat");
    return 0;
}
