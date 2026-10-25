/*
 * title: Fixed-slot record store with pread/pwrite and a free list
 * topic: io_files
 * covers: positional record I/O, slot allocation, free list in header, checksum per record, reopen and rescan, model check
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)


static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned rnd_below(unsigned n) {
    unsigned v = (unsigned)(xs() >> 33);
    return v % n;
}

static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define SLOT 48
#define HDR 16
#define NAME_MAX_LEN 24
#define MAX_KEYS 64

/* header: magic u32, free_head i32 (-1 none), slots u32, live u32 */
/* slot:   state u8 (0 free / 1 used), key u32 at 4, val i64 at 8, name[24] at 16, crc u32 at 44; free slots keep next-free at 4 */

static void put32(unsigned char *p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static unsigned get32(const unsigned char *p) { unsigned v = 0; for (int i = 0; i < 4; i++) v |= (unsigned)p[i] << (8 * i); return v; }
static void put64(unsigned char *p, long long v) { unsigned long long u = (unsigned long long)v; for (int i = 0; i < 8; i++) p[i] = (unsigned char)(u >> (8 * i)); }
static long long get64(const unsigned char *p) { unsigned long long u = 0; for (int i = 0; i < 8; i++) u |= (unsigned long long)p[i] << (8 * i); return (long long)u; }

static unsigned crc(const unsigned char *p, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

typedef struct { int fd; int slots; int live; int free_head; } Store;

static void hdr_write(Store *s) {
    unsigned char h[HDR] = {0};
    put32(h, 0x52454353u);
    put32(h + 4, (unsigned)s->free_head);
    put32(h + 8, (unsigned)s->slots);
    put32(h + 12, (unsigned)s->live);
    CHECK(pwrite(s->fd, h, HDR, 0) == HDR);
}

static off_t slot_off(int i) { return HDR + (off_t)i * SLOT; }

static void slot_write_used(Store *s, int i, unsigned key, long long val, const char *name) {
    unsigned char b[SLOT] = {0};
    b[0] = 1;
    put32(b + 4, key);
    put64(b + 8, val);
    strncpy((char *)b + 16, name, NAME_MAX_LEN - 1);
    put32(b + 44, crc(b, 44));
    CHECK(pwrite(s->fd, b, SLOT, slot_off(i)) == SLOT);
}

static int store_insert(Store *s, unsigned key, long long val, const char *name) {
    int i;
    if (s->free_head >= 0) {
        i = s->free_head;
        unsigned char b[SLOT];
        CHECK(pread(s->fd, b, SLOT, slot_off(i)) == SLOT && b[0] == 0);
        s->free_head = (int)get32(b + 4);
    } else {
        i = s->slots++;
    }
    slot_write_used(s, i, key, val, name);
    s->live++;
    hdr_write(s);
    return i;
}

static void store_delete(Store *s, int i) {
    unsigned char b[SLOT] = {0};
    b[0] = 0;
    put32(b + 4, (unsigned)s->free_head);
    CHECK(pwrite(s->fd, b, SLOT, slot_off(i)) == SLOT);
    s->free_head = i;
    s->live--;
    hdr_write(s);
}

static int store_read(Store *s, int i, unsigned *key, long long *val, char *name) {
    unsigned char b[SLOT];
    CHECK(pread(s->fd, b, SLOT, slot_off(i)) == SLOT);
    if (b[0] != 1) return 0;
    CHECK(get32(b + 44) == crc(b, 44));
    *key = get32(b + 4);
    *val = get64(b + 8);
    memcpy(name, b + 16, NAME_MAX_LEN);
    return 1;
}

static void store_open(Store *s, const char *path, int create) {
    s->fd = open(path, O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644);
    CHECK(s->fd >= 0);
    if (create) {
        s->slots = 0; s->live = 0; s->free_head = -1;
        hdr_write(s);
    } else {
        unsigned char h[HDR];
        CHECK(pread(s->fd, h, HDR, 0) == HDR && get32(h) == 0x52454353u);
        s->free_head = (int)get32(h + 4);
        s->slots = (int)get32(h + 8);
        s->live = (int)get32(h + 12);
    }
}

int main(void) {
    Store s;
    store_open(&s, "recs.db", 1);

    int slot_of[MAX_KEYS];
    long long mval[MAX_KEYS];
    int present[MAX_KEYS] = {0};
    int inserts = 0, deletes = 0, updates = 0, reuse = 0;
    for (int step = 0; step < 700; step++) {
        unsigned k = rnd_below(MAX_KEYS);
        unsigned op = rnd_below(10);
        char name[NAME_MAX_LEN];
        snprintf(name, sizeof name, "key-%02u", k);
        if (!present[k]) {
            int before = s.slots;
            long long v = (long long)(xs() >> 30) - (1LL << 33);
            slot_of[k] = store_insert(&s, k, v, name);
            if (s.slots == before) reuse++;
            mval[k] = v;
            present[k] = 1;
            inserts++;
        } else if (op < 3) {
            store_delete(&s, slot_of[k]);
            present[k] = 0;
            deletes++;
        } else {
            long long v = mval[k] * 3 + (long long)step;
            slot_write_used(&s, slot_of[k], k, v, name);
            mval[k] = v;
            updates++;
        }
    }
    int live_model = 0;
    for (int k = 0; k < MAX_KEYS; k++) live_model += present[k];
    printf("inserts=%d deletes=%d updates=%d reused_slots=%d\n", inserts, deletes, updates, reuse);
    printf("live=%d slots=%d file size=%ld\n", s.live, s.slots, fsize(s.fd));
    CHECK(s.live == live_model);
    CHECK(fsize(s.fd) == HDR + (long)s.slots * SLOT);
    CHECK(s.slots <= MAX_KEYS);
    close(s.fd);

    /* reopen, rescan every slot, and compare with the model */
    Store t;
    store_open(&t, "recs.db", 0);
    CHECK(t.live == live_model && t.slots == s.slots && t.free_head == s.free_head);
    int seen[MAX_KEYS] = {0};
    long long sum = 0;
    int free_seen = 0;
    for (int i = 0; i < t.slots; i++) {
        unsigned key;
        long long val;
        char name[NAME_MAX_LEN];
        if (!store_read(&t, i, &key, &val, name)) { free_seen++; continue; }
        CHECK(key < MAX_KEYS && present[key] && !seen[key]);
        CHECK(val == mval[key] && slot_of[key] == i);
        char want[NAME_MAX_LEN];
        snprintf(want, sizeof want, "key-%02u", key);
        CHECK(strcmp(name, want) == 0);
        seen[key] = 1;
        sum += val % 1000003;
    }
    printf("rescan: used=%d free=%d value digest=%lld\n", t.live, free_seen, sum);
    CHECK(free_seen == t.slots - t.live);

    /* walk the free list and make sure it visits exactly the free slots */
    int walked = 0;
    for (int i = t.free_head; i >= 0 && walked <= t.slots; walked++) {
        unsigned char b[SLOT];
        CHECK(pread(t.fd, b, SLOT, slot_off(i)) == SLOT && b[0] == 0);
        i = (int)get32(b + 4);
    }
    printf("free list length: %d\n", walked);
    CHECK(walked == free_seen);

    /* flipping a payload byte is detected by the record checksum */
    int victim = -1;
    for (int k = 0; k < MAX_KEYS; k++) if (present[k]) { victim = slot_of[k]; break; }
    CHECK(victim >= 0);
    unsigned char byte;
    CHECK(pread(t.fd, &byte, 1, slot_off(victim) + 9) == 1);
    byte ^= 0x40;
    CHECK(pwrite(t.fd, &byte, 1, slot_off(victim) + 9) == 1);
    unsigned char b[SLOT];
    CHECK(pread(t.fd, b, SLOT, slot_off(victim)) == SLOT);
    printf("corrupted record checksum ok: %d\n", get32(b + 44) == crc(b, 44));
    CHECK(get32(b + 44) != crc(b, 44));
    close(t.fd);
    unlink("recs.db");
    return 0;
}
