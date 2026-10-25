/*
 * title: Copy loops with different buffer sizes
 * topic: io_files
 * covers: fgetc/fputc copy, fread/fwrite copy, fgets/fputs copy, setvbuf sizes, identical results, block counts
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rs = 271828u;
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

static uint32_t sum_file(const char *name, long *size) {
    FILE *f = fopen(name, "rb");
    check(f != NULL, "sum open");
    uint32_t h = 0x811C9DC5u;
    int c;
    long n = 0;
    while ((c = fgetc(f)) != EOF) {
        h = (h ^ (uint32_t)c) * 0x01000193u;
        n++;
    }
    fclose(f);
    *size = n;
    return h;
}

static long copy_chars(const char *src, const char *dst, size_t vbuf) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    check(in && out, "open");
    char *b1 = malloc(vbuf), *b2 = malloc(vbuf);
    check(setvbuf(in, b1, _IOFBF, vbuf) == 0 && setvbuf(out, b2, _IOFBF, vbuf) == 0, "setvbuf");
    long n = 0;
    int c;
    while ((c = fgetc(in)) != EOF) {
        check(fputc(c, out) == c, "fputc");
        n++;
    }
    check(fclose(out) == 0, "close out");
    fclose(in);
    free(b1);
    free(b2);
    return n;
}

static long copy_blocks(const char *src, const char *dst, size_t blk, long *calls) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    check(in && out, "open");
    unsigned char *buf = malloc(blk);
    long n = 0;
    size_t got;
    *calls = 0;
    while ((got = fread(buf, 1, blk, in)) > 0) {
        check(fwrite(buf, 1, got, out) == got, "fwrite");
        n += (long)got;
        (*calls)++;
    }
    check(!ferror(in), "read error");
    check(fclose(out) == 0, "close out");
    fclose(in);
    free(buf);
    return n;
}

/* copies including embedded NULs, using fread(1)/fwrite for size-1 reads */
static long copy_items(const char *src, const char *dst, size_t item) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    check(in && out, "open");
    unsigned char *buf = malloc(item);
    long n = 0;
    size_t whole;
    while ((whole = fread(buf, item, 1, in)) == 1) {
        check(fwrite(buf, item, 1, out) == 1, "fwrite");
        n += (long)item;
    }
    /* trailing partial item is lost by item-wise fread(size=item, count=1): recover it */
    long done = n;
    fseek(in, 0, SEEK_END);
    long total = ftell(in);
    if (total > done) {
        fseek(in, done, SEEK_SET);
        size_t rest = (size_t)(total - done);
        check(fread(buf, 1, rest, in) == rest, "rest");
        check(fwrite(buf, 1, rest, out) == rest, "rest write");
        n += (long)rest;
    }
    check(fclose(out) == 0, "close out");
    fclose(in);
    free(buf);
    return n;
}

int main(void) {
    const size_t SIZE = 50021;
    FILE *f = fopen("src.bin", "wb");
    check(f != NULL, "open src");
    for (size_t i = 0; i < SIZE; i++) {
        unsigned r = rnd();
        /* mix of runs, text and binary, containing NUL and newline */
        int c = (i / 1000) % 3 == 0 ? (int)(r & 0xFF) : ((i / 1000) % 3 == 1 ? 'a' + (int)(r % 4) : '\n' * (int)(r & 1));
        check(fputc(c, f) == c, "gen");
    }
    fclose(f);
    long ssize;
    uint32_t ref = sum_file("src.bin", &ssize);
    printf("source: %ld bytes, hash %08x\n", ssize, (unsigned)ref);

    size_t vb[] = {2, 17, 512, 8192};
    for (size_t i = 0; i < sizeof vb / sizeof vb[0]; i++) {
        long n = copy_chars("src.bin", "dst.bin", vb[i]);
        long dsz;
        uint32_t h = sum_file("dst.bin", &dsz);
        printf("fgetc/fputc, stdio buffer %5zu: copied %ld, identical %s\n", vb[i], n,
               (h == ref && dsz == ssize && n == ssize) ? "yes" : "NO");
        check(h == ref && dsz == ssize && n == ssize, "char copy");
    }

    size_t blk[] = {1, 10, 1000, 4096, 65536};
    for (size_t i = 0; i < sizeof blk / sizeof blk[0]; i++) {
        long calls;
        long n = copy_blocks("src.bin", "dst.bin", blk[i], &calls);
        long dsz;
        uint32_t h = sum_file("dst.bin", &dsz);
        long expect_calls = (long)((SIZE + blk[i] - 1) / blk[i]);
        printf("fread/fwrite block %5zu: %5ld calls, identical %s\n", blk[i], calls,
               (h == ref && dsz == ssize) ? "yes" : "NO");
        check(h == ref && dsz == ssize && n == ssize && calls == expect_calls, "block copy");
    }

    size_t items[] = {3, 64, 1000};
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        long n = copy_items("src.bin", "dst.bin", items[i]);
        long dsz;
        uint32_t h = sum_file("dst.bin", &dsz);
        printf("item copy size %4zu (partial tail recovered): identical %s\n", items[i],
               (h == ref && dsz == ssize && n == ssize) ? "yes" : "NO");
        check(h == ref && dsz == ssize && n == ssize, "item copy");
    }

    /* line copy with fgets/fputs is NOT byte-safe for NUL bytes: show exactly why */
    FILE *in = fopen("src.bin", "rb");
    FILE *out = fopen("dst.bin", "wb");
    char line[256];
    while (fgets(line, sizeof line, in))
        fputs(line, out);
    fclose(in);
    fclose(out);
    long dsz;
    uint32_t h = sum_file("dst.bin", &dsz);
    printf("fgets/fputs copy of binary data: %s (%s)\n", h == ref ? "identical" : "differs",
           dsz < ssize ? "NUL bytes truncate lines" : "same length");
    check(h != ref && dsz < ssize, "fputs drops after NUL");

    remove("src.bin");
    remove("dst.bin");
    return 0;
}
