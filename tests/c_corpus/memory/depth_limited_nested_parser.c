/*
 * title: Depth-limited nested list parser with iterative teardown
 * topic: memory
 * covers: untrusted nesting depth, recursion limits, node budget, rollback on parse error, iterative free, hostile inputs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    int is_list;
    int value;
    struct Node *child;  /* first child (list) */
    struct Node *next;   /* sibling */
} Node;

enum { E_OK, E_SYNTAX, E_DEPTH, E_BUDGET, E_TRAIL };
static const char *en[] = {"ok", "syntax", "too-deep", "node-budget", "trailing"};

typedef struct { const char *s; size_t n, pos; int max_depth; int budget; int maxseen; long live; } Parser;

static Node *mk(Parser *p, int is_list, int value) {
    if (p->budget <= 0) return NULL;
    Node *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    p->budget--; p->live++;
    x->is_list = is_list; x->value = value;
    return x;
}

/* iterative destroy: walks with an explicit parent chain stored in the tree (child pointers get threaded) */
static void destroy(Parser *p, Node *root) {
    Node *cur = root;
    while (cur) {
        if (cur->child) {
            Node *c = cur->child;
            cur->child = NULL;
            /* push cur below c: reuse c's tail sibling? simpler: prepend cur to c's sibling chain end via a stack */
            Node *tail = c;
            while (tail->next) tail = tail->next;
            tail->next = cur->next;
            cur->next = NULL;
            free(cur); p->live--;
            cur = c;
        } else {
            Node *nx = cur->next;
            free(cur); p->live--;
            cur = nx;
        }
    }
}

static int parse_list(Parser *p, int depth, Node **out);

static int parse_item(Parser *p, int depth, Node **out) {
    if (p->pos >= p->n) return E_SYNTAX;
    char c = p->s[p->pos];
    if (c == '(') return parse_list(p, depth + 1, out);
    if (c >= '0' && c <= '9') {
        int v = 0;
        while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') { v = (v * 10 + (p->s[p->pos] - '0')) % 100000; p->pos++; }
        *out = mk(p, 0, v);
        return *out ? E_OK : E_BUDGET;
    }
    return E_SYNTAX;
}

static int parse_list(Parser *p, int depth, Node **out) {
    if (depth > p->max_depth) return E_DEPTH;
    if (depth > p->maxseen) p->maxseen = depth;
    p->pos++; /* '(' */
    Node *self = mk(p, 1, 0);
    if (!self) return E_BUDGET;
    Node **tail = &self->child;
    while (1) {
        while (p->pos < p->n && p->s[p->pos] == ' ') p->pos++;
        if (p->pos >= p->n) { destroy(p, self); return E_SYNTAX; }
        if (p->s[p->pos] == ')') { p->pos++; break; }
        Node *item;
        int e = parse_item(p, depth, &item);
        if (e) { destroy(p, self); return e; }
        *tail = item;
        tail = &item->next;
    }
    *out = self;
    return E_OK;
}

static int count_leaves(const Node *n) { /* iterative over tree using explicit stack */
    const Node *stack[512]; int sp = 0, total = 0;
    stack[sp++] = n;
    while (sp) {
        const Node *x = stack[--sp];
        for (; x; x = x->next) {
            if (x->is_list) { if (sp < 512) stack[sp++] = x->child; }
            else total++;
        }
    }
    return total;
}

static int try_parse(const char *s, size_t n, int max_depth, int budget, int *leaves, int *maxseen, long *live_after) {
    Parser p = {s, n, 0, max_depth, budget, 0, 0};
    Node *root = NULL;
    while (p.pos < n && s[p.pos] == ' ') p.pos++;
    int e = p.pos < n && s[p.pos] == '(' ? parse_list(&p, 1, &root) : E_SYNTAX;
    if (e == E_OK) {
        while (p.pos < n && s[p.pos] == ' ') p.pos++;
        if (p.pos != n) e = E_TRAIL;
    }
    *leaves = (e == E_OK) ? count_leaves(root) : -1;
    if (root) destroy(&p, root);
    *maxseen = p.maxseen;
    *live_after = p.live;
    return e;
}

int main(void) {
    struct { const char *label; const char *s; } t[] = {
        {"flat", "(1 2 3 4 5)"}, {"nested", "(1 (2 (3 (4 5)) 6) 7 (8))"}, {"empty", "()"},
        {"unclosed", "(1 (2 3)"}, {"junk", "(1 x 2)"}, {"trailing", "(1 2) 3"}, {"not a list", "42"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int lv, ms; long live;
        int e = try_parse(t[i].s, strlen(t[i].s), 16, 1000, &lv, &ms, &live);
        printf("%-10s -> %-8s leaves=%d maxdepth=%d leaked=%ld\n", t[i].label, en[e], lv, ms, live);
        if (live) return 1;
    }
    /* hostile: 100000 opening parens must hit the depth limit long before the C stack matters */
    size_t n = 100000;
    char *bomb = malloc(n + 1);
    if (!bomb) return 1;
    memset(bomb, '(', n); bomb[n] = '\0';
    int lv, ms; long live;
    int e = try_parse(bomb, n, 64, 1000000, &lv, &ms, &live);
    printf("paren bomb -> %s maxdepth=%d leaked=%ld\n", en[e], ms, live);
    if (live) return 1;
    /* depth exactly at the limit is fine, one more is refused */
    char deep[200];
    for (int d = 60; d <= 66; d += 3) {
        size_t k = 0;
        for (int i = 0; i < d; i++) deep[k++] = '(';
        deep[k++] = '7';
        for (int i = 0; i < d; i++) deep[k++] = ')';
        e = try_parse(deep, k, 64, 1000, &lv, &ms, &live);
        printf("depth %d with limit 64 -> %s leaves=%d\n", d, en[e], lv);
        if (live) return 1;
    }
    /* node budget: wide list of 500 numbers with budget 100 */
    char wide[4000];
    size_t k = 0;
    wide[k++] = '(';
    for (int i = 0; i < 500; i++) k += (size_t)snprintf(wide + k, sizeof wide - k, "%d ", i);
    wide[k++] = ')';
    e = try_parse(wide, k, 8, 100, &lv, &ms, &live);
    printf("wide list, budget 100 -> %s leaked=%ld\n", en[e], live);
    e = try_parse(wide, k, 8, 501, &lv, &ms, &live);
    printf("wide list, budget 501 -> %s leaves=%d\n", en[e], lv);
    free(bomb);
    return live == 0 ? 0 : 1;
}
