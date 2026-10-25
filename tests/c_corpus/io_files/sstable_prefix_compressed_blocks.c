/*
 * title: Sorted string table with prefix-compressed blocks, restart points and sparse index
 * topic: io_files
 * covers: sstable, prefix compression, restart points, varint lengths, block index in footer, binary search across blocks, range scan, tombstones, checksum
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

static uint32_t crc_tab[256];
void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
}
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n) {
    const unsigned char *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
uint32_t crc32_of(const void *buf, size_t n) { return crc32_update(0, buf, n); }

enum { BLOCK_TARGET = 200, RESTART = 4 };

typedef struct { char key[24]; char val[24]; int tomb; } KV;

static void pv(Buf *b, uint32_t v) { while (v >= 0x80) { bbyte(b, (unsigned)(v & 0x7F) | 0x80u); v >>= 7; } bbyte(b, v); }
static uint32_t rv(const unsigned char *p, size_t *pos) {
    uint32_t v = 0;
    int sh = 0;
    for (;;) { unsigned char c = p[(*pos)++]; v |= (uint32_t)(c & 0x7F) << sh; if (!(c & 0x80)) return v; sh += 7; }
}
static void p32(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) bbyte(b, (v >> (8 * i)) & 255u); }
static uint32_t g32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct { char last_key[24]; uint32_t off, len; } IndexEnt;

/* block: entries [shared][unshared][vlen|tomb][key suffix][value] ..., restart offsets u32[], nrestarts u32, crc u32 */
static void flush_block(Buf *f, Buf *blk, uint32_t *restarts, int nr, const char *last_key, IndexEnt *idx, int *nidx) {
    for (int i = 0; i < nr; i++) p32(blk, restarts[i]);
    p32(blk, (uint32_t)nr);
    p32(blk, crc32_of(blk->p, blk->n));
    idx[*nidx].off = (uint32_t)f->n;
    idx[*nidx].len = (uint32_t)blk->n;
    snprintf(idx[*nidx].last_key, sizeof idx[0].last_key, "%s", last_key);
    (*nidx)++;
    bput(f, blk->p, blk->n);
    blk->n = 0;
}

static int write_table(Buf *f, const KV *kv, int n, IndexEnt *idx, int *entries_in_index, size_t *raw_bytes) {
    Buf blk = {0};
    uint32_t restarts[64];
    int nr = 0, nidx = 0;
    char prev[24] = "";
    *raw_bytes = 0;
    for (int i = 0; i < n; i++) {
        if (i) CHECK(strcmp(kv[i - 1].key, kv[i].key) < 0);
        size_t shared = 0;
        int is_restart = nr == 0 || (i % RESTART == 0);
        if (nr == 0) prev[0] = 0;
        if (!is_restart) while (prev[shared] && prev[shared] == kv[i].key[shared]) shared++;
        if (is_restart) { CHECK(nr < 64); restarts[nr++] = (uint32_t)blk.n; shared = 0; }
        size_t un = strlen(kv[i].key) - shared;
        size_t vl = kv[i].tomb ? 0 : strlen(kv[i].val);
        pv(&blk, (uint32_t)shared); pv(&blk, (uint32_t)un); pv(&blk, (uint32_t)(vl * 2 + (kv[i].tomb ? 1u : 0u)));
        bput(&blk, kv[i].key + shared, un);
        bput(&blk, kv[i].val, vl);
        *raw_bytes += strlen(kv[i].key) + vl;
        snprintf(prev, sizeof prev, "%s", kv[i].key);
        if (blk.n >= BLOCK_TARGET || i == n - 1) { flush_block(f, &blk, restarts, nr, kv[i].key, idx, &nidx); nr = 0; }
    }
    bfree(&blk);
    size_t ioff = f->n;
    p32(f, (uint32_t)nidx);
    for (int i = 0; i < nidx; i++) { p32(f, idx[i].off); p32(f, idx[i].len); bbyte(f, (unsigned)strlen(idx[i].last_key)); bstr(f, idx[i].last_key); }
    p32(f, (uint32_t)ioff);
    p32(f, (uint32_t)n);
    bstr(f, "SST1");
    *entries_in_index = nidx;
    return nidx;
}

