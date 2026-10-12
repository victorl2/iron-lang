/*
 * title: Self-relative offset pointers in a relocatable buffer
 * topic: memory
 * covers: position-independent data, relative pointers, memcpy relocation, in-place insert after move
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A relative pointer stores the distance from its own address to the target,
 * so a block containing both survives being copied anywhere. Zero means NULL.
 */
typedef struct {
    int32_t off;
} RelPtr;

static void *rel_get(const RelPtr *r) {
    return r->off == 0 ? NULL : (void *)((char *)r + r->off);
}

static void rel_set(RelPtr *r, void *target) {
    r->off = target == NULL ? 0 : (int32_t)((char *)target - (char *)r);
}

typedef struct Node {
    int32_t key;
    int32_t count;
    RelPtr left, right;
    RelPtr name; /* points at a NUL-terminated string in the same block */
} Node;

typedef struct {
    RelPtr root;
    uint32_t used;
    uint32_t cap;
    /* payload follows */
} Block;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *block_take(Block *b, size_t n, size_t align) {
    uint32_t off = (uint32_t)((b->used + align - 1) / align * align);
    if (off + n > b->cap)
        return NULL;
    b->used = (uint32_t)(off + n);
    return (char *)b + off;
}

static int insert(Block *b, int32_t key, const char *name) {
    RelPtr *link = &b->root;
    while (rel_get(link)) {
        Node *n = rel_get(link);
        if (key == n->key) {
            n->count++;
            return 0;
        }
        link = key < n->key ? &n->left : &n->right;
    }
    Node *n = block_take(b, sizeof(Node), 4);
    size_t len = strlen(name) + 1;
    char *s = block_take(b, len, 1);
    if (!n || !s)
        return -1;
    n->key = key;
    n->count = 1;
    n->left.off = n->right.off = 0;
    memcpy(s, name, len);
    rel_set(&n->name, s);
    rel_set(link, n);
    return 1;
}

static void inorder(const Node *n, int depth, long *sum, int *maxd, int *cnt, char *out, size_t *pos, size_t cap) {
    if (!n)
        return;
    inorder(rel_get(&n->left), depth + 1, sum, maxd, cnt, out, pos, cap);
    *sum += (long)n->key * n->count;
    (*cnt)++;
    if (depth > *maxd)
        *maxd = depth;
    const char *nm = rel_get(&n->name);
    if (*pos < cap && *cnt <= 6)
        *pos += (size_t)snprintf(out + *pos, cap - *pos, "%s(%d)x%d ", nm, (int)n->key, (int)n->count);
    inorder(rel_get(&n->right), depth + 1, sum, maxd, cnt, out, pos, cap);
}

static void report(const char *label, Block *b) {
    long sum = 0;
    int maxd = 0, cnt = 0;
    char line[200];
    size_t pos = 0;
    line[0] = 0;
    inorder(rel_get(&b->root), 0, &sum, &maxd, &cnt, line, &pos, sizeof line);
    printf("%s: nodes=%d weighted-sum=%ld depth=%d used=%u\n  first: %s\n", label, cnt, sum, maxd,
           (unsigned)b->used, line);
}

int main(void) {
    enum { CAP = 4096 };
    Block *a = calloc(1, CAP);
    check(a != NULL, "calloc");
    a->used = sizeof(Block);
    a->cap = CAP;

    static const char *names[] = {"ant", "bee", "cat", "dog", "eel", "fox", "gnu", "hen"};
    uint32_t x = 7;
    int inserted = 0, dups = 0;
    for (int i = 0; i < 60; i++) {
        x = x * 1103515245u + 12345u;
        int32_t key = (int32_t)((x >> 16) % 40);
        int r = insert(a, key, names[(x >> 8) & 7]);
        check(r >= 0, "insert");
        if (r)
            inserted++;
        else
            dups++;
    }
    printf("inserted=%d duplicates=%d\n", inserted, dups);
    report("original", a);

    /* relocate: copy raw bytes to a new heap block, and to a shifted position in a static buffer */
    Block *b = malloc(CAP);
    check(b != NULL, "malloc");
    memcpy(b, a, CAP);
    free(a);
    report("moved-to-heap", b);

    static _Alignas(16) unsigned char stage[CAP + 64];
    Block *c = (Block *)(stage + 40);
    memcpy(c, b, b->cap);
    memset(b, 0xEE, CAP); /* wipe the old home: nothing may still point there */
    free(b);
    report("moved-shifted", c);

    /* keep mutating after the move */
    check(insert(c, 100, "zed") == 1, "insert after move");
    check(insert(c, 100, "zed") == 0, "duplicate after move");
    report("after-insert", c);
    check(insert(c, 101, "a-very-long-name-that-eats-space-a-very-long-name-that-eats-space") >= 0, "long");
    return 0;
}
