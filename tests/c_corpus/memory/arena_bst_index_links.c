/*
 * title: Arena-allocated binary tree with 32-bit index links
 * topic: memory
 * covers: index links instead of pointers, growable arena, free list of nodes, realloc-safe references, compact serialization
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIL 0xFFFFFFFFu

typedef struct {
    int32_t key;
    uint32_t left, right; /* arena indexes */
    uint32_t height;      /* AVL height, 1 for a leaf */
} Node; /* 16 bytes */

typedef struct {
    Node *nodes;
    uint32_t used, cap;
    uint32_t free_head; /* singly linked list threaded through .left */
    uint32_t root;
    uint32_t live;
    unsigned long grows;
} Tree;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t node_new(Tree *t, int32_t key) {
    uint32_t i;
    if (t->free_head != NIL) {
        i = t->free_head;
        t->free_head = t->nodes[i].left;
    } else {
        if (t->used == t->cap) {
            /* realloc moves the array: index links stay valid, pointers would not */
            uint32_t ncap = t->cap ? t->cap * 2 : 4;
            Node *n = realloc(t->nodes, sizeof(Node) * ncap);
            if (!n)
                exit(2);
            t->nodes = n;
            t->cap = ncap;
            t->grows++;
        }
        i = t->used++;
    }
    t->nodes[i].key = key;
    t->nodes[i].left = t->nodes[i].right = NIL;
    t->nodes[i].height = 1;
    t->live++;
    return i;
}

static void node_free(Tree *t, uint32_t i) {
    t->nodes[i].left = t->free_head;
    t->free_head = i;
    t->live--;
}

static uint32_t h(const Tree *t, uint32_t i) {
    return i == NIL ? 0 : t->nodes[i].height;
}

static void fix(Tree *t, uint32_t i) {
    uint32_t hl = h(t, t->nodes[i].left), hr = h(t, t->nodes[i].right);
    t->nodes[i].height = 1 + (hl > hr ? hl : hr);
}

static uint32_t rot_right(Tree *t, uint32_t y) {
    uint32_t x = t->nodes[y].left;
    t->nodes[y].left = t->nodes[x].right;
    t->nodes[x].right = y;
    fix(t, y);
    fix(t, x);
    return x;
}

static uint32_t rot_left(Tree *t, uint32_t x) {
    uint32_t y = t->nodes[x].right;
    t->nodes[x].right = t->nodes[y].left;
    t->nodes[y].left = x;
    fix(t, x);
    fix(t, y);
    return y;
}

static uint32_t balance(Tree *t, uint32_t i) {
    fix(t, i);
    int bal = (int)h(t, t->nodes[i].left) - (int)h(t, t->nodes[i].right);
    if (bal > 1) {
        uint32_t l = t->nodes[i].left;
        if (h(t, t->nodes[l].left) < h(t, t->nodes[l].right))
            t->nodes[i].left = rot_left(t, l);
        return rot_right(t, i);
    }
    if (bal < -1) {
        uint32_t r = t->nodes[i].right;
        if (h(t, t->nodes[r].right) < h(t, t->nodes[r].left))
            t->nodes[i].right = rot_right(t, r);
        return rot_left(t, i);
    }
    return i;
}

static uint32_t insert(Tree *t, uint32_t i, int32_t key, int *added) {
    if (i == NIL) {
        *added = 1;
        return node_new(t, key);
    }
    if (key < t->nodes[i].key) {
        uint32_t c = insert(t, t->nodes[i].left, key, added);
        t->nodes[i].left = c; /* re-read the array after a possible realloc */
    } else if (key > t->nodes[i].key) {
        uint32_t c = insert(t, t->nodes[i].right, key, added);
        t->nodes[i].right = c;
    } else {
        return i;
    }
    return balance(t, i);
}

static uint32_t min_node(const Tree *t, uint32_t i) {
    while (t->nodes[i].left != NIL)
        i = t->nodes[i].left;
    return i;
}

static uint32_t erase(Tree *t, uint32_t i, int32_t key, int *removed) {
    if (i == NIL)
        return NIL;
    if (key < t->nodes[i].key) {
        t->nodes[i].left = erase(t, t->nodes[i].left, key, removed);
    } else if (key > t->nodes[i].key) {
        t->nodes[i].right = erase(t, t->nodes[i].right, key, removed);
    } else {
        *removed = 1;
        uint32_t l = t->nodes[i].left, r = t->nodes[i].right;
        if (l == NIL || r == NIL) {
            node_free(t, i);
            return l == NIL ? r : l;
        }
        uint32_t m = min_node(t, r);
        t->nodes[i].key = t->nodes[m].key;
        int dummy = 0;
        t->nodes[i].right = erase(t, r, t->nodes[m].key, &dummy);
    }
    return balance(t, i);
}

