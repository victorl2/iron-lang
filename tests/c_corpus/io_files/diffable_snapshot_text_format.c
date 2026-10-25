/*
 * title: Sorted line-based snapshot format with three-way diff
 * topic: io_files
 * covers: snapshot serialization, canonical ordering, escaped values, line diff by merge walk, added/removed/changed classification, stable output
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
void bput(Buf *b, const void *s, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + k) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) fail("oom");
        b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, s, k);
    b->n += k;
}
void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* A "database" of key -> value pairs with binary-safe values is saved as one sorted, escaped line per key
 * so that two snapshots can be compared with a plain line diff. */
typedef struct { char key[24]; unsigned char val[24]; int len; } KV;
typedef struct { KV kv[40]; int n; } Db;

static int kv_cmp(const void *a, const void *b) { return strcmp(((const KV *)a)->key, ((const KV *)b)->key); }

static void esc(Buf *b, const unsigned char *v, int len) {
    for (int i = 0; i < len; i++) {
        unsigned c = v[i];
        if (c == '\\') bstr(b, "\\\\");
        else if (c == '\n') bstr(b, "\\n");
        else if (c == '\t') bstr(b, "\\t");
        else if (c < 0x20 || c >= 0x7F) { char t[8]; snprintf(t, sizeof t, "\\x%02x", c); bstr(b, t); }
        else bbyte(b, c);
    }
}

static int unesc(const char *s, size_t n, unsigned char *out, int cap) {
    int o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (o >= cap) return -1;
        if (c != '\\') { out[o++] = c; continue; }
        if (++i >= n) return -1;
        if (s[i] == '\\') out[o++] = '\\';
        else if (s[i] == 'n') out[o++] = '\n';
        else if (s[i] == 't') out[o++] = '\t';
        else if (s[i] == 'x' && i + 2 < n) {
            unsigned v;
            char h[3] = {s[i + 1], s[i + 2], 0};
            char *end;
            v = (unsigned)strtoul(h, &end, 16);
            if (*end) return -1;
            out[o++] = (unsigned char)v;
            i += 2;
        } else return -1;
    }
    return o;
}

/* Canonical text: header, sorted "key<TAB>value" lines, trailer with count. */
static void save(const char *name, Db *db) {
    qsort(db->kv, (size_t)db->n, sizeof db->kv[0], kv_cmp);
    Buf b = {0};
    bstr(&b, "#snapshot v1\n");
    for (int i = 0; i < db->n; i++) {
        bstr(&b, db->kv[i].key);
        bbyte(&b, '\t');
        esc(&b, db->kv[i].val, db->kv[i].len);
        bbyte(&b, '\n');
    }
    char t[32];
    snprintf(t, sizeof t, "#end %d\n", db->n);
    bstr(&b, t);
    wfile(name, b.p, b.n);
    bfree(&b);
}

static int load(const char *name, Db *db) {
    size_t n;
    unsigned char *raw = rfile(name, &n);
    char *text = malloc(n + 1);
    memcpy(text, raw, n);
    text[n] = 0;
    free(raw);
    db->n = 0;
    int ok = 1, ended = 0;
    char *p = text;
    if (strncmp(p, "#snapshot v1\n", 13)) ok = 0;
    else p += 13;
    while (ok && *p) {
        char *e = strchr(p, '\n');
        if (!e) { ok = 0; break; }
        *e = 0;
        if (!strncmp(p, "#end ", 5)) { ended = atoi(p + 5) == db->n; p = e + 1; if (*p) ok = 0; break; }
        char *tab = strchr(p, '\t');
        if (!tab || db->n >= 40 || tab - p >= 24) { ok = 0; break; }
        KV *kv = &db->kv[db->n];
        memcpy(kv->key, p, (size_t)(tab - p));
        kv->key[tab - p] = 0;
        kv->len = unesc(tab + 1, strlen(tab + 1), kv->val, 24);
        if (kv->len < 0) { ok = 0; break; }
        if (db->n && strcmp(db->kv[db->n - 1].key, kv->key) >= 0) { ok = 0; break; }
        db->n++;
        p = e + 1;
    }
    free(text);
    return ok && ended;
}

static void set(Db *db, const char *key, const void *v, int len) {
    for (int i = 0; i < db->n; i++)
        if (!strcmp(db->kv[i].key, key)) { memcpy(db->kv[i].val, v, (size_t)len); db->kv[i].len = len; return; }
    CHECK(db->n < 40);
    snprintf(db->kv[db->n].key, sizeof db->kv[0].key, "%s", key);
    memcpy(db->kv[db->n].val, v, (size_t)len);
    db->kv[db->n].len = len;
    db->n++;
}

