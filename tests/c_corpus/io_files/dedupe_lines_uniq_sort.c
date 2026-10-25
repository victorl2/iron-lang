/*
 * title: Line deduplication uniq and sort -u
 * topic: io_files
 * covers: adjacent duplicate removal, uniq -c counts, sort then unique, first-seen order with hash set, file streaming
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rs = 60221u;
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

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

typedef struct Node {
    char *key;
    int count;
    struct Node *next;
} Node;

enum { NB = 64 };

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s)
        h = h * 33u + (unsigned char)*s++;
    return h;
}

int main(void) {
    static const char *pool[] = {"apple", "pear", "fig", "plum", "kiwi", "lime", "date", "quince", "grape"};
    enum { N = 150 };
    FILE *f = fopen("dup.txt", "w");
    check(f != NULL, "open");
    for (int i = 0; i < N; i++) {
        unsigned r = rnd();
        const char *w = pool[r % 9];
        int rep = 1 + (int)((r >> 8) % 3); /* runs of repeated lines */
        for (int k = 0; k < rep; k++)
            fprintf(f, "%s\n", w);
    }
    fclose(f);

    /* 1. uniq -c: collapse adjacent runs */
    f = fopen("dup.txt", "r");
    char line[64], prev[64] = "";
    int run = 0, runs = 0, lines = 0, maxrun = 0;
    FILE *u = fopen("uniq.txt", "w");
    while (fgets(line, sizeof line, f)) {
        lines++;
        if (run > 0 && strcmp(line, prev) == 0) {
            run++;
        } else {
            if (run > 0)
                fputs(prev, u);
            run = 1;
            runs++;
            strcpy(prev, line);
        }
        if (run > maxrun)
            maxrun = run;
    }
    if (run > 0)
        fputs(prev, u);
    fclose(f);
    fclose(u);
    printf("input lines %d, adjacent runs %d, longest run %d\n", lines, runs, maxrun);

    /* 2. sort -u: load, sort, drop equal neighbours */
    f = fopen("dup.txt", "r");
    char **v = malloc(sizeof(char *) * (size_t)lines);
    check(v != NULL, "alloc");
    int n = 0;
    while (fgets(line, sizeof line, f))
        v[n++] = strdup(line);
    fclose(f);
    qsort(v, (size_t)n, sizeof(char *), cmp_str);
    int distinct = 0;
    printf("sorted unique:");
    for (int i = 0; i < n; i++) {
        if (i == 0 || strcmp(v[i], v[i - 1]) != 0) {
            distinct++;
            v[i][strcspn(v[i], "\n")] = 0;
            printf(" %s", v[i]);
            v[i][strlen(v[i])] = '\n';
        }
    }
    printf("\n");
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);

    /* 3. first-seen order using a chained hash set, with counts */
    Node *table[NB] = {0};
    Node *order[16];
    int no = 0;
    f = fopen("dup.txt", "r");
    while (fgets(line, sizeof line, f)) {
        unsigned h = hash(line) % NB;
        Node *p = table[h];
        while (p && strcmp(p->key, line) != 0)
            p = p->next;
        if (!p) {
            p = malloc(sizeof *p);
            check(p != NULL, "node");
            p->key = strdup(line);
            p->count = 0;
            p->next = table[h];
            table[h] = p;
            check(no < 16, "order cap");
            order[no++] = p;
        }
        p->count++;
    }
    fclose(f);
    printf("first-seen order with counts:\n");
    int total = 0;
    for (int i = 0; i < no; i++) {
        order[i]->key[strcspn(order[i]->key, "\n")] = 0;
        printf("  %-7s %3d\n", order[i]->key, order[i]->count);
        total += order[i]->count;
    }
    check(no == distinct, "same distinct count");
    check(total == lines, "counts add up");

    /* uniq output after adjacent collapse still contains duplicates non-adjacent */
    f = fopen("uniq.txt", "r");
    int ul = 0;
    while (fgets(line, sizeof line, f))
        ul++;
    fclose(f);
    printf("uniq output lines %d (runs %d), distinct values %d\n", ul, runs, distinct);
    check(ul == runs && ul >= distinct, "uniq");

    for (int i = 0; i < NB; i++) {
        Node *p = table[i];
        while (p) {
            Node *nx = p->next;
            free(p->key);
            free(p);
            p = nx;
        }
    }
    remove("dup.txt");
    remove("uniq.txt");
    return 0;
}
