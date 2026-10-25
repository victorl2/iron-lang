/*
 * title: Copy-on-write byte buffers with shared representations
 * topic: data_structures
 * covers: copy on write, reference counted representation, lazy detach, unique-owner fast path, sharing statistics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 4711u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct { unsigned char *data; size_t len, cap; int rc; } Rep;
typedef struct { Rep *rep; } Buf;

static long reps_alive, detaches, fast_writes, bytes_copied;

static Rep *rep_new(size_t cap) {
    Rep *r = malloc(sizeof *r);
    CHECK(r);
    r->cap = cap < 8 ? 8 : cap;
    r->data = malloc(r->cap);
    CHECK(r->data);
    r->len = 0; r->rc = 1;
    reps_alive++;
    return r;
}
static void rep_release(Rep *r) {
    if (--r->rc == 0) { free(r->data); free(r); reps_alive--; }
}
static Buf b_new(void) { Buf b = { rep_new(8) }; return b; }
static Buf b_from(const unsigned char *p, size_t n) {
    Buf b = { rep_new(n) };
    memcpy(b.rep->data, p, n);
    b.rep->len = n;
    return b;
}
static Buf b_share(Buf b) { b.rep->rc++; return b; }
static void b_drop(Buf *b) { rep_release(b->rep); b->rep = NULL; }
/* make the representation unique before any write; grows to at least `need` bytes */
static void b_unique(Buf *b, size_t need) {
    Rep *r = b->rep;
    if (r->rc == 1) {
        if (need > r->cap) {
            size_t nc = r->cap;
            while (nc < need) nc *= 2;
            r->data = realloc(r->data, nc);
            CHECK(r->data);
            r->cap = nc;
        }
        fast_writes++;
        return;
    }
    Rep *n = rep_new(need > r->len ? need : r->len);
    memcpy(n->data, r->data, r->len);
    n->len = r->len;
    bytes_copied += (long)r->len;
    detaches++;
    r->rc--;
    b->rep = n;
}
static void b_append(Buf *b, const unsigned char *p, size_t n) {
    b_unique(b, b->rep->len + n);
    memcpy(b->rep->data + b->rep->len, p, n);
    b->rep->len += n;
}
static void b_set(Buf *b, size_t i, unsigned char c) {
    CHECK(i < b->rep->len);
    if (b->rep->data[i] == c && b->rep->rc > 1) return; /* no-op writes do not detach */
    b_unique(b, b->rep->len);
    b->rep->data[i] = c;
}
static void b_truncate(Buf *b, size_t n) {
    CHECK(n <= b->rep->len);
    if (n == b->rep->len) return;
    b_unique(b, b->rep->len);
    b->rep->len = n;
}
static Buf b_concat(Buf a, Buf c) { /* new buffer, inputs untouched */
    Buf r = { rep_new(a.rep->len + c.rep->len) };
    memcpy(r.rep->data, a.rep->data, a.rep->len);
    memcpy(r.rep->data + a.rep->len, c.rep->data, c.rep->len);
    r.rep->len = a.rep->len + c.rep->len;
    return r;
}

#define NB 12
#define MAXLEN 600

int main(void) {
    Buf b[NB];
    unsigned char model[NB][MAXLEN];
    size_t mlen[NB];
    for (int i = 0; i < NB; i++) {
        b[i] = i % 2 ? b_new() : b_from((const unsigned char *)"seed", 4);
        mlen[i] = i % 2 ? 0 : 4;
        memcpy(model[i], "seed", 4);
    }
    long shares = 0, appends = 0, sets = 0, noops = 0, truncs = 0, concats = 0;
    for (int step = 0; step < 6000; step++) {
        int i = (int)(rnd() % NB), j = (int)(rnd() % NB);
        unsigned op = rnd() % 12;
        if (op < 3) {
            if (i != j) {
                b_drop(&b[i]);
                b[i] = b_share(b[j]);
                memcpy(model[i], model[j], mlen[j]);
                mlen[i] = mlen[j];
                shares++;
            }
        } else if (op < 6) {
            unsigned char chunk[16];
            size_t n = 1 + rnd() % 15;
            for (size_t k = 0; k < n; k++) chunk[k] = (unsigned char)('a' + rnd() % 26);
            if (mlen[i] + n <= MAXLEN) {
                b_append(&b[i], chunk, n);
                memcpy(model[i] + mlen[i], chunk, n);
                mlen[i] += n;
                appends++;
            }
        } else if (op < 9) {
            if (mlen[i]) {
                size_t pos = rnd() % mlen[i];
                unsigned char c = (unsigned char)('a' + rnd() % 3);
                long before = detaches;
                b_set(&b[i], pos, c);
                if (model[i][pos] == c) noops++;
                model[i][pos] = c;
                (void)before;
                sets++;
            }
        } else if (op < 10) {
            if (mlen[i]) {
                size_t n = rnd() % (mlen[i] + 1);
                b_truncate(&b[i], n);
                mlen[i] = n;
                truncs++;
            }
        } else if (op < 11) {
            if (mlen[i] + mlen[j] <= MAXLEN) {
                Buf c = b_concat(b[i], b[j]);
                unsigned char tmp[MAXLEN];
                memcpy(tmp, model[i], mlen[i]);
                memcpy(tmp + mlen[i], model[j], mlen[j]);
                b_drop(&b[i]);
                b[i] = c;
                memcpy(model[i], tmp, mlen[i] + mlen[j]);
                mlen[i] += mlen[j];
                concats++;
            }
        } else {
            b_drop(&b[i]);
            b[i] = b_new();
            mlen[i] = 0;
        }
        /* every handle reads exactly its own model, regardless of how much is shared */
        CHECK(b[i].rep->len == mlen[i] && memcmp(b[i].rep->data, model[i], mlen[i]) == 0);
        if (step % 200 == 0) {
            int refs[NB];
            for (int x = 0; x < NB; x++) {
                refs[x] = 0;
                for (int y = 0; y < NB; y++) if (b[y].rep == b[x].rep) refs[x]++;
                CHECK(b[x].rep->rc == refs[x]);
                CHECK(b[x].rep->len == mlen[x] && memcmp(b[x].rep->data, model[x], mlen[x]) == 0);
            }
        }
    }
    long distinct = 0;
    int max_share = 0;
    for (int x = 0; x < NB; x++) {
        int firstseen = 1;
        for (int y = 0; y < x; y++) if (b[y].rep == b[x].rep) firstseen = 0;
        distinct += firstseen;
        if (b[x].rep->rc > max_share) max_share = b[x].rep->rc;
    }
    CHECK(distinct == reps_alive);
    size_t total = 0;
    for (int x = 0; x < NB; x++) total += mlen[x];
    printf("shares=%ld appends=%ld sets=%ld (no-op %ld) truncates=%ld concats=%ld\n", shares, appends, sets, noops, truncs, concats);
    printf("detaches=%ld in-place writes=%ld bytes copied by detach=%ld\n", detaches, fast_writes, bytes_copied);
    printf("handles=%d distinct representations=%ld max sharing=%d total bytes=%zu\n", NB, distinct, max_share, total);
    for (int x = 0; x < NB; x++) b_drop(&b[x]);
    CHECK(reps_alive == 0);
    return 0;
}
