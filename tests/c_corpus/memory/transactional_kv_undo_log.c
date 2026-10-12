/*
 * title: Transactional key-value store with undo log and savepoints
 * topic: memory
 * covers: transactions, undo log ownership, nested savepoints, rollback, oracle cross-check, leak-free commit
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NKEYS 16

static long live;
static void *xm(size_t n) { void *p = malloc(n); if (p) live++; return p; }
static void xf(void *p) { if (p) { live--; free(p); } }
static char *xdup(const char *s) { char *d = xm(strlen(s) + 1); if (d) strcpy(d, s); return d; }

typedef struct Undo { int key; char *old; struct Undo *next; } Undo; /* old == NULL means "was absent" */

typedef struct {
    char *slots[NKEYS];
    Undo *log;
    int depth;
    int marks[8];      /* undo-log lengths at each savepoint */
    int loglen;
} Store;

static void put(Store *s, int k, const char *v) {
    if (s->depth > 0) {
        Undo *u = xm(sizeof *u);
        u->key = k;
        u->old = s->slots[k]; /* ownership of the old value moves into the log */
        u->next = s->log;
        s->log = u;
        s->loglen++;
    } else {
        xf(s->slots[k]);
    }
    s->slots[k] = v ? xdup(v) : NULL;
}

static void begin(Store *s) { s->marks[s->depth++] = s->loglen; }

static void undo_to(Store *s, int len) {
    while (s->loglen > len) {
        Undo *u = s->log;
        s->log = u->next;
        xf(s->slots[u->key]);
        s->slots[u->key] = u->old;
        xf(u);
        s->loglen--;
    }
}
static void rollback(Store *s) { undo_to(s, s->marks[--s->depth]); }

static void commit(Store *s) {
    s->depth--;
    if (s->depth == 0) {
        /* outermost commit: the log's old values are now garbage */
        while (s->log) { Undo *u = s->log; s->log = u->next; xf(u->old); xf(u); }
        s->loglen = 0;
    } /* inner commit: entries stay so the outer transaction can still undo them */
}

static unsigned r = 99u;
static unsigned rnd(void) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; }

static int same(const Store *a, char *const *oracle) {
    for (int i = 0; i < NKEYS; i++) {
        if ((a->slots[i] == NULL) != (oracle[i] == NULL)) return 0;
        if (a->slots[i] && strcmp(a->slots[i], oracle[i]) != 0) return 0;
    }
    return 1;
}

static void oracle_copy(char **dst, char *const *src) {
    for (int i = 0; i < NKEYS; i++) { xf(dst[i]); dst[i] = src[i] ? xdup(src[i]) : NULL; }
}

static int count(const Store *s) { int c = 0; for (int i = 0; i < NKEYS; i++) c += s->slots[i] != NULL; return c; }

int main(void) {
    Store st;
    memset(&st, 0, sizeof st);
    char *snap[3][NKEYS];
    memset(snap, 0, sizeof snap);
    int commits = 0, rollbacks = 0, ops = 0;

    for (int round = 0; round < 60; round++) {
        int depth_target = 1 + (int)(rnd() % 3);
        for (int d = 0; d < depth_target; d++) {
            oracle_copy(snap[d], st.slots);
            begin(&st);
            int nops = 1 + (int)(rnd() % 6);
            for (int i = 0; i < nops; i++) {
                int k = (int)(rnd() % NKEYS);
                unsigned vr = rnd();
                char buf[16];
                snprintf(buf, sizeof buf, "v%u", vr % 1000);
                put(&st, k, vr % 5 == 0 ? NULL : buf);
                ops++;
            }
        }
        /* unwind: randomly commit or roll back each level; rolled-back level restores its snapshot */
        for (int d = depth_target - 1; d >= 0; d--) {
            if (rnd() % 2) {
                rollback(&st);
                if (!same(&st, snap[d])) { fprintf(stderr, "rollback mismatch round %d depth %d\n", round, d); return 1; }
                rollbacks++;
            } else {
                commit(&st);
                commits++;
                /* a committed inner level only becomes durable if the outers commit; nothing to check yet */
            }
        }
        if (st.depth != 0 || st.loglen != 0 || st.log) { fprintf(stderr, "log not empty\n"); return 1; }
    }
    printf("ops=%d commits=%d rollbacks=%d keys present=%d\n", ops, commits, rollbacks, count(&st));

    /* deterministic finale with a savepoint */
    put(&st, 0, "base");
    begin(&st);
    put(&st, 0, "outer");
    put(&st, 1, "one");
    begin(&st);
    put(&st, 0, "inner");
    put(&st, 2, "two");
    rollback(&st);
    printf("after inner rollback: k0=%s k1=%s k2=%s\n", st.slots[0], st.slots[1], st.slots[2] ? st.slots[2] : "(absent)");
    begin(&st);
    put(&st, 3, "three");
    commit(&st);
    rollback(&st);
    printf("after outer rollback: k0=%s k1=%s k3=%s\n", st.slots[0], st.slots[1] ? st.slots[1] : "(absent)", st.slots[3] ? st.slots[3] : "(absent)");

    for (int i = 0; i < NKEYS; i++) xf(st.slots[i]);
    for (int d = 0; d < 3; d++) for (int i = 0; i < NKEYS; i++) xf(snap[d][i]);
    printf("live at end=%ld\n", live);
    return live == 0 ? 0 : 1;
}