typedef struct { const unsigned char *d; size_t n; IndexEnt idx[32]; int nidx; uint32_t count; int blocks_read; } Table;

static const char *open_table(Table *t, const unsigned char *d, size_t n) {
    t->d = d; t->n = n; t->blocks_read = 0;
    if (n < 12 || memcmp(d + n - 4, "SST1", 4)) return "bad magic";
    t->count = g32(d + n - 8);
    size_t ioff = g32(d + n - 12);
    if (ioff >= n) return "bad index offset";
    size_t p = ioff;
    t->nidx = (int)g32(d + p); p += 4;
    if (t->nidx > 32) return "index too large";
    for (int i = 0; i < t->nidx; i++) {
        t->idx[i].off = g32(d + p); t->idx[i].len = g32(d + p + 4);
        size_t kl = d[p + 8];
        memcpy(t->idx[i].last_key, d + p + 9, kl);
        t->idx[i].last_key[kl] = 0;
        p += 9 + kl;
    }
    return NULL;
}

/* decode a whole block into kv (verifies crc) */
static int load_block(Table *t, int b, KV *out, int max) {
    const unsigned char *blk = t->d + t->idx[b].off;
    size_t len = t->idx[b].len;
    t->blocks_read++;
    if (crc32_of(blk, len - 4) != g32(blk + len - 4)) return -1;
    uint32_t nr = g32(blk + len - 8);
    size_t data_end = len - 8 - 4 * (size_t)nr;
    size_t pos = 0;
    int cnt = 0;
    char prev[24] = "";
    while (pos < data_end) {
        uint32_t shared = rv(blk, &pos), un = rv(blk, &pos), vt = rv(blk, &pos);
        if (cnt >= max || shared > strlen(prev) || shared + un >= 24) return -2;
        KV *kv = &out[cnt++];
        memcpy(kv->key, prev, shared);
        memcpy(kv->key + shared, blk + pos, un);
        kv->key[shared + un] = 0;
        pos += un;
        kv->tomb = vt & 1;
        uint32_t vl = vt >> 1;
        memcpy(kv->val, blk + pos, vl);
        kv->val[vl] = 0;
        pos += vl;
        snprintf(prev, sizeof prev, "%s", kv->key);
    }
    return cnt;
}

/* point lookup: binary search the sparse index, read one block */
static int get(Table *t, const char *key, char *val) {
    int lo = 0, hi = t->nidx - 1, b = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(t->idx[mid].last_key, key) >= 0) { b = mid; hi = mid - 1; } else lo = mid + 1;
    }
    if (b < 0) return 0;
    KV kv[40];
    int c = load_block(t, b, kv, 40);
    CHECK(c > 0);
    for (int i = 0; i < c; i++)
        if (!strcmp(kv[i].key, key)) { if (kv[i].tomb) return 2; strcpy(val, kv[i].val); return 1; }
    return 0;
}

static int kvcmp(const void *a, const void *b) { return strcmp(((const KV *)a)->key, ((const KV *)b)->key); }

