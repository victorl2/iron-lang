/*
 * title: Undo/redo stack owning memento snapshots
 * topic: memory
 * covers: memento ownership, bounded history with eviction of oldest, redo truncation on new edit, snapshot deep copies
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST_MAX 5

typedef struct {
    char *text;
    int cursor;
} Memento;

typedef struct {
    char *text;
    size_t len;
    int cursor;
    Memento *hist[HIST_MAX + 1];
    int nhist;  /* number of stored states */
    int pos;    /* index of the current state in hist */
    int evicted;
} Editor;

static int live_mementos;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Memento *snap(const Editor *e) {
    Memento *m = malloc(sizeof *m);
    check(m != NULL, "alloc");
    m->text = malloc(e->len + 1);
    check(m->text != NULL, "alloc");
    memcpy(m->text, e->text, e->len + 1);
    m->cursor = e->cursor;
    live_mementos++;
    return m;
}

static void memento_free(Memento *m) {
    free(m->text);
    free(m);
    live_mementos--;
}

static void restore(Editor *e, const Memento *m) {
    size_t n = strlen(m->text);
    e->text = realloc(e->text, n + 1);
    check(e->text != NULL, "realloc");
    memcpy(e->text, m->text, n + 1);
    e->len = n;
    e->cursor = m->cursor;
}

static void commit(Editor *e) {
    /* a new edit discards the redo tail */
    while (e->nhist > e->pos + 1)
        memento_free(e->hist[--e->nhist]);
    e->hist[e->nhist++] = snap(e);
    e->pos = e->nhist - 1;
    if (e->nhist > HIST_MAX) { /* evict the oldest state */
        memento_free(e->hist[0]);
        memmove(&e->hist[0], &e->hist[1], (size_t)(e->nhist - 1) * sizeof(Memento *));
        e->nhist--;
        e->pos--;
        e->evicted++;
    }
}

static void insert(Editor *e, const char *s) {
    size_t n = strlen(s);
    e->text = realloc(e->text, e->len + n + 1);
    check(e->text != NULL, "realloc");
    memmove(e->text + e->cursor + n, e->text + e->cursor, e->len - (size_t)e->cursor + 1);
    memcpy(e->text + e->cursor, s, n);
    e->len += n;
    e->cursor += (int)n;
    commit(e);
}

static void delete_back(Editor *e, int n) {
    if (n > e->cursor)
        n = e->cursor;
    memmove(e->text + e->cursor - n, e->text + e->cursor, e->len - (size_t)e->cursor + 1);
    e->len -= (size_t)n;
    e->cursor -= n;
    commit(e);
}

static int undo(Editor *e) {
    if (e->pos == 0)
        return 0;
    restore(e, e->hist[--e->pos]);
    return 1;
}

static int redo(Editor *e) {
    if (e->pos + 1 >= e->nhist)
        return 0;
    restore(e, e->hist[++e->pos]);
    return 1;
}

static void show(const Editor *e, const char *what) {
    printf("%-14s \"%s\" cur=%d  hist=%d pos=%d evicted=%d\n", what, e->text, e->cursor, e->nhist,
           e->pos, e->evicted);
}

int main(void) {
    Editor e;
    memset(&e, 0, sizeof e);
    e.text = malloc(1);
    check(e.text != NULL, "alloc");
    e.text[0] = 0;
    e.hist[e.nhist++] = snap(&e);

    insert(&e, "hello");
    insert(&e, " world");
    show(&e, "typed");
    delete_back(&e, 3);
    show(&e, "backspace 3");
    check(undo(&e), "undo");
    show(&e, "undo");
    check(undo(&e), "undo");
    show(&e, "undo");
    check(redo(&e), "redo");
    show(&e, "redo");
    insert(&e, "!!");
    show(&e, "new edit");
    printf("redo after new edit: %d\n", redo(&e));

    for (int i = 0; i < 6; i++) {
        char buf[4];
        snprintf(buf, sizeof buf, "%d", i);
        insert(&e, buf);
    }
    show(&e, "many edits");
    int steps = 0;
    while (undo(&e))
        steps++;
    show(&e, "undo to oldest");
    printf("undo steps possible: %d (history bounded by %d)\n", steps, HIST_MAX);
    check(steps == HIST_MAX - 1, "bounded history");
    check(live_mementos == e.nhist, "mementos == history");

    while (e.nhist)
        memento_free(e.hist[--e.nhist]);
    free(e.text);
    check(live_mementos == 0, "leak");
    printf("live mementos at end: %d\n", live_mementos);
    return 0;
}
