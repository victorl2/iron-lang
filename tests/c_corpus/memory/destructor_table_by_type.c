/*
 * title: Destructor table indexed by type id
 * topic: memory
 * covers: type-tagged objects, per-type destructor and clone tables, function pointer dispatch, nested owned members
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Type { T_INT, T_STR, T_LIST, T_PAIR, T_COUNT };

typedef struct Obj Obj;
struct Obj {
    enum Type type;
    union {
        long i;
        char *s;
        struct { Obj **items; int n; } list;
        struct { Obj *a, *b; } pair;
    } u;
};

static int live[T_COUNT];
static int destroyed[T_COUNT];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void obj_free(Obj *o);
static Obj *obj_clone(const Obj *o);

static void d_int(Obj *o) { (void)o; }
static void d_str(Obj *o) { free(o->u.s); }
static void d_list(Obj *o) {
    for (int i = 0; i < o->u.list.n; i++)
        obj_free(o->u.list.items[i]);
    free(o->u.list.items);
}
static void d_pair(Obj *o) {
    obj_free(o->u.pair.a);
    obj_free(o->u.pair.b);
}

static Obj *alloc(enum Type t) {
    Obj *o = calloc(1, sizeof *o);
    check(o != NULL, "alloc");
    o->type = t;
    live[t]++;
    return o;
}

static Obj *c_int(const Obj *o) {
    Obj *n = alloc(T_INT);
    n->u.i = o->u.i;
    return n;
}
static Obj *c_str(const Obj *o) {
    Obj *n = alloc(T_STR);
    n->u.s = malloc(strlen(o->u.s) + 1);
    check(n->u.s != NULL, "alloc");
    strcpy(n->u.s, o->u.s);
    return n;
}
static Obj *c_list(const Obj *o) {
    Obj *n = alloc(T_LIST);
    n->u.list.n = o->u.list.n;
    n->u.list.items = malloc((size_t)o->u.list.n * sizeof(Obj *) + 1);
    for (int i = 0; i < o->u.list.n; i++)
        n->u.list.items[i] = obj_clone(o->u.list.items[i]);
    return n;
}
static Obj *c_pair(const Obj *o) {
    Obj *n = alloc(T_PAIR);
    n->u.pair.a = obj_clone(o->u.pair.a);
    n->u.pair.b = obj_clone(o->u.pair.b);
    return n;
}

static const struct {
    const char *name;
    void (*dtor)(Obj *);
    Obj *(*clone)(const Obj *);
} vtab[T_COUNT] = {
    {"int", d_int, c_int}, {"str", d_str, c_str}, {"list", d_list, c_list}, {"pair", d_pair, c_pair}};

static void obj_free(Obj *o) {
    if (!o)
        return;
    vtab[o->type].dtor(o);
    destroyed[o->type]++;
    live[o->type]--;
    free(o);
}
static Obj *obj_clone(const Obj *o) { return vtab[o->type].clone(o); }

static Obj *mk_int(long v) {
    Obj *o = alloc(T_INT);
    o->u.i = v;
    return o;
}
static Obj *mk_str(const char *s) {
    Obj *o = alloc(T_STR);
    o->u.s = malloc(strlen(s) + 1);
    check(o->u.s != NULL, "alloc");
    strcpy(o->u.s, s);
    return o;
}
static Obj *mk_pair(Obj *a, Obj *b) {
    Obj *o = alloc(T_PAIR);
    o->u.pair.a = a;
    o->u.pair.b = b;
    return o;
}
static Obj *mk_list(int n, Obj **items) {
    Obj *o = alloc(T_LIST);
    o->u.list.items = malloc((size_t)n * sizeof(Obj *) + 1);
    check(o->u.list.items != NULL, "alloc");
    memcpy(o->u.list.items, items, (size_t)n * sizeof(Obj *));
    o->u.list.n = n;
    return o;
}

static void show(const Obj *o) {
    switch (o->type) {
    case T_INT: printf("%ld", o->u.i); break;
    case T_STR: printf("\"%s\"", o->u.s); break;
    case T_PAIR:
        printf("(");
        show(o->u.pair.a);
        printf(" . ");
        show(o->u.pair.b);
        printf(")");
        break;
    case T_LIST:
        printf("[");
        for (int i = 0; i < o->u.list.n; i++) {
            if (i)
                printf(", ");
            show(o->u.list.items[i]);
        }
        printf("]");
        break;
    default: break;
    }
}

static void report(const char *when) {
    printf("%s:", when);
    for (int t = 0; t < T_COUNT; t++)
        printf(" %s=%d/%d", vtab[t].name, live[t], destroyed[t]);
    printf("\n");
}

int main(void) {
    Obj *items[4] = {mk_int(7), mk_str("iron"), mk_pair(mk_int(1), mk_str("x")), NULL};
    Obj *inner[2] = {mk_int(-5), mk_str("deep")};
    items[3] = mk_list(2, inner);
    Obj *root = mk_list(4, items);
    show(root);
    printf("\n");
    report("built");

    Obj *copy = obj_clone(root);
    copy->u.list.items[0]->u.i = 70;
    copy->u.list.items[3]->u.list.items[1]->u.s[0] = 'D';
    show(root);
    printf("\n");
    show(copy);
    printf("\n");
    report("cloned");
    obj_free(root);
    report("root freed");
    check(live[T_INT] == 3 && live[T_STR] == 3 && live[T_PAIR] == 1 && live[T_LIST] == 2, "copy alive");
    obj_free(copy);
    report("copy freed");
    for (int t = 0; t < T_COUNT; t++)
        check(live[t] == 0, "leak");
    return 0;
}
