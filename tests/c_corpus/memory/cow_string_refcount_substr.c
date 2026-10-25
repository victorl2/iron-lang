/*
 * title: Copy-on-write strings with shared storage
 * topic: memory
 * covers: copy-on-write, refcounted string storage, unshare on write, append in place when unique
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int rc;
    size_t len, cap;
    char *data;
} Rep;

typedef struct { Rep *rep; } CowStr;

static int live_reps;
static int copies_made;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Rep *rep_new(const char *s, size_t n, size_t cap) {
    Rep *r = malloc(sizeof *r);
    check(r != NULL, "alloc");
    if (cap < n + 1)
        cap = n + 1;
    r->data = malloc(cap);
    check(r->data != NULL, "alloc");
    memcpy(r->data, s, n);
    r->data[n] = 0;
    r->len = n;
    r->cap = cap;
    r->rc = 1;
    live_reps++;
    return r;
}

static CowStr cs_from(const char *s) {
    CowStr c = {rep_new(s, strlen(s), 0)};
    return c;
}

static CowStr cs_dup(CowStr s) {
    s.rep->rc++;
    return s;
}

static void cs_free(CowStr *s) {
    if (s->rep && --s->rep->rc == 0) {
        free(s->rep->data);
        free(s->rep);
        live_reps--;
    }
    s->rep = NULL;
}

/* Make storage unique before writing. */
static void cs_unshare(CowStr *s) {
    if (s->rep->rc > 1) {
        Rep *n = rep_new(s->rep->data, s->rep->len, s->rep->cap);
        s->rep->rc--;
        s->rep = n;
        copies_made++;
    }
}

static void cs_set(CowStr *s, size_t i, char c) {
    check(i < s->rep->len, "index");
    cs_unshare(s);
    s->rep->data[i] = c;
}

static void cs_append(CowStr *s, const char *t) {
    size_t tl = strlen(t);
    cs_unshare(s);
    Rep *r = s->rep;
    if (r->len + tl + 1 > r->cap) {
        size_t nc = r->cap * 2;
        while (nc < r->len + tl + 1)
            nc *= 2;
        r->data = realloc(r->data, nc);
        check(r->data != NULL, "realloc");
        r->cap = nc;
    }
    memcpy(r->data + r->len, t, tl + 1);
    r->len += tl;
}

static void cs_upper(CowStr *s) {
    /* only unshare if something would actually change */
    for (size_t i = 0; i < s->rep->len; i++) {
        char c = s->rep->data[i];
        if (c >= 'a' && c <= 'z') {
            cs_unshare(s);
            s->rep->data[i] = (char)(c - 32);
        }
    }
}

int main(void) {
    CowStr a = cs_from("hello world");
    CowStr b = cs_dup(a);
    CowStr c = cs_dup(a);
    printf("rc=%d live=%d copies=%d\n", a.rep->rc, live_reps, copies_made);

    cs_set(&b, 0, 'J');
    printf("a=%s b=%s c=%s\n", a.rep->data, b.rep->data, c.rep->data);
    printf("rc a=%d b=%d live=%d copies=%d\n", a.rep->rc, b.rep->rc, live_reps, copies_made);

    cs_set(&b, 1, 'E'); /* already unique: no new copy */
    check(copies_made == 1, "unique write must not copy");

    cs_append(&c, "!!!");
    printf("a=%s c=%s copies=%d\n", a.rep->data, c.rep->data, copies_made);

    CowStr d = cs_from("ALREADY UPPER 123");
    CowStr e = cs_dup(d);
    cs_upper(&e);
    check(e.rep == d.rep, "no-op upper shares");
    cs_upper(&a);
    printf("a upper=%s, c=%s\n", a.rep->data, c.rep->data);
    printf("d rc=%d, live=%d, copies=%d\n", d.rep->rc, live_reps, copies_made);

    /* build a long string by appends, checking growth keeps content */
    CowStr g = cs_from("");
    CowStr snap[4];
    for (int i = 0; i < 40; i++) {
        char piece[8];
        snprintf(piece, sizeof piece, "%d,", i);
        cs_append(&g, piece);
        if (i % 10 == 9)
            snap[i / 10] = cs_dup(g);
    }
    for (int i = 0; i < 4; i++)
        printf("snap%d len=%zu tail=%s\n", i, snap[i].rep->len,
               snap[i].rep->data + snap[i].rep->len - 4);
    check(g.rep->len == strlen(g.rep->data), "len consistent");
    printf("final len=%zu copies=%d\n", g.rep->len, copies_made);
    for (int i = 0; i < 4; i++)
        cs_free(&snap[i]);
    cs_free(&g);
    cs_free(&a); cs_free(&b); cs_free(&c); cs_free(&d); cs_free(&e);
    check(live_reps == 0, "leak");
    return 0;
}
