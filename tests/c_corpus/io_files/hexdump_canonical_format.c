/*
 * title: Canonical hex dump of a file
 * topic: io_files
 * covers: hexdump -C layout, 16-byte rows, partial last row, repeated row squeezing, printable column, fread blocks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Dump to `out`; identical consecutive rows are squeezed into '*'. Returns rows printed. */
static int hexdump(FILE *in, FILE *out) {
    unsigned char row[16], prev[16];
    long off = 0;
    int have_prev = 0, squeezed = 0, rows = 0;
    size_t n;
    while ((n = fread(row, 1, 16, in)) > 0) {
        if (n == 16 && have_prev && memcmp(row, prev, 16) == 0) {
            if (!squeezed) {
                fputs("*\n", out);
                squeezed = 1;
            }
            off += 16;
            continue;
        }
        squeezed = 0;
        fprintf(out, "%08lx ", off);
        for (int i = 0; i < 16; i++) {
            if (i == 8)
                fputc(' ', out);
            if ((size_t)i < n)
                fprintf(out, " %02x", row[i]);
            else
                fputs("   ", out);
        }
        fputs("  |", out);
        for (size_t i = 0; i < n; i++)
            fputc(row[i] >= 32 && row[i] < 127 ? row[i] : '.', out);
        fputs("|\n", out);
        rows++;
        memcpy(prev, row, 16);
        have_prev = n == 16;
        off += (long)n;
    }
    fprintf(out, "%08lx\n", off);
    return rows;
}

int main(void) {
    FILE *f = fopen("dump.bin", "wb");
    check(f != NULL, "open");
    fputs("Hello, hexdump!\n", f);              /* 16 bytes of text */
    for (int i = 0; i < 48; i++)                /* three identical rows of zeros */
        fputc(0, f);
    for (int i = 0; i < 32; i++)                /* every byte value 0x00..0x1f then repeated 0xff row */
        fputc(i, f);
    for (int i = 0; i < 32; i++)
        fputc(0xFF, f);
    fputs("tail\x01\x7f\x80 end", f);           /* partial row with non-printables */
    fclose(f);

    f = fopen("dump.bin", "rb");
    FILE *o = fopen("dump.txt", "w");
    check(f && o, "open pair");
    int rows = hexdump(f, o);
    fclose(f);
    fclose(o);

    o = fopen("dump.txt", "r");
    char line[128];
    int total = 0;
    while (fgets(line, sizeof line, o)) {
        fputs(line, stdout);
        total++;
    }
    fclose(o);
    printf("%d rows printed, %d output lines\n", rows, total);
    check(rows == 6 && total == 9, "row counts");

    /* the dump can be parsed back into the original bytes (squeezed rows excepted) */
    o = fopen("dump.txt", "r");
    f = fopen("dump.bin", "rb");
    int checked = 0;
    while (fgets(line, sizeof line, o)) {
        unsigned long off;
        if (line[0] == '*' || strlen(line) < 20)
            continue;
        check(sscanf(line, "%lx", &off) == 1, "offset");
        unsigned char expect[16];
        fseek(f, (long)off, SEEK_SET);
        size_t n = fread(expect, 1, 16, f);
        const char *p = line + 9;
        for (size_t i = 0; i < n; i++) {
            if (i == 8)
                p++;
            unsigned v;
            check(sscanf(p, " %2x", &v) == 1, "hex");
            check(v == expect[i], "hex matches file");
            p += 3;
            checked++;
        }
    }
    fclose(o);
    fclose(f);
    printf("verified %d bytes against the file\n", checked);
    remove("dump.bin");
    remove("dump.txt");
    return 0;
}
