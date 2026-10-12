/*
 * title: K-way merge of sorted text files
 * topic: io_files
 * covers: merging sorted line files, head-of-stream comparison, duplicates across inputs, stable source ordering, empty inputs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K = 5, LINE = 48, TOTAL_MAX = 400 };

static unsigned rs = 8675309u;
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

typedef struct {
    FILE *f;
    char head[LINE];
    int live;
} Src;

static void advance(Src *s) {
    s->live = fgets(s->head, LINE, s->f) != NULL;
    if (s->live)
        s->head[strcspn(s->head, "\n")] = 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void) {
    int sizes[K] = {40, 0, 25, 60, 1};
    char names[K][16];
    char *all[TOTAL_MAX];
    int nall = 0;

    for (int k = 0; k < K; k++) {
        snprintf(names[k], sizeof names[k], "run%d.txt", k);
        char **mine = malloc(sizeof(char *) * (size_t)(sizes[k] + 1));
        check(mine != NULL, "alloc");
        for (int i = 0; i < sizes[k]; i++) {
            /* words from a small alphabet so duplicates across files are common */
            unsigned a = rnd() % 6, b = rnd() % 6, c = rnd() % 10;
            char w[LINE];
            snprintf(w, sizeof w, "%c%c%u", (char)('a' + a), (char)('a' + b), c);
            mine[i] = strdup(w);
        }
        qsort(mine, (size_t)sizes[k], sizeof(char *), cmp_str);
        FILE *f = fopen(names[k], "w");
        check(f != NULL, "open");
        for (int i = 0; i < sizes[k]; i++) {
            fprintf(f, "%s\n", mine[i]);
            all[nall++] = mine[i]; /* keep ownership for the reference sort */
        }
        fclose(f);
        free(mine);
    }
    check(nall <= TOTAL_MAX, "capacity");

    /* merge */
    Src src[K];
    for (int k = 0; k < K; k++) {
        src[k].f = fopen(names[k], "r");
        check(src[k].f != NULL, "open src");
        advance(&src[k]);
    }
    FILE *out = fopen("merged.txt", "w");
    check(out != NULL, "open out");
    int taken[K] = {0};
    int total = 0;
    for (;;) {
        int best = -1;
        for (int k = 0; k < K; k++)
            if (src[k].live && (best < 0 || strcmp(src[k].head, src[best].head) < 0))
                best = k;
        if (best < 0)
            break;
        fprintf(out, "%s\n", src[best].head);
        taken[best]++;
        total++;
        advance(&src[best]);
    }
    fclose(out);
    for (int k = 0; k < K; k++)
        fclose(src[k].f);

    printf("merged %d lines; taken per source:", total);
    for (int k = 0; k < K; k++)
        printf(" %d", taken[k]);
    printf("\n");
    check(total == nall, "total");

    /* verify sortedness and compare with a global sort */
    qsort(all, (size_t)nall, sizeof(char *), cmp_str);
    FILE *m = fopen("merged.txt", "r");
    char line[LINE];
    int i = 0, unique = 0;
    char prev[LINE] = "";
    char first[LINE] = "", last[LINE] = "";
    while (fgets(line, sizeof line, m)) {
        line[strcspn(line, "\n")] = 0;
        check(i < nall && strcmp(line, all[i]) == 0, "matches global sort");
        check(strcmp(prev, line) <= 0, "sorted");
        if (strcmp(prev, line) != 0)
            unique++;
        if (i == 0)
            strcpy(first, line);
        strcpy(last, line);
        strcpy(prev, line);
        i++;
    }
    fclose(m);
    check(i == nall, "count");
    printf("first %s last %s, %d distinct values\n", first, last, unique);

    /* most common value */
    int bestc = 0, run = 0;
    char bestv[LINE] = "";
    for (int j = 0; j < nall; j++) {
        run = (j > 0 && strcmp(all[j], all[j - 1]) == 0) ? run + 1 : 1;
        if (run > bestc) {
            bestc = run;
            strcpy(bestv, all[j]);
        }
    }
    printf("most repeated value %s x%d\n", bestv, bestc);

    for (int j = 0; j < nall; j++)
        free(all[j]);
    for (int k = 0; k < K; k++)
        remove(names[k]);
    remove("merged.txt");
    return 0;
}
