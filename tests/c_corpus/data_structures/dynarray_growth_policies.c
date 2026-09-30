/*
 * title: Dynamic array growth policy comparison
 * topic: data_structures
 * covers: growth factor, amortized cost, realloc counting, fibonacci growth, arithmetic growth
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef enum { P_DOUBLE, P_ONEPOINTFIVE, P_PLUS16, P_FIB, P_EXACT, P_COUNT } Policy;
static const char *policy_name[P_COUNT] = { "double", "x1.5", "plus16", "fibonacci", "exact" };

typedef struct {
    int *data;
    size_t len, cap;
    Policy pol;
    size_t fib_a, fib_b;
    unsigned long grows, copied;
} Arr;

static void arr_init(Arr *a, Policy p) {
    memset(a, 0, sizeof *a);
    a->pol = p; a->fib_a = 1; a->fib_b = 2;
}

static size_t next_cap(Arr *a, size_t need) {
    size_t c = a->cap;
    switch (a->pol) {
    case P_DOUBLE: c = c ? c * 2 : 1; break;
    case P_ONEPOINTFIVE: c = c < 2 ? c + 2 : c + c / 2; break;
    case P_PLUS16: c += 16; break;
    case P_FIB:
        while (a->fib_b <= a->cap) { size_t t = a->fib_a + a->fib_b; a->fib_a = a->fib_b; a->fib_b = t; }
        c = a->fib_b;
        break;
    case P_EXACT: c = need; break;
    default: break;
    }
    return c < need ? need : c;
}

static void arr_push(Arr *a, int x) {
    if (a->len == a->cap) {
        size_t nc = next_cap(a, a->len + 1);
        int *nd = malloc(nc * sizeof *nd);
        CHECK(nd != NULL);
        if (a->len) memcpy(nd, a->data, a->len * sizeof *nd);
        free(a->data);
        a->data = nd; a->cap = nc;
        a->grows++; a->copied += a->len;
    }
    a->data[a->len++] = x;
}

int main(void) {
    static const size_t sizes[] = { 10, 100, 1000, 20000 };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        printf("n=%zu\n", sizes[s]);
        for (int p = 0; p < P_COUNT; p++) {
            Arr a; arr_init(&a, (Policy)p);
            unsigned long long sum = 0;
            for (size_t i = 0; i < sizes[s]; i++) arr_push(&a, (int)(i * 7 % 101));
            for (size_t i = 0; i < a.len; i++) {
                CHECK(a.data[i] == (int)(i * 7 % 101));
                sum += (unsigned)a.data[i];
            }
            size_t waste = a.cap - a.len;
            /* copied elements per push: amortized cost measured in thousandths */
            unsigned long amort = a.copied * 1000 / sizes[s];
            printf("  %-9s grows=%-6lu copied=%-9lu amort_x1000=%-6lu waste=%-6zu sum=%llu\n",
                   policy_name[p], a.grows, a.copied, amort, waste, sum);
            /* invariants of each policy */
            if (p == P_EXACT) CHECK(a.grows == sizes[s] && waste == 0);
            if (p == P_DOUBLE) CHECK(a.cap < 2 * a.len);
            if (p == P_PLUS16 && sizes[s] >= 1000) CHECK(a.grows == (sizes[s] + 15) / 16);
            free(a.data);
        }
    }
    /* doubling: total copies bounded by 2n */
    Arr d; arr_init(&d, P_DOUBLE);
    for (int i = 0; i < 100000; i++) arr_push(&d, i);
    CHECK(d.copied < 2 * d.len);
    printf("double 100000: grows=%lu copied=%lu cap=%zu\n", d.grows, d.copied, d.cap);
    free(d.data);
    return 0;
}
