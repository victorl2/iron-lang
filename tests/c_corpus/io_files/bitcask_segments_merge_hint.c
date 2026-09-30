/*
 * title: Bitcask-style segmented log store with merge and hint files
 * topic: io_files
 * covers: log-structured storage, keydir, segment rotation, tombstones, merge of immutable segments, hint files, crash-tail recovery
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rndn(uint32_t n) {
    uint64_t v = rnd();
    return (uint32_t)((v >> 16) % n);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint32_t crc32_update(uint32_t crc, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static inline unsigned get16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* Read up to n bytes at offset; returns the number of bytes read (short at EOF). */
static inline size_t pread_upto(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        check(r >= 0, "pread");
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got;
}

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer (caller frees). */
static inline unsigned char *slurp(const char *path, size_t *len) {
    long n = file_size(path);
    check(n >= 0, "slurp stat");
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    check(b != NULL, "malloc");
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "slurp open");
    size_t got = pread_upto(fd, b, (size_t)n, 0);
    close(fd);
    check(got == (size_t)n, "slurp short");
    *len = (size_t)n;
    return b;
}
enum { KEYSPACE = 300, SEGMAX = 900, MAXSEG = 64, TOMB = 0xFFFF };

/* Record: crc u32 | key u32 | vlen u16 | value bytes. Hint: key u32 | off u32 | vlen u16 | pad u16. */
typedef struct {
    int seg;
    uint32_t off, vlen;
    int live;
} Loc;

typedef struct {
    Loc dir[KEYSPACE];
    int active;
    uint32_t active_len;
    int segs[MAXSEG]; /* segment ids present, ascending */
    int nsegs;
    int next_id;
    long garbage_bytes, live_bytes;
    int fd_active;
} Store;

static void seg_name(char *out, size_t cap, int id, const char *ext) { snprintf(out, cap, "seg%03d.%s", id, ext); }

static size_t rec_len(uint32_t vlen) { return 10u + (vlen == TOMB ? 0u : vlen); }

static void open_active(Store *s) {
    char nm[32];
    s->active = s->next_id++;
    seg_name(nm, sizeof nm, s->active, "log");
    s->fd_active = open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(s->fd_active >= 0, "open active");
    s->active_len = 0;
    s->segs[s->nsegs++] = s->active;
}

static void value_for(uint32_t key, uint32_t ver, unsigned char *v, unsigned len) {
    for (unsigned i = 0; i < len; i++)
        v[i] = (unsigned char)(key * 31u + ver * 17u + i);
}

static void append(Store *s, uint32_t key, const unsigned char *val, uint32_t vlen) {
    unsigned char rec[10 + 64];
    put32(rec + 4, key);
    put16(rec + 8, vlen);
    size_t vb = vlen == TOMB ? 0 : vlen;
    if (vb)
        memcpy(rec + 10, val, vb);
    put32(rec, crc32_update(0, rec + 4, 6 + vb));
    size_t n = 10 + vb;
    if (s->active_len + n > SEGMAX) {
        close(s->fd_active);
        open_active(s);
    }
    write_all(s->fd_active, rec, n);
    Loc *l = &s->dir[key];
    if (l->live) {
        s->garbage_bytes += (long)rec_len(l->vlen);
        s->live_bytes -= (long)rec_len(l->vlen);
    }
    l->seg = s->active;
    l->off = s->active_len;
    l->vlen = vlen;
    l->live = vlen != TOMB;
    if (l->live)
        s->live_bytes += (long)n;
    else
        s->garbage_bytes += (long)n;
    s->active_len += (uint32_t)n;
}

static int get(Store *s, uint32_t key, unsigned char *out, uint32_t *vlen) {
    Loc *l = &s->dir[key];
    if (!l->live)
        return 0;
    char nm[32];
    seg_name(nm, sizeof nm, l->seg, "log");
    int fd = open(nm, O_RDONLY);
    check(fd >= 0, "open seg for get");
    unsigned char rec[10 + 64];
    size_t n = rec_len(l->vlen);
    check(pread_upto(fd, rec, n, l->off) == n, "get read");
    close(fd);
    check(get32(rec) == crc32_update(0, rec + 4, n - 4), "get crc");
    check(get32(rec + 4) == key, "get key");
    memcpy(out, rec + 10, l->vlen);
    *vlen = l->vlen;
    return 1;
}

