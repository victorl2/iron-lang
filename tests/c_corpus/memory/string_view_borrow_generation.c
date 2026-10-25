/*
 * title: Borrowed string views vs owned strings
 * topic: memory
 * covers: string views, ptr+len slices, borrowed vs owned, dangling detection by generation, tokenizing without copies
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
    unsigned gen; /* bumped whenever buf may move or change */
} Owned;

typedef struct {
    const char *p;
    size_t n;
    const Owned *src; /* who lent us this memory */
    unsigned gen;
} View;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void owned_init(Owned *o, const char *s) {
    o->len = strlen(s);
    o->cap = o->len + 1;
    o->buf = malloc(o->cap);
    check(o->buf != NULL, "alloc");
    memcpy(o->buf, s, o->len + 1);
    o->gen = 1;
}

static void owned_append(Owned *o, const char *s) {
    size_t n = strlen(s);
    char *nb = malloc(o->len + n + 1); /* always move, like a worst-case realloc */
    check(nb != NULL, "alloc");
    memcpy(nb, o->buf, o->len);
    memcpy(nb + o->len, s, n + 1);
    memset(o->buf, '#', o->len); /* poison the old block */
    free(o->buf);
    o->buf = nb;
    o->len += n;
    o->cap = o->len + 1;
    o->gen++;
}

static View view_of(const Owned *o, size_t off, size_t n) {
    check(off + n <= o->len, "view range");
    View v = {o->buf + off, n, o, o->gen};
    return v;
}

static int view_valid(View v) { return v.src == NULL || v.src->gen == v.gen; }

/* Take the next token separated by sep from *rest; no copies. */
static View next_token(View *rest, char sep) {
    View t = *rest;
    size_t i = 0;
    while (i < rest->n && rest->p[i] != sep)
        i++;
    t.n = i;
    if (i < rest->n) {
        rest->p += i + 1;
        rest->n -= i + 1;
    } else {
        rest->p += i;
        rest->n = 0;
    }
    return t;
}

static View view_trim(View v) {
    while (v.n && v.p[0] == ' ') {
        v.p++;
        v.n--;
    }
    while (v.n && v.p[v.n - 1] == ' ')
        v.n--;
    return v;
}

static Owned view_to_owned(View v) {
    check(view_valid(v), "promote dangling view");
    Owned o;
    o.buf = malloc(v.n + 1);
    check(o.buf != NULL, "alloc");
    memcpy(o.buf, v.p, v.n);
    o.buf[v.n] = 0;
    o.len = v.n;
    o.cap = v.n + 1;
    o.gen = 1;
    return o;
}

int main(void) {
    Owned line;
    owned_init(&line, "name = iron ;  kind=lang; ver = 4.2 ;tag=alpha");
    View rest = view_of(&line, 0, line.len);
    View fields[8];
    int nf = 0;
    while (rest.n > 0)
        fields[nf++] = view_trim(next_token(&rest, ';'));
    for (int i = 0; i < nf; i++) {
        View f = fields[i];
        View kv = f;
        View k = view_trim(next_token(&kv, '='));
        View v = view_trim(kv);
        printf("field %d: key=[%.*s] value=[%.*s]\n", i, (int)k.n, k.p, (int)v.n, v.p);
    }

    Owned keeper = view_to_owned(fields[1]); /* owned copy survives */
    printf("owned copy: [%s] len=%zu\n", keeper.buf, keeper.len);

    int valid_before = 0, valid_after = 0;
    for (int i = 0; i < nf; i++)
        valid_before += view_valid(fields[i]);
    owned_append(&line, " ;extra=1");
    for (int i = 0; i < nf; i++)
        valid_after += view_valid(fields[i]);
    printf("valid views before append: %d, after: %d\n", valid_before, valid_after);
    check(valid_before == nf && valid_after == 0, "generation invalidation");

    /* re-derive views from the new buffer */
    View again = view_of(&line, 0, line.len);
    int count = 0;
    while (again.n > 0) {
        (void)next_token(&again, ';');
        count++;
    }
    printf("fields after append: %d, keeper still [%s]\n", count, keeper.buf);
    View lit = {"static text", 11, NULL, 0};
    printf("literal view valid: %d\n", view_valid(lit));
    free(line.buf);
    free(keeper.buf);
    return 0;
}