static void del(Db *db, const char *key) {
    for (int i = 0; i < db->n; i++)
        if (!strcmp(db->kv[i].key, key)) { db->kv[i] = db->kv[--db->n]; return; }
}

static void print_val(const KV *kv) {
    Buf b = {0};
    esc(&b, kv->val, kv->len);
    bbyte(&b, 0);
    printf("%s", (char *)b.p);
    bfree(&b);
}

/* merge-walk of two sorted snapshots */
static void diff(const Db *a, const Db *b, int *added, int *removed, int *changed, int verbose) {
    int i = 0, j = 0;
    *added = *removed = *changed = 0;
    while (i < a->n || j < b->n) {
        int c = i >= a->n ? 1 : j >= b->n ? -1 : strcmp(a->kv[i].key, b->kv[j].key);
        if (c < 0) { (*removed)++; if (verbose) { printf("- %s\t", a->kv[i].key); print_val(&a->kv[i]); putchar('\n'); } i++; }
        else if (c > 0) { (*added)++; if (verbose) { printf("+ %s\t", b->kv[j].key); print_val(&b->kv[j]); putchar('\n'); } j++; }
        else {
            if (a->kv[i].len != b->kv[j].len || memcmp(a->kv[i].val, b->kv[j].val, (size_t)a->kv[i].len)) {
                (*changed)++;
                if (verbose) { printf("~ %s\t", a->kv[i].key); print_val(&a->kv[i]); printf(" => "); print_val(&b->kv[j]); putchar('\n'); }
            }
            i++; j++;
        }
    }
}

int main(void) {
    static Db a, b, ra, rb;
    static const char *keys[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu"};
    for (int i = 0; i < 12; i++) {
        unsigned char v[16];
        int len = 1 + (int)(rnd() % 12);
        for (int k = 0; k < len; k++) {
            uint32_t r = rnd();
            v[k] = r % 5 == 0 ? (unsigned char)(r >> 8) : (unsigned char)('a' + (r >> 8) % 26);
        }
        set(&a, keys[(i * 5) % 12], v, len);
    }
    /* embed nasty bytes */
    set(&a, "weird", "tab\there\nnl\\bs\x01\xFF", 16);
    b = a;
    set(&b, "alpha", "changed", 7);
    del(&b, "mu");
    del(&b, "beta");
    set(&b, "new-key", "fresh", 5);
    set(&b, "another", "", 0);
    set(&b, "weird", "tab\there\nnl\\bs\x01\xFE", 16);
    save("snap_a.txt", &a);
    save("snap_b.txt", &b);
    CHECK(load("snap_a.txt", &ra) && load("snap_b.txt", &rb));
    int ad, rm, ch;
    diff(&ra, &rb, &ad, &rm, &ch, 1);
    printf("added=%d removed=%d changed=%d\n", ad, rm, ch);
    CHECK(ad == 2 && rm == 2 && ch == 2);
    /* saving what we loaded reproduces the file byte for byte */
    save("snap_a2.txt", &ra);
    size_t n1, n2;
    unsigned char *f1 = rfile("snap_a.txt", &n1), *f2 = rfile("snap_a2.txt", &n2);
    CHECK(n1 == n2 && !memcmp(f1, f2, n1));
    printf("canonical form stable: %zu bytes\n", n1);
    /* snapshots of the same data built in another insertion order are identical */
    Db c = {0};
    for (int i = b.n - 1; i >= 0; i--) set(&c, b.kv[i].key, b.kv[i].val, b.kv[i].len);
    save("snap_c.txt", &c);
    size_t n3;
    unsigned char *f3 = rfile("snap_b.txt", &n3), *f4 = rfile("snap_c.txt", &n3);
    CHECK(!memcmp(f3, f4, n3));
    printf("insertion-order independent: yes\n");
    /* first lines of the file */
    fwrite(f1, 1, 60, stdout);
    printf("...\n");
    /* corrupt files are rejected */
    static const char *bad[] = {"#snapshot v2\n#end 0\n", "#snapshot v1\na\tx\n", "#snapshot v1\nb\t1\na\t2\n#end 2\n",
                                "#snapshot v1\na\t\\q\n#end 1\n", "#snapshot v1\na\t1\n#end 5\n"};
    for (int i = 0; i < 5; i++) {
        wfile("bad.txt", bad[i], strlen(bad[i]));
        Db x;
        printf("bad %d: %s\n", i, load("bad.txt", &x) ? "accepted" : "rejected");
    }
    free(f1); free(f2); free(f3); free(f4);
    remove("snap_a.txt"); remove("snap_b.txt"); remove("snap_a2.txt"); remove("snap_c.txt"); remove("bad.txt");
    return 0;
}
