/*
 * title: Binary records with fixed byte layout
 * topic: io_files
 * covers: fwrite, fread, explicit little-endian encoding, fseek record access
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { REC = 24, N = 40 };

typedef struct {
    uint32_t id;
    int16_t temp;
    uint16_t flags;
    uint64_t stamp;
    char tag[8];
} Rec;

static unsigned long long rs = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 16);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void put_le(unsigned char *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t get_le(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void encode(const Rec *r, unsigned char *b) {
    put_le(b, r->id, 4);
    put_le(b + 4, (uint16_t)r->temp, 2);
    put_le(b + 6, r->flags, 2);
    put_le(b + 8, r->stamp, 8);
    memcpy(b + 16, r->tag, 8);
}

static void decode(const unsigned char *b, Rec *r) {
    r->id = (uint32_t)get_le(b, 4);
    r->temp = (int16_t)(uint16_t)get_le(b + 4, 2);
    r->flags = (uint16_t)get_le(b + 6, 2);
    r->stamp = get_le(b + 8, 8);
    memcpy(r->tag, b + 16, 8);
}

static int same(const Rec *a, const Rec *b) {
    return a->id == b->id && a->temp == b->temp && a->flags == b->flags &&
           a->stamp == b->stamp && memcmp(a->tag, b->tag, 8) == 0;
}

int main(void) {
    Rec src[N];
    for (int i = 0; i < N; i++) {
        memset(&src[i], 0, sizeof src[i]);
        src[i].id = 1000u + (unsigned)i * 7u;
        src[i].temp = (int16_t)((int)(rnd() % 2000) - 1000);
        src[i].flags = (uint16_t)(rnd() & 0xFFFFu);
        uint64_t hi = rnd();
        uint64_t lo = rnd();
        src[i].stamp = (hi << 32) | lo;
        snprintf(src[i].tag, sizeof src[i].tag, "s%03d", i);
    }

    FILE *f = fopen("recs.bin", "wb");
    check(f != NULL, "open w");
    for (int i = 0; i < N; i++) {
        unsigned char b[REC];
        encode(&src[i], b);
        check(fwrite(b, REC, 1, f) == 1, "fwrite");
    }
    check(fclose(f) == 0, "close");

    f = fopen("recs.bin", "rb");
    check(f != NULL, "open r");
    check(fseek(f, 0, SEEK_END) == 0, "seek end");
    long size = ftell(f);
    printf("file size %ld = %d records of %d bytes\n", size, N, REC);
    check(size == (long)N * REC, "size");

    /* visit records in a scrambled order using seeks */
    unsigned long long sum = 0;
    for (int k = 0; k < N; k++) {
        int idx = (k * 17 + 5) % N;
        unsigned char b[REC];
        check(fseek(f, (long)idx * REC, SEEK_SET) == 0, "seek");
        check(fread(b, REC, 1, f) == 1, "fread");
        Rec r;
        decode(b, &r);
        check(same(&r, &src[idx]), "roundtrip");
        sum += r.id + (unsigned)(uint16_t)r.temp + r.flags;
    }
    printf("scrambled sum %llu\n", sum);

    /* raw byte inspection of the first record */
    unsigned char first[REC];
    rewind(f);
    check(fread(first, 1, REC, f) == REC, "first");
    printf("first record bytes:");
    for (int i = 0; i < 16; i++)
        printf(" %02x", first[i]);
    printf("\n");
    printf("id=%u temp=%d flags=0x%04x tag=%s\n", (unsigned)get_le(first, 4),
           (int)(int16_t)get_le(first + 4, 2), (unsigned)get_le(first + 6, 2),
           (const char *)(first + 16));

    /* partial trailing record detection */
    check(fseek(f, size - 5, SEEK_SET) == 0, "seek tail");
    unsigned char b[REC];
    size_t got = fread(b, 1, REC, f);
    printf("tail read of %d bytes returned %zu, eof=%d\n", REC, got, feof(f) != 0);
    check(got == 5 && feof(f), "partial");
    fclose(f);

    /* update one record in place */
    f = fopen("recs.bin", "r+b");
    check(f != NULL, "open r+");
    src[13].flags ^= 0xFFFFu;
    unsigned char e[REC];
    encode(&src[13], e);
    fseek(f, 13L * REC, SEEK_SET);
    check(fwrite(e, REC, 1, f) != 0, "fwrite");
    fseek(f, 13L * REC, SEEK_SET);
    check(fread(b, REC, 1, f) != 0, "fread");
    Rec r13;
    decode(b, &r13);
    check(same(&r13, &src[13]), "update");
    printf("record 13 flags now 0x%04x\n", r13.flags);
    fclose(f);

    check(remove("recs.bin") == 0, "remove");
    return 0;
}