int main(void) {
    crc_init();
    static KV kv[120];
    static const char *stems[] = {"user:", "user:admin:", "session:", "config.", "config.net."};
    int n = 0;
    while (n < 120) {
        uint32_t a = rnd(), b = rnd(), c = rnd();
        snprintf(kv[n].key, sizeof kv[n].key, "%s%04u", stems[a % 5], (unsigned)(b % 800));
        snprintf(kv[n].val, sizeof kv[n].val, "v%u", (unsigned)(c % 100000));
        kv[n].tomb = c % 11 == 0;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(kv[i].key, kv[n].key)) dup = 1;
        if (!dup) n++;
    }
    qsort(kv, (size_t)n, sizeof kv[0], kvcmp);
    Buf f = {0};
    IndexEnt idx[32];
    int nidx;
    size_t raw;
    write_table(&f, kv, n, idx, &nidx, &raw);
    wfile("t.sst", f.p, f.n);
    size_t fn;
    unsigned char *d = rfile("t.sst", &fn);
    printf("%d entries in %d blocks; raw key+value bytes %zu, file %zu bytes\n", n, nidx, raw, fn);
    Table t;
    CHECK(open_table(&t, d, fn) == NULL);
    CHECK((int)t.count == n && t.nidx == nidx);
    for (int i = 0; i < t.nidx; i++) printf("  block %d: offset %4u len %3u last key %s\n", i, (unsigned)t.idx[i].off, (unsigned)t.idx[i].len, t.idx[i].last_key);
    /* full scan matches source */
    KV blk[40];
    int total = 0;
    for (int b = 0; b < t.nidx; b++) {
        int c = load_block(&t, b, blk, 40);
        CHECK(c > 0);
        for (int i = 0; i < c; i++) {
            CHECK(!strcmp(blk[i].key, kv[total].key) && blk[i].tomb == kv[total].tomb);
            if (!kv[total].tomb) CHECK(!strcmp(blk[i].val, kv[total].val));
            total++;
        }
        CHECK(!strcmp(blk[c - 1].key, t.idx[b].last_key));
    }
    CHECK(total == n);
    /* point lookups: all present keys, absent keys and tombstones, counting block reads */
    int found = 0, tombs = 0, missing = 0;
    t.blocks_read = 0;
    for (int i = 0; i < n; i++) {
        char v[24];
        int r = get(&t, kv[i].key, v);
        if (kv[i].tomb) { CHECK(r == 2); tombs++; }
        else { CHECK(r == 1 && !strcmp(v, kv[i].val)); found++; }
    }
    static const char *absent[] = {"aaa", "config.0000", "session:9999", "user:admin:0400x", "zzz", "user:"};
    for (int i = 0; i < 6; i++) {
        char v[24];
        int r = get(&t, absent[i], v);
        printf("lookup %-18s -> %s\n", absent[i], r == 0 ? "absent" : r == 1 ? "found" : "deleted");
        missing += r == 0;
    }
    printf("lookups: %d live, %d tombstones, %d absent; %d block reads for %d gets (at most one each)\n", found, tombs, missing, t.blocks_read, n + 6);
    CHECK(t.blocks_read <= n + 6);
    /* prefix compression effect */
    size_t stored = 0;
    for (int b = 0; b < t.nidx; b++) stored += t.idx[b].len;
    printf("data blocks %zu bytes vs raw %zu bytes\n", stored, raw);
    /* range scan [from, to) using the index to skip blocks */
    const char *from = "session:0200", *to = "session:0300";
    int start_block = 0;
    while (start_block < t.nidx && strcmp(t.idx[start_block].last_key, from) < 0) start_block++;
    int in_range = 0, touched = 0;
    for (int b = start_block; b < t.nidx; b++) {
        int c = load_block(&t, b, blk, 40);
        touched++;
        int done = 0;
        for (int i = 0; i < c; i++) {
            if (strcmp(blk[i].key, to) >= 0) { done = 1; break; }
            if (strcmp(blk[i].key, from) >= 0 && !blk[i].tomb) in_range++;
        }
        if (done) break;
    }
    int expect = 0;
    for (int i = 0; i < n; i++) if (strcmp(kv[i].key, from) >= 0 && strcmp(kv[i].key, to) < 0 && !kv[i].tomb) expect++;
    CHECK(in_range == expect);
    printf("range [%s, %s): %d live keys, %d of %d blocks touched\n", from, to, in_range, touched, t.nidx);
    /* corrupt one byte in a block: crc rejects the block but neighbours still read */
    d[t.idx[1].off + 5] ^= 0x10;
    CHECK(load_block(&t, 1, blk, 40) == -1);
    CHECK(load_block(&t, 0, blk, 40) > 0 && load_block(&t, 2, blk, 40) > 0);
    printf("corrupted block 1 rejected, blocks 0 and 2 still readable\n");
    d[fn - 1] = 'X';
    printf("bad magic: %s\n", open_table(&t, d, fn));
    free(d); bfree(&f);
    remove("t.sst");
    return 0;
}
