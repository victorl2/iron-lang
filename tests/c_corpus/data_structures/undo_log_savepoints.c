/*
 * title: Transactional store with undo log and nested savepoints
 * topic: data_structures
 * covers: undo log, savepoints, partial rollback, release, commit, before-image records
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 909090u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define NKEYS 32
typedef struct { int present; int val; } Cell;
/* one undo record holds the before-image of a single cell */
typedef struct { int key; Cell before; } Undo;
typedef struct { int log_pos; } Savepoint;

typedef struct {
    Cell cell[NKEYS];
    Undo *log;
    int log_n, log_cap;
    Savepoint sp[16];
    int sp_n;
    int in_txn;
    long records_written, records_undone;
} Db;

static void log_push(Db *d, int key) {
    if (!d->in_txn) return;
    if (d->log_n == d->log_cap) {
        d->log_cap = d->log_cap ? d->log_cap * 2 : 16;
        d->log = realloc(d->log, (size_t)d->log_cap * sizeof(Undo));
        CHECK(d->log);
    }
    d->log[d->log_n].key = key;
    d->log[d->log_n].before = d->cell[key];
    d->log_n++;
    d->records_written++;
}
static void db_begin(Db *d) { CHECK(!d->in_txn); d->in_txn = 1; d->log_n = 0; d->sp_n = 0; }
static void db_set(Db *d, int key, int val) { log_push(d, key); d->cell[key].present = 1; d->cell[key].val = val; }
static void db_del(Db *d, int key) { log_push(d, key); d->cell[key].present = 0; d->cell[key].val = 0; }
static void db_add(Db *d, int key, int delta) {
    log_push(d, key);
    d->cell[key].present = 1;
    d->cell[key].val += delta;
}
static int db_savepoint(Db *d) {
    CHECK(d->in_txn && d->sp_n < 16);
    d->sp[d->sp_n].log_pos = d->log_n;
    return d->sp_n++;
}
static void undo_to(Db *d, int pos) {
    while (d->log_n > pos) {
        Undo *u = &d->log[--d->log_n];
        d->cell[u->key] = u->before;
        d->records_undone++;
    }
}
/* roll back to savepoint id; the savepoint stays usable, later ones are discarded */
static void db_rollback_to(Db *d, int id) {
    CHECK(d->in_txn && id < d->sp_n);
    undo_to(d, d->sp[id].log_pos);
    d->sp_n = id + 1;
}
/* release forgets the savepoint (and later ones); their log records fold into the parent */
static void db_release(Db *d, int id) {
    CHECK(d->in_txn && id < d->sp_n);
    d->sp_n = id;
}
static void db_commit(Db *d) { CHECK(d->in_txn); d->in_txn = 0; d->log_n = 0; d->sp_n = 0; }
static void db_rollback(Db *d) { CHECK(d->in_txn); undo_to(d, 0); d->in_txn = 0; d->sp_n = 0; }

static int same(const Cell *a, const Cell *b) { return memcmp(a, b, sizeof(Cell)) == 0; }
static long checksum(const Cell *c) {
    long h = 7;
    for (int i = 0; i < NKEYS; i++) h = (h * 131 + (c[i].present ? c[i].val + 1 : 0)) % 1000003;
    return h;
}

int main(void) {
    Db db;
    memset(&db, 0, sizeof db);
    Cell committed[NKEYS];
    memset(committed, 0, sizeof committed);
    long txns = 0, commits = 0, rollbacks = 0, partials = 0, releases = 0;
    for (int t = 0; t < 400; t++) {
        db_begin(&db);
        txns++;
        Cell snap[16][NKEYS]; /* full copies taken at each savepoint, as the oracle */
        Cell start[NKEYS];
        memcpy(start, db.cell, sizeof start);
        int sp_ids = 0;
        int nops = 5 + (int)(rnd() % 30);
        for (int i = 0; i < nops; i++) {
            unsigned op = rnd() % 12;
            int k = (int)(rnd() % NKEYS);
            if (op < 4) db_set(&db, k, (int)(rnd() % 100));
            else if (op < 6) db_del(&db, k);
            else if (op < 8) db_add(&db, k, (int)(rnd() % 7) - 3);
            else if (op < 9) {
                if (sp_ids < 16 && db.sp_n < 16) {
                    int id = db_savepoint(&db);
                    memcpy(snap[id], db.cell, sizeof snap[id]);
                    sp_ids = id + 1;
                }
            } else if (op < 11) {
                if (db.sp_n > 0) {
                    int id = (int)(rnd() % (unsigned)db.sp_n);
                    db_rollback_to(&db, id);
                    CHECK(db.sp_n == id + 1);
                    for (int c = 0; c < NKEYS; c++) CHECK(same(&db.cell[c], &snap[id][c]));
                    sp_ids = id + 1;
                    partials++;
                }
            } else if (db.sp_n > 0) {
                int id = (int)(rnd() % (unsigned)db.sp_n);
                db_release(&db, id);
                sp_ids = id;
                releases++;
            }
        }
        if (rnd() % 3 == 0) {
            db_rollback(&db);
            for (int c = 0; c < NKEYS; c++) CHECK(same(&db.cell[c], &start[c]));
            rollbacks++;
        } else {
            memcpy(committed, db.cell, sizeof committed);
            db_commit(&db);
            commits++;
        }
        for (int c = 0; c < NKEYS; c++) CHECK(same(&db.cell[c], &committed[c]));
        /* writes outside a transaction are not logged */
        if (t % 50 == 0) {
            long w = db.records_written;
            db_set(&db, 0, 1);
            db_del(&db, 0);
            CHECK(db.records_written == w);
            committed[0] = db.cell[0];
        }
    }
    int live = 0;
    for (int c = 0; c < NKEYS; c++) live += db.cell[c].present;
    printf("txns=%ld commits=%ld rollbacks=%ld partial rollbacks=%ld savepoint releases=%ld\n", txns, commits, rollbacks, partials, releases);
    printf("undo records written=%ld undone=%ld, live cells=%d checksum=%ld\n", db.records_written, db.records_undone, live, checksum(db.cell));
    free(db.log);
    return 0;
}
