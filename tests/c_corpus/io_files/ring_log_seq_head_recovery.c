/*
 * title: Fixed-slot ring log with sequence numbers and head discovery by binary search
 * topic: io_files
 * covers: circular log without a header, per-slot sequence and crc, rotation-point binary search, torn slot handling, crash sweep over wrap points
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

/* Write all bytes at an offset; abort the program on failure. */
static inline void pwrite_all(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, off);
        check(w > 0, "pwrite");
        p += w;
        off += w;
        n -= (size_t)w;
    }
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
enum { SLOTS = 16, SZ = 32, PAY = 23 };
/* slot: seq u32 (0 = never written) | crc u32 | len u8 | payload[23] */

static void make_payload(unsigned char *p, uint32_t seq, unsigned *len) {
    *len = 1 + (seq * 5u) % PAY;
    for (unsigned i = 0; i < *len; i++)
        p[i] = (unsigned char)('a' + (seq * 3u + i) % 26);
}

static void encode(unsigned char *slot, uint32_t seq) {
    unsigned char pay[PAY];
    unsigned len;
    memset(slot, 0, SZ);
    make_payload(pay, seq, &len);
    put32(slot, seq);
    slot[8] = (unsigned char)len;
    memcpy(slot + 9, pay, len);
    put32(slot + 4, crc32_update(crc32_update(0, slot, 4), slot + 8, SZ - 8));
}

static int slot_valid(const unsigned char *slot) {
    if (get32(slot) == 0)
        return 0;
    return get32(slot + 4) == crc32_update(crc32_update(0, slot, 4), slot + 8, SZ - 8);
}

/* Write records 1..count; the final one is torn after `tear` bytes (0 = complete). */
static void write_ring(int fd, uint32_t count, int tear) {
    unsigned char zeros[SZ * SLOTS];
    memset(zeros, 0, sizeof zeros);
    check(ftruncate(fd, 0) == 0, "trunc");
    pwrite_all(fd, zeros, sizeof zeros, 0);
    for (uint32_t seq = 1; seq <= count; seq++) {
        unsigned char slot[SZ];
        encode(slot, seq);
        size_t n = (seq == count && tear > 0) ? (size_t)tear : SZ;
        pwrite_all(fd, slot, n, (off_t)((seq - 1) % SLOTS) * SZ);
    }
}

/* Recovery by linear scan: returns newest valid seq and number of valid slots. */
static uint32_t recover_linear(int fd, int *nvalid, uint32_t *oldest) {
    uint32_t best = 0, old = 0xFFFFFFFFu;
    *nvalid = 0;
    for (int i = 0; i < SLOTS; i++) {
        unsigned char slot[SZ];
        check(pread_upto(fd, slot, SZ, (off_t)i * SZ) == SZ, "slot read");
        if (slot_valid(slot)) {
            uint32_t s = get32(slot);
            (*nvalid)++;
            if (s > best)
                best = s;
            if (s < old)
                old = s;
        }
    }
    *oldest = *nvalid ? old : 0;
    return best;
}

/* Binary search for the rotation point, valid only when every slot holds a valid record.
 * seq at slot i is increasing except at the wrap: find the slot with the smallest seq. */
static uint32_t recover_bsearch(int fd, int *probes) {
    unsigned char s0[SZ];
    check(pread_upto(fd, s0, SZ, 0) == SZ, "read slot 0");
    uint32_t first = get32(s0);
    int lo = 0, hi = SLOTS - 1;
    *probes = 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        unsigned char sm[SZ];
        check(pread_upto(fd, sm, SZ, (off_t)mid * SZ) == SZ, "read mid");
        (*probes)++;
        if (get32(sm) >= first)
            lo = mid + 1; /* still in the first (unwrapped) run */
        else
            hi = mid;
    }
    /* lo is the oldest slot when a wrap exists, or SLOTS-1 when the slots are still in order */
    unsigned char sl[SZ];
    int newest_slot = (lo == 0 || get32(s0) > 0) ? (lo + SLOTS - 1) % SLOTS : 0;
    if (lo == SLOTS - 1) {
        check(pread_upto(fd, sl, SZ, (off_t)lo * SZ) == SZ, "read last");
        if (get32(sl) >= first)
            newest_slot = SLOTS - 1;
    }
    check(pread_upto(fd, sl, SZ, (off_t)newest_slot * SZ) == SZ, "read newest");
    return get32(sl);
}

int main(void) {
    int fd = open("ring.log", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open ring");
    int checked = 0, torn_seen = 0, bs_checked = 0;
    long probe_total = 0;
    for (uint32_t count = 0; count <= 70; count++) {
        for (int tear = 0; tear < 3; tear++) {
            if (count == 0 && tear)
                continue;
            int tear_bytes = tear == 0 ? 0 : (tear == 1 ? 6 : 20);
            write_ring(fd, count, tear_bytes);
            int nvalid;
            uint32_t oldest;
            uint32_t newest = recover_linear(fd, &nvalid, &oldest);
            uint32_t complete = tear_bytes ? count - 1 : count; /* last complete record */
            int torn_survived = 0;
            if (tear_bytes) {
                /* a tear that only rewrote identical bytes leaves a valid record: treat it as complete */
                unsigned char ls[SZ];
                check(pread_upto(fd, ls, SZ, (off_t)((count - 1) % SLOTS) * SZ) == SZ, "read torn slot");
                if (slot_valid(ls) && get32(ls) == count) {
                    complete = count;
                    torn_survived = 1;
                }
            }
            check(newest == complete, "newest complete record found");
            uint32_t expect_valid = complete < SLOTS ? complete : SLOTS;
            /* torn record overwrites a slot whose old record is lost when it was overwriting one */
            if (tear_bytes && !torn_survived && count > SLOTS)
                expect_valid = SLOTS - 1;
            check((uint32_t)nvalid == expect_valid, "valid slot count");
            if (nvalid) {
                /* all surviving records are consecutive and payloads decode correctly */
                for (uint32_t s = oldest; s <= newest; s++) {
                    unsigned char slot[SZ], want[SZ];
                    check(pread_upto(fd, slot, SZ, (off_t)((s - 1) % SLOTS) * SZ) == SZ, "read back");
                    encode(want, s);
                    
                    check(memcmp(slot, want, SZ) == 0, "record intact");
                }
            }
            if (tear_bytes)
                torn_seen++;
            checked++;
            if (!tear_bytes && count >= SLOTS) { /* fully wrapped or exactly full: every slot valid */
                int probes;
                uint32_t nb = recover_bsearch(fd, &probes);
                check(nb == newest, "binary search agrees with linear scan");
                probe_total += probes;
                bs_checked++;
            }
        }
    }
    printf("write points checked=%d torn variants=%d\n", checked, torn_seen);
    printf("binary-search head discovery agreed %d times, average probes=%.2f (slots=%d)\n", bs_checked,
           (double)probe_total / bs_checked, SLOTS);
    /* Show one concrete recovered window. */
    write_ring(fd, 37, 0);
    int nv;
    uint32_t old, newest = recover_linear(fd, &nv, &old);
    printf("after 37 records: oldest=%u newest=%u valid=%d\n", old, newest, nv);
    write_ring(fd, 37, 9);
    newest = recover_linear(fd, &nv, &old);
    printf("after 37 records with torn last: oldest=%u newest=%u valid=%d\n", old, newest, nv);
    close(fd);
    unlink("ring.log");
    return 0;
}
