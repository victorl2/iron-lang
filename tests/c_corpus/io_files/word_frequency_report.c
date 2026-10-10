/*
 * title: Word frequency report with histogram
 * topic: io_files
 * covers: tokenizing a text file with fgetc, case folding, hash table counts, sorting by count then word, aligned bar chart, report file
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NB = 128, MAXW = 24 };

typedef struct Word {
    char w[MAXW];
    int n;
    struct Word *next;
} Word;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned hash(const char *s) {
    unsigned h = 0;
    while (*s)
        h = h * 131u + (unsigned char)*s++;
    return h % NB;
}

static int cmp(const void *a, const void *b) {
    const Word *x = *(Word *const *)a, *y = *(Word *const *)b;
    if (x->n != y->n)
        return y->n - x->n;
    return strcmp(x->w, y->w);
}

int main(void) {
    const char *corpus =
        "It was the best of times, it was the worst of times, it was the age of wisdom, "
        "it was the age of foolishness, it was the epoch of belief, it was the epoch of "
        "incredulity, it was the season of Light, it was the season of Darkness.\n"
        "We had everything before us, we had nothing before us; we were all going direct to "
        "Heaven, we were all going direct the other way.\n";
    FILE *f = fopen("corpus.txt", "w");
    check(f != NULL, "open");
    fputs(corpus, f);
    fclose(f);

    Word *table[NB] = {0};
    int distinct = 0, tokens = 0, maxlen = 0;
    f = fopen("corpus.txt", "r");
    check(f != NULL, "reopen");
    char cur[MAXW];
    int n = 0, c;
    do {
        c = fgetc(f);
        if (c != EOF && (isalpha(c) || c == '\'')) {
            if (n < MAXW - 1)
                cur[n++] = (char)tolower(c);
            continue;
        }
        if (n == 0)
            continue;
        cur[n] = 0;
        n = 0;
        tokens++;
        unsigned h = hash(cur);
        Word *w = table[h];
        while (w && strcmp(w->w, cur) != 0)
            w = w->next;
        if (!w) {
            w = calloc(1, sizeof *w);
            check(w != NULL, "calloc");
            snprintf(w->w, sizeof w->w, "%s", cur);
            w->next = table[h];
            table[h] = w;
            distinct++;
        }
        w->n++;
        if ((int)strlen(cur) > maxlen)
            maxlen = (int)strlen(cur);
    } while (c != EOF);
    fclose(f);

    Word **all = malloc(sizeof(Word *) * (size_t)distinct);
    check(all != NULL, "alloc");
    int k = 0;
    for (int i = 0; i < NB; i++)
        for (Word *w = table[i]; w; w = w->next)
            all[k++] = w;
    check(k == distinct, "collect");
    qsort(all, (size_t)distinct, sizeof *all, cmp);

    f = fopen("report.txt", "w");
    check(f != NULL, "report open");
    fprintf(f, "%d tokens, %d distinct words, longest %d\n", tokens, distinct, maxlen);
    fprintf(f, "%-*s %5s %6s  %s\n", maxlen, "word", "count", "share", "histogram");
    int shown = 0, sum_shown = 0;
    for (int i = 0; i < distinct && all[i]->n >= 3; i++) {
        double share = 100.0 * all[i]->n / tokens;
        fprintf(f, "%-*s %5d %5.1f%%  ", maxlen, all[i]->w, all[i]->n, share);
        for (int b = 0; b < all[i]->n; b++)
            fputc('#', f);
        fputc('\n', f);
        shown++;
        sum_shown += all[i]->n;
    }
    fprintf(f, "%d words with 3 or more occurrences cover %d tokens\n", shown, sum_shown);
    check(fclose(f) == 0, "report close");

    f = fopen("report.txt", "r");
    char line[128];
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);

    /* singletons in alphabetical order */
    int singles = 0;
    printf("singletons:");
    Word **s = malloc(sizeof(Word *) * (size_t)distinct);
    for (int i = 0; i < distinct; i++)
        if (all[i]->n == 1)
            s[singles++] = all[i];
    for (int i = 0; i < singles; i++)
        for (int j = i + 1; j < singles; j++)
            if (strcmp(s[j]->w, s[i]->w) < 0) {
                Word *t = s[i];
                s[i] = s[j];
                s[j] = t;
            }
    for (int i = 0; i < singles && i < 12; i++)
        printf(" %s", s[i]->w);
    printf("%s (%d total)\n", singles > 12 ? " ..." : "", singles);
    free(s);

    int total = 0;
    for (int i = 0; i < distinct; i++)
        total += all[i]->n;
    check(total == tokens, "counts add up");
    check(strcmp(all[0]->w, "the") == 0 && all[0]->n == 9, "top word");

    for (int i = 0; i < distinct; i++)
        free(all[i]);
    free(all);
    remove("corpus.txt");
    remove("report.txt");
    return 0;
}
