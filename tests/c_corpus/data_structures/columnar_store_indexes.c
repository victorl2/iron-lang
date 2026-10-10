/*
 * title: Columnar table with dictionary column, sorted index and posting-list index
 * topic: data_structures
 * covers: column store, dictionary encoding, sorted secondary index, posting lists, tombstones, group-by aggregation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 7000007u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define MAXROWS 2000
#define NDEPT 8
#define MAXDICT 32

static const char *dept_names[MAXDICT] = { "ops", "eng", "hr", "fin", "legal", "sales", "it", "art", "qa", "docs" };

typedef struct {
    int n;
    int salary[MAXROWS];
    int age[MAXROWS];
    unsigned char dept_code[MAXROWS];       /* dictionary-encoded column */
    unsigned char dead[MAXROWS];            /* tombstones */
    char dict[MAXDICT][8];                  /* code -> string */
    int dict_n;
    /* secondary index 1: rows sorted by (salary, row) */
    int by_salary[MAXROWS];
    int idx_n;
    /* secondary index 2: posting list per dictionary code */
    int *post[MAXDICT];
    int post_n[MAXDICT], post_cap[MAXDICT];
} Table;

static int dict_code(Table *t, const char *s) {
    for (int i = 0; i < t->dict_n; i++) if (strcmp(t->dict[i], s) == 0) return i;
    CHECK(t->dict_n < MAXDICT);
    snprintf(t->dict[t->dict_n], sizeof t->dict[0], "%s", s);
    return t->dict_n++;
}
static int key_less(const Table *t, int a, int b) {
    if (t->salary[a] != t->salary[b]) return t->salary[a] < t->salary[b];
    return a < b;
}
static void t_insert(Table *t, int salary, int age, const char *dept) {
    CHECK(t->n < MAXROWS);
    int r = t->n++;
    t->salary[r] = salary; t->age[r] = age; t->dead[r] = 0;
    int code = dict_code(t, dept);
    t->dept_code[r] = (unsigned char)code;
    /* maintain sorted index by binary search + shift */
    int lo = 0, hi = t->idx_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (key_less(t, t->by_salary[mid], r)) lo = mid + 1; else hi = mid;
    }
    memmove(&t->by_salary[lo + 1], &t->by_salary[lo], (size_t)(t->idx_n - lo) * sizeof(int));
    t->by_salary[lo] = r;
    t->idx_n++;
    /* posting list append (rows arrive in increasing order, so lists stay sorted) */
    if (t->post_n[code] == t->post_cap[code]) {
        t->post_cap[code] = t->post_cap[code] ? t->post_cap[code] * 2 : 8;
        t->post[code] = realloc(t->post[code], (size_t)t->post_cap[code] * sizeof(int));
        CHECK(t->post[code]);
    }
    t->post[code][t->post_n[code]++] = r;
}
static void t_delete(Table *t, int r) { t->dead[r] = 1; }

/* index query: salary in [lo, hi]; returns matching live rows via callback order = index order */
static int q_salary_range(const Table *t, int lo, int hi, int *out) {
    int a = 0, b = t->idx_n;
    while (a < b) { int m = (a + b) / 2; if (t->salary[t->by_salary[m]] < lo) a = m + 1; else b = m; }
    int n = 0;
    for (int i = a; i < t->idx_n && t->salary[t->by_salary[i]] <= hi; i++)
        if (!t->dead[t->by_salary[i]]) out[n++] = t->by_salary[i];
    return n;
}
static int q_dept(const Table *t, const char *dept, int *out) {
    int code = -1;
    for (int i = 0; i < t->dict_n; i++) if (strcmp(t->dict[i], dept) == 0) code = i;
    if (code < 0) return 0;
    int n = 0;
    for (int i = 0; i < t->post_n[code]; i++)
        if (!t->dead[t->post[code][i]]) out[n++] = t->post[code][i];
    return n;
}
/* conjunction: dept and salary range, intersecting the two candidate sets through a bitmap */
static int q_and(const Table *t, const char *dept, int lo, int hi, int *out) {
    static int a[MAXROWS], b[MAXROWS];
    static unsigned char mark[MAXROWS];
    int na = q_dept(t, dept, a), nb = q_salary_range(t, lo, hi, b);
    memset(mark, 0, (size_t)t->n);
    for (int i = 0; i < na; i++) mark[a[i]] = 1;
    int n = 0;
    for (int i = 0; i < nb; i++) if (mark[b[i]]) out[n++] = b[i];
    return n;
}

