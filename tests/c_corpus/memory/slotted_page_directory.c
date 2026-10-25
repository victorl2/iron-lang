/*
 * title: Slotted page with slot directory, tombstones and compaction
 * topic: memory
 * covers: database page layout, slot array growing up, records growing down, deletion, in-place update, defragmentation, model check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 1024u
#define HDR 8u
#define SLOT 4u

/*
 * bytes 0..1  nslots      2..3  free_end (start of the record area, records grow downward)
 * bytes 4..5  live bytes  6..7  reserved
 * then nslots * (u16 offset, u16 length) growing upward; offset 0 marks a deleted slot.
 */
static unsigned char page[PAGE];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned rd16(unsigned off) {
    return (unsigned)page[off] | ((unsigned)page[off + 1] << 8);
}

static void wr16(unsigned off, unsigned v) {
    page[off] = (unsigned char)(v & 0xFF);
    page[off + 1] = (unsigned char)(v >> 8);
}

#define NSLOTS rd16(0)
#define FREE_END rd16(2)
#define LIVE rd16(4)

static void page_init(void) {
    memset(page, 0, sizeof page);
    wr16(0, 0);
    wr16(2, PAGE);
    wr16(4, 0);
}

static unsigned slot_off(unsigned i) {
    return rd16(HDR + i * SLOT);
}

static unsigned slot_len(unsigned i) {
    return rd16(HDR + i * SLOT + 2);
}

static unsigned contiguous_free(void) {
    unsigned dir_end = HDR + NSLOTS * SLOT;
    return FREE_END - dir_end;
}

static unsigned total_free(void) {
    return PAGE - HDR - NSLOTS * SLOT - LIVE;
}

static int find_dead_slot(void) {
    for (unsigned i = 0; i < NSLOTS; i++)
        if (slot_off(i) == 0)
            return (int)i;
    return -1;
}

static void compact(void) {
    unsigned char tmp[PAGE];
    unsigned end = PAGE;
    for (unsigned i = 0; i < NSLOTS; i++) {
        unsigned o = slot_off(i), l = slot_len(i);
        if (!o)
            continue;
        end -= l;
        memcpy(tmp + end, page + o, l);
        wr16(HDR + i * SLOT, end);
    }
    memcpy(page + end, tmp + end, PAGE - end);
    wr16(2, end);
}

/* returns slot id or -1 when the record cannot fit even after compaction */
static int insert(const void *data, unsigned len) {
    int slot = find_dead_slot();
    unsigned need = len + (slot < 0 ? SLOT : 0);
    if (need > total_free())
        return -1;
    if (need > contiguous_free())
        compact();
    check(need <= contiguous_free(), "compaction made room");
    if (slot < 0) {
        slot = (int)NSLOTS;
        wr16(0, NSLOTS + 1);
    }
    unsigned at = FREE_END - len;
    memcpy(page + at, data, len);
    wr16(2, at);
    wr16(HDR + (unsigned)slot * SLOT, at);
    wr16(HDR + (unsigned)slot * SLOT + 2, len);
    wr16(4, LIVE + len);
    return slot;
}

static int erase(unsigned slot) {
    if (slot >= NSLOTS || !slot_off(slot))
        return 0;
    wr16(4, LIVE - slot_len(slot));
    wr16(HDR + slot * SLOT, 0);
    wr16(HDR + slot * SLOT + 2, 0);
    return 1;
}

/* update in place when the new value is no longer than the old one, otherwise delete + insert */
static int update(unsigned slot, const void *data, unsigned len) {
    if (slot >= NSLOTS || !slot_off(slot))
        return 0;
    unsigned o = slot_off(slot), l = slot_len(slot);
    if (len <= l) {
        memcpy(page + o, data, len);
        wr16(HDR + slot * SLOT + 2, len);
        wr16(4, LIVE - l + len);
        return 1;
    }
    unsigned char keep[PAGE];
    memcpy(keep, page + o, l);
    erase(slot);
    /* the slot id must be preserved: the freed slot is the first dead one only if no earlier one exists */
    int prior_dead = find_dead_slot();
    if (prior_dead != (int)slot) {
        /* another dead slot sits earlier; re-insert and then move the directory entry */
        int n = insert(data, len);
        if (n < 0) {
            /* roll back */
            int back = insert(keep, l);
            check(back >= 0, "rollback fits");
            unsigned bo = slot_off((unsigned)back), bl = slot_len((unsigned)back);
            wr16(HDR + slot * SLOT, bo);
            wr16(HDR + slot * SLOT + 2, bl);
            wr16(HDR + (unsigned)back * SLOT, 0);
            wr16(HDR + (unsigned)back * SLOT + 2, 0);
            return -1;
        }
        unsigned no = slot_off((unsigned)n), nl = slot_len((unsigned)n);
        wr16(HDR + slot * SLOT, no);
        wr16(HDR + slot * SLOT + 2, nl);
        wr16(HDR + (unsigned)n * SLOT, 0);
        wr16(HDR + (unsigned)n * SLOT + 2, 0);
        return 1;
    }
    int n = insert(data, len);
    if (n < 0) {
        int back = insert(keep, l);
        check(back == (int)slot, "rollback into same slot");
        return -1;
    }
    check(n == (int)slot, "same slot reused");
    return 1;
}

