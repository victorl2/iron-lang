/*
 * title: Init/deinit pairs with partial-init rollback
 * topic: memory
 * covers: RAII-style init/deinit, composite objects, partial initialization rollback, idempotent deinit, nested components
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_countdown = -1; /* fail the Nth init call overall (0-based) */
static int inits, deinits;
static char seq[256];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void trace(const char *s) {
    check(strlen(seq) + strlen(s) + 1 < sizeof seq, "trace");
    strcat(seq, s);
}

static int should_fail(void) {
    if (fail_countdown == 0)
        return 1;
    if (fail_countdown > 0)
        fail_countdown--;
    return 0;
}

typedef struct { int *cells; int n; } Grid;
typedef struct { char *text; } Label;
typedef struct { Grid grid; Label title; Label footer; } Panel;
typedef struct { Panel panels[3]; Label name; int ready; } Window;

static int grid_init(Grid *g, int n) {
    memset(g, 0, sizeof *g);
    if (should_fail())
        return -1;
    g->cells = calloc((size_t)n, sizeof(int));
    check(g->cells != NULL, "alloc");
    g->n = n;
    inits++;
    trace("G+");
    return 0;
}
static void grid_deinit(Grid *g) {
    if (!g->cells)
        return;
    free(g->cells);
    g->cells = NULL;
    deinits++;
    trace("G-");
}

static int label_init(Label *l, const char *s) {
    memset(l, 0, sizeof *l);
    if (should_fail())
        return -1;
    l->text = malloc(strlen(s) + 1);
    check(l->text != NULL, "alloc");
    strcpy(l->text, s);
    inits++;
    trace("L+");
    return 0;
}
static void label_deinit(Label *l) {
    if (!l->text)
        return;
    free(l->text);
    l->text = NULL;
    deinits++;
    trace("L-");
}

static void panel_deinit(Panel *p) {
    label_deinit(&p->footer);
    label_deinit(&p->title);
    grid_deinit(&p->grid);
}

static int panel_init(Panel *p, int n) {
    memset(p, 0, sizeof *p);
    if (grid_init(&p->grid, n) != 0)
        goto fail;
    if (label_init(&p->title, "title") != 0)
        goto fail;
    if (label_init(&p->footer, "foot") != 0)
        goto fail;
    return 0;
fail:
    panel_deinit(p); /* deinit tolerates partially initialized members */
    return -1;
}

static void window_deinit(Window *w) {
    for (int i = 2; i >= 0; i--)
        panel_deinit(&w->panels[i]);
    label_deinit(&w->name);
    w->ready = 0;
}

static int window_init(Window *w) {
    memset(w, 0, sizeof *w);
    if (label_init(&w->name, "main") != 0)
        goto fail;
    for (int i = 0; i < 3; i++)
        if (panel_init(&w->panels[i], 4 + i) != 0)
            goto fail;
    w->ready = 1;
    return 0;
fail:
    window_deinit(w);
    return -1;
}

int main(void) {
    /* total init calls on success: 1 label + 3 panels * 3 = 10 */
    int total = 10;
    int failures = 0;
    for (int f = 0; f <= total; f++) {
        Window w;
        inits = deinits = 0;
        seq[0] = 0;
        fail_countdown = f;
        int r = window_init(&w);
        int ok_inits = inits;
        printf("fail=%2d -> %s inits=%d deinits=%d\n", f, r ? "rolled back" : "ready", inits,
               deinits);
        if (r) {
            failures++;
            check(inits == deinits, "rollback must undo every init");
            check(!w.ready, "not ready");
        } else {
            check(f == total && ok_inits == total, "success only without failure");
            printf("  sequence: %s\n", seq);
            window_deinit(&w);
            check(inits == deinits, "balanced after deinit");
            window_deinit(&w); /* idempotent */
            check(inits == deinits, "second deinit is a no-op");
        }
    }
    printf("failures=%d\n", failures);
    check(failures == total, "count");
    return 0;
}