int main(void) {
    static Table t;
    memset(&t, 0, sizeof t);
    int qcount = 0;
    long total_hits = 0;
    for (int step = 0; step < 1800; step++) {
        unsigned op = rnd() % 10;
        if (op < 5 && t.n < MAXROWS - 1) {
            int sal = 30000 + (int)(rnd() % 90) * 500;
            int age = 20 + (int)(rnd() % 45);
            t_insert(&t, sal, age, dept_names[rnd() % NDEPT]);
        } else if (op < 6 && t.n > 0) {
            t_delete(&t, (int)(rnd() % (unsigned)t.n));
        } else if (t.n > 0) {
            int lo = 30000 + (int)(rnd() % 90) * 500;
            int hi = lo + (int)(rnd() % 40) * 500;
            const char *d = dept_names[rnd() % NDEPT];
            static int res[MAXROWS];
            int n1 = q_salary_range(&t, lo, hi, res);
            int expect = 0;
            for (int r = 0; r < t.n; r++) if (!t.dead[r] && t.salary[r] >= lo && t.salary[r] <= hi) expect++;
            CHECK(n1 == expect);
            for (int i = 1; i < n1; i++) CHECK(t.salary[res[i - 1]] <= t.salary[res[i]]);
            int n2 = q_dept(&t, d, res);
            expect = 0;
            for (int r = 0; r < t.n; r++) if (!t.dead[r] && strcmp(t.dict[t.dept_code[r]], d) == 0) expect++;
            CHECK(n2 == expect);
            int n3 = q_and(&t, d, lo, hi, res);
            expect = 0;
            for (int r = 0; r < t.n; r++)
                if (!t.dead[r] && strcmp(t.dict[t.dept_code[r]], d) == 0 && t.salary[r] >= lo && t.salary[r] <= hi) expect++;
            CHECK(n3 == expect);
            total_hits += n1 + n2 + n3;
            qcount++;
        }
    }
    /* group-by dept with sum and average salary, straight from the encoded column */
    long sum[MAXDICT] = { 0 };
    int cnt[MAXDICT] = { 0 };
    int live = 0;
    for (int r = 0; r < t.n; r++) if (!t.dead[r]) { sum[t.dept_code[r]] += t.salary[r]; cnt[t.dept_code[r]]++; live++; }
    printf("rows=%d live=%d dictionary=%d strings, queries=%d, total hits=%ld\n", t.n, live, t.dict_n, qcount, total_hits);
    int sorted_codes[MAXDICT];
    for (int i = 0; i < t.dict_n; i++) sorted_codes[i] = i;
    for (int i = 1; i < t.dict_n; i++) {
        int v = sorted_codes[i], j = i - 1;
        while (j >= 0 && strcmp(t.dict[sorted_codes[j]], t.dict[v]) > 0) { sorted_codes[j + 1] = sorted_codes[j]; j--; }
        sorted_codes[j + 1] = v;
    }
    for (int i = 0; i < t.dict_n; i++) {
        int c = sorted_codes[i];
        if (!cnt[c]) continue;
        /* cross-check the group aggregate through the posting list */
        long s2 = 0;
        int c2 = 0;
        for (int k = 0; k < t.post_n[c]; k++) if (!t.dead[t.post[c][k]]) { s2 += t.salary[t.post[c][k]]; c2++; }
        CHECK(s2 == sum[c] && c2 == cnt[c]);
        printf("  %-6s rows=%3d avg salary=%ld\n", t.dict[c], cnt[c], sum[c] / cnt[c]);
    }
    /* index order check: whole sorted index is monotone */
    for (int i = 1; i < t.idx_n; i++) CHECK(key_less(&t, t.by_salary[i - 1], t.by_salary[i]));
    for (int c = 0; c < MAXDICT; c++) free(t.post[c]);
    return 0;
}
