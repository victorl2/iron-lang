/*
 * title: Destroying possibly-cyclic or shared lists without double free
 * topic: memory
 * covers: Floyd cycle detection before free, visited marking, shared tails, ownership by unique ids, bounded traversal
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct N { int id; int mark; struct N *next; } N;

static int nodes_live, double_frees_avoided;

static N *mk(int id) {
    N *n = malloc(sizeof *n);
    if (!n) exit(1);
    n->id = id; n->mark = 0; n->next = NULL;
    nodes_live++;
    return n;
}

static N *build_line(int start, int count) {
    N *head = NULL, **t = &head;
    for (int i = 0; i < count; i++) { *t = mk(start + i); t = &(*t)->next; }
    return head;
}

/* Floyd: returns length of cycle (0 if none) and the index at which it starts via *mu */
static int find_cycle(N *head, int *mu) {
    N *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next; fast = fast->next->next;
        if (slow == fast) {
            int lam = 1;
            for (N *p = slow->next; p != slow; p = p->next) lam++;
            slow = fast = head;
            for (int i = 0; i < lam; i++) fast = fast->next;
            int m = 0;
            while (slow != fast) { slow = slow->next; fast = fast->next; m++; }
            *mu = m;
            return lam;
        }
    }
    *mu = -1;
    return 0;
}

static int list_len(N *head, int cap) { int n = 0; while (head && n < cap) { n++; head = head->next; } return n; }

/* Safe destroy: mark while walking; stop at the first marked node (cycle or shared tail already freed/being freed). */
static int safe_destroy(N *head) {
    /* pass 1: mark and collect */
    N *cur = head;
    N *order[256];
    int cnt = 0;
    while (cur && !cur->mark) { cur->mark = 1; order[cnt++] = cur; cur = cur->next; }
    if (cur && cur->mark) double_frees_avoided++;
    for (int i = 0; i < cnt; i++) { free(order[i]); nodes_live--; }
    return cnt;
}

int main(void) {
    /* acyclic */
    N *a = build_line(1, 5);
    int mu;
    printf("acyclic: cycle=%d len=%d\n", find_cycle(a, &mu), list_len(a, 100));
    int c = safe_destroy(a);
    printf("destroyed %d nodes live=%d\n", c, nodes_live);

    /* full cycle (tail points to head) */
    N *b = build_line(10, 6);
    N *t = b; while (t->next) t = t->next;
    t->next = b;
    int lam = find_cycle(b, &mu);
    printf("ring: cycle length=%d starts at index %d, bounded len=%d\n", lam, mu, list_len(b, 50));
    c = safe_destroy(b);
    printf("destroyed %d nodes live=%d avoided=%d\n", c, nodes_live, double_frees_avoided);

    /* rho shape: tail of 4 then loop of 3 */
    N *r = build_line(20, 7);
    t = r; while (t->next) t = t->next;
    N *loop_start = r; for (int i = 0; i < 4; i++) loop_start = loop_start->next;
    t->next = loop_start;
    lam = find_cycle(r, &mu);
    printf("rho: cycle length=%d starts at index %d\n", lam, mu);
    c = safe_destroy(r);
    printf("destroyed %d nodes live=%d avoided=%d\n", c, nodes_live, double_frees_avoided);

    /* two heads sharing a tail: free list 1 fully, then list 2 up to the shared part must not touch freed nodes */
    N *tail = build_line(30, 3);
    N *h1 = build_line(40, 2), *h2 = build_line(50, 3);
    t = h1; while (t->next) t = t->next; t->next = tail;
    t = h2; while (t->next) t = t->next; t->next = tail;
    printf("shared tail lens: %d %d\n", list_len(h1, 50), list_len(h2, 50));
    /* strategy: mark-and-collect over both heads together */
    N *heads[2] = {h1, h2};
    N *all[64]; int na = 0;
    for (int i = 0; i < 2; i++)
        for (N *p = heads[i]; p && !p->mark; p = p->next) { p->mark = 1; all[na++] = p; }
    for (int i = 0; i < na; i++) { free(all[i]); nodes_live--; }
    printf("freed %d unique nodes across two heads, live=%d\n", na, nodes_live);
    return nodes_live == 0 ? 0 : 1;
}
