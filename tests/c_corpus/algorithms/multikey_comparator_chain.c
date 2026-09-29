/*
 * title: Multi-key sorting with composable comparator chains
 * topic: algorithms
 * covers: comparator composition, function pointer tables, ascending and descending keys, qsort with total order, structs with strings
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    char dept[8];
    char name[10];
    int salary;
    int year;
} Emp;

static unsigned st = 8888u;
static unsigned rng(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 12;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef int (*Cmp)(const Emp *, const Emp *);

static int by_dept(const Emp *a, const Emp *b) { return strcmp(a->dept, b->dept); }
static int by_name(const Emp *a, const Emp *b) { return strcmp(a->name, b->name); }
static int by_salary(const Emp *a, const Emp *b) { return (a->salary > b->salary) - (a->salary < b->salary); }
static int by_year(const Emp *a, const Emp *b) { return (a->year > b->year) - (a->year < b->year); }
static int by_id(const Emp *a, const Emp *b) { return (a->id > b->id) - (a->id < b->id); }
static int by_salary_desc(const Emp *a, const Emp *b) { return by_salary(b, a); }
static int by_year_desc(const Emp *a, const Emp *b) { return by_year(b, a); }

#define MAXKEYS 6
typedef struct {
    const char *label;
    Cmp keys[MAXKEYS];
} Chain;

static const Chain *cur;

static int cmp_chain(const void *x, const void *y) {
    const Emp *a = x, *b = y;
    for (int i = 0; i < MAXKEYS && cur->keys[i]; i++) {
        int c = cur->keys[i](a, b);
        if (c)
            return c;
    }
    return by_id(a, b); /* unique tie-break: total order, qsort result is deterministic */
}

int main(void) {
    static const char *depts[] = {"ENG", "OPS", "HR", "SALES"};
    static const char *first[] = {"Ada", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal"};
    enum { N = 40 };
    Emp e[N], w[N];
    for (int i = 0; i < N; i++) {
        e[i].id = 100 + i;
        strcpy(e[i].dept, depts[rng() % 4]);
        snprintf(e[i].name, sizeof e[i].name, "%s%d", first[rng() % 8], (int)(rng() % 3));
        e[i].salary = 40 + (int)(rng() % 8) * 5;
        e[i].year = 2015 + (int)(rng() % 8);
    }
    static const Chain chains[] = {
        {"dept, salary desc, name", {by_dept, by_salary_desc, by_name}},
        {"year desc, dept, id", {by_year_desc, by_dept}},
        {"salary, year, name", {by_salary, by_year, by_name}},
        {"name only (ties by id)", {by_name}},
    };
    for (unsigned c = 0; c < sizeof chains / sizeof chains[0]; c++) {
        memcpy(w, e, sizeof e);
        cur = &chains[c];
        qsort(w, N, sizeof(Emp), cmp_chain);
        for (int i = 1; i < N; i++)
            check(cmp_chain(&w[i - 1], &w[i]) < 0, "strict total order");
        printf("chain: %s\n", chains[c].label);
        for (int i = 0; i < 5; i++)
            printf("  %d %-5s %-5s %d %d\n", w[i].id, w[i].dept, w[i].name, w[i].salary, w[i].year);
    }
    /* group-by after dept-major sort: count and total salary per dept */
    memcpy(w, e, sizeof e);
    cur = &chains[0];
    qsort(w, N, sizeof(Emp), cmp_chain);
    int i = 0;
    printf("groups:\n");
    while (i < N) {
        int j = i, total = 0;
        while (j < N && strcmp(w[j].dept, w[i].dept) == 0)
            total += w[j++].salary;
        printf("  %-5s n=%d total=%d top=%s\n", w[i].dept, j - i, total, w[i].name);
        i = j;
    }
    return 0;
}
