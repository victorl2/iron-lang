/*
 * title: getdelim with custom record separators
 * topic: io_files
 * covers: getdelim ';' and NUL delimiters, empty records, trailing partial record, key=value split
 * deps: posix
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

static int split_kv(char *rec, char **k, char **v) {
    char *eq = strchr(rec, '=');
    if (!eq)
        return 0;
    *eq = 0;
    *k = rec;
    *v = eq + 1;
    return 1;
}

int main(void) {
    /* semicolon separated key=value settings with quirks */
    const char *text = "host=example;port=8080;;debug=;bad record;name=with space;last=x";
    FILE *f = fopen("delim.txt", "w");
    check(f != NULL, "open");
    fputs(text, f);
    fclose(f);

    f = fopen("delim.txt", "r");
    char *rec = NULL;
    size_t cap = 0;
    ssize_t n;
    int idx = 0, good = 0, empty = 0, malformed = 0;
    while ((n = getdelim(&rec, &cap, ';', f)) != -1) {
        int term = n > 0 && rec[n - 1] == ';';
        if (term)
            rec[n - 1] = 0;
        char *k, *v;
        if (rec[0] == 0) {
            empty++;
            printf("#%d empty record\n", idx);
        } else if (split_kv(rec, &k, &v)) {
            good++;
            printf("#%d key=\"%s\" value=\"%s\"%s\n", idx, k, v, term ? "" : " (unterminated)");
        } else {
            malformed++;
            printf("#%d malformed \"%s\"\n", idx, rec);
        }
        idx++;
    }
    free(rec);
    fclose(f);
    printf("records %d: good %d empty %d malformed %d\n", idx, good, empty, malformed);
    check(idx == 7 && good == 5 && empty == 1 && malformed == 1, "counts");

    /* NUL separated list, records may contain newlines */
    f = fopen("nul.bin", "wb");
    const char blob[] = "first\0second line\nwith newline\0\0fourth\0tail";
    check(fwrite(blob, 1, sizeof blob - 1, f) != 0, "fwrite");
    fclose(f);

    f = fopen("nul.bin", "rb");
    rec = NULL;
    cap = 0;
    idx = 0;
    size_t total = 0;
    while ((n = getdelim(&rec, &cap, '\0', f)) != -1) {
        total += (size_t)n;
        size_t len = (size_t)n;
        int term = len > 0 && rec[len - 1] == 0;
        if (term)
            len--;
        char shown[64];
        size_t o = 0;
        for (size_t i = 0; i < len && o + 3 < sizeof shown; i++) {
            if (rec[i] == '\n') {
                shown[o++] = '\\';
                shown[o++] = 'n';
            } else {
                shown[o++] = rec[i];
            }
        }
        shown[o] = 0;
        printf("nul record %d len %zu term=%d \"%s\"\n", idx++, len, term, shown);
    }
    check(total == sizeof blob - 1, "all bytes consumed");
    printf("nul records %d, bytes %zu\n", idx, total);
    free(rec);
    fclose(f);

    /* delimiter equal to newline behaves like getline */
    f = fopen("nl.txt", "w");
    fputs("a\nbb\nccc", f);
    fclose(f);
    f = fopen("nl.txt", "r");
    rec = NULL;
    cap = 0;
    int lens[4], m = 0;
    while ((n = getdelim(&rec, &cap, '\n', f)) != -1 && m < 4)
        lens[m++] = (int)n;
    printf("newline delimited lengths: %d %d %d\n", lens[0], lens[1], lens[2]);
    check(m == 3, "three lines");
    free(rec);
    fclose(f);

    remove("delim.txt");
    remove("nul.bin");
    remove("nl.txt");
    return 0;
}
