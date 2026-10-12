/*
 * title: Fixed-capacity array stack with error codes
 * topic: data_structures
 * covers: array stack, overflow/underflow error paths, status enums, peek, bulk ops, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xFEEDFACE12345ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 12); }

typedef enum { ST_OK, ST_FULL, ST_EMPTY, ST_RANGE } Status;
static const char *status_name(Status s) {
    switch (s) { case ST_OK: return "ok"; case ST_FULL: return "full"; case ST_EMPTY: return "empty"; case ST_RANGE: return "range"; }
    return "?";
}

#define CAP 16
typedef struct { int items[CAP]; int top; } Stack;

static void st_init(Stack *s) { s->top = 0; }
static Status st_push(Stack *s, int v) { if (s->top == CAP) return ST_FULL; s->items[s->top++] = v; return ST_OK; }
static Status st_pop(Stack *s, int *out) { if (!s->top) return ST_EMPTY; *out = s->items[--s->top]; return ST_OK; }
static Status st_peek(const Stack *s, int *out) { if (!s->top) return ST_EMPTY; *out = s->items[s->top - 1]; return ST_OK; }
/* look n entries below the top (0 = top) */
static Status st_peek_at(const Stack *s, int depth, int *out) {
    if (depth < 0 || depth >= s->top) return ST_RANGE;
    *out = s->items[s->top - 1 - depth]; return ST_OK;
}
static Status st_dup(Stack *s) { int v; Status r = st_peek(s, &v); return r != ST_OK ? r : st_push(s, v); }
static Status st_swap(Stack *s) {
    if (s->top < 2) return ST_EMPTY;
    int t = s->items[s->top - 1]; s->items[s->top - 1] = s->items[s->top - 2]; s->items[s->top - 2] = t;
    return ST_OK;
}
/* rotate the top n items: the deepest of them moves to the top */
static Status st_roll(Stack *s, int n) {
    if (n < 0 || n > s->top) return ST_RANGE;
    if (n < 2) return ST_OK;
    int *base = s->items + s->top - n; int first = base[0];
    memmove(base, base + 1, (size_t)(n - 1) * sizeof(int));
    base[n - 1] = first;
    return ST_OK;
}
static Status st_pop_n(Stack *s, int n) { if (n > s->top) return ST_RANGE; s->top -= n; return ST_OK; }

int main(void) {
    Stack s; st_init(&s);
    int model[CAP + 4], mtop = 0;
    long hist[4][4] = {{0}}; /* [op][status] */
    static const char *opname[] = { "push", "pop", "peek", "peek_at", "dup", "swap", "roll", "pop_n" };
    long stats[8][4] = {{0}};
    (void)hist;
    for (int step = 0; step < 20000; step++) {
        int op = (int)(rnd() % 8);
        int v = (int)(rnd() % 1000);
        int n = (int)(rnd() % 6);
        /* bias: half the time push when low, pop when high to visit both extremes */
        if (mtop == 0 && rnd() % 2) op = 0;
        if (mtop == CAP && rnd() % 2) op = 1;
        Status got = ST_OK, want = ST_OK; int out = -1, wout = -1;
        switch (op) {
        case 0: got = st_push(&s, v); if (mtop == CAP) want = ST_FULL; else model[mtop++] = v; break;
        case 1: got = st_pop(&s, &out); if (!mtop) want = ST_EMPTY; else wout = model[--mtop]; break;
        case 2: got = st_peek(&s, &out); if (!mtop) want = ST_EMPTY; else wout = model[mtop - 1]; break;
        case 3: got = st_peek_at(&s, n, &out); if (n >= mtop) want = ST_RANGE; else wout = model[mtop - 1 - n]; break;
        case 4: got = st_dup(&s); if (!mtop) want = ST_EMPTY; else if (mtop == CAP) want = ST_FULL; else { model[mtop] = model[mtop - 1]; mtop++; } break;
        case 5: got = st_swap(&s); if (mtop < 2) want = ST_EMPTY; else { int t = model[mtop-1]; model[mtop-1] = model[mtop-2]; model[mtop-2] = t; } break;
        case 6: got = st_roll(&s, n); if (n > mtop) want = ST_RANGE; else if (n >= 2) { int f = model[mtop - n]; for (int i = mtop - n; i + 1 < mtop; i++) model[i] = model[i + 1]; model[mtop - 1] = f; } break;
        default: got = st_pop_n(&s, n); if (n > mtop) want = ST_RANGE; else mtop -= n; break;
        }
        CHECK(got == want);
        if (got == ST_OK && (op == 1 || op == 2 || op == 3)) CHECK(out == wout);
        stats[op][got]++;
        CHECK(s.top == mtop);
        CHECK(memcmp(s.items, model, (size_t)mtop * sizeof(int)) == 0);
    }
    for (int op = 0; op < 8; op++) {
        printf("%-8s", opname[op]);
        for (int st = 0; st < 4; st++) printf(" %s=%ld", status_name((Status)st), stats[op][st]);
        printf("\n");
    }
    printf("final depth=%d\n", s.top);
    return 0;
}
