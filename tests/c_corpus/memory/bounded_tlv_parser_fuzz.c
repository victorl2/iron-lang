/*
 * title: Bounded TLV parser fuzzed with random and mutated input
 * topic: memory
 * covers: length-prefixed parsing, exact-size heap inputs, bounds checks, error taxonomy, PRNG fuzzing
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire format: magic 'T' 'L', u8 count, then count records of [u8 type][u16 be length][value]. */
enum { P_OK, P_SHORT_HDR, P_BAD_MAGIC, P_TRUNC_REC, P_TRUNC_VAL, P_BAD_TYPE, P_TOO_BIG, P_TRAILING, P_NERR };
static const char *pname[P_NERR] = {"ok", "short-header", "bad-magic", "trunc-record", "trunc-value", "bad-type", "too-big", "trailing"};

#define MAX_VAL 64
#define MAX_TYPE 7

typedef struct { uint8_t type; uint16_t len; const uint8_t *val; } Item;

static int parse(const uint8_t *buf, size_t n, Item *items, int max_items, int *count, size_t *used_bytes) {
    size_t pos = 0;
    *count = 0;
    if (n < 3) return P_SHORT_HDR;
    if (buf[0] != 'T' || buf[1] != 'L') return P_BAD_MAGIC;
    int cnt = buf[2];
    pos = 3;
    for (int i = 0; i < cnt; i++) {
        if (n - pos < 3) return P_TRUNC_REC;      /* never compute pos+3 first */
        uint8_t type = buf[pos];
        uint16_t len = (uint16_t)((buf[pos + 1] << 8) | buf[pos + 2]);
        pos += 3;
        if (type == 0 || type > MAX_TYPE) return P_BAD_TYPE;
        if (len > MAX_VAL) return P_TOO_BIG;
        if (n - pos < len) return P_TRUNC_VAL;
        if (*count < max_items) {
            items[*count].type = type; items[*count].len = len; items[*count].val = buf + pos;
            (*count)++;
        }
        pos += len;
    }
    *used_bytes = pos;
    return pos == n ? P_OK : P_TRAILING;
}

static uint64_t st = 0x123456789ABCDEFull;
static uint32_t rnd(void) {
    st += 0x9E3779B97F4A7C15ull;
    uint64_t z = st;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}

static size_t make_valid(uint8_t *out) {
    size_t pos = 0;
    out[pos++] = 'T'; out[pos++] = 'L';
    uint32_t cnt = rnd() % 6;
    out[pos++] = (uint8_t)cnt;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t len = rnd() % (MAX_VAL + 1);
        out[pos++] = (uint8_t)(1 + rnd() % MAX_TYPE);
        out[pos++] = (uint8_t)(len >> 8);
        out[pos++] = (uint8_t)len;
        for (uint32_t k = 0; k < len; k++) out[pos++] = (uint8_t)rnd();
    }
    return pos;
}

int main(void) {
    int hist[P_NERR] = {0};
    long total_items = 0;
    uint8_t scratch[1024];
    for (int iter = 0; iter < 6000; iter++) {
        size_t n;
        int mode = iter % 4;
        if (mode == 0) {
            n = make_valid(scratch);
        } else if (mode == 1) {
            n = make_valid(scratch);
            int flips = 1 + (int)(rnd() % 3);
            for (int f = 0; f < flips; f++) {
                uint8_t bit = (uint8_t)(1u << (rnd() % 8));
                scratch[rnd() % n] ^= bit;
            }
        } else if (mode == 2) {
            n = make_valid(scratch);
            n = rnd() % (n + 1); /* truncate anywhere */
        } else {
            n = rnd() % 40;
            for (size_t i = 0; i < n; i++) scratch[i] = (uint8_t)rnd();
            if (n >= 2 && rnd() % 2) { scratch[0] = 'T'; scratch[1] = 'L'; }
        }
        /* copy into an exactly sized heap block so any overread trips ASan */
        uint8_t *exact = malloc(n ? n : 1);
        if (!exact) return 1;
        memcpy(exact, scratch, n);
        Item items[8];
        int cnt;
        size_t used = 0;
        int rc = parse(exact, n, items, 8, &cnt, &used);
        if (rc == P_OK) {
            if (used != n) { fprintf(stderr, "used mismatch\n"); return 1; }
            for (int i = 0; i < cnt; i++) {
                if (items[i].val < exact || items[i].val + items[i].len > exact + n) { fprintf(stderr, "escape\n"); return 1; }
                total_items++;
            }
        }
        hist[rc]++;
        free(exact);
    }
    for (int i = 0; i < P_NERR; i++) printf("%-13s %d\n", pname[i], hist[i]);
    printf("items accepted: %ld\n", total_items);
    /* every unmodified valid message must parse OK: that is the 1500 of mode 0 */
    if (hist[P_OK] < 1500) { fprintf(stderr, "valid messages rejected\n"); return 1; }
    return 0;
}
