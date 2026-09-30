/*
 * title: Refcounted string interner with eviction
 * topic: memory
 * covers: interning, symbol table with reference counts, eviction at zero, open addressing with tombstones, stable symbol ids
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TBL 32

typedef struct {
    char *str;
    unsigned hash;
    int refs;
    unsigned id;
} Sym;

static Sym *table[TBL];
static Sym tomb;
static int live_syms;
static unsigned next_id = 1;
static int evictions;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned fnv(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static Sym *intern(const char *s) {
    unsigned h = fnv(s);
    int first_tomb = -1;
    for (unsigned i = 0; i < TBL; i++) {
        unsigned k = (h + i) % TBL;
        Sym *e = table[k];
        if (e == NULL) {
            if (first_tomb >= 0)
                k = (unsigned)first_tomb;
            Sym *n = malloc(sizeof *n);
            check(n != NULL, "alloc");
            n->str = malloc(strlen(s) + 1);
            check(n->str != NULL, "alloc");
            strcpy(n->str, s);
            n->hash = h;
            n->refs = 1;
            n->id = next_id++;
            table[k] = n;
            live_syms++;
            return n;
        }
        if (e == &tomb) {
            if (first_tomb < 0)
                first_tomb = (int)k;
            continue;
        }
        if (e->hash == h && strcmp(e->str, s) == 0) {
            e->refs++;
            return e;
        }
    }
    if (first_tomb >= 0) {
        Sym *n = malloc(sizeof *n);
        check(n != NULL, "alloc");
        n->str = malloc(strlen(s) + 1);
        check(n->str != NULL, "alloc");
        strcpy(n->str, s);
        n->hash = h;
        n->refs = 1;
        n->id = next_id++;
        table[first_tomb] = n;
        live_syms++;
        return n;
    }
    check(0, "table full");
    return NULL;
}

static void sym_release(Sym *s) {
    check(s->refs > 0, "release underflow");
    if (--s->refs > 0)
        return;
    for (int i = 0; i < TBL; i++)
        if (table[i] == s) {
            table[i] = &tomb;
            break;
        }
    free(s->str);
    free(s);
    live_syms--;
    evictions++;
}

int main(void) {
    const char *words[] = {"if", "else", "while", "return", "x", "y", "count", "if", "x", "x",
                           "else", "tmp", "if", "y", "return"};
    int n = (int)(sizeof words / sizeof words[0]);
    Sym *syms[16];
    for (int i = 0; i < n; i++)
        syms[i] = intern(words[i]);

    printf("distinct symbols: %d of %d words\n", live_syms, n);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            check((syms[i] == syms[j]) == (strcmp(words[i], words[j]) == 0), "identity == equality");
    printf("ids:");
    for (int i = 0; i < 7; i++)
        printf(" %s=%u(rc%d)", syms[i]->str, syms[i]->id, syms[i]->refs);
    printf("\n");

    /* release every reference to "x": it is evicted and leaves a tombstone */
    for (int i = 0; i < n; i++)
        if (strcmp(words[i], "x") == 0)
            sym_release(syms[i]);
    printf("after releasing x: live=%d evictions=%d\n", live_syms, evictions);

    /* probing across tombstones still finds later entries */
    Sym *again = intern("y");
    check(again == syms[5], "y found through tombstones");
    printf("y still shared: rc=%d\n", again->refs);
    sym_release(again);

    /* a new symbol may reuse the tombstone slot but gets a fresh id */
    Sym *z = intern("x");
    printf("x re-interned with id %u (previous id 5)\n", z->id);
    check(z->id != 5, "fresh id");

    /* drop everything else */
    for (int i = 0; i < n; i++)
        if (strcmp(words[i], "x") != 0)
            sym_release(syms[i]);
    sym_release(z);
    printf("final live=%d evictions=%d\n", live_syms, evictions);
    check(live_syms == 0, "leak");
    return 0;
}
