/*
 * title: Per-block checksums with XOR parity scrub and single-block repair
 * topic: memory
 * covers: integrity checksums, RAID-style parity block, silent corruption injection, scrub pass, reconstruction
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB 8
#define BS 64

typedef struct {
    unsigned char *blocks[NB];
    unsigned char *parity;
    uint32_t sum[NB];
} Store;

static uint32_t crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void recompute_parity(Store *s) {
    memset(s->parity, 0, BS);
    for (int b = 0; b < NB; b++) for (int i = 0; i < BS; i++) s->parity[i] ^= s->blocks[b][i];
}

static int store_init(Store *s, uint32_t seed) {
    memset(s, 0, sizeof *s);
    s->parity = malloc(BS);
    if (!s->parity) return 0;
    for (int b = 0; b < NB; b++) {
        s->blocks[b] = malloc(BS);
        if (!s->blocks[b]) return 0;
        uint32_t x = seed + (uint32_t)b * 7919u;
        for (int i = 0; i < BS; i++) { x = x * 1664525u + 1013904223u; s->blocks[b][i] = (unsigned char)(x >> 24); }
        s->sum[b] = crc32(s->blocks[b], BS);
    }
    recompute_parity(s);
    return 1;
}
static void store_free(Store *s) { for (int b = 0; b < NB; b++) free(s->blocks[b]); free(s->parity); }

/* scrub: verify all checksums; if exactly one block is bad, rebuild it from parity; return #bad found, -1 if unrepairable */
static int scrub(Store *s, int *repaired_block) {
    int bad[NB], nbad = 0;
    for (int b = 0; b < NB; b++) if (crc32(s->blocks[b], BS) != s->sum[b]) bad[nbad++] = b;
    *repaired_block = -1;
    if (nbad == 0) return 0;
    if (nbad > 1) return -1;
    int b = bad[0];
    unsigned char rebuilt[BS];
    memcpy(rebuilt, s->parity, BS);
    for (int o = 0; o < NB; o++) if (o != b) for (int i = 0; i < BS; i++) rebuilt[i] ^= s->blocks[o][i];
    if (crc32(rebuilt, BS) != s->sum[b]) return -1;  /* parity itself is bad */
    memcpy(s->blocks[b], rebuilt, BS);
    *repaired_block = b;
    return 1;
}

int main(void) {
    Store s;
    if (!store_init(&s, 99)) return 1;
    unsigned char golden[NB][BS];
    for (int b = 0; b < NB; b++) memcpy(golden[b], s.blocks[b], BS);
    int rb;
    printf("clean scrub: %d\n", scrub(&s, &rb));

    /* every single-bit flip in every block: detected and repaired */
    long flips = 0, repaired_ok = 0;
    for (int b = 0; b < NB; b++)
        for (int bit = 0; bit < BS * 8; bit += 5) {
            s.blocks[b][bit / 8] ^= (unsigned char)(1u << (bit % 8));
            int r = scrub(&s, &rb);
            flips++;
            if (r == 1 && rb == b && memcmp(s.blocks[b], golden[b], BS) == 0) repaired_ok++;
        }
    printf("single-bit flips: %ld injected, %ld repaired exactly\n", flips, repaired_ok);
    if (flips != repaired_ok) return 1;

    /* burst damage in one block (whole block scribbled) */
    memset(s.blocks[3], 0xFF, BS);
    int r = scrub(&s, &rb);
    printf("scribbled block 3: scrub=%d repaired=%d intact=%d\n", r, rb, memcmp(s.blocks[3], golden[3], BS) == 0);

    /* two damaged blocks: detected but unrepairable */
    s.blocks[1][0] ^= 1;
    s.blocks[6][10] ^= 0x80;
    r = scrub(&s, &rb);
    printf("two damaged blocks: scrub=%d repaired=%d\n", r, rb);
    s.blocks[1][0] ^= 1;
    s.blocks[6][10] ^= 0x80;
    printf("after manual undo: scrub=%d\n", scrub(&s, &rb));

    /* corrupt parity plus one block: repair must refuse rather than write garbage */
    s.parity[5] ^= 0x10;
    s.blocks[2][7] ^= 0x01;
    r = scrub(&s, &rb);
    printf("parity+block damage: scrub=%d block2 restored=%d\n", r, memcmp(s.blocks[2], golden[2], BS) == 0);
    s.blocks[2][7] ^= 0x01;
    printf("crc32 of block 0: %08x\n", (unsigned)crc32(golden[0], BS));
    store_free(&s);
    return 0;
}
