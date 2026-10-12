/*
 * title: Append-only key-value log with compaction
 * topic: io_files
 * covers: write-ahead style log, replay into a hash table, tombstones, torn tail record tolerance, compaction via temp file and rename
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NB = 32 };

typedef struct Entry {
    char key[16];
    char val[24];
    struct Entry *next;
} Entry;

typedef struct {
    Entry *b[NB];
    int count;
} Table;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned hk(const char *s) {
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h % NB;
}

static Entry *lookup(Table *t, const char *k) {
    for (Entry *e = t->b[hk(k)]; e; e = e->next)
        if (strcmp(e->key, k) == 0)
            return e;
    return NULL;
}

static void tset(Table *t, const char *k, const char *v) {
    Entry *e = lookup(t, k);
    if (!e) {
        e = calloc(1, sizeof *e);
        check(e != NULL, "calloc");
        snprintf(e->key, sizeof e->key, "%s", k);
        unsigned h = hk(k);
        e->next = t->b[h];
        t->b[h] = e;
        t->count++;
    }
    snprintf(e->val, sizeof e->val, "%s", v);
}

static void tdel(Table *t, const char *k) {
    unsigned h = hk(k);
    for (Entry **pp = &t->b[h]; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->key, k) == 0) {
            Entry *e = *pp;
            *pp = e->next;
            free(e);
            t->count--;
            return;
        }
    }
}

static void tfree(Table *t) {
    for (int i = 0; i < NB; i++) {
        Entry *e = t->b[i];
        while (e) {
            Entry *n = e->next;
            free(e);
            e = n;
        }
        t->b[i] = NULL;
    }
    t->count = 0;
}

/* record: "S key val\n" or "D key\n", followed by a checksum digit per line "#c\n" folded into the line */
static unsigned line_sum(const char *s) {
    unsigned x = 0;
    for (; *s; s++)
        x = x * 7 + (unsigned char)*s;
    return x % 97;
}

static void log_put(FILE *f, char op, const char *k, const char *v) {
    char body[64];
    if (op == 'S')
        snprintf(body, sizeof body, "S %s %s", k, v);
    else
        snprintf(body, sizeof body, "D %s", k);
    fprintf(f, "%s|%02u\n", body, line_sum(body));
}

/* replay; returns the number of valid records, stops at the first corrupt or torn one */
static int replay(const char *name, Table *t, int *bad) {
    FILE *f = fopen(name, "r");
    if (!f)
        return 0;
    char line[128];
    int good = 0;
    *bad = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        char *bar = strrchr(line, '|');
        if (n == 0 || line[n - 1] != '\n' || !bar) {
            (*bad)++;
            break;
        }
        *bar = 0;
        unsigned sum = (unsigned)atoi(bar + 1);
        if (sum != line_sum(line)) {
            (*bad)++;
            break;
        }
        char op, k[16], v[24];
        if (line[0] == 'S' && sscanf(line, "%c %15s %23s", &op, k, v) == 3)
            tset(t, k, v);
        else if (line[0] == 'D' && sscanf(line, "%c %15s", &op, k) == 2)
            tdel(t, k);
        good++;
    }
    fclose(f);
    return good;
}

static void dump(Table *t, const char *title) {
    const char *keys[] = {"alpha", "beta", "gamma", "delta", "eps", "zeta", "eta", "theta"};
    printf("%s (%d keys):", title, t->count);
    for (int i = 0; i < 8; i++) {
        Entry *e = lookup(t, keys[i]);
        if (e)
            printf(" %s=%s", e->key, e->val);
    }
    printf("\n");
}

int main(void) {
    FILE *f = fopen("kv.log", "w");
    check(f != NULL, "open");
    log_put(f, 'S', "alpha", "1");
    log_put(f, 'S', "beta", "2");
    log_put(f, 'S', "gamma", "3");
    log_put(f, 'S', "alpha", "one");
    log_put(f, 'D', "beta", "");
    log_put(f, 'S', "delta", "4");
    log_put(f, 'S', "gamma", "three");
    log_put(f, 'S', "beta", "again");
    log_put(f, 'D', "delta", "");
    log_put(f, 'S', "eps", "5");
    fclose(f);

    Table t = {{0}, 0};
    int bad;
    int n = replay("kv.log", &t, &bad);
    printf("replayed %d records, %d bad\n", n, bad);
    dump(&t, "state");
    check(n == 10 && bad == 0 && t.count == 4, "replay");
    check(strcmp(lookup(&t, "alpha")->val, "one") == 0, "alpha");

    /* compaction: write one S record per live key into a temp file, then rename over the log */
    f = fopen("kv.tmp", "w");
    check(f != NULL, "tmp");
    const char *order[] = {"alpha", "beta", "gamma", "eps"};
    for (int i = 0; i < 4; i++) {
        Entry *e = lookup(&t, order[i]);
        check(e != NULL, "live key");
        log_put(f, 'S', e->key, e->val);
    }
    check(fclose(f) == 0, "tmp close");
    check(rename("kv.tmp", "kv.log") == 0, "rename");

    Table t2 = {{0}, 0};
    n = replay("kv.log", &t2, &bad);
    printf("after compaction: %d records, %d bad\n", n, bad);
    dump(&t2, "state");
    check(n == 4 && t2.count == t.count, "compact");
    tfree(&t);
    tfree(&t2);

    /* simulate a crash: append a torn record with no newline, then a corrupt one */
    f = fopen("kv.log", "a");
    log_put(f, 'S', "zeta", "6");
    fputs("S eta 7|", f); /* torn: missing checksum and newline */
    fclose(f);
    Table t3 = {{0}, 0};
    n = replay("kv.log", &t3, &bad);
    printf("with torn tail: %d records, %d bad\n", n, bad);
    dump(&t3, "state");
    check(n == 5 && bad == 1 && lookup(&t3, "eta") == NULL && lookup(&t3, "zeta"), "torn");
    tfree(&t3);

    /* a flipped byte in the middle stops replay at that record */
    f = fopen("kv.log", "r+");
    fseek(f, 2, SEEK_SET);
    int c = fgetc(f);
    fseek(f, 2, SEEK_SET);
    fputc(c == 'z' ? 'y' : 'z', f);
    fclose(f);
    Table t4 = {{0}, 0};
    n = replay("kv.log", &t4, &bad);
    printf("with corrupt first record: %d records, %d bad, %d keys\n", n, bad, t4.count);
    check(n == 0 && bad == 1 && t4.count == 0, "corrupt");
    tfree(&t4);
    remove("kv.log");
    return 0;
}
