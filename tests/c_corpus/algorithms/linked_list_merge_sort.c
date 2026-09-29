/*
 * title: Merge sort on a singly linked list
 * topic: algorithms
 * covers: linked lists, slow/fast pointer split, pointer-to-pointer tail, stable merge, dynamic allocation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int key;
    int seq;
    struct Node *next;
} Node;

static unsigned st = 31337u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Node *merge(Node *a, Node *b) {
    Node *head = NULL, **tail = &head;
    while (a && b) {
        if (b->key < a->key) {
            *tail = b;
            b = b->next;
        } else {
            *tail = a;
            a = a->next;
        }
        tail = &(*tail)->next;
    }
    *tail = a ? a : b;
    return head;
}

static Node *split_half(Node *h) {
    /* returns the second half; terminates the first */
    Node *slow = h, *fast = h->next;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    Node *second = slow->next;
    slow->next = NULL;
    return second;
}

static int depth_max;

static Node *sort(Node *h, int depth) {
    if (depth > depth_max)
        depth_max = depth;
    if (!h || !h->next)
        return h;
    Node *second = split_half(h);
    return merge(sort(h, depth + 1), sort(second, depth + 1));
}

/* natural-runs variant: iterative, merges adjacent ascending runs until one is left */
static Node *take_run(Node **rest) {
    Node *run = *rest, *p = run;
    while (p->next && p->next->key >= p->key)
        p = p->next;
    *rest = p->next;
    p->next = NULL;
    return run;
}

static Node *natural(Node *h, int *rounds) {
    *rounds = 0;
    if (!h)
        return h;
    for (;;) {
        Node *out = NULL, **tail = &out;
        int runs = 0;
        Node *rest = h;
        while (rest) {
            Node *r1 = take_run(&rest);
            Node *r2 = rest ? take_run(&rest) : NULL;
            *tail = merge(r1, r2);
            while (*tail)
                tail = &(*tail)->next;
            runs++;
        }
        h = out;
        (*rounds)++;
        if (runs == 1)
            return h;
    }
}

static Node *build(int n) {
    Node *head = NULL;
    for (int i = n - 1; i >= 0; i--) {
        Node *x = malloc(sizeof *x);
        check(x != NULL, "alloc");
        x->key = (int)(rng() % 50);
        x->seq = i;
        x->next = head;
        head = x;
    }
    return head;
}

static void verify(Node *h, int n) {
    int count = 0;
    for (Node *p = h; p; p = p->next) {
        count++;
        if (p->next) {
            check(p->key <= p->next->key, "sorted");
            if (p->key == p->next->key)
                check(p->seq < p->next->seq, "stable");
        }
    }
    check(count == n, "length preserved");
}

static void release(Node *h) {
    while (h) {
        Node *n = h->next;
        free(h);
        h = n;
    }
}

int main(void) {
    static const int sizes[] = {0, 1, 2, 5, 17, 100, 1000};
    for (int s = 0; s < 7; s++) {
        int n = sizes[s];
        Node *h = build(n);
        Node *h2 = build(0);
        (void)h2;
        depth_max = 0;
        Node *sorted = sort(h, 0);
        verify(sorted, n);
        printf("n=%-4d depth=%-2d", n, depth_max);
        if (n) {
            printf(" head=%d/%d tail:", sorted->key, sorted->seq);
            Node *p = sorted;
            while (p->next)
                p = p->next;
            printf(" %d/%d", p->key, p->seq);
        }
        printf("\n");
        release(sorted);
    }
    for (int s = 0; s < 7; s++) {
        int n = sizes[s], rounds;
        Node *h = build(n);
        Node *r = natural(h, &rounds);
        verify(r, n);
        printf("natural n=%-4d rounds=%d\n", n, rounds);
        release(r);
    }
    return 0;
}
