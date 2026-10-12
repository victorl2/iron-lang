/*
 * title: fopen mode semantics matrix
 * topic: io_files
 * covers: fopen r/w/a/r+/w+/a+, truncation, append position, read back
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

static const char *NAME = "modes.txt";

static void put(const char *s) {
    FILE *f = fopen(NAME, "w");
    check(f != NULL, "put open");
    fputs(s, f);
    check(fclose(f) == 0, "put close");
}

static void slurp(char *buf, size_t cap) {
    FILE *f = fopen(NAME, "rb");
    check(f != NULL, "slurp open");
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
}

static void show(const char *label, const char *expect) {
    char buf[128];
    slurp(buf, sizeof buf);
    printf("%-4s -> [%s]\n", label, buf);
    check(strcmp(buf, expect) == 0, label);
}

int main(void) {
    /* r: read only, missing file fails */
    remove(NAME);
    check(fopen(NAME, "r") == NULL, "r missing");
    check(fopen(NAME, "r+") == NULL, "r+ missing");
    puts("r/r+ on missing file fail");

    /* w creates and truncates */
    put("abcdef");
    FILE *f = fopen(NAME, "w");
    check(f != NULL, "w");
    fputs("XY", f);
    fclose(f);
    show("w", "XY");

    /* a appends, ignoring seeks for writes */
    put("abcdef");
    f = fopen(NAME, "a");
    check(f != NULL, "a");
    fseek(f, 0, SEEK_SET);
    fputs("123", f);
    fclose(f);
    show("a", "abcdef123");

    /* r+ overwrites in place, no truncation */
    put("abcdef");
    f = fopen(NAME, "r+");
    check(f != NULL, "r+");
    fputs("XY", f);
    fclose(f);
    show("r+", "XYcdef");

    /* r+ read then write requires a seek in between */
    put("0123456789");
    f = fopen(NAME, "r+");
    char two[3] = {0};
    check(fread(two, 1, 2, f) == 2, "r+ read");
    check(fseek(f, 0, SEEK_CUR) == 0, "r+ seek between");
    fputs("__", f);
    fclose(f);
    printf("r+ read \"%s\"\n", two);
    show("r+rw", "01__456789");

    /* w+ truncates then allows reading back */
    put("old contents");
    f = fopen(NAME, "w+");
    check(f != NULL, "w+");
    fputs("fresh data", f);
    rewind(f);
    char rb[32] = {0};
    check(fgets(rb, sizeof rb, f) != NULL, "w+ read");
    printf("w+ read back \"%s\"\n", rb);
    check(strcmp(rb, "fresh data") == 0, "w+ data");
    fclose(f);
    show("w+", "fresh data");

    /* a+ reads from start, writes at end */
    put("head|");
    f = fopen(NAME, "a+");
    check(f != NULL, "a+");
    char first[5] = {0};
    fseek(f, 0, SEEK_SET); /* initial position of a+ reads is implementation-defined */
    check(fread(first, 1, 4, f) == 4, "a+ read");
    printf("a+ initial read \"%s\"\n", first);
    check(strcmp(first, "head") == 0, "a+ first");
    fseek(f, 0, SEEK_SET);
    fputs("tail", f);
    fseek(f, 0, SEEK_SET);
    char all[32] = {0};
    size_t n = fread(all, 1, sizeof all - 1, f);
    printf("a+ full read (%zu) \"%s\"\n", n, all);
    check(strcmp(all, "head|tail") == 0, "a+ full");
    fclose(f);
    show("a+", "head|tail");

    /* explicit b flag forms are equivalent */
    put("bin");
    f = fopen(NAME, "rb+");
    check(f != NULL, "rb+");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fputc('!', f);
    fclose(f);
    printf("rb+ size before append %ld\n", sz);
    show("rb+", "bin!");

    f = fopen(NAME, "r+b");
    check(f != NULL, "r+b");
    fclose(f);

    /* writing to a read-only stream fails */
    f = fopen(NAME, "r");
    check(fputc('z', f) == EOF, "write on r stream");
    check(ferror(f) != 0, "ferror on r stream");
    fclose(f);
    puts("write on r stream rejected");

    check(remove(NAME) == 0, "cleanup");
    return 0;
}
