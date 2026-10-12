/*
 * title: Linked stack with node freelist recycling
 * topic: data_structures
 * covers: linked stack, freelist, node reuse, allocation counting, tagged values via union
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xA5A5A5A55A5A5A5AULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 9); }

typedef enum { T_INT, T_CHAR, T_PAIR } Tag;
typedef struct { Tag tag; union { long i; char c; struct { short a, b; } p; } u; } Value;

typedef struct Node { Value v; struct Node *next; } Node;
typedef struct { Node *top; Node *free_list; size_t depth, free_count; long mallocs, reuses; } Stack;

static Node *node_get(Stack *s) {
    Node *n;
    if (s->free_list) { n = s->free_list; s->free_list = n->next; s->free_count--; s->reuses++; }
    else { n = malloc(sizeof *n); CHECK(n); s->mallocs++; }
    return n;
}
static void push(Stack *s, Value v) { Node *n = node_get(s); n->v = v; n->next = s->top; s->top = n; s->depth++; }
static Value pop(Stack *s) {
    CHECK(s->top);
    Node *n = s->top; Value v = n->v;
    s->top = n->next; s->depth--;
    n->next = s->free_list; s->free_list = n; s->free_count++;
    return v;
}
static void trim_freelist(Stack *s, size_t keep) {
    while (s->free_count > keep) { Node *n = s->free_list; s->free_list = n->next; free(n); s->free_count--; }
}
static void destroy(Stack *s) {
    while (s->top) pop(s);
    trim_freelist(s, 0);
}

static Value rand_value(void) {
    Value v; memset(&v, 0, sizeof v);
    switch (rnd() % 3) {
    case 0: v.tag = T_INT; v.u.i = (long)(rnd() % 100000) - 50000; break;
    case 1: v.tag = T_CHAR; v.u.c = (char)('a' + rnd() % 26); break;
    default: v.tag = T_PAIR; v.u.p.a = (short)(rnd() % 300); v.u.p.b = (short)(rnd() % 300); break;
    }
    return v;
}
static int equal(Value a, Value b) {
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case T_INT: return a.u.i == b.u.i;
    case T_CHAR: return a.u.c == b.u.c;
    default: return a.u.p.a == b.u.p.a && a.u.p.b == b.u.p.b;
    }
}
static long score(Value v) {
    switch (v.tag) { case T_INT: return v.u.i; case T_CHAR: return v.u.c; default: return v.u.p.a * 1000L + v.u.p.b; }
}

#define MAXD 2000
int main(void) {
    Stack s = {0};
    static Value model[MAXD];
    size_t mtop = 0;
    long total = 0, popped = 0, tagcount[3] = {0};
    size_t peak = 0;
    for (int step = 0; step < 30000; step++) {
        /* phases: growth then shrink then random, to exercise the freelist */
        int phase = (step / 3000) % 3;
        unsigned pushp = phase == 0 ? 80 : phase == 1 ? 20 : 50;
        if (rnd() % 100 < pushp && mtop < MAXD) {
            Value v = rand_value(); push(&s, v); model[mtop++] = v; tagcount[v.tag]++;
        } else if (mtop) {
            Value v = pop(&s);
            CHECK(equal(v, model[--mtop]));
            total += score(v); popped++;
        }
        CHECK(s.depth == mtop);
        if (mtop > peak) peak = mtop;
        if (step % 5000 == 4999) trim_freelist(&s, 10);
    }
    Node *p = s.top;
    for (size_t i = mtop; i > 0; i--, p = p->next) CHECK(equal(p->v, model[i - 1]));
    CHECK(p == NULL);
    printf("mallocs=%ld reuses=%ld peak=%zu final_depth=%zu freelist=%zu\n", s.mallocs, s.reuses, peak, s.depth, s.free_count);
    printf("popped=%ld score_total=%ld tags int=%ld char=%ld pair=%ld\n", popped, total, tagcount[0], tagcount[1], tagcount[2]);
    CHECK(s.mallocs >= (long)peak);
    destroy(&s);
    return 0;
}
