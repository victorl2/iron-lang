/*
 * title: Write-ahead journal recovery under torn writes at every byte
 * topic: memory
 * covers: atomic multi-field update, commit record with checksum, simulated crash at each byte, recovery replay, all-or-nothing
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simulated persistent memory: a data area of 4 words and a journal area. A "crash" stops writes after N bytes. */
#define NW 4
typedef struct {
    uint32_t data[NW];
    unsigned char journal[64];
} Disk;

static long crash_after = -1; /* bytes allowed to reach the disk; -1 = unlimited */
static long written;

static void disk_write(void *dst, const void *src, size_t n) {
    const unsigned char *s = src;
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++) {
        if (crash_after >= 0 && written >= crash_after) return; /* power lost: this and later bytes never land */
        d[i] = s[i];
        written++;
    }
}

static uint32_t checksum(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

/* journal record: [u8 count][count * (u8 idx, u32 val)][u32 checksum over everything before] then the commit magic byte 0xC5 */
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get32(const unsigned char *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }

static void atomic_update(Disk *d, const int *idx, const uint32_t *val, int n) {
    unsigned char rec[64];
    size_t len = 0;
    rec[len++] = (unsigned char)n;
    for (int i = 0; i < n; i++) { rec[len++] = (unsigned char)idx[i]; put32(rec + len, val[i]); len += 4; }
    put32(rec + len, checksum(rec, len)); len += 4;
    rec[len++] = 0xC5;
    /* 1. write the journal record 2. apply to data 3. clear the journal header */
    disk_write(d->journal, rec, len);
    for (int i = 0; i < n; i++) disk_write(&d->data[idx[i]], &val[i], sizeof val[i]);
    unsigned char zero = 0;
    disk_write(d->journal, &zero, 1);
}

/* recovery: if a complete, checksummed, committed record exists, (re)apply it; otherwise ignore the journal */
static int recover(Disk *d) {
    unsigned n = d->journal[0];
    if (n == 0 || n > NW) return 0;
    size_t len = 1 + (size_t)n * 5;
    if (len + 5 > sizeof d->journal) return 0;
    if (d->journal[len + 4] != 0xC5) return 0;
    if (get32(d->journal + len) != checksum(d->journal, len)) return 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = d->journal[1 + i * 5];
        if (idx >= NW) return 0;
        memcpy(&d->data[idx], d->journal + 2 + i * 5, 4);
    }
    d->journal[0] = 0;
    return 1;
}

int main(void) {
    Disk base;
    memset(&base, 0, sizeof base);
    for (int i = 0; i < NW; i++) base.data[i] = 1000u + (uint32_t)i;

    const int idx[3] = {0, 2, 3};
    const uint32_t val[3] = {7777u, 8888u, 9999u};

    /* find the total bytes of a clean run */
    Disk clean = base;
    crash_after = -1; written = 0;
    atomic_update(&clean, idx, val, 3);
    long total = written;
    printf("clean run writes %ld bytes\n", total);

    int old_state = 0, new_state = 0, replayed = 0, mixed = 0;
    for (long cut = 0; cut <= total; cut++) {
        Disk d = base;
        crash_after = cut; written = 0;
        atomic_update(&d, idx, val, 3);
        crash_after = -1;
        int rc = recover(&d);
        replayed += rc;
        int is_old = 1, is_new = 1;
        for (int i = 0; i < NW; i++) {
            if (d.data[i] != base.data[i]) is_old = 0;
            if (d.data[i] != clean.data[i]) is_new = 0;
        }
        if (is_old) old_state++;
        else if (is_new) new_state++;
        else mixed++;
    }
    printf("crash points tested: %ld\n", total + 1);
    printf("recovered to old state: %d, new state: %d (of which replayed from journal: %d), torn/mixed: %d\n", old_state, new_state, replayed, mixed);
    if (mixed) { fprintf(stderr, "found a torn state\n"); return 1; }

    /* the naive protocol (write data in place, no journal) has torn states */
    int naive_mixed = 0;
    for (long cut = 0; cut <= 12; cut++) {
        Disk d = base;
        crash_after = cut; written = 0;
        for (int i = 0; i < 3; i++) disk_write(&d.data[idx[i]], &val[i], 4);
        crash_after = -1;
        int is_old = 1, is_new = 1;
        for (int i = 0; i < NW; i++) { if (d.data[i] != base.data[i]) is_old = 0; if (d.data[i] != clean.data[i]) is_new = 0; }
        if (!is_old && !is_new) naive_mixed++;
    }
    printf("naive in-place update: %d of 13 crash points leave a torn state\n", naive_mixed);
    return 0;
}
