/*
 * title: Hierarchical allocation contexts (talloc style)
 * topic: memory
 * covers: parent-child ownership tree, free subtree, reparenting (steal), destructors, name-based reports
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Ctx Ctx;
struct Ctx {
    Ctx *parent, *first_child, *next, *prev;
    void (*dtor)(Ctx *);
    const char *name;
    size_t size;
};

static int live_ctx;
static char order[512];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *payload(Ctx *c) { return (void *)(c + 1); }

static void link_child(Ctx *parent, Ctx *c) {
    c->parent = parent;
    c->prev = NULL;
    c->next = NULL;
    if (parent) {
        c->next = parent->first_child;
        if (parent->first_child)
            parent->first_child->prev = c;
        parent->first_child = c;
    }
}

static void unlink_child(Ctx *c) {
    if (c->prev)
        c->prev->next = c->next;
    else if (c->parent)
        c->parent->first_child = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->parent = c->prev = c->next = NULL;
}

static Ctx *hdr(void *p);

static void *ctx_new(void *parent_payload, size_t size, const char *name) {
    Ctx *parent = parent_payload ? hdr(parent_payload) : NULL;
    Ctx *c = calloc(1, sizeof *c + size);
    check(c != NULL, "alloc");
    c->name = name;
    c->size = size;
    link_child(parent, c);
    live_ctx++;
    return payload(c);
}

static Ctx *hdr(void *p) { return (Ctx *)p - 1; }

static void ctx_free_c(Ctx *c) {
    /* children first (they may reference the parent in destructors), then self */
    while (c->first_child) {
        Ctx *ch = c->first_child;
        unlink_child(ch);
        ctx_free_c(ch);
    }
    if (c->dtor)
        c->dtor(c);
    strcat(order, c->name);
    strcat(order, " ");
    free(c);
    live_ctx--;
}

static void ctx_free(void *p) {
    Ctx *c = hdr(p);
    unlink_child(c);
    ctx_free_c(c);
}

static void ctx_steal(void *new_parent, void *p) {
    Ctx *c = hdr(p);
    unlink_child(c);
    link_child(new_parent ? hdr(new_parent) : NULL, c);
}

static void ctx_set_dtor(void *p, void (*d)(Ctx *)) { hdr(p)->dtor = d; }

static size_t ctx_total(Ctx *c) {
    size_t t = c->size;
    for (Ctx *ch = c->first_child; ch; ch = ch->next)
        t += ctx_total(ch);
    return t;
}

static int ctx_count(Ctx *c) {
    int n = 1;
    for (Ctx *ch = c->first_child; ch; ch = ch->next)
        n += ctx_count(ch);
    return n;
}

static void print_tree(Ctx *c, int depth) {
    /* children are listed newest first; print in reverse for creation order */
    printf("%*s%s (%zu)\n", depth * 2, "", c->name, c->size);
    Ctx *last = c->first_child;
    if (!last)
        return;
    while (last->next)
        last = last->next;
    for (Ctx *ch = last; ch; ch = ch->prev)
        print_tree(ch, depth + 1);
}

static int dtor_calls;
static void counting_dtor(Ctx *c) {
    (void)c;
    dtor_calls++;
}

int main(void) {
    char *root = ctx_new(NULL, 8, "root");
    char *conf = ctx_new(root, 32, "conf");
    char *req = ctx_new(root, 16, "request");
    char *hdrs = ctx_new(req, 64, "headers");
    char *body = ctx_new(req, 128, "body");
    char *tmp = ctx_new(body, 4, "tmp");
    char *keys = ctx_new(conf, 24, "keys");
    ctx_set_dtor(hdrs, counting_dtor);
    ctx_set_dtor(tmp, counting_dtor);
    (void)keys;
    strcpy(tmp, "abc");

    print_tree(hdr(root), 0);
    printf("total bytes %zu in %d contexts\n", ctx_total(hdr(root)), ctx_count(hdr(root)));

    /* steal the body out of the request so it outlives it */
    ctx_steal(root, body);
    printf("after steal: request subtree=%d body subtree=%d\n", ctx_count(hdr(req)),
           ctx_count(hdr(body)));
    order[0] = 0;
    ctx_free(req);
    printf("freed request: %s| live=%d dtors=%d\n", order, live_ctx, dtor_calls);
    check(strcmp(tmp, "abc") == 0, "stolen subtree intact");

    order[0] = 0;
    ctx_free(conf);
    printf("freed conf: %s| live=%d\n", order, live_ctx);
    print_tree(hdr(root), 0);

    order[0] = 0;
    ctx_free(root);
    printf("freed root: %s| live=%d dtors=%d\n", order, live_ctx, dtor_calls);
    check(live_ctx == 0 && dtor_calls == 2, "clean");
    return 0;
}
