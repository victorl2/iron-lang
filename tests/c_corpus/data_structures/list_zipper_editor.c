/*
 * title: Persistent list zipper as a text-cursor editor
 * topic: data_structures
 * covers: zipper, immutable cursor, O(1) local edits, free undo via persistence
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 13579u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Cons { char c; const struct Cons *next; } Cons;
/* left is stored nearest-first (reversed); right is stored in reading order */
typedef struct { const Cons *left, *right; int pos, len; } Zip;

static Cons **pool;
static size_t pool_n, pool_cap;
static const Cons *cons(char c, const Cons *next) {
    Cons *n = malloc(sizeof *n);
    CHECK(n);
    n->c = c; n->next = next;
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 1024;
        pool = realloc(pool, pool_cap * sizeof *pool);
        CHECK(pool);
    }
    pool[pool_n++] = n;
    return n;
}

static Zip z_empty(void) { Zip z = { NULL, NULL, 0, 0 }; return z; }
static Zip z_left(Zip z) {
    if (!z.left) return z;
    Zip n = { z.left->next, cons(z.left->c, z.right), z.pos - 1, z.len };
    return n;
}
static Zip z_right(Zip z) {
    if (!z.right) return z;
    Zip n = { cons(z.right->c, z.left), z.right->next, z.pos + 1, z.len };
    return n;
}
static Zip z_insert(Zip z, char c) { Zip n = { cons(c, z.left), z.right, z.pos + 1, z.len + 1 }; return n; }
static Zip z_backspace(Zip z) {
    if (!z.left) return z;
    Zip n = { z.left->next, z.right, z.pos - 1, z.len - 1 };
    return n;
}
static Zip z_delete(Zip z) {
    if (!z.right) return z;
    Zip n = { z.left, z.right->next, z.pos, z.len - 1 };
    return n;
}
static Zip z_replace(Zip z, char c) {
    if (!z.right) return z;
    Zip n = { z.left, cons(c, z.right->next), z.pos, z.len };
    return n;
}
static Zip z_home(Zip z) { while (z.left) z = z_left(z); return z; }
static Zip z_end(Zip z) { while (z.right) z = z_right(z); return z; }
static Zip z_goto(Zip z, int p) {
    while (z.pos > p) z = z_left(z);
    while (z.pos < p && z.right) z = z_right(z);
    return z;
}
static Zip z_from_string(const char *s) {
    Zip z = z_empty();
    for (; *s; s++) z = z_insert(z, *s);
    return z_home(z);
}
static void z_text(Zip z, char *out, size_t cap) {
    Zip h = z_home(z);
    size_t n = 0;
    for (const Cons *c = h.right; c && n + 1 < cap; c = c->next) out[n++] = c->c;
    out[n] = 0;
}
/* word-wise motion built from the primitives */
static int is_word(char c) { return c != ' ' && c != 0; }
static Zip z_word_right(Zip z) {
    while (z.right && !is_word(z.right->c)) z = z_right(z);
    while (z.right && is_word(z.right->c)) z = z_right(z);
    return z;
}
static Zip z_delete_word(Zip z) {
    while (z.right && !is_word(z.right->c)) z = z_delete(z);
    while (z.right && is_word(z.right->c)) z = z_delete(z);
    return z;
}

#define MAXT 400
int main(void) {
    /* scripted session with undo through kept versions */
    Zip v0 = z_from_string("hello world");
    Zip v1 = z_word_right(v0);
    Zip v2 = z_insert(v1, ',');
    Zip v3 = z_end(v2);
    Zip v4 = z_insert(v3, '!');
    char buf[MAXT];
    z_text(v4, buf, sizeof buf);
    printf("edited: \"%s\" cursor=%d\n", buf, v4.pos);
    z_text(v2, buf, sizeof buf);
    printf("undo to v2: \"%s\" cursor=%d\n", buf, v2.pos);
    Zip v5 = z_delete_word(z_goto(v4, 5));
    z_text(v5, buf, sizeof buf);
    printf("delete word: \"%s\" len=%d\n", buf, v5.len);
    z_text(v0, buf, sizeof buf);
    printf("original still: \"%s\"\n", buf);

    /* randomized: many versions kept, model is a char array plus cursor */
    enum { V = 8 };
    Zip zs[V];
    char model[V][MAXT];
    int mlen[V], mpos[V];
    for (int i = 0; i < V; i++) { zs[i] = z_empty(); mlen[i] = mpos[i] = 0; }
    long ops = 0;
    for (int step = 0; step < 6000; step++) {
        int s = (int)(rnd() % V), d = (int)(rnd() % V);
        unsigned op = rnd() % 12;
        char c = (char)('a' + rnd() % 26);
        Zip z = zs[s];
        char m[MAXT];
        memcpy(m, model[s], sizeof m);
        int len = mlen[s], pos = mpos[s];
        switch (op) {
        case 0: case 1: case 2:
            if (len < MAXT - 1) { z = z_insert(z, c); memmove(m + pos + 1, m + pos, (size_t)(len - pos)); m[pos++] = c; len++; }
            break;
        case 3: if (pos > 0) { z = z_backspace(z); memmove(m + pos - 1, m + pos, (size_t)(len - pos)); pos--; len--; } break;
        case 4: if (pos < len) { z = z_delete(z); memmove(m + pos, m + pos + 1, (size_t)(len - pos - 1)); len--; } break;
        case 5: if (pos < len) { z = z_replace(z, c); m[pos] = c; } break;
        case 6: case 7: if (pos > 0) { z = z_left(z); pos--; } break;
        case 8: case 9: if (pos < len) { z = z_right(z); pos++; } break;
        case 10: z = z_home(z); pos = 0; break;
        default: { int p = len ? (int)(rnd() % (unsigned)(len + 1)) : 0; z = z_goto(z, p); pos = p; break; }
        }
        zs[d] = z;
        memcpy(model[d], m, sizeof m);
        mlen[d] = len; mpos[d] = pos;
        CHECK(z.pos == pos && z.len == len);
        ops++;
        if (step % 100 == 0) {
            char t[MAXT];
            z_text(z, t, sizeof t);
            CHECK((int)strlen(t) == len && memcmp(t, m, (size_t)len) == 0);
        }
    }
    long total = 0;
    for (int i = 0; i < V; i++) {
        char t[MAXT];
        z_text(zs[i], t, sizeof t);
        CHECK((int)strlen(t) == mlen[i] && memcmp(t, model[i], (size_t)mlen[i]) == 0);
        total += mlen[i];
        CHECK((zs[i].left == NULL) == (zs[i].pos == 0));
    }
    printf("random session: %ld ops, %d versions, %ld chars total, cells allocated=%zu\n", ops, V, total, pool_n);
    for (size_t i = 0; i < pool_n; i++) free(pool[i]);
    free(pool);
    return 0;
}