/* Merge all immutable segments into one, writing a hint file for it. */
static int merge(Store *s) {
    int old[MAXSEG], nold = 0;
    for (int i = 0; i < s->nsegs; i++)
        if (s->segs[i] != s->active)
            old[nold++] = s->segs[i];
    if (nold < 2)
        return 0;
    int mid = s->next_id++;
    char nm[32], hn[32];
    seg_name(nm, sizeof nm, mid, "log");
    seg_name(hn, sizeof hn, mid, "hint");
    int fd = open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int hf = open(hn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint32_t off = 0;
    for (uint32_t k = 0; k < KEYSPACE; k++) {
        Loc *l = &s->dir[k];
        int in_old = 0;
        for (int i = 0; i < nold; i++)
            if (old[i] == l->seg)
                in_old = 1;
        if (!l->live || !in_old)
            continue;
        unsigned char v[64];
        uint32_t vl;
        check(get(s, k, v, &vl), "merge get");
        unsigned char rec[10 + 64], h[12];
        put32(rec + 4, k);
        put16(rec + 8, vl);
        memcpy(rec + 10, v, vl);
        put32(rec, crc32_update(0, rec + 4, 6 + vl));
        write_all(fd, rec, 10 + vl);
        put32(h, k);
        put32(h + 4, off);
        put16(h + 8, vl);
        put16(h + 10, 0);
        write_all(hf, h, 12);
        l->seg = mid;
        l->off = off;
        off += 10 + vl;
    }
    close(fd);
    close(hf);
    for (int i = 0; i < nold; i++) {
        seg_name(nm, sizeof nm, old[i], "log");
        unlink(nm);
        seg_name(nm, sizeof nm, old[i], "hint");
        unlink(nm);
    }
    /* segment list: merged segment first, then active */
    s->segs[0] = mid;
    s->segs[1] = s->active;
    s->nsegs = 2;
    /* garbage accounting restarts: everything in old segments was either live or garbage removed */
    s->garbage_bytes = 0;
    return nold;
}

/* Rebuild the keydir from disk: hint file when present, else scan the log; torn tails are cut off. */
static void recover(Store *s, const int *ids, int n, int *hints_used, int *torn_cut) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < n; i++) {
        int id = ids[i];
        char hn[32], nm[32];
        seg_name(hn, sizeof hn, id, "hint");
        seg_name(nm, sizeof nm, id, "log");
        s->segs[s->nsegs++] = id;
        if (id >= s->next_id)
            s->next_id = id + 1;
        if (file_size(hn) >= 0) {
            size_t hl;
            unsigned char *h = slurp(hn, &hl);
            for (size_t p = 0; p + 12 <= hl; p += 12) {
                Loc *l = &s->dir[get32(h + p)];
                l->seg = id;
                l->off = get32(h + p + 4);
                l->vlen = get16(h + p + 8);
                l->live = 1;
            }
            free(h);
            (*hints_used)++;
            continue;
        }
        size_t len;
        unsigned char *b = slurp(nm, &len);
        size_t p = 0;
        while (p + 10 <= len) {
            uint32_t key = get32(b + p + 4), vl = get16(b + p + 8);
            size_t rl = rec_len(vl);
            if (p + rl > len || key >= KEYSPACE || crc32_update(0, b + p + 4, rl - 4) != get32(b + p))
                break;
            Loc *l = &s->dir[key];
            l->seg = id;
            l->off = (uint32_t)p;
            l->vlen = vl;
            l->live = vl != TOMB;
            p += rl;
        }
        if (p < len) {
            check(truncate(nm, (off_t)p) == 0, "cut torn tail");
            (*torn_cut)++;
        }
        free(b);
        s->active_len = (uint32_t)p;
    }
}

int main(void) {
    Store s;
    memset(&s, 0, sizeof s);
    open_active(&s);
    static uint32_t ver[KEYSPACE];
    static unsigned char len_of[KEYSPACE], present[KEYSPACE];
    int puts = 0, dels = 0, merges = 0, merged_segs = 0;
    for (int step = 0; step < 1500; step++) {
        uint32_t k = rndn(KEYSPACE);
        if (rndn(6) == 0) {
            append(&s, k, NULL, TOMB);
            present[k] = 0;
            dels++;
        } else {
            unsigned char v[64];
            unsigned len = 4 + rndn(40);
            ver[k] = (uint32_t)step;
            value_for(k, ver[k], v, len);
            append(&s, k, v, len);
            len_of[k] = (unsigned char)len;
            present[k] = 1;
            puts++;
        }
        if (step % 400 == 399) {
            int m = merge(&s);
            if (m) {
                merges++;
                merged_segs += m;
            }
        }
    }
    printf("puts=%d deletes=%d merges=%d segments merged=%d\n", puts, dels, merges, merged_segs);
    long fsz = 0;
    for (int i = 0; i < s.nsegs; i++) {
        char nm[32];
        seg_name(nm, sizeof nm, s.segs[i], "log");
        fsz += file_size(nm);
    }
    printf("segments now=%d total log bytes=%ld live bytes=%ld\n", s.nsegs, fsz, s.live_bytes);
    close(s.fd_active);
    /* Simulate a crash mid-append: tear the last active record. */
    char an[32];
    seg_name(an, sizeof an, s.active, "log");
    long asz = file_size(an);
    check(asz > 5, "active nonempty");
    check(truncate(an, (off_t)(asz - 3)) == 0, "tear");
    /* The torn record's key reverts to whatever earlier record survives: recompute by scan below. */
    int ids[MAXSEG], nid = 0;
    for (int i = 0; i < s.nsegs; i++)
        ids[nid++] = s.segs[i];
    Store r;
    int hints = 0, torn = 0;
    recover(&r, ids, nid, &hints, &torn);
    /* Expected model: everything except possibly the key of the torn record. Verify all others. */
    int mism = 0, checked = 0;
    for (uint32_t k = 0; k < KEYSPACE; k++) {
        unsigned char v[64], exp[64];
        uint32_t vl = 0;
        int ok = get(&r, k, v, &vl);
        if (ok != present[k] || (ok && vl != len_of[k])) {
            mism++;
            continue;
        }
        if (ok) {
            value_for(k, ver[k], exp, vl);
            check(memcmp(v, exp, vl) == 0, "value bytes");
        }
        checked++;
    }
    check(mism <= 1, "at most the torn key differs");
    printf("recovery: hint files used=%d torn tails cut=%d keys verified=%d differing=%d\n", hints, torn, checked,
           mism);
    for (int i = 0; i < nid; i++) {
        char nm[32];
        seg_name(nm, sizeof nm, ids[i], "log");
        unlink(nm);
        seg_name(nm, sizeof nm, ids[i], "hint");
        unlink(nm);
    }
    return 0;
}
