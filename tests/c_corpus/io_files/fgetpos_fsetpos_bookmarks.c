/*
 * title: fgetpos and fsetpos line bookmarks
 * topic: io_files
 * covers: fgetpos, fsetpos, opaque positions, revisiting lines, bookmark table
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINES = 12 };

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static const char *words[] = {"amber", "basalt", "cobalt", "dune", "ember", "flint",
                              "garnet", "hazel", "indigo", "jade", "kelp", "lapis"};

int main(void) {
    FILE *f = fopen("bm.txt", "w+");
    check(f != NULL, "open");
    for (int i = 0; i < LINES; i++)
        fprintf(f, "%02d:%s%s\n", i, words[i], (i % 3 == 0) ? "-extra-long-suffix" : "");
    rewind(f);

    fpos_t pos[LINES];
    char line[128];
    int n = 0;
    for (;;) {
        fpos_t here;
        check(fgetpos(f, &here) == 0, "fgetpos");
        if (fgets(line, sizeof line, f) == NULL)
            break;
        check(n < LINES, "too many lines");
        pos[n++] = here;
    }
    printf("recorded %d bookmarks\n", n);
    check(n == LINES, "line count");

    /* jump around using the bookmarks */
    int order[] = {7, 0, 11, 3, 3, 9, 1};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        int k = order[i];
        check(fsetpos(f, &pos[k]) == 0, "fsetpos");
        check(fgets(line, sizeof line, f) != NULL, "fgets");
        line[strcspn(line, "\n")] = 0;
        int idx = atoi(line);
        printf("bookmark %2d -> \"%s\"\n", k, line);
        check(idx == k, "index matches");
    }

    /* fsetpos clears EOF */
    fseek(f, 0, SEEK_END);
    check(fgetc(f) == EOF && feof(f), "eof set");
    fsetpos(f, &pos[5]);
    check(!feof(f), "eof cleared");
    int c = fgetc(f);
    printf("after eof, bookmark 5 starts with '%c'\n", c);

    /* a bookmark is usable for writing too: overwrite the digits of line 4 */
    fsetpos(f, &pos[4]);
    fputs("XX", f);
    fflush(f);
    fsetpos(f, &pos[4]);
    check(fgets(line, sizeof line, f) != NULL, "fgets");
    line[strcspn(line, "\n")] = 0;
    printf("patched line 4: \"%s\"\n", line);
    check(strcmp(line, "XX:ember") == 0, "patched");

    /* two independent streams with the same bookmark see the same data */
    FILE *g = fopen("bm.txt", "r");
    check(g != NULL, "second open");
    fsetpos(g, &pos[8]);
    char l2[128], l3[128];
    check(fgets(l2, sizeof l2, g) != NULL, "fgets");
    fsetpos(f, &pos[8]);
    check(fgets(l3, sizeof l3, f) != NULL, "fgets");
    check(strcmp(l2, l3) == 0, "same data through both streams");
    l2[strcspn(l2, "\n")] = 0;
    printf("second stream line 8: \"%s\"\n", l2);
    fclose(g);

    long total = 0;
    fseek(f, 0, SEEK_END);
    total = ftell(f);
    printf("total bytes %ld\n", total);
    fclose(f);
    check(remove("bm.txt") == 0, "remove");
    return 0;
}