static int32_t prev_key;
static uint32_t visited;

static void inorder(const Tree *t, uint32_t i, int *sorted) {
    if (i == NIL)
        return;
    inorder(t, t->nodes[i].left, sorted);
    if (visited && t->nodes[i].key <= prev_key)
        *sorted = 0;
    prev_key = t->nodes[i].key;
    visited++;
    inorder(t, t->nodes[i].right, sorted);
}

static int avl_ok(const Tree *t, uint32_t i) {
    if (i == NIL)
        return 1;
    int bal = (int)h(t, t->nodes[i].left) - (int)h(t, t->nodes[i].right);
    if (bal < -1 || bal > 1)
        return 0;
    uint32_t hl = h(t, t->nodes[i].left), hr = h(t, t->nodes[i].right);
    if (t->nodes[i].height != 1 + (hl > hr ? hl : hr))
        return 0;
    return avl_ok(t, t->nodes[i].left) && avl_ok(t, t->nodes[i].right);
}

static uint32_t rs = 1234567;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

int main(void) {
    Tree t;
    memset(&t, 0, sizeof t);
    t.root = NIL;
    t.free_head = NIL;
    enum { RANGE = 2000 };
    static unsigned char present[RANGE];
    unsigned long added_n = 0, removed_n = 0;
    for (int step = 0; step < 6000; step++) {
        uint32_t r = rnd();
        int32_t key = (int32_t)(r % RANGE);
        int flag = 0;
        if ((r >> 16) % 3 != 0) {
            t.root = insert(&t, t.root, key, &flag);
            check(flag == !present[key], "insert result");
            present[key] = 1;
            added_n += (unsigned long)flag;
        } else {
            t.root = erase(&t, t.root, key, &flag);
            check(flag == present[key], "erase result");
            present[key] = 0;
            removed_n += (unsigned long)flag;
        }
        if (step % 500 == 0)
            check(avl_ok(&t, t.root), "avl invariant mid-run");
    }
    unsigned expect_live = 0;
    for (int k = 0; k < RANGE; k++)
        expect_live += present[k];
    int sorted = 1;
    visited = 0;
    inorder(&t, t.root, &sorted);
    check(sorted, "in-order sorted");
    check(visited == expect_live && t.live == expect_live, "count");
    check(avl_ok(&t, t.root), "avl invariant");
    printf("added=%lu removed=%lu live=%u height=%u\n", added_n, removed_n, (unsigned)t.live, (unsigned)h(&t, t.root));
    printf("arena: %u slots used of %u, grows=%lu, %zu bytes/node\n", (unsigned)t.used, (unsigned)t.cap, t.grows,
           sizeof(Node));
    unsigned free_len = 0;
    for (uint32_t i = t.free_head; i != NIL; i = t.nodes[i].left)
        free_len++;
    printf("free list length: %u (used - live = %u)\n", free_len, (unsigned)(t.used - t.live));
    check(free_len == t.used - t.live, "free list accounting");

    /* the arena is a flat array: serialize it wholesale and rebuild from the bytes */
    size_t bytes = sizeof(Node) * t.used;
    Node *copy = malloc(bytes);
    check(copy != NULL, "malloc");
    memcpy(copy, t.nodes, bytes);
    Tree c = t;
    c.nodes = copy;
    c.cap = t.used;
    uint32_t depth_sum = 0, node_count = 0;
    /* iterative walk with an explicit stack on the copy */
    uint32_t stack[64], depth[64];
    int sp = 0;
    stack[sp] = c.root;
    depth[sp++] = 1;
    while (sp) {
        sp--;
        uint32_t i = stack[sp], d = depth[sp];
        if (i == NIL)
            continue;
        depth_sum += d;
        node_count++;
        stack[sp] = c.nodes[i].left;
        depth[sp++] = d + 1;
        stack[sp] = c.nodes[i].right;
        depth[sp++] = d + 1;
    }
    check(node_count == t.live, "copy walk count");
    printf("copy walk: nodes=%u mean depth x100=%u\n", (unsigned)node_count, (unsigned)(depth_sum * 100 / node_count));
    free(copy);
    free(t.nodes);
    return 0;
}
