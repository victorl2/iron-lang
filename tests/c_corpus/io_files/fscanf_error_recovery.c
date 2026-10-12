/*
 * title: fscanf parsing with error recovery
 * topic: io_files
 * covers: fscanf return values, %n, %[^\n] skipping bad lines, EOF vs matching failure, width limits
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

/* discard the rest of the current line */
static void skip_line(FILE *f) {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n')
        ;
}

int main(void) {
    const char *text = "alice 30 55.5\n"
                       "bob thirty 61.0\n"
                       "carol 27 48.25\n"
                       "dave 41\n"
                       "erin 35 70\n"
                       "\n"
                       "frank_with_a_very_long_name 22 80.5\n"
                       "gina 0x1F 52.5\n"
                       "hal -5 65.75 trailing junk\n";
    FILE *f = fopen("people.txt", "w");
    check(f != NULL, "open w");
    check(fputs(text, f) >= 0, "fputs");
    fclose(f);

    f = fopen("people.txt", "r");
    check(f != NULL, "open r");
    int lineno = 0, ok = 0, bad = 0;
    double weight_sum = 0;
    long age_sum = 0;
    char raw[128];
    /* fscanf skips newlines as whitespace, so a short line would swallow the next one:
     * read whole lines with fgets and scan each one, which makes recovery trivial */
    while (fgets(raw, sizeof raw, f)) {
        char name[12];
        int age, used = 0;
        double w;
        lineno++;
        int n = sscanf(raw, "%11s %d %lf%n", name, &age, &w, &used);
        if (n == EOF) {
            printf("line %d blank\n", lineno);
            continue;
        }
        if (n == 3) {
            ok++;
            age_sum += age;
            weight_sum += w;
            printf("line %d ok: %-12s age %3d weight %6.2f%s\n", lineno, name, age, w,
                   raw[used] != '\n' ? " (trailing text ignored)" : "");
        } else {
            bad++;
            printf("line %d bad: matched %d field(s)\n", lineno, n);
        }
    }
    printf("ok %d bad %d, age sum %ld, weight sum %.2f\n", ok, bad, age_sum, weight_sum);
    check(feof(f), "eof");
    fclose(f);

    /* the same data through fscanf directly shows the hazard: the short "dave 41" line
     * makes %lf run into the next line */
    f = fopen("people.txt", "r");
    check(f != NULL, "reopen");
    int recs = 0;
    for (;;) {
        char name[12];
        int age;
        double w;
        int n = fscanf(f, "%11s %d %lf", name, &age, &w);
        if (n == EOF)
            break;
        if (n < 3) {
            skip_line(f);
            continue;
        }
        recs++;
    }
    fclose(f);
    printf("fscanf-only pass accepted %d records\n", recs);

    /* %n reports consumed characters: tokenise a line of integers by hand */
    const char *nums = "12 -7 +99 0 abc 5";
    f = fopen("nums.txt", "w");
    check(fputs(nums, f) >= 0, "nums");
    fclose(f);

    char line[64];
    f = fopen("nums.txt", "r");
    check(fgets(line, sizeof line, f) != NULL, "read line");
    fclose(f);
    const char *p = line;
    int used = 0, v, count = 0, sum = 0;
    while (sscanf(p, "%d%n", &v, &used) == 1) {
        printf("  int %d consumed %d\n", v, used);
        sum += v;
        count++;
        p += used;
    }
    printf("parsed %d ints (sum %d), stopped at \"%s\"\n", count, sum, p + strspn(p, " "));
    check(count == 4 && sum == 104, "int scan");

    /* scanset parsing with width and mixed literals */
    const char *rec = "id:42|name:Widget Pro|tags:red,blue,green";
    int id = 0;
    char nm[16] = "", tags[32] = "";
    int r = sscanf(rec, "id:%d|name:%15[^|]|tags:%31s", &id, nm, tags);
    printf("record: matched %d id=%d name=\"%s\" tags=\"%s\"\n", r, id, nm, tags);
    check(r == 3 && id == 42 && strcmp(nm, "Widget Pro") == 0, "scanset");

    /* matching failure leaves the offending text unread */
    int a = -1, b = -1;
    r = sscanf("17 xyz", "%d %d", &a, &b);
    printf("partial: matched %d a=%d b=%d\n", r, a, b);
    check(r == 1 && a == 17 && b == -1, "partial");

    /* empty input reports EOF, distinct from a matching failure */
    r = sscanf("", "%d", &a);
    printf("empty input returns EOF: %d\n", r == EOF);
    check(r == EOF, "eof return");

    remove("people.txt");
    remove("nums.txt");
    return 0;
}
