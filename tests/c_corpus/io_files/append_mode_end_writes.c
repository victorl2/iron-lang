/*
 * title: Append mode always writes at the end
 * topic: io_files
 * covers: fopen a, fseek then write, SEEK_END, growing log, ftell after append
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

static long fsize(const char *name) {
    FILE *f = fopen(name, "rb");
    check(f != NULL, "fsize open");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void append_line(const char *name, int seq, const char *msg) {
    FILE *f = fopen(name, "a");
    check(f != NULL, "append open");
    fprintf(f, "%03d %s\n", seq, msg);
    check(fclose(f) == 0, "append close");
}

int main(void) {
    const char *name = "journal.log";
    remove(name);

    const char *msgs[] = {"boot", "mount", "ready", "request", "response", "idle", "halt"};
    int nm = (int)(sizeof msgs / sizeof msgs[0]);
    for (int i = 0; i < nm; i++) {
        append_line(name, i, msgs[i]);
        printf("after entry %d size %ld\n", i, fsize(name));
    }

    /* append streams ignore seeks for writing */
    FILE *f = fopen(name, "a");
    check(f != NULL, "open a");
    fseek(f, 0, SEEK_SET);
    fputs("999 rewound\n", f);
    fclose(f);

    f = fopen(name, "r");
    char line[64];
    int count = 0;
    char last[64] = "";
    char first[64] = "";
    while (fgets(line, sizeof line, f)) {
        if (count == 0)
            strcpy(first, line);
        strcpy(last, line);
        count++;
    }
    fclose(f);
    first[strcspn(first, "\n")] = 0;
    last[strcspn(last, "\n")] = 0;
    printf("lines %d, first \"%s\", last \"%s\"\n", count, first, last);
    check(count == nm + 1, "count");
    check(strcmp(first, "000 boot") == 0, "first untouched");
    check(strcmp(last, "999 rewound") == 0, "last appended");

    /* ftell on an append stream after writing reflects the new end */
    f = fopen(name, "a+");
    check(f != NULL, "open a+");
    long before = fsize(name);
    fputs("tail\n", f);
    fflush(f);
    long tell = ftell(f);
    printf("size before %ld, tell after write %ld\n", before, tell);
    check(tell == before + 5, "tell");

    /* read the last two lines through the same stream */
    fseek(f, -17, SEEK_END);
    char a[32], b[32];
    check(fgets(a, sizeof a, f) != NULL, "a");
    check(fgets(b, sizeof b, f) != NULL, "b");
    a[strcspn(a, "\n")] = 0;
    b[strcspn(b, "\n")] = 0;
    printf("read back \"%s\" then \"%s\"\n", a, b);
    fclose(f);

    /* many small appends from two handles interleave without loss */
    FILE *h1 = fopen("two.log", "a");
    FILE *h2 = fopen("two.log", "a");
    check(h1 && h2, "two handles");
    setvbuf(h1, NULL, _IONBF, 0);
    setvbuf(h2, NULL, _IONBF, 0);
    for (int i = 0; i < 20; i++) {
        fprintf(h1, "A%02d\n", i);
        fprintf(h2, "B%02d\n", i);
    }
    fclose(h1);
    fclose(h2);
    f = fopen("two.log", "r");
    int na = 0, nb = 0;
    int okorder = 1, lasta = -1, lastb = -1;
    while (fgets(line, sizeof line, f)) {
        int v = atoi(line + 1);
        if (line[0] == 'A') {
            na++;
            okorder &= (v == lasta + 1);
            lasta = v;
        } else {
            nb++;
            okorder &= (v == lastb + 1);
            lastb = v;
        }
    }
    fclose(f);
    printf("two handles: A=%d B=%d in-order=%d size=%ld\n", na, nb, okorder, fsize("two.log"));
    check(na == 20 && nb == 20 && okorder, "interleave");

    remove(name);
    remove("two.log");
    return 0;
}