typedef struct {
    int live;
    unsigned char data[64];
    unsigned len;
} Model;

static uint32_t rs = 8675309;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static void verify(const Model *m, unsigned nmodel) {
    unsigned live_bytes = 0;
    for (unsigned i = 0; i < nmodel; i++) {
        if (i >= NSLOTS) {
            check(!m[i].live, "model slot beyond directory must be dead");
            continue;
        }
        if (m[i].live) {
            check(slot_off(i) != 0 && slot_len(i) == m[i].len, "live slot length");
            check(memcmp(page + slot_off(i), m[i].data, m[i].len) == 0, "live slot bytes");
            check(slot_off(i) >= FREE_END && slot_off(i) + slot_len(i) <= PAGE, "record inside record area");
            live_bytes += m[i].len;
        } else {
            check(slot_off(i) == 0, "dead slot marked");
        }
    }
    check(live_bytes == LIVE, "live byte counter");
    check(HDR + NSLOTS * SLOT <= FREE_END, "directory and records do not overlap");
}

int main(void) {
    page_init();
    Model model[256];
    memset(model, 0, sizeof model);
    unsigned ins = 0, del = 0, upd = 0, rejected = 0;
    for (int step = 0; step < 4000; step++) {
        uint32_t r = rnd();
        unsigned op = r % 10;
        if (op < 5) {
            unsigned len = 4 + (r >> 8) % 56;
            unsigned char data[64];
            for (unsigned i = 0; i < len; i++)
                data[i] = (unsigned char)(r >> (i % 16) ^ i);
            int s = insert(data, len);
            if (s < 0) {
                rejected++;
                continue;
            }
            check(s < 256 && !model[s].live, "fresh slot");
            model[s].live = 1;
            model[s].len = len;
            memcpy(model[s].data, data, len);
            ins++;
        } else if (op < 8) {
            unsigned s = (r >> 8) % 40;
            int ok = erase(s);
            check(ok == (s < 256 && model[s].live), "erase result");
            if (ok) {
                model[s].live = 0;
                del++;
            }
        } else {
            unsigned s = (r >> 8) % 40;
            unsigned len = 4 + (r >> 16) % 56;
            unsigned char data[64];
            for (unsigned i = 0; i < len; i++)
                data[i] = (unsigned char)(r + i * 3);
            if (!model[s].live) {
                check(update(s, data, len) == 0, "update of dead slot");
                continue;
            }
            int u = update(s, data, len);
            if (u == 1) {
                model[s].len = len;
                memcpy(model[s].data, data, len);
                upd++;
            } else {
                check(u == -1, "update rollback");
                rejected++;
            }
        }
        if (step % 50 == 0)
            verify(model, 256);
    }
    verify(model, 256);
    unsigned live_slots = 0;
    for (int i = 0; i < 256; i++)
        live_slots += (unsigned)model[i].live;
    printf("inserts=%u deletes=%u updates=%u rejected=%u\n", ins, del, upd, rejected);
    printf("live records=%u directory slots=%u live bytes=%u\n", live_slots, (unsigned)NSLOTS, (unsigned)LIVE);
    printf("contiguous free=%u total free=%u\n", contiguous_free(), total_free());
    printf("fragmentation bytes=%u\n", total_free() - contiguous_free());
    compact();
    verify(model, 256);
    printf("after compaction: contiguous free=%u fragmentation=%u\n", contiguous_free(), total_free() - contiguous_free());
    check(contiguous_free() == total_free(), "compaction removes fragmentation");

    /* fill the page to the brim with 12-byte records until refusal */
    unsigned filled = 0;
    unsigned char rec[12];
    memset(rec, 0x5A, sizeof rec);
    while (insert(rec, sizeof rec) >= 0)
        filled++;
    printf("extra 12-byte records that fit: %u, free left=%u\n", filled, total_free());
    check(total_free() < 12 + SLOT || (find_dead_slot() >= 0 && total_free() < 12), "page is full");
    return 0;
}
