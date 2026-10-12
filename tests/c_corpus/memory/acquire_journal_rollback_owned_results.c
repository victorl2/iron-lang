/*
 * title: Journaled acquisitions with rollback and owned results
 * topic: memory
 * covers: transaction journal of allocations, commit vs abort, ownership handed to caller on commit, injected failures in a loop, nested journals
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *ptrs[32];
    int n;
} Journal;

static int fail_at = -1, acq_no;
static int live_blocks;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *jalloc(Journal *j, size_t n) {
    if (acq_no++ == fail_at)
        return NULL;
    check(j->n < 32, "journal full");
    void *p = calloc(1, n);
    check(p != NULL, "alloc");
    j->ptrs[j->n++] = p;
    live_blocks++;
    return p;
}

/* Abort frees everything the journal tracks. */
static void jabort(Journal *j) {
    while (j->n > 0) {
        free(j->ptrs[--j->n]);
        live_blocks--;
    }
}

/* Commit forgets the entries (ownership now belongs to whoever holds the pointers). */
static void jcommit(Journal *j) { j->n = 0; }

/* Merge a child journal into its parent: the parent takes responsibility. */
static int jmerge(Journal *parent, Journal *child) {
    if (parent->n + child->n > 32)
        return -1;
    memcpy(&parent->ptrs[parent->n], child->ptrs, (size_t)child->n * sizeof(void *));
    parent->n += child->n;
    child->n = 0;
    return 0;
}

typedef struct {
    char *key;
    char *value;
} Pair;

typedef struct {
    Pair *pairs;
    int n;
    char *summary;
} Config;

static Pair *parse_pair(Journal *j, const char *k, const char *v) {
    Pair *p = jalloc(j, sizeof *p);
    if (!p)
        return NULL;
    p->key = jalloc(j, strlen(k) + 1);
    if (!p->key)
        return NULL;
    strcpy(p->key, k);
    p->value = jalloc(j, strlen(v) + 1);
    if (!p->value)
        return NULL;
    strcpy(p->value, v);
    return p;
}

/* Builds a Config or returns NULL with nothing leaked. */
static Config *build_config(const char *const *kv, int npairs) {
    Journal j = {{0}, 0};
    Config *cfg = jalloc(&j, sizeof *cfg);
    if (!cfg)
        goto fail;
    cfg->pairs = jalloc(&j, (size_t)npairs * sizeof(Pair));
    if (!cfg->pairs)
        goto fail;
    for (int i = 0; i < npairs; i++) {
        Journal inner = {{0}, 0};
        Pair *p = parse_pair(&inner, kv[2 * i], kv[2 * i + 1]);
        if (!p) {
            jabort(&inner); /* only the failed pair's allocations */
            goto fail;
        }
        cfg->pairs[i] = *p;
        /* the Pair struct itself was a temporary: release it, keep key/value */
        for (int k = 0; k < inner.n; k++)
            if (inner.ptrs[k] == p) {
                inner.ptrs[k] = inner.ptrs[--inner.n];
                free(p);
                live_blocks--;
                break;
            }
        check(jmerge(&j, &inner) == 0, "merge");
        cfg->n++;
    }
    cfg->summary = jalloc(&j, 32);
    if (!cfg->summary)
        goto fail;
    snprintf(cfg->summary, 32, "%d pairs", cfg->n);
    jcommit(&j);
    return cfg;
fail:
    jabort(&j);
    return NULL;
}

static void config_free(Config *c) {
    for (int i = 0; i < c->n; i++) {
        free(c->pairs[i].key);
        free(c->pairs[i].value);
        live_blocks -= 2;
    }
    free(c->pairs);
    free(c->summary);
    free(c);
    live_blocks -= 3;
}

int main(void) {
    const char *kv[] = {"host", "iron.example", "port", "8080", "mode", "fast", "log", "debug"};
    int npairs = 4;
    int total = -1;
    for (fail_at = -1;; fail_at++) {
        acq_no = 0;
        Config *c = build_config(kv, npairs);
        int made = acq_no;
        if (fail_at < 0) {
            check(c != NULL, "baseline must succeed");
            total = made;
            printf("baseline: %d acquisitions, summary=\"%s\", %s=%s\n", total, c->summary,
                   c->pairs[2].key, c->pairs[2].value);
            config_free(c);
            check(live_blocks == 0, "baseline leak");
            continue;
        }
        if (c) {
            check(fail_at >= total, "success only past the last acquisition");
            config_free(c);
            printf("fail_at=%d: success (fault never reached)\n", fail_at);
            break;
        }
        printf("fail_at=%2d: aborted after %2d acquisitions, live=%d\n", fail_at, made, live_blocks);
        check(live_blocks == 0, "leak after abort");
        check(made == fail_at + 1, "aborted right at the fault");
    }
    check(live_blocks == 0, "final leak");
    return 0;
}
